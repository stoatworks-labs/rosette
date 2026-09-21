#pragma once

/**
    Factory presets: a whole press an operator can reach in one gesture.

    A preset is a *printing process* -- "a newspaper", "a two-colour Riso",
    "a silkscreen poster" -- rather than a set of slider positions. Each row
    sets the separation, the screen, the wander, the inks, the paper and
    which plates are on the press, and leaves the registration offsets, the
    audio, Solo and Mix to the operator: those are performance controls, and
    a preset that reset them every time it was picked would cost more than it
    gave.

    **Presets are an OVERRIDE, not a write.** Resolume does not consume value
    events, so a plugin cannot push a preset's values back into the inspector;
    if it changes its own parameters the sliders keep showing the old numbers.
    So while the dropdown is on anything but Custom, the row's values are laid
    over the operator's at read time (`Rosette::Effective`), and the
    inspector is, for those columns, not the truth. Element 0 of the dropdown
    is Custom and is not in this table: it means "the controls are the
    truth". Moving a covered control while a preset is active drops the
    dropdown back to Custom -- judged by comparing values against what the
    host last sent, not by the change reason, so a host echoing its own
    values cannot un-set the preset. `rztest --presets` drives that.

    **Standard parameters hold the host-facing 0..1**; **option and boolean
    parameters hold their real value**, because `SetParamInfo`'s 0..1 clamp is
    guarded by the parameter type. `tools/check_presets.py` fails a row with
    a fraction in a discrete column.

    ## The first row is also the constructor's defaults

    `Offset Litho` and the defaults in `Rosette.cpp` are the same press,
    written twice, and `rztest --defaults` is the test that fails when they
    go out of step.
*/

