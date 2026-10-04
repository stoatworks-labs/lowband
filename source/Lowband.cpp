#include "Lowband.h"

#include "Controls.h"
#include "Diag.h"
#include "Model.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9). The symptom without it is an unknown-type error on
//ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

using namespace ffglex;
using namespace lowband;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Lowband >,// Create method
	"LB01",                  // Plugin unique ID of maximum length 4.
	"SW Lowband",            // Plugin name
	2,                       // API major version number
	1,                       // API minor version number
	0,                       // Plugin major version number
	1,                       // Plugin minor version number
	FF_EFFECT,               // Plugin type
	"A Hi8 tape played back on a Video8 deck.\n\nThe luma is a real FM signal: written by Hi8 at 5.7-7.7 MHz, read by a Video8 deck's channel and demodulator built for 4.2-5.4 MHz. The contrast comes out 5/3 too high, a glow trails every highlight, the black lifts, bright edges throw black streaks, and with tape noise the highlights break up first. The colour-under chroma is the same in both formats, so the colour survives. Play a Video8 tape, or use a Hi8 deck, for the clean reference.\n\nStart with Tape Noise and Head Clog.",// Plugin description
	"Lowband FFGL effect"    // About
);

namespace
{
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

GLint loc( const FFGLShader& shader, const char* name )
{
	return glGetUniformLocation( shader.GetGLID(), name );
}

void bindTextures( std::initializer_list< GLuint > textures )
{
	int unit = 0;
	for( GLuint texture : textures )
	{
		glActiveTexture( GL_TEXTURE0 + unit );
		glBindTexture( GL_TEXTURE_2D, texture );
		++unit;
	}
	glActiveTexture( GL_TEXTURE0 );
}

void unbindTextures( int count )
{
	for( int unit = count - 1; unit >= 0; --unit )
	{
		glActiveTexture( GL_TEXTURE0 + unit );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	glActiveTexture( GL_TEXTURE0 );
}

/// Client-memory pixel transfers, whatever the host left bound (receipt's).
/// A host that leaves a pixel buffer object bound turns glTexSubImage2D's
/// pointer into an offset into it and glReadPixels into a write to it; a
/// host that left a row length set tears every row. Set for the call, put
/// back after.
struct ClientTransfer
{
	GLint unpackBuffer = 0, packBuffer = 0;
	GLint unpackRow = 0, packRow = 0, unpackAlign = 4, packAlign = 4;
	ClientTransfer()
	{
		glGetIntegerv( GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer );
		glGetIntegerv( GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer );
		glGetIntegerv( GL_UNPACK_ROW_LENGTH, &unpackRow );
		glGetIntegerv( GL_PACK_ROW_LENGTH, &packRow );
		glGetIntegerv( GL_UNPACK_ALIGNMENT, &unpackAlign );
		glGetIntegerv( GL_PACK_ALIGNMENT, &packAlign );
		glBindBuffer( GL_PIXEL_UNPACK_BUFFER, 0 );
		glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
		glPixelStorei( GL_UNPACK_ROW_LENGTH, 0 );
		glPixelStorei( GL_PACK_ROW_LENGTH, 0 );
		glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
		glPixelStorei( GL_PACK_ALIGNMENT, 4 );
	}
	~ClientTransfer()
	{
		glBindBuffer( GL_PIXEL_UNPACK_BUFFER, static_cast< GLuint >( unpackBuffer ) );
		glBindBuffer( GL_PIXEL_PACK_BUFFER, static_cast< GLuint >( packBuffer ) );
		glPixelStorei( GL_UNPACK_ROW_LENGTH, unpackRow );
		glPixelStorei( GL_PACK_ROW_LENGTH, packRow );
		glPixelStorei( GL_UNPACK_ALIGNMENT, unpackAlign );
		glPixelStorei( GL_PACK_ALIGNMENT, packAlign );
	}
};

/// The engine's workers: the machine's cores less one for the host, at most
/// eight (past that the per-frame thread start costs more than it saves).
int defaultThreads()
{
	const unsigned hc = std::thread::hardware_concurrency();
	return std::clamp( static_cast< int >( hc ) - 1, 1, 8 );
}
} // namespace

//---------------------------------------------------------------------------
Lowband::Lowband()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The noise and the dropouts move with the video frame: host time,
	//frame-relative.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults, chosen on Resolume's demo clips (AGENTS.md, "Decisions").
	// Filled BEFORE any declaration: SetParamInfof reads its default out of
	// GetFloatParameter.
	//---------------------------------------------------------------------
	params[ PT_RECORDING ]  = static_cast< float >( model::kHi8 );
	params[ PT_TAPE_NOISE ] = 0.6f;//24.4 dB: Hi8's highlights break up, its darks and a Video8 tape stay clean

