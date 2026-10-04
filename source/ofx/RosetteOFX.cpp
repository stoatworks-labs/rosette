/// The OpenFX build of Rosette, for DaVinci Resolve, Nuke, Natron, Vegas and
/// other OFX hosts.
///
/// ------------------------------------------------------- what is shared
///
/// **Everything that is not a pixel.** The separation, the dot-gain curve,
/// the ink model, the spot functions and their threshold table, the press
/// wander, the parameter curves and the preset table are the same C++ the
/// FFGL build runs and `rztest` measures, linked straight in from
/// `rosette_dsp`. The step from controls to what the shaders are told is
/// `print::Configure`, which the FFGL plugin calls too.
///
/// What the GPU did per fragment -- the separate pass, the mip chain, the
/// four lattices and the print -- is `Print.cpp`, mirrored line for line from
/// `Shaders.cpp` and held against it by `rztest --cpu`. This file only
/// marshals: OFX's pixel formats in, premultiplied float through the print
/// pass, and back out.
///
/// ------------------------------------------------------ what is missing
///
/// **The audio side, entirely.** OFX has no spectrum to offer, and a timeline
/// renders frames in whatever order it likes, so an envelope follower and an
/// onset detector have nothing to follow. The Audio buffer and Audio Drive are
/// not declared here rather than declared and dead, and the plugin
/// description says so. No preset touches either, so every preset means the
/// same press in both builds.
///
/// ----------------------------------------------- what is the same, by design
///
/// **The press wander needs no history.** It is bounded noise and a pure
/// function of time (Press.h), so a frame rendered alone, out of order or
/// twice wanders exactly as it does in sequence. OFX time is in frames;
/// seconds are frames over the clip's frame rate.
///
/// **Presets are written, not laid over.** The FFGL build cannot push values
/// into Resolume's inspector, so there a preset overrides the controls at read
/// time. OFX can, so here a preset sets its controls -- one undo step -- and
/// an edit that leaves a covered control different from the preset's value
/// drops the menu back to Custom, judged by value, as every fleet port does.
///
/// ------------------------------------------------------------- and tiles
///
/// The tone of a cell is read from a mip chain built over the whole frame,
/// so there is no tile that could be rendered alone. `setSupportsTiles(
/// false )` is a statement of fact about the effect.

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Presets.h"
#include "../Print.h"
#include "../Screen.h"
#include "../Separation.h"

