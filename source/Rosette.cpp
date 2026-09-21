#include "Rosette.h"

#include "Controls.h"
#include "Diag.h"
#include "Press.h"
#include "Screen.h"
#include "Separation.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace rosette;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Rosette >,// Create method
	"RZ01",                  // Plugin unique ID of maximum length 4.
	"SW Rosette",            // Plugin name
	2,                       // API major version number
	1,                       // API minor version number
	0,                       // Plugin major version number
	1,                       // Plugin minor version number
	FF_EFFECT,               // Plugin type
	"Offset litho: four halftone plates at their own screen angles, laid down by a press that never quite registers them. The rosettes, the moire, the colour fringes that wander, the dot gain and the overprint all fall out of the model rather than being drawn.\n\nSolo a plate to see its lattice. Start from a Preset, at the bottom.",
	"Rosette FFGL effect" );

static_assert( Rosette::PT_COUNT - Rosette::PT_ABOUT_FIRST == stoatworks::about::kParamCount,
               "the About block's size changed with the generated header" );

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kSoloNames[] = { "Off", "Cyan", "Magenta", "Yellow", "Black" };
constexpr int kSoloCount       = 5;

/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Seconds of host time a single frame may advance by. The host's clock
/// jumps when the composition is scrubbed or the machine sleeps; a clamped
/// delta keeps the audio envelopes from decaying to nothing in one step.
constexpr double kMaxFrameDelta = 0.25;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

int optionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}
} // namespace