	params[ PT_DROPOUTS ]   = 0.15f;
	params[ PT_DECK ]       = static_cast< float >( model::kVideo8 );
	params[ PT_STANDARD ]   = static_cast< float >( model::kPAL );
	params[ PT_HEAD_CLOG ]  = 0.4f;//0.06 um: 4.4 dB off Video8's sync tip, 8.0 dB off Hi8's peak white (PAL)
	params[ PT_SYNC_AGC ]   = 0.0f;
	params[ PT_MIX ]        = 1.0f;

	auto declareOptions = [ this ]( unsigned int id, const char* name, int count, const char* ( *nameAt )( int ) ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), nameAt( i ), static_cast< float >( i ) );
	};

	declareOptions( PT_RECORDING, "Recording", model::kFormatCount, controls::FormatName );
	SetParamInfof( PT_TAPE_NOISE, "Tape Noise", FF_TYPE_STANDARD );
	SetParamInfof( PT_DROPOUTS, "Dropouts", FF_TYPE_STANDARD );

	declareOptions( PT_DECK, "Deck", model::kFormatCount, controls::FormatName );
	declareOptions( PT_STANDARD, "Standard", model::kStandardCount, controls::StandardName );
	SetParamInfof( PT_HEAD_CLOG, "Head Clog", FF_TYPE_STANDARD );

	SetParamInfo( PT_SYNC_AGC, "Sync AGC", FF_TYPE_BOOLEAN, false );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//SetParamGroup collapses consecutive ids under one header, so each group
	//is a contiguous run of the enum.
	for( FFUInt32 i = PT_RECORDING; i <= PT_DROPOUTS; ++i )
		SetParamGroup( i, "Tape" );
	for( FFUInt32 i = PT_DECK; i <= PT_HEAD_CLOG; ++i )
		SetParamGroup( i, "Deck" );
	for( FFUInt32 i = PT_SYNC_AGC; i <= PT_MIX; ++i )
		SetParamGroup( i, "Monitor" );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Lowband effect" );
	diag::init();
}