namespace
{
using namespace rosette;

constexpr const char* kPluginIdentifier = "com.stoatworks.rosette";
constexpr const char* kPluginName       = "Rosette";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"Offset litho: four halftone plates at their own screen angles, laid down "
	"by a press that never quite registers them.\n\n"
	"The picture is separated into cyan, magenta, yellow and black, and each "
	"plate is screened as its own lattice of dots. The rosettes, the moire, "
	"the colour fringes that wander, the dot gain and the overprint -- cyan "
	"over yellow is green -- all fall out of the model rather than being "
	"drawn. Solo a plate to see its lattice; start from a Preset.\n\n"
	"The press wander is a pure function of time, so any frame renders on its "
	"own and scrubbing shows the press at that moment. Fusion reports no frame "
	"rate; there, time-based controls assume 24 fps.\n\n"
	"The Resolume build of this effect is also audio-reactive (Audio Drive "
	"shakes the plates and throws them on a beat). OpenFX has no audio to "
	"offer a plugin, so that control is absent here rather than present and "
	"doing nothing.\n\n"
	"https://stoatworks-labs.com";

//The script names. Permanent: saved projects refer to them.
constexpr const char* kParamPreset          = "preset";
constexpr const char* kParamBlackGeneration = "blackGeneration";
constexpr const char* kParamTotalInk        = "totalInk";
constexpr const char* kParamScreen          = "screen";
constexpr const char* kParamDotShape        = "dotShape";
constexpr const char* kParamDotGain         = "dotGain";
constexpr const char* kParamInkSpread       = "inkSpread";
constexpr const char* kParamAngle[ 4 ]      = { "angleC", "angleM", "angleY", "angleK" };
constexpr const char* kParamRegister[ 4 ][ 2 ] = {
	{ "registerCX", "registerCY" },
	{ "registerMX", "registerMY" },
	{ "registerYX", "registerYY" },
	{ "registerKX", "registerKY" },
};
constexpr const char* kParamWander      = "pressWander";
constexpr const char* kParamWanderSpeed = "wanderSpeed";
constexpr const char* kParamInk[ 4 ]    = { "inkC", "inkM", "inkY", "inkK" };
constexpr const char* kParamPaper       = "paper";
constexpr const char* kParamInkDensity  = "inkDensity";
constexpr const char* kParamPlate[ 4 ]  = { "plateC", "plateM", "plateY", "plateK" };
constexpr const char* kParamSolo        = "solo";
constexpr const char* kParamMix         = "mix";

constexpr const char* kPlateLetter[ 4 ] = { "C", "M", "Y", "K" };
constexpr const char* kSoloNames[]      = { "Off", "Cyan", "Magenta", "Yellow", "Black" };
constexpr int kSoloCount                = 5;

/// The threshold table: built once per process, on first use, and never
/// freed. A function-local static OBJECT would register an exit-time
/// destructor inside this module, and a host that unloads the bundle before
/// exit then runs it through a dangling pointer -- the teardown trap the
/// factory below avoids the same way. Initialisation of a local static is
/// thread-safe, and renders do arrive on several threads at once.
const float* thresholdTable()
{
	static const std::vector< float >* table = new std::vector< float >( BuildThresholdTable() );
	return table->data();
}

//---------------------------------------------------------------------------
// What the host may not say.
//
// DaVinci Resolve's Fusion page provides NO frame rate -- not on the effect,
// not on any clip -- and the Support library turns a property the host does
// not know into OFX::Exception::PropertyUnknownToHost. Uncaught, that leaves
// render() as kOfxStatErrMissingHostFeature and Resolve fails the frame: every
// frame, because the press wander needs seconds. Its Edit page does provide
// one. So every host property this plugin reads that is not guaranteed is read
// here, each inside its own try, with a stated fallback.
//---------------------------------------------------------------------------

/// What Fusion is assumed to run at: Resolve's default timeline rate. Only
/// the wander's speed depends on it.
constexpr double kFallbackFramesPerSecond = 24.0;

/// The first positive, finite frame rate of: the output clip, the source
/// clip, the effect. Otherwise kFallbackFramesPerSecond.
double framesPerSecond( const OFX::ImageEffect& effect, const OFX::Clip* output, const OFX::Clip* source )
{
	const auto usable = []( double fps ) { return std::isfinite( fps ) && fps > 0.0; };

	try
	{
		if( output != nullptr && usable( output->getFrameRate() ) )
			return output->getFrameRate();
	}
	catch( ... )
	{
	}
	try
	{
		if( source != nullptr && usable( source->getFrameRate() ) )
			return source->getFrameRate();
	}
	catch( ... )
	{
	}
	try
	{
		if( usable( effect.getFrameRate() ) )
			return effect.getFrameRate();
	}
	catch( ... )
	{
	}
	return kFallbackFramesPerSecond;
}

/// Whether the source is premultiplied. An RGB clip has no alpha to be
/// premultiplied by, and a host that says "unpremultiplied" about one is
/// describing something that does not exist; treating it as premultiplied
/// makes the round trip an identity. A host that will not say is taken to
/// mean premultiplied, which is what every host this was tried in does say.
bool sourceIsPremultiplied( const OFX::Clip* source, OFX::PixelComponentEnum components )
{
	if( components != OFX::ePixelComponentRGBA )
		return true;
	try
	{
		return source->getPreMultiplication() != OFX::eImageUnPreMultiplied;
	}
	catch( ... )
	{
		return true;
	}
}

/// The source image's pixel aspect, or 1 if the host does not give a usable
/// one.
float pixelAspectOf( const OFX::Image* image )
{
	try
	{
		const double par = image->getPixelAspectRatio();
		if( std::isfinite( par ) && par > 0.0 )
			return static_cast< float >( par );
	}
	catch( ... )
	{
	}
	return 1.0f;
}

//---------------------------------------------------------------------------
// Marshalling. One pixel in as premultiplied float, one pixel out from it.
// GL hands the FFGL build each 8-bit channel as value / 255, and this divides
// rather than multiplying by a reciprocal so the two builds start from the
// same number.
//---------------------------------------------------------------------------
template< class PIX, int nComponents, int maxValue >
inline void readPixel( const OFX::Image* image, int x, int y, bool premultiplied, float out[ 4 ] )
{
	const PIX* pixel = static_cast< const PIX* >( image->getPixelAddress( x, y ) );
	if( pixel == nullptr )
	{
		out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0f;
		return;
	}

	const float scale = static_cast< float >( maxValue );
	const float alpha = nComponents == 4 ? static_cast< float >( pixel[ 3 ] ) / scale : 1.0f;
	for( int c = 0; c < 3; ++c )
	{
		const float v = static_cast< float >( pixel[ c ] ) / scale;
		//The print pass works premultiplied, as Resolume hands it over. A clip
		//the host says is straight is multiplied up on the way in.
		out[ c ] = premultiplied ? v : v * alpha;
	}
	out[ 3 ] = alpha;
}

/// Integer formats clamp and round, as a GL framebuffer does; float is left
/// alone. The rounding is of the exact product, which is why it is done in
/// double: 0.9f * 255.0f rounds UP to exactly 229.5 in single precision, and
/// a paper of 0.9 would come out one step brighter than the GPU writes it.
template< class PIX, int maxValue >
inline PIX quantise( float v )
{
	if( maxValue == 1 )
		return static_cast< PIX >( v );
	return static_cast< PIX >( std::lround( static_cast< double >( std::clamp( v, 0.0f, 1.0f ) ) * static_cast< double >( maxValue ) ) );
}

template< class PIX, int nComponents, int maxValue >
inline void writePixel( PIX* pixel, const float in[ 4 ], bool premultiplied )
{
	const float alpha = in[ 3 ];
	for( int c = 0; c < 3; ++c )
	{
		float v = in[ c ];
		if( !premultiplied )
			v = alpha > 0.0f ? v / alpha : 0.0f;

		pixel[ c ] = quantise< PIX, maxValue >( v );
	}

	if( nComponents == 4 )
		pixel[ 3 ] = quantise< PIX, maxValue >( alpha );
}

//---------------------------------------------------------------------------
// Pass 1: separate the whole picture into the plates buffer, a band of rows
// per thread. Level 0 only; the mip chain is built after.
//---------------------------------------------------------------------------
class SeparateProcessorBase : public OFX::MultiThread::Processor
{
public:
	SeparateProcessorBase( const OFX::Image* source, const OfxRectI& picture, const print::Settings& settings,
	                       bool premultiplied, print::Plates& plates ) :
		src( source ),
		bounds( picture ),
		set( settings ),
		premult( premultiplied ),
		out( plates )
	{
	}

protected:
	const OFX::Image* src;
	OfxRectI bounds;
	const print::Settings& set;
	bool premult;
	print::Plates& out;
};

template< class PIX, int nComponents, int maxValue >
class SeparateProcessor : public SeparateProcessorBase
{
public:
	using SeparateProcessorBase::SeparateProcessorBase;

