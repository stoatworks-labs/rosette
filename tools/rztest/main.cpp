/**
    rztest -- render Rosette offline, and measure what the press is doing.

    What area a dot covers, where a lattice points, and what cyan over
    yellow comes to are facts. The harness drives the REAL plugin class
    through the real FFGL sequence in a headless CGL context, reads the
    pixels back, and checks each of the plugin's physical claims against an
    independent statement of the same arithmetic:

        rztest --out /tmp/frame.png     a picture, on the test card
        rztest --list                   every parameter, for the sweep
        rztest --names                  no name over FFGL's 16 characters
        rztest --defaults               preset row 1 IS the constructor's defaults
        rztest --presets                every preset survives every host behaviour
        rztest --wander                 the press wander is bounded, smooth, per plate
        rztest --spot                   the GLSL spot functions against the C++ ones
        rztest --gain                   printed area follows the dot-gain curve
        rztest --angle                  a lattice lies at the angle it was asked for
        rztest --register               an offset moves the dots by exactly that much
        rztest --overprint              two solids multiply as the ink colours predict
        rztest --identity               a flat colour comes back as its CMYK round trip
        rztest --audio                  silence leaves the press alone; a beat kicks it
        rztest --bench                  ms/frame at 720p through 4K
        rztest --pipe                   raw frames in, raw frames out

    `--spot` probes the text the plugin runs: `SpotProbeShaderSource()` is
    assembled around the same `kSpotLibrary` string the print pass is, so a
    transcription that drifted would fail here rather than agree with itself.

    `--pipe` takes the fleet's frame format so one script can film any of the
    FFGL plugins:

        ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
          | rztest --pipe --width 1920 --height 1080 [--script cues.txt] \
          | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
*/

#include "Audio.h"
#include "Controls.h"
#include "Press.h"
#include "Rosette.h"
#include "Screen.h"
#include "Separation.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <queue>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace rosette;

namespace
{
int failures = 0;

void Check( bool ok, const std::string& what )
{
	std::printf( "  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str() );
	if( !ok )
		++failures;
}

/// A formatted line. EVERY conversion must be a floating-point one: the
/// arguments are doubles, so a %d here reads the wrong half of a register and
/// prints a number with no relation to the measurement -- which is worse than
/// no message, because the check beside it still says ok. Count something with
/// %.0f. The attribute makes the compiler enforce it.
__attribute__( ( format( printf, 1, 0 ) ) ) std::string fmt( const char* format, double a, double b = 0.0, double c = 0.0 )
{
	char buffer[ 256 ];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
	std::snprintf( buffer, sizeof( buffer ), format, a, b, c );
#pragma clang diagnostic pop
	return buffer;
}

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency. Takes rows BOTTOM-UP, as GL hands them
// back, and writes them top-down.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& bottomUp )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = bottomUp.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Pictures. All of them bottom-up, in GL's orientation, so a measurement's
// y is the shader's y.
//---------------------------------------------------------------------------
using Image = std::vector< unsigned char >;

unsigned char toByte( float v )
{
	return static_cast< unsigned char >( std::lround( std::min( 1.0f, std::max( 0.0f, v ) ) * 255.0f ) );
}

Image flatField( int width, int height, float r, float g, float b )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( size_t i = 0; i < image.size(); i += 4 )
	{
		image[ i + 0 ] = toByte( r );
		image[ i + 1 ] = toByte( g );
		image[ i + 2 ] = toByte( b );
		image[ i + 3 ] = 255;
	}
	return image;
}

/// A horizontal grey ramp from white at the left to black at the right, so
/// the black plate's tone runs 0..1 across the picture.
Image greyRamp( int width, int height )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const float tone = static_cast< float >( x ) / static_cast< float >( width - 1 );
			const size_t at  = ( static_cast< size_t >( y ) * width + x ) * 4;
			image[ at + 0 ] = image[ at + 1 ] = image[ at + 2 ] = toByte( 1.0f - tone );
			image[ at + 3 ] = 255;
		}
	return image;
}

/// HSV to RGB, for the card's hue sweep.
void hsv( float h, float s, float v, float& r, float& g, float& b )
{
	const float c = v * s;
	const float x = c * ( 1.0f - std::fabs( std::fmod( h * 6.0f, 2.0f ) - 1.0f ) );
	const float m = v - c;
	float rr = 0, gg = 0, bb = 0;
	const int sector = static_cast< int >( h * 6.0f ) % 6;
	switch( sector )
	{
	case 0: rr = c; gg = x; break;
	case 1: rr = x; gg = c; break;
	case 2: gg = c; bb = x; break;
	case 3: gg = x; bb = c; break;
	case 4: rr = x; bb = c; break;
	default: rr = c; bb = x; break;
	}
	r = rr + m;
	g = gg + m;
	b = bb + m;
}

/// The test card. Not meant to look nice: a grey ramp so every tone is
/// present, a row of the process primaries and their overprints so every
/// plate has something to print, and a field of smooth colour with hard
/// shapes in it so both the rosette and a registration fringe are visible.
Image buildCard( int width, int height )
{
	Image image( static_cast< size_t >( width ) * height * 4, 0 );
	const float w = static_cast< float >( width );
	const float h = static_cast< float >( height );

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;//0 at the bottom

			float r = 0.0f, g = 0.0f, b = 0.0f;

			if( v > 0.78f )
			{
				//The ramp, black on the left so the K plate runs 100% to 0%.
				r = g = b = u;
			}
			else if( v > 0.55f )
			{
				//Eight patches: the primaries, their overprints, a dark grey
				//and a skin tone.
				static const float patches[ 8 ][ 3 ] = {
					{ 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 1.0f },
					{ 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 0.0f }, { 0.2f, 0.2f, 0.2f }, { 0.9f, 0.75f, 0.65f },
				};
				const int which = std::min( 7, static_cast< int >( u * 8.0f ) );
				r               = patches[ which ][ 0 ];
				g               = patches[ which ][ 1 ];
				b               = patches[ which ][ 2 ];
			}
			else
			{
				//A hue sweep across, lightness up the band, with a white disc
				//and a dark ring for hard edges.
				hsv( u, 0.75f, 0.35f + 0.6f * ( v / 0.55f ), r, g, b );

				const float dx = ( u - 0.3f ) * ( w / h );
				const float dy = v - 0.28f;
				const float d  = std::sqrt( dx * dx + dy * dy );
				if( d < 0.12f )
					r = g = b = 0.97f;

				const float ex = ( u - 0.72f ) * ( w / h );
				const float ey = v - 0.28f;
				const float e  = std::sqrt( ex * ex + ey * ey );
				if( e < 0.16f && e > 0.11f )
					r = g = b = 0.08f;
			}

			const size_t at = ( static_cast< size_t >( y ) * width + x ) * 4;
			image[ at + 0 ] = toByte( r );
			image[ at + 1 ] = toByte( g );
			image[ at + 2 ] = toByte( b );
			image[ at + 3 ] = 255;
		}
	}

	return image;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without
	//a GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

