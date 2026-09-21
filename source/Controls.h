#pragma once

/**
    Host parameters are 0..1; these are what they mean.

    Every ranged parameter this plugin declares is a plain FF_TYPE_STANDARD
    float in 0..1, including the ones that stand for pixels, degrees or a
    percentage of ink. That is not a style preference: `SetParamInfo` clamps a
    standard default into 0..1 *before* returning, and `SetParamRange` can only
    be called afterwards -- so a parameter declared in pixels cannot declare a
    default in pixels, and 8 would silently become 1. The conversions live
    here, in one file the plugin and the harness both use, so there is only
    ever one answer to what a slider position means.

    Where a mapping is geometric it is because the interesting range is at one
    end: the difference between a 2 px screen and a 4 px one is a different
    picture, the difference between 36 px and 38 px is not.
*/
namespace rosette
{

/// 0 to 1, linear. How much of the neutral component of a colour is moved
/// off the CMY plates and onto the black plate: 0 prints greys as equal parts
/// cyan, magenta and yellow; 1 prints them as black alone.
float BlackGenerationFromParam( float value );

/// 200% to 400%, linear, returned as 2.0 to 4.0. The most ink one point on
/// the paper may carry, summed over the four plates. 400% is no limit at all.
float TotalInkFromParam( float value );

/// 2 to 40 pixels per dot, geometrically. The screen ruling, expressed in the
/// only unit that means the same thing on every canvas.
float ScreenPxFromParam( float value );

/// 0 to 0.4, linear: the dot gain at the 50% tone, in fractional area. A real
/// litho press gains ten to twenty points in the midtone; newsprint gains
/// thirty. See `DotGain` in Separation.h for the curve.
float DotGainFromParam( float value );

/// 0 to 0.5 of a cell, linear. How softly the ink meets the paper: the width
/// of a dot's edge, over and above the one-pixel antialiasing.
float InkSpreadFromParam( float value );

/// 0 to 180 degrees, returned in radians. Screen angles repeat every 180
/// degrees, so the whole travel is the whole range.
float AngleFromParam( float value );

/// -20 to +20 pixels, linear, with 0.5 as zero. A registration error.
float RegisterFromParam( float value );

/// 0 to 20 pixels, linear. The amplitude of the press's slow wander.
float WanderFromParam( float value );

/// 0.05 to 2 Hz, geometrically. How quickly the wander changes direction.
float WanderSpeedFromParam( float value );

/// 0 to 20 pixels, linear. How far the audio can shake a plate.
float AudioDriveFromParam( float value );

/// 0 to 1.5, linear, with 0.667 as unity. Scales every ink's absorption.
float InkDensityFromParam( float value );

} // namespace rosette
