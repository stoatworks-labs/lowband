#include "Dsp.h"

#include <cmath>

namespace lowband::dsp
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

/// Q of the k-th pole pair of an order-N Butterworth.
double butterworthQ( int order, int k )
{
	return 1.0 / ( 2.0 * std::sin( ( 2.0 * k + 1.0 ) * kPi / ( 2.0 * order ) ) );
}
} // namespace

std::complex< double > Biquad::Response( double omega ) const
{
	const std::complex< double > z1 = std::polar( 1.0, -omega );
	const std::complex< double > z2 = z1 * z1;
	return ( b0 + b1 * z1 + b2 * z2 ) / ( 1.0 + a1 * z1 + a2 * z2 );
}

double Biquad::GroupDelay( double omega ) const
{
	//tau = Re{ sum k b_k z^-k / sum b_k z^-k } - the same for a.
	const std::complex< double > z1 = std::polar( 1.0, -omega );
	const std::complex< double > z2 = z1 * z1;
	const std::complex< double > B  = b0 + b1 * z1 + b2 * z2;
	const std::complex< double > Br = b1 * z1 + 2.0 * b2 * z2;
	const std::complex< double > A  = 1.0 + a1 * z1 + a2 * z2;
	const std::complex< double > Ar = a1 * z1 + 2.0 * a2 * z2;
	return ( Br / B ).real() - ( Ar / A ).real();
}

void Cascade::Finalise()
{
	coeffs.clear();
	for( const Biquad& s : sections )
	{
		coeffs.push_back( static_cast< float >( s.b0 ) );
		coeffs.push_back( static_cast< float >( s.b1 ) );
		coeffs.push_back( static_cast< float >( s.b2 ) );
		coeffs.push_back( static_cast< float >( s.a1 ) );
		coeffs.push_back( static_cast< float >( s.a2 ) );
	}
}

std::complex< double > Cascade::Response( double omega ) const
{
	std::complex< double > h = 1.0;
	for( const Biquad& s : sections )
		h *= s.Response( omega );
	return h;
}

double Cascade::GroupDelay( double omega ) const
{
	double t = 0.0;
	for( const Biquad& s : sections )
		t += s.GroupDelay( omega );
	return t;
}

double Cascade::DcGain() const
{
	double g = 1.0;
	for( const Biquad& s : sections )
		g *= s.DcGain();
	return g;
}

void Cascade::SteadyState( float x, float* state ) const
{
	//Run in the same float coefficients the chain runs, so a constant in is
	//exactly a constant out from the first sample (to float rounding).
	const float* c = coeffs.data();
	for( size_t i = 0; i < sections.size(); ++i, c += 5, state += 2 )
	{
		const double g = ( static_cast< double >( c[ 0 ] ) + c[ 1 ] + c[ 2 ] ) / ( 1.0 + c[ 3 ] + c[ 4 ] );
		const double y = g * x;
		const double s2 = c[ 2 ] * static_cast< double >( x ) - c[ 4 ] * y;
		const double s1 = c[ 1 ] * static_cast< double >( x ) - c[ 3 ] * y + s2;
		state[ 0 ]      = static_cast< float >( s1 );
		state[ 1 ]      = static_cast< float >( s2 );
		x               = static_cast< float >( y );
	}
}

void Cascade::SteadyStateLanes( const float* x, float* state ) const
{
	float s1[ 64 ];
	for( int k = 0; k < kLanes; ++k )
	{
		SteadyState( x[ k ], s1 );
		for( size_t i = 0; i < sections.size(); ++i )
		{
			state[ i * 2 * kLanes + k ]          = s1[ 2 * i ];
			state[ i * 2 * kLanes + kLanes + k ] = s1[ 2 * i + 1 ];
		}
	}
}

namespace
{
/// RunLanes's body: N sections, kLanes lanes, the state in local arrays the
/// compiler keeps in registers. The arithmetic per lane is Step's.
template< int N >
void runLanes( const float* coeffs, float* data, int total, float* state )
{
	constexpr int K = Cascade::kLanes;
	float s1[ N ][ K ], s2[ N ][ K ], c[ N ][ 5 ];
	for( int j = 0; j < N; ++j )
	{
		for( int q = 0; q < 5; ++q )
			c[ j ][ q ] = coeffs[ 5 * j + q ];
		for( int k = 0; k < K; ++k )
		{
			s1[ j ][ k ] = state[ j * 2 * K + k ];
			s2[ j ][ k ] = state[ j * 2 * K + K + k ];
		}
	}
	for( int i = 0; i < total; ++i )
	{
		float x[ K ];
		for( int k = 0; k < K; ++k )
			x[ k ] = data[ i * K + k ];
		for( int j = 0; j < N; ++j )
			for( int k = 0; k < K; ++k )
			{
				const float y = c[ j ][ 0 ] * x[ k ] + s1[ j ][ k ];
				s1[ j ][ k ]  = c[ j ][ 1 ] * x[ k ] - c[ j ][ 3 ] * y + s2[ j ][ k ];
				s2[ j ][ k ]  = c[ j ][ 2 ] * x[ k ] - c[ j ][ 4 ] * y;
				x[ k ]        = y;
			}
		for( int k = 0; k < K; ++k )
			data[ i * K + k ] = x[ k ];
	}
	for( int j = 0; j < N; ++j )
		for( int k = 0; k < K; ++k )
		{
			state[ j * 2 * K + k ]     = s1[ j ][ k ];
			state[ j * 2 * K + K + k ] = s2[ j ][ k ];
		}
}
} // namespace

