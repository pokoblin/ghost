// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/negotiation.hpp"

#include <algorithm>

namespace gdp {

namespace {

bool contains(const std::vector<std::string> &list, const std::string &name) {
	return std::find(list.begin(), list.end(), name) != list.end();
}

// `offered` filtered to what `supported` also has, in `offered`'s order,
// duplicates dropped: the one rule behind both negotiations.
std::vector<std::string> intersect_in_order(const std::vector<std::string> &offered,
	const std::vector<std::string> &supported) {
	std::vector<std::string> common;
	for (const auto &name : offered) {
		if (contains(supported, name) && !contains(common, name)) {
			common.push_back(name);
		}
	}
	return common;
}

} // namespace

std::vector<std::string> common_video_codecs(const std::vector<std::string> &offered,
	const std::vector<std::string> &supported) {
	return intersect_in_order(offered, supported);
}

std::vector<std::string> negotiate_capabilities(const std::vector<std::string> &offered,
	const std::vector<std::string> &supported) {
	return intersect_in_order(offered, supported);
}

bool has_capability(const std::vector<std::string> &negotiated, const std::string &name) {
	return contains(negotiated, name);
}

bool declares_bit_depth(const std::vector<CodecFormat> &declared, const std::string &codec,
	uint32_t bit_depth) {
	if (bit_depth == 8) {
		return true; // the baseline, gdp-spec.md §7.3
	}
	for (const CodecFormat &format : declared) {
		if (format.codec != codec) {
			continue;
		}
		if (std::find(format.bit_depths.begin(), format.bit_depths.end(), bit_depth) !=
			format.bit_depths.end()) {
			return true;
		}
	}
	return false;
}

uint32_t negotiate_bit_depth(const std::vector<CodecFormat> &declared, const std::string &codec,
	const std::vector<uint32_t> &wanted) {
	for (uint32_t depth : wanted) {
		if (declares_bit_depth(declared, codec, depth)) {
			return depth;
		}
	}
	return 8;
}

} // namespace gdp
