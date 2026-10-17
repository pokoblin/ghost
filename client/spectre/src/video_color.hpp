// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// How the host's decoded pictures turn back into RGB: OutputDescriptor.
// color (gdp-spec.md §7.3) as spectre keeps it, from SessionAccept and
// every DisplaysChanged. SessionClient fills it in, the presenter builds
// its samplers from it (present/video_image_source.hpp): the matrix and
// range are what the ycbcr conversions are made with, the depth which
// image formats the frames arrive in (NV12 or P010, 8- or 16-bit planes
// for PyroWave). Primaries and transfer are not here yet: the host never
// sends anything but BT.709/sRGB, and HDR will add them to this one
// struct rather than to the sampler code around it.
#pragma once

#include <cstdint>

namespace spectre {

enum class ColorMatrix {
	Bt601,
	Bt709,
	Bt2020Ncl,
};

enum class ColorRange {
	Limited,
	Full,
};

struct VideoColor {
	ColorMatrix matrix = ColorMatrix::Bt601;
	ColorRange range = ColorRange::Limited;
	// Bits per component of the coded pictures: 8 or 10. The host picks it
	// within what SessionHello declared (gdp-spec.md §7.3).
	uint32_t bit_depth = 8;

	bool operator==(const VideoColor &o) const {
		return matrix == o.matrix && range == o.range && bit_depth == o.bit_depth;
	}
	bool operator!=(const VideoColor &o) const { return !(*this == o); }
};

} // namespace spectre
