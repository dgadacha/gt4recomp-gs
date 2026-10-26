// SPDX-FileCopyrightText: 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez
// SPDX-FileCopyrightText: 2026 GT4Recomp contributors
// SPDX-License-Identifier: MIT

// GT4Recomp fork (macOS port): SMAA 1x ([GT4GS] SMAA, GSDeviceMTL::ApplyGT4Look), a Metal port of the reference
// SMAA.hlsl (github.com/iryoku/smaa, MIT; the licence is 3rdparty/smaa/LICENSE.txt). Same functions and names, with the
// "High" preset: luma edge detection with local contrast adaptation, diagonal and corner detection, no predication, no
// reprojection, subsample indices zero. Differences from the reference: Metal syntax (references for out and inout
// parameters, select for SMAAMovc, discard_fragment), and the vertex shader offsets computed in the pixel shaders.
//
// Pass 1: ps_smaa_edges (the picture to its edges, discarding pixels without one into a cleared target).
// Pass 2: ps_smaa_weights (edges, AreaTex RG8 160x560, SearchTex R8 64x16 to the blending weights).
// Pass 3: ps_smaa_blend (the picture and the weights to the antialiased picture).
// cb: SMAA_RT_METRICS, float4(1 / width, 1 / height, width, height) of the picture.

#include "GSMTLShaderCommon.h"

using namespace metal;

#define SMAA_THRESHOLD 0.1f
#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#define SMAA_CORNER_ROUNDING 25
#define SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR 2.0f

#define SMAA_AREATEX_MAX_DISTANCE 16
#define SMAA_AREATEX_MAX_DISTANCE_DIAG 20
#define SMAA_AREATEX_PIXEL_SIZE (1.0f / float2(160.0f, 560.0f))
#define SMAA_AREATEX_SUBTEX_SIZE (1.0f / 7.0f)
#define SMAA_SEARCHTEX_SIZE float2(66.0f, 33.0f)
#define SMAA_SEARCHTEX_PACKED_SIZE float2(64.0f, 16.0f)
#define SMAA_CORNER_ROUNDING_NORM (float(SMAA_CORNER_ROUNDING) / 100.0f)

constexpr sampler smaa_linear(coord::normalized, filter::linear, mip_filter::none, address::clamp_to_edge);
constexpr sampler smaa_point(coord::normalized, filter::nearest, mip_filter::none, address::clamp_to_edge);

#define SMAASampleLevelZero(tex, coord) tex.sample(smaa_linear, coord, level(0))
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.sample(smaa_linear, coord, level(0), offset)
#define SMAASample(tex, coord) tex.sample(smaa_linear, coord)
#define SMAASamplePoint(tex, coord) tex.sample(smaa_point, coord)

static inline float2 mad(float2 a, float2 b, float2 c) { return a * b + c; }
static inline float3 mad(float3 a, float3 b, float3 c) { return a * b + c; }
static inline float4 mad(float4 a, float4 b, float4 c) { return a * b + c; }
static inline float mad(float a, float b, float c) { return a * b + c; }

//-----------------------------------------------------------------------------
// Edge Detection Pixel Shader (First Pass): luma edge detection.