//---------------------------------------------------------------------------
FFResult Lowband::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = shaders::Vertex();
	struct
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	} const stages[] = {
		{ &intakeShader, shaders::Intake(), "intake" },
		{ &displayShader, shaders::Display(), "display" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( vertex, stage.fragment ) )
			continue;
		//FF_FAIL is invisible to an operator: the effect simply does nothing.
		//This line is the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Lowband: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Lowband::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

//---------------------------------------------------------------------------
FFResult Lowband::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	//The host's viewport, before anything of ours changes it:
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	const int W = static_cast< int >( picture.Width );
	const int H = static_cast< int >( picture.Height );

	//---------------------------------------------------------------------
	// The settings.
	//---------------------------------------------------------------------
	EngineSettings s;
	s.standard         = controls::OptionIndex( params[ PT_STANDARD ], model::kStandardCount );
	s.recording        = controls::OptionIndex( params[ PT_RECORDING ], model::kFormatCount );
	s.deck             = controls::OptionIndex( params[ PT_DECK ], model::kFormatCount );
	s.noise            = !quiet && controls::NoiseOn( params[ PT_TAPE_NOISE ] );
	s.cnrDb            = controls::CnrDb( params[ PT_TAPE_NOISE ] );
	s.clogMetres       = controls::ClogMetres( params[ PT_HEAD_CLOG ] );
	s.dropoutsPerFrame = quiet ? 0.0 : controls::DropoutsPerFrame( params[ PT_DROPOUTS ] );
	s.syncAgc          = params[ PT_SYNC_AGC ] >= 0.5f;
	s.perturb          = perturb;
	s.forceDropout     = forceDropout;
	s.forced           = forcedDrop;
	const float mixAmount = controls::Amount( params[ PT_MIX ] );

	const model::Standard& standard = model::StandardOf( s.standard );
	const int P                     = standard.ActivePixels();
	const int N                     = standard.frameLines;

	//---------------------------------------------------------------------
	// The clock. A resize is not a reason to touch it (the negative control
	// makes it one).
	//---------------------------------------------------------------------
	const bool rasterChanged = lastWidth != 0 && ( lastWidth != W || lastHeight != H );
	lastWidth                = W;
	lastHeight               = H;
	if( rasterChanged && ( perturb & model::kPerturbResizeResetsClock ) )
		clock.Reset();
	clock.Update( hostTimeSeen ? hostTime : -1.0 );
	s.videoFrame = static_cast< int64_t >( std::floor( clock.Now() * standard.FrameRate() ) );
	lastSettings = s;

	//---------------------------------------------------------------------
	// Buffers. Every allocation here, before anything binds a texture:
	// FFGLFBO::Initialise sizes its colour texture under a scoped binding,
	// and every ffglex Scoped* binding CLEARS to 0 on exit.
	//---------------------------------------------------------------------
	if( !intake.Ensure( P, N, GL_RGBA32F, PassBuffer::Sampling::Nearest ) )
	{
		diag::error( "could not allocate the intake: " + std::to_string( P ) + " x " + std::to_string( N ) );
		return FF_FAIL;
	}
	if( linesTexture == 0 || linesW != P || linesH != N )
	{
		if( linesTexture != 0 )
			glDeleteTextures( 1, &linesTexture );
		glGenTextures( 1, &linesTexture );
		glBindTexture( GL_TEXTURE_2D, linesTexture );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, P, N, 0, GL_RGBA, GL_FLOAT, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glBindTexture( GL_TEXTURE_2D, 0 );
		linesW = P;
		linesH = N;
	}

	//---------------------------------------------------------------------
	// Intake: the host frame onto the standard's raster, and back to the CPU.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, intake.GetGLID() );
		intake.ResizeViewPort();
		ScopedShaderBinding shader( intakeShader.GetGLID() );
		bindTextures( { picture.Handle } );
		glUniform1i( loc( intakeShader, "Source" ), 0 );
		glUniform1i( loc( intakeShader, "HostW" ), W );
		glUniform1i( loc( intakeShader, "HostH" ), H );
		glUniform1i( loc( intakeShader, "Pixels" ), P );
		glUniform1i( loc( intakeShader, "Lines" ), N );
		quad.Draw();
		unbindTextures( 1 );
	}
	intakeData.resize( static_cast< size_t >( P ) * N * 4 );
	linesData.resize( intakeData.size() );
	{
		ClientTransfer transfer;
		glBindFramebuffer( GL_FRAMEBUFFER, intake.GetGLID() );
		glReadPixels( 0, 0, P, N, GL_RGBA, GL_FLOAT, intakeData.data() );
	}

	//---------------------------------------------------------------------
	// The tape and the deck, on the CPU.
	//---------------------------------------------------------------------
	const int threads = threadsForTest > 0 ? threadsForTest : defaultThreads();
	engine.Process( intakeData.data(), N, P, s, linesData.data(), threads );

	{
		ClientTransfer transfer;
		glBindTexture( GL_TEXTURE_2D, linesTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, P, N, GL_RGBA, GL_FLOAT, linesData.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	//---------------------------------------------------------------------
	// Display.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
		ScopedShaderBinding shader( displayShader.GetGLID() );
		bindTextures( { linesTexture, picture.Handle } );
		glUniform1i( loc( displayShader, "Lines" ), 0 );
		glUniform1i( loc( displayShader, "Source" ), 1 );
		glUniform1i( loc( displayShader, "HostW" ), W );
		glUniform1i( loc( displayShader, "HostH" ), H );
		glUniform1i( loc( displayShader, "LineCount" ), N );
		glUniform1i( loc( displayShader, "Pixels" ), P );
		glUniform1f( loc( displayShader, "MixAmount" ), mixAmount );
		quad.Draw();
		unbindTextures( 2 );
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Lowband::DeInitGL()
{
	intakeShader.FreeGLResources();
	displayShader.FreeGLResources();
	quad.Release();
	intake.Destroy();
	if( linesTexture != 0 )
	{
		glDeleteTextures( 1, &linesTexture );
		linesTexture = 0;
	}
	linesW = linesH = 0;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
size_t Lowband::StateBytesForTest() const
{
	size_t bytes = 0;
	if( intake.IsValid() )
		bytes += static_cast< size_t >( intake.GetWidth() ) * intake.GetHeight() * 16;
	bytes += static_cast< size_t >( linesW ) * linesH * 16;
	bytes += ( intakeData.size() + linesData.size() ) * sizeof( float );
	return bytes + engine.StateBytes();
}

//---------------------------------------------------------------------------
FFResult Lowband::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Lowband::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

char* Lowband::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Lowband::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only; it has to say so
	// successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}
