// The reference side of demo/tools/check_port.sh. NOT a copy of the plugin:
// check_port.mjs pastes four pieces of source/Lowband.{h,cpp} into the @@
// markers below at run time, unedited -- the ParamID enum, the anonymous
// namespace (loc, bindTextures, ClientTransfer, defaultThreads, ...), the
// whole of Lowband::Lowband() and the whole of Lowband::ProcessOpenGL -- and
// compiles them against the plugin's own Engine.cpp, Model.cpp, Dsp.cpp,
// Controls.cpp and Clock.cpp. What this file supplies is only the scaffolding
// those pieces need to compile without a GL context or the FFGL SDK: GL entry
// points and ffglex classes that RECORD what the plugin does (every uniform
// by name, every texture per unit, every framebuffer drawn into, the deck's
// picture as uploaded) instead of doing it, and a glReadPixels that hands the
// plugin the intake check_port.mjs wrote, as if the GPU had produced it.
//
// stdin, one scenario per block:
//   SCENARIO W H p0 .. p7 n
//   then n lines:  t intake-path upload-path
// (the eight parameter floats in ParamID order, then per frame the host time,
// the P x N x 4 float intake to read back, and where to write the P x N x 4
// float upload). stdout: the declarations, the designs, then per frame the
// record check_port.mjs compares with the port.

#include "Engine.h"
#include "Model.h"
#include "Controls.h"
#include "Clock.h"
#include "Dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

//---------------------------------------------------------------------------
// GL, recorded.
//---------------------------------------------------------------------------
using GLuint   = unsigned int;
using GLint    = int;
using GLenum   = unsigned int;
using GLsizei  = int;
using GLfloat  = float;
using GLubyte  = unsigned char;
using FFUInt32 = unsigned int;
using FFResult = unsigned int;

constexpr GLenum GL_VIEWPORT = 0x0BA2, GL_TEXTURE_2D = 0x0DE1, GL_RGBA32F = 0x8814, GL_RGBA = 0x1908,
                 GL_FLOAT = 0x1406, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800,
                 GL_NEAREST = 0x2600, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
                 GL_CLAMP_TO_EDGE = 0x812F, GL_FRAMEBUFFER = 0x8D40, GL_TEXTURE0 = 0x84C0,
                 GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02,
                 GL_PIXEL_UNPACK_BUFFER_BINDING = 0x88EF, GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED,
                 GL_UNPACK_ROW_LENGTH = 0x0CF2, GL_PACK_ROW_LENGTH = 0x0D02, GL_UNPACK_ALIGNMENT = 0x0CF5,
                 GL_PACK_ALIGNMENT = 0x0D05, GL_PIXEL_UNPACK_BUFFER = 0x88EC, GL_PIXEL_PACK_BUFFER = 0x88EB;
constexpr FFResult FF_SUCCESS = 0, FF_FAIL = 1;
constexpr FFUInt32 FF_TYPE_BOOLEAN = 0, FF_TYPE_EVENT = 1, FF_TYPE_STANDARD = 10, FF_TYPE_OPTION = 11,
                   FF_TYPE_TEXT = 100;

namespace rec
{
std::map< GLuint, std::string > names;///< texture and framebuffer ids -> buffer names
std::vector< std::string > uniformNames;
GLuint program = 0;
std::map< GLuint, std::string > programNames;
int activeUnit = 0;
GLuint bound[ 8 ] = {};
GLuint framebuffer = 0;
GLuint nextTexture = 500;
std::vector< std::string > lines;///< the current pass's uniform lines
std::string out;
std::vector< float > intake;     ///< what the next glReadPixels returns
std::string uploadPath;          ///< where the next glTexSubImage2D goes

std::string bits( float v )
{
	uint32_t u;
	std::memcpy( &u, &v, 4 );
	char b[ 16 ];
	std::snprintf( b, sizeof b, "%08x", u );
	return b;
}
std::string bits64( double v )
{
	uint64_t u;
	std::memcpy( &u, &v, 8 );
	char b[ 24 ];
	std::snprintf( b, sizeof b, "%016llx", static_cast< unsigned long long >( u ) );
	return b;
}
std::string nameOf( GLuint id )
{
	auto it = names.find( id );
	return it == names.end() ? ( id == 0 ? std::string( "none" ) : "tex" + std::to_string( id ) ) : it->second;
}
} // namespace rec