fragment float4 ps_smaa_edges(ConvertShaderData data [[stage_in]],
	texture2d<float> colorTex [[texture(GSMTLTextureIndexNonHW)]], constant float4& SMAA_RT_METRICS [[buffer(GSMTLBufferIndexUniforms)]])
{
	const float2 texcoord = data.t;
	float4 offset[3];
	offset[0] = mad(SMAA_RT_METRICS.xyxy, float4(-1.0f, 0.0f, 0.0f, -1.0f), texcoord.xyxy);
	offset[1] = mad(SMAA_RT_METRICS.xyxy, float4( 1.0f, 0.0f, 0.0f,  1.0f), texcoord.xyxy);
	offset[2] = mad(SMAA_RT_METRICS.xyxy, float4(-2.0f, 0.0f, 0.0f, -2.0f), texcoord.xyxy);

	const float2 threshold = float2(SMAA_THRESHOLD, SMAA_THRESHOLD);

	// Calculate lumas:
	const float3 weights = float3(0.2126f, 0.7152f, 0.0722f);
	const float L = dot(SMAASamplePoint(colorTex, texcoord).rgb, weights);

	const float Lleft = dot(SMAASamplePoint(colorTex, offset[0].xy).rgb, weights);
	const float Ltop  = dot(SMAASamplePoint(colorTex, offset[0].zw).rgb, weights);

	// We do the usual threshold:
	float4 delta;
	delta.xy = abs(L - float2(Lleft, Ltop));
	float2 edges = step(threshold, delta.xy);

	// Then discard if there is no edge:
	if (dot(edges, float2(1.0f, 1.0f)) == 0.0f)
		discard_fragment();

	// Calculate right and bottom deltas:
	const float Lright = dot(SMAASamplePoint(colorTex, offset[1].xy).rgb, weights);
	const float Lbottom = dot(SMAASamplePoint(colorTex, offset[1].zw).rgb, weights);
	delta.zw = abs(L - float2(Lright, Lbottom));

	// Calculate the maximum delta in the direct neighborhood:
	float2 maxDelta = max(delta.xy, delta.zw);

	// Calculate left-left and top-top deltas:
	const float Lleftleft = dot(SMAASamplePoint(colorTex, offset[2].xy).rgb, weights);
	const float Ltoptop = dot(SMAASamplePoint(colorTex, offset[2].zw).rgb, weights);
	delta.zw = abs(float2(Lleft, Ltop) - float2(Lleftleft, Ltoptop));

	// Calculate the final maximum delta:
	maxDelta = max(maxDelta.xy, delta.zw);
	const float finalDelta = max(maxDelta.x, maxDelta.y);

	// Local contrast adaptation:
	edges.xy *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);

	return float4(edges, 0.0f, 0.0f);
}

//-----------------------------------------------------------------------------
// Diagonal Search Functions

// Allows to decode two binary values from a bilinear-filtered access.
static float2 SMAADecodeDiagBilinearAccess(float2 e)
{
	e.r = e.r * abs(5.0f * e.r - 5.0f * 0.75f);
	return round(e);
}

static float4 SMAADecodeDiagBilinearAccess(float4 e)
{
	e.rb = e.rb * abs(5.0f * e.rb - 5.0f * 0.75f);
	return round(e);
}

// These functions allows to perform diagonal pattern searches.
static float2 SMAASearchDiag1(texture2d<float> edgesTex, float2 texcoord, float2 dir, thread float2& e, float4 SMAA_RT_METRICS)
{
	float4 coord = float4(texcoord, -1.0f, 1.0f);
	const float3 t = float3(SMAA_RT_METRICS.xy, 1.0f);
	while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9f)
	{
		coord.xyz = mad(t, float3(dir, 1.0f), coord.xyz);
		e = SMAASampleLevelZero(edgesTex, coord.xy).rg;
		coord.w = dot(e, float2(0.5f, 0.5f));
	}
	return coord.zw;
}

static float2 SMAASearchDiag2(texture2d<float> edgesTex, float2 texcoord, float2 dir, thread float2& e, float4 SMAA_RT_METRICS)
{
	float4 coord = float4(texcoord, -1.0f, 1.0f);
	coord.x += 0.25f * SMAA_RT_METRICS.x; // See @SearchDiag2Optimization
	const float3 t = float3(SMAA_RT_METRICS.xy, 1.0f);
	while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9f)
	{
		coord.xyz = mad(t, float3(dir, 1.0f), coord.xyz);

		// @SearchDiag2Optimization
		// Fetch both edges at once using bilinear filtering:
		e = SMAASampleLevelZero(edgesTex, coord.xy).rg;
		e = SMAADecodeDiagBilinearAccess(e);

		coord.w = dot(e, float2(0.5f, 0.5f));
	}
	return coord.zw;
}

// Similar to SMAAArea, this calculates the area corresponding to a certain diagonal distance and crossing edges 'e'.
static float2 SMAAAreaDiag(texture2d<float> areaTex, float2 dist, float2 e, float offset)
{
	float2 texcoord = mad(float2(SMAA_AREATEX_MAX_DISTANCE_DIAG, SMAA_AREATEX_MAX_DISTANCE_DIAG), e, dist);

	// We do a scale and bias for mapping to texel space:
	texcoord = mad(SMAA_AREATEX_PIXEL_SIZE, texcoord, 0.5f * SMAA_AREATEX_PIXEL_SIZE);

	// Diagonal areas are on the second half of the texture:
	texcoord.x += 0.5f;

	// Move to proper place, according to the subpixel offset:
	texcoord.y += SMAA_AREATEX_SUBTEX_SIZE * offset;

	// Do it!
	return SMAASampleLevelZero(areaTex, texcoord).rg;
}

