#pragma once

#include <string>

/**
	The two GPU passes. Everything between them -- the whole FM chain -- runs
	on the CPU (Engine.h); the GPU only brings the host's picture onto the
	standard's raster and takes the deck's picture back to the host's.

	    intake:  host frame -> P x N, the area average of the host pixels each
	             covers, as Y'UV (BT.601). Line 0 (the top) at row 0.
	    display: P x N (Y', U, V) -> host frame: each host row its nearest
	             line, Catmull-Rom across, R'G'B', Mix.

	Each is assembled at run time from a version line, an optional shared
	block and a body; `lbtest --dump-shaders` writes the exact strings.
*/
namespace lowband::shaders
{

std::string Vertex();
std::string Intake();
std::string Display();

} // namespace lowband::shaders
