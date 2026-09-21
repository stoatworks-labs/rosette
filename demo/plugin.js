/**
 * Rosette — browser demo.
 *
 * Offset litho: four halftone plates, each a lattice of dots at its own screen
 * angle, laid down one after another by a press that never quite registers
 * them. The rosettes, the moire, the dot gain and the overprint are not drawn —
 * they fall out of the model, and the model is almost entirely in the shaders,
 * which is why this plugin ports to a browser more completely than any of its
 * siblings.
 *
 * The five shader constants below are `kVertexShader`, `kSeparateShader`,
 * `kSpotLibrary`, `kPrintPreamble` and `kPrintMain` from `source/Shaders.cpp`,
 * copied across unedited and assembled the way `PrintShaderSource()` assembles
 * them — preamble, then the spot library, then main.
 * `demo/tools/check_shaders.py` compares them to the C++ character for
 * character and `tools/verify.sh` runs it, because two copies of a shader is
 * exactly the arrangement that drifts.
 *
 * Everything else here is a **port**, hand-translated and checked by nobody but
 * a reader:
 *
 *   - `source/Controls.cpp` — every slider position to the number the shader
 *     is handed. Screen in pixels per dot, angles in radians, registration in
 *     pixels, gain in points at the 50% tone.
 *   - `source/Screen.cpp` — `Spot()` and `BuildThresholdTable()`. The
 *     threshold table is the one CPU stage that matters: a dot's *shape* is a
 *     spot function, and its *area* is made exact by ranking that function
 *     over the cell and reading T(a) off as the a-th quantile. Ported rather
 *     than approximated, because if T(a) were simply `a` a round dot's area
 *     would go as the square of the tone and Dot Gain would stop being the
 *     only thing between a tone and its printed area.
 *   - `source/Press.cpp` — the wander. Bounded value noise in time, joined by
 *     a quintic, over the same PCG integer hash the plugin uses, so a frame
 *     rendered twice wanders identically. It is a pure function of time, which
 *     is the reason it survives the trip to a browser at all.
 *   - `source/Presets.h` — the six factory rows, mirrored column for column.
 *
 * ------------------------------------------------------- what is missing
 *
 * **The audio side, entirely.** `Audio Drive` and the FFT buffer it reads are
 * absent rather than present and dead: the spectrum reaches the plugin through
 * a Resolume parameter, a browser has no equivalent, and asking a visitor for
 * their microphone to demonstrate a video effect is not a trade worth making.
 *
 * That removal is exact rather than approximate, which is worth saying. With
 * Audio Drive at its default of zero the plugin computes `shake` and `kick` as
 * zero, `press::Wander` returns `{0,0}` on its `amplitudePx <= 0` early exit,
 * and the plate lands on registration plus wander alone — which is what this
 * page computes. The audio path is not simplified here; it is switched off in
 * the same place the plugin switches it off.
 *
 * **The Preset parameter is a dropdown in the kit's panel, not a parameter.**
 * In the plugin a preset is an OVERRIDE laid over the operator's values at read
 * time, because Resolume does not consume value events and a plugin therefore
 * cannot push values back into the inspector. A browser has no such problem, so
 * the six rows are wired to the kit's preset control and they write the sliders
 * — which is what the plugin would do if it could. The rows themselves are the
 * plugin's, column for column. One consequence: picking a preset here also
 * resets the registration offsets, Solo and Mix to their defaults, where the
 * plugin deliberately leaves those to the operator.
 *
 * **Nothing here is measured.** `rztest --spot`, `--gain`, `--angle`,
 * `--register`, `--overprint` and `--identity` are why the model is worth
 * believing, and they are an offline harness in the repository, not this page.
 *
 * What this page is NOT: it is the plugin's shaders, not the plugin. No
 * Resolume, no FFGL, no C++ — and GLSL ES 3.00 rather than desktop GL 4.1 core,
 * which the kit's `port()` handles. A pixel here is not evidence about a pixel
 * there.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, GLError, bindTexture, mipLevels } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//
// The backticks inside the comments are escaped, because a template literal has
// nowhere else to go; check_shaders.py decodes that one escape before comparing
// and rejects any other backslash, so the escape cannot hide a difference.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

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
`;

const SEPARATE = `#version 410 core

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
`;

const SPOT_LIBRARY = `
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
`;

const PRINT_PREAMBLE = `#version 410 core

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
`;

const PRINT_MAIN = `
//The printed coverage of plate i at picture pixel \`pix\`.
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

	//The threshold that gives a dot of exactly area \`a\`, by rank.
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
`;

//The print pass, assembled exactly as `PrintShaderSource()` assembles it.
//check_shaders.py asserts that the C++ still joins the same three pieces in
//this order, so a change to the assembly there fails here rather than making
//the page quietly run a different shader.
const PRINT = PRINT_PREAMBLE + SPOT_LIBRARY + PRINT_MAIN;

//---------------------------------------------------------------------------
// A port of source/Controls.cpp.
//
// Every ranged parameter the plugin declares is a plain 0..1 float, because
// SetParamInfo clamps a standard default into 0..1 before SetParamRange can
// widen it — so a control in pixels or degrees cannot declare its default in
// pixels or degrees, and the conversion lives in one file that the plugin and
// its harness both use. These are that file.
//---------------------------------------------------------------------------

const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
const lerp = (from, to, t) => from + (to - from) * clamp01(t);

/** Equal slider movements are equal ratios — right wherever the question is
 *  "how many times more" rather than "how much more". */
