# Working on rosette — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 **effect** (`RZ01`, shown as `SW Rosette`) for
Resolume Arena/Avenue that prints the clip: four halftone plates at their own
screen angles, laid down by a press that never quite registers them. C++17 +
GLSL 4.10, CMake, universal macOS `.bundle` and a Windows `.dll`. MIT,
intended home `github.com/stoatworks-labs/rosette`.

`CLAUDE.md` is the command reference. This file is the *why*: read it before
touching the spot functions, the threshold table or the separation.

Built 2026-09-21 in one session, from the fleet's templates (tinsel for the
passes, harness, verify and CI; macroblock for the audio analyser; graticule
for the preset-override shape and the About header, which started as a hand copy
of graticule's and is generated now).

---

## The one idea

**A halftone is not a texture. It is four plates and a press.**

Model the plates — a lattice of dots per ink, each at its own screen angle,
each an area of coverage derived from that ink's separation — and model the
press that lays them one after another and never quite lines them up. Then the
things a printer recognises are *consequences*, not features:

- **Rosettes** are what four lattices at 15°, 75°, 0° and 45° do when you
  overlay them. Nothing draws a rosette.
- **Moiré** is what two lattices at nearly the same angle do. The Moiré preset
  is magenta at 16° against cyan's 15°, and the beat you see is real
  interference between two screens, not a pattern pretending to be one.
- **Colour fringes** are the plates being in different places, and they
  **wander** because a press's registration error is not static.
- **Dot gain** is the midtones fattening because ink spreads into paper.
- **Cyan over yellow is green** because inks are filters and multiply.

The payoff is the same as tinsel's: controls compose instead of accumulating.
There is no "rosette amount" slider, because there could not be one — you get
rosettes by setting four angles the way a printer sets them, and you get moiré
by setting them the way a printer avoids.

### Three things worth understanding before changing anything

**1. A dot's shape and a dot's area are separate questions, and the area is
made exact by ranking.**

A spot function gives every point in a cell a value in 0..1: low where the ink
starts, high where the paper is last to go. A dot for tone `a` is the set of
points whose spot value is below a threshold `T(a)`. That is how PostScript
describes a screen, and it is the right split — but only if `T(a)` is chosen
correctly.

The obvious `T(a) = a` is wrong for every shape but the line screen. A circle
of radius `r` has area proportional to `r²`, so thresholding a radial function
at `a` prints an area that goes as `a²`: the midtones come out far too light,
and the error looks exactly like dot gain with the sign reversed — which is
the sort of bug that gets "corrected" by retuning the gain curve and then lives
for ever.

So `BuildThresholdTable` samples each spot function over the cell (512×512
points), sorts the values, and reads `T(a)` off as the a-th **quantile**. The
fraction of the cell below `T(a)` is then `a` by construction, for every
shape. Real RIPs build their threshold arrays the same way. The consequence
that matters: **Dot Gain is the only thing standing between a tone and its
printed area**, so `--gain` is a sharp test rather than a vague one.

**2. The separation's normalised form is load-bearing.**

`k = BG · min(c', m', y')`, then `c = (c' − k)/(1 − k)`. The normalisation is
not cosmetic. Run it forward through the multiplicative ink model with ideal
inks and the red channel comes back as `(1 − c)(1 − k) = 1 − c' = R`: it is
the separation that round-trips through overprint. The un-normalised `c' − k`
does not, and a picture separated that way comes back systematically light.

**3. Inks are filters, not lights.**

`out = paper · Π(1 − coverage·density·(1 − ink))`, per channel. Adding inks —
the thing that looks natural if you think of the plates as layers — makes cyan
over yellow grey and black over anything a dark grey. `--overprint` pins this
at the solids, bit-exactly.

---

## The traps

Ordered by how much time they would cost the next person.

**The onset detector went deaf for a second and a half after audio started,
and the harness is the only reason anyone knows.** The analyser keeps an
adaptive floor — a running mean of spectral flux — and fires when the flux
clears it by a margin. On the very first frame there is no previous frame, so
`binPrevious` is a buffer of zeroes and *every bin reads as having risen from
silence*: the flux comes out enormous. Worse, `dt` is zero on that frame, so
every one-pole snaps rather than filters and the floor takes that whole number
in one step. The bar then sits several times higher than any real onset and
decays with a one-second time constant.

