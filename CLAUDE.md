# rosette

Offset litho — four halftone plates, their screen angles and the press that
misregisters them — as an FFGL **effect** for Resolume Arena/Avenue. C++/GLSL,
CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the spot functions, the threshold table or the
separation.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Install into Arena: `cmake --install build`
- Render a frame offline: `./build/rztest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Screen=0.6" --set "Solo=4" --set "Dot Gain=0.5"`
- List parameters: `./build/rztest --list`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check, ~10 s warm)
- GLSL spot functions vs the C++: `./build/rztest --spot`
- Printed area vs the dot-gain curve: `./build/rztest --gain`
- A lattice's angle and pitch: `./build/rztest --angle`
- A registration offset: `./build/rztest --register`
- The ink model at solids: `./build/rztest --overprint`
- The CMYK round trip: `./build/rztest --identity`
- The press wander: `./build/rztest --wander`
- The audio path: `./build/rztest --audio`
- Preset 1 IS the constructor's defaults: `./build/rztest --defaults`
- Presets survive every host behaviour: `./build/rztest --presets`
- No name over 16 characters: `./build/rztest --names`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Preset rows the right width and kind: `python3 tools/check_presets.py`
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

## Not done yet
- Never loaded into Resolume. Never compiled on Windows. CI has never run.
- No OpenFX port, no browser demo, no user guide, no release tag, no remote.
- `source/StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume).

    ~/Library/Logs/rosette/rosette.YYYY-MM-DD.log