const geometric = (from, to, t) => from * Math.pow(to / from, clamp01(t));

const blackGenerationFromParam = (v) => clamp01(v);
const totalInkFromParam = (v) => lerp(2, 4, v);
const screenPxFromParam = (v) => geometric(2, 40, v);
const dotGainFromParam = (v) => lerp(0, 0.4, v);
const inkSpreadFromParam = (v) => lerp(0, 0.5, v);
const angleFromParam = (v) => clamp01(v) * Math.PI;
const registerFromParam = (v) => lerp(-20, 20, v);
const wanderFromParam = (v) => lerp(0, 20, v);
const wanderSpeedFromParam = (v) => geometric(0.05, 2, v);
const inkDensityFromParam = (v) => lerp(0, 1.5, v);
//`audioDriveFromParam` is deliberately absent: see the note at the top.

//---------------------------------------------------------------------------
// A port of source/Screen.cpp — the spot functions and the threshold table.
//
// A dot shape is a spot function over one cell, with p in [-1,1]^2; a dot for
// tone `a` is the set of points whose spot value is below a threshold T(a).
// The shape and the area are then separate questions, and the area is made
// exact by RANKING: sample the spot function over the cell, sort, and read
// T(a) off as the a-th quantile. The fraction of the cell below T(a) is then
// `a` by construction for every shape.
//
// `spot()` here is the CPU twin of the GLSL above, and it exists for the same
// reason the plugin keeps two copies: the table has to be built before there
// is anything to sample a shader with.
//---------------------------------------------------------------------------

const kEllipse = 1.4;
const kThresholdSize = 256;
const kRankSamples = 512;
const kShapeCount = 4;

/** `rosette::Spot`. Shapes: 0 Round, 1 Elliptical, 2 Square, 3 Line. */
function spot(x, y, shape) {
  const ax = Math.abs(x);
  const ay = Math.abs(y);

  if (shape === 0 || shape === 1) {
    const ky = shape === 0 ? 1.0 : kEllipse;
    const dc = Math.sqrt(x * x + ky * ky * y * y);
    const u = 1.0 - ax;
    const v = 1.0 - ay;
    const dk = Math.sqrt(u * u + ky * ky * v * v);
    return dc / Math.max(dc + dk, 1e-6);
  }
  if (shape === 2) return Math.max(ax, ay);
  return ay;
}

/**
 * `rosette::BuildThresholdTable`. 256 entries per shape, shape-major.
 *
 * A quarter of a million samples per shape, sorted, four times over. It takes
 * a couple of hundred milliseconds once, at renderer construction, and is then
 * a 256x4 texture for the life of the page. Doing it at a coarser sampling
 * would be the one shortcut that shows: the quantiles move, the printed area
 * stops matching the tone, and the picture is *plausible* rather than obviously
 * wrong — which is the failure mode the plugin's own harness exists to catch.
 */
function buildThresholdTable() {
  const table = new Float32Array(kThresholdSize * kShapeCount);
  const samples = new Float32Array(kRankSamples * kRankSamples);

  for (let s = 0; s < kShapeCount; s += 1) {
    let n = 0;
    for (let j = 0; j < kRankSamples; j += 1) {
      //Sample at the centres of a kRankSamples grid, so the four quadrants
      //are sampled identically.
      const y = ((j + 0.5) / kRankSamples) * 2 - 1;
      for (let i = 0; i < kRankSamples; i += 1) {
        const x = ((i + 0.5) / kRankSamples) * 2 - 1;
        samples[n] = spot(x, y, s);
        n += 1;
      }
    }
    //A TypedArray sorts numerically; Array.prototype.sort would sort these
    //as strings and put 0.5 before 0.09.
    samples.sort();

    const row = s * kThresholdSize;
    for (let i = 0; i < kThresholdSize; i += 1) {
      const a = i / (kThresholdSize - 1);
      const index = Math.min(samples.length - 1, Math.round(a * (samples.length - 1)));
      table[row + i] = samples[index];
    }
    //Pinned outside the function's range, so a 0% tone has no ink under any
    //antialiasing width and a 100% tone has no paper.
    table[row] = -1.0;
    table[row + kThresholdSize - 1] = 2.0;
  }

  return table;
}