GLint glGetUniformLocation( GLuint program, const char* name )
{
	(void)program;
	rec::uniformNames.push_back( name );
	return static_cast< GLint >( rec::uniformNames.size() - 1 );
}
static const std::string& uname( GLint location )
{
	return rec::uniformNames[ static_cast< size_t >( location ) ];
}
void glUniform1i( GLint l, GLint v )
{
	rec::lines.push_back( "I " + uname( l ) + " " + std::to_string( v ) );
}
void glUniform1f( GLint l, GLfloat v )
{
	rec::lines.push_back( "F " + uname( l ) + " " + rec::bits( v ) );
}
void glGetIntegerv( GLenum name, GLint* v )
{
	//One value, except the viewport's four.
	v[ 0 ] = 0;
	if( name == GL_VIEWPORT )
		v[ 1 ] = v[ 2 ] = v[ 3 ] = 0;
}
void glGenTextures( GLsizei n, GLuint* ids )
{
	for( int i = 0; i < n; ++i )
	{
		ids[ i ] = rec::nextTexture++;
		rec::names[ ids[ i ] ] = "lines";
	}
}
void glDeleteTextures( GLsizei, const GLuint* ) {}
void glBindTexture( GLenum, GLuint t )
{
	rec::bound[ rec::activeUnit ] = t;
}
void glActiveTexture( GLenum unit )
{
	rec::activeUnit = static_cast< int >( unit - GL_TEXTURE0 );
}
void glTexImage2D( GLenum, GLint, GLint, GLsizei w, GLsizei h, GLint, GLenum, GLenum, const void* )
{
	rec::out += "ALLOC lines " + std::to_string( w ) + " " + std::to_string( h ) + "\n";
}
void glTexParameteri( GLenum, GLenum, GLint ) {}
void glTexSubImage2D( GLenum, GLint, GLint, GLint, GLsizei w, GLsizei h, GLenum, GLenum, const void* data )
{
	rec::out += "UPLOAD " + rec::nameOf( rec::bound[ rec::activeUnit ] ) + " " + std::to_string( w ) + " " + std::to_string( h ) + "\n";
	std::ofstream f( rec::uploadPath, std::ios::binary );
	f.write( static_cast< const char* >( data ), static_cast< std::streamsize >( sizeof( float ) ) * w * h * 4 );
}
void glBindFramebuffer( GLenum, GLuint fbo )
{
	rec::framebuffer = fbo;
}
void glBindBuffer( GLenum, GLuint ) {}
void glViewport( GLint, GLint, GLsizei, GLsizei ) {}
void glPixelStorei( GLenum, GLint ) {}
void glReadPixels( GLint, GLint, GLsizei w, GLsizei h, GLenum, GLenum, void* data )
{
	rec::out += "READBACK " + rec::nameOf( rec::framebuffer ) + " " + std::to_string( w ) + " " + std::to_string( h ) + "\n";
	const size_t n = static_cast< size_t >( w ) * h * 4;
	if( rec::intake.size() != n )
	{
		std::fprintf( stderr, "intake has %zu floats, the read-back wants %zu\n", rec::intake.size(), n );
		std::exit( 3 );
	}
	std::memcpy( data, rec::intake.data(), n * sizeof( float ) );
}
const GLubyte* glGetString( GLenum )
{
	return reinterpret_cast< const GLubyte* >( "refport" );
}

//---------------------------------------------------------------------------
// ffglex and the plugin's own classes, as recorders.
//---------------------------------------------------------------------------
namespace ffglex
{
struct FFGLShader
{
	GLuint id;
	FFGLShader( GLuint i, const char* name ) : id( i )
	{
		rec::programNames[ i ] = name;
	}
	GLuint GetGLID() const
	{
		return id;
	}
};
struct ScopedShaderBinding
{
	explicit ScopedShaderBinding( GLuint p )
	{
		rec::program = p;
	}
	~ScopedShaderBinding()
	{
		rec::program = 0;
	}
};
struct FFGLScreenQuad
{
	void Draw()
	{
		std::string s = "PASS " + rec::programNames[ rec::program ] + " -> " + rec::nameOf( rec::framebuffer ) + " textures";
		int last = -1;
		for( int u = 0; u < 8; ++u )
			if( rec::bound[ u ] != 0 )
				last = u;
		for( int u = 0; u <= last; ++u )
			s += " " + rec::nameOf( rec::bound[ u ] );
		rec::out += s + "\n";
		std::sort( rec::lines.begin(), rec::lines.end() );
		for( const auto& l : rec::lines )
			rec::out += "  " + l + "\n";
		rec::lines.clear();
	}
};
} // namespace ffglex

