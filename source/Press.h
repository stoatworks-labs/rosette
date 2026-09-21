#pragma once

#include <cstdint>

/**
    The press: where each plate is, and how it wanders.

    A real press never quite registers its plates, and the error is not
    static -- paper stretches, cylinders warm, the web tension breathes -- so
    the colour fringes on a printed run *move* from sheet to sheet. Here that
    is a slow, smooth, bounded noise per plate, added to the operator's
    registration offsets, and the audio can shake it.

    **Bounded noise, not a random walk.** The spec calls it a walk and a true
    walk is what it looks like over a few seconds, but a walk has no bound:
    left running for an hour it takes the plate off the frame. So each axis
    of each plate is a smooth value noise in time -- pseudo-random values on
    an integer lattice, joined by a quintic curve so the velocity is
    continuous too -- summed over two octaves and scaled to the amplitude.
    It is always within +-amplitude, it is different for every plate, and it
    is a pure function of time, so a frame rendered twice wanders identically.

    **Integer randomness.** The lattice values come from the same PCG hash
    the shaders use, seeded by plate, axis and lattice index. Nothing here
    depends on a `sin` or on the order frames arrive in.
*/
namespace rosette::press
{

struct Offset
{
	float x = 0.0f;
	float y = 0.0f;
};

/// Smooth value noise in -1..1 on lane `lane`, at time `t` in lattice units.
/// C1 continuous.
float Noise( uint32_t lane, double t );

/// Where plate `plate` has wandered to at `seconds`, in pixels, for a wander
/// of `amplitudePx` changing direction about `speedHz` times a second.
Offset Wander( int plate, double seconds, float amplitudePx, float speedHz );

/// A unit vector chosen by hash, for the direction an audio kick throws a
/// plate. `salt` picks a new one per kick.
Offset KickDirection( int plate, uint32_t salt );

} // namespace rosette::press
