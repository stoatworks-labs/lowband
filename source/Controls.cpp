#include "Controls.h"

#include "Model.h"

#include <algorithm>
#include <cmath>

namespace lowband::controls
{
namespace
{
double unit( float value )
{
	return std::clamp( static_cast< double >( value ), 0.0, 1.0 );
}

constexpr double kCnrTop    = 46.0;
constexpr double kCnrBottom = 10.0;
constexpr double kClogMax   = 0.15e-6;
} // namespace

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

const char* FormatName( int index )
{
	return index == model::kHi8 ? "Hi8" : "Video8";
}

const char* StandardName( int index )
{
	return index == model::kNTSC ? "NTSC" : "PAL";
}

bool NoiseOn( float value )
{
	return value > 0.0f;
}

double CnrDb( float value )
{
	return kCnrTop - ( kCnrTop - kCnrBottom ) * unit( value );
}

float CnrParam( double db )
{
	return static_cast< float >( std::clamp( ( kCnrTop - db ) / ( kCnrTop - kCnrBottom ), 0.0, 1.0 ) );
}

double DropoutsPerFrame( float value )
{
	const double v = unit( value );
	return 30.0 * v * v;
}

double ClogMetres( float value )
{
	return kClogMax * unit( value );
}

float ClogParam( double metres )
{
	return static_cast< float >( std::clamp( metres / kClogMax, 0.0, 1.0 ) );
}

float Amount( float value )
{
	return static_cast< float >( unit( value ) );
}

} // namespace lowband::controls