Rosette::Rosette()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are. They
	// are also preset row 1, and rztest --defaults holds the two together.
	//---------------------------------------------------------------------
	params[ PT_BLACK_GEN ] = 1.0f;
	params[ PT_TOTAL_INK ] = 0.5f;//300%

	params[ PT_SCREEN ]     = 0.46f;//about 8 px per dot
	params[ PT_DOT_SHAPE ]  = static_cast< float >( DotShape::Round );
	params[ PT_DOT_GAIN ]   = 0.375f;//15 points at 50%
	params[ PT_INK_SPREAD ] = 0.1f;
	params[ PT_ANGLE_C ]    = 0.083333f;//15 degrees
	params[ PT_ANGLE_M ]    = 0.416667f;//75
	params[ PT_ANGLE_Y ]    = 0.0f;     //0
	params[ PT_ANGLE_K ]    = 0.25f;    //45

	for( FFUInt32 i = PT_REG_C_X; i <= PT_REG_K_Y; ++i )
		params[ i ] = 0.5f;//zero offset
	params[ PT_WANDER ]       = 0.0f;
	params[ PT_WANDER_SPEED ] = 0.4f;
	params[ PT_AUDIO_DRIVE ]  = 0.0f;

	for( int plate = 0; plate < kPlateCount; ++plate )
		for( int ch = 0; ch < 3; ++ch )
			params[ PT_INK_C_R + plate * 3 + ch ] = kDefaultInk[ plate ][ ch ];
	for( int ch = 0; ch < 3; ++ch )
		params[ PT_PAPER_R + ch ] = kDefaultPaper[ ch ];
	params[ PT_INK_DENSITY ] = 0.667f;//unity
	params[ PT_PLATE_C ] = params[ PT_PLATE_M ] = params[ PT_PLATE_Y ] = params[ PT_PLATE_K ] = 1.0f;
	params[ PT_SOLO ]        = 0.0f;

	params[ PT_MIX ]    = 1.0f;
	params[ PT_PRESET ] = 0.0f;//Custom: the sliders are the truth

	//---------------------------------------------------------------------
	// Declaration. Every ranged parameter is a plain 0..1 float, with the
	// conversions in Controls.cpp -- see the note there. Option lists are
	// declared in the order they are written: none is long enough to need
	// sorting, and Off / Custom belong at the top of theirs.
	//---------------------------------------------------------------------
	auto option = [ this ]( unsigned int id, const char* name, int count, auto nameOf ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), nameOf( i ), static_cast< float >( i ) );
	};
	auto colour = [ this ]( unsigned int first, const char* label ) {
		//Consecutive red/green/blue parameters are what a host needs to show
		//a swatch instead of three sliders.
		const std::string base = label;
		SetParamInfo( first + 0, ( base + " Red" ).c_str(), FF_TYPE_RED, params[ first + 0 ] );
		SetParamInfo( first + 1, ( base + " Green" ).c_str(), FF_TYPE_GREEN, params[ first + 1 ] );
		SetParamInfo( first + 2, ( base + " Blue" ).c_str(), FF_TYPE_BLUE, params[ first + 2 ] );
	};

	SetParamInfof( PT_BLACK_GEN, "Black Generation", FF_TYPE_STANDARD );
	SetParamInfof( PT_TOTAL_INK, "Total Ink", FF_TYPE_STANDARD );

	SetParamInfof( PT_SCREEN, "Screen", FF_TYPE_STANDARD );
	option( PT_DOT_SHAPE, "Dot Shape", static_cast< int >( DotShape::Count ),
	        []( int v ) { return DotShapeName( static_cast< DotShape >( v ) ); } );
	SetParamInfof( PT_DOT_GAIN, "Dot Gain", FF_TYPE_STANDARD );
	SetParamInfof( PT_INK_SPREAD, "Ink Spread", FF_TYPE_STANDARD );
	SetParamInfof( PT_ANGLE_C, "Angle C", FF_TYPE_STANDARD );
	SetParamInfof( PT_ANGLE_M, "Angle M", FF_TYPE_STANDARD );
	SetParamInfof( PT_ANGLE_Y, "Angle Y", FF_TYPE_STANDARD );
	SetParamInfof( PT_ANGLE_K, "Angle K", FF_TYPE_STANDARD );

	SetParamInfof( PT_REG_C_X, "Register C X", FF_TYPE_STANDARD );
	SetParamInfof( PT_REG_C_Y, "Register C Y", FF_TYPE_STANDARD );
	SetParamInfof( PT_REG_M_X, "Register M X", FF_TYPE_STANDARD );
	SetParamInfof( PT_REG_M_Y, "Register M Y", FF_TYPE_STANDARD );
	SetParamInfof( PT_REG_Y_X, "Register Y X", FF_TYPE_STANDARD );
	SetParamInfof( PT_REG_Y_Y, "Register Y Y", FF_TYPE_STANDARD );
	SetParamInfof( PT_REG_K_X, "Register K X", FF_TYPE_STANDARD );
	SetParamInfof( PT_REG_K_Y, "Register K Y", FF_TYPE_STANDARD );
	SetParamInfof( PT_WANDER, "Press Wander", FF_TYPE_STANDARD );
	SetParamInfof( PT_WANDER_SPEED, "Wander Speed", FF_TYPE_STANDARD );

	// An FFT buffer: Resolume shows it as an audio-source picker and writes
	// one spectrum bin per element. Element defaults are zero on purpose --
	// with no audio routed, Audio Drive does nothing rather than the plates
	// twitching to a phantom signal.
	SetBufferParamInfo( PT_AUDIO, "Audio", audio::kBins, FF_USAGE_FFT );
	for( int i = 0; i < audio::kBins; ++i )
		SetParamElementInfo( PT_AUDIO, i, "", 0.0f );
	SetParamInfof( PT_AUDIO_DRIVE, "Audio Drive", FF_TYPE_STANDARD );

	colour( PT_INK_C_R, "Ink C" );
	colour( PT_INK_M_R, "Ink M" );
	colour( PT_INK_Y_R, "Ink Y" );
	colour( PT_INK_K_R, "Ink K" );
	colour( PT_PAPER_R, "Paper" );
	SetParamInfof( PT_INK_DENSITY, "Ink Density", FF_TYPE_STANDARD );
	SetParamInfo( PT_PLATE_C, "Plate C", FF_TYPE_BOOLEAN, true );
	SetParamInfo( PT_PLATE_M, "Plate M", FF_TYPE_BOOLEAN, true );
	SetParamInfo( PT_PLATE_Y, "Plate Y", FF_TYPE_BOOLEAN, true );
	SetParamInfo( PT_PLATE_K, "Plate K", FF_TYPE_BOOLEAN, true );
	option( PT_SOLO, "Solo", kSoloCount, []( int v ) { return kSoloNames[ v ]; } );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	// Factory presets. Element 0 is Custom; the rest are laid over the
	// controls at read time. See Presets.h.
	option( PT_PRESET, "Preset", 1 + presets::kCount,
	        []( int v ) { return v == 0 ? "Custom" : presets::kPresets[ v - 1 ].name; } );

	// Groups, the way Resolume shows them. SetParamGroup collapses runs of
	// consecutive same-group ids, so each group is one contiguous run.
	for( FFUInt32 i = PT_BLACK_GEN; i <= PT_TOTAL_INK; ++i )
		SetParamGroup( i, "Separation" );
	for( FFUInt32 i = PT_SCREEN; i <= PT_ANGLE_K; ++i )
		SetParamGroup( i, "Screen" );
	for( FFUInt32 i = PT_REG_C_X; i <= PT_AUDIO_DRIVE; ++i )
		SetParamGroup( i, "Press" );
	for( FFUInt32 i = PT_INK_C_R; i <= PT_SOLO; ++i )
		SetParamGroup( i, "Ink" );
	SetParamGroup( PT_MIX, "Output" );
	SetParamGroup( PT_PRESET, "Preset" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Rosette effect" );

	diag::init();
}

