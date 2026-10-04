#include "Model.h"

#include <algorithm>
#include <cmath>

namespace lowband::model
{
namespace
{
/**
	The two systems: colourunder's table (from ITU-R BT.470-6 and BT.1700;
	SMPTE 170M). 625/50: line 64 us, 1.65 us front porch, 4.7 us sync, 5.7 us
	back porch, so 51.95 us active, 576 lines. 525/59.94: 1001 / 15.75 us,
	1.5, 4.7, 4.7 us, so 52.6556 us active, 480 lines. Levels in volts of a
	1 V sync-tip-to-white signal: PAL 0.3 / 0.7; NTSC 40 / 100 IRE of 140, no
	set-up (the Japanese convention; set-up would only move black).

	The 8 mm colour-under carriers: PAL 46.875 fH = 732.42 kHz, NTSC 47.25 fH =
	743.44 kHz (vhs-decode's format_defs/video8.py; Sencore Tech Tip 189 gives
	743 kHz for both 8 mm and Hi-8).
*/
constexpr double kFhPal  = 15625.0;
constexpr double kFhNtsc = 15750000.0 / 1001.0;

const Standard kStandards[ kStandardCount ] = {
	{ "625/50 (PAL)", 64.0, 1.65, 4.7, 5.7, 576, 25, 1, 0.3, 0.7, 46.875 * kFhPal },
	{ "525/59.94 (NTSC)", 1001.0 / 15.75, 1.5, 4.7, 4.7, 480, 30000, 1001, 40.0 / 140.0, 100.0 / 140.0, 47.25 * kFhNtsc },
};

/**
	The two formats.

	FM carriers: Sencore Tech Tip 189 ("Comparison of VCR formats"): 8 mm sync
	tip 4.2 MHz, peak white 5.4 MHz, 1.2 MHz deviation; Hi-8 5.7 / 7.7 MHz,
	2.0 MHz. Wikipedia's Hi8 and 8 mm articles agree, and vhs-decode uses the
	same figures for PAL and NTSC.

	Emphasis: vhs-decode gives Video8 a time constant of 1.30 us ("1.25~1.35us",
	the same as VHS) and Hi8 0.47 us ("From spec"), and the same shelf gain to
	both, 11.5794 dB. The shelf's corners from those: Video8 122 kHz and
	464 kHz, Hi8 339 kHz and 1.28 MHz.

	The white clip at 220 % of sync-to-white above the tip, and the dark clip
	at 90 % below blanking: vhs-decode's comment on Hi8 ("white clip 220% -
	10.1mhz?", "dark clip 90%"), read that way, and applied to Video8 too by
	assumption.

	The deck's RF band: vhs-decode's filters for a capture of each format's RF
	(Video8: 1.9 MHz high-pass, the band-pass ending at 7.0 MHz; Hi8: 1.85 MHz
	and 10.31 MHz). They are a decoder's settings, standing in for a deck's
	head, equaliser and RF filters, which no source here describes. The Y
	low-pass after the demodulator: vhs-decode's, 3.5 MHz and 5.0 MHz. The
	recording's luma low-pass is assumed to be the same band.
*/
const double kShelfX = 3.786301;//10^( 11.5794 / 20 )

const Format kFormats[ kFormatCount ] = {
	{ "Video8", 4.2e6, 1.2e6, 1.30e-6, kShelfX, 3.5e6, 2.2, 0.9, 1.9e6, 7.0e6, 3.5e6 },
	{ "Hi8", 5.7e6, 2.0e6, 0.47e-6, kShelfX, 5.0e6, 2.2, 0.9, 1.85e6, 10.3e6, 5.0e6 },
};

/// Peter Acklam's rational approximation to the normal quantile, then one
/// Halley step against erfc: good to double precision for the table's range.
double normalQuantile( double p )
{
	static const double a[] = { -3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02, 1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00 };
	static const double b[] = { -5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01 };
	static const double c[] = { -7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00, -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00 };
	static const double d[] = { 7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00 };
	const double low = 0.02425;
	double x;
	if( p < low )
	{
		const double q = std::sqrt( -2.0 * std::log( p ) );
		x = ( ( ( ( ( c[ 0 ] * q + c[ 1 ] ) * q + c[ 2 ] ) * q + c[ 3 ] ) * q + c[ 4 ] ) * q + c[ 5 ] ) / ( ( ( ( d[ 0 ] * q + d[ 1 ] ) * q + d[ 2 ] ) * q + d[ 3 ] ) * q + 1.0 );
	}
	else if( p > 1.0 - low )
	{
		const double q = std::sqrt( -2.0 * std::log( 1.0 - p ) );
		x = -( ( ( ( ( c[ 0 ] * q + c[ 1 ] ) * q + c[ 2 ] ) * q + c[ 3 ] ) * q + c[ 4 ] ) * q + c[ 5 ] ) / ( ( ( ( d[ 0 ] * q + d[ 1 ] ) * q + d[ 2 ] ) * q + d[ 3 ] ) * q + 1.0 );
	}
	else
	{
		const double q = p - 0.5;
		const double r = q * q;
		x = ( ( ( ( ( a[ 0 ] * r + a[ 1 ] ) * r + a[ 2 ] ) * r + a[ 3 ] ) * r + a[ 4 ] ) * r + a[ 5 ] ) * q / ( ( ( ( ( b[ 0 ] * r + b[ 1 ] ) * r + b[ 2 ] ) * r + b[ 3 ] ) * r + b[ 4 ] ) * r + 1.0 );
	}
	const double e = 0.5 * std::erfc( -x / std::sqrt( 2.0 ) ) - p;
	const double u = e * std::sqrt( 2.0 * kPi ) * std::exp( x * x / 2.0 );
	return x - u / ( 1.0 + x * u / 2.0 );
}

uint32_t low32( int64_t v )
{
	return static_cast< uint32_t >( static_cast< uint64_t >( v ) & 0xffffffffu );
}

uint32_t high32( int64_t v )
{
	return static_cast< uint32_t >( static_cast< uint64_t >( v ) >> 32 );
}

constexpr uint32_t kNoiseSeed = 0x4C420001u;
constexpr uint32_t kDropSeed  = 0x4C420002u;
} // namespace

int Standard::LineSamples() const
{
	return static_cast< int >( std::lround( line * kSampleHz * 1e-6 ) );
}

int Standard::SyncSamples() const
{
	return static_cast< int >( std::lround( sync * kSampleHz * 1e-6 ) );
}

int Standard::ActiveStart() const
{
	return static_cast< int >( std::floor( ( sync + backPorch ) * kSampleHz * 1e-6 ) );
}

int Standard::ActivePixels() const
{
	return static_cast< int >( std::lround( Active() * kPixelHz * 1e-6 ) );
}

double Standard::WritingSpeed() const
{
	//Two heads 180 degrees apart, one field each: one turn a frame.
	return kPi * 0.040 * FrameRate();
}

const Standard& StandardOf( int index )
{
	return kStandards[ std::clamp( index, 0, kStandardCount - 1 ) ];
}

const Format& FormatOf( int index )
{
	return kFormats[ std::clamp( index, 0, kFormatCount - 1 ) ];
}

int PlaybackMode( int deck, int recording )
{
	return deck == kHi8 ? std::clamp( recording, 0, kFormatCount - 1 ) : kVideo8;
}

double ReferenceBandHz()
{
	return kFormats[ kVideo8 ].rfLowPassHz - kFormats[ kVideo8 ].rfHighPassHz;
}

double NoiseSigma( double cnrDb )
{
	//Carrier power 1/2; white noise of variance sigma^2 puts sigma^2 B / ( fs / 2 )
	//of it in a band B.
	const double cnr = std::pow( 10.0, cnrDb / 10.0 );
	return std::sqrt( 0.5 * ( kSampleHz / 2.0 ) / ( ReferenceBandHz() * cnr ) );
}

double SpacingLossDb( double spacingMetres, double fHz, double speed )
{
	//20 log10( e^( 2 pi d / lambda ) ) = 54.575 d / lambda.
	return 20.0 / std::log( 10.0 ) * 2.0 * kPi * spacingMetres * fHz / speed;
}

std::vector< float > ClogTaps( double spacingMetres, double speed, int halfLength )
{
	if( spacingMetres <= 0.0 )
		return {};
	//h[ n ] = ( 2 / fs ) int_0^{fs/2} e^{-a f} cos( 2 pi f n / fs ) df, a = 2 pi d / v:
	//closed form a ( 1 - (-1)^n e^{-a fs/2} ) / ( a^2 + ( 2 pi n / fs )^2 ).
	const double a  = 2.0 * kPi * spacingMetres / speed;
	const double fs = kSampleHz;
	const double eB = std::exp( -a * fs / 2.0 );
	std::vector< float > taps( static_cast< size_t >( 2 * halfLength + 1 ) );
	for( int n = -halfLength; n <= halfLength; ++n )
	{
		const double b    = 2.0 * kPi * n / fs;
		const double sign = ( n & 1 ) ? -1.0 : 1.0;
		const double h    = ( 2.0 / fs ) * a * ( 1.0 - sign * eB ) / ( a * a + b * b );
		taps[ static_cast< size_t >( n + halfLength ) ] = static_cast< float >( h );
	}
	return taps;
}

std::vector< Dropout > Dropouts( int64_t videoFrame, double perFrame, int lines, double activeUs )
{
	std::vector< Dropout > out;
	if( perFrame <= 0.0 )
		return out;
	const uint32_t seed = Hash( low32( videoFrame ) ^ Hash( high32( videoFrame ) + kDropSeed ) );
	int count           = static_cast< int >( std::floor( perFrame ) );
	if( HashUnit( Hash( seed ^ 0x51u ) ) < perFrame - std::floor( perFrame ) )
		++count;
	count = std::min( count, kMaxDropouts );
	for( int i = 0; i < count; ++i )
	{
		const uint32_t h = Hash( seed + 0x9E3779B9u * static_cast< uint32_t >( i + 1 ) );
		Dropout d;
		d.line            = static_cast< int >( Hash( h ^ 1u ) % static_cast< uint32_t >( lines ) );
		d.us0             = HashUnit( Hash( h ^ 2u ) ) * activeUs;
		//Log-uniform from 1 to 25 us: most are short, a few cross a third of the line.
		const double len = std::exp( std::log( 1.0 ) + HashUnit( Hash( h ^ 3u ) ) * ( std::log( 25.0 ) - std::log( 1.0 ) ) );
		d.us1             = std::min( activeUs, d.us0 + len );
		out.push_back( d );
	}
	return out;
}

double HashUnit( uint32_t h )
{
	return static_cast< double >( h >> 8 ) * ( 1.0 / 16777216.0 );
}

const float* GaussianTable()
{
	static const std::vector< float > table = [] {
		std::vector< float > t( size_t( 1 ) << kGaussianBits );
		const double n = static_cast< double >( t.size() );
		for( size_t i = 0; i < t.size(); ++i )
			t[ i ] = static_cast< float >( normalQuantile( ( static_cast< double >( i ) + 0.5 ) / n ) );
		return t;
	}();
	return table.data();
}

uint32_t LineSeed( int64_t videoFrame, int line )
{
	const uint32_t f = Hash( low32( videoFrame ) ^ Hash( high32( videoFrame ) + kNoiseSeed ) );
	return Hash( f ^ Hash( static_cast< uint32_t >( line ) * 0x85EBCA6Bu + 0x27D4EB2Fu ) );
}

} // namespace lowband::model