//---------------------------------------------------------------------------
// A port of source/Press.cpp — where each plate is, and how it wanders.
//
// Bounded noise, never a random walk: a walk has no bound and takes a plate
// off the frame if left running. Each axis of each plate is smooth value noise
// in time — pseudo-random values on an integer lattice joined by a quintic, so
// velocity is continuous too — summed over two octaves and scaled to the
// amplitude.
//
// The randomness is the plugin's PCG-style integer hash, exact in 32 bits.
// `Math.imul` is not a micro-optimisation here: `x * 747796405` in JavaScript
// is a double multiply that silently loses the low bits past 2^53, and the
// whole point of an integer hash is that both sides agree bit for bit.
//---------------------------------------------------------------------------

/** `rosette::HashInt`. */
function hashInt(x) {
  x = (Math.imul(x >>> 0, 747796405) + 2891336453) >>> 0;
  const w = Math.imul(((x >>> ((x >>> 28) + 4)) ^ x) >>> 0, 277803737) >>> 0;
  return ((w >>> 22) ^ w) >>> 0;
}

/** `rosette::Hash01`. */
const hash01 = (x) => hashInt(x) / 4294967296;

/** `rosette::press::latticeValue` — a value in -1..1 for a lane and an index. */
function latticeValue(lane, index) {
  //Two multiplies by large odd constants keep neighbouring lanes and
  //neighbouring indices far apart in the hash's input space. C++ precedence
  //puts both multiplies before the xor.
  const seed = (Math.imul(lane >>> 0, 2654435761) ^ Math.imul(index >>> 0, 340573321)) >>> 0;
  return hash01(hashInt(seed)) * 2 - 1;
}

/** Quintic ease: zero first AND second derivative at both ends, so the joined
 *  curve has continuous velocity and the plate never kinks. */
const quintic = (f) => f * f * f * (f * (f * 6 - 15) + 10);

/** `rosette::press::Noise`. */
function pressNoise(lane, t) {
  const floored = Math.floor(t);
  const f = t - floored;
  const a = latticeValue(lane, floored);
  const b = latticeValue(lane, floored + 1);
  return a + (b - a) * quintic(f);
}

/** `rosette::press::Wander`, in pixels. */
function pressWander(plate, seconds, amplitudePx, speedHz) {
  if (amplitudePx <= 0) return [0, 0];

  //Two octaves: the slow one is the drift, the fast one at a third of the
  //weight is the jitter on top of it. The second octave's lane is offset so it
  //never shares a lattice value with the first, and its rate is an
  //irrational-looking multiple so the two never beat visibly.
  const base = plate * 16;
  const t1 = seconds * speedHz;
  const t2 = t1 * 2.618 + 7.0;

  return [
    amplitudePx * (0.75 * pressNoise(base + 0, t1) + 0.25 * pressNoise(base + 2, t2)),
    amplitudePx * (0.75 * pressNoise(base + 1, t1) + 0.25 * pressNoise(base + 3, t2)),
  ];
}

//`press::KickDirection` is not ported. It exists to throw a plate on an audio
//onset, and there are no onsets here.

//---------------------------------------------------------------------------
// The chain — the plugin's two passes, in the plugin's order.
//---------------------------------------------------------------------------

const REGISTER_IDS = [
  ['regCX', 'regCY'],
  ['regMX', 'regMY'],
  ['regYX', 'regYY'],
  ['regKX', 'regKY'],
];

const INK_IDS = [
  ['inkCR', 'inkCG', 'inkCB'],
  ['inkMR', 'inkMG', 'inkMB'],
  ['inkYR', 'inkYG', 'inkYB'],
  ['inkKR', 'inkKG', 'inkKB'],
];

/**
 * The kit has no vec2-array setter — `setArray` covers 1, 3 and 4 components
 * — and `uniform1fv` into a `vec2[4]` is rejected as a size mismatch, leaving
 * the uniform holding whatever it held before. For `Offsets` that is every
 * plate at zero, i.e. a press in perfect registration, which looks like a
 * working demo with two dead groups of controls.
 */
function setVec2Array(gl, program, name, values) {
  const location = program.location(`${name}[0]`) ?? program.location(name);
  if (location !== null) gl.uniform2fv(location, values);
}