Measured on the harness's own feed before the fix: a bar of **1.44** against
hits of **0.48**, and **1 onset detected in two seconds instead of 3**. It is
not a harness artefact — it happens in the host every time audio starts, a
clip is triggered or the analyser is reset, which is precisely when an
operator is watching for the first kick. The fix is that the first frame
establishes what "previous" means and reports no flux at all: nothing rose,
because there was nothing for it to rise from. `--audio` is what caught it and
what holds it.

**A displacement is only measurable modulo the lattice, and a test that
forgets this reports a plugin bug that is not there.** `--register` originally
matched each base dot to the *nearest* moved dot. That works while the offset
is under half a cell and aliases the moment it is not: at a 16 px screen an
offset of (−5, +7) — magnitude 8.6 — came back as a worst disagreement of
**16.05 px**, exactly one cell. The lattice is periodic, so a shift by `d` and
a shift by `d` plus a lattice vector produce an identical picture; no
measurement, here or on a real press, can tell them apart. The check now
matches to the dot nearest where the offset *says* it should be and asserts
the residual is ~0, and says plainly that what it establishes is the
displacement modulo the lattice.

**The CMYK round trip at the finest screen is 0.042 off, and it is the
screen's resolution rather than the colour model.** At a 2 px cell there are
four pixels to carry a dot of arbitrary area and the antialiasing width is a
good fraction of the cell, so the frame mean of a screened colour departs from
the continuous ink model. It would be easy to read that as a broken separation
and go looking in the wrong place — or to widen a tolerance until it passed,
which is worse. What settles it is that the error **falls monotonically with
the cell size** (0.0415 → 0.0148 → 0.0099 → 0.0069 at 2, 4, 8 and 16 px) while
colours with **no screened plate** — every plate blank or solid, so no halftone
exists in the picture at all — stay at **0.0016** at every cell size. A wrong
separation or a wrong ink model would be just as wrong at 16 px as at 2, and
would break the unscreened bound, which is twenty times tighter. `--identity`
asserts all three of those things rather than one number.

**A spot function that is discontinuous cannot be antialiased.** The
PostScript Euclidean dot is written as `r²/2` inside the diamond and mirrored
outside it, which has the right level sets but jumps across `|x| + |y| = 1` —
precisely the 50% turnover, where the edge is longest. The form used here,
`dist(centre) / (dist(centre) + dist(nearest corner))`, has the same level
sets, is continuous everywhere, and is exactly 0.5 on that diamond, so the
checkerboard at 50% is still the classic one. Elliptical is the same function
with the metric stretched vertically, which is why one branch serves both.

**The antialiasing width needs the spot function's slope, and `fwidth` is the
wrong tool.** The cell coordinate comes from a `floor`, so it jumps at every
cell boundary, and `fwidth` there reports an enormous derivative and paints an
edge that does not exist — a grid of soft lines over the whole picture.
`spotSlope` returns an analytic bound instead, and is deliberately *not*
mirrored: only the softness of a dot's edge depends on it, never its area.

**`std::max` with an initializer list needs one type.** `std::max({ double,
float, float })` is an ambiguous call, not a conversion, and the error names
neither argument. Two of these in the harness cost a build.

**A `%d` fed a double prints a number with no relation to the measurement, and
the check beside it still says ok.** `--angle` reported "**0 dots**, measured
45.00 deg" and passed, because the message went through a helper taking
doubles. A check whose evidence is wrong is worse than one with no evidence,
because it reads as proof. The helper now carries
`__attribute__((format(printf, 1, 0)))` so the compiler refuses the next one;
count things with `%.0f`.