	void multiThreadFunction( unsigned int threadId, unsigned int nThreads ) override
	{
		print::Plates::Level& level = out.levels[ 0 ];
		const int rows              = level.height;
		const int first             = static_cast< int >( static_cast< long long >( rows ) * threadId / nThreads );
		const int last              = static_cast< int >( static_cast< long long >( rows ) * ( threadId + 1 ) / nThreads );

		for( int y = first; y < last; ++y )
		{
			float* row = level.texels.data() + static_cast< size_t >( y ) * static_cast< size_t >( level.width ) * 4;
			for( int x = 0; x < level.width; ++x )
			{
				float rgba[ 4 ];
				readPixel< PIX, nComponents, maxValue >( src, bounds.x1 + x, bounds.y1 + y, premult, rgba );
				print::SeparateTexel( rgba, set, row + static_cast< size_t >( x ) * 4 );
			}
		}
	}
};

//---------------------------------------------------------------------------
// Pass 2: print, over the render window, a band of rows per thread.
//---------------------------------------------------------------------------
class PrintProcessorBase : public OFX::ImageProcessor
{
public:
	explicit PrintProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setup( const OFX::Image* source, const OfxRectI& picture, const print::Settings* settings,
	            const print::Plates* platesBuffer, bool premultiplied )
	{
		src     = source;
		bounds  = picture;
		set     = settings;
		plates  = platesBuffer;
		premult = premultiplied;
	}

protected:
	const OFX::Image* src         = nullptr;
	OfxRectI bounds               = { 0, 0, 0, 0 };
	const print::Settings* set    = nullptr;
	const print::Plates* plates   = nullptr;
	bool premult                  = true;
};

template< class PIX, int nComponents, int maxValue >
class PrintProcessor : public PrintProcessorBase
{
public:
	using PrintProcessorBase::PrintProcessorBase;

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const float* thresholds = thresholdTable();

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			for( int x = window.x1; x < window.x2; ++x )
			{
				PIX* dst = static_cast< PIX* >( _dstImg->getPixelAddress( x, y ) );
				if( dst == nullptr )
					continue;

				//Outside the picture there is nothing to print on.
				float printed[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
				if( x >= bounds.x1 && x < bounds.x2 && y >= bounds.y1 && y < bounds.y2 )
				{
					float source[ 4 ];
					readPixel< PIX, nComponents, maxValue >( src, x, y, premult, source );
					print::PrintPixel( *set, *plates, thresholds, x - bounds.x1, y - bounds.y1, source, printed );
				}
				writePixel< PIX, nComponents, maxValue >( dst, printed, premult );
			}
		}
	}
};