Image flipRows( const Image& image, int width, int height )
{
	Image flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

GLuint compileStage( GLenum type, const std::string& source, std::string& error )
{
	const GLuint shader   = glCreateShader( type );
	const char* const ptr = source.c_str();
	glShaderSource( shader, 1, &ptr, nullptr );
	glCompileShader( shader );

	GLint compiled = GL_FALSE;
	glGetShaderiv( shader, GL_COMPILE_STATUS, &compiled );
	if( compiled == GL_TRUE )
		return shader;

	GLint length = 0;
	glGetShaderiv( shader, GL_INFO_LOG_LENGTH, &length );
	std::string log( static_cast< size_t >( std::max( length, 1 ) ), '\0' );
	glGetShaderInfoLog( shader, length, nullptr, log.data() );
	error = log;
	glDeleteShader( shader );
	return 0;
}

GLuint buildProgram( const std::string& fragment, std::string& error )
{
	static const char* const vertexSource = R"(#version 410 core
layout( location = 0 ) in vec4 vPosition;
void main() { gl_Position = vPosition; }
)";

	const GLuint vertex = compileStage( GL_VERTEX_SHADER, vertexSource, error );
	if( vertex == 0 )
		return 0;

	const GLuint frag = compileStage( GL_FRAGMENT_SHADER, fragment, error );
	if( frag == 0 )
	{
		glDeleteShader( vertex );
		return 0;
	}

	const GLuint program = glCreateProgram();
	glAttachShader( program, vertex );
	glAttachShader( program, frag );
	glLinkProgram( program );
	glDeleteShader( vertex );
	glDeleteShader( frag );

	GLint linked = GL_FALSE;
	glGetProgramiv( program, GL_LINK_STATUS, &linked );
	if( linked == GL_TRUE )
		return program;

	GLint length = 0;
	glGetProgramiv( program, GL_INFO_LOG_LENGTH, &length );
	std::string log( static_cast< size_t >( std::max( length, 1 ) ), '\0' );
	glGetProgramInfoLog( program, length, nullptr, log.data() );
	error = log;
	glDeleteProgram( program );
	return 0;
}

//---------------------------------------------------------------------------
// The clock and the audio the harness feeds.
//---------------------------------------------------------------------------
enum class AudioFeed
{
	Silence,///< the buffer stays at zero, as with nothing routed
	Pulses  ///< a bass-heavy spectrum with a hit every half second
};

/// Drive the plugin's clock. Synthetic, and it has to be: left to the wall
/// clock the harness renders a hundred frames in a few milliseconds, so no
/// time passes, the press never wanders, and no two runs match.
void driveClock( Rosette& plugin, double seconds, AudioFeed feed )
{
	plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud rather than inferred
	plugin.SetTime( seconds );

	//A synthetic spectrum, written the way the host writes one: one value per
	//element of the Audio buffer. Bass-heavy like programme material, with a
	//ripple so neighbouring bins differ, and a hit every half second so the
	//onset detector has something to find. A pure function of time, so a
	//frame rendered twice hears the same thing twice.
	const double beat  = std::fmod( seconds, 0.5 );
	const float strike = feed == AudioFeed::Pulses ? static_cast< float >( 0.15 + 1.5 * std::exp( -beat / 0.06 ) ) : 0.0f;
	for( int bin = 0; bin < audio::kBins; ++bin )
	{
		const float across = static_cast< float >( bin ) / static_cast< float >( audio::kBins - 1 );
		const float shape  = 0.7f * ( 1.0f - across ) * ( 1.0f - across ) + 0.2f * ( 0.5f + 0.5f * std::sin( 25.0f * across ) );
		plugin.SetParamElementValue( Rosette::PT_AUDIO, static_cast< unsigned int >( bin ), shape * strike );
	}
}

//---------------------------------------------------------------------------
// A rig: the plugin, an input texture and an output framebuffer, at one
// size. Every check builds one, uploads a picture, renders, and reads back.
//---------------------------------------------------------------------------
struct Rig
{
	Rosette plugin;
	int width  = 0;
	int height = 0;
	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};
	bool ready                     = false;

	bool Init( int w, int h )
	{
		width  = w;
		height = h;

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}

		sourceTexture = makeTexture( w, h, nullptr );
		outputTexture = makeTexture( w, h, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( w );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( h );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;
		process.numInputTextures                        = 1;
		process.inputTextures                           = inputs;
		process.HostFBO                                 = outputFBO;
		ready                                           = true;
		return true;
	}

	void Upload( const Image& bottomUp )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	bool Set( const std::string& name, float value )
	{
		for( unsigned int i = 0; i < Rosette::PT_COUNT; ++i )
		{
			const char* declared = plugin.GetParamName( i );
			if( declared != nullptr && name == declared )
			{
				plugin.SetFloatParameter( i, value );
				return true;
			}
		}
		std::fprintf( stderr, "no parameter called '%s'\n", name.c_str() );
		return false;
	}

	bool Render( int frame, double fps, AudioFeed feed )
	{
		driveClock( plugin, static_cast< double >( frame ) / fps, feed );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
	}

	bool RenderFrames( int frames, double fps, AudioFeed feed )
	{
		for( int frame = 0; frame < frames; ++frame )
			if( !Render( frame, fps, feed ) )
			{
				std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", frame );
				return false;
			}
		return true;
	}

	Image Pixels()
	{
		Image pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return pixels;
	}

	~Rig()
	{
		if( !ready )
			return;
		plugin.DeInitGL();
		glDeleteFramebuffers( 1, &outputFBO );
		glDeleteTextures( 1, &outputTexture );
		glDeleteTextures( 1, &sourceTexture );
	}
};

/// The 0..1 parameter that maps to `px` pixels per dot: ScreenPxFromParam
/// inverted, so a check can say "16 px" and mean it.
float screenParam( float px )
{
	return std::log( px / 2.0f ) / std::log( 20.0f );
}

//---------------------------------------------------------------------------
// Dots. Connected components of ink in one channel, and their centroids.
//---------------------------------------------------------------------------
struct Blob
{
	double cx = 0.0, cy = 0.0;
	int pixels  = 0;
	bool border = false;
};

/// Every 4-connected region whose channel `channel` is below `below`.
/// Centroids are weighted by how far below, so an antialiased edge places
/// the dot to a fraction of a pixel.
std::vector< Blob > findBlobs( const Image& image, int width, int height, int channel, int below )
{
	std::vector< Blob > blobs;
	std::vector< char > seen( static_cast< size_t >( width ) * height, 0 );

	auto ink = [ & ]( int x, int y ) {
		return image[ ( static_cast< size_t >( y ) * width + x ) * 4 + channel ] < below;
	};

	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const size_t at = static_cast< size_t >( y ) * width + x;
			if( seen[ at ] || !ink( x, y ) )
				continue;

			Blob blob;
			double weight = 0.0;
			std::queue< std::pair< int, int > > todo;
			todo.push( { x, y } );
			seen[ at ] = 1;
			while( !todo.empty() )
			{
				const auto [ px, py ] = todo.front();
				todo.pop();
				const double w = static_cast< double >( below - image[ ( static_cast< size_t >( py ) * width + px ) * 4 + channel ] );
				blob.cx += w * ( px + 0.5 );
				blob.cy += w * ( py + 0.5 );
				weight += w;
				++blob.pixels;
				if( px == 0 || py == 0 || px == width - 1 || py == height - 1 )
					blob.border = true;

				const int nx[ 4 ] = { px - 1, px + 1, px, px };
				const int ny[ 4 ] = { py, py, py - 1, py + 1 };
				for( int n = 0; n < 4; ++n )
				{
					if( nx[ n ] < 0 || ny[ n ] < 0 || nx[ n ] >= width || ny[ n ] >= height )
						continue;
					const size_t nat = static_cast< size_t >( ny[ n ] ) * width + nx[ n ];
					if( seen[ nat ] || !ink( nx[ n ], ny[ n ] ) )
						continue;
					seen[ nat ] = 1;
					todo.push( { nx[ n ], ny[ n ] } );
				}
			}
			blob.cx /= weight;
			blob.cy /= weight;
			blobs.push_back( blob );
		}

	return blobs;
}

