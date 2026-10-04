# rosette

Offset litho — four halftone plates, their screen angles and the press that
misregisters them — as an FFGL **effect** for Resolume Arena/Avenue, and the
same effect as an **OpenFX** filter for Resolve/Vegas/Nuke/Natron, rendered on
the CPU. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`,
and `Rosette.ofx.bundle` for macOS, Windows and Linux. MIT.

Read `AGENTS.md` before changing the spot functions, the threshold table or the
separation.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build` (both plugins: `Rosette.bundle`, `Rosette.ofx.bundle`)
- OpenFX alone, no FFGL SDK or GL: add `-DROSETTE_BUILD_FFGL=OFF` (what Linux does)
- Install into Arena: `cmake --install build`
- Run the OpenFX plugin in a host: `../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.rosette --size 640x360 --out /tmp/o.bmp`
  (it also scans `/Library/OFX/Plugins` and takes the FIRST match — `--manifest` shows which bundle)
- Render a frame offline: `./build/rztest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Screen=0.6" --set "Solo=4" --set "Dot Gain=0.5"`
- List parameters: `./build/rztest --list`
- Film footage through the plugin: `ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - | ./build/rztest --pipe --width 1920 --height 1080 [--script cues.txt] | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov` — raw RGBA in, raw RGBA
  out, through the real plugin class. A cue line is `frame Parameter Name value`,
  interpolated between cues; an unknown name is refused rather than ignored.

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check, ~10 s warm)
- With `OFXHOST=<an ofxprobe that has --quirks fusion>` it also renders the
  OpenFX plugin the way Resolve's Fusion page hosts it (no frame rate); without
  one that step says skipped
- GLSL spot functions vs the C++: `./build/rztest --spot`
- Printed area vs the dot-gain curve: `./build/rztest --gain`
- A lattice's angle and pitch: `./build/rztest --angle`
- A registration offset: `./build/rztest --register`
- The ink model at solids: `./build/rztest --overprint`
- The CMYK round trip: `./build/rztest --identity`
- The press wander: `./build/rztest --wander`
- The GPU against the OpenFX build's CPU print: `./build/rztest --cpu` (needs a
  GPU; `RZTEST_RENDERER=software` shows what CI sees, which is a skip)
- The audio path: `./build/rztest --audio`
- Preset 1 IS the constructor's defaults: `./build/rztest --defaults`
- Presets survive every host behaviour: `./build/rztest --presets`
- No name over 16 characters: `./build/rztest --names`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Preset rows the right width and kind: `python3 tools/check_presets.py`
- The browser demo still runs the plugin's GLSL: `python3 demo/tools/check_shaders.py`
- Render cost: `./build/rztest --bench` (0.23 ms/frame at 1080p, 0.50 at 4K)

## Notes
- **A dot's shape and a dot's area are separate questions.** The shape is a
  spot function; the area is made exact by *ranking* that function over the
  cell (`BuildThresholdTable`), so the printed area equals the tone by
  construction for every shape. Never make a dot by thresholding a spot
  function against the tone directly — a round dot's area would then go as the
  square of the tone.
- **Dot Gain is the only thing between a tone and its printed area.** If
  `--gain` fails, either the ranking or the curve is wrong; nothing else in the
  chain is allowed an opinion.
- **Inks are filters, not lights.** `InkModel` multiplies transmittances per
  channel. Adding ink colours would make cyan over yellow grey.
- **The separation's normalised form is load-bearing.** `c = (c' - k)/(1 - k)`
  is the one that round-trips through multiplicative overprint with ideal inks;
  the un-normalised `c' - k` does not.
- The spot functions exist **twice** — `Screen.cpp` and `kSpotLibrary` in
  `Shaders.cpp` — and every mirrored line is marked `//= mirrored` on both
  sides. Change one, change both, run `--spot`.
- **The print pass exists twice too**: `kPrintMain`/`kSeparateShader` in
  `Shaders.cpp` and `Print.cpp`, the OpenFX build's CPU render. `Print.cpp`
  marks every line it copies; the GLSL is not marked, because demo/plugin.js
  must match it character for character. Change one, change both, run `--cpu`.
- `Print.cpp` also mirrors **the GPU's own sampling**, measured on the M4:
  the RGBA16F target rounds toward zero, the mip chain has two paths, sampler
  weights are 1/256 and the trilinear blend is in 64ths of a half-float LOD.
  `Print.h` has the details; AGENTS.md has why each one mattered.
- `Print.cpp` is compiled with `-ffp-contract=off`, except the rotation into a
  plate's frame, which is an explicit `std::fma` because that is how the M4's
  shader compiler builds it. Contraction there decides which cell a pixel on
  a cell boundary lands in.
- `print::Configure` turns controls into uniforms for BOTH builds. The FFGL
  plugin adds its audio offsets on top; the OpenFX one has no audio.