//---------------------------------------------------------------------------
class RosettePlugin : public OFX::ImageEffect
{
public:
	explicit RosettePlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		preset          = fetchChoiceParam( kParamPreset );
		blackGeneration = fetchDoubleParam( kParamBlackGeneration );
		totalInk        = fetchDoubleParam( kParamTotalInk );
		screen          = fetchDoubleParam( kParamScreen );
		dotShape        = fetchChoiceParam( kParamDotShape );
		dotGain         = fetchDoubleParam( kParamDotGain );
		inkSpread       = fetchDoubleParam( kParamInkSpread );
		for( int plate = 0; plate < kPlateCount; ++plate )
		{
			angle[ plate ]          = fetchDoubleParam( kParamAngle[ plate ] );
			registration[ plate ][ 0 ] = fetchDoubleParam( kParamRegister[ plate ][ 0 ] );
			registration[ plate ][ 1 ] = fetchDoubleParam( kParamRegister[ plate ][ 1 ] );
			ink[ plate ]            = fetchRGBParam( kParamInk[ plate ] );
			plateOn[ plate ]        = fetchBooleanParam( kParamPlate[ plate ] );
		}
		wander      = fetchDoubleParam( kParamWander );
		wanderSpeed = fetchDoubleParam( kParamWanderSpeed );
		paper       = fetchRGBParam( kParamPaper );
		inkDensity  = fetchDoubleParam( kParamInkDensity );
		solo        = fetchChoiceParam( kParamSolo );
		mix         = fetchDoubleParam( kParamMix );
	}

	/// Press Wander moves the plates with the frame's time. Unless a plugin says
	/// so, a host may treat its output as fixed while its inputs and parameters
	/// are, and Resolve's Fusion page does: it rendered every fleet generator
	/// once and repeated that frame. A preference, not a requirement: a host that
	/// does not know the property is left to its own default rather than failing
	/// the effect.
	void getClipPreferences( OFX::ClipPreferencesSetter& preferences ) override
	{
		try
		{
			preferences.setOutputFrameVarying( true );
		}
		catch( ... )
		{
		}
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src( srcClip->fetchImage( args.time ) );

		if( dst == nullptr || src == nullptr )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		if( src->getPixelDepth() != depth || src->getPixelComponents() != comps )
			OFX::throwSuiteStatusException( kOfxStatErrImageFormat );

		const OfxRectI bounds = src->getBounds();
		const int width       = bounds.x2 - bounds.x1;
		const int height      = bounds.y2 - bounds.y1;
		if( width <= 0 || height <= 0 )
			return;

		const bool premultiplied = sourceIsPremultiplied( srcClip, comps );

		//OFX time is FRAMES. Seconds are frames over the frame rate, which
		//Fusion does not report at all -- see framesPerSecond().
		const double fps = framesPerSecond( *this, dstClip, srcClip );

		//A proxy render at half size gets a screen and a press half as many
		//pixels across, so it looks like the full render. An anamorphic clip
		//keeps its dots round.
		const float scale = std::isfinite( args.renderScale.x ) && args.renderScale.x > 0.0
		                        ? static_cast< float >( args.renderScale.x )
		                        : 1.0f;
		const float par = pixelAspectOf( src.get() );

		const print::Settings settings = print::Configure( controlsAt( args.time ), args.time / fps, width, height, scale, par );

		//The plates buffer lives for this render only: renders arrive alone,
		//out of order and on several threads at once, and nothing about one
		//frame may be left for the next to find.
		print::Plates plates;
		plates.Allocate( width, height );

		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? run< unsigned char, 4, 255 >( args, dst.get(), src.get(), bounds, settings, plates, premultiplied )
				: run< unsigned char, 3, 255 >( args, dst.get(), src.get(), bounds, settings, plates, premultiplied );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? run< unsigned short, 4, 65535 >( args, dst.get(), src.get(), bounds, settings, plates, premultiplied )
				: run< unsigned short, 3, 65535 >( args, dst.get(), src.get(), bounds, settings, plates, premultiplied );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? run< float, 4, 1 >( args, dst.get(), src.get(), bounds, settings, plates, premultiplied )
				: run< float, 3, 1 >( args, dst.get(), src.get(), bounds, settings, plates, premultiplied );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	bool isIdentity( const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip, double& identityTime ) override
	{
		//Mix at zero is the input, exactly: the shader's last line is
		//mix( source, result, MixAmount ).
		if( mix->getValueAtTime( args.time ) <= 0.0 )
		{
			identityClip = srcClip;
			identityTime = args.time;
			return true;
		}
		return false;
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		if( stoatworks::about::ofx::changedParam( args, paramName ) )
			return;

		if( paramName == kParamPreset )
		{
			int chosen = 0;
			preset->getValue( chosen );
			if( chosen <= 0 || chosen > presets::kCount || applyingPreset )
				return;//Custom: the controls keep whatever they said

			//The copy IS the preset: the same table the FFGL build reads, in
			//the same 0..1 space. One edit block, so one undo takes the whole
			//preset back.
			const presets::Preset& row = presets::kPresets[ chosen - 1 ];
			applyingPreset             = true;
			beginEditBlock( "Preset" );
			applyPreset( row );
			endEditBlock();
			applyingPreset = false;
			return;
		}

		//Editing a covered control while a preset is active hands control back
		//to the controls. Judged by value, not by the change reason: hosts are
		//not consistent about reasons, but "still equal to the preset" is
		//unambiguous and also absorbs a host echoing our own writes late.
		if( applyingPreset || args.reason == OFX::eChangeTime )
			return;

		int active = 0;
		preset->getValue( active );
		if( active <= 0 || active > presets::kCount )
			return;

		if( coveredAndMoved( paramName, presets::kPresets[ active - 1 ] ) )
		{
			applyingPreset = true;
			preset->setValue( 0 );
			applyingPreset = false;
		}
	}

private:
	template< class PIX, int nComponents, int maxValue >
	void run( const OFX::RenderArguments& args, OFX::Image* dst, const OFX::Image* src, const OfxRectI& bounds,
	          const print::Settings& settings, print::Plates& plates, bool premultiplied )
	{
		//1. Separate, the whole picture: the print pass reads a cell's tone
		//from anywhere in it.
		SeparateProcessor< PIX, nComponents, maxValue > separate( src, bounds, settings, premultiplied, plates );
		separate.multiThread();

		if( abort() )
			return;

		//2. The mip chain, which is where a cell's mean tone comes from.
		print::BuildMipChain( plates );

		//3. Print the render window.
		PrintProcessor< PIX, nComponents, maxValue > printer( *this );
		printer.setDstImg( dst );
		printer.setup( src, bounds, &settings, &plates, premultiplied );
		printer.setRenderWindow( args.renderWindow );
		printer.process();
	}

	print::Controls controlsAt( double t ) const
	{
		const auto value = []( OFX::DoubleParam* param, double time ) {
			return static_cast< float >( param->getValueAtTime( time ) );
		};
		//ChoiceParam answers through an out parameter rather than a return
		//value, unlike every other param type in the Support library.
		const auto choice = []( OFX::ChoiceParam* param, double time ) {
			int index = 0;
			param->getValueAtTime( time, index );
			return static_cast< float >( index );
		};

		print::Controls c;
		c.blackGeneration = value( blackGeneration, t );
		c.totalInk        = value( totalInk, t );
		c.screen          = value( screen, t );
		c.dotShape        = choice( dotShape, t );
		c.dotGain         = value( dotGain, t );
		c.inkSpread       = value( inkSpread, t );
		for( int plate = 0; plate < kPlateCount; ++plate )
		{
			c.angle[ plate ]             = value( angle[ plate ], t );
			c.registration[ plate ][ 0 ] = value( registration[ plate ][ 0 ], t );
			c.registration[ plate ][ 1 ] = value( registration[ plate ][ 1 ], t );

			double r = 0.0, g = 0.0, b = 0.0;
			ink[ plate ]->getValueAtTime( t, r, g, b );
			c.ink[ plate ][ 0 ] = static_cast< float >( r );
			c.ink[ plate ][ 1 ] = static_cast< float >( g );
			c.ink[ plate ][ 2 ] = static_cast< float >( b );

			c.plate[ plate ] = plateOn[ plate ]->getValueAtTime( t ) ? 1.0f : 0.0f;
		}
		c.wander      = value( wander, t );
		c.wanderSpeed = value( wanderSpeed, t );

		double r = 0.0, g = 0.0, b = 0.0;
		paper->getValueAtTime( t, r, g, b );
		c.paper[ 0 ] = static_cast< float >( r );
		c.paper[ 1 ] = static_cast< float >( g );
		c.paper[ 2 ] = static_cast< float >( b );

		c.inkDensity = value( inkDensity, t );
		c.solo       = choice( solo, t );
		c.mix        = value( mix, t );
		return c;
	}

	//-----------------------------------------------------------------------
	// Presets. The table is plain floats in presets::Param order (Presets.h);
	// these give each column its parameter and each parameter type its
	// reading of a float.
	//-----------------------------------------------------------------------
	static bool differs( OFX::DoubleParam* param, float v )
	{
		double current = 0.0;
		param->getValue( current );
		return std::fabs( current - static_cast< double >( v ) ) > 1e-4;
	}
	static bool differs( OFX::ChoiceParam* param, float v )
	{
		int current = 0;
		param->getValue( current );
		return current != static_cast< int >( std::lround( v ) );
	}
	static bool differs( OFX::BooleanParam* param, float v )
	{
		bool current = false;
		param->getValue( current );
		return current != ( v >= 0.5f );
	}
	static bool differs( OFX::RGBParam* param, const float* v )
	{
		double rgb[ 3 ] = { 0.0, 0.0, 0.0 };
		param->getValue( rgb[ 0 ], rgb[ 1 ], rgb[ 2 ] );
		for( int ch = 0; ch < 3; ++ch )
			if( std::fabs( rgb[ ch ] - static_cast< double >( v[ ch ] ) ) > 1e-4 )
				return true;
		return false;
	}

	static void setIfChanged( OFX::DoubleParam* param, float v )
	{
		if( differs( param, v ) )
			param->setValue( static_cast< double >( v ) );
	}
	static void setIfChanged( OFX::ChoiceParam* param, float v )
	{
		if( differs( param, v ) )
			param->setValue( static_cast< int >( std::lround( v ) ) );
	}
	static void setIfChanged( OFX::BooleanParam* param, float v )
	{
		if( differs( param, v ) )
			param->setValue( v >= 0.5f );
	}
	static void setIfChanged( OFX::RGBParam* param, const float* v )
	{
		if( differs( param, v ) )
			param->setValue( static_cast< double >( v[ 0 ] ), static_cast< double >( v[ 1 ] ), static_cast< double >( v[ 2 ] ) );
	}

	void applyPreset( const presets::Preset& row )
	{
		using namespace presets;
		setIfChanged( blackGeneration, row.v[ kBlackGen ] );
		setIfChanged( totalInk, row.v[ kTotalInk ] );
		setIfChanged( screen, row.v[ kScreen ] );
		setIfChanged( dotShape, row.v[ kDotShape ] );
		setIfChanged( dotGain, row.v[ kDotGain ] );
		setIfChanged( inkSpread, row.v[ kInkSpread ] );
		for( int plate = 0; plate < kPlateCount; ++plate )
		{
			setIfChanged( angle[ plate ], row.v[ kAngleC + plate ] );
			setIfChanged( ink[ plate ], &row.v[ kInkCR + plate * 3 ] );
			setIfChanged( plateOn[ plate ], row.v[ kPlateC + plate ] );
		}
		setIfChanged( wander, row.v[ kWander ] );
		setIfChanged( wanderSpeed, row.v[ kWanderSpeed ] );
		setIfChanged( paper, &row.v[ kPaperR ] );
		setIfChanged( inkDensity, row.v[ kInkDensity ] );
	}

	/// True when `name` is a control the preset covers and it no longer holds
	/// the preset's value.
	bool coveredAndMoved( const std::string& name, const presets::Preset& row ) const
	{
		using namespace presets;
		if( name == kParamBlackGeneration )
			return differs( blackGeneration, row.v[ kBlackGen ] );
		if( name == kParamTotalInk )
			return differs( totalInk, row.v[ kTotalInk ] );
		if( name == kParamScreen )
			return differs( screen, row.v[ kScreen ] );
		if( name == kParamDotShape )
			return differs( dotShape, row.v[ kDotShape ] );
		if( name == kParamDotGain )
			return differs( dotGain, row.v[ kDotGain ] );
		if( name == kParamInkSpread )
			return differs( inkSpread, row.v[ kInkSpread ] );
		if( name == kParamWander )
			return differs( wander, row.v[ kWander ] );
		if( name == kParamWanderSpeed )
			return differs( wanderSpeed, row.v[ kWanderSpeed ] );
		if( name == kParamPaper )
			return differs( paper, &row.v[ kPaperR ] );
		if( name == kParamInkDensity )
			return differs( inkDensity, row.v[ kInkDensity ] );
		for( int plate = 0; plate < kPlateCount; ++plate )
		{
			if( name == kParamAngle[ plate ] )
				return differs( angle[ plate ], row.v[ kAngleC + plate ] );
			if( name == kParamInk[ plate ] )
				return differs( ink[ plate ], &row.v[ kInkCR + plate * 3 ] );
			if( name == kParamPlate[ plate ] )
				return differs( plateOn[ plate ], row.v[ kPlateC + plate ] );
		}
		//Registration, Solo and Mix are performance controls no preset covers.
		return false;
	}

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::ChoiceParam* preset          = nullptr;
	OFX::DoubleParam* blackGeneration = nullptr;
	OFX::DoubleParam* totalInk        = nullptr;
	OFX::DoubleParam* screen          = nullptr;
	OFX::ChoiceParam* dotShape        = nullptr;
	OFX::DoubleParam* dotGain         = nullptr;
	OFX::DoubleParam* inkSpread       = nullptr;
	OFX::DoubleParam* angle[ 4 ]      = {};
	OFX::DoubleParam* registration[ 4 ][ 2 ] = {};
	OFX::DoubleParam* wander          = nullptr;
	OFX::DoubleParam* wanderSpeed     = nullptr;
	OFX::RGBParam* ink[ 4 ]           = {};
	OFX::RGBParam* paper              = nullptr;
	OFX::DoubleParam* inkDensity      = nullptr;
	OFX::BooleanParam* plateOn[ 4 ]   = {};
	OFX::ChoiceParam* solo            = nullptr;
	OFX::DoubleParam* mix             = nullptr;

	/// True while our own writes are in flight, so the changedParam calls they
	/// cause are not mistaken for the operator editing.
	bool applyingPreset = false;
};