function createRenderer(gl, quad) {
  // The threshold table is an R32F texture sampled with GL_LINEAR, exactly as
  // `Rosette::UploadThresholds` uploads it: a tone between two entries has to
  // get a threshold between two entries or the dot area steps in 256 visible
  // stages. Desktop GL filters float textures as a matter of course; WebGL2
  // makes it an extension, and a float texture with a LINEAR filter and no
  // extension is INCOMPLETE — it samples as opaque black, which here means
  // every threshold reads 0 and every plate prints solid. Fail loudly instead.
  if (!gl.getExtension('OES_texture_float_linear')) {
    throw new GLError(
      'OES_texture_float_linear is missing. The threshold table that makes a dot cover exactly its tone is a float texture read with a linear filter, and without it every dot would print solid.',
    );
  }

  const separateShader = new Program(gl, VERTEX, SEPARATE, 'separate');
  const printShader = new Program(gl, VERTEX, PRINT, 'print');

  // cmyk * alpha, mipmapped. The mip chain is not an optimisation: the tone of
  // a cell is the picture's MEAN over that cell, and the print pass reads it
  // with one textureLod at the level whose texel is about a cell wide.
  const plates = new PassBuffer(gl, { filter: 'linear', mip: true });

  const thresholdTexture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, thresholdTexture);
  gl.texImage2D(
    gl.TEXTURE_2D, 0, gl.R32F, kThresholdSize, kShapeCount, 0,
    gl.RED, gl.FLOAT, buildThresholdTable(),
  );
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);

  const offsets = new Float32Array(8);
  const inks = new Float32Array(12);

  return {
    render({ input, params, width, height, time }) {
      // The picture and the output are the same raster here; in Resolume they
      // are the clip and the composition, which Rosette also treats as one.
      const halfTexelX = 0.5 / width;
      const halfTexelY = 0.5 / height;

      //-------------------------------------------------------------------
      // The press. Where each plate sits this frame: the operator's
      // registration plus the wander. The plugin adds a continuous audio
      // shake on a faster lane and a kick in a hashed direction; with no
      // audio both are identically zero, which is the state this page is in.
      //-------------------------------------------------------------------
      const wander = wanderFromParam(params.get('wander'));
      const wanderSpeed = wanderSpeedFromParam(params.get('wanderSpeed'));

      for (let plate = 0; plate < 4; plate += 1) {
        const [wx, wy] = pressWander(plate, time, wander, wanderSpeed);
        offsets[plate * 2 + 0] = registerFromParam(params.get(REGISTER_IDS[plate][0])) + wx;
        offsets[plate * 2 + 1] = registerFromParam(params.get(REGISTER_IDS[plate][1])) + wy;
      }

      gl.disable(gl.BLEND);

      //-------------------------------------------------------------------
      // 1. Separate. RGB to CMYK per pixel, weighted by alpha, into a
      //    mipmapped float buffer.
      //-------------------------------------------------------------------
      plates.ensure(width, height, gl.RGBA16F);
      plates.bind();

      separateShader.use();
      bindTexture(gl, 0, input.texture);
      separateShader.setSampler('InputTexture', 0);
      separateShader.set('MaxUV', 1, 1);
      separateShader.set('HalfTexel', halfTexelX, halfTexelY);
      separateShader.set('BlackGeneration', blackGenerationFromParam(params.get('blackGen')));
      separateShader.set('TotalInk', totalInkFromParam(params.get('totalInk')));
      quad.draw();

      plates.generateMipmap();

      //-------------------------------------------------------------------
      // 2. Print. Four lattices, four dots, the ink model, the paper, Mix.
      //    Straight to the canvas, which is this page's equivalent of the
      //    host's framebuffer.
      //-------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);

      printShader.use();
      bindTexture(gl, 0, input.texture);
      bindTexture(gl, 1, plates.texture);
      bindTexture(gl, 2, thresholdTexture);
      printShader.setSampler('InputTexture', 0);
      printShader.setSampler('PlatesTexture', 1);
      printShader.setSampler('ThresholdTexture', 2);

      printShader.set('MaxUV', 1, 1);
      printShader.set('Size', width, height);
      printShader.set('HalfTexel', halfTexelX, halfTexelY);
      printShader.set('ShapeCount', kShapeCount);

      const screenPx = screenPxFromParam(params.get('screen'));
      printShader.set('ScreenPx', screenPx);
      // The mip level whose texel is about one cell. `mipLevels` counts the
      // levels; the highest one is that minus one, and it has to be clamped
      // there or a 40-pixel screen on a small canvas asks for a level that
      // does not exist.
      const maxMipLevel = mipLevels(plates.width, plates.height) - 1;
      printShader.set('PlateLod', Math.min(Math.max(Math.log2(screenPx), 0), maxMipLevel));
      printShader.set('DotShape', params.option('dotShape'));
      printShader.set('DotGain', dotGainFromParam(params.get('dotGain')));
      printShader.set('InkSpread', inkSpreadFromParam(params.get('inkSpread')));

      printShader.set(
        'Angles',
        angleFromParam(params.get('angleC')), angleFromParam(params.get('angleM')),
        angleFromParam(params.get('angleY')), angleFromParam(params.get('angleK')),
      );
      setVec2Array(gl, printShader, 'Offsets', offsets);

      // Solo wins over the plate switches: it is "show me this lattice", and
      // it would be no use if the plate happened to be off.
      const solo = params.option('solo');
      const plateIds = ['plateC', 'plateM', 'plateY', 'plateK'];
      const on = plateIds.map((id, plate) =>
        (solo > 0 ? (solo - 1 === plate ? 1 : 0) : (params.get(id) > 0.5 ? 1 : 0)));
      printShader.set('PlateOn', on[0], on[1], on[2], on[3]);

      for (let plate = 0; plate < 4; plate += 1) {
        for (let channel = 0; channel < 3; channel += 1) {
          inks[plate * 3 + channel] = params.get(INK_IDS[plate][channel]);
        }
      }
      printShader.setArray('InkColour', inks, 3);
      printShader.set('Paper', params.get('paperR'), params.get('paperG'), params.get('paperB'));
      printShader.set('InkDensity', inkDensityFromParam(params.get('inkDensity')));
      printShader.set('MixAmount', params.get('mix'));

      quad.draw();
    },
  };
}

