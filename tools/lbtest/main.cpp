/**
	lbtest -- render Lowband offline, and measure the deck out of it.

	Every check here drives the REAL plugin class through a headless GL
	context on a synthetic clock (or the real engine the plugin runs, for the
	checks that need the RF), and measures the property it claims against a
	prediction derived here from the formats' stated numbers by a different
	route from the one the plugin takes:

		lbtest --out /tmp/frame.png     a picture, on the moving test card
		lbtest --list                   every parameter, its kind and default
		(the checks: see usage())
		lbtest --negative               every check can FAIL
		lbtest --names --model          the checks that need no GL
		lbtest --bench                  the render cost
		lbtest --dump-shaders DIR       the exact GLSL the plugin compiles
		lbtest --pipe                   raw frames in, raw frames out

	LBTEST_RENDERER=software runs any of it on Apple's software renderer (what
	a GPU-less CI runner has). AGENTS.md has one line per check on where each
	tolerance comes from.
*/

#include "Controls.h"
#include "Dsp.h"
#include "Engine.h"
#include "Lowband.h"
#include "Model.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
namespace model    = lowband::model;
namespace controls = lowband::controls;
namespace dsp      = lowband::dsp;

int g_checks   = 0;
int g_failures = 0;

constexpr double kU  = 5.9604644775390625e-8;//2^-24, half a float ulp at 1
constexpr double kPi = 3.14159265358979323846;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
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

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
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
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
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
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
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
	//LBTEST_RENDERER=software asks for Apple's software renderer by id, on a
	//Mac that has a GPU. It is what a GPU-less CI runner falls back to, and it
	//is not bit-repeatable frame to frame (repousse's resize check failed CI
	//by one ulp), so a check that would fail only in CI can be run here first.
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "LBTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		std::fprintf( stderr, "lbtest: LBTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
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

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
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

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}


//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Lowband::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Lowband& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Lowband::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		//An integer carries a real range.
		if( p.type == FF_TYPE_INTEGER )
		{
			const RangeStruct range = plugin.GetParamRange( i );
			p.low                   = range.min;
			p.high                  = range.max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Lowband& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Lowband& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Lowband& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

//---------------------------------------------------------------------------
// BT.601 Y' and the U, V scalings, both ways, in double.
//---------------------------------------------------------------------------
constexpr double kUs = 0.492111, kVs = 0.877283;

void yuvToRgb( double y, double u, double v, float* rgb )
{
	const double r = y + v / kVs, b = y + u / kUs;
	const double g = ( y - 0.299 * r - 0.114 * b ) / 0.587;
	rgb[ 0 ]       = static_cast< float >( r );
	rgb[ 1 ]       = static_cast< float >( g );
	rgb[ 2 ]       = static_cast< float >( b );
}

void rgbToYuv( const float* c, double& y, double& u, double& v )
{
	y = 0.299 * c[ 0 ] + 0.587 * c[ 1 ] + 0.114 * c[ 2 ];
	u = kUs * ( c[ 2 ] - y );
	v = kVs * ( c[ 0 ] - y );
}

//---------------------------------------------------------------------------
// Pictures, float RGBA, top-first.
//---------------------------------------------------------------------------
using Picture = std::vector< float >;

Picture yuvPicture( int W, int H, const std::function< void( int, int, double&, double&, double& ) >& at )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( int r = 0; r < H; ++r )
		for( int x = 0; x < W; ++x )
		{
			double y = 0.5, u = 0.0, v = 0.0;
			at( x, r, y, u, v );
			float* px = p.data() + ( static_cast< size_t >( r ) * W + x ) * 4;
			yuvToRgb( y, u, v, px );
			px[ 3 ] = 1.0f;
		}
	return p;
}

Picture flat( int W, int H, double level )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( size_t i = 0; i < p.size(); i += 4 )
	{
		p[ i ] = p[ i + 1 ] = p[ i + 2 ] = static_cast< float >( level );
		p[ i + 3 ]                       = 1.0f;
	}
	return p;
}

/// The deck a check runs: the controls it moves, everything else at a
/// stated value (no noise, no dropouts, no clog unless the check asks).
struct Knobs
{
	int recording   = model::kHi8;
	int deck        = model::kVideo8;
	int standard    = model::kPAL;
	double cnrDb    = -1.0;///< < 0: Tape Noise 0, no noise
	double dropouts = 0.0;
	double clogUm   = 0.0;
	bool syncAgc    = false;
	double mix      = 1.0;
};

void apply( Lowband& p, const Knobs& k )
{
	set( p, "Recording", static_cast< float >( k.recording ) );
	set( p, "Deck", static_cast< float >( k.deck ) );
	set( p, "Standard", static_cast< float >( k.standard ) );
	set( p, "Tape Noise", k.cnrDb < 0.0 ? 0.0f : controls::CnrParam( k.cnrDb ) );
	set( p, "Dropouts", static_cast< float >( k.dropouts ) );
	set( p, "Head Clog", controls::ClogParam( k.clogUm * 1e-6 ) );
	set( p, "Sync AGC", k.syncAgc ? 1.0f : 0.0f );
	set( p, "Mix", static_cast< float >( k.mix ) );
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock that drives it.
//---------------------------------------------------------------------------
struct Session
{
	Lowband plugin;
	int width  = 0;
	int height = 0;
	double fps = 60.0;
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip or the composition changes size: hand
	/// the SAME instance a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	void upload( const Picture& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void upload( const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	/// A synthetic clock, and it has to be synthetic: frame n is clocked at
	/// n / fps seconds, the unit declared, not inferred.
	bool renderAt( long frame )
	{
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( static_cast< double >( frame ) / fps );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %ld\n", frame );
		return ok;
	}

	template< typename P >
	bool render( long frame, const P& pixels )
	{
		upload( pixels );
		return renderAt( frame );
	}

	/// A row from the TOP, RGBA floats.
	std::vector< float > readRow( int y )
	{
		std::vector< float > row( static_cast< size_t >( width ) * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, height - 1 - y, width, 1, GL_RGBA, GL_FLOAT, row.data() );
		return row;
	}

	/// The whole picture, top first, RGBA floats.
	std::vector< float > readAll()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return flipRows( pixels, width, height );
	}

	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

/// A check's session: the knobs, the perturbation, noise on or forced off.
bool open( Session& s, int W, int H, const Knobs& k, int perturb, bool quietNoise = false )
{
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	s.plugin.SetQuietForTest( quietNoise );
	return s.begin( W, H );
}

/// Least squares: v( x ) ~ c0 + c1 cos( w ( x + 1/2 ) ) + c2 sin( w ( x + 1/2 ) )
/// over [ a, b ). Returns the amplitude and the phase phi of A cos( w x' + phi ).
void fitSinusoid( const std::vector< double >& v, int a, int b, double w, double& amplitude, double& phase )
{
	double M[ 3 ][ 3 ] = {}, r[ 3 ] = {};
	for( int x = a; x < b; ++x )
	{
		const double f[ 3 ] = { 1.0, std::cos( w * ( x + 0.5 ) ), std::sin( w * ( x + 0.5 ) ) };
		for( int i = 0; i < 3; ++i )
		{
			r[ i ] += f[ i ] * v[ static_cast< size_t >( x ) ];
			for( int j = 0; j < 3; ++j )
				M[ i ][ j ] += f[ i ] * f[ j ];
		}
	}
	//Gaussian elimination, 3 x 3.
	for( int i = 0; i < 3; ++i )
	{
		int pivot = i;
		for( int j = i + 1; j < 3; ++j )
			if( std::fabs( M[ j ][ i ] ) > std::fabs( M[ pivot ][ i ] ) )
				pivot = j;
		std::swap( M[ i ], M[ pivot ] );
		std::swap( r[ i ], r[ pivot ] );
		for( int j = i + 1; j < 3; ++j )
		{
			const double f = M[ j ][ i ] / M[ i ][ i ];
			for( int c = i; c < 3; ++c )
				M[ j ][ c ] -= f * M[ i ][ c ];
			r[ j ] -= f * r[ i ];
		}
	}
	double c[ 3 ];
	for( int i = 2; i >= 0; --i )
	{
		double s = r[ i ];
		for( int j = i + 1; j < 3; ++j )
			s -= M[ i ][ j ] * c[ j ];
		c[ i ] = s / M[ i ][ i ];
	}
	amplitude = std::hypot( c[ 1 ], c[ 2 ] );
	phase     = std::atan2( -c[ 2 ], c[ 1 ] );
}

/// Y' or U of a top-first output row.
std::vector< double > channelOf( const std::vector< float >& row, int W, int channel )
{
	std::vector< double > out( static_cast< size_t >( W ) );
	for( int x = 0; x < W; ++x )
	{
		double y, u, v;
		rgbToYuv( row.data() + 4 * x, y, u, v );
		out[ static_cast< size_t >( x ) ] = channel == 0 ? y : channel == 1 ? u : v;
	}
	return out;
}

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAIL";
}

int report( bool ok, bool quiet, const char* format, ... ) __attribute__( ( format( printf, 3, 4 ) ) );
int report( bool ok, bool quiet, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	//Quiet is a negative control's run: its failures are the point, and the
	//summary line says so. --perturb runs the same thing verbosely.
	if( quiet )
		return ok ? 0 : 1;
	va_list args;
	va_start( args, format );
	std::printf( "   %-4s ", verdict( ok ) );
	std::vprintf( format, args );
	std::printf( "\n" );
	va_end( args );
	return ok ? 0 : 1;
}

void note( bool quiet, const char* format, ... ) __attribute__( ( format( printf, 2, 3 ) ) );
void note( bool quiet, const char* format, ... )
{
	if( quiet )
		return;
	va_list args;
	va_start( args, format );
	std::printf( "        " );
	std::vprintf( format, args );
	std::printf( "\n" );
	va_end( args );
}

//---------------------------------------------------------------------------
// The moving card, for --out, the sweep and the bench: a row of colour
// patches, a grey ramp, a white disc on an orbit, a static black square, a
// drifting blue bar, and a flashing patch (on for 6 frames in 30).
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int64_t frame )
{
	std::vector< unsigned char > img( static_cast< size_t >( width ) * height * 4 );
	const double t  = static_cast< double >( frame ) / 60.0;
	const double cx = 0.72 + 0.14 * std::cos( 1.2 * t ), cy = 0.68 + 0.16 * std::sin( 1.2 * t );
	const double barX = std::fmod( 0.05 * t, 1.0 );
	const bool flash  = frame % 30 < 6;
	const double patches[ 8 ][ 3 ] = {
		{ 0.80, 0.10, 0.10 }, { 0.88, 0.67, 0.55 }, { 0.90, 0.85, 0.15 }, { 0.15, 0.60, 0.20 },
		{ 0.20, 0.80, 0.85 }, { 0.15, 0.25, 0.85 }, { 0.53, 0.81, 0.92 }, { 0.95, 0.95, 0.95 },
	};
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double fx = ( x + 0.5 ) / width, fy = ( y + 0.5 ) / height;
			double r = 0.40, g = 0.40, b = 0.40;
			if( fy > 0.06 && fy < 0.30 )
			{
				const int i = std::clamp( static_cast< int >( ( fx - 0.04 ) / 0.115 ), 0, 7 );
				if( fx > 0.04 && fx < 0.96 && std::fmod( fx - 0.04, 0.115 ) < 0.105 )
				{
					r = patches[ i ][ 0 ];
					g = patches[ i ][ 1 ];
					b = patches[ i ][ 2 ];
				}
			}
			if( fy > 0.36 && fy < 0.46 && fx > 0.04 && fx < 0.96 )
				r = g = b = ( fx - 0.04 ) / 0.92;
			if( fx > 0.08 && fx < 0.28 && fy > 0.56 && fy < 0.92 )
				r = g = b = flash ? 0.98 : 0.02;
			const double dx = ( fx - cx ) * width, dy = ( fy - cy ) * height;
			if( dx * dx + dy * dy < ( height * 0.08 ) * ( height * 0.08 ) )
				r = g = b = 0.98;
			if( std::fabs( fx - barX ) < 0.02 && fy > 0.5 )
			{
				r = 0.2;
				g = 0.3;
				b = 0.9;
			}
			unsigned char* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ]           = static_cast< unsigned char >( std::lround( 255.0 * r ) );
			px[ 1 ]           = static_cast< unsigned char >( std::lround( 255.0 * g ) );
			px[ 2 ]           = static_cast< unsigned char >( std::lround( 255.0 * b ) );
			px[ 3 ]           = 255;
		}
	return img;
}

