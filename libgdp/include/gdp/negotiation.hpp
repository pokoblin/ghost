// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Session-level negotiation helpers (gdp-spec.md §6.6, §6.7): which video
// codec a session runs on, and which optional capabilities both ends
// agreed to. Pure string-list logic shared by wraith (the side that
// chooses) and spectre (the side that checks the choice against what it
// offered), so the two can't disagree about the rules.
#pragma once

// The tokens being negotiated: video codecs (video_codec.hpp) and the one
// audio codec (audio_format.hpp).
#include "gdp/audio_format.hpp"
#include "gdp/video_codec.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gdp {

// The video codecs a session may run on: every entry of `offered` (the
// client's preference order, SessionHello.codecs) that also appears in
// `supported` (what the server can encode), in `offered`'s order,
// duplicates dropped. Empty if there is no common codec -- including when
// `offered` is empty; a client that offers nothing gets nothing, rather
// than a guess.
//
// A list rather than a single pick because "can encode" is not fully
// knowable without trying: a codec can have a registered backend that
// then fails to open on this particular host (no hardware entrypoint for
// the profile). The server walks the list until one opens and accepts
// that one, which is what gdp-spec.md §6.6's "the first entry of `codecs`
// it can encode" means in practice.
std::vector<std::string> common_video_codecs(const std::vector<std::string> &offered,
	const std::vector<std::string> &supported);

// The capability set a session runs with: every entry of `offered`
// (SessionHello.capabilities) that also appears in `supported`, in
// `offered`'s order, duplicates dropped. Anything only one side knows
// about is silently left out -- that's the whole point of the mechanism
// (gdp-spec.md §6.7): a peer that doesn't recognize a
// name never has to act on it.
std::vector<std::string> negotiate_capabilities(const std::vector<std::string> &offered,
	const std::vector<std::string> &supported);

bool has_capability(const std::vector<std::string> &negotiated, const std::string &name);

// SessionHello/SessionAccept.capabilities name (gdp-spec.md §6.7, §7.3):
// the client applies every matrix and range this version defines, as
// OutputDescriptor.color states them, from SessionAccept and every
// DisplaysChanged on. Without it the host keeps to each codec's default
// colour (§7.3's table), which a client that ignores the descriptor
// already assumes.
inline constexpr const char *kCapabilityColor = "color";

// One SessionHello.codec_formats entry (gdp-spec.md §7.3): the bit depths
// a client decodes `codec` at besides the 8-bit baseline. Chroma
// subsamplings are not negotiated yet (every wraith encoder is 4:2:0), so
// they are not carried here.
struct CodecFormat {
	std::string codec;
	std::vector<uint32_t> bit_depths;
};

// Whether `declared` (the client's codec_formats) lists `bit_depth` for
// `codec`. 8 is the baseline every client takes, so it is always declared;
// an empty list, or a codec with no entry, declares nothing beyond it.
bool declares_bit_depth(const std::vector<CodecFormat> &declared, const std::string &codec,
	uint32_t bit_depth);

// The bit depth a session on `codec` runs at: the highest of `wanted`
// (the host's preference order, e.g. {10, 8}) the client declared for the
// codec, or 8 when none is. The host MUST stay within the declaration
// (gdp-spec.md §7.3) and 8 always is, so this never fails; whether the
// host's encoder actually opens at the depth is the host's own business
// (it falls back a step when it doesn't).
uint32_t negotiate_bit_depth(const std::vector<CodecFormat> &declared, const std::string &codec,
	const std::vector<uint32_t> &wanted);

} // namespace gdp
