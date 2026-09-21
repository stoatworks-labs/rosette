#pragma once

#include <cstdint>
#include <vector>

/**
    The halftone screen: a spot function per dot shape, and the threshold
    table that turns a tone into a dot of exactly that area.

    **A dot shape is a spot function.** Over one cell, with coordinates p in
    [-1, 1]^2, a spot function gives every point a value in 0..1: low at
    wherever the ink starts, high at wherever the paper is last to go. A dot
    for tone `a` is the set of points whose spot value is below a threshold
    T(a). That is how PostScript describes a screen and it is the right
    abstraction, because the *shape* of the dot and the *area* of the dot are
    then separate questions.

    **The area is made exact by ranking.** If T(a) were simply `a`, the printed
    area would equal the tone only for a spot function whose values happen to
    be uniformly distributed over the cell -- true of a line screen, false of
    a round dot, where a circle of radius r has area proportional to r^2. So
    `BuildThresholdTable` samples each spot function over the cell, sorts the
    values, and reads T(a) off as the a-th quantile: the fraction of the cell
    below T(a) is then `a` by construction, for every shape, and Dot Gain is
    the *only* thing standing between a tone and its printed area. PostScript
    RIPs build their threshold arrays by the same ranking. `rztest --gain`
    measures that the GPU's printed area follows the curve, and `rztest --spot`
    checks the GLSL spot functions against these.

    The four shapes:

    - **Round** grows a near-circle from the centre, is a checkerboard of
      diamonds at exactly 50% -- the classic Euclidean dot's turnover -- and
      then inverts into near-circular holes shrinking towards the cell
      corner. The value is the distance to the centre over the sum of that
      and the distance to the nearest corner: 0 at the centre, 1 at the
      corner, exactly 0.5 on the diamond |x| + |y| = 1. The PostScript
      Euclidean spot function has the same level sets but is discontinuous
      across that diamond, which would leave the 50% edge un-antialiased;
      this form is continuous everywhere.
    - **Elliptical** is the same function with the metric stretched
      vertically, so the dot is wider than it is tall. It meets its row
      neighbours at about 40% and chains along the screen angle before it
      closes in the other direction at about 60%, which is what an
      elliptical screen is for: no abrupt 50% flip.
    - **Square** grows a square from the centre and never inverts; at high
      tones the paper is a grid of fine lines.
    - **Line** is a band along the screen angle.
*/
namespace rosette
{

enum class DotShape
{
	Round = 0,
	Elliptical,
	Square,
	Line,
	Count
};

const char* DotShapeName( DotShape shape );

/// The spot function, mirrored in GLSL. `x`, `y` in -1..1; returns 0..1.
float Spot( float x, float y, DotShape shape );

/// Entries per shape in the threshold table.
constexpr int kThresholdSize = 256;

/// Samples per axis when ranking a spot function. 512 x 512 points per cell
/// puts the quantiles within 4e-6 of the continuous answer, which is well
/// under the 8-bit output the result is judged in.
constexpr int kRankSamples = 512;

/// T(a) for every shape: `kThresholdSize` floats per shape, shape-major, in
/// DotShape order. Entry `i` is the threshold for tone `i / ( size - 1 )`.
std::vector< float > BuildThresholdTable();

/// The same PCG-style integer hash the shaders use, for anything that needs
/// a reproducible random number on the CPU. Exact in 32 bits, identical on
/// both sides; never `fract( sin( x ) )`.
uint32_t HashInt( uint32_t x );

/// The hash as a float in 0..1.
float Hash01( uint32_t x );

} // namespace rosette