//===========================================================================
// THE CHECKS
//===========================================================================

struct Check
{
	const char* flag;
	int ( *run )( int, int, int, bool );
	const char* help;
	int negativeBits;      ///< the perturbation that must fail it (0: none)
	const char* negativeWhat;
	bool gl;               ///< needs a GL context (the rest drive the engine alone)
};

//---------------------------------------------------------------------------
// The formats and standards as the sources state them, typed HERE
// (ATTRIBUTIONS.md), never read out of the plugin: a constant typed wrong in
// Model.cpp has to show up as a failed check, not as an agreement.
//---------------------------------------------------------------------------
struct StatedFormat
{
	const char* name;
	double tipMHz, devMHz;  ///< Sencore Tech Tip 189
	double tauUs, xDb;      ///< vhs-decode format_defs/video8.py
	double rfHighMHz, rfLowMHz, yLowMHz;
};
const StatedFormat kStatedFormats[ 2 ] = {
	{ "Video8", 4.2, 1.2, 1.30, 11.5794, 1.9, 7.0, 3.5 },
	{ "Hi8", 5.7, 2.0, 0.47, 11.5794, 1.85, 10.3, 5.0 },
};

struct StatedStandard
{
	const char* name;
	double lineUs, front, sync, back;///< BT.470-6 / SMPTE 170M
	int lines;
	double S, Wh;                    ///< volts below / above blanking
	double fps, fH, underFh;         ///< frame rate, line rate, colour-under carrier in fH
};
const StatedStandard kStatedStandards[ 2 ] = {
	{ "PAL", 64.0, 1.65, 4.7, 5.7, 576, 0.3, 0.7, 25.0, 15625.0, 46.875 },
	{ "NTSC", 1001.0 / 15.75, 1.5, 4.7, 4.7, 480, 40.0 / 140.0, 100.0 / 140.0, 30000.0 / 1001.0, 15750000.0 / 1001.0, 47.25 },
};

constexpr double kFs = 40.5e6;

double shelfX( const StatedFormat& f )
{
	return std::pow( 10.0, f.xDb / 20.0 );
}

/// The carrier, Hz, for a level in volts above blanking.
double carrierHz( const StatedFormat& f, const StatedStandard& st, double volts )
{
	return ( f.tipMHz + ( volts + st.S ) / ( st.S + st.Wh ) * f.devMHz ) * 1e6;
}

/// |H| of the deck's RF band (the analogue Butterworth prototypes).
double rfGain( const StatedFormat& deck, double fHz, double lowMHz = -1.0 )
{
	const double lo = lowMHz > 0.0 ? lowMHz : deck.rfLowMHz;
	return std::abs( dsp::AnalogueButterworth( 2, deck.rfHighMHz * 1e6, fHz, true ) * dsp::AnalogueButterworth( 8, lo * 1e6, fHz, false ) );
}

/// The worst residual of the 2f carrier after the demodulator's 8th-order
/// low-pass, in Y' (fraction of white), over carriers [ fLo, fHi ]: the
/// fundamental 2f has the amplitude f of the mean (a pulse train's
/// harmonics), attenuated by the low-pass at 2 fLo, then x^-1 by the
/// de-emphasis, and mapped by the deck's volts per Hz.
double rippleBound( const StatedFormat& deck, const StatedStandard& st, double fLo, double fHi )
{
	const double h = std::abs( dsp::AnalogueButterworth( 8, deck.yLowMHz * 1e6, 2.0 * fLo, false ) );
	return 2.0 * fHi * h / shelfX( deck ) / ( deck.devMHz * 1e6 ) * ( st.S + st.Wh ) / st.Wh;
}

int samplesOf( double us )
{
	return static_cast< int >( std::lround( us * kFs * 1e-6 ) );
}

/// The deck's picture as the plugin uploaded it (Y', U, V, 1), line 0 first.
struct Deck
{
	std::vector< float > lines;
	int P = 0, N = 0;
	double y( int line, int pixel ) const
	{
		return lines[ ( static_cast< size_t >( line ) * P + pixel ) * 4 ];
	}
	/// Mean Y' over lines [ l0, l1 ) and pixels [ p0, p1 ).
	double mean( int l0, int l1, int p0, int p1 ) const
	{
		double s = 0.0;
		for( int l = l0; l < l1; ++l )
			for( int p = p0; p < p1; ++p )
				s += y( l, p );
		return s / ( static_cast< double >( l1 - l0 ) * ( p1 - p0 ) );
	}
};

Deck renderDeck( Session& s, long frame, const Picture& picture )
{
	Deck d;
	if( !s.render( frame, picture ) )
		return d;
	d.lines = s.plugin.LinesForTest();
	const model::Standard& st = model::StandardOf( s.plugin.LastSettingsForTest().standard );
	d.P                       = st.ActivePixels();
	d.N                       = st.frameLines;
	return d;
}

/// Raster pixel of the time t us after the active picture's start.
int pixelAt( double us )
{
	return static_cast< int >( std::lround( us * 13.5 - 1.0 / 3.0 ) );
}

/// A grey picture, left part at a, right part at b, split at a fraction of
/// the width.
Picture twoLevel( int W, int H, double a, double b, double split )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	const int xs = static_cast< int >( std::lround( split * W ) );
	for( int r = 0; r < H; ++r )
		for( int x = 0; x < W; ++x )
		{
			float* px = p.data() + ( static_cast< size_t >( r ) * W + x ) * 4;
			px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( x < xs ? a : b );
			px[ 3 ]                     = 1.0f;
		}
	return p;
}

/// As twoLevel, with the split moved right by ( block % 8 ) host pixels for
/// each block of 16 host rows: the edge meets the 40.5 MHz grid at eight
/// different phases.
Picture twoLevelStaggered( int W, int H, double a, double b, double split )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	const int base = static_cast< int >( std::lround( split * W ) );
	for( int r = 0; r < H; ++r )
	{
		const int xs = base + ( r / 16 ) % 8;
		for( int x = 0; x < W; ++x )
		{
			float* px = p.data() + ( static_cast< size_t >( r ) * W + x ) * 4;
			px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( x < xs ? a : b );
			px[ 3 ]                     = 1.0f;
		}
	}
	return p;
}

//---------------------------------------------------------------------------
// --carrier: the recorded FM's frequency, by counting crossings.
//---------------------------------------------------------------------------
/// Frequency over samples [ i0, i1 ) of an RF line: ( crossings - 1 ) / 2
/// over the time from the first crossing to the last, each placed by linear
/// interpolation.
double countedHz( const std::vector< float >& rf, int i0, int i1 )
{
	double first = -1.0, last = -1.0;
	int crossings = 0;
	for( int i = std::max( 1, i0 ); i < i1; ++i )
	{
		const double x0 = rf[ static_cast< size_t >( i - 1 ) ], x1 = rf[ static_cast< size_t >( i ) ];
		if( ( x0 < 0.0 ) != ( x1 < 0.0 ) )
		{
			const double t = ( i - 1 ) + x0 / ( x0 - x1 );
			if( first < 0.0 )
				first = t;
			last = t;
			++crossings;
		}
	}
	if( crossings < 3 )
		return 0.0;
	return ( crossings - 1 ) / 2.0 / ( ( last - first ) / kFs );
}

/// The worst error, in samples, of a linearly interpolated zero crossing of
/// a sinusoid at f sampled at 40.5 MHz, over every phase of the samples.
double linearCrossingError( double fHz )
{
	const double h = 2.0 * kPi * fHz / kFs;//radians a sample
	double worst   = 0.0;
	for( int k = 0; k <= 1000; ++k )
	{
		//The crossing at theta = 0, the sample before it at -u h.
		const double u  = k / 1000.0;
		const double s0 = std::sin( -u * h ), s1 = std::sin( ( 1.0 - u ) * h );
		const double t  = s0 / ( s0 - s1 );//estimate of u
		worst           = std::max( worst, std::fabs( t - u ) );
	}
	return worst;
}

int runCarrier( int, int, int perturb, bool quiet )
{
	if( !quiet )
		std::printf( "carrier: the recorded FM at the sync tip, blanking and peak white, by counting crossings\n" );
	int failures = 0;
	lowband::Engine engine;
	for( int si = 0; si < 2; ++si )
		for( int fi = 0; fi < 2; ++fi )
		{
			const StatedStandard& st = kStatedStandards[ si ];
			const StatedFormat& f    = kStatedFormats[ fi ];
			lowband::EngineSettings s;
			s.standard  = si;
			s.recording = fi;
			s.deck      = fi;
			s.noise     = false;
			s.perturb   = perturb;
			const model::Standard& ms = model::StandardOf( si );
			const int a0             = ms.ActiveStart();
			//Settled: the pre-emphasis's step response is 1 + ( x - 1 ) e^{-t x / tau},
			//so the residual after t0 is at most ( x - 1 ) e^{-t0 x / tau} of the step.
			auto settle = [ & ]( double t0us, double stepV ) {
				return ( shelfX( f ) - 1.0 ) * std::exp( -t0us * shelfX( f ) / f.tauUs ) * stepV * f.devMHz * 1e6 / ( st.S + st.Wh );
			};
			const std::vector< float > white = engine.RecordFlatForTest( s, 1.0 );
			const std::vector< float > black = engine.RecordFlatForTest( s, 0.0 );
			const double tip   = countedHz( black, samplesOf( 2.0 ), samplesOf( 4.5 ) );
			const double blank = countedHz( black, a0 + samplesOf( 10.0 ), a0 + samplesOf( 40.0 ) );
			const double wh    = countedHz( white, a0 + samplesOf( 10.0 ), a0 + samplesOf( 40.0 ) );
			const double wantTip = carrierHz( f, st, -st.S ), wantBlank = carrierHz( f, st, 0.0 ), wantWhite = carrierHz( f, st, st.Wh );
			//The count's own error: the first and last crossings each placed to
			//within the linear interpolation's worst error, over the window.
			auto counting = [ & ]( double fHz, double us ) {
				return 2.0 * linearCrossingError( fHz ) / ( us * kFs * 1e-6 ) * fHz;
			};
			const double tolTip   = settle( 2.0, st.S ) + counting( wantTip, 2.5 ) + 1e-6 * wantTip;
			const double tolBlank = settle( 10.0, st.Wh ) + counting( wantBlank, 30.0 ) + 1e-6 * wantBlank;
			const double tolLine  = settle( 10.0, st.Wh ) + counting( wantWhite, 30.0 ) + 1e-6 * wantWhite;
			failures += report( std::fabs( tip - wantTip ) <= tolTip, quiet, "%s %-6s sync tip    %.4f MHz (stated %.4f, +-%.4f)", st.name, f.name, tip / 1e6, wantTip / 1e6, tolTip / 1e6 );
			failures += report( std::fabs( blank - wantBlank ) <= tolBlank, quiet, "%s %-6s blanking   %.4f MHz (stated %.4f, +-%.4f)", st.name, f.name, blank / 1e6, wantBlank / 1e6, tolBlank / 1e6 );
			failures += report( std::fabs( wh - wantWhite ) <= tolLine, quiet, "%s %-6s peak white %.4f MHz (stated %.4f, +-%.4f)", st.name, f.name, wh / 1e6, wantWhite / 1e6, tolLine / 1e6 );
		}
	return failures;
}

