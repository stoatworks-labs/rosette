#include "Separation.h"

#include <algorithm>
#include <cmath>

namespace rosette
{

const float kDefaultInk[ kPlateCount ][ 3 ] = {
	{ 0.00f, 0.68f, 0.94f },//cyan
	{ 0.93f, 0.00f, 0.55f },//magenta
	{ 1.00f, 0.95f, 0.00f },//yellow
	{ 0.14f, 0.12f, 0.13f },//black
};

const float kDefaultPaper[ 3 ] = { 1.0f, 1.0f, 1.0f };

Cmyk Separate( float r, float g, float b, float blackGeneration, float totalInk )
{
	const float cp = 1.0f - std::clamp( r, 0.0f, 1.0f );//= mirrored
	const float mp = 1.0f - std::clamp( g, 0.0f, 1.0f );//= mirrored
	const float yp = 1.0f - std::clamp( b, 0.0f, 1.0f );//= mirrored

	//The black plate takes its share of the neutral component.
	const float k = std::min( cp, std::min( mp, yp ) ) * blackGeneration;//= mirrored

	//Normalised so the round trip through multiplicative overprint is the
	//identity with ideal inks. At k == 1 the point is solid black already
	//and the colour plates carry nothing.
	const float d = 1.0f - k;//= mirrored
	float c       = d > 1e-4f ? ( cp - k ) / d : 0.0f;//= mirrored
	float m       = d > 1e-4f ? ( mp - k ) / d : 0.0f;//= mirrored
	float y       = d > 1e-4f ? ( yp - k ) / d : 0.0f;//= mirrored

	//Total ink limit: scale the colour plates, never the black.
	const float colour = c + m + y;//= mirrored
	if( colour + k > totalInk && colour > 1e-6f )//= mirrored
	{
		const float scale = std::max( 0.0f, totalInk - k ) / colour;//= mirrored
		c *= scale;//= mirrored
		m *= scale;//= mirrored
		y *= scale;//= mirrored
	}

	return Cmyk { c, m, y, k };
}

float DotGain( float tone, float gain )
{
	const float a = std::clamp( tone, 0.0f, 1.0f );//= mirrored
	return std::clamp( a + 4.0f * gain * a * ( 1.0f - a ), 0.0f, 1.0f );//= mirrored
}

void InkModel( const float coverage[ kPlateCount ], const float ink[ kPlateCount ][ 3 ],
               const float paper[ 3 ], float density, float out[ 3 ] )
{
	for( int ch = 0; ch < 3; ++ch )
	{
		float transmit = 1.0f;//= mirrored
		for( int i = 0; i < kPlateCount; ++i )
		{
			const float absorb = std::clamp( coverage[ i ] * density * ( 1.0f - ink[ i ][ ch ] ), 0.0f, 1.0f );//= mirrored
			transmit *= 1.0f - absorb;//= mirrored
		}
		out[ ch ] = paper[ ch ] * transmit;//= mirrored
	}
}

} // namespace rosette