namespace lowband
{
class PassBuffer
{
public:
	enum class Sampling
	{
		Nearest,
		Linear
	};
	PassBuffer( GLuint id, const char* name ) : tex( id ), fbo( id + 1000 )
	{
		rec::names[ tex ] = name;
		rec::names[ fbo ] = name;
	}
	bool Ensure( int w, int h, GLint, Sampling )
	{
		if( w != width || h != height )
			rec::out += "ENSURE " + rec::names[ tex ] + " " + std::to_string( w ) + " " + std::to_string( h ) + "\n";
		width  = w;
		height = h;
		return true;
	}
	GLuint TextureID() const
	{
		return tex;
	}
	GLuint GetGLID() const
	{
		return fbo;
	}
	void ResizeViewPort() {}

private:
	GLuint tex, fbo;
	int width = 0, height = 0;
};

namespace diag
{
inline void error( const std::string& ) {}
inline void info( const std::string& ) {}
inline void init() {}
} // namespace diag
} // namespace lowband

struct FFGLTextureStruct
{
	FFUInt32 Width, Height, HardwareWidth, HardwareHeight;
	GLuint Handle;
};
struct ProcessOpenGLStruct
{
	FFUInt32 numInputTextures;
	FFGLTextureStruct** inputTextures;
	GLuint HostFBO;
};
struct FFGLLog
{
	static void LogToHost( const char* ) {}
};

namespace stoatworks::about
{
inline constexpr unsigned kParamCount = 4;
struct Button
{
	const char* label;
};
inline std::vector< Button > buttons()
{
	return { { "Project page" }, { "Source on GitHub" }, { "Support the work" } };
}
inline const char* defaultText()
{
	return "about";
}
} // namespace stoatworks::about

//---------------------------------------------------------------------------
// The plugin class, as far as ProcessOpenGL and the constructor reach. The
// member names and types are Lowband.h's.
//---------------------------------------------------------------------------
class Lowband
{
public:
	Lowband();
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL );

//@@ENUM@@

	//CFFGLPlugin's parameter declarations, recorded.
	std::string decl;
	void SetMinInputs( unsigned ) {}
	void SetMaxInputs( unsigned ) {}
	void SetTimeSupported( bool ) {}
	float GetFloatParameter( unsigned index )
	{
		return params[ index ];
	}
	void SetParamInfo( unsigned index, const char* name, unsigned type, float value )
	{
		declare( index, name, type, value );
	}
	void SetParamInfo( unsigned index, const char* name, unsigned type, bool value )
	{
		declare( index, name, type, value ? 1.0f : 0.0f );
	}
	void SetParamInfo( unsigned index, const char* name, unsigned type, const char* )
	{
		declare( index, name, type, 0.0f );
	}
	void SetParamInfof( unsigned index, const char* name, unsigned type )
	{
		SetParamInfo( index, name, type, GetFloatParameter( index ) );
	}
	void SetOptionParamInfo( unsigned index, const char* name, unsigned count, float value )
	{
		declare( index, name, FF_TYPE_OPTION, value );
		decl += "  COUNT " + std::to_string( count ) + "\n";
	}
	void SetParamElementInfo( unsigned index, unsigned element, const char* name, float value )
	{
		decl += "  ELEMENT " + std::to_string( index ) + " " + std::to_string( element ) + " " + name + " " + rec::bits( value ) + "\n";
	}
	void SetParamGroup( unsigned index, std::string group )
	{
		decl += "  GROUP " + std::to_string( index ) + " " + group + "\n";
	}
	void declare( unsigned index, const char* name, unsigned type, float value )
	{
		decl += "PARAM " + std::to_string( index ) + " " + std::to_string( type ) + " " + rec::bits( value ) + " " + name + "\n";
	}

	ffglex::FFGLShader intakeShader{ 1, "intake" };
	ffglex::FFGLShader displayShader{ 2, "display" };
	ffglex::FFGLScreenQuad quad;

	lowband::PassBuffer intake{ 10, "intake" };
	GLuint linesTexture = 0;
	int linesW = 0, linesH = 0;

	std::vector< float > intakeData;
	std::vector< float > linesData;
	lowband::Engine engine;
	lowband::EngineSettings lastSettings;

	lowband::Clock clock;
	bool hostTimeSeen = false;
	double hostTime   = 0.0;///< CFFGLPlugin's member, which SetTime writes
	int lastWidth = 0, lastHeight = 0;

	int perturb        = 0;
	bool quiet         = false;
	int threadsForTest = 0;
	bool forceDropout  = false;
	lowband::model::Dropout forcedDrop{};

	float params[ PT_COUNT ] = {};
};

using namespace ffglex;
using namespace lowband;

//@@ANON@@

//@@CONSTRUCTOR@@