//---------------------------------------------------------------------------
// --matched: a deck playing its own format reproduces a flat field.
//---------------------------------------------------------------------------
int runMatched( int W, int H, int perturb, bool quiet )
{
	if( !quiet )
		std::printf( "matched: a deck playing its own format gives back the level it was given, %dx%d\n", W, H );
	int failures = 0;
	for( int si = 0; si < 2; ++si )
		for( int fi = 0; fi < 2; ++fi )
		{
			const StatedStandard& st = kStatedStandards[ si ];
			const StatedFormat& f    = kStatedFormats[ fi ];
			Session s;
			Knobs k;
			k.standard  = si;
			k.recording = fi;
			k.deck      = fi;
			if( !open( s, W, H, k, perturb, true ) )
				return failures + 1;
			//Levels the clips cannot reach: the pre-emphasis's overshoot at the
			//picture's start, x 0.7 l above blanking, stays under the white
			//clip (1.9 V) and its undershoot at the end, ( x - 1 ) 0.7 l,
			//above the dark clip (0.9 x 0.7 V) for l <= 0.3.
			const double levels[] = { 0.05, 0.15, 0.25 };
			const double tol      = rippleBound( f, st, carrierHz( f, st, 0.0 ), carrierHz( f, st, st.Wh ) ) + 1e-4;
			for( double level : levels )
			{
				const Deck d   = renderDeck( s, 10, flat( W, H, level ) );
				const double m = d.mean( d.N / 4, 3 * d.N / 4, pixelAt( 10.0 ), d.P - pixelAt( 2.0 ) );
				failures += report( std::fabs( m - level ) <= tol, quiet, "%s %-6s on its own deck: level %.2f reads %.5f (+-%.5f, the 2f residual)", st.name, f.name, level, m, tol );
			}
			s.end();
		}
	return failures;
}

//---------------------------------------------------------------------------
// --gain: Hi8 on Video8 has the deviation ratio for a gain.
//---------------------------------------------------------------------------
/// The step from a to b at the middle of the line: the mean of the left part
/// (10 us after the picture starts, to 2 us before the split) and of the
/// right (10 us after the split, to 2 us before the end).
void stepMeans( const Deck& d, const StatedStandard& st, double& left, double& right )
{
	const double active = st.lineUs - st.front - st.sync - st.back;
	const double split  = active / 2.0;
	left                = d.mean( d.N / 4, 3 * d.N / 4, pixelAt( 10.0 ), pixelAt( split - 2.0 ) );
	right               = d.mean( d.N / 4, 3 * d.N / 4, pixelAt( split + 10.0 ), pixelAt( active - 2.0 ) );
}

int runGain( int W, int H, int perturb, bool quiet )
{
	if( !quiet )
		std::printf( "gain: Hi8 on a Video8 deck has a gain of 2.0 / 1.2 MHz; a matched deck 1, %dx%d\n", W, H );
	int failures = 0;
	struct Case
	{
		int rec, deck;
	};
	const Case cases[] = { { model::kHi8, model::kVideo8 }, { model::kVideo8, model::kVideo8 }, { model::kHi8, model::kHi8 } };
	for( int si = 0; si < 2; ++si )
		for( const Case& c : cases )
		{
			const StatedStandard& st = kStatedStandards[ si ];
			const StatedFormat& tape = kStatedFormats[ c.rec ];
			const StatedFormat& deck = kStatedFormats[ c.deck ];
			Session s;
			Knobs k;
			k.standard  = si;
			k.recording = c.rec;
			k.deck      = c.deck;
			if( !open( s, W, H, k, perturb, true ) )
				return failures + 1;
			const double a = 0.05, b = 0.25;
			const Deck d   = renderDeck( s, 10, twoLevel( W, H, a, b, 0.5 ) );
			double left, right;
			stepMeans( d, st, left, right );
			const double gain = ( right - left ) / ( b - a );
			const double want = tape.devMHz / deck.devMHz;
			//Each region's mean is off by at most the 2f residual, and by what
			//is left of the slowest tail 10 us after the edge (the deck's
			//de-emphasis, 1.3 us at the slowest: e^-7.7 of the step).
			const double ripple = rippleBound( deck, st, carrierHz( tape, st, 0.0 ), carrierHz( tape, st, st.Wh ) );
			const double tail   = std::exp( -10.0 / std::max( tape.tauUs, deck.tauUs ) ) * want;
			const double tol    = ( 2.0 * ripple + tail ) / ( b - a ) + 1e-4;
			failures += report( std::fabs( gain - want ) <= tol, quiet, "%s %-6s on %-6s: gain %.5f (stated %.5f = %.1f / %.1f, +-%.5f)", st.name, tape.name, deck.name, gain, want, tape.devMHz, deck.devMHz, tol );
			s.end();
		}
	return failures;
}

//---------------------------------------------------------------------------
// --emphasis: the mismatched pair leaves the deck's own time constant.
//---------------------------------------------------------------------------
int runEmphasis( int W, int H, int perturb, bool quiet )
{
	if( !quiet )
		std::printf( "emphasis: Hi8's pre-emphasis under Video8's de-emphasis leaves a tail with Video8's 1.3 us, %dx%d\n", W, H );
	int failures = 0;
	struct Case
	{
		int rec, deck;
	};
	const Case cases[] = { { model::kHi8, model::kVideo8 }, { model::kVideo8, model::kVideo8 }, { model::kHi8, model::kHi8 } };
	for( const Case& c : cases )
	{
		const StatedStandard& st = kStatedStandards[ 0 ];
		const StatedFormat& tape = kStatedFormats[ c.rec ];
		const StatedFormat& deck = kStatedFormats[ c.deck ];
		Session s;
		Knobs k;
		k.recording = c.rec;
		k.deck      = c.deck;
		if( !open( s, W, H, k, perturb, true ) )
			return failures + 1;
		const double a = 0.05, b = 0.25;
		//The edge staggered over eight phases of the sample grid: the
		//demodulator places each crossing to a few 1e-4 of a sample, in a
		//pattern that repeats as the crossings slide past the grid (0.6 MHz
		//for this step's 6.65 MHz carrier, +-0.1 % of white). Averaged over
		//the eight it cancels; the tail is a sum of exponentials of one tau,
		//so the average is one too. Measured from the LAST split.
		const Deck d = renderDeck( s, 10, twoLevelStaggered( W, H, a, b, 0.5 ) );
		double left, right;
		stepMeans( d, st, left, right );
		const double active = st.lineUs - st.front - st.sync - st.back;
		const double split  = active / 2.0 + 7.0 * d.P / W / 13.5;
		//The residual r(t) = y(t) - y(final), averaged down the whole frame,
		//from 2.5 to 5 us after the split. The faster poles (the
		//pre-emphasis's tau / x, 0.12 us; the low-passes, 0.23 us at the
		//slowest) are gone by then; and so is most of the FM channel's own
		//stretch: the carrier climbs through the tail, near Video8's band edge
		//the band's group delay climbs with it, and early in the tail that
		//draws the curve out (a fit from 1.5 us read 1.34 us).
		std::vector< double > ts, rs;
		const double fitFrom = 2.5, fitTo = 5.0;
		for( int p = pixelAt( split + fitFrom ); p <= pixelAt( split + fitTo ); ++p )
		{
			ts.push_back( ( p + 1.0 / 3.0 ) / 13.5 );
			rs.push_back( d.mean( 0, d.N, p, p + 1 ) - right );
		}
		const double ripple = rippleBound( deck, st, carrierHz( tape, st, 0.0 ), carrierHz( tape, st, st.Wh ) );
		const double step   = right - left;
		const bool mismatch = c.rec != c.deck;
		auto fitTau = [ & ]( double bias ) {
			//Least squares on ln | r + bias |: slope -1 / tau.
			double sx = 0, sy = 0, sxx = 0, sxy = 0;
			const double n = static_cast< double >( ts.size() );
			for( size_t i = 0; i < ts.size(); ++i )
			{
				const double v = std::log( std::max( 1e-12, std::fabs( rs[ i ] + bias ) ) );
				sx += ts[ i ];
				sy += v;
				sxx += ts[ i ] * ts[ i ];
				sxy += ts[ i ] * v;
			}
			const double slope = ( n * sxy - sx * sy ) / ( n * sxx - sx * sx );
			return -1.0 / slope;
		};
		if( mismatch )
		{
			const bool tail = std::fabs( rs.front() ) > 10.0 * ripple;
			failures += report( tail, quiet, "%s on %s: the step has a slow tail, %.4f of it left %.1f us after the edge (the 2f residual is %.5f)", tape.name, deck.name, std::fabs( rs.front() ) / step,
			                    fitFrom, ripple / step );
			const double tau = tail ? fitTau( 0.0 ) : 0.0;
			//The tolerance: refit with every residual moved by the 2f bound
			//both ways; plus the band's group-delay swing over the window (the
			//carrier at the window's start against the final one), as a
			//fraction of the window, of tau; plus 1e-3 for the warp of a
			//122 kHz pole at 40.5 MHz (1e-5) and the rows' averaging.
			const double spread = tail ? std::max( std::fabs( fitTau( ripple ) - tau ), std::fabs( fitTau( -ripple ) - tau ) ) : 0.0;
			const dsp::Cascade rfBand = dsp::ButterworthHighPass( 2, deck.rfHighMHz * 1e6, kFs ).Then( dsp::ButterworthLowPass( 8, deck.rfLowMHz * 1e6, kFs ) );
			const double tapeV  = ( 0.25 * st.Wh - 0.05 * st.Wh );
			const double fFinal = carrierHz( tape, st, 0.25 * st.Wh );
			const double fStart = fFinal - std::fabs( rs.front() ) / step * tapeV * tape.devMHz * 1e6 / ( st.S + st.Wh );
			const double swing  = std::fabs( rfBand.GroupDelay( 2.0 * kPi * fFinal / kFs ) - rfBand.GroupDelay( 2.0 * kPi * fStart / kFs ) ) / kFs * 1e6;
			const double tol    = spread + swing / ( fitTo - fitFrom ) * deck.tauUs + 1e-3 * deck.tauUs;
			failures += report( tail && std::fabs( tau - deck.tauUs ) <= tol, quiet, "%s on %s: the tail's time constant is %.4f us (the deck's de-emphasis, %.2f us; +-%.4f)", tape.name, deck.name, tau,
			                    deck.tauUs, tol );
			//And its sign: a rising step arrives short and creeps up.
			failures += report( rs.front() < 0.0, quiet, "%s on %s: a rising step arrives short of its level and creeps up (residual %.4f)", tape.name, deck.name, rs.front() );
		}
		else
			failures += report( std::fabs( rs.front() ) <= 3.0 * ripple + 1e-4, quiet, "%s on its own deck: no tail, %.6f left %.1f us after the edge (<= %.6f)", tape.name, std::fabs( rs.front() ), fitFrom, 3.0 * ripple + 1e-4 );
		s.end();
	}
	return failures;
}

