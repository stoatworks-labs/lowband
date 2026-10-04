#include "Shaders.h"

namespace lowband::shaders
{
namespace
{
const char* const kVersion = "#version 410 core\n";

//---------------------------------------------------------------------------
// The vertex shader both passes share.
//---------------------------------------------------------------------------
const char* const kVertexBody = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// BT.601's Y' and the PAL/NTSC U, V scalings, both ways.
//---------------------------------------------------------------------------
const char* const kCommon = R"(
const float kUScale = 0.492111;
const float kVScale = 0.877283;

vec3 rgbToYuv( vec3 c )
{
	float y = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
	return vec3( y, kUScale * ( c.b - y ), kVScale * ( c.r - y ) );
}

vec3 yuvToRgb( vec3 yuv )
{
	float r = yuv.x + yuv.z / kVScale;
	float b = yuv.x + yuv.y / kUScale;
	float g = ( yuv.x - 0.299 * r - 0.114 * b ) / 0.587;
	return vec3( r, g, b );
}
)";

//---------------------------------------------------------------------------
// intake: each pixel of the standard's active raster as the area average of
// the host pixels it covers, in both directions. In units of 1/Lines of a
// host row, line l spans [ l H, ( l + 1 ) H ) and row r spans
// [ r Lines, ( r + 1 ) Lines ): the overlaps are integers and sum to H; the
// same across, in units of 1/Pixels of a column. Line 0 is the TOP of the
// picture and is written to row 0, so a read-back returns it first.
//
// The picture is recorded as handed over: Resolume's demo clips with alpha
// are already black wherever they are transparent (colourunder measured it),
// so multiplying by alpha again would darken every soft edge twice.
//---------------------------------------------------------------------------
const char* const kIntakeBody = R"(
uniform sampler2D Source;
uniform int HostW;
uniform int HostH;
uniform int Pixels;
uniform int Lines;

out vec4 fragColor;

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	int ylo = p.y * HostH;
	int yhi = ( p.y + 1 ) * HostH;
	int r0  = ylo / Lines;
	int r1  = ( yhi - 1 ) / Lines;
	int xlo = p.x * HostW;
	int xhi = ( p.x + 1 ) * HostW;
	int c0  = xlo / Pixels;
	int c1  = ( xhi - 1 ) / Pixels;
	vec4 sum = vec4( 0.0 );
	for( int r = r0; r <= r1; ++r )
	{
		int oy = min( ( r + 1 ) * Lines, yhi ) - max( r * Lines, ylo );
		vec4 row = vec4( 0.0 );
		for( int c = c0; c <= c1; ++c )
		{
			int ox = min( ( c + 1 ) * Pixels, xhi ) - max( c * Pixels, xlo );
			row += float( ox ) * texelFetch( Source, ivec2( c, HostH - 1 - r ), 0 );
		}
		sum += float( oy ) * row;
	}
	vec4 avg  = sum / ( float( HostH ) * float( HostW ) );
	fragColor = vec4( rgbToYuv( avg.rgb ), avg.a );
}
)";

//---------------------------------------------------------------------------
// display: to the host. Each host row shows its nearest line (a TV draws a
// line as a stripe); across, the line's pixels by Catmull-Rom, which reads
// each pixel exactly when the host is as wide as the raster. Past either
// end of the line is blanking.
//---------------------------------------------------------------------------
const char* const kDisplayBody = R"(
uniform sampler2D Lines;
uniform sampler2D Source;
uniform int HostW;
uniform int HostH;
uniform int LineCount;
uniform int Pixels;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

vec3 tap( int i, int l )
{
	return texelFetch( Lines, ivec2( clamp( i, 0, Pixels - 1 ), l ), 0 ).xyz;
}

void main()
{
	int x   = clamp( int( floor( uv.x * float( HostW ) ) ), 0, HostW - 1 );
	int rgl = clamp( int( floor( uv.y * float( HostH ) ) ), 0, HostH - 1 );
	int r   = HostH - 1 - rgl;
	int l   = ( ( 2 * r + 1 ) * LineCount ) / ( 2 * HostH );

	float s  = ( float( x ) + 0.5 ) * float( Pixels ) / float( HostW ) - 0.5;
	float i0 = floor( s );
	float t  = s - i0;
	int i    = int( i0 );
	float t2 = t * t, t3 = t2 * t;
	float w0 = 0.5 * ( -t3 + 2.0 * t2 - t );
	float w1 = 0.5 * ( 3.0 * t3 - 5.0 * t2 + 2.0 );
	float w2 = 0.5 * ( -3.0 * t3 + 4.0 * t2 + t );
	float w3 = 0.5 * ( t3 - t2 );
	vec3 yuv = w0 * tap( i - 1, l ) + w1 * tap( i, l ) + w2 * tap( i + 1, l ) + w3 * tap( i + 2, l );

	vec4 src  = texelFetch( Source, ivec2( x, rgl ), 0 );
	vec3 rgb  = clamp( yuvToRgb( yuv ), 0.0, 1.0 );
	fragColor = vec4( mix( src.rgb, rgb, MixAmount ), mix( src.a, 1.0, MixAmount ) );
}
)";

std::string assemble( const char* body, bool common = false )
{
	std::string s = kVersion;
	if( common )
		s += kCommon;
	s += body;
	return s;
}
} // namespace

std::string Vertex()
{
	return assemble( kVertexBody );
}

std::string Intake()
{
	return assemble( kIntakeBody, true );
}

std::string Display()
{
	return assemble( kDisplayBody, true );
}

} // namespace lowband::shaders