//@@PROCESS@@

/// The designs the chain is built from, for every format, standard and mode:
/// the float coefficients each filter RUNS with, the steady state a few
/// inputs give, and the double numbers they come from.
static void designs()
{
	namespace m = lowband::model;
	std::string out = "DESIGN shelfX " + rec::bits64( m::FormatOf( 0 ).emphasisX ) + "\n";
	auto cascade = [ & ]( const std::string& name, const lowband::dsp::Cascade& c ) {
		out += "DESIGN " + name;
		for( float v : c.coeffs )
			out += " " + rec::bits( v );
		out += "\n";
		for( float x : { 0.0f, 0.3f, -0.21f, 5.7e6f, 0.5627f } )
		{
			float st[ 64 ] = {};
			c.SteadyState( x, st );
			out += "STEADY " + name + " " + rec::bits( x );
			for( int i = 0; i < c.StateSize(); ++i )
				out += " " + rec::bits( st[ i ] );
			out += "\n";
		}
		//At DC and at the four blanking carriers Engine::configure evaluates
		//the delay at (each mode's, on each standard); then three probes,
		//which also carry libm's cos and sin.
		for( double w : { 0.0 } )
			out += "DELAY " + name + " " + rec::bits64( w ) + " " + rec::bits64( c.GroupDelay( w ) ) + "\n";
		for( int f = 0; f < m::kFormatCount; ++f )
			for( int st = 0; st < m::kStandardCount; ++st )
			{
				const double S = m::StandardOf( st ).syncVolts, Wh = m::StandardOf( st ).whiteVolts;
				const m::Format& own = m::FormatOf( f );
				const double w = 2.0 * m::kPi * ( own.syncTipHz + S / ( S + Wh ) * own.deviationHz ) / m::kSampleHz;
				out += "DELAY " + name + " " + rec::bits64( w ) + " " + rec::bits64( c.GroupDelay( w ) ) + "\n";
			}
		for( double w : { 0.1, 0.77, 2.5 } )
			out += "PROBE " + name + " " + rec::bits64( w ) + " " + rec::bits64( c.GroupDelay( w ) ) + "\n";
	};
	for( int f = 0; f < m::kFormatCount; ++f )
	{
		const m::Format& F = m::FormatOf( f );
		const std::string tag = F.name;
		cascade( tag + ".recordY", lowband::dsp::ButterworthLowPass( m::kRecordYOrder, F.recordYHz, m::kSampleHz ) );
		cascade( tag + ".pre", lowband::dsp::Shelf( F.emphasisTau, F.emphasisX, m::kSampleHz, false ) );
		cascade( tag + ".de", lowband::dsp::Shelf( F.emphasisTau, F.emphasisX, m::kSampleHz, true ) );
		cascade( tag + ".rfHigh", lowband::dsp::ButterworthHighPass( m::kRfHighOrder, F.rfHighPassHz, m::kSampleHz ) );
		cascade( tag + ".rfLow", lowband::dsp::ButterworthLowPass( m::kRfLowOrder, F.rfLowPassHz, m::kSampleHz ) );
		cascade( tag + ".yLow", lowband::dsp::ButterworthLowPass( m::kYLowOrder, F.yLowPassHz, m::kSampleHz ) );
		cascade( tag + ".record", lowband::dsp::ButterworthLowPass( m::kRecordYOrder, F.recordYHz, m::kSampleHz ).Then( lowband::dsp::Shelf( F.emphasisTau, F.emphasisX, m::kSampleHz, false ) ) );
	}
	cascade( "chroma", lowband::dsp::ButterworthLowPass( 2, m::kChromaHalfHz, m::kPixelHz ) );
	for( int st = 0; st < m::kStandardCount; ++st )
	{
		const m::Standard& S = m::StandardOf( st );
		out += "STANDARD " + std::to_string( st ) + " " + std::to_string( S.LineSamples() ) + " " + std::to_string( S.SyncSamples() ) + " "
		       + std::to_string( S.ActiveStart() ) + " " + std::to_string( S.ActivePixels() ) + " " + rec::bits64( S.WritingSpeed() ) + " "
		       + rec::bits64( S.Active() ) + " " + rec::bits64( S.FrameRate() ) + "\n";
		for( double um : { 0.0, 0.01, 0.06, 0.15 } )
		{
			out += "CLOG " + std::to_string( st ) + " " + rec::bits64( um );
			for( float t : m::ClogTaps( um * 1e-6, S.WritingSpeed(), m::kClogHalfTaps ) )
				out += " " + rec::bits( t );
			out += "\n";
		}
	}
	for( double db : { 46.0, 30.0, 24.4, 10.0, 27.7 } )
		out += "SIGMA " + rec::bits64( db ) + " " + rec::bits64( m::NoiseSigma( db ) ) + "\n";
	for( long long frame : { 0LL, 1LL, 7LL, 1000LL, 123456789LL } )
	{
		for( double per : { 0.0, 0.3, 4.5, 30.0, 100.0 } )
		{
			out += "DROPOUTS " + std::to_string( frame ) + " " + rec::bits64( per );
			for( const m::Dropout& d : m::Dropouts( frame, per, 576, m::StandardOf( 0 ).Active() ) )
				out += " " + std::to_string( d.line ) + ":" + rec::bits64( d.us0 ) + ":" + rec::bits64( d.us1 ) + ":"
				       + std::to_string( std::lround( d.us0 * m::kSampleHz * 1e-6 ) ) + ":" + std::to_string( std::lround( d.us1 * m::kSampleHz * 1e-6 ) );
			out += "\n";
		}
		out += "SEEDS " + std::to_string( frame );
		for( int line : { 0, 1, 2, 575, 479 } )
			out += " " + std::to_string( m::LineSeed( frame, line ) );
		out += "\n";
	}
	std::cout << out;
	//The Gaussian table, every entry, as a file next to the uploads.
	std::ofstream g( rec::uploadPath, std::ios::binary );
	g.write( reinterpret_cast< const char* >( m::GaussianTable() ), sizeof( float ) << m::kGaussianBits );
}

