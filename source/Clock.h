#pragma once

/**
	The raster's clock: seconds since the plugin's first frame, in double,
	from whatever the host hands SetTime. Copied from colourunder (standards'
	before it) unchanged; only the video frame that seeds the noise reads it.

	Three problems, all inherited from the fleet.

	**What unit is the host's clock in?** The FFGL header never says. Resolume
	sends milliseconds; the offline harness and oxbow send seconds. The unit is
	voted on against a steady wall clock over the first few frames (readout's
	voting, by way of cadence) and then stuck to. The harness DECLARES its
	unit, because it renders as fast as the GPU allows and there is nothing
	for the voting to measure.

	**Resolume's clock overflows a float.** It has been measured at ~499
	million ms, where a float resolves ~0.03 s -- more than a field. So the
	clock is kept as an origin and an offset in double, the field index is
	decided in double (`model::FieldAt`) and nothing absolute ever reaches a
	shader.

	**The raster cannot be clamped the way an envelope can.** Cadence and the
	rest clamp every frame's delta into [1/240, 1/24] s. Here that would put
	the raster out of step with real time -- the harness drives the host at
	144 frames a second and at 23.976 -- and the capacitor's trajectory
	would stop being a function of elapsed time. So a delta is believed
	unless it is backwards or longer than half a second -- a scrub, a loop, a
	stall -- in which case the clock steps on by one nominal frame and
	carries on from there. The raster sees a continuous clock either way.
*/
namespace lowband
{
class Clock
{
public:
	/// Advance to this frame. `hostTime` is whatever the host last handed to
	/// SetTime, or negative if it never has.
	void Update( double hostTime );

	/// Declare the host's unit instead of letting Update infer it.
	void SetScaleForTest( double scale )
	{
		clockScale = scale;
	}

	/// The offline --clock check's negative control: keep the arithmetic in
	/// float, the way an absolute host clock would be if it reached a shader.
	void SetFloatForTest( bool on )
	{
		useFloat = on;
	}

	/// Seconds since the first frame. Monotonic.
	double Now() const
	{
		return now;
	}

	/// True on a frame whose delta was not believed.
	bool Jumped() const
	{
		return jumped;
	}

	/// True until the first Update.
	bool Fresh() const
	{
		return lastScaled < 0.0 && !started;
	}

	double ClockScale() const
	{
		return clockScale;
	}

	void Reset();

	/// Longer than this between two frames is a jump, not a frame.
	static constexpr double kMaxFrameSeconds = 0.5;
	/// What a jump advances the clock by.
	static constexpr double kNominalFrameSeconds = 1.0 / 60.0;

private:
	double clockScale   = 0.0;///< 0 undecided, 1 seconds, 0.001 milliseconds
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;

	bool started      = false;
	double lastScaled = -1.0;
	double anchor     = 0.0;///< the scaled host time the current run started at
	double offset     = 0.0;///< the clock's value at that moment
	double now        = 0.0;
	bool jumped       = false;
	bool useFloat     = false;
};

} // namespace lowband