//---------------------------------------------------------------------------
// --clamp: the black lifts by what the porch still holds of the sync.
//---------------------------------------------------------------------------
/// The step response of pre( s ) de( s ), pre = ( 1 + s t1 ) / ( 1 + s t1 / x ),
/// de = ( 1 + s t2 / x ) / ( 1 + s t2 ), by residues: 1 + sum over the two
/// poles of N( p ) / ( p D'( p ) ) e^{ p t }.
double emphasisStep( double t1, double t2, double x, double t )
{
	if( t < 0.0 )
		return 0.0;
	auto N  = [ & ]( double s ) { return ( 1.0 + s * t1 ) * ( 1.0 + s * t2 / x ); };
	auto Dp = [ & ]( double s ) { return ( t1 / x ) * ( 1.0 + s * t2 ) + ( 1.0 + s * t1 / x ) * t2; };
	const double p1 = -x / t1, p2 = -1.0 / t2;
	return 1.0 + N( p1 ) / ( p1 * Dp( p1 ) ) * std::exp( p1 * t ) + N( p2 ) / ( p2 * Dp( p2 ) ) * std::exp( p2 * t );
}

int runClamp( int W, int H, int perturb, bool quiet )
{
	if( !quiet )
		std::printf( "clamp: Hi8 on Video8 lifts the black by what the back porch still holds of the sync, %dx%d\n", W, H );
	int failures = 0;
	for( int si = 0; si < 2; ++si )
	{
		const StatedStandard& st = kStatedStandards[ si ];
		const StatedFormat& tape = kStatedFormats[ 1 ];
		const StatedFormat& deck = kStatedFormats[ 0 ];
		const model::Standard& ms = model::StandardOf( si );
		Session s;
		Knobs k;
		k.standard = si;
		if( !open( s, W, H, k, perturb, true ) )
			return failures + 1;
		const Deck d    = renderDeck( s, 10, flat( W, H, 0.0 ) );
		const double lift = d.mean( d.N / 4, 3 * d.N / 4, pixelAt( 20.0 ), pixelAt( 40.0 ) );

		//The prediction: the sync pulse, -S over [ 0, T ), through Hi8's
		//pre-emphasis and Video8's de-emphasis, x 2.0 / 1.2, read where the
		//plugin's clamp reads (1 us after the sync to 0.5 us before the
		//picture, offset by the deck's compensated delay) on the time the
		//signal really took: the low-passes' delays and the RF band's at the
		//porch's carrier, which the analytic response leaves out.
		const double fs  = kFs;
		const double D   = s.plugin.EngineForTest().DelayForTest( s.plugin.LastSettingsForTest() );
		const int Dr     = static_cast< int >( std::lround( D ) );
		const dsp::Cascade recY = dsp::ButterworthLowPass( 4, tape.yLowMHz * 1e6, fs );
		const dsp::Cascade rf   = dsp::ButterworthHighPass( 2, deck.rfHighMHz * 1e6, fs ).Then( dsp::ButterworthLowPass( 8, deck.rfLowMHz * 1e6, fs ) );
		const dsp::Cascade yLow = dsp::ButterworthLowPass( 8, deck.yLowMHz * 1e6, fs );
		const double wBlank     = 2.0 * kPi * carrierHz( tape, st, 0.0 ) / fs;
		const double wTip       = 2.0 * kPi * carrierHz( tape, st, -st.S ) / fs;
		const double delta      = recY.GroupDelay( 0.0 ) + rf.GroupDelay( wBlank ) - 0.5 + yLow.GroupDelay( 0.0 );
		const double T          = st.sync;
		const double G          = tape.devMHz / deck.devMHz;
		const int p0            = Dr + ms.SyncSamples() + samplesOf( 1.0 );
		const int p1            = Dr + ms.ActiveStart() - samplesOf( 0.5 );
		double porch            = 0.0;
		for( int i = p0; i < p1; ++i )
		{
			const double t = ( i - delta ) / fs * 1e6;
			porch += G * ( -st.S ) * ( emphasisStep( tape.tauUs, deck.tauUs, shelfX( tape ), t ) - emphasisStep( tape.tauUs, deck.tauUs, shelfX( tape ), t - T ) );
		}
		porch /= ( p1 - p0 );
		const double want = -porch / st.Wh;
		//Tolerance: the delay stands in for each low-pass's whole response, so
		//the tail's level is off by the RF band's group delay swing over the
		//sync's edge (tip to blanking) against 1.3 us, and by the low-passes'
		//spread, second order: ( 0.15 us / 1.3 us )^2 < 2 %.
		const double swing = std::fabs( rf.GroupDelay( wTip ) - rf.GroupDelay( wBlank ) ) / fs * 1e6;
		const double ripple = rippleBound( deck, st, carrierHz( tape, st, -st.S ), carrierHz( tape, st, 0.0 ) );
		const double tol   = std::fabs( want ) * ( swing / deck.tauUs + 0.02 ) + ripple;
		failures += report( std::fabs( lift - want ) <= tol && want > 3.0 * ripple, quiet, "%s Hi8 on Video8: black reads %.5f, the porch's held sync predicts %.5f (+-%.5f)", st.name, lift, want, tol );
		s.end();

		//A matched deck does not lift.
		Session m;
		k.recording = model::kVideo8;
		if( !open( m, W, H, k, perturb, true ) )
			return failures + 1;
		const Deck dm      = renderDeck( m, 10, flat( W, H, 0.0 ) );
		const double black = dm.mean( dm.N / 4, 3 * dm.N / 4, pixelAt( 20.0 ), pixelAt( 40.0 ) );
		const double rippleV8 = rippleBound( deck, st, carrierHz( deck, st, -st.S ), carrierHz( deck, st, 0.0 ) );
		failures += report( std::fabs( black ) <= rippleV8 + 1e-4, quiet, "%s Video8 on Video8: black reads %.5f (<= %.5f)", st.name, black, rippleV8 + 1e-4 );
		m.end();
	}
	return failures;
}

