#pragma once

#include <complex>
#include <vector>

/**
	The filters the signal chain is made of, and nothing else.

	Every filter here is designed in double and RUN in float: the chain runs
	at 40.5 MHz over 1.5 million samples a frame, and float is what makes that
	affordable. Designs are the textbook ones -- Butterworth through the
	bilinear transform with the corner prewarped, and a first-order shelf
	through the bilinear transform unwarped (its two corners cannot both be
	prewarped; at 40.5 MHz the warp at the highest corner used, 1.28 MHz, is
	0.2 %) -- so that the harness can compute the same responses by a
	different route (the analogue prototype, evaluated at s = j omega) and
	compare.

	A Butterworth low-pass or high-pass of even order is a cascade of biquads;
	the shelf is one first-order section. All run Direct Form II transposed.
*/
/// No aliasing between the pointers so marked: clang and MSVC both spell it
/// __restrict. Without it the lane loops are not vectorised.
#define LB_RESTRICT __restrict

namespace lowband::dsp
{

struct Biquad
{
	double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

	/// The response at omega (radians a sample).
	std::complex< double > Response( double omega ) const;
	/// The group delay at omega, in samples.
	double GroupDelay( double omega ) const;
	double DcGain() const
	{
		return ( b0 + b1 + b2 ) / ( 1.0 + a1 + a2 );
	}
};

/// A cascade, run in float. The state lives with the caller (one per line in
/// flight), so the coefficients can be shared between threads.
struct Cascade
{
	std::vector< Biquad > sections;
	/// float copies, five a section: b0 b1 b2 a1 a2.
	std::vector< float > coeffs;

	void Finalise();
	std::complex< double > Response( double omega ) const;
	double GroupDelay( double omega ) const;
	double DcGain() const;
	int StateSize() const
	{
		return 2 * static_cast< int >( sections.size() );
	}

	/// The state that gives a constant output for a constant input x.
	void SteadyState( float x, float* state ) const;

	//--- eight independent lines in lock step, one per lane: the same
	//--- arithmetic as Step, per lane, so a lane's result does not depend on
	//--- which lanes it shares a batch with. State is [ section ][ 2 ][ lane ].
	static constexpr int kLanes = 8;
	int StateSizeLanes() const
	{
		return 2 * kLanes * static_cast< int >( sections.size() );
	}
	void SteadyStateLanes( const float* x, float* state ) const;
	/// Run the cascade over a whole [ sample ][ lane ] array in place,
	/// starting from `state` and leaving the final state there. The state is
	/// held in registers across the array (the section count is a template
	/// argument underneath), which is what makes an IIR chain at 40.5 MHz
	/// affordable: through memory, every sample's recurrence waits on a store.
	void RunLanes( float* data, int total, float* state ) const;
	/// The cascade followed by another, as one.
	Cascade Then( const Cascade& next ) const;
	inline void StepLanes( float* x, float* state ) const
	{
		const float* c = coeffs.data();
		const int n    = static_cast< int >( sections.size() );
		for( int i = 0; i < n; ++i, c += 5, state += 2 * kLanes )
		{
			const float b0 = c[ 0 ], b1 = c[ 1 ], b2 = c[ 2 ], a1 = c[ 3 ], a2 = c[ 4 ];
			for( int k = 0; k < kLanes; ++k )
			{
				const float y        = b0 * x[ k ] + state[ k ];
				state[ k ]           = b1 * x[ k ] - a1 * y + state[ kLanes + k ];
				state[ kLanes + k ]  = b2 * x[ k ] - a2 * y;
				x[ k ]               = y;
			}
		}
	}
	/// One sample through the whole cascade.
	inline float Step( float x, float* state ) const
	{
		const float* c = coeffs.data();
		const int n    = static_cast< int >( sections.size() );
		for( int i = 0; i < n; ++i, c += 5, state += 2 )
		{
			const float y = c[ 0 ] * x + state[ 0 ];
			state[ 0 ]    = c[ 1 ] * x - c[ 3 ] * y + state[ 1 ];
			state[ 1 ]    = c[ 2 ] * x - c[ 4 ] * y;
			x             = y;
		}
		return x;
	}
};

/// Butterworth low-pass, even order, corner (-3 dB) at fc, sampled at fs.
Cascade ButterworthLowPass( int order, double fc, double fs );
/// Butterworth high-pass, even order.
Cascade ButterworthHighPass( int order, double fc, double fs );
/// The emphasis shelf ( 1 + s tau ) / ( 1 + s tau / x ): unity at DC, x at
/// high frequency, corners 1 / ( 2 pi tau ) and x / ( 2 pi tau ). Inverse
/// swaps numerator and denominator: the de-emphasis.
Cascade Shelf( double tau, double x, double fs, bool inverse );

/// The analogue prototypes the designs come from, at frequency f (Hz): what
/// the harness compares the digital filters against.
std::complex< double > AnalogueButterworth( int order, double fc, double f, bool highPass );
std::complex< double > AnalogueShelf( double tau, double x, double f, bool inverse );

} // namespace lowband::dsp