//---------------------------------------------------------------------------
// Description.
//---------------------------------------------------------------------------
OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                          OFX::GroupParamDescriptor* group, const char* name,
                                          const std::string& label, const char* hint, double value )
{
	OFX::DoubleParamDescriptor* param = desc.defineDoubleParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setRange( 0.0, 1.0 );
	param->setDisplayRange( 0.0, 1.0 );
	param->setDefault( value );
	param->setParent( *group );
	page->addChild( *param );
	return param;
}

OFX::GroupParamDescriptor* defineGroup( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                        const char* name, const char* label )
{
	OFX::GroupParamDescriptor* group = desc.defineGroupParam( name );
	group->setLabels( label, label, label );
	page->addChild( *group );
	return group;
}

void defineColour( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, OFX::GroupParamDescriptor* group,
                   const char* name, const std::string& label, const char* hint, const float* value )
{
	OFX::RGBParamDescriptor* param = desc.defineRGBParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setDefault( value[ 0 ], value[ 1 ], value[ 2 ] );
	param->setParent( *group );
	page->addChild( *param );
}

mDeclarePluginFactory( RosettePluginFactory, {}, {} );
} // namespace

void RosettePluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// The tone of every cell comes from a mip chain over the whole picture, so
	// no tile can be rendered alone. Frames are independent of each other and
	// of render order: the press wander is a function of time, not of the
	// frames before it, so there is no temporal access to ask for.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
	desc.setSupportsMultipleClipPARs( false );
	desc.setSupportsMultipleClipDepths( false );
}

void RosettePluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	// Same names, same 0..1 ranges, same defaults and same groups as the FFGL
	// build, so the two inspectors read alike and one set of docs covers both.
	// The defaults are preset row 1, Offset Litho, which is what the FFGL
	// constructor's defaults are (`rztest --defaults` holds those two
	// together) -- so reading them from the table here makes three copies one.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );
	const presets::Preset& factory = presets::kPresets[ 0 ];

	// Factory presets, from the same table the FFGL build reads. First on the
	// page here: FFGL puts it last only because a parameter id can never move.
	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "A whole printing process in one move. Picking one sets the separation, "
	                      "the screen, the wander, the inks, the paper and which plates are on the "
	                      "press; editing any of those afterwards falls back to Custom. Registration, "
	                      "Solo and Mix are left alone." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < presets::kCount; ++i )
		presetParam->appendOption( presets::kPresets[ i ].name );
	presetParam->setDefault( 0 );
	presetParam->setIsPersistant( true );
	presetParam->setEvaluateOnChange( false );//the copied values re-render; the label does not
	presetParam->setAnimates( false );
	page->addChild( *presetParam );

	//------------------------------------------------------------ Separation
	OFX::GroupParamDescriptor* separation = defineGroup( desc, page, "separationGroup", "Separation" );

	defineSlider( desc, page, separation, kParamBlackGeneration, "Black Generation",
	              "How much of a colour's neutral part moves off cyan, magenta and yellow and onto "
	              "the black plate. 0 prints greys as equal parts of the three colours; 1 prints them "
	              "as black alone.",
	              factory.v[ presets::kBlackGen ] );
	defineSlider( desc, page, separation, kParamTotalInk, "Total Ink",
	              "The most ink one point may carry over all four plates, 200% to 400%. 0.5 is 300%; "
	              "the colour plates are scaled down to fit, never the black.",
	              factory.v[ presets::kTotalInk ] );

	//---------------------------------------------------------------- Screen
	OFX::GroupParamDescriptor* screenGroup = defineGroup( desc, page, "screenGroup", "Screen" );

	defineSlider( desc, page, screenGroup, kParamScreen, "Screen",
	              "The screen ruling: 2 to 40 pixels per dot, geometrically. 0.46 is about 8.",
	              factory.v[ presets::kScreen ] );

	OFX::ChoiceParamDescriptor* shapeParam = desc.defineChoiceParam( kParamDotShape );
	shapeParam->setLabels( "Dot Shape", "Dot Shape", "Dot Shape" );
	shapeParam->setHint( "Round turns into a checkerboard at 50%; Elliptical chains along the "
	                     "screen angle first; Square never inverts; Line is a line screen. The "
	                     "printed area is the same for every shape." );
	for( int i = 0; i < static_cast< int >( DotShape::Count ); ++i )
		shapeParam->appendOption( DotShapeName( static_cast< DotShape >( i ) ) );
	shapeParam->setDefault( static_cast< int >( std::lround( factory.v[ presets::kDotShape ] ) ) );
	shapeParam->setParent( *screenGroup );
	page->addChild( *shapeParam );

	defineSlider( desc, page, screenGroup, kParamDotGain, "Dot Gain",
	              "How much a 50% dot grows as the ink spreads into the paper, 0 to 40 points. "
	              "0.375 is 15 points, a good litho press; 0.75 is newsprint.",
	              factory.v[ presets::kDotGain ] );
	defineSlider( desc, page, screenGroup, kParamInkSpread, "Ink Spread",
	              "How softly the ink meets the paper: the width of a dot's edge, up to half a "
	              "cell, on top of the one-pixel antialiasing.",
	              factory.v[ presets::kInkSpread ] );
	for( int plate = 0; plate < kPlateCount; ++plate )
		defineSlider( desc, page, screenGroup, kParamAngle[ plate ], std::string( "Angle " ) + kPlateLetter[ plate ],
		              "The plate's screen angle, 0 to 180 degrees. The standard set is C 15, M 75, "
		              "Y 0, K 45; two plates set close together make a moire.",
		              factory.v[ presets::kAngleC + plate ] );

	//----------------------------------------------------------------- Press
	OFX::GroupParamDescriptor* pressGroup = defineGroup( desc, page, "pressGroup", "Press" );

	for( int plate = 0; plate < kPlateCount; ++plate )
		for( int axis = 0; axis < 2; ++axis )
			defineSlider( desc, page, pressGroup, kParamRegister[ plate ][ axis ],
			              std::string( "Register " ) + kPlateLetter[ plate ] + ( axis == 0 ? " X" : " Y" ),
			              "A registration error for this plate, -20 to +20 pixels. 0.5 is in register.",
			              0.5 );
	defineSlider( desc, page, pressGroup, kParamWander, "Press Wander",
	              "How far the plates drift as the press runs, 0 to 20 pixels: a slow, bounded "
	              "wander per plate that depends only on the time, so any frame renders alone.",
	              factory.v[ presets::kWander ] );
	defineSlider( desc, page, pressGroup, kParamWanderSpeed, "Wander Speed",
	              "How often the wander changes direction, 0.05 to 2 times a second, geometrically.",
	              factory.v[ presets::kWanderSpeed ] );

	//------------------------------------------------------------------- Ink
	OFX::GroupParamDescriptor* inkGroup = defineGroup( desc, page, "inkGroup", "Ink" );

	for( int plate = 0; plate < kPlateCount; ++plate )
		defineColour( desc, page, inkGroup, kParamInk[ plate ], std::string( "Ink " ) + kPlateLetter[ plate ],
		              "The ink this plate prints with. Inks are filters: each one removes what it "
		              "absorbs from the paper's light, and overprints multiply.",
		              &factory.v[ presets::kInkCR + plate * 3 ] );
	defineColour( desc, page, inkGroup, kParamPaper, "Paper", "The stock the press prints on.",
	              &factory.v[ presets::kPaperR ] );
	defineSlider( desc, page, inkGroup, kParamInkDensity, "Ink Density",
	              "Scales every ink's absorption, 0 to 1.5. 0.667 is unity.",
	              factory.v[ presets::kInkDensity ] );

	for( int plate = 0; plate < kPlateCount; ++plate )
	{
		const std::string label = std::string( "Plate " ) + kPlateLetter[ plate ];
		OFX::BooleanParamDescriptor* param = desc.defineBooleanParam( kParamPlate[ plate ] );
		param->setLabels( label, label, label );
		param->setHint( "Whether this plate is on the press." );
		param->setDefault( factory.v[ presets::kPlateC + plate ] >= 0.5f );
		param->setParent( *inkGroup );
		page->addChild( *param );
	}

	OFX::ChoiceParamDescriptor* soloParam = desc.defineChoiceParam( kParamSolo );
	soloParam->setLabels( "Solo", "Solo", "Solo" );
	soloParam->setHint( "Print one plate alone, to see its lattice. Wins over the plate switches." );
	for( int i = 0; i < kSoloCount; ++i )
		soloParam->appendOption( kSoloNames[ i ] );
	soloParam->setDefault( 0 );
	soloParam->setParent( *inkGroup );
	page->addChild( *soloParam );

	//---------------------------------------------------------------- Output
	OFX::GroupParamDescriptor* outputGroup = defineGroup( desc, page, "outputGroup", "Output" );

	defineSlider( desc, page, outputGroup, kParamMix, "Mix", "Wet/dry against the untouched input.", 1.0 );

	// The Stoatworks About block: a read-only credit line and one push button
	// per link, in a group that starts folded. Last, so it sits under the
	// effect's own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* RosettePluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new RosettePlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static RosettePluginFactory* factory =
		new RosettePluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
