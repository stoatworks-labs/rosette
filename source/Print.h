#pragma once

#include <vector>

/**
    The print pass, on the CPU. What the OpenFX build renders with, and what
    `rztest --cpu` holds against the GPU.

    The FFGL build prints on the GPU in two passes (Shaders.h): separate into
    a mipmapped CMYK buffer, then screen four lattices over it. An OpenFX host
    hands over a CPU buffer instead, so the per-pixel half of that -- and only
    that -- is written a second time here, mirrored line for line from the
    GLSL and marked `//= mirrored` on both sides. Everything that is not
    per-pixel is shared rather than mirrored: the separation, the dot-gain
    curve, the ink model and the spot functions are the functions in
    Separation.cpp and Screen.cpp that the GLSL already mirrors, and the
    conversion from controls to what the shaders are told is `Configure()`,
    which the FFGL plugin calls too.

    **The GPU's sampling is part of the picture, so it is mirrored as well.**
    The tone of a cell is one fetch from a mip chain, and what that fetch
    returns is decided by the driver, not by the shader. Measured on the
    Apple M4's GL (4.1 Metal) rather than assumed, by scratch probes that
    read every mip level and thousands of samples back:

    - The separate pass writes an RGBA16F target, which rounds each value
      to half precision TOWARD ZERO. `SeparateTexel()`.
    - `glGenerateMipmap` has two paths. Level 1, and any level made from one
      with an odd side (1080 rows reach 135, then 67), is a bilinear sample
      of the level above at the new texel's centre, with fixed-point
      weights, rounded once with ties away from zero -- NOT a box that drops
      the odd row. Every other level halves exactly and is a 2x2 box done as
      two steps, each rounded to nearest-even. Levels 1 to 6 -- every level
      a 2..40 px screen reads -- match the GPU's texel for texel at
      1920x1080, 1280x720, 640x360, 1024x1024 and 3840x2160, but for at most
      eight texels a level. The smallest levels, and some odd frame sizes
      (720x486), go a third way in a few texels, by one half-float step.
      `BuildMipChain()`.
    - Bilinear weights are fixed point, rounded to 1/256 of a texel (exact
      against the R32F threshold texture). Between two mip levels the LOD is
      held as a half float and the blend is its fraction truncated to a
      64th, so the step is finer at LOD 0.3 than at LOD 3.3. What comes back
      from the RGBA16F plates is rounded to half once, ties away from zero.
      `SampleTrilinear()`, `SampleThreshold()`.
    - Which multiplies the shader compiler fuses into adds. The rotation into
      a plate's frame is evidently built as `fma( s, y, c * x )`, which
      matters on a cell boundary through pixel centres; `Coverage()` writes
      exactly that, and the rest of this file is compiled with contraction
      off, which is what measured closest everywhere else.

    Other GPUs are free to do all of these differently -- the GL spec leaves
    mipmap generation and filter precision to the implementation -- so the
    FFGL build may itself vary from one graphics card to the next (nobody has
    measured by how much), and Apple's own software renderer differs a great
    deal. This file matches the GPU it was measured on, and `rztest --cpu`
    says how closely.

    **Coordinates are GL's**: row 0 at the bottom, a pixel's centre at
    `x + 0.5`. OpenFX uses the same orientation, so nothing flips.
*/
namespace rosette::print
{

/// The controls the two passes read, as host values: 0..1 for every ranged
/// control, the element index for Dot Shape and Solo, 0 or 1 for the plate
/// switches -- the same values Presets.h holds. The FFGL plugin fills this
/// from `Rosette::Effective()`, the OpenFX plugin from its parameters at the
/// time being rendered.
struct Controls
{
	//No defaults worth the name: both builds fill every field, and a third
	//copy of the defaults here would be one more place for them to drift.
	//Preset row 1 and the FFGL constructor are the two that are held together.
	float blackGeneration = 0.0f;
	float totalInk        = 0.0f;

	float screen     = 0.0f;
	float dotShape   = 0.0f;
	float dotGain    = 0.0f;
	float inkSpread  = 0.0f;
	float angle[ 4 ] = {};

	float registration[ 4 ][ 2 ] = {};
	float wander                 = 0.0f;
	float wanderSpeed            = 0.0f;

	float ink[ 4 ][ 3 ] = {};
	float paper[ 3 ]    = {};
	float inkDensity    = 0.0f;
	float plate[ 4 ]    = {};
	float solo          = 0.0f;