//---------------------------------------------------------------------------
// --threshold: the carrier for white sits furthest outside the deck's band.
//---------------------------------------------------------------------------
int runThreshold( int W, int H, int perturb, bool quiet )
{
	const double cnr = 30.0;
	if( !quiet )
		std::printf( "threshold: at %.0f dB carrier-to-noise, Hi8's white is noisier than its black by the deck band's |H| ratio, %dx%d\n", cnr, W, H );
	int failures = 0;
	struct Case
	{
		int rec, deck;
	};
	const Case cases[] = { { model::kHi8, model::kVideo8 }, { model::kVideo8, model::kVideo8 } };
	for( const Case& c : cases )
	{
		const StatedStandard& st = kStatedStandards[ 0 ];
		const StatedFormat& tape = kStatedFormats[ c.rec ];
		const StatedFormat& deck = kStatedFormats[ c.deck ];
		double rms[ 2 ] = {};
		for( int lv = 0; lv < 2; ++lv )
		{
			const double level = lv == 0 ? 0.0 : 1.0;
			Session clean, noisy;
			Knobs k;
			k.recording = c.rec;
			k.deck      = c.deck;
			if( !open( clean, W, H, k, perturb, true ) )
				return failures + 1;
			k.cnrDb = cnr;
			if( !open( noisy, W, H, k, perturb, false ) )
				return failures + 1;
			const Deck a = renderDeck( clean, 10, flat( W, H, level ) );
			const Deck b = renderDeck( noisy, 10, flat( W, H, level ) );
			double sum = 0.0;
			long n     = 0;
			for( int l = a.N / 4; l < 3 * a.N / 4; ++l )
				for( int p = pixelAt( 10.0 ); p < a.P - pixelAt( 2.0 ); ++p )
				{
					const double e = b.y( l, p ) - a.y( l, p );
					sum += e * e;
					++n;
				}
			rms[ lv ] = std::sqrt( sum / n );
			clean.end();
			noisy.end();
		}
		//Above threshold a limiter and a frequency discriminator turn the
		//noise in the band into output noise in proportion to noise over
		//carrier: the noise in the band is the same at both levels, the
		//carrier is |H( f )| of the band at its own frequency.
		const double fBlack = carrierHz( tape, st, 0.0 ), fWhite = carrierHz( tape, st, st.Wh );
		const double want   = rfGain( deck, fBlack ) / rfGain( deck, fWhite );
		const double ratio  = rms[ 1 ] / rms[ 0 ];
		//First-order theory: +-25 % for the demodulator's noise spectrum and
		//the de-emphasis, which this ignores.
		failures += report( ratio >= 0.75 * want && ratio <= 1.33 * want, quiet, "%s on %s: noise at white / at black %.3f (|H| at %.2f / %.2f MHz predicts %.3f; rms %.4f / %.4f)", tape.name, deck.name, ratio,
		                    fBlack / 1e6, fWhite / 1e6, want, rms[ 1 ], rms[ 0 ] );
		if( c.rec != c.deck )
			failures += report( ratio > 1.5, quiet, "%s on %s: the highlights break up first (white %.2fx black)", tape.name, deck.name, ratio );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --streak: a bright edge's overshoot leaves the band and the crossings go.
//---------------------------------------------------------------------------
int runStreak( int W, int H, int perturb, bool quiet )
{
	if( !quiet )
		std::printf( "streak: Hi8 on Video8 dips below black after a black-to-white edge; on Hi8 it does not, %dx%d\n", W, H );
	int failures = 0;
	struct Case
	{
		int rec, deck;
	};
	const Case cases[] = { { model::kHi8, model::kVideo8 }, { model::kHi8, model::kHi8 } };
	for( const Case& c : cases )
	{
		const StatedStandard& st = kStatedStandards[ 0 ];
		const StatedFormat& tape = kStatedFormats[ c.rec ];
		const StatedFormat& deck = kStatedFormats[ c.deck ];
		Session s;
		Knobs k;
		k.recording = c.rec;
		k.deck      = c.deck;
		if( !open( s, W, H, k, perturb, true ) )
			return failures + 1;
		const Deck d = renderDeck( s, 10, twoLevel( W, H, 0.0, 1.0, 0.5 ) );
		double left, right;
		stepMeans( d, st, left, right );
		const double active = st.lineUs - st.front - st.sync - st.back;
		const double split  = active / 2.0;
		double lowest       = 1e9;
		for( int p = pixelAt( split - 1.0 ); p <= pixelAt( split + 1.0 ); ++p )
			lowest = std::min( lowest, d.mean( d.N / 4, 3 * d.N / 4, p, p + 1 ) );
		const double dip = left - lowest;
		//The overshoot of a black-to-white edge after Hi8's pre-emphasis is
		//clipped at 220 %: 10.1 MHz, where Video8's 8th-order band at 7 MHz
		//passes |H| = 0.05. A deck whose band holds it (Hi8's, 10.3 MHz) only
		//rings: its low-passes' undershoot is under a tenth of the step.
		if( c.rec != c.deck )
			failures += report( dip > 0.2 * ( right - left ), quiet, "%s on %s: the edge dips %.3f below black (%.0f%% of the step), |H( 10.1 MHz )| = %.3f", tape.name, deck.name, dip,
			                    100.0 * dip / ( right - left ), rfGain( deck, 10.1e6 ) );
		else
			failures += report( dip < 0.1 * ( right - left ), quiet, "%s on %s: no streak, the edge dips %.3f (%.1f%% of the step)", tape.name, deck.name, dip, 100.0 * dip / ( right - left ) );
		s.end();
	}
	return failures;
}

//---------------------------------------------------------------------------
// --wallace: the head clog's loss is 54.6 d / lambda dB.
//---------------------------------------------------------------------------
int runWallace( int, int, int perturb, bool quiet )
{
	if( !quiet )
		std::printf( "wallace: the head clog's FIR loses 54.6 d / lambda dB, lambda = v / f, v a 40 mm drum at the frame rate\n" );
	int failures = 0;
	lowband::Engine engine;
	for( int si = 0; si < 2; ++si )
	{
		const StatedStandard& st = kStatedStandards[ si ];
		const double v           = kPi * 0.040 * st.fps;
		for( double dUm : { 0.03, 0.08, 0.15 } )
		{
			lowband::EngineSettings s;
			s.standard   = si;
			s.clogMetres = dUm * 1e-6;
			s.perturb    = perturb;
			const std::vector< float >& taps = engine.ClogTapsForTest( s );
			const int M                      = static_cast< int >( taps.size() / 2 );
			//The ideal taps, h[ n ] = ( 2 / fs ) a ( 1 - (-1)^n e^{-a fs/2} ) /
			//( a^2 + ( 2 pi n / fs )^2 ), realise e^{-a f} exactly below fs / 2;
			//the plugin keeps |n| <= M. What it leaves out, summed to 1e6:
			const double a = 2.0 * kPi * dUm * 1e-6 / v;
			auto tailAt    = [ & ]( double fHz ) {
                double t = 0.0;
                for( int n = M + 1; n < 1000000; ++n )
                {
                    const double b    = 2.0 * kPi * n / kFs;
                    const double sign = ( n & 1 ) ? -1.0 : 1.0;
                    t += 2.0 * ( 2.0 / kFs ) * a * ( 1.0 - sign * std::exp( -a * kFs / 2.0 ) ) / ( a * a + b * b ) * std::cos( 2.0 * kPi * fHz * n / kFs );
                }
                return t;
			};
			for( double fMHz : { 4.2, 7.7, 10.1 } )
			{
				double h = 0.0;
				for( int n = -M; n <= M; ++n )
					h += taps[ static_cast< size_t >( n + M ) ] * std::cos( 2.0 * kPi * fMHz * 1e6 * n / kFs );
				const double want     = 54.575 * dUm * 1e-6 * fMHz * 1e6 / v;
				const double realised = std::exp( -a * fMHz * 1e6 ) - tailAt( fMHz * 1e6 );
				const double lossDb   = -20.0 * std::log10( h );
				//The taps are floats: 2M + 1 roundings of at most 2^-24 of each.
				failures += report( std::fabs( h - realised ) <= ( 2 * M + 1 ) * kU * 2.0, quiet, "%s d %.2f um at %4.1f MHz: %.3f dB; Wallace %.3f, less the %d-tap truncation %.3f (%.1e off)", st.name, dUm, fMHz, lossDb, want,
				                    2 * M + 1, -20.0 * std::log10( realised ), std::fabs( h - realised ) );
				failures += report( std::fabs( lossDb - want ) <= 0.5, quiet, "%s d %.2f um at %4.1f MHz: within 0.5 dB of the law (%+.3f)", st.name, dUm, fMHz, lossDb - want );
			}
		}
	}
	return failures;
}

//---------------------------------------------------------------------------
// --registration: a matched deck puts an edge back where it was.
//---------------------------------------------------------------------------
int runRegistration( int W, int H, int perturb, bool quiet )
{
	if( !quiet )
		std::printf( "registration: a matched deck puts a small luma step where the source had it, on the deck's raster, %dx%d\n", W, H );
	int failures = 0;
	for( int si = 0; si < 2; ++si )
		for( int fi = 0; fi < 2; ++fi )
		{
			const StatedStandard& st = kStatedStandards[ si ];
			const StatedFormat& f    = kStatedFormats[ fi ];
			const model::Standard& ms = model::StandardOf( si );
			const int P              = ms.ActivePixels();
			Session s;
			Knobs k;
			k.standard  = si;
			k.recording = fi;
			k.deck      = fi;
			if( !open( s, W, H, k, perturb, true ) )
				return failures + 1;
			//A small step stays clear of the clips. The split at a host pixel
			//boundary: in raster pixels it is xs P / W. Measured on the deck's
			//own raster (the picture the plugin uploads), every line averaged:
			//the host picture point-samples it, and at 320 wide a host pixel is
			//2.2 raster pixels (--display checks that pass on its own).
			const double a = 0.4, b = 0.5;
			const int xs   = static_cast< int >( std::lround( 0.5 * W ) );
			const Deck d   = renderDeck( s, 10, twoLevel( W, H, a, b, 0.5 ) );
			if( d.lines.empty() )
				return failures + 1;
			std::vector< double > y( static_cast< size_t >( P ) );
			for( int p = 0; p < P; ++p )
				y[ static_cast< size_t >( p ) ] = d.mean( 0, d.N, p, p + 1 );
			const double mid = 0.5 * ( a + b );
			double found     = -1.0;
			for( int x = P / 4; x < 3 * P / 4 - 1; ++x )
				if( ( y[ static_cast< size_t >( x ) ] - mid ) * ( y[ static_cast< size_t >( x + 1 ) ] - mid ) <= 0.0 && y[ static_cast< size_t >( x ) ] != y[ static_cast< size_t >( x + 1 ) ] )
				{
					found = x + ( mid - y[ static_cast< size_t >( x ) ] ) / ( y[ static_cast< size_t >( x + 1 ) ] - y[ static_cast< size_t >( x ) ] );
					break;
				}
			//Raster pixel centres sit at p (the intake's pixel p covers
			//[ p, p + 1 ) of the active line, centre p + 1/2): the source's edge
			//is at xs P / W - 1/2 in centre coordinates.
			const double offsetPx = found - ( static_cast< double >( xs ) * P / W - 0.5 );
			//What a matched deck's compensation (the chain's DC group delays)
			//leaves: a step's 50 % point sits later than the DC delay by the
			//rise of the Butterworths' group delay toward their corners.
			//Measured here by running the same small step through the
			//baseband chain in double (record low-pass, pre-emphasis, the
			//deck's low-pass, de-emphasis: the plugin's designs, a different
			//route) against their DC delays. The RF band's delay is taken at
			//the step's carrier against the blanking carrier the deck uses.
			const double fs = kFs;
			const dsp::Cascade chainBase = dsp::ButterworthLowPass( 4, f.yLowMHz * 1e6, fs )
			                                   .Then( dsp::Shelf( f.tauUs * 1e-6, shelfX( f ), fs, false ) )
			                                   .Then( dsp::ButterworthLowPass( 8, f.yLowMHz * 1e6, fs ) )
			                                   .Then( dsp::Shelf( f.tauUs * 1e-6, shelfX( f ), fs, true ) );
			std::vector< double > state( 64, 0.0 );
			const int n0 = 400, nT = 1200;
			double prev = 0.0, cross = -1.0;
			{
				std::vector< double > s1( chainBase.sections.size() ), s2( chainBase.sections.size() );
				for( int i = 0; i < nT; ++i )
				{
					double x = i >= n0 ? 1.0 : 0.0;
					for( size_t j = 0; j < chainBase.sections.size(); ++j )
					{
						const dsp::Biquad& q = chainBase.sections[ j ];
						const double out     = q.b0 * x + s1[ j ];
						s1[ j ]              = q.b1 * x - q.a1 * out + s2[ j ];
						s2[ j ]              = q.b2 * x - q.a2 * out;
						x                    = out;
					}
					if( cross < 0.0 && i > n0 && prev < 0.5 && x >= 0.5 )
						cross = ( i - 1 ) + ( 0.5 - prev ) / ( x - prev );
					prev = x;
				}
			}
			//The step input crosses 50 % at n0 - 1/2 (between samples).
			const dsp::Cascade rf = dsp::ButterworthHighPass( 2, f.rfHighMHz * 1e6, fs ).Then( dsp::ButterworthLowPass( 8, f.rfLowMHz * 1e6, fs ) );
			const double wMid     = 2.0 * kPi * carrierHz( f, st, mid * st.Wh ) / fs;
			const double wBlank   = 2.0 * kPi * carrierHz( f, st, 0.0 ) / fs;
			const double expectSamples = ( cross - ( n0 - 0.5 ) ) - chainBase.GroupDelay( 0.0 ) + rf.GroupDelay( wMid ) - rf.GroupDelay( wBlank );
			const double expectPx      = expectSamples / 3.0;
			//Tolerance: a fifth of a 40.5 MHz sample for the FM chain's own
			//placement (the demodulator's crossing error, the RF band's group
			//delay across the step's carrier swing), and a twentieth of a pixel
			//for the linear interpolation of the 50 % point between pixels.
			const double tol = 0.05 + 0.2 / 3.0;
			failures += report( found >= 0.0 && std::fabs( offsetPx - expectPx ) <= tol, quiet, "%s %-6s: the step lands %+.3f raster px from the source (%+.3f expected from the filters' shape; +-%.3f)", st.name, f.name, offsetPx,
			                    expectPx, tol );
			s.end();
		}
	return failures;
}

//---------------------------------------------------------------------------
// --intake: the host frame onto the standard's raster.
//---------------------------------------------------------------------------
int runIntake( int W, int H, int perturb, bool quiet )
{
	if( !quiet )
		std::printf( "intake: each raster pixel is the area average of the host pixels it covers, as Y'UV, %dx%d\n", W, H );
	int failures = 0;
	for( int si = 0; si < 2; ++si )
	{
		const model::Standard& ms = model::StandardOf( si );
		const int P = ms.ActivePixels(), N = ms.frameLines;
		Session s;
		Knobs k;
		k.standard = si;
		if( !open( s, W, H, k, perturb, true ) )
			return failures + 1;
		Picture p( static_cast< size_t >( W ) * H * 4 );
		for( int r = 0; r < H; ++r )
			for( int x = 0; x < W; ++x )
			{
				float* px = p.data() + ( static_cast< size_t >( r ) * W + x ) * 4;
				px[ 0 ]   = static_cast< float >( 0.5 + 0.4 * std::sin( 0.13 * x + 0.02 * r ) );
				px[ 1 ]   = static_cast< float >( 0.5 + 0.4 * std::cos( 0.07 * x - 0.05 * r ) );
				px[ 2 ]   = static_cast< float >( ( ( x / 7 + r / 5 ) & 1 ) ? 0.9 : 0.1 );
				px[ 3 ]   = 1.0f;
			}
		if( !s.render( 10, p ) )
			return failures + 1;
		const std::vector< float >& in = s.plugin.IntakeForTest();
		double worst = 0.0;
		for( int l = 0; l < N; l += 7 )
			for( int i = 0; i < P; i += 3 )
			{
				//The exact area average, by the overlap of [ l H, ( l + 1 ) H ) with
				//[ r N, ( r + 1 ) N ), and the same across, in double.
				double rgb[ 3 ] = {};
				for( int r = 0; r < H; ++r )
				{
					const double oy = std::max( 0.0, std::min( ( r + 1.0 ) * N, ( l + 1.0 ) * H ) - std::max( 1.0 * r * N, 1.0 * l * H ) );
					if( oy <= 0.0 )
						continue;
					for( int x = 0; x < W; ++x )
					{
						const double ox = std::max( 0.0, std::min( ( x + 1.0 ) * P, ( i + 1.0 ) * W ) - std::max( 1.0 * x * P, 1.0 * i * W ) );
						if( ox <= 0.0 )
							continue;
						for( int c = 0; c < 3; ++c )
							rgb[ c ] += oy * ox * p[ ( static_cast< size_t >( r ) * W + x ) * 4 + c ];
					}
				}
				float avg[ 3 ];
				for( int c = 0; c < 3; ++c )
					avg[ c ] = static_cast< float >( rgb[ c ] / ( static_cast< double >( H ) * W ) );
				double y, u, v;
				rgbToYuv( avg, y, u, v );
				const float* got = in.data() + ( static_cast< size_t >( l ) * P + i ) * 4;
				worst            = std::max( { worst, std::fabs( got[ 0 ] - y ), std::fabs( got[ 1 ] - u ), std::fabs( got[ 2 ] - v ) } );
			}
		//A sum of up to ( W / P + 1 ) ( H / N + 1 ) floats under 1: a few ulps.
		failures += report( worst <= 2e-6, quiet, "%s: the intake is the host's area average in Y'UV (%.2g off at most, every 7th line and 3rd pixel)", ms.name, worst );
		s.end();
	}
	return failures;
}

//---------------------------------------------------------------------------
// --display: the host picture is the deck's.
//---------------------------------------------------------------------------
int runDisplay( int W, int H, int perturb, bool quiet )
{
	if( !quiet )
		std::printf( "display: the host picture is the deck's raster read back: nearest line, Catmull-Rom across, R'G'B' clamped; at the deck's own raster and at %dx%d\n", W, H );
	int failures = 0;
	for( int si = 0; si < 2; ++si )
	{
		const model::Standard& ms = model::StandardOf( si );
		const int P = ms.ActivePixels(), N = ms.frameLines;
		for( int pass = 0; pass < 2; ++pass )
		{
			const int w = pass == 0 ? P : W, h = pass == 0 ? N : H;
			Session s;
			Knobs k;
			k.standard = si;
			k.cnrDb    = 24.0;
			if( !open( s, w, h, k, perturb, false ) )
				return failures + 1;
			const Picture p = yuvPicture( w, h, [ & ]( int x, int r, double& y, double& u, double& v ) {
				y = 0.3 + 0.25 * std::sin( 0.05 * x ) * std::cos( 0.07 * r );
				u = 0.08 * std::sin( 0.01 * x );
				v = -0.06;
			} );
			if( !s.render( 10, p ) )
				return failures + 1;
			const std::vector< float > out   = s.readAll();
			const std::vector< float >& deck = s.plugin.LinesForTest();
			double worst = 0.0;
			long clamped = 0;
			for( int r = 0; r < h; ++r )
			{
				const int l = ( ( 2 * r + 1 ) * N ) / ( 2 * h );
				for( int x = 0; x < w; ++x )
				{
					const double sx = ( x + 0.5 ) * P / w - 0.5;
					const double i0 = std::floor( sx ), t = sx - i0;
					const double t2 = t * t, t3 = t2 * t;
					const double wt[ 4 ] = { 0.5 * ( -t3 + 2.0 * t2 - t ), 0.5 * ( 3.0 * t3 - 5.0 * t2 + 2.0 ), 0.5 * ( -3.0 * t3 + 4.0 * t2 + t ), 0.5 * ( t3 - t2 ) };
					//The weights' derivatives in t, for the bound below.
					const double dw[ 4 ] = { 0.5 * ( -3.0 * t2 + 4.0 * t - 1.0 ), 0.5 * ( 9.0 * t2 - 10.0 * t ), 0.5 * ( -9.0 * t2 + 8.0 * t + 1.0 ), 0.5 * ( 3.0 * t2 - 2.0 * t ) };
					double yuv[ 3 ] = {}, slope[ 3 ] = {};
					for( int q = 0; q < 4; ++q )
					{
						const int i = std::clamp( static_cast< int >( i0 ) - 1 + q, 0, P - 1 );
						for( int c = 0; c < 3; ++c )
						{
							const double v = deck[ ( static_cast< size_t >( l ) * P + i ) * 4 + c ];
							yuv[ c ] += wt[ q ] * v;
							slope[ c ] += dw[ q ] * v;
						}
					}
					//The shader's sample position is a float: ( x + 1/2 ) P is exact
					//and the subtraction is, but GLSL 4.10 (s4.7.1) lets a division
					//be 2.5 ulp out: for a position near 700, 1.5e-4 of a pixel. It
					//moves each channel by its spline's slope there, and the matrix
					//carries U to B by 1 / 0.492 and V to R by 1 / 0.877. Plus 2e-6
					//for the float weights and matrix (correctly rounded products of
					//values under 2), and 1 % on the slope for the neighbourhood the
					//rounded position may stand in.
					const double posErr = 2.5 * std::ldexp( 1.0, std::ilogb( sx + 0.5 ) - 23 );
					const double bound  = posErr * 1.01 * ( std::fabs( slope[ 0 ] ) + std::fabs( slope[ 1 ] ) / 0.492111 + std::fabs( slope[ 2 ] ) / 0.877283 ) + 2e-6;
					float rgb[ 3 ];
					yuvToRgb( yuv[ 0 ], yuv[ 1 ], yuv[ 2 ], rgb );
					for( int c = 0; c < 3; ++c )
					{
						const double want = std::clamp( static_cast< double >( rgb[ c ] ), 0.0, 1.0 );
						clamped += want != rgb[ c ];
						worst = std::max( worst, std::fabs( want - out[ ( static_cast< size_t >( r ) * w + x ) * 4 + c ] ) / bound );
					}
				}
			}
			failures += report( worst <= 1.0, quiet, "%s at %dx%d: every host pixel is the deck's picture read back (at most %.2f of its float bound; %ld values clamped)", ms.name, w, h, worst, clamped );
			s.end();
		}
	}
	return failures;
}

//---------------------------------------------------------------------------
// --dropout: a dropout disturbs its own line, from where it starts.
//---------------------------------------------------------------------------
int runDropout( int W, int H, int perturb, bool quiet )
{
	if( !quiet )
		std::printf( "dropout: a forced dropout disturbs its own line only, and only from its start, %dx%d\n", W, H );
	int failures = 0;
	const int line = 101;
	const double us0 = 12.0, us1 = 16.0;
	Session a, b;
	Knobs k;
	if( !open( a, W, H, k, perturb, true ) || !open( b, W, H, k, perturb, true ) )
		return 1;
	b.plugin.SetDropoutForTest( true, line, us0, us1 );
	const Picture card = twoLevel( W, H, 0.3, 0.6, 0.5 );
	const Deck da      = renderDeck( a, 10, card );
	const Deck db      = renderDeck( b, 10, card );
	int otherLines = 0, firstPixel = -1;
	double inside  = 0.0;
	for( int l = 0; l < da.N; ++l )
		for( int p = 0; p < da.P; ++p )
		{
			const bool differs = da.y( l, p ) != db.y( l, p );
			if( differs && l != line )
				++otherLines;
			if( differs && l == line && firstPixel < 0 )
				firstPixel = p;
			if( l == line && p >= pixelAt( us0 + 0.5 ) && p < pixelAt( us1 ) )
				inside = std::max( inside, std::fabs( da.y( l, p ) - db.y( l, p ) ) );
		}
	//The chain is causal except for the deck's delay compensation, which
	//moves the output D samples earlier, and the dropout's raised-cosine
	//edge: nothing can change before us0 - edge - D.
	const double D        = a.plugin.EngineForTest().DelayForTest( a.plugin.LastSettingsForTest() );
	const double earliest = us0 - model::kDropoutEdgeUs - D / kFs * 1e6 - 1.0 / 13.5;
	failures += report( otherLines == 0, quiet, "no other line changes (%d pixels did)", otherLines );
	failures += report( firstPixel >= pixelAt( earliest ), quiet, "line %d changes from %.2f us (the dropout starts at %.1f; nothing may move before %.2f)", line, ( firstPixel + 1.0 / 3.0 ) / 13.5, us0, earliest );
	failures += report( inside > 0.1, quiet, "and inside the dropout the line is broken (up to %.3f of white off)", inside );
	a.end();
	b.end();
	return failures;
}

//---------------------------------------------------------------------------
// --threads: one worker and many give the same picture.
//---------------------------------------------------------------------------
int runThreads( int W, int H, int perturb, bool quiet )
{
	if( !quiet )
		std::printf( "threads: the deck's picture does not depend on the worker count, %dx%d\n", W, H );
	int failures = 0;
	std::vector< std::vector< float > > outs;
	for( int threads : { 1, 3, 8 } )
	{
		Session s;
		Knobs k;
		k.cnrDb    = 20.0;
		k.dropouts = 0.6;
		k.clogUm   = 0.05;
		if( !open( s, W, H, k, perturb, false ) )
			return 1;
		s.plugin.SetThreadsForTest( threads );
		std::vector< float > all;
		for( long f = 0; f < 6; ++f )
		{
			const std::vector< unsigned char > card = buildCard( W, H, f );
			if( !s.render( f * 3, card ) )
				return 1;
			const std::vector< float >& l = s.plugin.LinesForTest();
			all.insert( all.end(), l.begin(), l.end() );
		}
		outs.push_back( all );
		s.end();
	}
	size_t differ = 0;
	for( size_t i = 0; i < outs[ 0 ].size(); ++i )
		differ += ( outs[ 1 ][ i ] != outs[ 0 ][ i ] ) + ( outs[ 2 ][ i ] != outs[ 0 ][ i ] );
	failures += report( differ == 0, quiet, "1, 3 and 8 workers: six frames with noise, dropouts and a clog, bit for bit (%zu values differ)", differ );
	return failures;
}

//---------------------------------------------------------------------------
// --resize: nothing is lost across a resize.
//---------------------------------------------------------------------------
int runResize( int W, int H, int perturb, bool quiet )
{
	if( !quiet )
		std::printf( "resize: every frame after a resize to 1.5x and back is the unresized run's, %dx%d\n", W, H );
	int failures = 0;
	auto run     = [ & ]( bool resize, std::vector< std::vector< float > >& out ) -> bool {
		Session s;
		Knobs k;
		k.cnrDb    = 24.0;
		k.dropouts = 0.5;
		if( !open( s, W, H, k, perturb, false ) )
			return false;
		out.clear();
		for( long m = 0; m < 40; ++m )
		{
			if( resize && m == 12 )
				s.resize( W * 3 / 2, H * 3 / 2 );
			if( resize && m == 18 )
				s.resize( W, H );
			const Picture p = yuvPicture( s.width, s.height, [ & ]( int x, int r, double& y, double& u, double& v ) {
				y = 0.4 + 0.25 * std::sin( 0.07 * x + 0.05 * r );
				u = 0.06;
				v = -0.04;
			} );
			if( !s.render( m, p ) )
				return false;
			if( m >= 24 )
				out.push_back( s.readAll() );
		}
		s.end();
		return true;
	};
	std::vector< std::vector< float > > a, b;
	if( !run( false, a ) || !run( true, b ) )
		return 1;
	//The deck is on the CPU and deterministic; only the display pass is the
	//GPU's, and the software renderer is not bit-repeatable: four float ulps
	//of the value is its allowance (gate's rule).
	size_t differ = 0, moved = 0;
	for( size_t f = 0; f < a.size(); ++f )
		for( size_t i = 0; i < a[ f ].size(); ++i )
		{
			if( std::fabs( a[ f ][ i ] - b[ f ][ i ] ) > 4.0 * 2.0 * kU * std::fabs( a[ f ][ i ] ) + 1e-30 )
				++differ;
			moved += a[ f ][ i ] != a[ 0 ][ i ];
		}
	failures += report( differ == 0, quiet, "frames 24-39 after a resize to %dx%d and back: every subpixel is the unresized run's (%zu differ)", W * 3 / 2, H * 3 / 2, differ );
	failures += report( moved > 0, quiet, "and those frames move (%zu subpixels change: the noise follows the clock), so there was state to keep", moved );
	return failures;
}

//---------------------------------------------------------------------------
// --alpha: a tape has no alpha.
//---------------------------------------------------------------------------
int runAlpha( int W, int H, int perturb, bool quiet )
{
	if( !quiet )
		std::printf( "alpha: output alpha is 1 at Mix 1 and the clip's at Mix 0, %dx%d\n", W, H );
	int failures = 0;
	Picture clear( static_cast< size_t >( W ) * H * 4, 0.0f );
	for( size_t i = 0; i < clear.size(); i += 4 )
		clear[ i ] = clear[ i + 1 ] = clear[ i + 2 ] = 0.25f;
	for( double mix : { 1.0, 0.0 } )
	{
		Session s;
		Knobs k;
		k.mix = mix;
		if( !open( s, W, H, k, perturb, true ) )
			return 1;
		if( !s.render( 10, clear ) )
			return 1;
		const std::vector< float > out = s.readAll();
		double lo = 1e9, hi = -1e9, worst = 0.0;
		for( size_t i = 0; i < out.size(); i += 4 )
		{
			lo = std::min( lo, static_cast< double >( out[ i + 3 ] ) );
			hi = std::max( hi, static_cast< double >( out[ i + 3 ] ) );
			if( mix == 0.0 )
				for( int c = 0; c < 4; ++c )
					worst = std::max( worst, std::fabs( static_cast< double >( out[ i + c ] ) - clear[ i + c ] ) );
		}
		if( mix == 1.0 )
			failures += report( lo == 1.0 && hi == 1.0, quiet, "Mix 1 on a transparent clip: alpha %.3f..%.3f, opaque", lo, hi );
		else
			failures += report( worst <= 4.0 * kU, quiet, "Mix 0: the clip exactly, alpha 0 kept (%.2g off at most)", worst );
		s.end();
	}
	return failures;
}

//---------------------------------------------------------------------------
// --names and --model: no GL.
//---------------------------------------------------------------------------
int runNames()
{
	std::printf( "names: nothing the host will silently truncate; every name unique as Arena addresses it\n" );
	Lowband plugin;
	int failures = 0;
	std::set< std::string > seen;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		if( p.index >= Lowband::PT_ABOUT_FIRST )
			continue;
		//Arena addresses a parameter by its name lower-cased with the spaces
		//removed: two that reduce alike are one parameter to it.
		std::string key;
		for( char c : p.name )
			if( c != ' ' )
				key += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
		failures += report( p.name.size() <= 16, false, "%-16s %2zu characters", p.name.c_str(), p.name.size() );
		failures += report( seen.insert( key ).second, false, "%-16s unique as Arena addresses it ('%s')", p.name.c_str(), key.c_str() );
	}
	failures += report( std::string( "SW Lowband" ).size() <= 16, false, "display name 'SW Lowband' is %zu characters", std::string( "SW Lowband" ).size() );
	return failures;
}

int runModel()
{
	std::printf( "model: the plugin's numbers against the stated ones\n" );
	int failures = 0;
	for( int fi = 0; fi < 2; ++fi )
	{
		const StatedFormat& f  = kStatedFormats[ fi ];
		const model::Format& m = model::FormatOf( fi );
		failures += report( m.syncTipHz == f.tipMHz * 1e6 && m.deviationHz == f.devMHz * 1e6 && std::fabs( m.PeakWhiteHz() - ( f.tipMHz + f.devMHz ) * 1e6 ) < 1e-6, false,
		                    "%-6s FM: sync tip %.1f MHz, peak white %.1f MHz, deviation %.1f MHz", f.name, f.tipMHz, f.tipMHz + f.devMHz, f.devMHz );
		failures += report( std::fabs( m.emphasisTau - f.tauUs * 1e-6 ) < 1e-15 && std::fabs( m.emphasisX - shelfX( f ) ) < 1e-6, false, "%-6s emphasis: tau %.2f us, shelf %.4f dB, corners %.0f and %.0f kHz", f.name, f.tauUs, f.xDb,
		                    1e3 / ( 2.0 * kPi * f.tauUs ), 1e3 * shelfX( f ) / ( 2.0 * kPi * f.tauUs ) );
		failures += report( m.rfHighPassHz == f.rfHighMHz * 1e6 && m.rfLowPassHz == f.rfLowMHz * 1e6 && m.yLowPassHz == f.yLowMHz * 1e6, false, "%-6s deck: RF band %.2f to %.2f MHz, Y low-pass %.1f MHz", f.name, f.rfHighMHz,
		                    f.rfLowMHz, f.yLowMHz );
	}
	failures += report( model::PlaybackMode( model::kVideo8, model::kHi8 ) == model::kVideo8 && model::PlaybackMode( model::kHi8, model::kVideo8 ) == model::kVideo8 && model::PlaybackMode( model::kHi8, model::kHi8 ) == model::kHi8, false,
	                    "a Video8 deck has one mode; a Hi8 deck plays each tape in its own" );
	for( int si = 0; si < 2; ++si )
	{
		const StatedStandard& st  = kStatedStandards[ si ];
		const model::Standard& ms = model::StandardOf( si );
		const double active       = st.lineUs - st.front - st.sync - st.back;
		failures += report( ms.LineSamples() == static_cast< int >( std::lround( st.lineUs * 40.5 ) ) && ms.frameLines == st.lines && std::fabs( ms.Active() - active ) < 1e-12, false,
		                    "%s: %d samples a line at 40.5 MHz, %d lines, %.4f us active = %d pixels at 13.5 MHz", st.name, ms.LineSamples(), st.lines, active, ms.ActivePixels() );
		failures += report( std::fabs( ms.syncVolts - st.S ) < 1e-12 && std::fabs( ms.whiteVolts - st.Wh ) < 1e-12, false, "%s: sync %.4f V below blanking, white %.4f V above", st.name, st.S, st.Wh );
		failures += report( std::fabs( ms.colourUnderHz - st.underFh * st.fH ) < 1e-6, false, "%s: colour-under %.3f kHz = %.3f fH (the same for Hi8)", st.name, st.underFh * st.fH / 1e3, st.underFh );
		const double v = kPi * 0.040 * st.fps;
		failures += report( std::fabs( ms.WritingSpeed() - v ) < 1e-12, false, "%s: writing speed %.4f m/s (40 mm drum, %.3f turns a second)", st.name, v, st.fps );
	}
	failures += report( std::fabs( model::SpacingLossDb( 1e-7, 1e7, 3.0 ) - 54.575 * 1e-7 * 1e7 / 3.0 ) < 1e-3, false, "Wallace: %.3f dB per wavelength of spacing (20 log10 e^2 pi)", 20.0 / std::log( 10.0 ) * 2.0 * kPi );
	//The noise: sigma^2 B / ( fs / 2 ) of white noise in the 5.1 MHz band
	//against a carrier's 1/2.
	for( double db : { 10.0, 28.0, 46.0 } )
	{
		const double sigma = model::NoiseSigma( db );
		const double cnr   = 10.0 * std::log10( 0.5 / ( sigma * sigma * 5.1e6 / ( kFs / 2.0 ) ) );
		failures += report( std::fabs( cnr - db ) < 1e-9, false, "noise sigma %.5f gives %.1f dB in the Video8 band", sigma, cnr );
	}
	failures += report( std::fabs( controls::CnrDb( 0.0f ) - 46.0 ) < 1e-12 && std::fabs( controls::CnrDb( 1.0f ) - 10.0 ) < 1e-12 && !controls::NoiseOn( 0.0f ), false, "Tape Noise: off at 0, then 46 dB down to 10 dB" );
	return failures;
}

const Check kChecks[] = {
	{ "--carrier", runCarrier, "the recorded FM at sync tip, blanking and white is the format's (counted)", model::kPerturbHi8AtVideo8Tip, "Hi8 written from Video8's sync tip", false },
	{ "--matched", runMatched, "a deck playing its own format gives back a flat field's level", model::kPerturbRisingOnly, "the demodulator counts rising crossings only", true },
	{ "--gain", runGain, "Hi8 on Video8 has the gain 2.0 / 1.2; a matched deck 1", model::kPerturbDeckDeviation, "the Video8 deck uses the tape's map", true },
	{ "--emphasis", runEmphasis, "the mismatched emphasis leaves a tail with the deck's 1.3 us", model::kPerturbDeckEmphasis, "the Video8 deck de-emphasises with the tape's tau", true },
	{ "--clamp", runClamp, "the black lifts by what the porch still holds of the sync", model::kPerturbDeckEmphasis, "the Video8 deck de-emphasises with the tape's tau", true },
	{ "--threshold", runThreshold, "Hi8's white is noisier than its black by the band's |H| ratio", model::kPerturbWideBand, "the Video8 deck's band as wide as Hi8's", true },
	{ "--streak", runStreak, "Hi8 on Video8 dips below black after a bright edge", model::kPerturbWideBand, "the Video8 deck's band as wide as Hi8's", true },
	{ "--wallace", runWallace, "the head clog loses 54.6 d / lambda dB", model::kPerturbWrongSpeed, "the clog at the other standard's speed", false },
	{ "--registration", runRegistration, "a matched deck puts an edge back where it was", model::kPerturbNoDelay, "the deck's delay left uncompensated", true },
	{ "--intake", runIntake, "the host frame's area average onto the standard's raster, in Y'UV", 0, "", true },
	{ "--display", runDisplay, "the host picture is the deck's, at the deck's raster and between its pixels at another", 0, "", true },
	{ "--dropout", runDropout, "a dropout disturbs its own line, from its start", model::kPerturbDropoutLine, "the dropout two lines down", true },
	{ "--threads", runThreads, "one worker and many give the same picture", model::kPerturbSeedByWorker, "the noise seeded by worker", true },
	{ "--resize", runResize, "nothing is lost across a resize", model::kPerturbResizeResetsClock, "a resize restarts the clock", true },
	{ "--alpha", runAlpha, "alpha 1 at Mix 1, the clip's at Mix 0", 0, "", true },
};

int runNegative( int W, int H )
{
	std::printf( "negative controls: each perturbation of the plugin's model must FAIL its check, %dx%d\n", W, H );
	int failures = 0;
	for( const Check& c : kChecks )
	{
		if( c.negativeBits == 0 || c.run == nullptr )
			continue;
		const int before = g_failures;
		const int checks = g_checks;
		const int failed = c.run( W, H, c.negativeBits, true );
		g_failures       = before;
		g_checks         = checks;
		failures += report( failed > 0, false, "%-44s -> %s fails (%d of its checks)", c.negativeWhat, c.flag, failed );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( Lowband& plugin, int width, int height, int frames, double fps, size_t& stateBytes )
{
	Session session;
	session.floatOutput = false;
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.index < Lowband::PT_ABOUT_FIRST && p.type != FF_TYPE_BUFFER && p.type != FF_TYPE_EVENT && p.type != FF_TYPE_TEXT )
			session.plugin.SetFloatParameter( p.index, p.value );
	session.fps = fps;
	if( !session.begin( width, height ) )
		return -1.0;

	//The card is uploaded once: a host's frame is already on the GPU, and
	//the plugin's cost does not depend on what the frame holds.
	const std::vector< unsigned char > card = buildCard( width, height, 0 );
	const int warmup                        = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( frame, card );
	glFinish();

	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int frame = 0; frame < frames; ++frame )
			session.renderAt( warmup + run * frames + frame );
		glFinish();
		const auto end       = std::chrono::steady_clock::now();
		const double seconds = std::chrono::duration< double >( end - start ).count();
		best                 = std::min( best, seconds * 1000.0 / static_cast< double >( frames ) );
	}
	stateBytes = session.plugin.StateBytesForTest();
	session.end();
	return best;
}

