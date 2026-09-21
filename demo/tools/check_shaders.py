"""The demo's GLSL must be the plugin's GLSL, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two have drifted.

------------------------------------------------------------------- the point

`demo/plugin.js` carries a second copy of every shader in `source/Shaders.cpp`,
because a browser cannot include a C++ file. Two copies of a shader is exactly
the arrangement that drifts, and the drift is invisible from both sides: the
plugin keeps working, the page keeps working, and they quietly stop being the
same effect. The page's whole claim is that what it runs is the plugin's own
code, so the moment that stops being checkable the page is a lie.

This compares the text, not the behaviour. Reformatting counts as drift, and
that is deliberate -- "it is only whitespace" is how a real change gets waved
through.

The print pass is assembled from three pieces rather than written out, so the
ASSEMBLY is checked too: `PrintShaderSource()` in the C++ and `const PRINT =`
in the JS must still join the same constants in the same order. A shader whose
pieces are all identical and whose order has changed would otherwise pass.

--------------------------------------------------------------- what it cannot

Nothing here checks the *ported* arithmetic. The Controls.cpp conversions, the
`spot`/`buildThresholdTable` port of Screen.cpp and the `pressWander` port of
Press.cpp in plugin.js are a hand translation, and only a reader can tell
whether they still agree. When you change one of those, change it there too --
and remember that a wrong one shows up on the page as a dot that is subtly the
wrong size or a press that wanders subtly wrong, which nobody will notice.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The C++ constant, and the JS constant it must equal.
PAIRS = [
    ("source/Shaders.cpp", "kVertexShader", "VERTEX"),
    ("source/Shaders.cpp", "kSeparateShader", "SEPARATE"),
    ("source/Shaders.cpp", "kSpotLibrary", "SPOT_LIBRARY"),
    ("source/Shaders.cpp", "kPrintPreamble", "PRINT_PREAMBLE"),
    ("source/Shaders.cpp", "kPrintMain", "PRINT_MAIN"),
]

# How the print pass is put together on each side, in order.
ASSEMBLY_CPP = ["kPrintPreamble", "kSpotLibrary", "kPrintMain"]
ASSEMBLY_JS = ["PRINT_PREAMBLE", "SPOT_LIBRARY", "PRINT_MAIN"]


def cpp_literal(path, name):
    """The body of `[static] const char* const name = R"( ... )";`.

    A literal may be several adjacent raw strings -- MSVC caps one at about
    16 KB -- so everything up to the terminating semicolon is joined.
    """
    source = (ROOT / path).read_text()
    match = re.search(
        r'(?:static\s+)?const char\* const\s+' + re.escape(name)
        + r'\s*=\s*((?:\s*R"\((?:.*?)\)")+)\s*;',
        source,
        re.S,
    )
    if match is None:
        return None
    return "".join(re.findall(r'R"\((.*?)\)"', match.group(1), re.S))


def js_literal(source, name):
    """The body of ``const NAME = `...`;``, with the one permitted escape
    decoded.

    The GLSL contains backticks -- `pix`, `a` -- inside its comments, and a
    template literal has nowhere else to put them, so plugin.js writes them as
    \\` and this undoes that. Any OTHER backslash is refused rather than
    decoded: the escape exists to carry a backtick across, not to become a
    second place where the two copies may differ.
    """
    match = re.search(
        r'^const\s+' + re.escape(name) + r'\s*=\s*`(.*?)(?<!\\)`;',
        source,
        re.S | re.M,
    )
    if match is None:
        return None, None

    body = match.group(1)
    stray = re.search(r'\\(?!`)', body)
    if stray is not None:
        return None, f"{name} contains a backslash that is not an escaped backtick"
    return body.replace("\\`", "`"), None


def assembly(text, pattern, names):
    """Are `names` joined, in this order, by the expression `pattern` matches?"""
    match = re.search(pattern, text, re.S)
    if match is None:
        return None
    return [word for word in re.findall(r'\w+', match.group(1)) if word in names]


def main():
    js = (ROOT / "demo/plugin.js").read_text()
    failures = 0

    for path, cpp_name, js_name in PAIRS:
        expected = cpp_literal(path, cpp_name)
        actual, problem = js_literal(js, js_name)

        if expected is None:
            print(f"MISSING  {cpp_name} not found in {path}")
            failures += 1
            continue
        if problem is not None:
            print(f"UNUSABLE {problem}")
            failures += 1
            continue
        if actual is None:
            print(f"MISSING  {js_name} not found in demo/plugin.js")
            failures += 1
            continue

        # A ${ in the GLSL would end the JS template literal's plain text and
        # interpolate something; the mismatch would be reported here rather
        # than at the real cause.
        if "${" in expected:
            print(f"UNUSABLE {cpp_name} contains ${{ -- it cannot be a JS template literal")
            failures += 1
            continue

        if expected == actual:
            print(f"ok       {js_name} matches {cpp_name} ({len(expected)} chars)")
            continue

        failures += 1
        print(f"DRIFTED  {js_name} does not match {cpp_name}")

        expected_lines = expected.splitlines()
        actual_lines = actual.splitlines()
        for i in range(max(len(expected_lines), len(actual_lines))):
            a = expected_lines[i] if i < len(expected_lines) else "<end>"
            b = actual_lines[i] if i < len(actual_lines) else "<end>"
            if a != b:
                print(f"         first difference at line {i + 1}")
                print(f"           {path}: {a!r}")
                print(f"           demo/plugin.js: {b!r}")
                break

    # The assembly, on both sides.
    cpp = (ROOT / "source/Shaders.cpp").read_text()
    cpp_order = assembly(
        cpp, r'std::string PrintShaderSource\(\)\s*\{\s*return\s+(.*?);', ASSEMBLY_CPP)
    js_order = assembly(js, r'const PRINT\s*=\s*(.*?);', ASSEMBLY_JS)

    if cpp_order != ASSEMBLY_CPP:
        print(f"CHANGED  PrintShaderSource() no longer joins {ASSEMBLY_CPP} in that order (found {cpp_order})")
        failures += 1
    elif js_order != ASSEMBLY_JS:
        print(f"DRIFTED  demo/plugin.js assembles PRINT as {js_order}, not {ASSEMBLY_JS}")
        failures += 1
    else:
        print(f"ok       the print pass is assembled the same way on both sides")

    print()
    if failures:
        print(f"{failures} problem(s). Copy the C++ across; do not edit the JS.")
        return 1

    print(f"all {len(PAIRS)} shaders are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