**Under oxbow this plugin does not settle on a host clock unit, and that is a
known limitation rather than a bug to fix blind.** The detector votes on the
ratio of the host-time delta to the wall-time delta, and its guard requires a
wall delta of **at least 0.5 ms** before a frame's ratio counts. An offline
host renders a frame faster than that, so on 2026-09-21 under oxbow on x64
Windows rosette logged `scale=0.000000` — no unit decided by frame 60 — and
fell back to the wall clock, which is its documented fallback ("wrong in origin
but right in rate"). Nothing rendered wrongly: the same run was 120 frames, gl
error 0x0, PASS. Three siblings in the same run (galvo, cadence, readout)
did settle on `scale=1.000000` (seconds) by frame 60 under the same host,
because their guards are looser — the same problem is solved with four
different guards across six repos, and rosette's is the strict one.

In Arena, rosette **never reached the frame-60 log line at all** during that
run, so **its clock unit inside Arena is unconfirmed**. Two of the siblings
that did log there saw milliseconds. What depends on this clock is the **press
wander**, which is a pure function of time: on the wall-clock fallback it still
wanders at the right rate, but its origin is not the host's, so it will not
line up with the host's timeline. Do not "fix" the guard on the strength of an
offline host — see the open question at the foot of this file.

**An ssh session on Windows has no desktop, so Arena cannot be started from
one.** ssh lands on the service window station; Arena launched from there sits
at about 31 MB doing nothing and cannot be screenshotted. It has to go through
the session-1 scheduled-task wrapper — on win-lab that is `C:\arena-lab\s1.ps1`.

**Arena's REST API will confirm a plugin is registered and then lie about
adding it.** `/api/v1/effects` lists effects by **`idstring`** — the FFGL id,
`RZ01` here, not the display name — which is how registration was confirmed.
But the add-effect endpoint returns **200 without adding anything**, so
instantiation has to be driven from Arena's own effects browser (a double-click
applies to the current selection) and confirmed in the plugin's diag log. After
the run, `/api/v1/composition/…/clips/1` still showed only `Transform`.

**Inherited from the fleet, and all still true here:** `ScopedFBOBinding` does
not restore the viewport (capture the host's and put it back before the print
pass); every `ffglex::Scoped*` clears its binding to 0 on exit rather than
restoring, and `FFGLFBO::Initialise` allocates under one, so every `Ensure()`
happens before anything binds a texture; `FFGLFBO::Release()` leaks the colour
texture, which is why `PassBuffer::Destroy()` deletes it first;
`FFGLScopedFBOBinding.h` is not in the umbrella header; `SetParamInfo` clamps a
STANDARD default into 0..1 before `SetParamRange` can widen it, so every ranged
parameter is 0..1 with the conversions in `Controls.cpp`; the core is an
**OBJECT** library because `CFFGLPluginInfo` registers itself from a file-scope
constructor nothing names; `SetTextParameter` must return `FF_SUCCESS` for the
About block or no host can instantiate the plugin; and the harness drives a
synthetic 60 fps clock, without which no time passes offline and the press
provably never wanders.

---

## Shape of the code

    source/Separation.{h,cpp}  RGB→CMYK, the dot-gain curve, the ink model.
                               Mirrored in GLSL; the harness's truth.
    source/Screen.{h,cpp}      the spot functions and the ranked threshold
                               table. Mirrored in GLSL.
    source/Press.{h,cpp}       bounded smooth noise per plate: the wander.
    source/Audio.{h,cpp}       64 bins → a level and an onset.
    source/Controls.*          0..1 host parameters to physical units.
    source/Shaders.cpp         two passes. kSpotLibrary is shared with the
                               harness's probe.
    source/PassBuffer.*        FFGLFBO with the leak fixed (from tinsel).
    source/Rosette.*           the plugin: parameters, presets, the passes.
    source/Diag.*              a log file, for the shader that will not compile.
    tools/rztest/              the offline harness.
    tools/sweep.py             no control is silently dead.
    tools/verify.sh            all of it.

Two passes:

1. **separate** — picture size, mipmapped. RGB to CMYK with the black
   generation and the ink limit, weighted by alpha. The mip chain is what lets
   the print pass read a whole cell's mean tone in one `textureLod`, which is
   the "area-average sampling" the model needs: sampling the tone at a point
   would alias horribly against the dot lattice.
2. **print** — output size, straight to the host's framebuffer. Four lattices,
   four dots, the ink model, the paper, Mix.

### The GLSL spot functions are a fragment, not a shader

`kSpotLibrary` has no `#version` and no `main`. `PrintShaderSource()` assembles
the print pass around it and `SpotProbeShaderSource()` assembles the harness's
probe around the same string, so `--spot` runs *the text the plugin runs*. A
test that compiled its own transcription would agree with itself perfectly and
prove nothing.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement, on this machine (M4 Max, macOS 26.4, 2026-09-21):**

- **The GPU computes the spot functions the C++ predicts.** 4 shapes × 66,049
  points = **264,196 comparisons, zero disagreements** past 1e-5; largest
  honest difference **1.79e-07**, which is `sqrt` rounding.
