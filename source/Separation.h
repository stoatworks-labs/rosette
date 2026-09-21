#pragma once

/**
    The colour arithmetic, in C++.

    Three functions, each mirrored line for line in the GLSL of `Shaders.cpp`
    and marked `//= mirrored` on both sides. The plugin runs the GLSL; the
    harness calls these to say what the GLSL *should* have produced, so
    `rztest --overprint` and `rztest --identity` are comparing the GPU against
    an independent statement of the same arithmetic rather than against
    themselves.

    **Separation.** RGB to CMYK with adjustable black generation. The naive
    separation is c' = 1 - R, m' = 1 - G, y' = 1 - B; the black plate takes
    `k = BG * min( c', m', y' )` of the neutral component, and what is left on
    each colour plate is normalised: `c = ( c' - k ) / ( 1 - k )`. That
    normalised form is not a stylistic choice. Run it forward through the ink
    model below with ideal inks -- cyan absorbing only red, and so on -- and
    the red channel comes out as `( 1 - c )( 1 - k ) = 1 - c' = R`: it is the
    one separation that round-trips through multiplicative overprint. A total
    ink limit then scales the three colour plates down, never the black, so
    that no point on the paper carries more than the press can dry.

    **Dot gain.** A printed dot is bigger than the dot on the plate because ink
    spreads into the paper, and it grows most in the midtone because that is
    where a dot has the most edge for its area. The curve here is the simple
    parabolic one: `a' = a + 4 G a ( 1 - a )`, where G is the gain at the 50%
    tone, so a control that says "0.2" means "a 50% dot prints as 70%", which
    is how a pressman quotes it. It is zero at both ends -- no dot cannot
    grow and a solid cannot grow -- and symmetric, which is a simplification
    of the Murray-Davies shape but a small one.

    **Ink model.** Inks are filters, not lights. Each plate's coverage removes
    a fraction of what the paper reflects, per channel, and the plates
    multiply: `out = paper * PRODUCT_i ( 1 - cov_i * D * ( 1 - ink_i ) )`.
    That is why cyan over yellow is green and why black over anything is
    black -- and why adding inks as though they were lights would have got
    both wrong. The channel-wise absorption is clamped to 1 so that Ink
    Density over unity darkens rather than going negative.
*/
namespace rosette
{

constexpr int kPlateCount = 4;

/// One pixel's worth of ink, in plate order: cyan, magenta, yellow, black.
struct Cmyk
{
	float c, m, y, k;
};

/// RGB (straight, not premultiplied) to CMYK. `blackGeneration` is 0..1,
/// `totalInk` is 2.0..4.0 as in TotalInkFromParam.
Cmyk Separate( float r, float g, float b, float blackGeneration, float totalInk );

/// The dot-gain curve. `tone` and the result are fractional area in 0..1;
/// `gain` is the gain at 50%.
float DotGain( float tone, float gain );

/// The multiplicative overprint. `coverage[ 4 ]` is the printed area of each
/// plate, `ink[ 4 ][ 3 ]` the ink colours, `paper[ 3 ]` the paper. Writes the
/// reflected RGB to `out[ 3 ]`.
void InkModel( const float coverage[ kPlateCount ], const float ink[ kPlateCount ][ 3 ],
               const float paper[ 3 ], float density, float out[ 3 ] );

/// The default inks and paper, as sRGB in 0..1. Process cyan, magenta and
/// yellow at the values the printing trade quotes them, black as a rich
/// near-black rather than 0,0,0 -- a solid of real black ink reflects a few
/// per cent, and that is what makes shadow detail survive on a print.
extern const float kDefaultInk[ kPlateCount ][ 3 ];
extern const float kDefaultPaper[ 3 ];

} // namespace rosette
