// SPDX-FileCopyrightText: 2026 GT4Recomp contributors
// SPDX-License-Identifier: GPL-3.0+

// GT4Recomp fork (macOS port): contact shadows (screen-space ambient occlusion, [GT4GS] AO) of GT4's 3D scene, from its
// depth target, before the game's screen effects and HUD (GSRendererHW::GT4ContactShadows,
// GSDeviceMTL::ApplyGT4ContactShadows).
//
// GT4 draws with depth test GEQUAL: the depth target holds Z * 2^-32 (tfx.metal), larger nearer, about proportional
// to 1 / distance. 1e6 / Z gives a view distance in arbitrary units, about 2.4 m each on the Nurburgring (car widths).
// A point's view position is rebuilt from its pixel, that distance and an assumed field of view; its normal from its
// neighbours. The occlusion is the scalable-ambient-obscurance estimate: 12 points on a spiral around the pixel (turned by
// a per-pixel noise), each one counting by how much it stands above the surface, within twice the radius.
//
// The occlusion is a soft, low-frequency term: it is computed at a reduced resolution (the scene divided by an integer
// factor, about 1,000 pixels wide whatever the internal resolution), which keeps the samples close in memory.
// Pass 1, ps_gt4_ao_depth: the view distance of every reduced pixel (0: nothing drawn), RGBA16Float.
// Pass 2, ps_gt4_ao: the occlusion (1 is unoccluded) and the distance of every reduced pixel, RGBA16Float.
// Pass 3, ps_gt4_ao_apply: at full resolution, the four nearest reduced pixels weighted bilinearly and by depth
// similarity, multiplied into the scene's colour by the pipeline's blending (colour channels only); pixels without a
// shadow are discarded, so they cost no write.

#include "GSMTLShaderCommon.h"

using namespace metal;

struct GT4AOUniform
{
	float4 view; ///< tan(half horizontal field of view), tan(half vertical field of view), radius (view units), strength
	float4 area; ///< the scene's rectangle in full-resolution pixels: x, y, width, height
	float4 reduced; ///< reduction factor, largest sample radius in reduced pixels, depth texture width, depth texture height
	float4 shape; ///< angle bias (a sample counts only above it), 0, 0, 0
};

constant int GT4_AO_TAPS = 12;

static float gt4_ao_depth_distance(depth2d<float> depth, int2 pixel, constant GT4AOUniform& u)
{
	if (pixel.x < 0 || pixel.y < 0 || pixel.x >= int(u.reduced.z) || pixel.y >= int(u.reduced.w))
		return 0.0f;
	const float z = depth.read(uint2(pixel)) * 4294967296.0f;
	return z >= 1.0f ? 1.0e6f / z : 0.0f; // 0: nothing drawn there
}

// The view position of a point at `pixel` (full-resolution pixels, fractional) and `distance`.
static float3 gt4_ao_position(float2 pixel, float distance, constant GT4AOUniform& u)
{
	const float2 uv = (pixel - u.area.xy) / u.area.zw;
	return float3((uv.x * 2.0f - 1.0f) * distance * u.view.x, (1.0f - uv.y * 2.0f) * distance * u.view.y, distance);
}

fragment float4 ps_gt4_ao_depth(ConvertShaderData data [[stage_in]], depth2d<float> depth [[texture(GSMTLTextureIndexNonHW)]],
	constant GT4AOUniform& u [[buffer(GSMTLBufferIndexUniforms)]])
{
	const int factor = int(u.reduced.x);
	const int2 pixel = int2(data.p.xy) * factor + factor / 2;
	return float4(gt4_ao_depth_distance(depth, pixel, u), 0.0f, 0.0f, 1.0f);
}