//---------------------------------------------------------------------------
// The controls, read out of the plugin's own constructor. Same names, same
// groups, same order, same defaults, same dropdown elements.
//
// Absent: the Audio buffer and Audio Drive, for the reason at the top of this
// file; the Preset dropdown, which is the kit's preset control here; and the
// About block, which is four buttons that open a browser.
//---------------------------------------------------------------------------

const degrees = (v) => `${(angleFromParam(v) * 180 / Math.PI).toFixed(0)}°`;
const pixels = (v) => `${registerFromParam(v).toFixed(1)} px`;
const percent = (v) => `${Math.round(clamp01(v) * 100)}%`;

// The plugin's default inks and paper, from Separation.cpp by way of the
// constructor: process cyan, magenta and yellow at the values the trade quotes
// them, and black as a rich near-black rather than 0,0,0 — a solid of real
// black ink reflects a few per cent, and that is what makes shadow detail
// survive on a print.
const INK_C = [0.00, 0.68, 0.94];
const INK_M = [0.93, 0.00, 0.55];
const INK_Y = [1.00, 0.95, 0.00];
const INK_K = [0.14, 0.12, 0.13];

/** One preset row, in the order of `presets::Param`, spelled out so a reader
 *  can hold it against `source/Presets.h` column for column. */
function row({
  blackGen, totalInk, screen, dotShape, dotGain, inkSpread,
  angleC, angleM, angleY, angleK, wander, wanderSpeed,
  inkC, inkM, inkY, inkK, paper, inkDensity, plates,
}) {
  return {
    blackGen, totalInk, screen, dotShape, dotGain, inkSpread,
    angleC, angleM, angleY, angleK, wander, wanderSpeed,
    inkCR: inkC[0], inkCG: inkC[1], inkCB: inkC[2],
    inkMR: inkM[0], inkMG: inkM[1], inkMB: inkM[2],
    inkYR: inkY[0], inkYG: inkY[1], inkYB: inkY[2],
    inkKR: inkK[0], inkKG: inkK[1], inkKB: inkK[2],
    paperR: paper[0], paperG: paper[1], paperB: paper[2],
    inkDensity,
    plateC: plates[0], plateM: plates[1], plateY: plates[2], plateK: plates[3],
  };
}

