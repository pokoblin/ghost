// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The colour description every encoder writes into its stream (H.264 and
// H.265 VUI, AV1's color_config, x264's and NVENC's equivalents) for a
// session's ColorMatrix, as ITU-T H.273 code points -- the one table, so
// the backends can't drift apart. Always studio range.
//
// A client is told the matrix by OutputDescriptor.color (gdp-spec.md
// §7.3) and must not need this, but any decoder reading the stream on its
// own (a player on wraith -e's dump, the browser client through
// WebCodecs) goes by these, and one left to guess takes HD video as
// BT.709.
#pragma once

#include "encode/encoder.hpp"

#include <cstdint>

namespace wraith {

struct ColourDescription {
	uint8_t primaries;
	uint8_t transfer;
	uint8_t matrix;
};

inline ColourDescription colour_description(ColorMatrix matrix) {
	switch (matrix) {
	case ColorMatrix::Bt709:
		// BT.709 primaries and matrix with the sRGB transfer: what the
		// desktop is (OutputDescriptor.color's primaries and transfer).
		return {1, 13, 1};
	case ColorMatrix::Bt601: break;
	}
	// SMPTE 170M (BT.601) throughout: what every stream has said since the
	// colour description was first written, kept as it was for the default.
	return {6, 6, 6};
}

} // namespace wraith
