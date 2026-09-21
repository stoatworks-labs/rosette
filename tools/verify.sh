#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
# Each check answers a question none of the others can:
#
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out. A shader that will not
#                 compile presents to an operator as "the effect does
#                 nothing", with the real message buried in the log.
#   build         a fresh universal Release build, which is what ships
#   suites        the plugin's physical claims, measured: the spot functions
#                 against their GLSL, printed area against the dot-gain
#                 curve, a lattice's angle and pitch off its own dot
#                 centroids, a registration offset off the same, the
#                 overprint arithmetic, the round trip, the press wander and
#                 the audio path
#   presets       is every preset row the right width and kind
#   demo          is the browser demo still running the plugin's own GLSL --
#                 demo/plugin.js carries a second copy of every shader,
#                 because a browser cannot include a C++ file, and two copies
#                 of a shader is the arrangement that drifts invisibly: both
#                 sides keep working and quietly stop being the same effect
#   sweep         does every control change the picture
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop
#                 while still producing a bundle that loads and exports
#                 plugMain
#   lipo          is the macOS build really universal, or did CMake latch
#                 the architecture list before -DCMAKE_OSX_ARCHITECTURES
#                 arrived and report success anyway
#   plist         does CFBundleExecutable name the binary that is actually
#                 on disk -- if it does not, codesign reports "code object
#                 is not signed at all" about a *nested* object and mentions
#                 neither the plist nor the cause
#   codesign      the exact command the release job runs, against a copy
#   oxbow         the name, id and type a HOST sees, which nothing else here
#                 reaches
#   bench         the render cost, for the record. Not pass/fail -- there is
#                 no threshold worth asserting on somebody else's GPU -- but
#                 a verify run leaves a timing on the record, which is what
#                 turns "it feels slower" into a comparison.
#
# The last five are release-job work done locally on purpose. A check that
# only runs in CI, after a tag, is a check that will catch you after the tag.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-universal}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V,
# which demands an explicit layout( location ) on every uniform and varying.
# Those are Vulkan rules and not GLSL ones, and without the flag every shader
# "fails" for reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

# Shaders the plugin assembles at run time. Mirrors PrintShaderSource() and
# SpotProbeShaderSource() in Shaders.cpp -- a name that has moved is a
# KeyError here, not a silent skip.
ASSEMBLED = {
	"PrintShader":     [ "kPrintPreamble", "kSpotLibrary", "kPrintMain" ],
	"SpotProbeShader": [ "#version 410 core\n", "kSpotLibrary", "probeMain" ],
}

# A shader may be several adjacent raw strings (MSVC caps one literal at about
# 16 KB), so everything up to the terminating semicolon is joined.
named = {}
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is
	# a fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

def piece( p ):
	if p.startswith( "#version" ): return p
	return named[ p ]

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )

for name, parts in ASSEMBLED.items():
	emit( name, "".join( piece( p ) for p in parts ) )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders"
if shaders_compile; then
	pass "every shader compiles"
else
	fail "a shader does not compile"
fi

#---------------------------------------------------------------------------
# A fresh universal Release build -- the one that ships. The dev build in
# build/ is arm64 only and is not what any of the binary checks below should
# be looking at.
#---------------------------------------------------------------------------
step "build"
if [ ! -d "$BUILD" ]; then
	cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
fi
if cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "universal Release build"
else
	fail "build failed -- run: cmake --build $BUILD"
	exit 1
fi

RZTEST="$BUILD/rztest"

step "suites"
for t in names defaults presets wander spot gain angle register overprint identity audio; do
	if "$RZTEST" --$t >/dev/null 2>&1; then pass "rztest --$t"; else fail "rztest --$t"; fi
done

step "presets"
if python3 tools/check_presets.py >/dev/null 2>&1; then
	pass "every row the right width and kind"
else
	python3 tools/check_presets.py | sed 's/^/   /'
	fail "tools/check_presets.py"
fi

step "demo"
if python3 demo/tools/check_shaders.py >/dev/null 2>&1; then
	pass "demo/plugin.js runs the plugin's own shaders"
else
	python3 demo/tools/check_shaders.py | sed 's/^/   /'
	fail "demo/tools/check_shaders.py"
fi

step "sweep"
if python3 tools/sweep.py --binary "$RZTEST" --size 480x270 >/tmp/rosette-sweep.txt 2>&1; then
	pass "$( tail -1 /tmp/rosette-sweep.txt )"
else
	echo "   *** dead controls, see /tmp/rosette-sweep.txt"
	tail -4 /tmp/rosette-sweep.txt | sed 's/^/   /'
	fail "tools/sweep.py reports a dead control"
fi

BUNDLE="$BUILD/Rosette.bundle"
BIN="$BUNDLE/Contents/MacOS/Rosette"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under
	# `set -o pipefail`: grep exits at once, nm takes SIGPIPE, and the
	# pipeline reports that failure. Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ "$ident" = "com.stoatworks.ffgl.rosette" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Rosette.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	# The name, id and type as a HOST reads them. A bundle can build, export
	# plugMain and still register itself under the wrong name or the wrong
	# type, and nothing else here would notice.
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		out=$("$OXBOW" probe "$BUNDLE" 2>&1)
		case "$out" in
			*"SW Rosette"*) pass "the host sees the name SW Rosette" ;;
			*) fail "wrong or missing name -- see: $OXBOW probe $BUNDLE" ;;
		esac
		case "$out" in
			*"RZ01"*) pass "the host sees the id RZ01" ;;
			*) fail "wrong or missing id" ;;
		esac
		case "$out" in
			*"type:        effect"*) pass "the host sees an effect" ;;
			*) fail "wrong plugin type" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

step "bench"
"$RZTEST" --bench --frames 60 2>&1 | sed -n '3,8p' | sed 's/^/   /'

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