// This searches for diagonal patterns and returns the corresponding weights.
static float2 SMAACalculateDiagWeights(texture2d<float> edgesTex, texture2d<float> areaTex, float2 texcoord, float2 e,
	float4 subsampleIndices, float4 SMAA_RT_METRICS)
{
	float2 weights = float2(0.0f, 0.0f);

	// Search for the line ends:
	float4 d;
	float2 end;
	if (e.r > 0.0f)
	{
		d.xz = SMAASearchDiag1(edgesTex, texcoord, float2(-1.0f, 1.0f), end, SMAA_RT_METRICS);
		d.x += float(end.y > 0.9f);
	}
	else
		d.xz = float2(0.0f, 0.0f);
	d.yw = SMAASearchDiag1(edgesTex, texcoord, float2(1.0f, -1.0f), end, SMAA_RT_METRICS);

	if (d.x + d.y > 2.0f) // d.x + d.y + 1 > 3
	{
		// Fetch the crossing edges:
		const float4 coords = mad(float4(-d.x + 0.25f, d.x, d.y, -d.y - 0.25f), SMAA_RT_METRICS.xyxy, texcoord.xyxy);
		float4 c;
		c.xy = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2(-1, 0)).rg;
		c.zw = SMAASampleLevelZeroOffset(edgesTex, coords.zw, int2( 1, 0)).rg;
		c.yxwz = SMAADecodeDiagBilinearAccess(c.xyzw);

		// Merge crossing edges at each side into a single value:
		float2 cc = mad(float2(2.0f, 2.0f), c.xz, c.yw);

		// Remove the crossing edge if we didn't found the end of the line:
		cc = select(cc, float2(0.0f, 0.0f), bool2(step(0.9f, d.zw)));

		// Fetch the areas for this line:
		weights += SMAAAreaDiag(areaTex, d.xy, cc, subsampleIndices.z);
	}

	// Search for the line ends:
	d.xz = SMAASearchDiag2(edgesTex, texcoord, float2(-1.0f, -1.0f), end, SMAA_RT_METRICS);
	if (SMAASampleLevelZeroOffset(edgesTex, texcoord, int2(1, 0)).r > 0.0f)
	{
		d.yw = SMAASearchDiag2(edgesTex, texcoord, float2(1.0f, 1.0f), end, SMAA_RT_METRICS);
		d.y += float(end.y > 0.9f);
	}
	else
		d.yw = float2(0.0f, 0.0f);

	if (d.x + d.y > 2.0f) // d.x + d.y + 1 > 3
	{
		// Fetch the crossing edges:
		const float4 coords = mad(float4(-d.x, -d.x, d.y, d.y), SMAA_RT_METRICS.xyxy, texcoord.xyxy);
		float4 c;
		c.x  = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2(-1,  0)).g;
		c.y  = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2( 0, -1)).r;
		c.zw = SMAASampleLevelZeroOffset(edgesTex, coords.zw, int2( 1,  0)).gr;
		float2 cc = mad(float2(2.0f, 2.0f), c.xz, c.yw);

		// Remove the crossing edge if we didn't found the end of the line:
		cc = select(cc, float2(0.0f, 0.0f), bool2(step(0.9f, d.zw)));

		// Fetch the areas for this line:
		weights += SMAAAreaDiag(areaTex, d.xy, cc, subsampleIndices.w).gr;
	}

	return weights;
}

//-----------------------------------------------------------------------------
// Horizontal/Vertical Search Functions

// How much length should we add in the last step of the searches: 0, 1 or 2, from the bilinearly interpolated edge
// (see @PSEUDO_GATHER4).
static float SMAASearchLength(texture2d<float> searchTex, float2 e, float offset)
{
	// The texture is flipped vertically, with left and right cases taking half of the space horizontally:
	float2 scale = SMAA_SEARCHTEX_SIZE * float2(0.5f, -1.0f);
	float2 bias = SMAA_SEARCHTEX_SIZE * float2(offset, 1.0f);

	// Scale and bias to access texel centers:
	scale += float2(-1.0f,  1.0f);
	bias  += float2( 0.5f, -0.5f);

	// Convert from pixel coordinates to texcoords (the texture is cropped to SMAA_SEARCHTEX_PACKED_SIZE):
	scale *= 1.0f / SMAA_SEARCHTEX_PACKED_SIZE;
	bias *= 1.0f / SMAA_SEARCHTEX_PACKED_SIZE;

	// Lookup the search texture:
	return SMAASampleLevelZero(searchTex, mad(scale, e, bias)).r;
}

