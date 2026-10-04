#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <thread>

namespace lowband
{
namespace
{
using model::kSampleHz;
constexpr int L4 = dsp::Cascade::kLanes;

/// cos( 2 pi p ) for p in [ 0, 1 ): folded to a quarter turn, then the Taylor
/// series to the tenth power (error under 5e-7, below a float's step at 1).
inline float cosTurns( float p )
{
	const float x  = p - 0.5f;//cos( 2 pi p ) = -cos( 2 pi x )
	const float ax = std::fabs( x );
	const bool folded = ax > 0.25f;
	const float a  = folded ? 0.5f - ax : ax;//cos( pi - t ) = -cos( t )
	const float t  = 6.28318530717958647692f * a;
	const float t2 = t * t;
	const float c  = 1.0f + t2 * ( -0.5f + t2 * ( 1.0f / 24.0f + t2 * ( -1.0f / 720.0f + t2 * ( 1.0f / 40320.0f + t2 * ( -1.0f / 3628800.0f ) ) ) ) );
	return folded ? c : -c;
}

int samples( double us )
{
	return static_cast< int >( std::lround( us * kSampleHz * 1e-6 ) );
}

/// The composite luma of one line over its own samples [ from, to ), with
/// 0 <= from <= to <= L and k = 0 its sync's leading edge, in volts, into
/// dst[ ( k - from ) * stride ]: sync, blanking, and the picture interpolated
/// linearly between pixel centres at activeStart + 3 i + 1 (held flat over
/// the first pixel's left third and the last's right third). `pix` is the
/// line's Y' (stride 4), or null for a line with no picture (the vertical
/// interval).
void composite( const float* pix, int pixels, int syncN, int a0, float S, float Wh, int from, int to, float* dst, int stride )
{
	auto level = [ & ]( int i ) {
		return Wh * std::clamp( pix[ 4 * std::clamp( i, 0, pixels - 1 ) ], 0.0f, 1.0f );
	};
	for( int k = from; k < to; ++k )
	{
		float v = k < syncN ? -S : 0.0f;
		if( pix != nullptr && k >= a0 && k < a0 + 3 * pixels )
		{
			const int q    = k - a0 - 1;//pixel i's centre is at q = 3 i
			const int i0   = q >= 0 ? q / 3 : -1;
			const float t  = static_cast< float >( q - 3 * i0 ) * ( 1.0f / 3.0f );
			const float y0 = level( i0 ), y1 = level( i0 + 1 );
			v              = y0 + t * ( y1 - y0 );
		}
		dst[ static_cast< std::ptrdiff_t >( k - from ) * stride ] = v;
	}
}

/// Lane `lane` of a [ sample ][ lane ] array, linearly between samples.
inline float laneAt( const std::vector< float >& y, int total, int lane, double at )
{
	const double fl = std::floor( at );
	const int i     = std::clamp( static_cast< int >( fl ), 0, total - 2 );
	const float t   = static_cast< float >( at - fl );
	const float a   = y[ static_cast< size_t >( i ) * L4 + lane ];
	const float b   = y[ static_cast< size_t >( i + 1 ) * L4 + lane ];
	return a + t * ( b - a );
}
} // namespace

//---------------------------------------------------------------------------
void Engine::configure( const EngineSettings& s )
{
	const double clog = s.clogMetres;
	if( chain.standard == s.standard && chain.recording == s.recording && chain.deck == s.deck && chain.perturb == s.perturb && chain.clog == clog )
		return;

	const model::Standard& st = model::StandardOf( s.standard );
	const model::Format& rec  = model::FormatOf( s.recording );
	const int mode            = model::PlaybackMode( s.deck, s.recording );
	const model::Format& m    = model::FormatOf( mode );
	const double fs           = kSampleHz;

	chain.standard  = s.standard;
	chain.recording = s.recording;
	chain.deck      = s.deck;
	chain.perturb   = s.perturb;
	chain.clog      = clog;

	//The tape: written by the recording's camcorder.
	chain.recordY     = dsp::ButterworthLowPass( model::kRecordYOrder, rec.recordYHz, fs );
	chain.preEmphasis = dsp::Shelf( rec.emphasisTau, rec.emphasisX, fs, false );
	chain.recordTipHz = ( s.recording == model::kHi8 && ( s.perturb & model::kPerturbHi8AtVideo8Tip ) ) ? model::FormatOf( model::kVideo8 ).syncTipHz : rec.syncTipHz;
	chain.recordDevHz = rec.deviationHz;
	const double S    = st.syncVolts, Wh = st.whiteVolts;
	chain.whiteClipV  = -S + rec.whiteClip * ( S + Wh );
	chain.darkClipV   = -rec.darkClip * Wh;

	//The deck, in the mode it plays this tape in. The perturbations reach in
	//only where a Video8 deck meets a Hi8 tape.
	const bool mismatch = mode != s.recording;
	const double rfLow  = ( mismatch && ( s.perturb & model::kPerturbWideBand ) ) ? model::FormatOf( model::kHi8 ).rfLowPassHz : m.rfLowPassHz;
	chain.rfHigh        = dsp::ButterworthHighPass( model::kRfHighOrder, m.rfHighPassHz, fs );
	chain.rfLow         = dsp::ButterworthLowPass( model::kRfLowOrder, rfLow, fs );
	chain.yLow          = dsp::ButterworthLowPass( model::kYLowOrder, m.yLowPassHz, fs );
	const double deTau  = ( mismatch && ( s.perturb & model::kPerturbDeckEmphasis ) ) ? rec.emphasisTau : m.emphasisTau;
	chain.deEmphasis    = dsp::Shelf( deTau, m.emphasisX, fs, true );
	const bool tapeMap  = mismatch && ( s.perturb & model::kPerturbDeckDeviation );
	chain.deckTipHz     = tapeMap ? rec.syncTipHz : m.syncTipHz;
	chain.record        = chain.recordY.Then( chain.preEmphasis );
	chain.rf            = chain.rfHigh.Then( chain.rfLow );
	//The de-emphasis is linear with unity gain at DC, so it commutes with the
	//deck's affine map from Hz to volts: it runs on the frequency, straight
	//after the demodulator's low-pass.
	chain.playback      = chain.yLow.Then( chain.deEmphasis );
	chain.deckDevHz     = tapeMap ? rec.deviationHz : m.deviationHz;

	//The chroma: zero phase, so the corner is where a single pass is -3 dB
	//and the forward-backward pair is -6 dB: half amplitude at 0.5 MHz.
	chain.chroma = dsp::ButterworthLowPass( 2, model::kChromaHalfHz, model::kPixelHz );

	//The head clog at this standard's writing speed (the other's, perturbed).
	const double speed = model::StandardOf( ( s.perturb & model::kPerturbWrongSpeed ) ? 1 - s.standard : s.standard ).WritingSpeed();
	chain.clogTaps     = model::ClogTaps( clog, speed, model::kClogHalfTaps );

	//The deck's own luma delay, as its designers would have matched it with a
	//delay line: the whole chain for a tape of the deck's own mode, at low
	//frequency, the RF part at the blanking carrier. The FM phase takes a
	//sample's increment over the interval BEFORE the sample, so the frequency
	//changes half a sample early.
	const model::Format& own   = m;
	const dsp::Cascade ownY    = dsp::ButterworthLowPass( model::kRecordYOrder, own.recordYHz, fs );
	const dsp::Cascade ownPre  = dsp::Shelf( own.emphasisTau, own.emphasisX, fs, false );
	const dsp::Cascade ownDe   = dsp::Shelf( own.emphasisTau, own.emphasisX, fs, true );
	const dsp::Cascade ownHigh = dsp::ButterworthHighPass( model::kRfHighOrder, own.rfHighPassHz, fs );
	const dsp::Cascade ownLow  = dsp::ButterworthLowPass( model::kRfLowOrder, own.rfLowPassHz, fs );
	const double blankHz       = own.syncTipHz + S / ( S + Wh ) * own.deviationHz;
	const double w             = 2.0 * model::kPi * blankHz / fs;
	delay = ownY.GroupDelay( 0.0 ) + ownPre.GroupDelay( 0.0 ) + ownHigh.GroupDelay( w ) + ownLow.GroupDelay( w ) - 0.5 + chain.yLow.GroupDelay( 0.0 ) + ownDe.GroupDelay( 0.0 );
	if( s.perturb & model::kPerturbNoDelay )
		delay = 0.0;
}

//---------------------------------------------------------------------------
void Engine::recordBatch( const float* const* pix, const float* const* prev, int pixels, const EngineSettings& s, Scratch& sc, float* firstHz ) const
{
	const model::Standard& st = model::StandardOf( s.standard );
	const int L               = st.LineSamples();
	const int syncN           = st.SyncSamples();
	const int a0              = st.ActiveStart();
	const int warm            = samples( kWarmUs );
	const int total           = warm + L + samples( kTailUs );
	const float S             = static_cast< float >( st.syncVolts );
	const float Wh            = static_cast< float >( st.whiteVolts );
	const float fs            = static_cast< float >( kSampleHz );

	sc.total = total;
	sc.zero  = warm;
	sc.a.assign( static_cast< size_t >( total ) * L4, 0.0f );
	sc.b.assign( static_cast< size_t >( total ) * L4, 0.0f );
	float* a = sc.a.data();
	float* b = sc.b.data();

	//--- the composite luma: the line before in the same field, this line,
	//--- and the start of the next line's sync.
	for( int k = 0; k < L4; ++k )
	{
		composite( prev[ k ], pixels, syncN, a0, S, Wh, L - warm, L, a + k, L4 );
		composite( pix[ k ], pixels, syncN, a0, S, Wh, 0, L, a + k + static_cast< std::ptrdiff_t >( warm ) * L4, L4 );
		composite( nullptr, pixels, syncN, a0, S, Wh, 0, total - warm - L, a + k + static_cast< std::ptrdiff_t >( warm + L ) * L4, L4 );
	}

	//--- record: Y low-pass, pre-emphasis, clips, FM.
	sc.state.assign( 256, 0.0f );
	float* state = sc.state.data();
	chain.record.SteadyStateLanes( a, state );
	chain.record.RunLanes( a, total, state );
	const float whiteClip = static_cast< float >( chain.whiteClipV );
	const float darkClip  = static_cast< float >( chain.darkClipV );
	const float tip       = static_cast< float >( chain.recordTipHz );
	const float perVolt   = static_cast< float >( chain.recordDevHz ) / ( S + Wh );
	const float perSample = 1.0f / fs;
	const size_t n        = static_cast< size_t >( total ) * L4;
	float* LB_RESTRICT ar = a;
	float* LB_RESTRICT br = b;

	//The clips, and the frequency: kept in `a`. Sample 0's seeds the
	//demodulator's low-pass.
	for( size_t i = 0; i < n; ++i )
		ar[ i ] = tip + ( std::min( std::max( ar[ i ], darkClip ), whiteClip ) + S ) * perVolt;
	for( int k = 0; k < L4; ++k )
		firstHz[ k ] = ar[ k ];

	//The phase, in turns: a running sum per lane.
	float phase[ L4 ] = {};
	for( int i = 0; i < total; ++i )
		for( int k = 0; k < L4; ++k )
		{
			float q    = phase[ k ] + ar[ static_cast< size_t >( i ) * L4 + k ] * perSample;
			q          = q >= 1.0f ? q - 1.0f : q;
			phase[ k ] = q;
			br[ static_cast< size_t >( i ) * L4 + k ] = q;
		}

	//The carrier.
	for( size_t i = 0; i < n; ++i )
		br[ i ] = cosTurns( br[ i ] );
}

//---------------------------------------------------------------------------
void Engine::runBatch( const float* in, int lines, int pixels, const EngineSettings& s, int row0, const int* seedKeys,
                       const std::vector< model::Dropout >& drops, Scratch& sc ) const
{
	const model::Standard& st = model::StandardOf( s.standard );
	const int a0              = st.ActiveStart();
	const float S             = static_cast< float >( st.syncVolts );
	const float Wh            = static_cast< float >( st.whiteVolts );
	const float fs            = static_cast< float >( kSampleHz );

	const float* pix[ L4 ];
	const float* prev[ L4 ];
	for( int k = 0; k < L4; ++k )
	{
		const int row = row0 + k;
		pix[ k ]      = row < lines ? in + static_cast< size_t >( row ) * pixels * 4 : nullptr;
		prev[ k ]     = ( row >= 2 && row - 2 < lines ) ? in + static_cast< size_t >( row - 2 ) * pixels * 4 : nullptr;
	}
	float firstHz[ L4 ];
	recordBatch( pix, prev, pixels, s, sc, firstHz );
	const int total = sc.total;
	const int warm  = sc.zero;
	float* a        = sc.a.data();
	float* b        = sc.b.data();
	float* state    = sc.state.data();

	//--- tape: the clog's spacing loss, zero phase. The ends are held at the
	//--- first and last samples (inside the warm-up and the tail).
	if( !chain.clogTaps.empty() )
	{
		const int M    = model::kClogHalfTaps;
		const float* h = chain.clogTaps.data();
		sc.padded.resize( static_cast< size_t >( total + 2 * M ) * L4 );
		float* LB_RESTRICT p  = sc.padded.data();
		float* LB_RESTRICT bo = b;
		for( int i = -M; i < total + M; ++i )
		{
			const float* from = b + static_cast< size_t >( std::clamp( i, 0, total - 1 ) ) * L4;
			for( int k = 0; k < L4; ++k )
				p[ static_cast< size_t >( i + M ) * L4 + k ] = from[ k ];
		}
		//Symmetric: each tap pair once.
		for( int i = 0; i < total; ++i )
		{
			const float* w = p + static_cast< size_t >( i ) * L4;
			float acc[ L4 ];
			for( int k = 0; k < L4; ++k )
				acc[ k ] = h[ M ] * w[ M * L4 + k ];
			for( int j = 0; j < M; ++j )
				for( int k = 0; k < L4; ++k )
					acc[ k ] += h[ j ] * ( w[ j * L4 + k ] + w[ ( 2 * M - j ) * L4 + k ] );
			for( int k = 0; k < L4; ++k )
				bo[ static_cast< size_t >( i ) * L4 + k ] = acc[ k ];
		}
	}

	//--- dropouts on these lines: the carrier dipped 30 dB, raised-cosine edges.
	const int edge = std::max( 1, samples( model::kDropoutEdgeUs ) );
	for( const model::Dropout& d : drops )
	{
		const int line = ( s.perturb & model::kPerturbDropoutLine ) ? d.line + 2 : d.line;
		const int k    = line - row0;
		if( k < 0 || k >= L4 )
			continue;
		const int s0 = warm + a0 + samples( d.us0 );
		const int s1 = warm + a0 + samples( d.us1 );
		for( int i = std::max( 0, s0 - edge ); i < std::min( total, s1 + edge ); ++i )
		{
			float wgt = 1.0f;
			if( i < s0 )
				wgt = 0.5f - 0.5f * std::cos( 3.14159265f * static_cast< float >( i - ( s0 - edge ) ) / static_cast< float >( edge ) );
			else if( i >= s1 )
				wgt = 0.5f + 0.5f * std::cos( 3.14159265f * static_cast< float >( i - s1 ) / static_cast< float >( edge ) );
			b[ static_cast< size_t >( i ) * L4 + k ] *= 1.0f - ( 1.0f - static_cast< float >( model::kDropoutDepth ) ) * wgt;
		}
	}

	//--- the head amplifier's noise.
	if( s.noise )
	{
		const float sigma  = static_cast< float >( model::NoiseSigma( s.cnrDb ) );
		const float* gauss = model::GaussianTable();
		const int shift    = 32 - model::kGaussianBits;
		uint32_t seed[ L4 ];
		for( int k = 0; k < L4; ++k )
			seed[ k ] = model::LineSeed( s.videoFrame, seedKeys[ k ] );
		for( int i = 0; i < total; ++i )
			for( int k = 0; k < L4; ++k )
				b[ static_cast< size_t >( i ) * L4 + k ] += sigma * gauss[ model::Hash( seed[ k ] + static_cast< uint32_t >( i ) ) >> shift ];
	}

	//--- the deck: RF band, limiter, pulse count.
	std::fill( state, state + 256, 0.0f );
	chain.rf.RunLanes( b, total, state );
	//A unit of area at each crossing, spread over the four samples about it
	//by a cubic B-spline centred on the crossing's time. A pulse train sampled
	//at 40.5 MHz aliases: the 4th harmonic of white's 10.8 MHz crossing rate
	//lands at 2.7 MHz, in the picture. Splitting each pulse between the two
	//samples either side (a linear kernel, sinc^2) left a 2 % ripple on a
	//matched deck's white; the B-spline's sinc^4 takes it below 0.1 %.
	//
	//Pass 1 stores, for the interval ( i - 1, i ], the crossing's offset u
	//past sample i - 1 (or -1 for none) in `a`; pass 2 gathers each sample's
	//share from the four intervals that reach it.
	sc.pulses.assign( static_cast< size_t >( total ) * L4, 0.0f );
	float* LB_RESTRICT pulses = sc.pulses.data();
	{
		float* LB_RESTRICT offset  = a;
		const float* LB_RESTRICT x = b;
		const size_t n             = static_cast< size_t >( total ) * L4;
		for( size_t i = 0; i < static_cast< size_t >( L4 ); ++i )
			offset[ i ] = -1.0f;
		//The crossing's time: the linear estimate, refined by two Newton steps
		//on the cubic through the four samples about it. A linear estimate on
		//a carrier 7.5 samples a cycle (Video8's white) is up to 1 % of a
		//sample out, in a pattern that repeats every four crossings: a
		//2.7 MHz ripple on a flat field. The cubic takes it to 0.01 %.
		const bool risingOnly = ( s.perturb & model::kPerturbRisingOnly ) != 0;
		for( size_t i = L4; i < n; ++i )
		{
			const float x0   = x[ i - L4 ], x1 = x[ i ];
			const bool cross = risingOnly ? ( x0 < 0.0f && x1 >= 0.0f ) : ( x0 < 0.0f ) != ( x1 < 0.0f );
			float u          = cross ? x0 / ( x0 - x1 ) : -1.0f;
			if( cross && i >= 2 * static_cast< size_t >( L4 ) && i + L4 < n )
			{
				const float xm = x[ i - 2 * L4 ], xp = x[ i + L4 ];
				//Lagrange through ( -1, xm ), ( 0, x0 ), ( 1, x1 ), ( 2, xp ),
				//as c0 + c1 u + c2 u^2 + c3 u^3.
				const float c0 = x0;
				const float c1 = -xm / 3.0f - x0 / 2.0f + x1 - xp / 6.0f;
				const float c2 = xm / 2.0f - x0 + x1 / 2.0f;
				const float c3 = -xm / 6.0f + x0 / 2.0f - x1 / 2.0f + xp / 6.0f;
				for( int step = 0; step < 2; ++step )
				{
					const float p  = c0 + u * ( c1 + u * ( c2 + u * c3 ) );
					const float dp = c1 + u * ( 2.0f * c2 + u * 3.0f * c3 );
					u              = dp != 0.0f ? u - p / dp : u;
				}
				u = std::min( std::max( u, 0.0f ), 0.99999994f );
			}
			offset[ i ] = u;
		}
		//The interval ( i - 1, i ] with offset u reaches samples i - 2 .. i + 1
		//with the B-spline's weights ( 1 - u )^3 / 6, ( 3u^3 - 6u^2 + 4 ) / 6,
		//( -3u^3 + 3u^2 + 3u + 1 ) / 6, u^3 / 6: sample m takes w0 from
		//interval m + 2, w1 from m + 1, w2 from m and w3 from m - 1.
		auto weight = []( float u, int which ) {
			if( u < 0.0f )
				return 0.0f;
			const float u2 = u * u, u3 = u2 * u, v = 1.0f - u;
			switch( which )
			{
			case 0: return v * v * v * ( 1.0f / 6.0f );
			case 1: return ( 3.0f * u3 - 6.0f * u2 + 4.0f ) * ( 1.0f / 6.0f );
			case 2: return ( -3.0f * u3 + 3.0f * u2 + 3.0f * u + 1.0f ) * ( 1.0f / 6.0f );
			default: return u3 * ( 1.0f / 6.0f );
			}
		};
		const std::ptrdiff_t T = static_cast< std::ptrdiff_t >( total );
		for( std::ptrdiff_t m = 0; m < T; ++m )
			for( int k = 0; k < L4; ++k )
			{
				auto at = [ & ]( std::ptrdiff_t i ) {
					return ( i >= 0 && i < T ) ? offset[ static_cast< size_t >( i ) * L4 + k ] : -1.0f;
				};
				pulses[ static_cast< size_t >( m ) * L4 + k ] = weight( at( m + 2 ), 0 ) + weight( at( m + 1 ), 1 ) + weight( at( m ), 2 ) + weight( at( m - 1 ), 3 );
			}
	}

	//--- the demodulator's low-pass and the de-emphasis, then the deck's map.
	float init[ L4 ];
	for( int k = 0; k < L4; ++k )
		init[ k ] = 2.0f * firstHz[ k ] / fs;
	chain.playback.SteadyStateLanes( init, state );
	chain.playback.RunLanes( pulses, total, state );
	sc.y.resize( static_cast< size_t >( total ) * L4 );
	float* y               = sc.y.data();
	const float deckTip    = static_cast< float >( chain.deckTipHz );
	const float voltsPerHz = ( S + Wh ) / static_cast< float >( chain.deckDevHz );
	for( size_t i = 0; i < static_cast< size_t >( total ) * L4; ++i )
		y[ i ] = -S + ( pulses[ i ] * ( fs * 0.5f ) - deckTip ) * voltsPerHz;
}

//---------------------------------------------------------------------------
void Engine::monitorLane( const Scratch& sc, int lane, int pixels, const model::Standard& st, float* outY, LineResult& r ) const
{
	const int syncN = st.SyncSamples();
	const int a0    = st.ActiveStart();
	const int D     = static_cast< int >( std::lround( delay ) );
	const int zero  = sc.zero;
	const std::vector< float >& y = sc.y;

	//The clamp: the back porch, from 1 us after the sync to 0.5 us before the
	//picture, where the deck's own delay puts it.
	const int p0 = zero + D + syncN + samples( kPorchFromSyncEndUs );
	const int p1 = zero + D + a0 - samples( kPorchBeforeActiveUs );
	double porch = 0.0;
	for( int i = p0; i < p1; ++i )
		porch += y[ static_cast< size_t >( i ) * L4 + lane ];
	porch /= std::max( 1, p1 - p0 );

	const int t0 = zero + D + samples( kTipInsetUs );
	const int t1 = zero + D + syncN - samples( kTipInsetUs );
	double tip   = 0.0;
	for( int i = t0; i < t1; ++i )
		tip += y[ static_cast< size_t >( i ) * L4 + lane ];
	tip /= std::max( 1, t1 - t0 );

	r.porch = porch;
	r.tip   = tip;

	//Each pixel: the three samples about its centre, the deck's delay taken out.
	for( int i = 0; i < pixels; ++i )
	{
		const double c = zero + a0 + 3 * i + 1 + delay;
		outY[ i ]      = ( laneAt( y, sc.total, lane, c - 1.0 ) + laneAt( y, sc.total, lane, c ) + laneAt( y, sc.total, lane, c + 1.0 ) ) * ( 1.0f / 3.0f );
	}
}

//---------------------------------------------------------------------------
void Engine::chromaBatch( const float* in, int row0, int lines, int pixels, float* out, Scratch& sc ) const
{
	//Zero phase: forward then backward through the same filter, with the
	//blanking's zero colour before and after the picture. A batch of lines at
	//once, one a lane.
	const int pad   = 32;
	const int total = pixels + 2 * pad;
	auto reverse    = [ & ]( float* v ) {
		for( int i = 0, j = total - 1; i < j; ++i, --j )
			for( int k = 0; k < L4; ++k )
				std::swap( v[ static_cast< size_t >( i ) * L4 + k ], v[ static_cast< size_t >( j ) * L4 + k ] );
	};
	for( int c = 1; c <= 2; ++c )
	{
		sc.chroma.assign( static_cast< size_t >( total ) * L4, 0.0f );
		float* v = sc.chroma.data();
		for( int k = 0; k < L4 && row0 + k < lines; ++k )
			for( int i = 0; i < pixels; ++i )
				v[ static_cast< size_t >( i + pad ) * L4 + k ] = in[ ( static_cast< size_t >( row0 + k ) * pixels + i ) * 4 + c ];
		float state[ 2 * L4 ] = {};
		chain.chroma.RunLanes( v, total, state );
		reverse( v );
		std::fill( state, state + 2 * L4, 0.0f );
		chain.chroma.RunLanes( v, total, state );
		reverse( v );
		for( int k = 0; k < L4 && row0 + k < lines; ++k )
			for( int i = 0; i < pixels; ++i )
				out[ ( static_cast< size_t >( row0 + k ) * pixels + i ) * 4 + c ] = v[ static_cast< size_t >( i + pad ) * L4 + k ];
	}
}

//---------------------------------------------------------------------------
void Engine::Process( const float* in, int lines, int pixels, const EngineSettings& s, float* out, int threads )
{
	configure( s );
	const model::Standard& st = model::StandardOf( s.standard );

	std::vector< model::Dropout > drops = model::Dropouts( s.videoFrame, s.dropoutsPerFrame, lines, st.Active() );
	if( s.forceDropout )
		drops.insert( drops.begin(), s.forced );

	results.assign( static_cast< size_t >( lines ), LineResult{} );
	lumaOut.assign( static_cast< size_t >( lines ) * pixels, 0.0f );

	//Batches of kLanes consecutive frame rows: the same rows whatever the
	//worker count, so a line's arithmetic never depends on it.
	const int batches = ( lines + L4 - 1 ) / L4;
	const int workers = std::max( 1, std::min( threads, batches ) );
	auto work         = [ & ]( int worker ) {
		Scratch sc;
		for( int batch = worker; batch < batches; batch += workers )
		{
			const int row0 = batch * L4;
			int seeds[ L4 ];
			for( int k = 0; k < L4; ++k )
				seeds[ k ] = ( s.perturb & model::kPerturbSeedByWorker ) ? worker * L4 + k : row0 + k;
			runBatch( in, lines, pixels, s, row0, seeds, drops, sc );
			for( int k = 0; k < L4 && row0 + k < lines; ++k )
			{
				const int row = row0 + k;
				monitorLane( sc, k, pixels, st, lumaOut.data() + static_cast< size_t >( row ) * pixels, results[ static_cast< size_t >( row ) ] );
			}
			chromaBatch( in, row0, lines, pixels, out, sc );
		}
	};
	if( workers == 1 )
		work( 0 );
	else
	{
		std::vector< std::thread > pool;
		pool.reserve( static_cast< size_t >( workers - 1 ) );
		for( int w = 1; w < workers; ++w )
			pool.emplace_back( work, w );
		work( 0 );
		for( std::thread& t : pool )
			t.join();
	}

	//The monitor: the clamp per line, the AGC over the frame, in line order
	//so the sum is the same whatever the worker count.
	double depth = 0.0, porch = 0.0;
	for( const LineResult& r : results )
	{
		depth += r.porch - r.tip;
		porch += r.porch;
	}
	depth /= std::max( 1, lines );
	porch /= std::max( 1, lines );
	const double S    = st.syncVolts;
	const double gain = s.syncAgc ? S / std::max( depth, 0.05 * S ) : 1.0;
	lastDepth         = depth;
	lastGain          = gain;
	lastPorch         = porch;
	const float scale = static_cast< float >( gain / st.whiteVolts );
	for( int row = 0; row < lines; ++row )
	{
		const float black = static_cast< float >( results[ static_cast< size_t >( row ) ].porch );
		const float* yRow = lumaOut.data() + static_cast< size_t >( row ) * pixels;
		float* o          = out + static_cast< size_t >( row ) * pixels * 4;
		for( int i = 0; i < pixels; ++i )
		{
			o[ 4 * i + 0 ] = ( yRow[ i ] - black ) * scale;
			o[ 4 * i + 3 ] = 1.0f;
		}
	}
}

//---------------------------------------------------------------------------
std::vector< float > Engine::RecordFlatForTest( const EngineSettings& s, double level )
{
	configure( s );
	const model::Standard& st = model::StandardOf( s.standard );
	const int pixels          = st.ActivePixels();
	std::vector< float > line( static_cast< size_t >( pixels ) * 4, 0.0f );
	for( int i = 0; i < pixels; ++i )
		line[ static_cast< size_t >( i ) * 4 ] = static_cast< float >( level );
	const float* pix[ L4 ];
	for( int k = 0; k < L4; ++k )
		pix[ k ] = line.data();
	Scratch sc;
	float firstHz[ L4 ];
	recordBatch( pix, pix, pixels, s, sc, firstHz );
	std::vector< float > rf;
	for( int i = sc.zero; i < sc.total; ++i )
		rf.push_back( sc.b[ static_cast< size_t >( i ) * L4 ] );
	return rf;
}

std::vector< float > Engine::DeckLineForTest( const float* in, int lines, int pixels, const EngineSettings& s, int row )
{
	configure( s );
	const std::vector< model::Dropout > drops = s.forceDropout ? std::vector< model::Dropout >{ s.forced } : std::vector< model::Dropout >{};
	const int row0 = row - row % L4;
	int seeds[ L4 ];
	for( int k = 0; k < L4; ++k )
		seeds[ k ] = row0 + k;
	Scratch sc;
	runBatch( in, lines, pixels, s, row0, seeds, drops, sc );
	std::vector< float > y;
	for( int i = sc.zero; i < sc.total; ++i )
		y.push_back( sc.y[ static_cast< size_t >( i ) * L4 + ( row - row0 ) ] );
	return y;
}

size_t Engine::StateBytes() const
{
	size_t bytes = chain.clogTaps.size() * sizeof( float );
	bytes += results.size() * sizeof( LineResult ) + lumaOut.size() * sizeof( float );
	return bytes;
}

} // namespace lowband
