#pragma once

/**
	Host parameters are 0..1; these are what they mean.

	`SetParamInfo` clamps a STANDARD default into 0..1 *before* returning, and
	`SetParamRange` can only be called afterwards, so every slider here is a
	plain 0..1 float and the conversions live in this one file, which the
	plugin and the harness both use. Options are mapped by INDEX: an option
	parameter's range reads back 0..1 from the SDK whatever its element count.
*/
namespace lowband::controls
{

int OptionIndex( float value, int count );

const char* FormatName( int index );
const char* StandardName( int index );

/// Tape Noise: 0 is no noise at all; above it, the carrier-to-noise ratio in
/// the reference band, 46 dB at the bottom of the travel down to 10 dB.
bool NoiseOn( float value );
double CnrDb( float value );
/// The value that gives this CNR.
float CnrParam( double db );

/// Dropouts: per video frame, 30 v^2.
double DropoutsPerFrame( float value );

/// Head Clog: the head-to-tape spacing it adds, 0.15 v um, in metres.
double ClogMetres( float value );
float ClogParam( double metres );

float Amount( float value );

} // namespace lowband::controls