static std::vector< float > readFloats( const std::string& path )
{
	std::ifstream f( path, std::ios::binary | std::ios::ate );
	const std::streamsize n = f.tellg();
	f.seekg( 0 );
	std::vector< float > v( static_cast< size_t >( n ) / sizeof( float ) );
	f.read( reinterpret_cast< char* >( v.data() ), n );
	return v;
}

int main( int argc, char** argv )
{
	rec::names[ 0 ]  = "host";
	rec::names[ 99 ] = "picture";
	if( argc > 1 )
	{
		rec::uploadPath = argv[ 1 ];
		designs();
	}
	{
		Lowband declared;
		std::cout << declared.decl;
	}
	std::string word;
	while( std::cin >> word )
	{
		if( word != "SCENARIO" )
			return 2;
		int W, H, n;
		std::cin >> W >> H;
		Lowband plugin;
		for( int i = 0; i < 8; ++i )
			std::cin >> plugin.params[ i ];
		std::cin >> n;
		std::cout << "SCENARIO " << W << " " << H << "\n";
		FFGLTextureStruct picture{ static_cast< FFUInt32 >( W ), static_cast< FFUInt32 >( H ), static_cast< FFUInt32 >( W ), static_cast< FFUInt32 >( H ), 99 };
		FFGLTextureStruct* inputs[ 1 ] = { &picture };
		ProcessOpenGLStruct process{ 1, inputs, 0 };
		for( int f = 0; f < n; ++f )
		{
			double t;
			std::string inPath;
			std::cin >> t >> inPath >> rec::uploadPath;
			rec::intake = readFloats( inPath );
			//lbtest's renderAt: the unit declared, then Lowband::SetTime.
			plugin.clock.SetScaleForTest( 1.0 );
			plugin.hostTimeSeen = true;
			plugin.hostTime     = t;
			rec::out.clear();
			const FFResult r = plugin.ProcessOpenGL( &process );
			const lowband::EngineSettings& s = plugin.lastSettings;
			char head[ 512 ];
			std::snprintf( head, sizeof head, "FRAME %d result %u seconds %s videoFrame %lld\n", f, r, rec::bits64( plugin.clock.Now() ).c_str(),
			               static_cast< long long >( s.videoFrame ) );
			std::cout << head;
			std::snprintf( head, sizeof head, "SETTINGS %d %d %d %d %s %s %s %d\n", s.standard, s.recording, s.deck, s.noise ? 1 : 0,
			               rec::bits64( s.cnrDb ).c_str(), rec::bits64( s.clogMetres ).c_str(), rec::bits64( s.dropoutsPerFrame ).c_str(),
			               s.syncAgc ? 1 : 0 );
			std::cout << head;
			std::cout << "MONITOR " << rec::bits64( plugin.engine.LastSyncDepth() ) << " " << rec::bits64( plugin.engine.LastGain() ) << " "
			          << rec::bits64( plugin.engine.LastPorch() ) << " " << rec::bits64( plugin.engine.DelaySamples() ) << "\n";
			std::cout << rec::out;
		}
	}
	return 0;
}