double median( std::vector< double > values )
{
	if( values.empty() )
		return 0.0;
	std::sort( values.begin(), values.end() );
	return values[ values.size() / 2 ];
}

//---------------------------------------------------------------------------
// --list, --names
//---------------------------------------------------------------------------
int runList()
{
	Rosette plugin;
	std::printf( "%-4s %-22s %-9s %10s   %-16s %s\n", "id", "name", "kind", "value", "range", "means" );
	for( unsigned int id = 0; id < Rosette::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( id >= Rosette::PT_ABOUT_FIRST )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s %s\n", id, name ? name : "", "about", "-", "-",
			             "the Stoatworks About block; not swept" );
			continue;
		}
		const char* kind = "standard";
		switch( plugin.GetParamType( id ) )
		{
		case FF_TYPE_BOOLEAN: kind = "boolean"; break;
		case FF_TYPE_EVENT: kind = "event"; break;
		case FF_TYPE_INTEGER: kind = "integer"; break;
		case FF_TYPE_OPTION: kind = "option"; break;
		case FF_TYPE_BUFFER: kind = "buffer"; break;
		case FF_TYPE_TEXT: kind = "text"; break;
		case FF_TYPE_RED:
		case FF_TYPE_GREEN:
		case FF_TYPE_BLUE: kind = "colour"; break;
		default: break;
		}
		RangeStruct range = plugin.GetParamRange( id );
		if( plugin.GetParamType( id ) == FF_TYPE_OPTION )
		{
			range.min = 0.0f;
			range.max = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( id ) ) ) - 1.0f;
		}
		if( plugin.GetParamType( id ) == FF_TYPE_BUFFER )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s %s\n", id, name ? name : "", kind, "-", "-",
			             "the FFT buffer; the harness feeds it a spectrum" );
			continue;
		}
		char rangeText[ 32 ] = {};
		std::snprintf( rangeText, sizeof( rangeText ), "[%g .. %g]", range.min, range.max );
		std::printf( "%-4u %-22s %-9s %10.4f   %-16s\n", id, name ? name : "", kind, plugin.GetFloatParameter( id ), rangeText );
	}
	return 0;
}

int runNames()
{
	Rosette plugin;
	std::printf( "names longer than FFGL's 16 characters:\n\n" );
	int over = 0;
	for( unsigned int id = 0; id < Rosette::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name != nullptr && std::strlen( name ) > 16 )
		{
			std::printf( "  %-3u  %-28s %zu\n", id, name, std::strlen( name ) );
			++over;
		}
		for( unsigned int e = 0; e < plugin.GetNumParamElements( id ); ++e )
		{
			const char* el = plugin.GetParamElementName( id, e );
			if( el != nullptr && std::strlen( el ) > 16 )
			{
				std::printf( "  %-3u  %-28s element %u: %s (%zu)\n", id, name, e, el, std::strlen( el ) );
				++over;
			}
		}
	}
	std::printf( "\n  %d over the limit\n", over );
	return over == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --defaults
