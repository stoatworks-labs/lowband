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
};

//---------------------------------------------------------------------------
// --names and --model: no GL.
//---------------------------------------------------------------------------
int runNames()
{
	std::printf( "names: nothing the host will silently truncate; every name unique\n" );
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
	return 0;
}

const Check kChecks[] = {
	{ "--placeholder", nullptr, "", 0, "" },
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
		bool needGL = false;
		for( const std::string& check : checks )
		{
			if( check == "--names" || check == "--model" )
			{
				if( check == "--names" )
					runNames();
				else
					runModel();
				std::printf( "\n" );
			}
			else
				needGL = true;
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
				for( const std::string& check : checks )
				{
					const Check* found = nullptr;
					for( const Check& c : kChecks )
						if( check == c.flag )
							found = &c;
					if( found != nullptr )
						found->run( width, height, perturb, false );
					else if( check == "--negative" )
						runNegative( width, height );
					else
						continue;
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