// Horizontal/vertical search functions for the 2nd pass.
static float SMAASearchXLeft(texture2d<float> edgesTex, texture2d<float> searchTex, float2 texcoord, float end, float4 SMAA_RT_METRICS)
{
	// @PSEUDO_GATHER4: this texcoord has been offset by (-0.25, -0.125) to sample between edges, thus fetching four
	// edges in a row; sampling with different offsets in each direction tells which of them are active.
	float2 e = float2(0.0f, 1.0f);
	while (texcoord.x > end &&
	       e.g > 0.8281f && // Is there some edge not activated?
	       e.r == 0.0f)     // Or is there a crossing edge that breaks the line?
	{
		e = SMAASampleLevelZero(edgesTex, texcoord).rg;
		texcoord = mad(-float2(2.0f, 0.0f), SMAA_RT_METRICS.xy, texcoord);
	}

	const float offset = mad(-(255.0f / 127.0f), SMAASearchLength(searchTex, e, 0.0f), 3.25f);
	return mad(SMAA_RT_METRICS.x, offset, texcoord.x);
}

static float SMAASearchXRight(texture2d<float> edgesTex, texture2d<float> searchTex, float2 texcoord, float end, float4 SMAA_RT_METRICS)
{
	float2 e = float2(0.0f, 1.0f);
	while (texcoord.x < end &&
	       e.g > 0.8281f && // Is there some edge not activated?
	       e.r == 0.0f)     // Or is there a crossing edge that breaks the line?
	{
		e = SMAASampleLevelZero(edgesTex, texcoord).rg;
		texcoord = mad(float2(2.0f, 0.0f), SMAA_RT_METRICS.xy, texcoord);
	}
	const float offset = mad(-(255.0f / 127.0f), SMAASearchLength(searchTex, e, 0.5f), 3.25f);
	return mad(-SMAA_RT_METRICS.x, offset, texcoord.x);
}

static float SMAASearchYUp(texture2d<float> edgesTex, texture2d<float> searchTex, float2 texcoord, float end, float4 SMAA_RT_METRICS)
{
	float2 e = float2(1.0f, 0.0f);
	while (texcoord.y > end &&
	       e.r > 0.8281f && // Is there some edge not activated?
	       e.g == 0.0f)     // Or is there a crossing edge that breaks the line?
	{
		e = SMAASampleLevelZero(edgesTex, texcoord).rg;
		texcoord = mad(-float2(0.0f, 2.0f), SMAA_RT_METRICS.xy, texcoord);
	}
	const float offset = mad(-(255.0f / 127.0f), SMAASearchLength(searchTex, e.gr, 0.0f), 3.25f);
	return mad(SMAA_RT_METRICS.y, offset, texcoord.y);
}

static float SMAASearchYDown(texture2d<float> edgesTex, texture2d<float> searchTex, float2 texcoord, float end, float4 SMAA_RT_METRICS)
{
	float2 e = float2(1.0f, 0.0f);
	while (texcoord.y < end &&
	       e.r > 0.8281f && // Is there some edge not activated?
	       e.g == 0.0f)     // Or is there a crossing edge that breaks the line?
	{
		e = SMAASampleLevelZero(edgesTex, texcoord).rg;
		texcoord = mad(float2(0.0f, 2.0f), SMAA_RT_METRICS.xy, texcoord);
	}
	const float offset = mad(-(255.0f / 127.0f), SMAASearchLength(searchTex, e.gr, 0.5f), 3.25f);
	return mad(-SMAA_RT_METRICS.y, offset, texcoord.y);
}

// Ok, we have the distance and both crossing edges. So, what are the areas at each side of current edge?
static float2 SMAAArea(texture2d<float> areaTex, float2 dist, float e1, float e2, float offset)
{
	// Rounding prevents precision errors of bilinear filtering:
	float2 texcoord = mad(float2(SMAA_AREATEX_MAX_DISTANCE, SMAA_AREATEX_MAX_DISTANCE), round(4.0f * float2(e1, e2)), dist);

	// We do a scale and bias for mapping to texel space:
	texcoord = mad(SMAA_AREATEX_PIXEL_SIZE, texcoord, 0.5f * SMAA_AREATEX_PIXEL_SIZE);

	// Move to proper place, according to the subpixel offset:
	texcoord.y = mad(SMAA_AREATEX_SUBTEX_SIZE, offset, texcoord.y);

	// Do it!
	return SMAASampleLevelZero(areaTex, texcoord).rg;
}