mountDemo({
  name: 'Rosette',
  pluginId: 'RZ01',
  tagline:
    'Offset litho: four halftone plates at their own screen angles, laid down by a press that never quite registers them. The rosettes, the moire, the colour fringes that wander, the dot gain and the overprint all fall out of the model rather than being drawn. Solo a plate to see its lattice; start from a preset.',
  repo: 'https://github.com/stoatworks-labs/rosette',

  // The print carries the clip's alpha: paper and ink live inside it, so a
  // logo on transparency prints as itself and not as a rectangle of paper.
  showBackdrop: true,

  // The separation buffer is RGBA16F and mipmapped. A float render target is
  // an opt-in in WebGL2 where desktop GL simply has it — and eight bits would
  // quantise the cell tone that every dot's area is computed from, which shows
  // up as banding in the midtones rather than as an error.
  needFloat: true,

  params: [
    { id: 'blackGen', name: 'Black Generation', type: 'standard', default: 1.0, group: 'Separation',
      display: percent,
      hint: 'How much of the neutral component moves off the CMY plates onto the black plate. At 0 a grey prints as equal parts cyan, magenta and yellow — three lattices interfering where one would have done; at 1 it prints as black alone.' },
    { id: 'totalInk', name: 'Total Ink', type: 'standard', default: 0.5, group: 'Separation',
      display: (v) => `${Math.round(totalInkFromParam(v) * 100)}%`,
      hint: 'The most ink one point on the paper may carry, summed over the four plates. 400% is no limit at all; a newspaper cannot dry much past 240%. The limit scales the three colour plates and never the black.' },

    { id: 'screen', name: 'Screen', type: 'standard', default: 0.46, group: 'Screen',
      display: (v) => `${screenPxFromParam(v).toFixed(1)} px per dot`,
      hint: 'The screen ruling, in the only unit that means the same thing on every canvas. Geometric, because the difference between 2 px and 4 px is a different picture and the difference between 36 and 38 is not.' },
    { id: 'dotShape', name: 'Dot Shape', type: 'option', default: 0, group: 'Screen',
      elements: ['Round', 'Elliptical', 'Square', 'Line'],
      hint: 'Round is a near-circle that turns over into a checkerboard of diamonds at exactly 50%. Elliptical stretches the same function so the dot chains along the screen angle and never flips abruptly. Square never inverts. Line is a band along the angle.' },
    { id: 'dotGain', name: 'Dot Gain', type: 'standard', default: 0.375, group: 'Screen',
      display: (v) => `${Math.round(dotGainFromParam(v) * 100)} points at 50%`,
      hint: 'Ink spreads into the paper, so a printed dot is bigger than the dot on the plate — most in the midtone, where a dot has the most edge for its area. Quoted the way a pressman quotes it: 20 means a 50% dot prints as 70%.' },
    { id: 'inkSpread', name: 'Ink Spread', type: 'standard', default: 0.1, group: 'Screen',
      display: (v) => `${inkSpreadFromParam(v).toFixed(2)} cell`,
      hint: 'How softly the ink meets the paper: the width of a dot’s edge, over and above the one-pixel antialiasing. This is softness, not area — Dot Gain is the only thing that moves the area.' },
    { id: 'angleC', name: 'Angle C', type: 'standard', default: 0.083333, group: 'Screen', display: degrees,
      hint: 'The four screen angles are what makes a rosette. The traditional set is 15, 75, 0 and 45 degrees: 30 apart for the three strong inks, with yellow squeezed in where the eye is least able to see the pattern.' },
    { id: 'angleM', name: 'Angle M', type: 'standard', default: 0.416667, group: 'Screen', display: degrees,
      hint: 'Bring this within a degree or two of Angle C and the two lattices beat against each other — that is moire, and it is real interference rather than a texture.' },
    { id: 'angleY', name: 'Angle Y', type: 'standard', default: 0.0, group: 'Screen', display: degrees },
    { id: 'angleK', name: 'Angle K', type: 'standard', default: 0.25, group: 'Screen', display: degrees },

    { id: 'regCX', name: 'Register C X', type: 'standard', default: 0.5, group: 'Press', display: pixels,
      hint: 'A registration error, in pixels. The centre of the travel is dead register; a plate off by a couple of pixels is the colour fringe you see on a cheaply printed flyer.' },
    { id: 'regCY', name: 'Register C Y', type: 'standard', default: 0.5, group: 'Press', display: pixels },
    { id: 'regMX', name: 'Register M X', type: 'standard', default: 0.5, group: 'Press', display: pixels },
    { id: 'regMY', name: 'Register M Y', type: 'standard', default: 0.5, group: 'Press', display: pixels },
    { id: 'regYX', name: 'Register Y X', type: 'standard', default: 0.5, group: 'Press', display: pixels },
    { id: 'regYY', name: 'Register Y Y', type: 'standard', default: 0.5, group: 'Press', display: pixels },
    { id: 'regKX', name: 'Register K X', type: 'standard', default: 0.5, group: 'Press', display: pixels },
    { id: 'regKY', name: 'Register K Y', type: 'standard', default: 0.5, group: 'Press', display: pixels },
    { id: 'wander', name: 'Press Wander', type: 'standard', default: 0.0, group: 'Press',
      display: (v) => `${wanderFromParam(v).toFixed(1)} px`,
      hint: 'A real press never holds register: paper stretches, cylinders warm, web tension breathes, and the fringes move from sheet to sheet. Bounded smooth noise per plate, and a pure function of time — so a frame rendered twice wanders identically.' },
    { id: 'wanderSpeed', name: 'Wander Speed', type: 'standard', default: 0.4, group: 'Press',
      display: (v) => `${wanderSpeedFromParam(v).toFixed(2)} Hz`,
      hint: 'How often the wander changes direction. Does nothing while Press Wander is at zero.' },

    { id: 'inkCR', name: 'Ink C', type: 'colour', default: INK_C[0], group: 'Ink',
      hint: 'Ink C Red, Green and Blue in the plugin — a host folds three consecutive RED/GREEN/BLUE parameters into one swatch, and so does this panel. Inks are filters: this is what the cyan ink lets through, not what it emits.' },
    { id: 'inkCG', name: 'Ink C Green', type: 'colour', default: INK_C[1], group: 'Ink' },
    { id: 'inkCB', name: 'Ink C Blue', type: 'colour', default: INK_C[2], group: 'Ink' },
    { id: 'inkMR', name: 'Ink M', type: 'colour', default: INK_M[0], group: 'Ink' },
    { id: 'inkMG', name: 'Ink M Green', type: 'colour', default: INK_M[1], group: 'Ink' },
    { id: 'inkMB', name: 'Ink M Blue', type: 'colour', default: INK_M[2], group: 'Ink' },
    { id: 'inkYR', name: 'Ink Y', type: 'colour', default: INK_Y[0], group: 'Ink' },
    { id: 'inkYG', name: 'Ink Y Green', type: 'colour', default: INK_Y[1], group: 'Ink' },
    { id: 'inkYB', name: 'Ink Y Blue', type: 'colour', default: INK_Y[2], group: 'Ink' },
    { id: 'inkKR', name: 'Ink K', type: 'colour', default: INK_K[0], group: 'Ink',
      hint: 'A rich near-black rather than 0,0,0: a solid of real black ink reflects a few per cent, and that is what makes shadow detail survive on a print.' },
    { id: 'inkKG', name: 'Ink K Green', type: 'colour', default: INK_K[1], group: 'Ink' },
    { id: 'inkKB', name: 'Ink K Blue', type: 'colour', default: INK_K[2], group: 'Ink' },
    { id: 'paperR', name: 'Paper', type: 'colour', default: 1.0, group: 'Ink',
      hint: 'What the stock reflects before any ink. Everything is multiplied by it, so a warm paper warms the whole print rather than tinting the highlights alone.' },
    { id: 'paperG', name: 'Paper Green', type: 'colour', default: 1.0, group: 'Ink' },
    { id: 'paperB', name: 'Paper Blue', type: 'colour', default: 1.0, group: 'Ink' },
    { id: 'inkDensity', name: 'Ink Density', type: 'standard', default: 0.667, group: 'Ink',
      display: (v) => `${inkDensityFromParam(v).toFixed(2)}×`,
      hint: 'Scales every ink’s absorption. 1.00× is the ink as specified; below that the press is running thin, above it the ink is laid on heavy.' },
    { id: 'plateC', name: 'Plate C', type: 'boolean', default: 1, group: 'Ink',
      hint: 'Is this plate on the press at all. Turn three off and you have a one-colour job; turn yellow and black off and you have a two-colour Riso.' },
    { id: 'plateM', name: 'Plate M', type: 'boolean', default: 1, group: 'Ink' },
    { id: 'plateY', name: 'Plate Y', type: 'boolean', default: 1, group: 'Ink' },
    { id: 'plateK', name: 'Plate K', type: 'boolean', default: 1, group: 'Ink' },
    { id: 'solo', name: 'Solo', type: 'option', default: 0, group: 'Ink',
      elements: ['Off', 'Cyan', 'Magenta', 'Yellow', 'Black'],
      hint: 'One plate alone, so its lattice is readable. Solo wins over the plate switches — it would be no use if the plate happened to be off.' },

    { id: 'mix', name: 'Mix', type: 'standard', default: 1.0, group: 'Output' },
  ],

  sources: ['scene', 'ramp', 'bars', 'detail', 'grid', 'alpha'],

  // The six factory rows from source/Presets.h, column for column. A preset is
  // a printing PROCESS — a newspaper, a two-colour Riso, a silkscreen poster —
  // rather than a set of slider positions somebody liked.
  presets: {
    // The plugin's own defaults, named so they stay reachable. `rztest
    // --defaults` is the check that holds this row and the constructor
    // together; nothing on this page checks it.
    'Offset Litho': row({
      blackGen: 1.0, totalInk: 0.5, screen: 0.46, dotShape: 0, dotGain: 0.375, inkSpread: 0.1,
      angleC: 0.083333, angleM: 0.416667, angleY: 0.0, angleK: 0.25,
      wander: 0.0, wanderSpeed: 0.4,
      inkC: INK_C, inkM: INK_M, inkY: INK_Y, inkK: INK_K,
      paper: [1.0, 1.0, 1.0], inkDensity: 0.667, plates: [1, 1, 1, 1],
    }),
    // Coarse screen, thirty points of gain, grey stock, a low ink limit and a
    // thin ink.
    Newspaper: row({
      blackGen: 1.0, totalInk: 0.2, screen: 0.55, dotShape: 0, dotGain: 0.75, inkSpread: 0.25,
      angleC: 0.083333, angleM: 0.416667, angleY: 0.0, angleK: 0.25,
      wander: 0.05, wanderSpeed: 0.4,
      inkC: INK_C, inkM: INK_M, inkY: INK_Y, inkK: INK_K,
      paper: [0.84, 0.82, 0.78], inkDensity: 0.6, plates: [1, 1, 1, 1],
    }),
    // Two plates only — the cyan separation on a Riso blue, the magenta
    // separation on fluorescent pink — with no black and no yellow, which is
    // what makes it a Riso and not a litho.
    'Riso 2-Colour': row({
      blackGen: 0.0, totalInk: 1.0, screen: 0.6, dotShape: 0, dotGain: 0.5, inkSpread: 0.2,
      angleC: 0.25, angleM: 0.083333, angleY: 0.0, angleK: 0.25,
      wander: 0.3, wanderSpeed: 0.3,
      inkC: [0.00, 0.47, 0.75], inkM: [1.00, 0.28, 0.69], inkY: INK_Y, inkK: INK_K,
      paper: [0.96, 0.94, 0.88], inkDensity: 0.667, plates: [1, 1, 0, 0],
    }),
    // A poster: black at 45 degrees and one spot red at 0, square dots, a
    // coarse screen and a hard ink edge.
    Silkscreen: row({
      blackGen: 1.0, totalInk: 1.0, screen: 0.65, dotShape: 2, dotGain: 0.25, inkSpread: 0.05,
      angleC: 0.083333, angleM: 0.0, angleY: 0.0, angleK: 0.25,
      wander: 0.0, wanderSpeed: 0.4,
      inkC: INK_C, inkM: [0.85, 0.12, 0.16], inkY: INK_Y, inkK: INK_K,
      paper: [0.97, 0.95, 0.90], inkDensity: 0.667, plates: [0, 1, 0, 1],
    }),
    // The defaults with the magenta screen at 16 degrees, one off the cyan.
    Moire: row({
      blackGen: 1.0, totalInk: 0.5, screen: 0.46, dotShape: 0, dotGain: 0.375, inkSpread: 0.1,
      angleC: 0.083333, angleM: 0.088889, angleY: 0.0, angleK: 0.25,
      wander: 0.0, wanderSpeed: 0.4,
      inkC: INK_C, inkM: INK_M, inkY: INK_Y, inkK: INK_K,
      paper: [1.0, 1.0, 1.0], inkDensity: 0.667, plates: [1, 1, 1, 1],
    }),
    // The defaults with the press wandering eight pixels, so the fringes move.
    'Drifting Press': row({
      blackGen: 1.0, totalInk: 0.5, screen: 0.46, dotShape: 0, dotGain: 0.375, inkSpread: 0.1,
      angleC: 0.083333, angleM: 0.416667, angleY: 0.0, angleK: 0.25,
      wander: 0.4, wanderSpeed: 0.55,
      inkC: INK_C, inkM: INK_M, inkY: INK_Y, inkK: INK_K,
      paper: [1.0, 1.0, 1.0], inkDensity: 0.667, plates: [1, 1, 1, 1],
    }),
  },

  differences: [
    'The audio side is not here at all. The plugin can let a kick drum throw the plates out of register and a loud passage shake the press, through a spectrum Resolume hands it as an FFT parameter; a browser has no equivalent, and asking for a microphone to demonstrate a video effect is not a trade worth making. Audio Drive and the FFT buffer are therefore absent rather than present and dead — and the removal is exact, because with Audio Drive at zero the plugin computes those terms as zero too. Press Wander is here and is the same code: it is a function of time alone.',
    'The Preset dropdown is the panel’s, not the plugin’s parameter. In Resolume a preset is an override laid over the sliders at read time — the host does not consume value events, so the inspector keeps showing the old numbers — whereas here picking one writes the sliders. It also resets the registration offsets, Solo and Mix, which the plugin leaves to the operator on purpose. The six rows themselves are source/Presets.h, column for column.',
    'The threshold table — the ranking that makes a dot cover exactly its tone — is built in JavaScript at double precision from a port of Screen.cpp, where the plugin builds it in C++ at single. The quantiles agree to far better than the eight bits you are looking at, but it is a second implementation and only a reader is checking it.',
    'Start on Ramps and steps with Solo on Cyan and drag Screen. That staircase is the one clip where a dot’s area against its tone is readable by eye, and it is what Dot Gain is doing. Then set Solo back to Off and put Angle M within a degree of Angle C: the moire that appears is two real lattices beating, not a texture.',
    'The plugin’s numerical proof — the GLSL spot functions against their C++ originals over 264,196 points, printed area against the dot-gain curve to 1%, a screen angle recovered from the dots’ own centroids to 0.06 degrees, a registration offset to 0.05 px, and two solid inks overprinting bit-exactly — is an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});