fragment float4 ps_gt4_ao(ConvertShaderData data [[stage_in]], texture2d<float> distances [[texture(GSMTLTextureIndexNonHW)]],
	constant GT4AOUniform& u [[buffer(GSMTLBufferIndexUniforms)]])
{
	const int2 size = int2(distances.get_width(), distances.get_height());
	const int2 pixel = int2(data.p.xy);
	const float factor = u.reduced.x;
	const float distance = distances.read(uint2(pixel)).r;
	if (distance <= 0.0f)
		return float4(1.0f, 0.0f, 0.0f, 1.0f);
	const float3 p = gt4_ao_position((float2(pixel) + 0.5f) * factor, distance, u);

	// The normal from the neighbours, taking the smaller difference on each axis so that edges do not bend it.
	float3 neighbour[4];
	const int2 steps[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
	for (int i = 0; i < 4; i++)
	{
		const int2 n_pixel = clamp(pixel + steps[i], int2(0), size - 1);
		const float d = distances.read(uint2(n_pixel)).r;
		neighbour[i] = d > 0.0f ? gt4_ao_position((float2(n_pixel) + 0.5f) * factor, d, u) : p;
	}
	const float3 left = p - neighbour[0], right = neighbour[1] - p, up = p - neighbour[2], down = neighbour[3] - p;
	const float3 dx = abs(left.z) < abs(right.z) ? left : right;
	const float3 dy = abs(up.z) < abs(down.z) ? up : down;
	float3 n = cross(dx, dy);
	const float n_length = length(n);
	if (n_length <= 0.0f)
		return float4(1.0f, distance, 0.0f, 1.0f);
	n /= n_length;
	if (dot(n, p) > 0.0f)
		n = -n; // facing the camera, at the origin

	const float radius = u.view.z;
	const float r_px = min(radius / (distance * u.view.x) * (u.area.z * 0.5f) / factor, u.reduced.y);
	if (r_px < 1.0f)
		return float4(1.0f, distance, 0.0f, 1.0f);
	const float noise = fract(52.9829189f * fract(dot(data.p.xy, float2(0.06711056f, 0.00583715f))));
	const float range2 = 4.0f * radius * radius;
	float occlusion = 0.0f;
	for (int i = 0; i < GT4_AO_TAPS; i++)
	{
		const float angle = (float(i) + noise) * 2.39996323f;
		const float2 offset = float2(cos(angle), sin(angle)) * ((float(i) + 0.5f) / float(GT4_AO_TAPS)) * r_px;
		const int2 sample_pixel = pixel + int2(round(offset));
		if (any(sample_pixel < 0) || any(sample_pixel >= size))
			continue;
		const float sample_distance = distances.read(uint2(sample_pixel)).r;
		if (sample_distance <= 0.0f)
			continue;
		const float3 v = gt4_ao_position((float2(sample_pixel) + 0.5f) * factor, sample_distance, u) - p;
		const float vv = dot(v, v);
		if (vv > range2)
			continue;
		occlusion += max(0.0f, dot(v, n) * rsqrt(vv + 1e-9f) - u.shape.x) * (1.0f - vv / range2);
	}
	return float4(saturate(1.0f - u.view.w * occlusion / float(GT4_AO_TAPS)), distance, 0.0f, 1.0f);
}

fragment float4 ps_gt4_ao_apply(ConvertShaderData data [[stage_in]], depth2d<float> depth [[texture(GSMTLTextureIndexNonHW)]],
	texture2d<float> occlusion [[texture(GSMTLTextureIndexTex)]], constant GT4AOUniform& u [[buffer(GSMTLBufferIndexUniforms)]])
{
	const int2 pixel = int2(data.p.xy);
	const float distance = gt4_ao_depth_distance(depth, pixel, u);
	if (distance <= 0.0f)
		discard_fragment(); // nothing drawn: no shadow, and no write
	const int2 size = int2(occlusion.get_width(), occlusion.get_height());
	const float2 position = (float2(pixel) + 0.5f) / u.reduced.x - 0.5f;
	const int2 base = int2(floor(position));
	const float2 f = position - float2(base);
	float sum = 0.0f, weight = 0.0f;
	for (int y = 0; y <= 1; y++)
	{
		for (int x = 0; x <= 1; x++)
		{
			const float2 sample = occlusion.read(uint2(clamp(base + int2(x, y), int2(0), size - 1))).rg;
			const float bilinear = (x ? f.x : 1.0f - f.x) * (y ? f.y : 1.0f - f.y) + 1e-4f;
			const float w = (sample.y > 0.0f && abs(sample.y - distance) < 0.05f * distance) ? bilinear : 0.0f;
			sum += sample.x * w;
			weight += w;
		}
	}
	const float shade = weight > 0.0f ? sum / weight : 1.0f;
	if (shade > 0.998f)
		discard_fragment(); // unshaded: most of the picture, left as it is
	return float4(shade, shade, shade, 1.0f);
}