- **Printed area follows the dot-gain curve.** Six shape-and-gain cases over
  twenty tone bands of a grey ramp, each band's mean printed area recovered
  from the green channel and compared against the curve integrated over that
  band's tones: worst band off by **0.0099** of full area, against the 2% the
  design asks for. Gain 0 and gain 0.2 both hold.
- **A lattice lies where it was asked to.** The K plate alone on a flat 30%
  grey, its dot centroids found by connected components and weighted by ink:
  45°, 15°, 75° measured to **0.02°**, and an elliptical screen at 20° to
  **0.06°**. Nearest-neighbour spacing **15.95–15.98 px** for a 16 px screen,
  which is also what says Screen is in pixels.
- **Registration is exact.** (+3, −2), (−5, +7) and (+0.5, +0.25) px on the C
  plate move ~960 dots each by exactly that, worst disagreement **0.0491 px**
  (the sub-pixel case; the whole-pixel ones are **0.0000**).
- **Overprint is the ink model.** C over Y, C over M, M over Y and all three
  at solids: **every pixel exact**, 0/255.
- **The round trip.** With no plate screened, **0.0016**; with a screened
  plate, 0.0415 at a 2 px cell falling monotonically to 0.0069 at 16 px. See
  the trap above for why that is the screen and not the model.
- **The press wander is what it claims.** Over an hour of synthetic time:
  never further than its amplitude (**9.545** of 10 px), actually using it,
  moving at most **0.2174 px** between frames, the C and M plates **8.15 px**
  apart on average, exactly zero at zero amplitude, and a pure function of
  time.
- **The audio path.** Silence at full drive moves the plates **0.000 px** and
  finds no onsets; the beat at zero drive is heard (3 onsets) and ignored; at
  full drive it throws the plates **23.6 px**.
- **No dead controls.** All **44** swept parameters measurably change the
  picture (`tools/sweep.py`), with the FFT buffer and the four About buttons
  skipped for stated reasons.
- **The build is universal and registers a plugin** — `lipo` reports
  `x86_64 arm64`, `nm -gU` finds `_plugMain`, a local bundle ad-hoc signs, and
  `oxbow probe` reads back **SW Rosette / RZ01 / effect**, which is the only
  check here that sees what a host sees.
- **Presets survive every host behaviour.** All six rows against three hosts —
  one that echoes the plugin's values, one that restates its own, one that
  quantises to 1/1000 — and an operator edit still drops to Custom and takes
  effect.
- **The render cost**, by `--bench` (60 frames after a 20-frame warm-up,
  `glFinish` on both sides):

  | | ms/frame | % of a 60fps frame |
  | --- | --- | --- |
  | 1280×720 | 0.147 | 0.9% |
  | 1920×1080 | 0.232 | 1.4% |
  | 2560×1440 | 0.342 | 2.1% |
  | 3840×2160 | 0.501 | 3.0% |

  Two passes and no feedback, so it is cheap — about a fifth of tinsel at 4K.

**Verified in a real host, once — 2026-09-21, win-lab** (an x64 Windows 11 Pro
VM with **no GPU**: OpenGL is Mesa llvmpipe dropped in beside Arena; Resolume
Arena 7.27.1, build 15990):

- **The x64 DLL builds and exports the entry point.** It is **cross-compiled in
  the Parallels guest** on this Mac (ARM64 Windows 11, MSVC 2022 Build Tools,
  `cmake -A x64`, vcpkg triplet `x64-windows-static-md`); there is no x64
  Windows machine in the build loop. `Rosette.dll` is **380,928 bytes** and
  `dumpbin /EXPORTS` shows **`plugMain`**.
- **Arena registers it.** `/api/v1/effects` lists `SW Rosette` among 112 video
  effects, under `idstring` **`RZ01`**, with the description the plugin
  declares.
- **Arena loads the DLL.** `plugin loaded build=<stamp>` in the plugin's own
  diag log, carrying the stamp of the DLL built minutes earlier.
- **Arena instantiates it and the shaders compile.** Applied from Arena's own
  effects browser, the log reads `GL vendor=Mesa renderer=llvmpipe (LLVM
  22.1.8, 256 bits) version=4.5 (Core Profile) Mesa 26.2.0` and then
  `initialised`, and Arena drew its inspector, groups and all. The effect was
  applied to the **composition**, not to a clip, so the proof of instantiation
  is the diag log rather than the clip's effect list.
- **It also instantiates and renders headlessly on x64 Windows.** `oxbow
  selftest`: **120 frames, gl error 0x0, PASS**, with **921,600/921,600 lit
  pixels (100%)**.
