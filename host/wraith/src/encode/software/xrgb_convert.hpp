// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// XRGB8888 -> I420 (BT.601 or BT.709, limited range), the one colour conversion wraith
// does on the CPU: the software H.264 encoder's input (x264_encoder.cpp).
// VA-API converts on the GPU (VaapiEncoderBase's VPP), and the hardware
// backends' push_cpu() paths upload RGB as it is.
//
// It is worth having a SIMD version: at 4K the scalar loop costs ~12ms per
// frame, most of a 60fps budget on its own, and it runs on wraith's main
// thread.
#pragma once

#include <cstdint>

namespace wraith {

// A matrix's limited-range coefficients in 8.8 fixed point:
// Y = (yr*R + yg*G + yb*B + 128) >> 8, plus 16; U and V likewise from
// (ur, ug, ub) and (vr, vg, vb), plus 128. U's and V's each sum to 0, so
// grey has no chroma; Y's sum to 220, so white is 235.
struct YcbcrCoefficients {
	int yr, yg, yb;
	int ur, ug, ub;
	int vr, vg, vb;
};
// ITU-R BT.601-7 §2.5.1, rounded.
inline constexpr YcbcrCoefficients kBt601Coefficients = {66, 129, 25, -38, -74, 112, 112, -94, -18};
// ITU-R BT.709-6 §3, rounded. U's -86 rather than the nearer -87 keeps its
// row summing to 0.
inline constexpr YcbcrCoefficients kBt709Coefficients = {47, 157, 16, -26, -86, 112, 112, -102, -10};

// Converts `width` x `height` XRGB8888 pixels at `src` (bytes B,G,R,X per
// pixel; `src_stride` bytes per row) into three tightly packed planes:
// `y_plane` at width x height, `u_plane` and `v_plane` each at
// ((width+1)/2) x ((height+1)/2).
//
// Picks the fastest implementation this CPU supports; the result is
// byte-identical whichever runs, which xrgb_convert_test checks.
void xrgb_to_i420(const uint8_t *src, uint32_t width, uint32_t height, uint32_t src_stride, uint8_t *y_plane,
	uint8_t *u_plane, uint8_t *v_plane, const YcbcrCoefficients &coefficients = kBt601Coefficients);

// The portable implementation, straight from the matrix's formula. Exposed
// only so the test has something to compare the SIMD paths against --
// callers want xrgb_to_i420().
void xrgb_to_i420_scalar(const uint8_t *src, uint32_t width, uint32_t height, uint32_t src_stride,
	uint8_t *y_plane, uint8_t *u_plane, uint8_t *v_plane,
	const YcbcrCoefficients &coefficients = kBt601Coefficients);

// Which implementation xrgb_to_i420() dispatches to on this CPU ("avx2" or
// "scalar"), for the log line at encoder open.
const char *xrgb_to_i420_impl_name();

} // namespace wraith
