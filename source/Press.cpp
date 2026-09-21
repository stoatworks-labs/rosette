#include "Press.h"

#include "Screen.h"

#include <cmath>

namespace rosette::press
{
namespace
{
/// A lattice value in -1..1 for lane `lane` at integer `index`.
float latticeValue( uint32_t lane, int64_t index )
{
	//Two multiplies by large odd constants keep neighbouring lanes and
	//neighbouring indices far apart in the hash's input space.
	const uint32_t seed = lane * 2654435761u ^ static_cast< uint32_t >( index ) * 340573321u;
	return Hash01( HashInt( seed ) ) * 2.0f - 1.0f;
}

/// Quintic ease, which has zero first AND second derivative at both ends, so
/// the joined curve has continuous velocity and the plate never kinks.
float quintic( float f )
{
	return f * f * f * ( f * ( f * 6.0f - 15.0f ) + 10.0f );
}
} // namespace

float Noise( uint32_t lane, double t )
{
	const double floored = std::floor( t );
	const int64_t index  = static_cast< int64_t >( floored );
	const float f        = static_cast< float >( t - floored );

	const float a = latticeValue( lane, index );
	const float b = latticeValue( lane, index + 1 );
	return a + ( b - a ) * quintic( f );
}

Offset Wander( int plate, double seconds, float amplitudePx, float speedHz )
{
	if( amplitudePx <= 0.0f )
		return Offset {};

	//Two octaves: the slow one is the drift, the fast one at a third of the
	//weight is the jitter on top of it. The second octave's lane is offset
	//so it never shares a lattice value with the first, and its rate is an
	//irrational-looking multiple so the two never beat visibly.
	const uint32_t base = static_cast< uint32_t >( plate ) * 16u;
	const double t1     = seconds * static_cast< double >( speedHz );
	const double t2     = t1 * 2.618 + 7.0;

	Offset out;
	out.x = amplitudePx * ( 0.75f * Noise( base + 0u, t1 ) + 0.25f * Noise( base + 2u, t2 ) );
	out.y = amplitudePx * ( 0.75f * Noise( base + 1u, t1 ) + 0.25f * Noise( base + 3u, t2 ) );
	return out;
}

Offset KickDirection( int plate, uint32_t salt )
{
	const float angle = Hash01( HashInt( static_cast< uint32_t >( plate ) * 977u + salt * 7919u ) ) * 6.28318530718f;
	return Offset { std::cos( angle ), std::sin( angle ) };
}

} // namespace rosette::press