- **The diag log is clean** of WARN, ERROR and FAIL.

All of that ran on a **software rasteriser**. It says nothing about performance
on Windows: no frame timing was taken there, and the ms/frame table above is
macOS-only.

**Assumed, or not yet done:**

- **Never run on a GPU in Resolume, and never instantiated in Arena on macOS.**
  The one host run was llvmpipe on Windows; everything measured above was
  compiled, rendered and measured offline against the real plugin class in a
  headless CGL context. Arena drew an inspector for the plugin, but nothing in
  it was checked beyond its appearing, so how the parameters *present* — 49 of
  them in six groups, five colour triples that should show as swatches — is
  still untested.
- **The clock unit inside Arena is unconfirmed.** The frame-60 line that would
  name it was never logged during the run; see the trap above for why, and the
  open question below for what it would take to settle.
- **No real audio reached the plugin in Arena**, so the audio paths are still
  only exercised by the harness's synthetic spectra, and Resolume's 64-bin
  mapping is still assumed (below).
- **No long session, no composition save or reload and no preset recall** were
  exercised in the host, and nothing was tested against a Windows Resolume
  licence beyond what the running copy provides.
- CI has run and passed on GitHub — macOS and x64 Windows both — and so has the
  release workflow. Neither of those Windows builds has been put in front of
  Arena; the DLL that ran there was the hand-built one.
- **The 64 spectrum bins are assumed to be linear in frequency and
  low-first**, as the rest of the fleet assumes. Nothing here has seen
  Resolume's own FFT — only the harness's synthetic spectrum — so the audio
  has been exercised on a click train rather than on music.
- **The ink colours are plausible rather than measured.** Process cyan,
  magenta and yellow are set at the values the trade quotes; a real SWOP or
  FOGRA characterisation would differ, and nothing here has been compared
  against a printed sheet.
- **Dot gain is the symmetric parabolic curve**, `a + 4G·a(1−a)`, which is a
  simplification of the Murray–Davies shape: it is zero at both ends and
  symmetric about the midtone, where a real press's gain is skewed towards the
  shadows. Chosen because it makes the control mean what a pressman means by
  "fifteen points".
- **Dot Shape, Ink Spread and the presets are judged by eye.** Nothing
  measures whether Newspaper looks like newsprint.
- **No OpenFX port and no browser demo.** Neither is required for 0.1.0.
- **No user guide**, which is why `StoatworksAbout.h` carries `guide = ""` —
  a link that is not written is left out rather than shown as a button that
  opens a 404. That header is **generated** now: the project is registered in
  `stoatworks-website`'s `projects.json`, in `stoatworks-backend`'s
  `sync-about.py` TARGETS and in `attributions/names.json`, so do not hand-edit
  it. The About facts were chosen so the button count — and therefore the
  parameter count — does not change when it is regenerated.
  `ATTRIBUTIONS.md` is still a **provisional hand copy** in the shape the
  fleet's sync scripts generate, because `sync-attributions.py`'s own master
  lists do not know this repo yet.

## Open questions

- **What should the clock-unit guard be?** rosette requires a wall delta of at
  least 0.5 ms before a frame votes on the host's time unit, which an offline
  host never gives it, so it falls back to the wall clock there; the six
  plugins in that run use four different guards for the one problem.
  Nothing renders wrongly either way, but the press wander's origin follows
  whichever clock wins. The measurement that would settle it is rosette's own
  frame-60 line out of Arena, which the 2026-09-21 run never produced — not a
  change to the guard made on the strength of an offline host.
- **Is the tone sampled at the right scale?** The print pass reads the cell's
  mean from mip level `log2(ScreenPx)`, which is a box of about a cell. A
  proper screen samples the *area* of the cell, and a box aligned to the
  picture is not the cell — it is axis-aligned while the cell is rotated. At
  fine screens nobody could see the difference; at a 40 px cell on detailed
  footage it may soften detail that should survive.
- **Should Total Ink scale or clip?** It scales the three colour plates
  proportionally, which preserves hue and lightens. A press operator would
  more often expect the black to take up the slack.
- **The Riso preset puts the cyan separation on Riso blue and the magenta on
  fluorescent pink**, which is two-colour separation by reuse rather than a
  proper duotone. A real duotone would map luminance onto two inks with its
  own curves per ink, and that is probably the v0.2 feature.
