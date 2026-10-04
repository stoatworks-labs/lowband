#include "Clock.h"

#include <chrono>

namespace lowband
{
namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}
} // namespace

void Clock::Update( double hostTime )
{
	jumped             = false;
	const double wall  = wallSeconds();
	double scaled      = wall;

	if( hostTime >= 0.0 )
	{
		//The voting: the ratio of the host's delta to a steady clock's names
		//the unit outright, and nothing plausible sits between ~1 and ~1000.
		if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
		{
			const double hostDelta = hostTime - lastRawTime;
			const double wallDelta = wall - lastWallTime;
			if( hostDelta > 0.0 && wallDelta >= 0.0005 )
			{
				const double ratio = hostDelta / wallDelta;
				if( ratio > 0.1 && ratio < 10.0 )
					++secondsVotes;
				else if( ratio > 100.0 && ratio < 10000.0 )
					++millisVotes;
				if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
					clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
			}
		}
		lastRawTime  = hostTime;
		lastWallTime = wall;

		//Until the unit is settled, run on the wall clock: wrong in origin,
		//right in rate. A unit decided mid-run is a jump, handled below.
		if( clockScale != 0.0 )
			scaled = useFloat ? static_cast< double >( static_cast< float >( hostTime ) * static_cast< float >( clockScale ) )
			                  : hostTime * clockScale;
	}

	if( !started )
	{
		started    = true;
		anchor     = scaled;
		offset     = 0.0;
		now        = 0.0;
		lastScaled = scaled;
		return;
	}

	const double delta = scaled - lastScaled;
	if( delta < 0.0 || delta > kMaxFrameSeconds )
	{
		//A scrub, a loop, a stall, or the unit being decided. Step on by one
		//nominal frame and measure from here.
		jumped = true;
		offset = now + kNominalFrameSeconds;
		anchor = scaled;
		now    = offset;
	}
	else if( useFloat )
		now = static_cast< double >( static_cast< float >( offset ) + ( static_cast< float >( scaled ) - static_cast< float >( anchor ) ) );
	else
		//Origin and offset rather than a running sum of deltas: the sum
		//would carry every delta's rounding forever, this carries one.
		now = offset + ( scaled - anchor );

	lastScaled = scaled;
}

void Clock::Reset()
{
	started    = false;
	lastScaled = -1.0;
	anchor     = 0.0;
	offset     = 0.0;
	now        = 0.0;
	jumped     = false;
	//clockScale survives: the host has not changed.
}

} // namespace lowband
