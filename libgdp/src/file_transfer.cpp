// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/file_transfer.hpp"

#include "gdp/error_codes.hpp"
#include "gdp/framing.hpp"
#include "session.pb.h"

namespace gdp {

namespace {

// The length of the UTF-8 sequence starting at s[i], or 0 if it isn't a
// valid one: no overlong forms, surrogates or code points past U+10FFFF.
size_t utf8_sequence(const std::string &s, size_t i) {
	auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
	auto cont = [&](size_t k) { return k < s.size() && (byte(k) & 0xC0) == 0x80; };
	unsigned char c = byte(i);
	if (c < 0x80) {
		return 1;
	}
	if (c >= 0xC2 && c <= 0xDF) {
		return cont(i + 1) ? 2 : 0;
	}
	if (c >= 0xE0 && c <= 0xEF) {
		if (!cont(i + 1) || !cont(i + 2)) {
			return 0;
		}
		unsigned char c1 = byte(i + 1);
		if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) {
			return 0; // overlong, or a UTF-16 surrogate
		}
		return 3;
	}
	if (c >= 0xF0 && c <= 0xF4) {
		if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3)) {
			return 0;
		}
		unsigned char c1 = byte(i + 1);
		if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) {
			return 0; // overlong, or past U+10FFFF
		}
		return 4;
	}
	return 0;
}

} // namespace

bool valid_file_name(const std::string &name) {
	if (name.empty() || name.size() > kMaxFileNameBytes || name == "." || name == "..") {
		return false;
	}
	for (size_t i = 0; i < name.size();) {
		unsigned char c = static_cast<unsigned char>(name[i]);
		if (c < 0x20 || c == 0x7F || c == '/' || c == '\\') {
			return false;
		}
		size_t n = utf8_sequence(name, i);
		if (n == 0) {
			return false;
		}
		i += n;
	}
	return true;
}

std::string numbered_file_name(const std::string &name, unsigned n) {
	size_t dot = name.rfind('.');
	bool has_extension = dot != std::string::npos && dot != 0 && dot + 1 < name.size();
	std::string stem = has_extension ? name.substr(0, dot) : name;
	std::string extension = has_extension ? name.substr(dot) : std::string();
	std::string suffix = " (" + std::to_string(n) + ")";
	// Too long: drop whole characters off the end of the stem (never a
	// continuation byte on its own), then, if even that can't make room,
	// off the extension too.
	while (!stem.empty() && stem.size() + suffix.size() + extension.size() > kMaxFileNameBytes) {
		size_t cut = stem.size() - 1;
		while (cut > 0 && (static_cast<unsigned char>(stem[cut]) & 0xC0) == 0x80) {
			cut--;
		}
		stem.erase(cut);
	}
	while (!extension.empty() && stem.size() + suffix.size() + extension.size() > kMaxFileNameBytes) {
		size_t cut = extension.size() - 1;
		while (cut > 0 && (static_cast<unsigned char>(extension[cut]) & 0xC0) == 0x80) {
			cut--;
		}
		extension.erase(cut);
	}
	return stem + suffix + extension;
}

FileStreamReader::State FileStreamReader::feed(const uint8_t *data, size_t len) {
	if (state_ == State::kError) {
		return state_;
	}
	buf_.insert(buf_.end(), data, data + len);
	if (state_ == State::kOpened) {
		return state_;
	}
	// One §3.1 frame, then raw bytes: FrameReader would take the bytes
	// after the frame into its own buffer, so the frame is cut out here.
	if (buf_.size() < kLengthPrefixSize) {
		return state_;
	}
	uint32_t frame_len = static_cast<uint32_t>(buf_[0]) | static_cast<uint32_t>(buf_[1]) << 8 |
		static_cast<uint32_t>(buf_[2]) << 16 | static_cast<uint32_t>(buf_[3]) << 24;
	if (frame_len > kMaxFileOpenFrameSize) {
		state_ = State::kError;
		error_code_ = static_cast<uint64_t>(ErrorCode::kFrameTooLarge);
		return state_;
	}
	size_t frame_end = kLengthPrefixSize + frame_len;
	if (buf_.size() < frame_end) {
		return state_;
	}
	session::StreamOpen open;
	if (frame_len != 0 && !open.ParseFromArray(buf_.data() + kLengthPrefixSize, static_cast<int>(frame_len))) {
		state_ = State::kError;
		error_code_ = static_cast<uint64_t>(ErrorCode::kMalformedFrame);
		return state_;
	}
	kind_ = open.kind();
	params_ = open.params();
	buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(frame_end));
	state_ = State::kOpened;
	return state_;
}

std::vector<uint8_t> FileStreamReader::take_data() {
	if (state_ != State::kOpened) {
		return {};
	}
	std::vector<uint8_t> out;
	out.swap(buf_);
	return out;
}

} // namespace gdp