- `rosette_dsp` (no GL) and `rosette_core` (FFGL) are separate OBJECT
  libraries, and an OBJECT library's objects do not travel through another:
  every final target names `rosette_dsp` itself.
- The GLSL spot functions are a **fragment** (no `#version`, no `main`).
  `PrintShaderSource()` and `SpotProbeShaderSource()` assemble the print pass
  and the test probe around the *same* string, so the test runs what the plugin
  runs.
- **Randomness is an integer PCG hash**, never `fract(sin(x)*…)`.
- **The press wander is bounded noise, not a random walk.** A walk has no bound
  and takes a plate off the frame if left running.
- **`ScopedFBOBinding` does not restore the viewport.** Capture the host
  viewport at the top of `ProcessOpenGL` and restore it before the print pass.
- Every `ffglex::Scoped*` binding **clears to 0** on scope exit rather than
  restoring, and `FFGLFBO::Initialise` sizes its texture under one — so
  allocate buffers before binding anything.
- `ffglex::FFGLFBO::Release()` leaks the colour texture; `PassBuffer::Destroy()`
  deletes it first.
- `FFGLScopedFBOBinding.h` is **not** in `FFGLSDK.h`; include it by hand.
- All host parameters are 0..1 and mapped in `Controls.cpp`. `SetParamInfo`
  clamps a standard default into 0..1 before `SetParamRange` can widen it.
- **Parameter names must be unique** — `--set` and the sweep find them by name.
- `patch`, `sample`, `input`, `output`, `filter`, `common`, `active`, `half`
  and `layout` are GLSL reserved words. A shader that will not compile is
  `InitGL FAILED` and a clip that does nothing in the host.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no
  host can instantiate the plugin at all.
- `rosette_core` is an OBJECT library, not STATIC — the plugin registers itself
  from a file-scope constructor nothing references by name.
- **Presets are an OVERRIDE, not a write** — Resolume does not consume value
  events. `Effective()` is the one place that reads them.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `RZ01`; the host-facing name is `SW Rosette`.
- OpenFX identifier is `com.stoatworks.rosette`, label `Rosette`, group
  `Stoatworks`, bundle id `com.stoatworks.rosette.ofx`. Permanent, as are the
  parameter script names (AGENTS.md lists them).
- OFX factories and the threshold table are heap-allocated and never freed:
  an exit-time destructor in a plugin module runs through a dangling pointer
  when a host unloads it early.

## Windows
- The x64 DLL is **cross-compiled in the Parallels guest** on this Mac (ARM64
  Windows 11, MSVC 2022 Build Tools, `cmake -A x64`, vcpkg triplet
  `x64-windows-static-md`) — the same route as the fleet's
  `~/Projects/resolume/winbuild` scripts. There is no x64 Windows machine in
  the build loop.
- 2026-09-21: `Rosette.dll` is **380,928 B** and `dumpbin /EXPORTS` shows
  `plugMain`. It was registered, loaded and instantiated in Resolume Arena
  7.27.1 on win-lab (no GPU — Mesa llvmpipe), and `oxbow selftest` gave
  **120 frames, gl error 0x0, PASS**. `AGENTS.md` has what that does and does
  not prove, and the two Windows traps.

## Not done yet
- Never run on a GPU in Resolume; never instantiated in Arena on macOS. No
  frame timing on Windows — nothing there was timed.
- The plugin's clock unit inside Arena is unconfirmed (`AGENTS.md`).
- No user guide.
- The OpenFX build's one real host is Resolve Studio 21.1 on macOS, as a
  Fusion tool. The first build failed every frame there: Fusion reports no
  frame rate. Fixed with a guarded read and a 24 fps fallback, which renders
  byte-identical to the test host at 24 fps in Resolve (2026-10-04). Vegas,
  Nuke and Natron are untried; the Windows and Linux OpenFX builds have never
  rendered in a host. First released in v0.2.0.
- Any host property that is not guaranteed is read inside its own try with a
  fallback (`framesPerSecond()` and friends in RosetteOFX.cpp). An exception
  out of `render` is a failed frame in Resolve.
- `ATTRIBUTIONS.md` is still a provisional hand copy — `sync-attributions.py`
  does not know this repo. `source/StoatworksAbout.h` is generated by
  `sync-about.py` now; do not hand-edit it.

## Browser demo

`demo/` is the page at **rosette-demo.stoatworks-labs.com**: the plugin's own
two shaders, copied across unedited, plus a hand port of `Controls.cpp`,
`Screen.cpp`'s threshold ranking, `Press.cpp`'s wander and the six preset rows.
The kit in `demo/vendor/` is vendored from
`infrastructure/stoatworks-backend/resolume-demo/` by its `sync.sh` — fix a kit
bug THERE, never here. There is no build step; `cf-run npx wrangler deploy` from
the repo root uploads `demo/` as it stands, and the page is verified by content
rather than by status code. `AGENTS.md` has what the page leaves out and why.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume).

    ~/Library/Logs/rosette/rosette.YYYY-MM-DD.log
