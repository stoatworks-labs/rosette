#include "Shaders.h"

namespace rosette
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through, in 0..1 picture space. The usual FFGL vertex shader
	//folds MaxUV in here; that happens where the input is read instead, so
	//every buffer of ours is addressed as the picture and nothing else.
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Pass 1: separate. Mirrored from Separation.cpp; every mirrored line is
// marked on both sides.
//---------------------------------------------------------------------------
const char* const kSeparateShader = R"(#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;           //the part of the input texture that is really picture
uniform vec2 HalfTexel;       //half an input texel, in picture space
uniform float BlackGeneration;//0 no black plate .. 1 maximum black replacement
uniform float TotalInk;       //2.0 .. 4.0, i.e. 200% .. 400%

in vec2 uv;
out vec4 fragColor;

void main()
{
	//Half a texel in from the edge. GL_LINEAR at the picture boundary takes
	//half its weight from the texture's undrawn padding, which would put a
	//row of half-tone cells down the side of every frame.
	vec2 picture = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );
	vec4 src = texture( InputTexture, picture * MaxUV );

	//Un-premultiply before separating: a soft alpha edge is not a darker
	//colour, and separating it as one would put ink where there is only
	//transparency.
	vec3 straight = src.a > 0.0031 ? src.rgb / src.a : vec3( 0.0 );

	float cp = 1.0 - clamp( straight.r, 0.0, 1.0 );//= mirrored
	float mp = 1.0 - clamp( straight.g, 0.0, 1.0 );//= mirrored
	float yp = 1.0 - clamp( straight.b, 0.0, 1.0 );//= mirrored

	float k = min( cp, min( mp, yp ) ) * BlackGeneration;//= mirrored

	float d = 1.0 - k;//= mirrored
	float c = d > 1e-4 ? ( cp - k ) / d : 0.0;//= mirrored
	float m = d > 1e-4 ? ( mp - k ) / d : 0.0;//= mirrored
	float y = d > 1e-4 ? ( yp - k ) / d : 0.0;//= mirrored

	float colour = c + m + y;//= mirrored
	if( colour + k > TotalInk && colour > 1e-6 )//= mirrored
	{
		float scale = max( 0.0, TotalInk - k ) / colour;//= mirrored
		c *= scale;//= mirrored
		m *= scale;//= mirrored
		y *= scale;//= mirrored
	}

	//Weighted by alpha: a transparent pixel carries no ink, and the box
	//filter in the mip chain then averages coverage, which is the right
	//thing for a cell that straddles the picture's edge.
	fragColor = vec4( c, m, y, k ) * src.a;
}
)";

//---------------------------------------------------------------------------
// The spot functions. A fragment: no #version, no main, no uniforms, and no
// dependency on anything outside itself. Shared verbatim between the print
// pass and the harness's probe. Mirrored from Screen.cpp.
//---------------------------------------------------------------------------
const char* const kSpotLibrary = R"(
const float kEllipse = 1.4;//= mirrored

//The spot value of cell point p in -1..1, for shape 0 Round, 1 Elliptical,
//2 Square, 3 Line. See Screen.h.
float spot( vec2 p, int shape )
{
	float ax = abs( p.x );//= mirrored
	float ay = abs( p.y );//= mirrored

	if( shape == 0 || shape == 1 )
	{
		float ky = shape == 0 ? 1.0 : kEllipse;//= mirrored
		float dc = sqrt( p.x * p.x + ky * ky * p.y * p.y );//= mirrored
		float u = 1.0 - ax;//= mirrored
		float v = 1.0 - ay;//= mirrored
		float dk = sqrt( u * u + ky * ky * v * v );//= mirrored
		return dc / max( dc + dk, 1e-6 );//= mirrored
	}
	if( shape == 2 )
		return max( ax, ay );//= mirrored
	return ay;//= mirrored
}

//An upper bound on |grad spot| at p, for the antialiasing width. Not
//mirrored: only the softness of the edge depends on it, never the area.
float spotSlope( vec2 p, int shape )
{
	if( shape == 0 || shape == 1 )
	{
		//s = a / ( a + b ) with |grad a|, |grad b| <= ky, so |grad s| <= ky / ( a + b ).
		float ky = shape == 0 ? 1.0 : kEllipse;
		float ax = abs( p.x );
		float ay = abs( p.y );
		float dc = sqrt( p.x * p.x + ky * ky * p.y * p.y );
		float u = 1.0 - ax;
		float v = 1.0 - ay;
		float dk = sqrt( u * u + ky * ky * v * v );
		return ky / max( dc + dk, 1e-3 );
	}
	return 1.0;
}
)";

//---------------------------------------------------------------------------
// Pass 2: print. Assembled from three pieces by PrintShaderSource(), because
// the middle one is shared with the harness. See Shaders.h.
//---------------------------------------------------------------------------
static const char* const kPrintPreamble = R"(#version 410 core

uniform sampler2D InputTexture;    //the host's picture, premultiplied
uniform sampler2D PlatesTexture;   //cmyk * alpha, mipmapped, picture space
uniform sampler2D ThresholdTexture;//kThresholdSize x shapes, R32F: T(tone)

uniform vec2 MaxUV;      //the part of the input texture that is really picture
uniform vec2 Size;       //the picture, in pixels
uniform vec2 HalfTexel;  //half a picture texel, in picture space
uniform float ShapeCount;//rows in ThresholdTexture