	float mix = 0.0f;
};

/// Everything the separate and print passes are told: the shaders' uniforms,
/// in the units the shaders use.
struct Settings
{
	//Separate.
	float blackGeneration = 1.0f;
	float totalInk        = 3.0f;

	//The picture.
	float size[ 2 ]      = { 1.0f, 1.0f };///< pixels, x already multiplied by the pixel aspect
	float halfTexel[ 2 ] = { 0.5f, 0.5f };///< half a picture texel, in 0..1 picture space
	float pixelAspect    = 1.0f;          ///< 1 everywhere FFGL runs

	//Screen.
	float screenPx  = 8.0f;
	float plateLod  = 3.0f;
	int shape       = 0;
	float dotGain   = 0.15f;
	float inkSpread = 0.05f;
	float angle[ 4 ] = {};
	float cosAngle[ 4 ] = {};
	float sinAngle[ 4 ] = {};

	//The press: where each plate sits, in pixels.
	float offset[ 4 ][ 2 ] = {};

	//Ink.
	float plateOn[ 4 ]  = { 1.0f, 1.0f, 1.0f, 1.0f };
	float ink[ 4 ][ 3 ] = {};
	float paper[ 3 ]    = { 1.0f, 1.0f, 1.0f };
	float inkDensity    = 1.0f;

	float mix = 1.0f;
};

/// Controls to settings, for a picture `width` x `height` at `seconds`.
///
/// `pixelScale` is OpenFX's render scale: a proxy render at half size gets a
/// screen and a press half as many pixels across, so it looks like the full
/// render rather than twice as coarse. `pixelAspect` keeps the dots round on
/// an anamorphic clip. FFGL passes 1 for both and gets exactly the uniforms it
/// always set.
///
/// The press offset here is registration plus wander. The FFGL build adds
/// its audio shake and kick on top; the OpenFX build has no audio.
Settings Configure( const Controls& controls, double seconds, int width, int height,
                    float pixelScale = 1.0f, float pixelAspect = 1.0f );

/// The highest mip level of a `width` x `height` buffer -- the 1x1 one.
float MaxMipLevel( int width, int height );

/// How a float becomes a half. The GPU uses all three, in different places.
enum class HalfRounding
{
	ToNearestEven,
	TiesAway,
	TowardZero
};

/// `x` rounded to an IEEE half, back as a float. Portable bit arithmetic
/// rather than a compiler's half type, so the Windows and Linux builds round
/// exactly as the macOS one does.
float RoundToHalf( float x, HalfRounding mode );

/// The plates buffer: CMYK times alpha, one level per mip, bottom row first.
struct Plates
{
	struct Level
	{
		int width  = 0;
		int height = 0;
		std::vector< float > texels;///< 4 floats per texel: c m y k
	};

	std::vector< Level > levels;

	/// Size level 0 and every level below it for a `width` x `height`
	/// picture. Reuses the allocation when the size has not changed.
	void Allocate( int width, int height );
};

/// One texel of the separate pass. `rgba` is premultiplied, as the host
/// hands it over; `out` is c m y k times alpha, rounded to half precision as
/// the RGBA16F buffer stores it.
void SeparateTexel( const float rgba[ 4 ], const Settings& settings, float out[ 4 ] );

/// Rebuild levels 1.. from level 0, the way this machine's glGenerateMipmap
/// does. See the top of this file.
void BuildMipChain( Plates& plates );

/// `textureLod( PlatesTexture, uv, lod )[ channel ]`, with the sampler's
/// precision. One channel, because the print pass only ever wants plate i's.
float SampleTrilinear( const Plates& plates, float u, float v, float lod, int channel );

/// The threshold for a dot of area `a` in `shape`, read from a table laid
/// out as `BuildThresholdTable()` lays it out, the way the GPU's linear
/// filter reads it.
float SampleThreshold( const float* table, float a, int shape );

/// An upper bound on |grad spot| -- the antialiasing width. The GLSL's
/// `spotSlope`.
float SpotSlope( float x, float y, int shape );

/// The printed coverage of plate `plate` at picture point (`px`, `py`), in
/// pixels. The GLSL's `coverage()`.
float Coverage( const Settings& settings, const Plates& plates, const float* thresholds, int plate,
                float px, float py );

/// The whole print pass at pixel (`x`, `y`) of the picture: `source` is the
/// host's premultiplied pixel there, `out` what the shader writes.
void PrintPixel( const Settings& settings, const Plates& plates, const float* thresholds, int x, int y,
                 const float source[ 4 ], float out[ 4 ] );

} // namespace rosette::print
