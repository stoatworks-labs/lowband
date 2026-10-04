#pragma once

#include "Dsp.h"
#include "Model.h"

#include <cstdint>
#include <vector>

/**
	The signal chain, on the CPU, one line at a time.

	A line of the picture becomes a line of composite luma (sync, blanking,
	picture), is recorded as an FM carrier by the TAPE's format, and is played
	back through a DECK's channel and demodulator:

	    picture (13.5 MHz) --linear--> 40.5 MHz
	    record:  Y low-pass -> pre-emphasis -> white/dark clip -> FM
	    tape:    head clog (Wallace) -> dropouts -> head-amplifier noise
	    deck:    RF high-pass -> RF low-pass -> limiter -> pulse count
	             -> Y low-pass (removes 2f) -> the deck's map to volts
	             -> de-emphasis
	    monitor: decimate to 13.5 MHz (3-sample box, the deck's own delay
	             compensated) -> back-porch clamp -> sync AGC (optional)
	    chroma:  U, V band-limited to the colour-under band (zero phase)

	The deck demodulates in the mode `model::PlaybackMode` gives it: a Video8
	deck always in Video8's, so a Hi8 tape's 5.7-7.7 MHz carrier is read by a
	channel and a map built for 4.2-5.4 MHz.

	**Lines are independent.** Each line starts from steady state, after a
	4 us warm-up taken from the line before it in time -- the previous line
	of the SAME field, two frame rows up -- so a line's output depends on its
	own picture, that line's, and its seed, and on nothing else. The lines are
	shared between worker threads and the result is the same for any number
	of them (`lbtest --threads`).

	**Randomness is seeded by (video frame, line)**, the video frame counted at
	the standard's 25 or 29.97 Hz, so a frame of tape is the same picture
	every time it is played.
*/
namespace lowband
{

struct EngineSettings
{
	int standard  = model::kPAL;
	int recording = model::kHi8;
	int deck      = model::kVideo8;
	bool noise    = true;
	double cnrDb  = 28.0;
	double clogMetres      = 0.0;
	double dropoutsPerFrame = 0.0;
	bool syncAgc  = false;
	int64_t videoFrame = 0;
	int perturb   = 0;

	//Test hooks.
	bool forceDropout = false;
	model::Dropout forced{};
};

class Engine
{
public:
	/// `in`: lines x pixels x 4 floats (Y', U, V, -), frame row 0 first.
	/// `out`: the same layout, ( Y', U, V, 1 ). `pixels` must be the
	/// standard's ActivePixels() and `lines` its frameLines.
	void Process( const float* in, int lines, int pixels, const EngineSettings& s, float* out, int threads );

	/// The deck's own luma delay, in 40.5 MHz samples, that the monitor stage
	/// takes back out (for the harness).
	double DelaySamples() const
	{
		return delay;
	}
	/// The last frame's mean sync depth (porch minus tip), volts on the deck's
	/// scale, and the gain the monitor applied.
	double LastSyncDepth() const
	{
		return lastDepth;
	}
	double LastGain() const
	{
		return lastGain;
	}
	/// The last frame's mean porch level, before the clamp took it out.
	double LastPorch() const
	{
		return lastPorch;
	}

	/// The tape's RF for one line of constant level `y` (0..1 of white), at
	/// 40.5 MHz, record chain only, after the warm-up: for --carrier.
	std::vector< float > RecordFlatForTest( const EngineSettings& s, double y );
	/// The demodulated, de-emphasised line before the monitor stage, at
	/// 40.5 MHz, for a picture line `row` of the last Process call's input.
	std::vector< float > DeckLineForTest( const float* in, int lines, int pixels, const EngineSettings& s, int row );

	/// Bytes the engine holds between frames (filter designs only).
	size_t StateBytes() const;

private:
	struct Chain
	{
		int standard = -1, recording = -1, deck = -1, perturb = -1;
		double clog  = -1.0;
		dsp::Cascade recordY, preEmphasis, record;///< record = recordY then preEmphasis
		dsp::Cascade rfHigh, rfLow, rf;            ///< rf = rfHigh then rfLow
		dsp::Cascade yLow, deEmphasis, playback;   ///< playback = yLow then deEmphasis
		dsp::Cascade chroma;
		std::vector< float > clogTaps;
		double recordTipHz = 0, recordDevHz = 0;
		double deckTipHz = 0, deckDevHz = 0;
		double whiteClipV = 0, darkClipV = 0;
	};

	/// Eight lines in lock step, one a lane; arrays are [ sample ][ lane ].
	static constexpr int kLanes = dsp::Cascade::kLanes;
	struct Scratch
	{
		std::vector< float > a, b, pulses, y, padded;
		std::vector< float > state;
		std::vector< float > chroma;
		int total = 0, zero = 0;
	};

	struct LineResult
	{
		double porch = 0.0, tip = 0.0;
	};

	void configure( const EngineSettings& s );
	/// The record half for a batch of lines: each line's composite luma (with the
	/// warm-up from the line before it in its field, and the next line's
	/// sync) through the camcorder, as an FM carrier in `sc.b`. Null picture
	/// pointers are lines with no picture (the vertical interval).
	void recordBatch( const float* const* pix, const float* const* prev, int pixels, const EngineSettings& s, Scratch& sc, float* firstHz ) const;
	/// The whole chain for frame rows row0 .. row0 + kLanes - 1 into `sc.y` (40.5 MHz,
	/// deck volts, [ sample ][ lane ]); sample 0 of each line at `sc.zero`.
	void runBatch( const float* in, int lines, int pixels, const EngineSettings& s, int row0, const int* seedKeys,
	               const std::vector< model::Dropout >& drops, Scratch& sc ) const;
	void monitorLane( const Scratch& sc, int lane, int pixels, const model::Standard& st, float* outY, LineResult& r ) const;
	void chromaBatch( const float* in, int row0, int lines, int pixels, float* out, Scratch& sc ) const;

	Chain chain;
	double delay     = 0.0;
	double lastDepth = 0.0, lastGain = 1.0, lastPorch = 0.0;
	std::vector< LineResult > results;
	std::vector< float > lumaOut;
};

/// The warm-up and the tail each line is processed with, us.
constexpr double kWarmUs = 4.0;
constexpr double kTailUs = 2.0;
/// The clamp's window in the back porch and the sync-tip window, us.
constexpr double kPorchFromSyncEndUs = 1.0;
constexpr double kPorchBeforeActiveUs = 0.5;
constexpr double kTipInsetUs = 0.8;

} // namespace lowband
