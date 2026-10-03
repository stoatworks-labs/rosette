#include "Print.h"

#include "Controls.h"
#include "Press.h"
#include "Screen.h"
#include "Separation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace rosette::print
{
namespace
{
int optionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

//GLSL's built-ins, as the GLSL spec defines them.
inline float glslClamp( float x, float lo, float hi )
{
	return std::min( std::max( x, lo ), hi );
}

inline float glslMix( float x, float y, float a )
{
	return x * ( 1.0f - a ) + y * a;
}

inline float glslSmoothstep( float edge0, float edge1, float x )
{
	const float t = glslClamp( ( x - edge0 ) / ( edge1 - edge0 ), 0.0f, 1.0f );
	return t * t * ( 3.0f - 2.0f * t );
}

/// A bilinear weight as the sampler holds it: 1/256 of a texel, rounded.
inline float fixedWeight( float fraction )
{
	return std::round( fraction * 256.0f ) / 256.0f;
}

/// One channel of a GL_LINEAR fetch from one level, clamped to edge.
float sampleLevel( const Plates::Level& level, float u, float v, int channel )
{
	const float sx = u * static_cast< float >( level.width ) - 0.5f;
	const float sy = v * static_cast< float >( level.height ) - 0.5f;
	const float x0 = std::floor( sx );
	const float y0 = std::floor( sy );
	const float fx = fixedWeight( sx - x0 );
	const float fy = fixedWeight( sy - y0 );

	const int ix0 = std::clamp( static_cast< int >( x0 ), 0, level.width - 1 );
	const int ix1 = std::clamp( static_cast< int >( x0 ) + 1, 0, level.width - 1 );
	const int iy0 = std::clamp( static_cast< int >( y0 ), 0, level.height - 1 );
	const int iy1 = std::clamp( static_cast< int >( y0 ) + 1, 0, level.height - 1 );

	const float* t = level.texels.data();
	const auto at  = [ & ]( int x, int y ) {
		return t[ ( static_cast< size_t >( y ) * static_cast< size_t >( level.width ) + static_cast< size_t >( x ) ) * 4
		          + static_cast< size_t >( channel ) ];
	};

	const float bottom = at( ix0, iy0 ) * ( 1.0f - fx ) + at( ix1, iy0 ) * fx;
	const float top    = at( ix0, iy1 ) * ( 1.0f - fx ) + at( ix1, iy1 ) * fx;
	return bottom * ( 1.0f - fy ) + top * fy;
}
} // namespace

//---------------------------------------------------------------------------
// Controls to uniforms. The FFGL plugin's ProcessOpenGL sets its shaders'
// uniforms from what this returns, so there is one answer to what a control
// means and it is not mirrored anywhere.
//---------------------------------------------------------------------------
float MaxMipLevel( int width, int height )
{
	if( width <= 0 || height <= 0 )
		return 0.0f;
	return std::floor( std::log2( static_cast< float >( std::max( width, height ) ) ) );
}

Settings Configure( const Controls& c, double seconds, int width, int height, float pixelScale, float pixelAspect )
{
	Settings s;

	s.blackGeneration = BlackGenerationFromParam( c.blackGeneration );
	s.totalInk        = TotalInkFromParam( c.totalInk );

	s.pixelAspect    = pixelAspect;
	s.size[ 0 ]      = static_cast< float >( width ) * pixelAspect;
	s.size[ 1 ]      = static_cast< float >( height );
	s.halfTexel[ 0 ] = 0.5f / static_cast< float >( width );
	s.halfTexel[ 1 ] = 0.5f / static_cast< float >( height );

	s.screenPx = ScreenPxFromParam( c.screen ) * pixelScale;
	//The mip level whose texel is about one cell: the tone of a cell is the
	//picture's mean over it, and the chain has already computed that.
	s.plateLod  = std::clamp( std::log2( s.screenPx ), 0.0f, MaxMipLevel( width, height ) );
	s.shape     = optionIndex( c.dotShape, static_cast< int >( DotShape::Count ) );
	s.dotGain   = DotGainFromParam( c.dotGain );
	s.inkSpread = InkSpreadFromParam( c.inkSpread );

	for( int i = 0; i < kPlateCount; ++i )
	{
		s.angle[ i ]    = AngleFromParam( c.angle[ i ] );
		s.cosAngle[ i ] = std::cos( s.angle[ i ] );
		s.sinAngle[ i ] = std::sin( s.angle[ i ] );
	}

	//The press: the operator's registration plus the wander.
	const float wander      = WanderFromParam( c.wander ) * pixelScale;
	const float wanderSpeed = WanderSpeedFromParam( c.wanderSpeed );
	for( int plate = 0; plate < kPlateCount; ++plate )
	{
		const press::Offset w   = press::Wander( plate, seconds, wander, wanderSpeed );
		s.offset[ plate ][ 0 ] = RegisterFromParam( c.registration[ plate ][ 0 ] ) * pixelScale + w.x;
		s.offset[ plate ][ 1 ] = RegisterFromParam( c.registration[ plate ][ 1 ] ) * pixelScale + w.y;
	}

	//Solo wins over the plate switches: it is "show me this lattice", and it
	//would be no use if the plate happened to be off.
	const int solo = optionIndex( c.solo, 1 + kPlateCount );
	for( int plate = 0; plate < kPlateCount; ++plate )
		s.plateOn[ plate ] = solo > 0 ? ( solo - 1 == plate ? 1.0f : 0.0f ) : ( c.plate[ plate ] > 0.5f ? 1.0f : 0.0f );

	for( int plate = 0; plate < kPlateCount; ++plate )
		for( int ch = 0; ch < 3; ++ch )
			s.ink[ plate ][ ch ] = c.ink[ plate ][ ch ];
	for( int ch = 0; ch < 3; ++ch )
		s.paper[ ch ] = c.paper[ ch ];
	s.inkDensity = InkDensityFromParam( c.inkDensity );

	s.mix = c.mix;
	return s;
}

//---------------------------------------------------------------------------
// The buffer.
//---------------------------------------------------------------------------
float RoundToHalf( float x, HalfRounding mode )
{
	uint32_t bits = 0;
	std::memcpy( &bits, &x, sizeof bits );
	const uint32_t sign = bits & 0x80000000u;
	uint32_t magnitude  = bits & 0x7fffffffu;

	if( magnitude >= 0x7f800000u )
		return x;//inf and NaN pass through

	//Past the largest half: infinity, or the largest half itself when
	//rounding toward zero. Nothing in a plates buffer comes near it.
	if( magnitude >= 0x477ff000u )
	{
		const uint32_t out = sign | ( mode == HalfRounding::TowardZero ? 0x477fe000u : 0x7f800000u );
		float result       = 0.0f;
		std::memcpy( &result, &out, sizeof result );
		return result;
	}

	//Below 2^-14 a half is subnormal and its step is a fixed 2^-24. Scaled up
	//by 2^24 the value is exact in a float, so rounding it to an integer is
	//rounding to the half's grid.
	if( magnitude < 0x38800000u )
	{
		const float scaled = std::fabs( x ) * 16777216.0f;
		float steps        = 0.0f;
		switch( mode )
		{
		case HalfRounding::ToNearestEven: steps = std::nearbyint( scaled ); break;
		case HalfRounding::TiesAway: steps = std::floor( scaled + 0.5f ); break;
		case HalfRounding::TowardZero: steps = std::floor( scaled ); break;
		}
		return std::copysign( steps / 16777216.0f, x );
	}

	//A normal half keeps 10 of a float's 23 mantissa bits; the other 13 go.
	//A carry out of the mantissa correctly bumps the exponent.
	switch( mode )
	{
	case HalfRounding::ToNearestEven: magnitude += 0x0fffu + ( ( magnitude >> 13 ) & 1u ); break;
	case HalfRounding::TiesAway: magnitude += 0x1000u; break;
	case HalfRounding::TowardZero: break;
	}
	magnitude &= ~0x1fffu;

	const uint32_t rounded = sign | magnitude;
	float out              = 0.0f;
	std::memcpy( &out, &rounded, sizeof out );
	return out;
}

void Plates::Allocate( int width, int height )
{
	if( !levels.empty() && levels[ 0 ].width == width && levels[ 0 ].height == height )
		return;

	levels.clear();
	const int count = static_cast< int >( MaxMipLevel( width, height ) ) + 1;
	for( int l = 0; l < count; ++l )
	{
		Level level;
		level.width  = std::max( 1, width >> l );
		level.height = std::max( 1, height >> l );
		level.texels.assign( static_cast< size_t >( level.width ) * static_cast< size_t >( level.height ) * 4, 0.0f );
		levels.push_back( std::move( level ) );
	}
}

void SeparateTexel( const float rgba[ 4 ], const Settings& settings, float out[ 4 ] )
{
	const float alpha = rgba[ 3 ];

	//Un-premultiply before separating: a soft alpha edge is not a darker
	//colour.
	float r = 0.0f, g = 0.0f, b = 0.0f;
	if( alpha > 0.0031f )//= mirrored
	{
		r = rgba[ 0 ] / alpha;//= mirrored
		g = rgba[ 1 ] / alpha;//= mirrored
		b = rgba[ 2 ] / alpha;//= mirrored
	}

	const Cmyk ink = Separate( r, g, b, settings.blackGeneration, settings.totalInk );

	//Weighted by alpha, then stored as an RGBA16F render target stores it:
	//rounded toward zero.
	out[ 0 ] = RoundToHalf( ink.c * alpha, HalfRounding::TowardZero );//= mirrored
	out[ 1 ] = RoundToHalf( ink.m * alpha, HalfRounding::TowardZero );//= mirrored
	out[ 2 ] = RoundToHalf( ink.y * alpha, HalfRounding::TowardZero );//= mirrored
	out[ 3 ] = RoundToHalf( ink.k * alpha, HalfRounding::TowardZero );//= mirrored
}

void BuildMipChain( Plates& plates )
{
	for( size_t l = 1; l < plates.levels.size(); ++l )
	{
		const Plates::Level& above = plates.levels[ l - 1 ];
		Plates::Level& level       = plates.levels[ l ];

		const auto texel = [ &above ]( int x, int y ) {
			x = std::clamp( x, 0, above.width - 1 );
			y = std::clamp( y, 0, above.height - 1 );
			return above.texels.data() + ( static_cast< size_t >( y ) * static_cast< size_t >( above.width ) + static_cast< size_t >( x ) ) * 4;
		};

		//Two paths, as measured. Level 1, and any level made from one with an
		//odd side, is a bilinear sample of the level above at the new texel's
		//centre -- fixed-point weights, float arithmetic, one rounding with
		//ties away from zero. Every other level halves exactly and is a 2x2
		//box done as two rounded steps: each row's pair averaged and rounded
		//to half, then the two rows.
		const bool halves = above.width == 2 * level.width && above.height == 2 * level.height;

		for( int y = 0; y < level.height; ++y )
		{
			for( int x = 0; x < level.width; ++x )
			{
				float* out = level.texels.data() + ( static_cast< size_t >( y ) * static_cast< size_t >( level.width ) + static_cast< size_t >( x ) ) * 4;

				if( l >= 2 && halves )
				{
					const float* a = texel( 2 * x, 2 * y );
					const float* b = texel( 2 * x + 1, 2 * y );
					const float* c = texel( 2 * x, 2 * y + 1 );
					const float* d = texel( 2 * x + 1, 2 * y + 1 );
					for( int ch = 0; ch < 4; ++ch )
					{
						const float bottom = RoundToHalf( a[ ch ] + ( b[ ch ] - a[ ch ] ) * 0.5f, HalfRounding::ToNearestEven );
						const float top    = RoundToHalf( c[ ch ] + ( d[ ch ] - c[ ch ] ) * 0.5f, HalfRounding::ToNearestEven );
						out[ ch ]          = RoundToHalf( bottom + ( top - bottom ) * 0.5f, HalfRounding::ToNearestEven );
					}
					continue;
				}

				const float u  = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( level.width );
				const float v  = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( level.height );
				const float sx = u * static_cast< float >( above.width ) - 0.5f;
				const float sy = v * static_cast< float >( above.height ) - 0.5f;
				const float x0 = std::floor( sx );
				const float y0 = std::floor( sy );
				const float fx = fixedWeight( sx - x0 );
				const float fy = fixedWeight( sy - y0 );
				const int ix   = static_cast< int >( x0 );
				const int iy   = static_cast< int >( y0 );

				const float* t00 = texel( ix, iy );
				const float* t10 = texel( ix + 1, iy );
				const float* t01 = texel( ix, iy + 1 );
				const float* t11 = texel( ix + 1, iy + 1 );
				for( int ch = 0; ch < 4; ++ch )
				{
					const float bottom = t00[ ch ] * ( 1.0f - fx ) + t10[ ch ] * fx;
					const float top    = t01[ ch ] * ( 1.0f - fx ) + t11[ ch ] * fx;
					out[ ch ]          = RoundToHalf( bottom * ( 1.0f - fy ) + top * fy, HalfRounding::TiesAway );
				}
			}
		}
	}
}

float SampleTrilinear( const Plates& plates, float u, float v, float lod, int channel )
{
	const int maxLevel = static_cast< int >( plates.levels.size() ) - 1;
	if( maxLevel < 0 )
		return 0.0f;

	//The LOD is held as a half float, rounded to nearest; the blend between
	//the two levels is its fraction truncated to six bits, a 64th. So the
	//step is finer at LOD 0.3 than at LOD 3.3 -- measured, not chosen.
	const float clamped = std::clamp( lod, 0.0f, static_cast< float >( maxLevel ) );
	const float held    = RoundToHalf( clamped, HalfRounding::ToNearestEven );
	int level           = static_cast< int >( std::floor( held ) );
	float blend         = std::floor( ( held - static_cast< float >( level ) ) * 64.0f ) / 64.0f;
	if( level >= maxLevel )
	{
		level = maxLevel;
		blend = 0.0f;
	}

	//Filtered in float across both levels, and what comes back is rounded to
	//half once, ties away from zero.
	const float near = sampleLevel( plates.levels[ static_cast< size_t >( level ) ], u, v, channel );
	if( blend == 0.0f )
		return RoundToHalf( near, HalfRounding::TiesAway );

	const float far = sampleLevel( plates.levels[ static_cast< size_t >( level + 1 ) ], u, v, channel );
	return RoundToHalf( near + ( far - near ) * blend, HalfRounding::TiesAway );
}

float SampleThreshold( const float* table, float a, int shape )
{
	//The GLSL's texture coordinate, then the sampler's texel position.
	const float u  = ( a * 255.0f + 0.5f ) / 256.0f;//= mirrored
	const float sx = u * static_cast< float >( kThresholdSize ) - 0.5f;
	const float x0 = std::floor( sx );
	const float fx = fixedWeight( sx - x0 );

	const int i0     = std::clamp( static_cast< int >( x0 ), 0, kThresholdSize - 1 );
	const int i1     = std::clamp( static_cast< int >( x0 ) + 1, 0, kThresholdSize - 1 );
	const float* row = table + static_cast< size_t >( shape ) * kThresholdSize;
	return row[ i0 ] * ( 1.0f - fx ) + row[ i1 ] * fx;
}

//---------------------------------------------------------------------------
// The print pass. Mirrored from kSpotLibrary's spotSlope and kPrintMain in
// Shaders.cpp; every mirrored line is marked on both sides.
//---------------------------------------------------------------------------
float SpotSlope( float x, float y, int shape )
{
	if( shape == 0 || shape == 1 )//= mirrored
	{
		const float ky = shape == 0 ? 1.0f : kEllipse;//= mirrored
		const float ax = std::fabs( x );//= mirrored
		const float ay = std::fabs( y );//= mirrored
		const float dc = std::sqrt( x * x + ky * ky * y * y );//= mirrored
		const float u  = 1.0f - ax;//= mirrored
		const float v  = 1.0f - ay;//= mirrored
		const float dk = std::sqrt( u * u + ky * ky * v * v );//= mirrored
		return ky / std::max( dc + dk, 1e-3f );//= mirrored
	}
	return 1.0f;//= mirrored
}

float Coverage( const Settings& s, const Plates& plates, const float* thresholds, int i, float px, float py )
{
	const float c  = s.cosAngle[ i ];
	const float sn = s.sinAngle[ i ];

	//Into the plate's own frame.
	//
	//The shader writes c * rel.x + s * rel.y; the M4's compiler builds it as
	//one rounded product fused into the other, fma( s, rel.y, c * rel.x ).
	//That is measured, not guessed: written plainly here, a 45-degree plate in
	//register has its cell boundary exactly on the pixel centres along x = y,
	//the two builds put those pixels in different cells, and they came out up
	//to 211/255 apart; fused this way, 18. std::fma is exact everywhere, so it
	//is also the same answer on arm64 and on x86_64. The rest of this file is
	//built with contraction off (CMakeLists.txt), which is what measured
	//closest for everything else.
	const float relX = px - s.offset[ i ][ 0 ];//= mirrored
	const float relY = py - s.offset[ i ][ 1 ];//= mirrored
	const float qx   = std::fma( sn, relY, c * relX ) / s.screenPx;//= mirrored
	const float qy   = std::fma( c, relY, -sn * relX ) / s.screenPx;//= mirrored

	const float cellX = std::floor( qx );//= mirrored
	const float cellY = std::floor( qy );//= mirrored
	const float pX    = 2.0f * ( qx - cellX ) - 1.0f;//= mirrored
	const float pY    = 2.0f * ( qy - cellY ) - 1.0f;//= mirrored

	//The tone: the picture's mean over the cell, one fetch at its centre.
	const float centreQX   = ( cellX + 0.5f ) * s.screenPx;//= mirrored
	const float centreQY   = ( cellY + 0.5f ) * s.screenPx;//= mirrored
	const float centrePixX = ( c * centreQX - sn * centreQY ) + s.offset[ i ][ 0 ];//= mirrored
	const float centrePixY = ( sn * centreQX + c * centreQY ) + s.offset[ i ][ 1 ];//= mirrored
	const float cu         = glslClamp( centrePixX / s.size[ 0 ], s.halfTexel[ 0 ], 1.0f - s.halfTexel[ 0 ] );//= mirrored
	const float cv         = glslClamp( centrePixY / s.size[ 1 ], s.halfTexel[ 1 ], 1.0f - s.halfTexel[ 1 ] );//= mirrored
	const float tone       = SampleTrilinear( plates, cu, cv, s.plateLod, i );//= mirrored

	const float a = DotGain( tone, s.dotGain );//the GLSL's two `a` lines, mirrored in Separation.cpp

	const float T = SampleThreshold( thresholds, a, s.shape );//= mirrored

	//Analytic antialiasing: three quarters of a pixel, plus Ink Spread.
	const float slope = SpotSlope( pX, pY, s.shape );//= mirrored
	const float w     = slope * ( 0.75f / s.screenPx + s.inkSpread );//= mirrored
	const float sv    = Spot( pX, pY, static_cast< DotShape >( s.shape ) );//= mirrored

	float ink = 1.0f - glslSmoothstep( T - w, T + w, sv );//= mirrored

	//A tone of exactly 0 or 1 is no dot or a solid, whatever the softness.
	ink = glslMix( ink, 1.0f, glslSmoothstep( 0.98f, 1.0f, a ) );//= mirrored
	ink = glslMix( ink, 0.0f, 1.0f - glslSmoothstep( 0.0f, 0.02f, a ) );//= mirrored

	return ink * s.plateOn[ i ];//= mirrored
}

void PrintPixel( const Settings& s, const Plates& plates, const float* thresholds, int x, int y,
                 const float source[ 4 ], float out[ 4 ] )
{
	const float px = ( static_cast< float >( x ) + 0.5f ) * s.pixelAspect;//= mirrored (uv * Size)
	const float py = static_cast< float >( y ) + 0.5f;//= mirrored (uv * Size)

	float coverage[ kPlateCount ];
	for( int i = 0; i < kPlateCount; ++i )
		coverage[ i ] = Coverage( s, plates, thresholds, i, px, py );

	//Inks are filters: Separation.cpp's InkModel is the GLSL's transmit loop.
	float printed[ 3 ];
	InkModel( coverage, s.ink, s.paper, s.inkDensity, printed );

	//The print exists where the picture does.
	const float alpha = source[ 3 ];
	const float result[ 4 ] = { printed[ 0 ] * alpha, printed[ 1 ] * alpha, printed[ 2 ] * alpha, alpha };//= mirrored

	for( int c = 0; c < 4; ++c )
		out[ c ] = glslMix( source[ c ], result[ c ], s.mix );//= mirrored
}

} // namespace rosette::print