uniform float ScreenPx;  //pixels per cell
uniform float PlateLod;  //mip level whose texel is about one cell
uniform float DotShape;
uniform float DotGain;   //gain at the 50% tone
uniform float InkSpread; //edge half-width, in cell units

uniform vec4 Angles;          //radians, C M Y K
uniform vec2 Offsets[ 4 ];    //where each plate sits, in pixels
uniform vec4 PlateOn;         //1 prints, 0 does not, after Solo

uniform vec3 InkColour[ 4 ];
uniform vec3 Paper;
uniform float InkDensity;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;
)";

static const char* const kPrintMain = R"(
//The printed coverage of plate i at picture pixel `pix`.
float coverage( int i, vec2 pix, int shape )
{
	float angle = Angles[ i ];
	float c = cos( angle );
	float s = sin( angle );

	//Into the plate's own frame: shift by the registration error, then turn
	//by minus the screen angle so that a row of cells runs *along* the angle
	//in the picture, measured anticlockwise from horizontal with y up.
	vec2 rel = pix - Offsets[ i ];
	vec2 q = vec2( c * rel.x + s * rel.y, -s * rel.x + c * rel.y ) / ScreenPx;

	vec2 cell = floor( q );
	vec2 p = 2.0 * ( q - cell ) - 1.0;

	//The tone is the picture's mean over the cell, sampled once at the
	//cell's centre from the mip level whose texel is a cell wide. Back into
	//picture space to find where that centre is.
	vec2 centreQ = ( cell + 0.5 ) * ScreenPx;
	vec2 centrePix = vec2( c * centreQ.x - s * centreQ.y, s * centreQ.x + c * centreQ.y ) + Offsets[ i ];
	vec2 cuv = clamp( centrePix / Size, HalfTexel, vec2( 1.0 ) - HalfTexel );
	float tone = textureLod( PlatesTexture, cuv, PlateLod )[ i ];

	//Ink spreads: the dot on the paper is bigger than the dot on the plate.
	float a = clamp( tone, 0.0, 1.0 );//= mirrored
	a = clamp( a + 4.0 * DotGain * a * ( 1.0 - a ), 0.0, 1.0 );//= mirrored

	//The threshold that gives a dot of exactly area `a`, by rank.
	float T = texture( ThresholdTexture,
	                   vec2( ( a * 255.0 + 0.5 ) / 256.0, ( float( shape ) + 0.5 ) / ShapeCount ) ).r;

	//Analytic antialiasing. p moves by 2 / ScreenPx per pixel, so a
	//three-quarter-pixel edge is a spot-value half-width of slope * 0.75 /
	//ScreenPx; Ink Spread adds a softness measured in cells. fwidth would do
	//the same job everywhere except at the cell boundary, where fract()
	//jumps and fwidth reports an edge that is not there.
	float slope = spotSlope( p, shape );
	float w = slope * ( 0.75 / ScreenPx + InkSpread );
	float sv = spot( p, shape );

	float ink = 1.0 - smoothstep( T - w, T + w, sv );

	//A tone of exactly 0 or 1 is no dot or a solid, whatever the softness.
	ink = mix( ink, 1.0, smoothstep( 0.98, 1.0, a ) );
	ink = mix( ink, 0.0, 1.0 - smoothstep( 0.0, 0.02, a ) );

	return ink * PlateOn[ i ];
}

void main()
{
	vec4 source = texture( InputTexture, uv * MaxUV );
	vec2 pix = uv * Size;
	int shape = int( DotShape + 0.5 );

	//Inks are filters. Each plate removes a fraction of what the paper
	//reflects, per channel, and the plates multiply -- which is why cyan
	//over yellow is green and black over anything is black.
	vec3 transmit = vec3( 1.0 );//= mirrored
	for( int i = 0; i < 4; ++i )
	{
		float cov = coverage( i, pix, shape );
		vec3 absorb = clamp( cov * InkDensity * ( vec3( 1.0 ) - InkColour[ i ] ), 0.0, 1.0 );//= mirrored
		transmit *= vec3( 1.0 ) - absorb;//= mirrored
	}
	vec3 printed = Paper * transmit;//= mirrored

	//The print exists where the picture does: paper and ink inside the
	//clip's alpha, nothing outside it, so a logo on transparency prints as
	//itself and not as a rectangle of paper.
	vec4 result = vec4( printed * source.a, source.a );

	fragColor = mix( source, result, MixAmount );
}
)";

//---------------------------------------------------------------------------
// Assembly.
//---------------------------------------------------------------------------
std::string PrintShaderSource()
{
	return std::string( kPrintPreamble ) + kSpotLibrary + kPrintMain;
}

std::string SpotProbeShaderSource()
{
	//One cell point per pixel across a square target, with spot() written
	//straight to red. The target is RGBA32F, so what comes back is the
	//shader's own float and not a quantised version of it.
	static const char* const probeMain = R"(
uniform float Shape;
uniform float Samples;

out vec4 fragColor;

void main()
{
	vec2 p = ( ( gl_FragCoord.xy ) / Samples ) * 2.0 - 1.0;
	fragColor = vec4( spot( p, int( Shape + 0.5 ) ), 0.0, 0.0, 1.0 );
}
)";

	return std::string( "#version 410 core\n" ) + kSpotLibrary + probeMain;
}

} // namespace rosette
