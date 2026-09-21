# Rosette

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The printing model is
> not asserted but measured: an offline harness drives the real plugin class in
> a headless GL context and checks each physical claim against an independent
> statement of the same arithmetic — the GLSL spot functions against their C++
> originals over **264,196 points with zero disagreements past 1e-5**, printed
> area against the dot-gain curve to **1%**, a screen angle recovered from the
> dots' own centroids to **0.06°**, a registration offset to **0.05 px**, and
> two solid inks overprinting **bit-exactly** as the ink colours predict (see
> [Status](#status)). It has **never been loaded into Resolume** — only
> compiled, rendered and measured offline. Check it in your own rig before
> trusting it in a show.

Offset litho as an FFGL effect for [Resolume](https://resolume.com) Arena and
Avenue.

![The test card printed: four screens interfering into rosettes, a halftone ramp, and the process primaries](docs/hero.jpg)

<sub>The repo's test card through the plugin at a coarse screen — rendered by
`rztest`, the offline harness, not captured from Resolume.</sub>

## A printed picture is four plates and a press

That is the whole design, and everything else follows from it.

A halftone is not a texture laid over a picture. It is four **plates** — cyan,
magenta, yellow and black — each a lattice of dots at its own **screen angle**,
laid down one after another by a **press** that never quite registers them.
Model those two things and the rest is a consequence rather than an effect:

- **Rosettes.** Put the four lattices at the standard angles — C 15°, M 75°,
  Y 0°, K 45° — and they interfere into the flower printers recognise. Nothing
  draws it. It is what four screens at those angles *do*.
- **Moiré.** Set two screens to the same angle, or one degree apart, and you get
  the coarse beat that costs a print run. The **Moiré** preset puts magenta at
  16° against cyan's 15°, and it is real interference between two lattices, not
  a texture pretending to be one.
- **Misregistration** is per plate, in pixels, and it **wanders** as the press
  runs — a slow, bounded drift that puts colour fringes on every edge and moves
  them. Audio can shake the press.
- **Dot gain.** Ink spreads into paper, so the midtones fatten. Fifteen points
  is a litho press; thirty is newsprint.
- **Overprint.** Inks are filters, not lights. They multiply, which is why cyan
  over yellow is green and why black over anything is black.

**Solo** a plate to see its lattice on its own, which is how the angles and the
screen are actually set.

## The controls

**Separation** — Black Generation (how much of the neutral component moves off
the CMY plates onto the black one: 0 prints greys as equal parts cyan, magenta
and yellow, 1 prints them as black alone) and Total Ink, the limit on how much
ink one point may carry, 200% to 400%.

**Screen** — Screen (2 to 40 px per dot), Dot Shape, Dot Gain, Ink Spread, and
an angle per plate.

Four dot shapes, and they are spot functions rather than sprites. **Round**
grows a near-circle, turns over through a checkerboard at exactly 50%, and
inverts into holes. **Elliptical** is the same function with the metric
stretched, so the dot meets its row neighbours first and chains along the screen
angle — which is what an elliptical screen is for: no abrupt flip at the
midtone. **Square** never inverts. **Line** is a band.

**Press** — a registration offset in x and y for each of the four plates, Press
Wander and Wander Speed, and an audio input with Audio Drive.

**Ink** — the four ink colours and the paper, as colour pickers, plus Ink
Density, a switch per plate and Solo.

**Output** — Mix.

**Six presets:** Offset Litho, Newspaper, Riso 2-Colour, Silkscreen, Moiré,
Drifting Press. Riso is two plates — the cyan separation on a Riso blue, the
magenta on fluorescent pink — with no black, no yellow, a coarse screen, a press
that drifts and the paper showing through, which is most of what makes a Riso
look like one.

## Audio

Resolume hands a plugin a 64-bin spectrum once per frame. Rosette follows it two
ways at once, because a press has two kinds of error: the level jitters the
plates continuously, and a detected onset **throws** them, each in its own
direction, decaying until the next hit. Audio Drive is zero by default, so with
nothing routed this is an ordinary manual halftone and behaves like one.

It is a modulation source at video rate, not a signal source: the smallest
interval it can resolve is a frame, so a kick lands up to 17 ms late at 60 fps.

## Build

Needs CMake and the Resolume FFGL SDK, which is a submodule.

```bash
git clone --recursive https://github.com/stoatworks-labs/rosette
cd rosette
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build    # → ~/Documents/Resolume Arena/Extra Effects
```

macOS builds universal (arm64 + x86_64) by default. Add
`-DCMAKE_OSX_ARCHITECTURES=arm64` for a faster development build.

## Building and testing

The harness renders the real plugin class headlessly, and every check below
measures rather than previews:

    ./build/rztest --out /tmp/frame.png     the test card, printed
    ./build/rztest --list                   every parameter, kind and default
    ./build/rztest --spot                   the GLSL spot functions vs the C++
    ./build/rztest --gain                   printed area vs the dot-gain curve
    ./build/rztest --angle                  a lattice's angle, off its centroids
    ./build/rztest --register               an offset moves the dots by that much
    ./build/rztest --overprint              two solids multiply as predicted
    ./build/rztest --identity               the CMYK round trip
    ./build/rztest --audio                  silence, and a beat
    ./build/rztest --bench                  720p through 4K
    python3 tools/sweep.py                  no control is silently dead
    tools/verify.sh                         all of it, on a fresh universal build

## Status

**v0.1.0, and honestly early.** Verified by measurement on an M4 Max, macOS
26.4, 2026-09-21:

| Check | Result |
| --- | --- |
| GLSL spot functions vs C++ | 4 shapes × 66,049 points — **264,196 comparisons, 0 disagreements** past 1e-5; largest difference 1.79e-07 |
| Printed area vs the dot-gain curve | 6 shape-and-gain cases over 20 tone bands — worst band off by **0.0099** of full area |
| Screen angle | 45°, 15°, 75° and 20° recovered from ~1,550 dot centroids to within **0.06°**; pitch 15.95–15.98 px for a 16 px screen |
| Registration | offsets of (+3, −2), (−5, +7) and (+0.5, +0.25) px move every dot by exactly that, worst disagreement **0.0491 px** |
| Overprint | C over Y, C over M, M over Y and all three — **every pixel exact**, 0/255 |
| CMYK round trip | with no plate screened, **0.0016**; with a screened plate, 0.0415 at a 2 px cell falling to 0.0069 at 16 px |
| Press wander | bounded (9.545 of 10 px), smooth (0.2174 px/frame), different per plate (8.15 px apart), exactly zero at zero |
| Audio | silence moves nothing at full drive; 3 onsets in two seconds; plates thrown 23.6 px |
| No dead controls | all **44** parameters measurably change the picture |
| macOS binary | universal (`x86_64 arm64`), exports `plugMain`, ad-hoc signs |
| Host metadata | `oxbow probe` reads **SW Rosette / RZ01 / effect** |
| Render cost | 0.15 ms/frame at 720p, 0.23 at 1080p, 0.50 at 4K — 3% of a 60 fps frame |

Run `tools/verify.sh` before believing any of it.

**Not yet done.** It has never been loaded into Resolume, so how the parameters
*present* — whether 49 controls in six groups read sensibly in the inspector,
whether the five colour triples show as swatches — is untested. The Windows
build has never been compiled, let alone run: the CI workflows here have never
executed, because the repo has no remote yet. There is no OpenFX port and no
browser demo; neither is required for 0.1.0. `source/StoatworksAbout.h` and
`ATTRIBUTIONS.md` are provisional hand copies, and there is no user guide, so
the About block deliberately carries no guide link. No release tag.

[AGENTS.md](AGENTS.md) has the full list of what is assumed rather than
measured, and the traps.

## Licence

MIT — see [LICENSE](LICENSE) and [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
