#pragma once

#include <cstdint>
#include <vector>

/**
	The two formats and the two standards, as numbers. No GL, no FFGL: the
	plugin and the harness's offline checks both link this. AGENTS.md and
	ATTRIBUTIONS.md say where every number comes from and which are assumed.

	**The one idea.** Hi8 is Video8 with the luma's FM carrier moved up --
	sync tip 5.7 MHz and peak white 7.7 MHz (2.0 MHz of deviation) where Video8
	has 4.2 and 5.4 MHz (1.2 MHz) -- and a shorter emphasis time constant. The
	colour-under chroma stayed where it was. A Video8 deck reads a Hi8 tape
	through a channel and a demodulator built for its own carrier, and the
	picture that comes out is the subject of this plugin. See Engine.h for the
	chain.

	**Two rates.** The picture is held at 13.5 MHz (BT.601: 701 pixels of
	PAL's 51.95 us active line, 711 of NTSC's 52.66 us). The FM chain runs at
	three times that, 40.5 MHz: four to five samples a cycle of Hi8's peak
	white, and an exact 2592 (PAL) or 2574 (NTSC) samples a line.
*/
namespace lowband::model
{

constexpr double kPi = 3.14159265358979323846;

/// The FM chain's rate and the picture's.
constexpr double kSampleHz = 40.5e6;
constexpr double kPixelHz  = 13.5e6;
constexpr int kOversample  = 3;

/// Negative-control hooks: a bitmask the shipped plugin always carries at 0.
enum Perturb : int
{
	kPerturbNone              = 0,
	kPerturbDeckDeviation     = 1 << 0,///< the Video8 deck demodulates with the TAPE's map (--gain must fail)
	kPerturbDeckEmphasis      = 1 << 1,///< the Video8 deck de-emphasises with the TAPE's tau (--gain's black and --emphasis must fail)
	kPerturbWideBand          = 1 << 2,///< the Video8 deck's RF band as wide as a Hi8 deck's (--threshold and --streak must fail)
	kPerturbWrongSpeed        = 1 << 3,///< the head clog at the other standard's writing speed (--wallace must fail)
	kPerturbNoDelay           = 1 << 4,///< the deck's own luma delay left uncompensated (--registration must fail)
	kPerturbDropoutLine       = 1 << 5,///< a dropout lands two frame lines below where it was put (--dropout must fail)
	kPerturbSeedByWorker      = 1 << 6,///< the noise seeded by worker, not by line (--threads must fail)
	kPerturbResizeResetsClock = 1 << 7,///< a resize restarts the clock (the photofinish class of bug)
	kPerturbHi8AtVideo8Tip    = 1 << 8,///< Hi8 written from Video8's sync tip (--carrier must fail)
	kPerturbRisingOnly        = 1 << 9,///< the demodulator counts only rising crossings (--matched must fail; a
	                                  ///< carrier offset would not: the back-porch clamp takes any DC out)
};

//---------------------------------------------------------------------------
// The two systems (colourunder's table: BT.470-6, BT.1700, SMPTE 170M).
//---------------------------------------------------------------------------
enum StandardIndex
{
	kPAL  = 0,
	kNTSC = 1,
	kStandardCount
};

struct Standard
{
	const char* name;
	double line;        ///< line period, us
	double frontPorch;  ///< us
	double sync;        ///< us
	double backPorch;   ///< us
	int frameLines;     ///< active lines in a frame (two fields)
	int64_t frameNum;   ///< frames per second, as num / den
	int64_t frameDen;
	double syncVolts;   ///< sync depth below blanking: 0.3 V (PAL), 40 IRE of 140 (NTSC)
	double whiteVolts;  ///< white above blanking: 0.7 V, 100 IRE of 140
	double colourUnderHz;///< the 8 mm colour-under carrier (informational: chroma is band-limited, not heterodyned)

