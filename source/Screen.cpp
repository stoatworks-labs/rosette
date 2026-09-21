#include "Screen.h"

#include <algorithm>
#include <cmath>

namespace rosette
{

const char* DotShapeName( DotShape shape )
{
	switch( shape )
	{
	case DotShape::Round: return "Round";
	case DotShape::Elliptical: return "Elliptical";
	case DotShape::Square: return "Square";
	case DotShape::Line: return "Line";
	default: return "?";
	}
}

/// The Elliptical dot's vertical stretch. 1.4 is a dot noticeably wider than
/// it is tall without the rows reading as lines at the midtone.
static constexpr float kEllipse = 1.4f;//= mirrored

float Spot( float x, float y, DotShape shape )
{
	const float ax = std::fabs( x );//= mirrored
	const float ay = std::fabs( y );//= mirrored

	switch( shape )
	{
	case DotShape::Round:
	case DotShape::Elliptical:
	{
		//How far along the way from the centre to the nearest corner, in a
		//metric that Elliptical stretches vertically. Continuous everywhere;
		//0 at the centre, 1 at the corner, and for Round exactly 0.5 on the
		//diamond |x| + |y| = 1, so the 50% tone is the classic checkerboard.
		const float ky = shape == DotShape::Round ? 1.0f : kEllipse;//= mirrored
		const float dc = std::sqrt( x * x + ky * ky * y * y );//= mirrored
		const float u  = 1.0f - ax;//= mirrored
		const float v  = 1.0f - ay;//= mirrored
		const float dk = std::sqrt( u * u + ky * ky * v * v );//= mirrored
		return dc / std::max( dc + dk, 1e-6f );//= mirrored
	}
	case DotShape::Square:
		return std::max( ax, ay );//= mirrored
	case DotShape::Line:
	default:
		return ay;//= mirrored
	}
}

std::vector< float > BuildThresholdTable()
{
	std::vector< float > table( static_cast< size_t >( kThresholdSize ) * static_cast< size_t >( DotShape::Count ) );

	std::vector< float > samples;
	samples.reserve( static_cast< size_t >( kRankSamples ) * kRankSamples );

	for( int s = 0; s < static_cast< int >( DotShape::Count ); ++s )
	{
		samples.clear();
		for( int j = 0; j < kRankSamples; ++j )
		{
			//Sample at the centres of a kRankSamples grid, so the four
			//quadrants are sampled identically.
			const float y = ( ( static_cast< float >( j ) + 0.5f ) / static_cast< float >( kRankSamples ) ) * 2.0f - 1.0f;
			for( int i = 0; i < kRankSamples; ++i )
			{
				const float x = ( ( static_cast< float >( i ) + 0.5f ) / static_cast< float >( kRankSamples ) ) * 2.0f - 1.0f;
				samples.push_back( Spot( x, y, static_cast< DotShape >( s ) ) );
			}
		}
		std::sort( samples.begin(), samples.end() );

		float* row = table.data() + static_cast< size_t >( s ) * kThresholdSize;
		for( int i = 0; i < kThresholdSize; ++i )
		{
			//The a-th quantile: the fraction of samples below it is a.
			const double a     = static_cast< double >( i ) / static_cast< double >( kThresholdSize - 1 );
			const size_t index = std::min( samples.size() - 1,
			                               static_cast< size_t >( a * static_cast< double >( samples.size() - 1 ) + 0.5 ) );
			row[ i ] = samples[ index ];
		}
		//The ends are pinned outside the function's range so that a 0% tone
		//has no ink under any antialiasing width and a 100% tone has no paper.
		row[ 0 ]                  = -1.0f;
		row[ kThresholdSize - 1 ] = 2.0f;
	}

	return table;
}

uint32_t HashInt( uint32_t x )
{
	//= mirrored
	x          = x * 747796405u + 2891336453u;
	uint32_t w = ( ( x >> ( ( x >> 28u ) + 4u ) ) ^ x ) * 277803737u;
	return ( w >> 22u ) ^ w;
}

float Hash01( uint32_t x )
{
	//= mirrored
	return static_cast< float >( HashInt( x ) ) * ( 1.0f / 4294967296.0f );
}

} // namespace rosette
