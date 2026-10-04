#pragma once

#include "Clock.h"
#include "Engine.h"
#include "Model.h"
#include "PassBuffer.h"

#include <FFGLSDK.h>

#include <string>
#include <vector>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

/**
	Lowband -- a Hi8 tape played back on a Video8 deck, as an FFGL effect.

	**The one idea.** Hi8 moved the luma's FM carrier up (sync tip 5.7 MHz,
	white 7.7 MHz, 2.0 MHz of deviation, against Video8's 4.2, 5.4 and 1.2)
	and left the colour-under chroma where it was. A Video8 deck reads the
	Hi8 carrier through a channel and a demodulator built for its own, so the
	contrast comes out 5/3 too high, the mismatched emphasis smears a glow to
	the right of every highlight and lifts the black at the clamp, bright
	edges throw black streaks where their overshoot leaves the deck's band,
	and with any tape noise the highlights break up first -- their carrier
	sits furthest outside the band. The colour comes through.

	The luma is a real FM signal: modulated, filtered, limited and
	demodulated by counting zero crossings, at 40.5 MHz, on the CPU, on
	worker threads (Engine.h). The GPU brings the host frame onto the
	standard's raster and takes the deck's picture back.

	Time is frame-relative: the clock is seconds since the first frame, in
	double (clamp's Clock), and the only thing that reads it is the video
	frame (25 or 29.97 Hz) that seeds the noise and the dropouts. Nothing is
	carried from one frame to the next, so a resize has nothing to lose.
*/
class Lowband : public CFFGLPlugin
{
public:
	Lowband();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- test hooks. Read by lbtest; the plugin's own operation never uses
	//--- them, and every one is off outside the harness.

	/// The harness DECLARES its clock unit rather than leaving the voting to
	/// infer one: it renders as fast as the GPU allows.
	void SetClockScaleForTest( double scale )
	{
		clock.SetScaleForTest( scale );
	}
	/// Negative-control hooks, a bitmask of `model::Perturb`.
	void SetPerturbForTest( int bits )
	{
		perturb = bits;
	}
	/// No tape noise and no dropouts, whatever the controls say.
	void SetQuietForTest( bool on )
	{
		quiet = on;
	}
	/// Worker threads for the engine (0: the plugin's own choice).
	void SetThreadsForTest( int n )
	{
		threadsForTest = n;
	}
	/// One dropout, on a frame line, over active-line time.
	void SetDropoutForTest( bool on, int line, double us0, double us1 )
	{
		forceDropout = on;
		forcedDrop   = { line, us0, us1 };
	}
	/// The video frame the last render seeded its noise with.
	int64_t LastVideoFrameForTest() const
	{
		return lastSettings.videoFrame;
	}
	const lowband::EngineSettings& LastSettingsForTest() const
	{
		return lastSettings;
	}
	lowband::Engine& EngineForTest()
	{
		return engine;
	}
	/// The last frame's intake (P x N x 4: Y', U, V, alpha) and the deck's
	/// picture as uploaded (P x N x 4: Y', U, V, 1), line 0 first.
	const std::vector< float >& IntakeForTest() const
	{
		return intakeData;
	}
	const std::vector< float >& LinesForTest() const
	{
		return linesData;
	}
	/// Bytes held between frames: GPU buffers and the CPU copies.
	size_t StateBytesForTest() const;

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Tape
		PT_RECORDING,
		PT_TAPE_NOISE,
		PT_DROPOUTS,

		//Deck
		PT_DECK,
		PT_STANDARD,
		PT_HEAD_CLOG,

		//Monitor
		PT_SYNC_AGC,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	ffglex::FFGLShader intakeShader;
	ffglex::FFGLShader displayShader;
	ffglex::FFGLScreenQuad quad;

	lowband::PassBuffer intake;///< P x N, RGBA32F: the host frame on the standard's raster
	GLuint linesTexture = 0;   ///< P x N, RGBA32F: the deck's picture, uploaded
	int linesW = 0, linesH = 0;

	std::vector< float > intakeData;
	std::vector< float > linesData;
	lowband::Engine engine;
	lowband::EngineSettings lastSettings;

	lowband::Clock clock;
	bool hostTimeSeen = false;
	int lastWidth = 0, lastHeight = 0;

	int perturb         = 0;
	bool quiet          = false;
	int threadsForTest  = 0;
	bool forceDropout   = false;
	lowband::model::Dropout forcedDrop{};

	/// Zero-initialised: the About block's ids are never stored to.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
