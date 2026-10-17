// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#version 450

// `videoSampler` is a combined image sampler with an immutable sampler
// (video_image_source.hpp). On Linux that sampler carries a
// VkSamplerYcbcrConversion over an NV12 or P010 image, so the YCbCr -> RGB
// conversion happens in fixed-function sampling hardware; on Windows the
// image is already RGB (8- or 10-bit). Either way texture() below returns
// RGB as floats and this shader does no colour math.
//
// What it does do is dither: the swapchain is 8-bit, and a 10-bit frame
// written to it with plain rounding would get its gradient steps back on
// the way out. Half an LSB of per-pixel noise before the rounding turns
// each step into a scatter the eye averages out. An 8-bit source rounds
// back to itself (the noise is within half a step of the exact value),
// so the same line serves every session.
layout(set = 0, binding = 0) uniform sampler2D videoSampler;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main() {
	vec3 rgb = texture(videoSampler, inUV).rgb;
	float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	outColor = vec4(rgb + (noise - 0.5) / 255.0, 1.0);
}