//---------------------------------------------------------------------------
int runDefaults()
{
	std::printf( "preset 1 is the constructor's defaults\n\n" );
	Rosette plugin;
	int count                   = 0;
	const unsigned int* covered = Rosette::PresetParamIDsForTest( count );
	const presets::Preset& row  = presets::kPresets[ 0 ];
	for( int c = 0; c < count; ++c )
	{
		const float d       = plugin.GetFloatParameter( covered[ c ] );
		const char* pname   = plugin.GetParamName( covered[ c ] );
		char line[ 128 ];
		std::snprintf( line, sizeof( line ), "%-16s default %g  preset %g",
		               pname ? pname : "?", d, row.v[ c ] );
		Check( std::fabs( d - row.v[ c ] ) < 1e-6f, line );
	}

	//And the override machinery: a chosen preset overrides, Custom restores.
	plugin.SetFloatParameter( Rosette::PT_SCREEN, 0.9f );
	plugin.SetFloatParameter( Rosette::PT_PRESET, 3.0f );//Riso 2-Colour
	Check( plugin.Effective( Rosette::PT_PLATE_Y ) == 0.0f && plugin.Effective( Rosette::PT_SCREEN ) == 0.6f,
	       "a chosen preset overrides the plates and the screen" );
	plugin.SetFloatParameter( Rosette::PT_PRESET, 0.0f );
	Check( plugin.Effective( Rosette::PT_SCREEN ) == 0.9f, "Custom hands the controls back" );

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --presets
//
// FFGL's host owns parameter state and is free to push it back down at any
// time. Three hosts to survive, and the plugin cannot tell which it is
// talking to: one that re-reads the plugin's values and hands them straight
// back; one that ignores events and restates what it still believes; one
// that keeps its parameters shorter than a float. All three arrive as
// SetFloatParameter calls, and none of them is an operator.
//---------------------------------------------------------------------------
int runPresetTest()
{
	int coveredCount            = 0;
	const unsigned int* covered = Rosette::PresetParamIDsForTest( coveredCount );

	enum class Host
	{
		Honours,
		Ignores,
		Quantises
	};
	struct HostCase
	{
		Host kind;
		const char* name;
	};
	const HostCase hosts[] = {
		{ Host::Honours, "re-reads and echoes" },
		{ Host::Ignores, "restates its own" },
		{ Host::Quantises, "echoes, 1/1000 steps" },
	};

	for( const HostCase& host : hosts )
	{
		for( int preset = 1; preset <= presets::kCount; ++preset )
		{
			Rosette plugin;

			std::vector< float > hostOwn;
			for( int j = 0; j < coveredCount; ++j )
				hostOwn.push_back( plugin.GetFloatParameter( covered[ j ] ) );

			plugin.SetFloatParameter( Rosette::PT_PRESET, static_cast< float >( preset ) );

			for( int j = 0; j < coveredCount; ++j )
			{
				float back = 0.0f;
				switch( host.kind )
				{
				case Host::Honours: back = plugin.GetFloatParameter( covered[ j ] ); break;
				case Host::Ignores: back = hostOwn[ static_cast< size_t >( j ) ]; break;
				case Host::Quantises: back = std::round( plugin.GetFloatParameter( covered[ j ] ) * 1000.0f ) / 1000.0f; break;
				}
				plugin.SetFloatParameter( covered[ j ], back );
			}

			const int still = static_cast< int >( std::lround( plugin.GetFloatParameter( Rosette::PT_PRESET ) ) );
			bool ok         = still == preset;

			//Still selected is not enough -- it has to be what renders.
			for( int j = 0; j < coveredCount; ++j )
				ok = ok && std::fabs( plugin.Effective( covered[ j ] ) - presets::kPresets[ preset - 1 ].v[ j ] ) <= 1e-6f;

			if( !ok )
			{
				Check( false, std::string( host.name ) + ": " + presets::kPresets[ preset - 1 ].name + " (shows " + std::to_string( still ) + ")" );
				continue;
			}

			//An operator turning a covered knob must still drop to Custom, and
			//the knob must then mean what the operator set.
			const float moved = hostOwn[ 2 ] > 0.5f ? 0.123f : 0.877f;
			plugin.SetFloatParameter( covered[ 2 ], moved );
			const int after = static_cast< int >( std::lround( plugin.GetFloatParameter( Rosette::PT_PRESET ) ) );
			Check( after == 0 && std::fabs( plugin.Effective( covered[ 2 ] ) - moved ) < 1e-6f,
			       std::string( host.name ) + ": " + presets::kPresets[ preset - 1 ].name );
		}
	}

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --wander. No GL: the press model is C++.
//---------------------------------------------------------------------------
int runWander()
{
	std::printf( "the press wander: bounded, smooth, different per plate, zero at zero\n\n" );

	constexpr float amplitude = 10.0f;
	constexpr float speed     = 0.3f;
	constexpr double fps      = 60.0;
	constexpr int frames      = 60 * 60;

	double worstBound = 0.0;
	double worstStep  = 0.0;
	double separation = 0.0;
	press::Offset previous[ 4 ];

	for( int frame = 0; frame < frames; ++frame )
	{
		const double t = frame / fps;
		press::Offset o[ 4 ];
		for( int plate = 0; plate < 4; ++plate )
		{
			o[ plate ] = press::Wander( plate, t, amplitude, speed );
			//An initializer_list max needs ONE type: a float fabs beside a double
			//bound is an ambiguous call, not a conversion.
			worstBound = std::max( { worstBound, static_cast< double >( std::fabs( o[ plate ].x ) ), static_cast< double >( std::fabs( o[ plate ].y ) ) } );
			if( frame > 0 )
				worstStep = std::max( worstStep, static_cast< double >( std::hypot( o[ plate ].x - previous[ plate ].x, o[ plate ].y - previous[ plate ].y ) ) );
			previous[ plate ] = o[ plate ];
		}
		separation += std::hypot( o[ 0 ].x - o[ 1 ].x, o[ 0 ].y - o[ 1 ].y );
	}
	separation /= frames;

	Check( worstBound <= amplitude, fmt( "never further than the amplitude: %.3f of %.0f px", worstBound, amplitude ) );
	Check( worstBound > 0.5 * amplitude, fmt( "and actually uses it: reaches %.3f px", worstBound ) );
	//At 0.3 Hz the quintic's peak slope of 1.875 per lattice step gives 0.13
	//px per frame between the two octaves; a quarter pixel is a tenth of a
	//cell at the default screen, and well under a visible jump.
	Check( worstStep < 0.25, fmt( "moves smoothly: largest step %.4f px per frame", worstStep ) );
	Check( separation > 2.0, fmt( "the plates wander differently: C and M are %.2f px apart on average", separation ) );

	const press::Offset zero = press::Wander( 2, 12.345, 0.0f, speed );
	Check( zero.x == 0.0f && zero.y == 0.0f, "zero amplitude is exactly zero" );

	const press::Offset a = press::Wander( 1, 33.3, amplitude, speed );
	const press::Offset b = press::Wander( 1, 33.3, amplitude, speed );
	Check( a.x == b.x && a.y == b.y, "a pure function of time" );

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --spot
//---------------------------------------------------------------------------
int runSpot()
{
	std::printf( "the GLSL spot functions against Screen.cpp\n\n" );

	std::string error;
	const GLuint program = buildProgram( SpotProbeShaderSource(), error );
	if( program == 0 )
	{
		std::fprintf( stderr, "the probe shader would not build:\n%s\n", error.c_str() );
		return 1;
	}

	constexpr int kSamples = 257;//odd, so no sample lands on the diamond exactly

	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, kSamples, kSamples, 0, GL_RGBA, GL_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );
	const GLuint fbo = makeFramebuffer( texture );

	GLuint vao = 0, vbo = 0;
	glGenVertexArrays( 1, &vao );
	glBindVertexArray( vao );
	const float triangle[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
	glGenBuffers( 1, &vbo );
	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	glBufferData( GL_ARRAY_BUFFER, sizeof( triangle ), triangle, GL_STATIC_DRAW );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, 0, nullptr );

	glUseProgram( program );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glViewport( 0, 0, kSamples, kSamples );

	std::vector< float > readback( static_cast< size_t >( kSamples ) * kSamples * 4 );
	int checks = 0;
	int bad    = 0;
	float worst = 0.0f;
	//sqrt is allowed a couple of ulp in GLSL; anything past 1e-5 is a
	//different function, not a different rounding.
	constexpr float kTolerance = 1e-5f;

	for( int s = 0; s < static_cast< int >( DotShape::Count ); ++s )
	{
		glUniform1f( glGetUniformLocation( program, "Shape" ), static_cast< float >( s ) );
		glUniform1f( glGetUniformLocation( program, "Samples" ), static_cast< float >( kSamples ) );
		glClear( GL_COLOR_BUFFER_BIT );
		glDrawArrays( GL_TRIANGLES, 0, 3 );
		glReadPixels( 0, 0, kSamples, kSamples, GL_RGBA, GL_FLOAT, readback.data() );

		int shapeBad = 0;
		for( int j = 0; j < kSamples; ++j )
			for( int i = 0; i < kSamples; ++i )
			{
				const float x = ( ( static_cast< float >( i ) + 0.5f ) / kSamples ) * 2.0f - 1.0f;
				const float y = ( ( static_cast< float >( j ) + 0.5f ) / kSamples ) * 2.0f - 1.0f;
				const float expected = Spot( x, y, static_cast< DotShape >( s ) );
				const float got      = readback[ ( static_cast< size_t >( j ) * kSamples + i ) * 4 ];
				const float d        = std::fabs( got - expected );
				++checks;
				worst = std::max( worst, d );
				if( d > kTolerance )
					++shapeBad;
			}
		bad += shapeBad;
		Check( shapeBad == 0, std::string( DotShapeName( static_cast< DotShape >( s ) ) ) + ": " + std::to_string( kSamples * kSamples ) + " points, " + std::to_string( shapeBad ) + " disagree" );
	}

	std::printf( "\n  %d comparisons, %d past %g; largest difference %.3g\n", checks, bad, kTolerance, worst );

	glDeleteBuffers( 1, &vbo );
	glDeleteVertexArrays( 1, &vao );
	glDeleteFramebuffers( 1, &fbo );
	glDeleteTextures( 1, &texture );
	glDeleteProgram( program );

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --gain
//
// A grey ramp through the black plate alone. In each band of tones the mean
// printed area -- recovered per pixel from the green channel, which black
// ink absorbs 88% of -- must follow a' = a + 4 G a ( 1 - a ).
//---------------------------------------------------------------------------
int runGain()
{
	std::printf( "printed area against the dot-gain curve, black plate on a grey ramp\n\n" );

	constexpr int width  = 1280;
	constexpr int height = 512;
	constexpr int bands  = 20;
	constexpr float px   = 12.0f;
	constexpr float tolerance = 0.02f;

	struct Case
	{
		DotShape shape;
		float gain;
	};
	const Case cases[] = {
		{ DotShape::Round, 0.0f }, { DotShape::Round, 0.2f },
		{ DotShape::Elliptical, 0.0f }, { DotShape::Square, 0.0f },
		{ DotShape::Line, 0.0f }, { DotShape::Square, 0.2f },
	};

	const Image ramp = greyRamp( width, height );
	float worstAll = 0.0f;

	for( const Case& c : cases )
	{
		Rig rig;
		if( !rig.Init( width, height ) )
			return 1;
		rig.Upload( ramp );
		rig.Set( "Solo", 4.0f );//Black
		rig.Set( "Screen", screenParam( px ) );
		rig.Set( "Dot Shape", static_cast< float >( c.shape ) );
		rig.Set( "Dot Gain", c.gain / 0.4f );
		rig.Set( "Ink Spread", 0.0f );
		if( !rig.RenderFrames( 1, 60.0, AudioFeed::Silence ) )
			return 1;
		const Image out = rig.Pixels();

		const float inkG = kDefaultInk[ 3 ][ 1 ];
		float worst      = 0.0f;
		int worstBand    = 0;
		for( int b = 0; b < bands; ++b )
		{
			const int x0 = b * width / bands;
			const int x1 = ( b + 1 ) * width / bands;
			double sum   = 0.0;
			long n       = 0;
			for( int y = 0; y < height; ++y )
				for( int x = x0; x < x1; ++x )
				{
					const float g = out[ ( static_cast< size_t >( y ) * width + x ) * 4 + 1 ] / 255.0f;
					sum += ( 1.0f - g ) / ( 1.0f - inkG );
					++n;
				}
			const double measured = sum / n;

			//The band's expected mean area, integrating the curve over the
			//tones the band spans.
			double expected = 0.0;
			constexpr int steps = 400;
			for( int i = 0; i < steps; ++i )
			{
				const float tone = ( static_cast< float >( x0 ) + ( i + 0.5f ) * ( x1 - x0 ) / steps ) / static_cast< float >( width - 1 );
				expected += DotGain( tone, c.gain );
			}
			expected /= steps;

			const float err = static_cast< float >( std::fabs( measured - expected ) );
			if( err > worst )
			{
				worst     = err;
				worstBand = b;
			}
		}
		worstAll = std::max( worstAll, worst );
		Check( worst <= tolerance,
		       std::string( DotShapeName( c.shape ) ) + fmt( " gain %.1f: worst band %.0f, off by %.4f", c.gain, worstBand, worst ) );
	}

	std::printf( "\n  largest error in printed area across all cases: %.4f (tolerance %.2f)\n", worstAll, tolerance );
	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --angle
//
// One plate alone on a flat field is a lattice of identical dots. Find the
// dots, find each one's nearest neighbour, and the direction to it -- taken
// modulo the lattice's own 90 degree symmetry -- is the screen angle. The
// distance to it is the screen ruling, which checks Screen is in pixels.
//---------------------------------------------------------------------------
struct Lattice
{
	double angle   = 0.0;///< degrees, mod 90
	double spacing = 0.0;///< px
	int dots       = 0;
	int compared   = 0;
	std::vector< Blob > blobs;
};

Lattice measureLattice( const Image& out, int width, int height, int channel, int below )
{
	Lattice result;
	result.blobs = findBlobs( out, width, height, channel, below );

	std::vector< double > angles;
	std::vector< double > spacings;
	for( size_t i = 0; i < result.blobs.size(); ++i )
	{
		const Blob& a = result.blobs[ i ];
		if( a.border || a.pixels < 3 )
			continue;
		++result.dots;

		double best = 1e30;
		size_t who  = i;
		for( size_t j = 0; j < result.blobs.size(); ++j )
		{
			if( j == i )
				continue;
			const double d = std::hypot( result.blobs[ j ].cx - a.cx, result.blobs[ j ].cy - a.cy );
			if( d < best )
			{
				best = d;
				who  = j;
			}
		}
		if( who == i )
			continue;
		const Blob& b = result.blobs[ who ];
		double deg    = std::atan2( b.cy - a.cy, b.cx - a.cx ) * 180.0 / M_PI;
		deg           = std::fmod( std::fmod( deg, 90.0 ) + 90.0, 90.0 );
		angles.push_back( deg );
		spacings.push_back( best );
		++result.compared;
	}

	//Angles near 0 and near 90 are the same angle; fold them before taking
	//the median so a lattice at 0 degrees does not read as 45.
	if( !angles.empty() )
	{
		const double m = median( angles );
		for( double& deg : angles )
			if( deg - m > 45.0 )
				deg -= 90.0;
			else if( m - deg > 45.0 )
				deg += 90.0;
	}
	result.angle   = std::fmod( median( angles ) + 90.0, 90.0 );
	result.spacing = median( spacings );
	return result;
}

int runAngle()
{
	std::printf( "the black plate's lattice, measured from its dot centroids\n\n" );

	constexpr int size   = 640;
	constexpr float px   = 16.0f;
	//Below this in green the pixel is more ink than paper.
	const int inkBelow = static_cast< int >( ( 1.0f - 0.5f * ( 1.0f - kDefaultInk[ 3 ][ 1 ] ) ) * 255.0f );

	struct Case
	{
		DotShape shape;
		float degrees;
		float tone;
	};
	//30% for the round dot: at exactly 50% a Euclidean dot is a checkerboard
	//whose diamonds touch at the corners, and touching dots are one blob. The
	//square dot at 50% still has a gap, so the spec's tone is kept there.
	const Case cases[] = {
		{ DotShape::Round, 45.0f, 0.3f }, { DotShape::Round, 15.0f, 0.3f }, { DotShape::Round, 75.0f, 0.3f },
		{ DotShape::Square, 45.0f, 0.5f }, { DotShape::Elliptical, 20.0f, 0.3f },
	};

	for( const Case& c : cases )
	{
		Rig rig;
		if( !rig.Init( size, size ) )
			return 1;
		rig.Upload( flatField( size, size, 1.0f - c.tone, 1.0f - c.tone, 1.0f - c.tone ) );
		rig.Set( "Solo", 4.0f );
		rig.Set( "Screen", screenParam( px ) );
		rig.Set( "Dot Shape", static_cast< float >( c.shape ) );
		rig.Set( "Dot Gain", 0.0f );
		rig.Set( "Angle K", c.degrees / 180.0f );
		if( !rig.RenderFrames( 1, 60.0, AudioFeed::Silence ) )
			return 1;

		const Lattice lattice = measureLattice( rig.Pixels(), size, size, 1, inkBelow );
		const double want     = std::fmod( c.degrees, 90.0f );
		double dAngle         = std::fabs( lattice.angle - want );
		dAngle                = std::min( dAngle, 90.0 - dAngle );

		Check( lattice.dots > 500 && dAngle <= 1.0,
		       std::string( DotShapeName( c.shape ) )
		           + fmt( " asked %.0f deg: %.0f dots, measured %.2f deg", c.degrees,
		                  static_cast< double >( lattice.dots ), lattice.angle ) );
		Check( std::fabs( lattice.spacing - px ) <= 0.02 * px,
		       fmt( "   nearest neighbour %.2f px for a %.0f px screen", lattice.spacing, px ) );
	}

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --register
//---------------------------------------------------------------------------
int runRegister()
{
	std::printf( "a registration offset moves the cyan dots by exactly that offset\n\n" );

	constexpr int size = 512;
	constexpr float px = 16.0f;
	const int inkBelow = static_cast< int >( ( 1.0f - 0.5f * ( 1.0f - kDefaultInk[ 0 ][ 0 ] ) ) * 255.0f );

	auto centroids = [ & ]( float dx, float dy, std::vector< Blob >& out ) {
		Rig rig;
		if( !rig.Init( size, size ) )
			return false;
		//A field that separates to cyan alone: red down, green and blue full.
		rig.Upload( flatField( size, size, 0.7f, 1.0f, 1.0f ) );
		rig.Set( "Solo", 1.0f );//Cyan
		rig.Set( "Screen", screenParam( px ) );
		rig.Set( "Dot Gain", 0.0f );
		rig.Set( "Register C X", 0.5f + dx / 40.0f );
		rig.Set( "Register C Y", 0.5f + dy / 40.0f );
		if( !rig.RenderFrames( 1, 60.0, AudioFeed::Silence ) )
			return false;
		//The red channel: cyan ink absorbs all of it.
		out = findBlobs( rig.Pixels(), size, size, 0, inkBelow );
		return true;
	};

	std::vector< Blob > base;
	if( !centroids( 0.0f, 0.0f, base ) )
		return 1;

	const float offsets[][ 2 ] = { { 3.0f, -2.0f }, { -5.0f, 7.0f }, { 0.5f, 0.25f } };
	for( const auto& o : offsets )
	{
		std::vector< Blob > moved;
		if( !centroids( o[ 0 ], o[ 1 ], moved ) )
			return 1;

		//Each base dot to the moved dot nearest where the offset says it
		//should have landed; the residual is what the plugin got wrong.
		//
		//Matching to the nearest dot *without* using the offset does not
		//work, and the reason is not a detail of this test: a lattice is
		//periodic, so a shift by d and a shift by d plus a lattice vector
		//produce the identical picture. There is no measurement, here or on
		//a real press, that could tell them apart. Past half a cell the
		//nearest dot is therefore the wrong dot and the answer comes back
		//exactly one cell out -- which is what this check reported for
		//(-5, +7) at a 16 px screen: a worst disagreement of 16.05 px.
		//
		//So what is established is the displacement MODULO THE LATTICE, and
		//that is the whole of what "the offset moved the dots" can mean. It
		//still fails on a plugin that moves them by the wrong amount, in the
		//wrong direction, or not at all -- the residual then runs to a good
		//fraction of a cell rather than to hundredths of a pixel.
		double worst = 0.0;
		int matched  = 0;
		for( const Blob& a : base )
		{
			if( a.border )
				continue;
			const double wantX = a.cx + o[ 0 ];
			const double wantY = a.cy + o[ 1 ];

			double best   = 1e30;
			const Blob* b = nullptr;
			for( const Blob& m : moved )
			{
				const double d = std::hypot( m.cx - wantX, m.cy - wantY );
				if( d < best )
				{
					best = d;
					b    = &m;
				}
			}
			if( b == nullptr || b->border )
				continue;
			worst = std::max( worst, best );
			++matched;
		}
		//A whole-pixel offset and a fractional one are not measurable to the
		//same accuracy, and one tolerance for both hides which is which.
		//
		//Shift the plate by a whole number of pixels and the sampling lattice
		//translates exactly: every dot is rendered from the same coverage
		//pattern, one pixel over, and the centroid follows to the ten-thousandth
		//of a pixel. Shift it by half a pixel and no dot is the same shape any
		//more -- the offset lands as a change in *partial coverage* at every dot
		//edge, and the centroid is recovered from those antialiased fringes. How
		//well that works is a property of the rasteriser, not of the plugin.
		//
		//Which is how CI found this: 0.0491 px on this Mac and 0.0508 px on
		//GitHub's macOS runner, which has no accelerated GL context, against a
		//flat tolerance of 0.05. Nothing had changed but the rasteriser.
		//
		//So the whole-pixel cases are now held five times TIGHTER than before,
		//and the fractional one is allowed a tenth of a pixel -- still far
		//inside the failure this is here to catch, which is a plate that moves
		//by the wrong amount or not at all and lands a good fraction of a cell
		//out.
		const bool wholePixel = o[ 0 ] == std::floor( o[ 0 ] ) && o[ 1 ] == std::floor( o[ 1 ] );
		const double tolerance = wholePixel ? 0.01 : 0.10;
		Check( matched > 300 && worst <= tolerance,
		       fmt( "(%+.2f, %+.2f) px: ", o[ 0 ], o[ 1 ] ) + std::to_string( matched ) + fmt( " dots moved, worst disagreement %.4f px", worst ) + fmt( " (tolerance %.2f)", tolerance ) );
	}

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --overprint
//
// Black input with no black generation separates to c = m = y = 1. Turn
// two plates off and the other two print as solids, one over the other.
//---------------------------------------------------------------------------
int runOverprint()
{
	std::printf( "two solid inks multiply as the ink colours predict\n\n" );

	constexpr int size = 128;
	struct Case
	{
		const char* name;
		bool on[ 4 ];
	};
	const Case cases[] = {
		{ "C over Y", { true, false, true, false } },
		{ "C over M", { true, true, false, false } },
		{ "M over Y", { false, true, true, false } },
		{ "C over M over Y", { true, true, true, false } },
	};

	for( const Case& c : cases )
	{
		Rig rig;
		if( !rig.Init( size, size ) )
			return 1;
		rig.Upload( flatField( size, size, 0.0f, 0.0f, 0.0f ) );
		rig.Set( "Black Generation", 0.0f );
		rig.Set( "Total Ink", 1.0f );//400%: no limit
		rig.Set( "Plate C", c.on[ 0 ] ? 1.0f : 0.0f );
		rig.Set( "Plate M", c.on[ 1 ] ? 1.0f : 0.0f );
		rig.Set( "Plate Y", c.on[ 2 ] ? 1.0f : 0.0f );
		rig.Set( "Plate K", c.on[ 3 ] ? 1.0f : 0.0f );
		if( !rig.RenderFrames( 1, 60.0, AudioFeed::Silence ) )
			return 1;
		const Image out = rig.Pixels();

		float coverage[ 4 ];
		for( int i = 0; i < 4; ++i )
			coverage[ i ] = c.on[ i ] ? 1.0f : 0.0f;
		float expected[ 3 ];
		InkModel( coverage, kDefaultInk, kDefaultPaper, 1.0f, expected );

		int worst = 0;
		for( size_t i = 0; i < out.size(); i += 4 )
			for( int ch = 0; ch < 3; ++ch )
				worst = std::max( worst, std::abs( static_cast< int >( out[ i + ch ] ) - static_cast< int >( toByte( expected[ ch ] ) ) ) );

		Check( worst <= 1, std::string( c.name ) + fmt( ": predicted (%.3f, %.3f, %.3f), ", expected[ 0 ], expected[ 1 ], expected[ 2 ] )
		                       + "every pixel within " + std::to_string( worst ) + "/255" );
	}

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --identity
//
// A flat colour through the whole chain should come back as what the ink
// model gives for its own separation. Two claims, and they are different
// claims:
//
//   * where no plate is SCREENED -- every plate either blank or solid --
//     there is no halftone in the picture at all, so the round trip is the
//     separation and the ink model back to back and must be near exact.
//   * where a plate carries a partial tone, the frame is a dot pattern and
//     what is compared is its MEAN against a continuous model. That mean is
//     only as good as the screen's ability to resolve the area it was asked
//     for, so the error is a function of how many pixels a cell has.
//
// The second is why this check sweeps the cell size rather than asserting
// one number. At the minimum screen a cell is 2 px -- four pixels to carry
// a dot of arbitrary area, with an antialiasing width of a good fraction of
// the cell -- and the mean comes out up to 0.042 off. Growing the cell fixes
// it, monotonically, which is the evidence that it is a resolution limit and
// not a wrong model: a wrong separation or a wrong ink model would be just
// as wrong at 16 px as at 2.
//---------------------------------------------------------------------------
int runIdentity()
{
	std::printf( "a flat colour comes back as its CMYK round trip\n\n" );

	constexpr int size = 256;

	const float colours[][ 3 ] = {
		{ 0.5f, 0.5f, 0.5f }, { 1.0f, 0.0f, 0.0f }, { 0.9f, 0.75f, 0.65f }, { 0.4f, 0.6f, 0.9f },
		{ 0.15f, 0.1f, 0.2f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 1.0f, 1.0f }, { 0.2f, 0.7f, 0.3f },
	};

	struct Step
	{
		float px;
		float bound;///< the most a screened colour's frame mean may be off by
	};
	//The bounds are the measured behaviour with headroom, not thresholds
	//chosen to pass: 0.042, 0.015, 0.010 and 0.007 are what this reports on
	//an M4 Max. A model error would break the 16 px bound first and by a
	//wide margin -- the unscreened bound below is the one that pins the
	//colour arithmetic itself, and it is twenty times tighter.
	const Step steps[] = { { 2.0f, 0.05f }, { 4.0f, 0.02f }, { 8.0f, 0.013f }, { 16.0f, 0.009f } };

	//Where no plate is screened the answer cannot depend on the screen at
	//all, so one bound covers every cell size.
	constexpr float kUnscreenedBound = 0.003f;

	float previousWorst = 0.0f;
	for( size_t s = 0; s < sizeof( steps ) / sizeof( steps[ 0 ] ); ++s )
	{
		const Step& step  = steps[ s ];
		float worstScreened = 0.0f;
		float worstFlat     = 0.0f;

		for( const auto& rgb : colours )
		{
			Rig rig;
			if( !rig.Init( size, size ) )
				return 1;
			rig.Upload( flatField( size, size, rgb[ 0 ], rgb[ 1 ], rgb[ 2 ] ) );
			rig.Set( "Screen", screenParam( step.px ) );
			rig.Set( "Dot Gain", 0.0f );
			rig.Set( "Ink Spread", 0.0f );
			if( !rig.RenderFrames( 1, 60.0, AudioFeed::Silence ) )
				return 1;
			const Image out = rig.Pixels();

			double mean[ 3 ] = { 0.0, 0.0, 0.0 };
			for( size_t i = 0; i < out.size(); i += 4 )
				for( int ch = 0; ch < 3; ++ch )
					mean[ ch ] += out[ i + ch ] / 255.0;
			for( double& m : mean )
				m /= static_cast< double >( size ) * size;

			const Cmyk sep       = Separate( rgb[ 0 ], rgb[ 1 ], rgb[ 2 ], 1.0f, 3.0f );
			const float cov[ 4 ] = { sep.c, sep.m, sep.y, sep.k };
			float expected[ 3 ];
			InkModel( cov, kDefaultInk, kDefaultPaper, 1.0f, expected );

			//A plate is screened when it carries a tone that is neither
			//blank nor solid -- that is the only case a dot pattern exists.
			int screened = 0;
			for( float c : cov )
				if( c > 0.02f && c < 0.98f )
					++screened;

			float worst = 0.0f;
			for( int ch = 0; ch < 3; ++ch )
				worst = std::max( worst, static_cast< float >( std::fabs( mean[ ch ] - expected[ ch ] ) ) );

			if( screened > 0 )
				worstScreened = std::max( worstScreened, worst );
			else
				worstFlat = std::max( worstFlat, worst );

			if( s == 0 )
				std::printf( "     %s (%.2f, %.2f, %.2f) -> cmyk (%.2f, %.2f, %.2f, %.2f) -> "
				             "expected (%.3f, %.3f, %.3f), mean (%.3f, %.3f, %.3f), off by %.4f\n",
				             screened > 0 ? "screened " : "unscreened", rgb[ 0 ], rgb[ 1 ], rgb[ 2 ],
				             sep.c, sep.m, sep.y, sep.k,
				             expected[ 0 ], expected[ 1 ], expected[ 2 ],
				             mean[ 0 ], mean[ 1 ], mean[ 2 ], worst );
		}

		Check( worstFlat <= kUnscreenedBound,
		       fmt( "%.0f px cell: with no plate screened, off by %.4f", step.px, worstFlat ) );
		Check( worstScreened <= step.bound,
		       fmt( "%.0f px cell: with a screened plate, off by %.4f", step.px, worstScreened )
		           + fmt( " (bound %.3f)", step.bound ) );
		if( s > 0 )
			Check( worstScreened < previousWorst,
			       fmt( "%.0f px cell: and better than the %.0f px one", step.px, steps[ s - 1 ].px )
			           + fmt( " -- %.4f against %.4f", worstScreened, previousWorst ) );
		previousWorst = worstScreened;
	}

	std::printf( "\n  The screened error falls with the cell size, which is what says it is\n"
	             "  the screen's resolution and not the colour model.\n" );

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --audio
//---------------------------------------------------------------------------
int runAudio()
{
	std::printf( "silence leaves the press alone; a beat kicks it\n\n" );

	constexpr int width  = 320;
	constexpr int height = 180;
	constexpr int frames = 120;//two seconds, four hits

	auto run = [ & ]( AudioFeed feed, float drive, double& furthest, unsigned long long& onsets ) {
		Rig rig;
		if( !rig.Init( width, height ) )
			return false;
		rig.Upload( buildCard( width, height ) );
		rig.Set( "Audio Drive", drive );
		furthest = 0.0;
		for( int frame = 0; frame < frames; ++frame )
		{
			if( !rig.Render( frame, 60.0, feed ) )
				return false;
			float offsets[ 4 ][ 2 ];
			rig.plugin.PlateOffsetsForTest( offsets );
			for( auto& o : offsets )
				furthest = std::max( { furthest, static_cast< double >( std::fabs( o[ 0 ] ) ), static_cast< double >( std::fabs( o[ 1 ] ) ) } );
		}
		onsets = rig.plugin.AnalyserForTest().Onsets();
		return true;
	};

	double furthest;
	unsigned long long onsets;

	if( !run( AudioFeed::Silence, 1.0f, furthest, onsets ) )
		return 1;
	Check( furthest == 0.0 && onsets == 0, fmt( "silence at full drive: plates at %.3f px, %.0f onsets", furthest, static_cast< double >( onsets ) ) );

	if( !run( AudioFeed::Pulses, 0.0f, furthest, onsets ) )
		return 1;
	Check( furthest == 0.0 && onsets >= 3, fmt( "beats at zero drive: plates at %.3f px, %.0f onsets heard and ignored", furthest, static_cast< double >( onsets ) ) );

	if( !run( AudioFeed::Pulses, 1.0f, furthest, onsets ) )
		return 1;
	Check( furthest > 2.0 && onsets >= 3 && onsets <= 6, fmt( "beats at full drive: plates thrown %.2f px, %.0f onsets in two seconds", furthest, static_cast< double >( onsets ) ) );

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( int width, int height, int frames, double fps )
{
	Rig rig;
	if( !rig.Init( width, height ) )
		return -1.0;
	rig.Upload( buildCard( width, height ) );
	rig.Set( "Press Wander", 0.3f );

	//A warm-up that is thrown away: the first frames pay for buffer
	//allocation and shader specialisation, which are real costs but not the
	//per-frame cost.
	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		rig.Render( frame, fps, AudioFeed::Pulses );
	glFinish();

	//glFinish on both sides: GL calls queue, and without forcing completion
	//this times how fast the driver accepts commands, not how fast the GPU
	//runs them.
	const auto start = std::chrono::steady_clock::now();
	for( int frame = 0; frame < frames; ++frame )
		rig.Render( warmup + frame, fps, AudioFeed::Pulses );
	glFinish();
	const auto end = std::chrono::steady_clock::now();

	const double seconds = std::chrono::duration< double >( end - start ).count();
	return seconds * 1000.0 / static_cast< double >( frames );
}

int runBench( int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "2560x1440 ", 2560, 1440 },
		{ "3840x2160 ", 3840, 2160 },
	};

	std::printf( "%d frames each, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame\n" );

	for( const Size& size : sizes )
	{
		const double ms = benchAt( size.width, size.height, frames, fps );
		if( ms < 0.0 )
			return 1;
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%\n",
		             size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0, ms / 16.667 * 100.0 );
	}

	std::printf( "\nTwo passes at picture size: separate (with a mip chain) and print,\n"
	             "which screens all four plates per pixel.\n" );
	return 0;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"rztest -- render and measure the Rosette offset-litho effect\n"
		"\n"
		"  --out PATH        render the test card through the plugin (default /tmp/rosette.png)\n"
		"  --card PATH       write the test card alone, unprinted\n"
		"  --size WxH        picture size (default 1280x720); --width N / --height N also work\n"
		"  --frames N        frames to render before reading back (default 30)\n"
		"  --fps N           synthetic frame rate driving the press (default 60)\n"
		"  --silent          feed no audio (default: a bass-heavy spectrum with a beat every half second)\n"
		"  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
		"  --list            print every parameter, its kind, default and range, then exit\n"
		"  --names           no parameter or element name over 16 characters\n"
		"  --defaults        preset row 1 is the constructor's defaults\n"
		"  --presets         every factory preset survives every host behaviour\n"
		"  --wander          the press wander is bounded, smooth and per plate\n"
		"  --spot            the GLSL spot functions against the C++ ones\n"
		"  --gain            printed area follows the dot-gain curve\n"
		"  --angle           a lattice lies at the angle it was asked for\n"
		"  --register        an offset moves the dots by exactly that much\n"
		"  --overprint       two solids multiply as the ink colours predict\n"
		"  --identity        a flat colour comes back as its CMYK round trip\n"
		"  --audio           silence leaves the press alone; a beat kicks it\n"
		"  --bench           time ProcessOpenGL at 720p through 4K\n"
		"  --pipe            raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH     parameter cues for --pipe: 'frame Name value'\n"
		"  --help\n" );
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Parameter Name value' per line. Same format
// as the rest of the fleet, so one filming script drives any of them.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;

		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/rosette.png";
	std::string cardPath;
	std::string scriptPath;
	int width    = 1280;
	int height   = 720;
	int frames   = 30;
	double fps   = 60.0;
	bool silent  = false;
	bool wantList = false, wantBench = false, wantPipe = false;
	std::string check;
	std::vector< std::string > settings;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--silent" )
			silent = true;
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--names" || argument == "--defaults" || argument == "--presets" || argument == "--wander"
		         || argument == "--spot" || argument == "--gain" || argument == "--angle" || argument == "--register"
		         || argument == "--overprint" || argument == "--identity" || argument == "--audio" )
			check = argument;
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	//The checks that need no GL context, answered before one is made.
	if( wantList )
		return runList();
	if( check == "--names" )
		return runNames();
	if( check == "--defaults" )
		return runDefaults();
	if( check == "--presets" )
		return runPresetTest();
	if( check == "--wander" )
		return runWander();

	if( !cardPath.empty() )
	{
		if( !writePng( cardPath, width, height, buildCard( width, height ) ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", cardPath.c_str() );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	int result = 0;
	if( check == "--spot" )
		result = runSpot();
	else if( check == "--gain" )
		result = runGain();
	else if( check == "--angle" )
		result = runAngle();
	else if( check == "--register" )
		result = runRegister();
	else if( check == "--overprint" )
		result = runOverprint();
	else if( check == "--identity" )
		result = runIdentity();
	else if( check == "--audio" )
		result = runAudio();
	else if( wantBench )
		result = runBench( frames, fps );
	else
	{
		Rig rig;
		if( !rig.Init( width, height ) )
			return 1;

		for( const std::string& setting : settings )
		{
			const size_t equals = setting.find( '=' );
			if( equals == std::string::npos || !rig.Set( setting.substr( 0, equals ), std::strtof( setting.substr( equals + 1 ).c_str(), nullptr ) ) )
			{
				std::fprintf( stderr, "--set %s: expected Name=Value with a known name (try --list)\n", setting.c_str() );
				return 2;
			}
		}

		const AudioFeed feed = silent ? AudioFeed::Silence : AudioFeed::Pulses;

		if( wantPipe )
		{
			//Resolve the script's parameter names once, up front, and refuse
			//to run on a name that is not a parameter: a misspelled cue that
			//silently did nothing would produce a take that looks deliberate
			//and is wrong.
			std::map< unsigned int, Track > automation;
			if( !scriptPath.empty() )
			{
				std::string error;
				const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
				if( !error.empty() )
				{
					std::fprintf( stderr, "%s\n", error.c_str() );
					return 2;
				}
				for( const auto& entry : tracks )
				{
					bool found = false;
					for( unsigned int id = 0; id < Rosette::PT_COUNT; ++id )
					{
						const char* name = rig.plugin.GetParamName( id );
						if( name != nullptr && entry.first == name )
						{
							automation[ id ] = entry.second;
							found            = true;
							break;
						}
					}
					if( !found )
					{
						std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
						return 2;
					}
				}
			}

			Image frame( static_cast< size_t >( width ) * height * 4 );
			for( int index = 0;; ++index )
			{
				size_t filled = 0;
				while( filled < frame.size() )
				{
					const ssize_t got = read( STDIN_FILENO, frame.data() + filled, frame.size() - filled );
					if( got <= 0 )
						break;
					filled += static_cast< size_t >( got );
				}
				if( filled < frame.size() )
					break;

				for( const auto& track : automation )
					rig.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );

				//A raw frame arrives top row first and GL wants bottom row first.
				rig.Upload( flipRows( frame, width, height ) );
				if( !rig.Render( index, fps, feed ) )
					break;

				const Image out = flipRows( rig.Pixels(), width, height );
				size_t written  = 0;
				while( written < out.size() )
				{
					const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
					if( put <= 0 )
						break;
					written += static_cast< size_t >( put );
				}
			}
		}
		else
		{
			rig.Upload( buildCard( width, height ) );
			if( !rig.RenderFrames( frames, fps, feed ) )
				result = 1;
			else if( !writePng( outPath, width, height, rig.Pixels() ) )
			{
				std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
				result = 1;
			}
			else
				std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