//-----------------------------------------------------------------------------
// Corner Detection Functions

static float2 SMAADetectHorizontalCornerPattern(texture2d<float> edgesTex, float2 weights, float4 texcoord, float2 d)
{
	const float2 leftRight = step(d.xy, d.yx);
	float2 rounding = (1.0f - SMAA_CORNER_ROUNDING_NORM) * leftRight;

	rounding /= leftRight.x + leftRight.y; // Reduce blending for pixels in the center of a line.

	float2 factor = float2(1.0f, 1.0f);
	factor.x -= rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(0,  1)).r;
	factor.x -= rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(1,  1)).r;
	factor.y -= rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(0, -2)).r;
	factor.y -= rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(1, -2)).r;

	return weights * saturate(factor);
}

static float2 SMAADetectVerticalCornerPattern(texture2d<float> edgesTex, float2 weights, float4 texcoord, float2 d)
{
	const float2 leftRight = step(d.xy, d.yx);
	float2 rounding = (1.0f - SMAA_CORNER_ROUNDING_NORM) * leftRight;

	rounding /= leftRight.x + leftRight.y;

	float2 factor = float2(1.0f, 1.0f);
	factor.x -= rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2( 1, 0)).g;
	factor.x -= rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2( 1, 1)).g;
	factor.y -= rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(-2, 0)).g;
	factor.y -= rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(-2, 1)).g;

	return weights * saturate(factor);
}

//-----------------------------------------------------------------------------
// Blending Weight Calculation Pixel Shader (Second Pass)

fragment float4 ps_smaa_weights(ConvertShaderData data [[stage_in]],
	texture2d<float> edgesTex [[texture(GSMTLTextureIndexNonHW)]],
	texture2d<float> areaTex [[texture(GSMTLTextureIndexTex)]],
	texture2d<float> searchTex [[texture(GSMTLTextureIndexPalette)]],
	constant float4& SMAA_RT_METRICS [[buffer(GSMTLBufferIndexUniforms)]])
{
	const float2 texcoord = data.t;
	const float4 subsampleIndices = float4(0.0f); // SMAA 1x
	const float2 pixcoord = texcoord * SMAA_RT_METRICS.zw;

	// We will use these offsets for the searches later on (see @PSEUDO_GATHER4):
	float4 offset[3];
	offset[0] = mad(SMAA_RT_METRICS.xyxy, float4(-0.25f, -0.125f,  1.25f, -0.125f), texcoord.xyxy);
	offset[1] = mad(SMAA_RT_METRICS.xyxy, float4(-0.125f, -0.25f, -0.125f,  1.25f), texcoord.xyxy);
	// And these for the searches, they indicate the ends of the loops:
	offset[2] = mad(SMAA_RT_METRICS.xxyy, float4(-2.0f, 2.0f, -2.0f, 2.0f) * float(SMAA_MAX_SEARCH_STEPS),
		float4(offset[0].xz, offset[1].yw));

	float4 weights = float4(0.0f, 0.0f, 0.0f, 0.0f);

	float2 e = SMAASample(edgesTex, texcoord).rg;

	if (e.g > 0.0f) // Edge at north
	{
		// Diagonals have both north and west edges, so searching for them in one of the boundaries is enough.
		weights.rg = SMAACalculateDiagWeights(edgesTex, areaTex, texcoord, e, subsampleIndices, SMAA_RT_METRICS);

		// We give priority to diagonals, so if we find a diagonal we skip horizontal/vertical processing.
		if (weights.r == -weights.g) // weights.r + weights.g == 0.0
		{
			float2 d;

			// Find the distance to the left:
			float3 coords;
			coords.x = SMAASearchXLeft(edgesTex, searchTex, offset[0].xy, offset[2].x, SMAA_RT_METRICS);
			coords.y = offset[1].y; // offset[1].y = texcoord.y - 0.25 * SMAA_RT_METRICS.y (@CROSSING_OFFSET)
			d.x = coords.x;

			// Now fetch the left crossing edges, two at a time using bilinear filtering. Sampling at -0.25 (see
			// @CROSSING_OFFSET) enables to discern what value each edge has:
			const float e1 = SMAASampleLevelZero(edgesTex, coords.xy).r;

			// Find the distance to the right:
			coords.z = SMAASearchXRight(edgesTex, searchTex, offset[0].zw, offset[2].y, SMAA_RT_METRICS);
			d.y = coords.z;

			// We want the distances to be in pixel units:
			d = abs(round(mad(SMAA_RT_METRICS.zz, d, -pixcoord.xx)));

			// SMAAArea below needs a sqrt, as the areas texture is compressed quadratically:
			const float2 sqrt_d = sqrt(d);

			// Fetch the right crossing edges:
			const float e2 = SMAASampleLevelZeroOffset(edgesTex, coords.zy, int2(1, 0)).r;

			// Ok, we know how this pattern looks like, now it is time for getting the actual area:
			weights.rg = SMAAArea(areaTex, sqrt_d, e1, e2, subsampleIndices.y);

			// Fix corners:
			coords.y = texcoord.y;
			weights.rg = SMAADetectHorizontalCornerPattern(edgesTex, weights.rg, coords.xyzy, d);
		}
		else
			e.r = 0.0f; // Skip vertical processing.
	}

	if (e.r > 0.0f) // Edge at west
	{
		float2 d;

		// Find the distance to the top:
		float3 coords;
		coords.y = SMAASearchYUp(edgesTex, searchTex, offset[1].xy, offset[2].z, SMAA_RT_METRICS);
		coords.x = offset[0].x; // offset[1].x = texcoord.x - 0.25 * SMAA_RT_METRICS.x;
		d.x = coords.y;

		// Fetch the top crossing edges:
		const float e1 = SMAASampleLevelZero(edgesTex, coords.xy).g;

		// Find the distance to the bottom:
		coords.z = SMAASearchYDown(edgesTex, searchTex, offset[1].zw, offset[2].w, SMAA_RT_METRICS);
		d.y = coords.z;

		// We want the distances to be in pixel units:
		d = abs(round(mad(SMAA_RT_METRICS.ww, d, -pixcoord.yy)));

		// SMAAArea below needs a sqrt, as the areas texture is compressed quadratically:
		const float2 sqrt_d = sqrt(d);

		// Fetch the bottom crossing edges:
		const float e2 = SMAASampleLevelZeroOffset(edgesTex, coords.xz, int2(0, 1)).g;

		// Get the area for this direction:
		weights.ba = SMAAArea(areaTex, sqrt_d, e1, e2, subsampleIndices.x);

		// Fix corners:
		coords.x = texcoord.x;
		weights.ba = SMAADetectVerticalCornerPattern(edgesTex, weights.ba, coords.xyxz, d);
	}

	return weights;
}