	double Active() const
	{
		return line - frontPorch - sync - backPorch;
	}
	double FrameRate() const
	{
		return static_cast< double >( frameNum ) / static_cast< double >( frameDen );
	}
	/// Samples a line at kSampleHz: 2592 (PAL), 2574 (NTSC).
	int LineSamples() const;
	/// The sync pulse's length in samples. Sample 0 is the sync's leading edge.
	int SyncSamples() const;
	/// The first sample of the active picture.
	int ActiveStart() const;
	/// Pixels of the active picture at kPixelHz: 701 (PAL), 711 (NTSC).
	int ActivePixels() const;
	/// The head drum's writing speed, m/s: a 40 mm drum, one turn a frame.
	double WritingSpeed() const;
};

const Standard& StandardOf( int index );

//---------------------------------------------------------------------------
// The two formats. Both options in the plugin list them in this order.
//---------------------------------------------------------------------------
enum FormatIndex
{
	kVideo8 = 0,
	kHi8    = 1,
	kFormatCount
};

struct Format
{
	const char* name;
	//The recording.
	double syncTipHz;    ///< the FM carrier at the sync tip
	double deviationHz;  ///< sync tip to peak white
	double emphasisTau;  ///< s: the main emphasis time constant
	double emphasisX;    ///< the shelf's high-frequency gain (linear)
	double recordYHz;    ///< the camcorder's luma low-pass before the modulator (assumed = the deck's Y band)
	double whiteClip;    ///< the white clip, a fraction of sync-to-white above the tip
	double darkClip;     ///< the dark clip, a fraction of blanking-to-white below blanking
	//A deck playing in this format's mode.
	double rfHighPassHz; ///< the RF band's lower edge (keeps the colour-under band and AFM out)
	double rfLowPassHz;  ///< its upper edge
	double yLowPassHz;   ///< the demodulator's output low-pass: the Y band and the 2f carrier's removal

	double PeakWhiteHz() const
	{
		return syncTipHz + deviationHz;
	}
};

const Format& FormatOf( int index );

/// The format a deck demodulates a tape as: a Hi8 deck detects a Video8
/// recording and switches to Video8 (Wikipedia: "All Hi8 equipment can record
/// and play in the legacy Video8 format"); a Video8 deck has one mode.
int PlaybackMode( int deck, int recording );

/// Filter orders (assumed; AGENTS.md "Decisions").
constexpr int kRecordYOrder = 4;
constexpr int kRfLowOrder   = 8;
constexpr int kRfHighOrder  = 2;
constexpr int kYLowOrder    = 8;

/// The chroma's half-amplitude frequency (the colour-under band; colourunder's
/// 0.5 MHz, the same for 8 mm, which carries a ~500 kHz band under 743 kHz).
constexpr double kChromaHalfHz = 0.5e6;

//---------------------------------------------------------------------------
// The tape.
//---------------------------------------------------------------------------
/// Tape Noise sets the carrier-to-noise ratio, in dB, measured over this
/// reference band: the Video8 deck's own (1.9 to 7.0 MHz). The noise density
/// is the tape's and the head amplifier's, so it is the same whichever deck
/// or mode reads it.
double ReferenceBandHz();
/// The per-sample standard deviation of white noise for this CNR, against a
/// carrier of amplitude 1.
double NoiseSigma( double cnrDb );

/// Wallace's spacing loss: exp( -2 pi d / lambda ), lambda = v / f. In dB,
/// 54.6 d / lambda.
double SpacingLossDb( double spacingMetres, double fHz, double speed );
/// The zero-phase FIR that realises it, from the band-limited closed form
/// (2 M + 1 taps, centre at index M). Empty for d = 0.
std::vector< float > ClogTaps( double spacingMetres, double speed, int halfLength );
constexpr int kClogHalfTaps = 24;

/// A dropout: the carrier dipped over [ s0, s1 ) samples of a frame line.
struct Dropout
{
	int line;
	double us0, us1;///< from the start of the active picture
};
constexpr int kMaxDropouts = 64;
/// The carrier's floor inside a dropout (-30 dB) and the edges' length.
constexpr double kDropoutDepth  = 0.0316227766;
constexpr double kDropoutEdgeUs = 0.3;
std::vector< Dropout > Dropouts( int64_t videoFrame, double perFrame, int lines, double activeUs );

//---------------------------------------------------------------------------
// Randomness: the fleet's integer hash, and a Gaussian table read by it.
//---------------------------------------------------------------------------
/// PCG's output permutation (the fleet's hashInt). Inline: the noise calls
/// it for every sample of every line.
inline uint32_t Hash( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
double HashUnit( uint32_t h );
/// 65,536 quantiles of the unit normal, at ( i + 1/2 ) / 65536.
const float* GaussianTable();
constexpr int kGaussianBits = 16;
uint32_t LineSeed( int64_t videoFrame, int line );

} // namespace lowband::model