//---------------------------------------------------------------------------
bool Rosette::UploadThresholds()
{
	const std::vector< float > table = BuildThresholdTable();

	glGenTextures( 1, &thresholdTexture );
	if( thresholdTexture == 0 )
		return false;

	Scoped2DTextureBinding binding( thresholdTexture );

	glTexImage2D( GL_TEXTURE_2D, 0, GL_R32F, kThresholdSize, static_cast< GLsizei >( DotShape::Count ), 0,
	              GL_RED, GL_FLOAT, table.data() );

	//Linear along the tone axis, so a tone between two entries gets a
	//threshold between two entries. The shape axis is sampled at texel
	//centres and never between them.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	return true;
}

//---------------------------------------------------------------------------
FFResult Rosette::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	//Held in a local so the pointer handed to Compile outlives the call.
	const std::string printSource = PrintShaderSource();

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &separateShader, kSeparateShader, "separate" },
		{ &printShader, printSource.c_str(), "print" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( kVertexShader, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume. These two lines are the only record
		//of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Rosette: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !UploadThresholds() )
	{
		diag::error( "could not upload the threshold table" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
void Rosette::UpdateClock()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;

	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		// A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	// Until the unit is settled -- and for a host that never calls SetTime --
	// run on the real clock: wrong in origin but right in rate.
	now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( raw )
		            + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );
}

void Rosette::UpdateAudio()
{
	const double dt = ( lastNow >= 0.0 && now > lastNow ) ? std::min( now - lastNow, kMaxFrameDelta ) : 0.0;

	float bins[ audio::kBins ] = {};
	int binCount              = 0;
	if( const ParamInfo* info = FindParamInfo( PT_AUDIO ) )
	{
		binCount = static_cast< int >( std::min< size_t >( info->elements.size(), audio::kBins ) );
		for( int i = 0; i < binCount; ++i )
			bins[ i ] = info->elements[ static_cast< size_t >( i ) ].value;
	}

	analyser.Update( bins, binCount, static_cast< float >( dt ), audio::Settings {} );

	// A new direction for every plate on every onset, so two kicks in a row
	// do not throw the press the same way twice.
	if( analyser.Fired() )
	{
		++kickSalt;
		for( int plate = 0; plate < kPlateCount; ++plate )
		{
			const press::Offset dir     = press::KickDirection( plate, kickSalt );
			kickDirection[ plate ][ 0 ] = dir.x;
			kickDirection[ plate ][ 1 ] = dir.y;
		}
	}
}

//---------------------------------------------------------------------------
FFResult Rosette::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	const int pictureWidth  = static_cast< int >( picture.Width );
	const int pictureHeight = static_cast< int >( picture.Height );

	//The host's viewport, read before anything of ours changes it.
	//ScopedFBOBinding restores the framebuffer binding and only that; the
	//separate pass's ResizeViewPort would otherwise leak into the print
	//pass, which draws to the host's own framebuffer.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	UpdateClock();
	UpdateAudio();

	//---------------------------------------------------------------------
	// The press. Where each plate sits this frame: the operator's
	// registration, plus the wander, plus whatever the audio is doing.
	//---------------------------------------------------------------------
	const float wander      = WanderFromParam( Effective( PT_WANDER ) );
	const float wanderSpeed = WanderSpeedFromParam( Effective( PT_WANDER_SPEED ) );
	const float drive       = AudioDriveFromParam( params[ PT_AUDIO_DRIVE ] );
	const float shake       = drive * 0.35f * analyser.Level();
	const float kick        = drive * analyser.Kick();

	for( int plate = 0; plate < kPlateCount; ++plate )
	{
		const press::Offset w = press::Wander( plate, now, wander, wanderSpeed );
		//The continuous shake wanders too, on a faster lane, so the level
		//jitters the press rather than displacing it one way.
		const press::Offset j = press::Wander( plate + 8, now, shake, 6.0f );

		plateOffset[ plate ][ 0 ] = RegisterFromParam( params[ PT_REG_C_X + plate * 2 ] ) + w.x + j.x + kick * kickDirection[ plate ][ 0 ];
		plateOffset[ plate ][ 1 ] = RegisterFromParam( params[ PT_REG_C_Y + plate * 2 ] ) + w.y + j.y + kick * kickDirection[ plate ][ 1 ];
	}

	lastNow = now;

	//---------------------------------------------------------------------
	// Buffers. Every Ensure() happens before anything binds a texture:
	// FFGLFBO::Initialise sizes its colour texture under a scoped binding
	// that clears to 0 on exit, so allocating mid-pass would unbind the
	// input from the active unit for that one frame.
	//---------------------------------------------------------------------
	if( !platesBuffer.Ensure( pictureWidth, pictureHeight, GL_RGBA16F, PassBuffer::Sampling::Mipmapped ) )
	{
		diag::error( "could not allocate the plates buffer" );
		return FF_FAIL;
	}

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );
	const float halfTexelX        = 0.5f / static_cast< float >( pictureWidth );
	const float halfTexelY        = 0.5f / static_cast< float >( pictureHeight );

	//---------------------------------------------------------------------
	// 1. Separate.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( platesBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		platesBuffer.ResizeViewPort();
		ScopedShaderBinding shader( separateShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( picture.Handle );

		separateShader.Set( "InputTexture", 0 );
		separateShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		separateShader.Set( "HalfTexel", halfTexelX, halfTexelY );
		separateShader.Set( "BlackGeneration", BlackGenerationFromParam( Effective( PT_BLACK_GEN ) ) );
		separateShader.Set( "TotalInk", TotalInkFromParam( Effective( PT_TOTAL_INK ) ) );
		quad.Draw();
	}
	platesBuffer.GenerateMipmaps();

	//---------------------------------------------------------------------
	// 2. Print, straight to the host's framebuffer.
	//---------------------------------------------------------------------
	{
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( printShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding inputBinding( picture.Handle );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding platesBinding( platesBuffer.TextureID() );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding thresholdBinding( thresholdTexture );

		printShader.Set( "InputTexture", 0 );
		printShader.Set( "PlatesTexture", 1 );
		printShader.Set( "ThresholdTexture", 2 );

		printShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		printShader.Set( "Size", static_cast< float >( pictureWidth ), static_cast< float >( pictureHeight ) );
		printShader.Set( "HalfTexel", halfTexelX, halfTexelY );
		printShader.Set( "ShapeCount", static_cast< float >( DotShape::Count ) );

		const float screenPx = ScreenPxFromParam( Effective( PT_SCREEN ) );
		printShader.Set( "ScreenPx", screenPx );
		//The mip level whose texel is about one cell: the tone of a cell is
		//the picture's mean over it, and the chain has already computed that.
		printShader.Set( "PlateLod", std::clamp( std::log2( screenPx ), 0.0f, platesBuffer.MaxMipLevel() ) );
		printShader.Set( "DotShape", static_cast< float >( optionIndex( Effective( PT_DOT_SHAPE ), static_cast< int >( DotShape::Count ) ) ) );
		printShader.Set( "DotGain", DotGainFromParam( Effective( PT_DOT_GAIN ) ) );
		printShader.Set( "InkSpread", InkSpreadFromParam( Effective( PT_INK_SPREAD ) ) );

		printShader.Set( "Angles",
		                 AngleFromParam( Effective( PT_ANGLE_C ) ), AngleFromParam( Effective( PT_ANGLE_M ) ),
		                 AngleFromParam( Effective( PT_ANGLE_Y ) ), AngleFromParam( Effective( PT_ANGLE_K ) ) );

		//FFGLShader::Set has no array overload, so the arrays go up raw.
		glUniform2fv( printShader.FindUniform( "Offsets" ), kPlateCount, &plateOffset[ 0 ][ 0 ] );

		//Solo wins over the plate switches: it is "show me this lattice",
		//and it would be no use if the plate happened to be off.
		const int solo = optionIndex( params[ PT_SOLO ], kSoloCount );
		float on[ kPlateCount ];
		for( int plate = 0; plate < kPlateCount; ++plate )
			on[ plate ] = solo > 0 ? ( solo - 1 == plate ? 1.0f : 0.0f )
			                       : ( Effective( PT_PLATE_C + plate ) > 0.5f ? 1.0f : 0.0f );
		printShader.Set( "PlateOn", on[ 0 ], on[ 1 ], on[ 2 ], on[ 3 ] );

		float inks[ kPlateCount ][ 3 ];
		for( int plate = 0; plate < kPlateCount; ++plate )
			for( int ch = 0; ch < 3; ++ch )
				inks[ plate ][ ch ] = Effective( PT_INK_C_R + plate * 3 + ch );
		glUniform3fv( printShader.FindUniform( "InkColour" ), kPlateCount, &inks[ 0 ][ 0 ] );
		printShader.Set( "Paper", Effective( PT_PAPER_R ), Effective( PT_PAPER_G ), Effective( PT_PAPER_B ) );
		printShader.Set( "InkDensity", InkDensityFromParam( Effective( PT_INK_DENSITY ) ) );
		printShader.Set( "MixAmount", params[ PT_MIX ] );

		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Rosette::DeInitGL()
{
	separateShader.FreeGLResources();
	printShader.FreeGLResources();
	quad.Release();
	platesBuffer.Destroy();

	if( thresholdTexture != 0 )
	{
		glDeleteTextures( 1, &thresholdTexture );
		thresholdTexture = 0;
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
bool Rosette::presetCovers( unsigned int index ) const
{
	for( unsigned int id : kPresetParamIDs )
		if( id == index )
			return true;
	return false;
}

void Rosette::seedHostValues()
{
	// Seeded on first parameter traffic, BEFORE any preset can be chosen:
	// seeding afterwards would record the operator's pre-preset values as
	// something other than the host's opening position.
	if( hostValuesSeeded )
		return;

	for( unsigned int i = 0; i < PT_COUNT; ++i )
		hostValues[ i ] = params[ i ];
	hostValuesSeeded = true;
}

bool Rosette::hostIsRestatingItself( unsigned int index, float value )
{
	const float lastFromHost = hostValues[ index ];
	hostValues[ index ]      = value;

	// A quantisation allowance rather than a float epsilon. A host that keeps
	// its parameters shorter than a float -- or round-trips them through a
	// UI, a MIDI value or a saved composition -- hands back a number near
	// ours rather than ours.
	constexpr float kSame = 1e-3f;
	return std::fabs( value - lastFromHost ) <= kSame;
}

FFResult Rosette::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	seedHostValues();

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_PRESET )
	{
		params[ PT_PRESET ] = value;
		return FF_SUCCESS;
	}

	const int active = optionIndex( params[ PT_PRESET ], 1 + presets::kCount );
	if( active > 0 && presetCovers( index ) )
	{
		// While a preset is laid over this control the host keeps restating
		// the value it believes in. That is not an edit and must not un-set
		// the preset; a value from neither the host's last word nor the
		// preset is the operator taking over.
		if( hostIsRestatingItself( index, value ) )
			return FF_SUCCESS;

		diag::info( "preset dropped to Custom: parameter " + std::to_string( index )
		            + " moved to " + std::to_string( value ) );
		params[ PT_PRESET ] = 0.0f;
		RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
	}
	else
	{
		hostValues[ index ] = value;
	}

	params[ index ] = value;
	return FF_SUCCESS;
}

float Rosette::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

float Rosette::Effective( unsigned int index ) const
{
	const int active = optionIndex( params[ PT_PRESET ], 1 + presets::kCount );
	if( active > 0 )
	{
		const presets::Preset& row = presets::kPresets[ active - 1 ];
		for( int c = 0; c < presets::kParamCount; ++c )
			if( kPresetParamIDs[ c ] == index )
				return row.v[ c ];
	}
	return index < PT_COUNT ? params[ index ] : 0.0f;
}

const unsigned int* Rosette::PresetParamIDsForTest( int& count )
{
	count = presets::kParamCount;
	return kPresetParamIDs;
}

void Rosette::PlateOffsetsForTest( float out[ 4 ][ 2 ] ) const
{
	for( int plate = 0; plate < kPlateCount; ++plate )
	{
		out[ plate ][ 0 ] = plateOffset[ plate ][ 0 ];
		out[ plate ][ 1 ] = plateOffset[ plate ][ 1 ];
	}
}

//---------------------------------------------------------------------------
char* Rosette::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Rosette::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Rosette::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void Rosette::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}