int runBench( Lowband& plugin, int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "3840x2160 ", 3840, 2160 },
	};
	std::printf( "%d frames each, best of three runs, after a 20-frame warm-up, glFinish both sides, the card uploaded once.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame   state held\n" );
	for( const Size& size : sizes )
	{
		size_t bytes    = 0;
		const double ms = benchAt( plugin, size.width, size.height, frames, fps, bytes );
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%          %6.2f MB\n", size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0,
		             ms / 16.667 * 100.0, static_cast< double >( bytes ) / 1048576.0 );
	}
	std::printf( "\nState is the standard's raster twice on the GPU (the intake and the deck's\n"
	             "picture, RGBA32F) and twice on the CPU (their copies), plus the engine's\n"
	             "per-line results. The FM chain's own buffers are per line and per worker,\n"
	             "allocated each frame. Whatever the settings above were, they are what was\n"
	             "measured; --set measures another.\n" );
	return 0;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"lbtest -- render and measure Lowband: a Hi8 tape on a Video8 deck\n"
		"\n"
		"  --out PATH          render the moving card through the plugin (default /tmp/lowband.png)\n"
		"  --average           write the mean of every frame rendered, not the last\n"
		"  --size WxH          raster (default 1280x720)\n"
		"  --frames N          frames to render before reading back (default 90)\n"
		"  --fps N             synthetic display rate driving the clock (default 60)\n"
		"  --source card|flat|white|black   what to feed (card moves); --level V for flat\n"
		"  --set \"Name=V\"      set a parameter by its display name (element index for options).\n"
		"                      Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks, at --size:\n" );
	for( const Check& c : kChecks )
		if( c.run != nullptr )
			std::printf( "  %-18s  %s\n", c.flag, c.help );
	std::printf(
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks verbosely against a perturbed model (bits in Model.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --names             nothing the host will silently truncate or merge\n"
		"  --model             the plugin's numbers against the stated ones\n"
		"\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K, and the state held\n"
		"  --profile           time the engine alone at 1, 2, 4 and 8 workers\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --allow-no-gl       with checks: report SKIP rather than FAIL when no GL context can be made\n"
		"\n"
		"  LBTEST_RENDERER=software   use Apple's software renderer (a GPU-less CI runner's)\n" );
}