namespace rosette
{
namespace presets
{
enum Param
{
	kBlackGen,
	kTotalInk,
	kScreen,
	kDotShape,
	kDotGain,
	kInkSpread,
	kAngleC,
	kAngleM,
	kAngleY,
	kAngleK,
	kWander,
	kWanderSpeed,
	kInkCR, kInkCG, kInkCB,
	kInkMR, kInkMG, kInkMB,
	kInkYR, kInkYG, kInkYB,
	kInkKR, kInkKG, kInkKB,
	kPaperR, kPaperG, kPaperB,
	kInkDensity,
	kPlateC,
	kPlateM,
	kPlateY,
	kPlateK,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Angles are 0..1 for 0..180 degrees: 15 deg = 0.083333, 45 = 0.25,
// 75 = 0.416667, 16 = 0.088889. Screen is geometric 2..40 px: 0.46 is about
// 8 px, 0.55 about 10, 0.6 about 12, 0.65 about 14. Dot Gain is linear
// 0..0.4: 0.375 is 15 points, 0.5 is 20, 0.75 is 30. Ink Density 0.667 is
// unity. Total Ink is linear 200..400%: 0.5 is 300%.
inline constexpr Preset kPresets[] = {
	// The plugin's own defaults, named so they stay reachable: four plates at
	// the standard angles, a fine screen, fifteen points of gain, white paper.
	{ "Offset Litho",
	  { /*BG*/ 1.0f, /*TIL*/ 0.5f, /*Screen*/ 0.46f, /*Shape*/ 0, /*Gain*/ 0.375f, /*Spread*/ 0.1f,
	    /*C*/ 0.083333f, /*M*/ 0.416667f, /*Y*/ 0.0f, /*K*/ 0.25f,
	    /*Wander*/ 0.0f, /*Speed*/ 0.4f,
	    /*InkC*/ 0.00f, 0.68f, 0.94f, /*InkM*/ 0.93f, 0.00f, 0.55f, /*InkY*/ 1.00f, 0.95f, 0.00f, /*InkK*/ 0.14f, 0.12f, 0.13f,
	    /*Paper*/ 1.0f, 1.0f, 1.0f, /*Density*/ 0.667f, /*Plates*/ 1, 1, 1, 1 } },

	// Coarse screen, thirty points of gain, grey stock, a low ink limit and a
	// thin ink: newsprint.
	{ "Newspaper",
	  { /*BG*/ 1.0f, /*TIL*/ 0.2f, /*Screen*/ 0.55f, /*Shape*/ 0, /*Gain*/ 0.75f, /*Spread*/ 0.25f,
	    /*C*/ 0.083333f, /*M*/ 0.416667f, /*Y*/ 0.0f, /*K*/ 0.25f,
	    /*Wander*/ 0.05f, /*Speed*/ 0.4f,
	    /*InkC*/ 0.00f, 0.68f, 0.94f, /*InkM*/ 0.93f, 0.00f, 0.55f, /*InkY*/ 1.00f, 0.95f, 0.00f, /*InkK*/ 0.14f, 0.12f, 0.13f,
	    /*Paper*/ 0.84f, 0.82f, 0.78f, /*Density*/ 0.6f, /*Plates*/ 1, 1, 1, 1 } },

	// Two plates only -- the cyan separation on a Riso blue, the magenta
	// separation on fluorescent pink -- with a coarse screen, a press that
	// drifts, and the paper showing through everywhere. No black plate and
	// no yellow, which is what makes it a Riso and not a litho.
	{ "Riso 2-Colour",
	  { /*BG*/ 0.0f, /*TIL*/ 1.0f, /*Screen*/ 0.6f, /*Shape*/ 0, /*Gain*/ 0.5f, /*Spread*/ 0.2f,
	    /*C*/ 0.25f, /*M*/ 0.083333f, /*Y*/ 0.0f, /*K*/ 0.25f,
	    /*Wander*/ 0.3f, /*Speed*/ 0.3f,
	    /*InkC*/ 0.00f, 0.47f, 0.75f, /*InkM*/ 1.00f, 0.28f, 0.69f, /*InkY*/ 1.00f, 0.95f, 0.00f, /*InkK*/ 0.14f, 0.12f, 0.13f,
	    /*Paper*/ 0.96f, 0.94f, 0.88f, /*Density*/ 0.667f, /*Plates*/ 1, 1, 0, 0 } },

	// A poster: black at 45 degrees and one spot red at 0, square dots, a
	// coarse screen and a hard ink edge.
	{ "Silkscreen",
	  { /*BG*/ 1.0f, /*TIL*/ 1.0f, /*Screen*/ 0.65f, /*Shape*/ 2, /*Gain*/ 0.25f, /*Spread*/ 0.05f,
	    /*C*/ 0.083333f, /*M*/ 0.0f, /*Y*/ 0.0f, /*K*/ 0.25f,
	    /*Wander*/ 0.0f, /*Speed*/ 0.4f,
	    /*InkC*/ 0.00f, 0.68f, 0.94f, /*InkM*/ 0.85f, 0.12f, 0.16f, /*InkY*/ 1.00f, 0.95f, 0.00f, /*InkK*/ 0.14f, 0.12f, 0.13f,
	    /*Paper*/ 0.97f, 0.95f, 0.90f, /*Density*/ 0.667f, /*Plates*/ 0, 1, 0, 1 } },

	// The defaults with the magenta screen turned to 16 degrees, one degree
	// off the cyan: real interference between two lattices, not a texture.
	{ "Moire",
	  { /*BG*/ 1.0f, /*TIL*/ 0.5f, /*Screen*/ 0.46f, /*Shape*/ 0, /*Gain*/ 0.375f, /*Spread*/ 0.1f,
	    /*C*/ 0.083333f, /*M*/ 0.088889f, /*Y*/ 0.0f, /*K*/ 0.25f,
	    /*Wander*/ 0.0f, /*Speed*/ 0.4f,
	    /*InkC*/ 0.00f, 0.68f, 0.94f, /*InkM*/ 0.93f, 0.00f, 0.55f, /*InkY*/ 1.00f, 0.95f, 0.00f, /*InkK*/ 0.14f, 0.12f, 0.13f,
	    /*Paper*/ 1.0f, 1.0f, 1.0f, /*Density*/ 0.667f, /*Plates*/ 1, 1, 1, 1 } },

	// The defaults with the press wandering eight pixels, so the fringes
	// move.
	{ "Drifting Press",
	  { /*BG*/ 1.0f, /*TIL*/ 0.5f, /*Screen*/ 0.46f, /*Shape*/ 0, /*Gain*/ 0.375f, /*Spread*/ 0.1f,
	    /*C*/ 0.083333f, /*M*/ 0.416667f, /*Y*/ 0.0f, /*K*/ 0.25f,
	    /*Wander*/ 0.4f, /*Speed*/ 0.55f,
	    /*InkC*/ 0.00f, 0.68f, 0.94f, /*InkM*/ 0.93f, 0.00f, 0.55f, /*InkY*/ 1.00f, 0.95f, 0.00f, /*InkK*/ 0.14f, 0.12f, 0.13f,
	    /*Paper*/ 1.0f, 1.0f, 1.0f, /*Density*/ 0.667f, /*Plates*/ 1, 1, 1, 1 } },
};

inline constexpr int kCount = static_cast< int >( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace rosette