void Cascade::RunLanes( float* data, int total, float* state ) const
{
	const int n = static_cast< int >( sections.size() );
	switch( n )
	{
	case 1: runLanes< 1 >( coeffs.data(), data, total, state ); return;
	case 2: runLanes< 2 >( coeffs.data(), data, total, state ); return;
	case 3: runLanes< 3 >( coeffs.data(), data, total, state ); return;
	case 4: runLanes< 4 >( coeffs.data(), data, total, state ); return;
	case 5: runLanes< 5 >( coeffs.data(), data, total, state ); return;
	case 6: runLanes< 6 >( coeffs.data(), data, total, state ); return;
	default:
		for( int i = 0; i < total; ++i )
			StepLanes( data + static_cast< size_t >( i ) * kLanes, state );
	}
}

Cascade Cascade::Then( const Cascade& next ) const
{
	Cascade out = *this;
	out.sections.insert( out.sections.end(), next.sections.begin(), next.sections.end() );
	out.Finalise();
	return out;
}

Cascade ButterworthLowPass( int order, double fc, double fs )
{
	Cascade c;
	const double K = std::tan( kPi * fc / fs );
	for( int k = 0; k < order / 2; ++k )
	{
		const double Q    = butterworthQ( order, k );
		const double norm = 1.0 / ( 1.0 + K / Q + K * K );
		Biquad s;
		s.b0 = K * K * norm;
		s.b1 = 2.0 * s.b0;
		s.b2 = s.b0;
		s.a1 = 2.0 * ( K * K - 1.0 ) * norm;
		s.a2 = ( 1.0 - K / Q + K * K ) * norm;
		c.sections.push_back( s );
	}
	c.Finalise();
	return c;
}

Cascade ButterworthHighPass( int order, double fc, double fs )
{
	Cascade c;
	const double K = std::tan( kPi * fc / fs );
	for( int k = 0; k < order / 2; ++k )
	{
		const double Q    = butterworthQ( order, k );
		const double norm = 1.0 / ( 1.0 + K / Q + K * K );
		Biquad s;
		s.b0 = norm;
		s.b1 = -2.0 * norm;
		s.b2 = norm;
		s.a1 = 2.0 * ( K * K - 1.0 ) * norm;
		s.a2 = ( 1.0 - K / Q + K * K ) * norm;
		c.sections.push_back( s );
	}
	c.Finalise();
	return c;
}

Cascade Shelf( double tau, double x, double fs, bool inverse )
{
	//( 1 + s tau ) / ( 1 + s tau / x ), s = c ( 1 - z^-1 ) / ( 1 + z^-1 ).
	const double c   = 2.0 * fs;
	double n0        = 1.0 + c * tau, n1 = 1.0 - c * tau;
	double d0        = 1.0 + c * tau / x, d1 = 1.0 - c * tau / x;
	if( inverse )
	{
		std::swap( n0, d0 );
		std::swap( n1, d1 );
	}
	Biquad s;
	s.b0 = n0 / d0;
	s.b1 = n1 / d0;
	s.b2 = 0.0;
	s.a1 = d1 / d0;
	s.a2 = 0.0;
	Cascade out;
	out.sections.push_back( s );
	out.Finalise();
	return out;
}

std::complex< double > AnalogueButterworth( int order, double fc, double f, bool highPass )
{
	const std::complex< double > s( 0.0, f / fc );
	std::complex< double > h = 1.0;
	for( int k = 0; k < order / 2; ++k )
	{
		const double Q = butterworthQ( order, k );
		h *= ( highPass ? s * s : std::complex< double >( 1.0 ) ) / ( s * s + s / Q + 1.0 );
	}
	return h;
}

std::complex< double > AnalogueShelf( double tau, double x, double f, bool inverse )
{
	const std::complex< double > s( 0.0, 2.0 * kPi * f );
	const std::complex< double > h = ( 1.0 + s * tau ) / ( 1.0 + s * tau / x );
	return inverse ? 1.0 / h : h;
}

} // namespace lowband::dsp