//---------------------------------------------------------------------------
// --profile: the engine alone, on the card's intake, at 1, 2, 4 and 8
// workers, against the whole ProcessOpenGL at 1080p.
//---------------------------------------------------------------------------
int runProfile( Lowband& configured, int frames )
{
	Session session;
	session.floatOutput = false;
	for( const NamedParameter& p : listParameters( configured ) )
		if( p.index < Lowband::PT_ABOUT_FIRST && p.type != FF_TYPE_EVENT && p.type != FF_TYPE_TEXT )
			session.plugin.SetFloatParameter( p.index, p.value );
	if( !session.begin( 1920, 1080 ) )
		return 1;
	const std::vector< unsigned char > card = buildCard( 1920, 1080, 0 );
	session.render( 0, card );
	glFinish();
	const std::vector< float > intake = session.plugin.IntakeForTest();
	lowband::EngineSettings s       = session.plugin.LastSettingsForTest();
	const model::Standard& st       = model::StandardOf( s.standard );
	std::vector< float > out( intake.size() );
	lowband::Engine engine;
	std::printf( "engine alone, %d x %d lines, %d frames each, best of three:\n", st.ActivePixels(), st.frameLines, frames );
	for( int threads : { 1, 2, 4, 8 } )
	{
		double best = 1e9;
		for( int run = 0; run < 3; ++run )
		{
			const auto start = std::chrono::steady_clock::now();
			for( int f = 0; f < frames; ++f )
			{
				s.videoFrame = f;
				engine.Process( intake.data(), st.frameLines, st.ActivePixels(), s, out.data(), threads );
			}
			const double ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / frames;
			best            = std::min( best, ms );
		}
		std::printf( "  %d worker%s  %7.3f ms/frame\n", threads, threads == 1 ? " " : "s", best );
	}
	session.end();
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	namespace shaders = lowband::shaders;
	const std::pair< const char*, std::string > files[] = {
		{ "vertex.vert", shaders::Vertex() },
		{ "intake.frag", shaders::Intake() },
		{ "display.frag", shaders::Display() },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//
// A STANDARD parameter ramps linearly between cues. An option, a boolean
// and an integer STEP: they hold the last cue at or before the frame,
// because there is nothing between Xenon and Tungsten to ramp through. An
// event fires on its cue frame only.
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

float valueAt( const Track& track, int frame, unsigned int type )
{
	if( track.empty() )
		return 0.0f;
	if( type == FF_TYPE_EVENT )
	{
		for( const auto& cue : track )
			if( cue.first == frame )
				return cue.second;
		return 0.0f;
	}
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			if( type != FF_TYPE_STANDARD )
				return frame == b.first ? b.second : a.second;
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/lowband.png";
	std::string scriptPath;
	std::string dumpDir;
	std::string source = "card";
	double level   = 0.5;
	int width      = 1280;
	int height     = 720;
	int frames     = 90;
	int failRender = -1;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantProfile = false;
	bool wantPipe  = false;
	bool average   = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	std::set< std::string > rendered = { "--negative" };
	for( const Check& c : kChecks )
		rendered.insert( c.flag );
	const std::set< std::string > offline  = { "--names", "--model" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--source" && hasNext )
			source = argv[ ++i ];
		else if( argument == "--level" && hasNext )
			level = std::strtod( argv[ ++i ], nullptr );
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
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--profile" )
			wantProfile = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--average" )
			average = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( rendered.count( argument ) || offline.count( argument ) )
			checks.push_back( argument );
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

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Lowband plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
	{
		auto requested = [ & ]( const std::string& flag ) {
			return std::find( checks.begin(), checks.end(), flag ) != checks.end();
		};
		if( requested( "--names" ) )
		{
			runNames();
			std::printf( "\n" );
		}
		if( requested( "--model" ) )
		{
			runModel();
			std::printf( "\n" );
		}
		bool needGL = requested( "--negative" );
		for( const Check& c : kChecks )
		{
			if( !requested( c.flag ) )
				continue;
			if( c.gl )
			{
				needGL = true;
				continue;
			}
			c.run( width, height, perturb, false );
			std::printf( "\n" );
		}

		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "         The rendering checks and their negative controls were NOT run.\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				for( const Check& c : kChecks )
				{
					if( !c.gl || !requested( c.flag ) )
						continue;
					c.run( width, height, perturb, false );
					std::printf( "\n" );
				}
				if( requested( "--negative" ) )
				{
					runNegative( width, height );
					std::printf( "\n" );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	Session session;
	session.floatOutput = false;
	session.fps         = fps;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}
	session.plugin.SetPerturbForTest( perturb );

	if( wantBench )
		return finish( runBench( session.plugin, frames < 40 ? 60 : frames, fps ) );
	if( wantProfile )
		return finish( runProfile( session.plugin, frames < 10 ? 30 : frames ) );

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		struct Automation
		{
			unsigned int index;
			unsigned int type;
			Track track;
		};
		std::vector< Automation > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation.push_back( { static_cast< unsigned int >( index ), session.plugin.GetParamType( static_cast< unsigned int >( index ) ), entry.second } );
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider
			//would, and an event is a press.
			for( const Automation& a : automation )
				session.plugin.SetFloatParameter( a.index, valueAt( a.track, index, a.type ) );

			const bool ok = index != failRender && session.render( index, frame );
			if( !ok )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );
	std::vector< double > sum;
	for( int frame = 0; frame < frames; ++frame )
	{
		bool ok = false;
		if( source == "card" )
			ok = session.render( frame, buildCard( width, height, frame ) );
		else if( source == "flat" )
			ok = session.render( frame, flat( width, height, level ) );
		else if( source == "white" )
			ok = session.render( frame, flat( width, height, 1.0 ) );
		else if( source == "black" )
			ok = session.render( frame, flat( width, height, 0.0 ) );
		else
		{
			std::fprintf( stderr, "unknown --source %s\n", source.c_str() );
			return finish( 2 );
		}
		if( !ok )
			return finish( 1 );
		if( average )
		{
			const std::vector< unsigned char > image = session.readBack();
			sum.resize( image.size(), 0.0 );
			for( size_t i = 0; i < image.size(); ++i )
				sum[ i ] += image[ i ];
		}
	}

	std::vector< unsigned char > image = session.readBack();
	if( average )
		for( size_t i = 0; i < image.size(); ++i )
			image[ i ] = static_cast< unsigned char >( std::lround( sum[ i ] / frames ) );
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames%s)\n", outPath.c_str(), width, height, frames, average ? ", averaged" : "" );
	return finish( 0 );
}
