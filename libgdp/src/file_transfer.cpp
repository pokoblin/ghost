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

FrameThenRawReader::State FrameThenRawReader::feed(const uint8_t *data, size_t len) {
	if (state_ == State::kError) {
		return state_;
	}
	buf_.insert(buf_.end(), data, data + len);
	if (state_ == State::kFramed || buf_.size() < kLengthPrefixSize) {
		return state_;
	}
	uint32_t frame_len = static_cast<uint32_t>(buf_[0]) | static_cast<uint32_t>(buf_[1]) << 8 |
		static_cast<uint32_t>(buf_[2]) << 16 | static_cast<uint32_t>(buf_[3]) << 24;
	if (frame_len > max_frame_size_) {
		state_ = State::kError;
		error_code_ = static_cast<uint64_t>(ErrorCode::kFrameTooLarge);
		return state_;
	}
	size_t frame_end = kLengthPrefixSize + frame_len;
	if (buf_.size() < frame_end) {
		return state_;
	}
	frame_.assign(reinterpret_cast<const char *>(buf_.data()) + kLengthPrefixSize, frame_len);
	buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(frame_end));
	state_ = State::kFramed;
	return state_;
}

std::vector<uint8_t> FrameThenRawReader::take_data() {
	if (state_ != State::kFramed) {
		return {};
	}
	std::vector<uint8_t> out;
	out.swap(buf_);
	return out;
}

FileStreamReader::State FileStreamReader::feed(const uint8_t *data, size_t len) {
	if (state_ != State::kWaiting) {
		if (state_ == State::kOpened) {
			reader_.feed(data, len);
		}
		return state_;
	}
	switch (reader_.feed(data, len)) {
	case FrameThenRawReader::State::kWaiting: return state_;
	case FrameThenRawReader::State::kError:
		state_ = State::kError;
		error_code_ = reader_.error_code();
		return state_;
	case FrameThenRawReader::State::kFramed: break;
	}
	session::StreamOpen open;
	if (!open.ParseFromString(reader_.frame())) {
		state_ = State::kError;
		error_code_ = static_cast<uint64_t>(ErrorCode::kMalformedFrame);
		return state_;
	}
	kind_ = open.kind();
	params_ = open.params();
	state_ = State::kOpened;
	return state_;
}

std::vector<uint8_t> FileStreamReader::take_data() {
	return state_ == State::kOpened ? reader_.take_data() : std::vector<uint8_t>();
}

namespace {

constexpr const char *kGnomeCopiedFiles = "x-special/gnome-copied-files";
constexpr const char *kUriList = "text/uri-list";

int hex_value(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// The local path of a file:// URI, or "" if it isn't one.
std::string local_path(const std::string &uri) {
	static const std::string scheme = "file://";
	if (uri.size() <= scheme.size() || uri.compare(0, scheme.size(), scheme) != 0) {
		return "";
	}
	std::string rest = uri.substr(scheme.size());
	size_t slash = rest.find('/');
	if (slash == std::string::npos) {
		return "";
	}
	std::string host = rest.substr(0, slash);
	if (!host.empty() && host != "localhost") {
		return "";
	}
	std::string path;
	for (size_t i = slash; i < rest.size(); i++) {
		if (rest[i] == '%' && i + 2 < rest.size() && hex_value(rest[i + 1]) >= 0 && hex_value(rest[i + 2]) >= 0) {
			path += static_cast<char>(hex_value(rest[i + 1]) * 16 + hex_value(rest[i + 2]));
			i += 2;
		} else {
			path += rest[i];
		}
	}
	return path.find('\0') == std::string::npos ? path : "";
}

} // namespace

std::string select_copied_files_mime(const std::vector<std::string> &offered) {
	for (const char *mime : {kGnomeCopiedFiles, kUriList}) {
		for (const std::string &o : offered) {
			if (o == mime) {
				return mime;
			}
		}
	}
	return "";
}

std::vector<std::string> copied_file_paths(const std::string &mime, const std::string &data) {
	std::vector<std::string> paths;
	bool gnome = mime == kGnomeCopiedFiles;
	if (!gnome && mime != kUriList) {
		return paths;
	}
	size_t start = 0;
	bool first = true;
	while (start <= data.size()) {
		size_t end = data.find('\n', start);
		std::string line = data.substr(start, end == std::string::npos ? std::string::npos : end - start);
		start = end == std::string::npos ? data.size() + 1 : end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == '\0')) {
			line.pop_back();
		}
		// GNOME's first line is the action ("copy" or "cut"); a URI list's
		// comments start with '#'.
		bool skip = (gnome && first) || line.empty() || line[0] == '#';
		first = false;
		if (skip) {
			continue;
		}
		std::string path = local_path(line);
		if (!path.empty()) {
			paths.push_back(std::move(path));
		}
	}
	return paths;
}

} // namespace gdp
