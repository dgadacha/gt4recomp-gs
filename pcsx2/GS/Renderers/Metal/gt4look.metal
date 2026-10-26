// SPDX-FileCopyrightText: 2026 GT4Recomp contributors
// SPDX-License-Identifier: GPL-3.0+

// GT4Recomp fork (macOS port): the bloom and film look of the GS output ([GT4GS] Bloom and FilmLook,
// GSDeviceMTL::ApplyGT4Look), at the internal resolution, after shade boost and FXAA.
//
// Bloom: the picture's light above a soft threshold, in linear light, is filtered down a chain of half-size targets
// (13-tap downsample) and back up (tent upsample, each level added to the next larger one), then blended over the
// picture with a screen blend, which cannot go past white. Film look: a gentle S-curve (deeper shadows, softer
// highlights, punchier midtones), cool shadows and warm highlights, and a light vignette.

#include "GSMTLShaderCommon.h"

using namespace metal;

constexpr sampler gt4_linear(coord::normalized, filter::linear, address::clamp_to_edge);

static float3 gt4_to_linear(float3 c)
{
	return pow(max(c, 0.0f), 2.2f);
}

static float3 gt4_to_display(float3 c)
{
	return pow(saturate(c), 1.0f / 2.2f);
}

// 13 bilinear taps over a 4x4 texel area (the "Call of Duty: Advanced Warfare" downsample), weights summing to 1.
static float3 gt4_down13(texture2d<float> tex, float2 uv)
{
	const float2 t = 1.0f / float2(tex.get_width(), tex.get_height());
	const float3 a = tex.sample(gt4_linear, uv + t * float2(-2, -2)).rgb;
	const float3 b = tex.sample(gt4_linear, uv + t * float2( 0, -2)).rgb;
	const float3 c = tex.sample(gt4_linear, uv + t * float2( 2, -2)).rgb;
	const float3 d = tex.sample(gt4_linear, uv + t * float2(-1, -1)).rgb;
	const float3 e = tex.sample(gt4_linear, uv + t * float2( 1, -1)).rgb;
	const float3 f = tex.sample(gt4_linear, uv + t * float2(-2,  0)).rgb;
	const float3 g = tex.sample(gt4_linear, uv).rgb;
	const float3 h = tex.sample(gt4_linear, uv + t * float2( 2,  0)).rgb;
	const float3 i = tex.sample(gt4_linear, uv + t * float2(-1,  1)).rgb;
	const float3 j = tex.sample(gt4_linear, uv + t * float2( 1,  1)).rgb;
	const float3 k = tex.sample(gt4_linear, uv + t * float2(-2,  2)).rgb;
	const float3 l = tex.sample(gt4_linear, uv + t * float2( 0,  2)).rgb;
	const float3 m = tex.sample(gt4_linear, uv + t * float2( 2,  2)).rgb;
	return g * 0.125f + (a + c + k + m) * 0.03125f + (b + f + h + l) * 0.0625f + (d + e + i + j) * 0.125f;
}

// 3x3 tent filter over the texture's own texels.
static float3 gt4_tent(texture2d<float> tex, float2 uv)
{
	const float2 t = 1.0f / float2(tex.get_width(), tex.get_height());
	float3 sum = tex.sample(gt4_linear, uv).rgb * 4.0f;
	sum += (tex.sample(gt4_linear, uv + float2(-t.x, 0)).rgb + tex.sample(gt4_linear, uv + float2(t.x, 0)).rgb +
	        tex.sample(gt4_linear, uv + float2(0, -t.y)).rgb + tex.sample(gt4_linear, uv + float2(0, t.y)).rgb) * 2.0f;
	sum += tex.sample(gt4_linear, uv + float2(-t.x, -t.y)).rgb + tex.sample(gt4_linear, uv + float2(t.x, -t.y)).rgb +
	       tex.sample(gt4_linear, uv + float2(-t.x, t.y)).rgb + tex.sample(gt4_linear, uv + float2(t.x, t.y)).rgb;
	return sum * (1.0f / 16.0f);
}

// The picture (display values) to its light above the threshold, in linear light, at half size. cb: threshold, knee.
fragment float4 ps_gt4_bloom_prefilter(ConvertShaderData data [[stage_in]],
	texture2d<float> tex [[texture(GSMTLTextureIndexNonHW)]], constant float4& cb [[buffer(GSMTLBufferIndexUniforms)]])
{
	const float3 c = gt4_to_linear(gt4_down13(tex, data.t));
	const float brightness = max(c.r, max(c.g, c.b));
	const float threshold = cb.x, knee = cb.y;
	float soft = clamp(brightness - threshold + knee, 0.0f, 2.0f * knee);
	soft = soft * soft / (4.0f * knee + 1e-5f);
	const float contribution = max(soft, brightness - threshold) / max(brightness, 1e-5f);
	return float4(c * contribution, 1.0f);
}

fragment float4 ps_gt4_bloom_down(ConvertShaderData data [[stage_in]], texture2d<float> tex [[texture(GSMTLTextureIndexNonHW)]])
{
	return float4(gt4_down13(tex, data.t), 1.0f);
}

// Added to the next larger level by the pipeline's blending.
fragment float4 ps_gt4_bloom_up(ConvertShaderData data [[stage_in]], texture2d<float> tex [[texture(GSMTLTextureIndexNonHW)]])
{
	return float4(gt4_tent(tex, data.t), 1.0f);
}

// cb: bloom strength (0 off), film look (0 or 1), vignette strength, picture aspect ratio.
fragment float4 ps_gt4_look(ConvertShaderData data [[stage_in]],
	texture2d<float> tex [[texture(GSMTLTextureIndexNonHW)]], texture2d<float> bloom [[texture(GSMTLTextureIndexTex)]],
	constant float4& cb [[buffer(GSMTLBufferIndexUniforms)]])
{
	const float4 picture = tex.read(uint2(data.p.xy));
	float3 c = picture.rgb;
	if (cb.x > 0.0f)
	{
		const float3 light = gt4_to_linear(c);
		const float3 glow = saturate(gt4_tent(bloom, data.t) * cb.x);
		c = gt4_to_display(1.0f - (1.0f - light) * (1.0f - glow));
	}
	if (cb.y > 0.0f)
	{
		const float3 curve = c * c * (3.0f - 2.0f * c);
		c = mix(c, curve, 0.35f);
		const float luminance = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
		const float3 tint = mix(float3(0.98f, 1.0f, 1.03f), float3(1.03f, 1.0f, 0.97f), smoothstep(0.2f, 0.8f, luminance));
		c = saturate(c * tint);
		const float2 offset = (data.t - 0.5f) * float2(cb.w, 1.0f);
		const float distance = length(offset) / length(float2(cb.w, 1.0f) * 0.5f);
		c *= 1.0f - cb.z * smoothstep(0.45f, 1.0f, distance);
	}
	return float4(c, picture.a);
}