//-----------------------------------------------------------------------------
// Neighborhood Blending Pixel Shader (Third Pass)

fragment float4 ps_smaa_blend(ConvertShaderData data [[stage_in]],
	texture2d<float> colorTex [[texture(GSMTLTextureIndexNonHW)]],
	texture2d<float> blendTex [[texture(GSMTLTextureIndexTex)]],
	constant float4& SMAA_RT_METRICS [[buffer(GSMTLBufferIndexUniforms)]])
{
	const float2 texcoord = data.t;
	const float4 offset = mad(SMAA_RT_METRICS.xyxy, float4(1.0f, 0.0f, 0.0f, 1.0f), texcoord.xyxy);

	// Fetch the blending weights for current pixel:
	float4 a;
	a.x = SMAASample(blendTex, offset.xy).a; // Right
	a.y = SMAASample(blendTex, offset.zw).g; // Top
	a.wz = SMAASample(blendTex, texcoord).xz; // Bottom / Left

	// Is there any blending weight with a value greater than 0.0?
	if (dot(a, float4(1.0f, 1.0f, 1.0f, 1.0f)) < 1e-5f)
		return SMAASampleLevelZero(colorTex, texcoord);

	const bool h = max(a.x, a.z) > max(a.y, a.w); // max(horizontal) > max(vertical)

	// Calculate the blending offsets:
	const float4 blendingOffset = h ? float4(a.x, 0.0f, a.z, 0.0f) : float4(0.0f, a.y, 0.0f, a.w);
	float2 blendingWeight = h ? a.xz : a.yw;
	blendingWeight /= dot(blendingWeight, float2(1.0f, 1.0f));

	// Calculate the texture coordinates:
	const float4 blendingCoord = mad(blendingOffset, float4(SMAA_RT_METRICS.xy, -SMAA_RT_METRICS.xy), texcoord.xyxy);

	// We exploit bilinear filtering to mix current pixel with the chosen neighbor:
	float4 color = blendingWeight.x * SMAASampleLevelZero(colorTex, blendingCoord.xy);
	color += blendingWeight.y * SMAASampleLevelZero(colorTex, blendingCoord.zw);
	return color;
}
