#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace rosette
{
namespace
{
inline float clamp01( float value )
{
	return std::min( std::max( value, 0.0f ), 1.0f );
}

inline float lerp( float from, float to, float t )
{
	return from + ( to - from ) * clamp01( t );
}

/// Geometric interpolation. Equal slider movements are equal *ratios*, which
/// is the right behaviour for any quantity where the question is "how many
/// times more" rather than "how much more".
inline float geometric( float from, float to, float t )
{
	return from * std::pow( to / from, clamp01( t ) );
}
} // namespace

float BlackGenerationFromParam( float value )
{
	return clamp01( value );
}

float TotalInkFromParam( float value )
{
	return lerp( 2.0f, 4.0f, value );
}

float ScreenPxFromParam( float value )
{
	return geometric( 2.0f, 40.0f, value );
}

float DotGainFromParam( float value )
{
	return lerp( 0.0f, 0.4f, value );
}

float InkSpreadFromParam( float value )
{
	return lerp( 0.0f, 0.5f, value );
}

float AngleFromParam( float value )
{
	return clamp01( value ) * 3.14159265358979323846f;
}

float RegisterFromParam( float value )
{
	return lerp( -20.0f, 20.0f, value );
}

float WanderFromParam( float value )
{
	return lerp( 0.0f, 20.0f, value );
}

float WanderSpeedFromParam( float value )
{
	return geometric( 0.05f, 2.0f, value );
}

float AudioDriveFromParam( float value )
{
	return lerp( 0.0f, 20.0f, value );
}

float InkDensityFromParam( float value )
{
	return lerp( 0.0f, 1.5f, value );
}

} // namespace rosette
