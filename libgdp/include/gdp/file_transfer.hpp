// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// File transfer rules (gdp-spec.md §7.12), shared by both ends so they
// can't disagree about which names cross the wire: the capability and
// stream kind, the name rules, the "name (1).ext" a receiver falls back
// to, and the reader that splits a "file" stream into its StreamOpen frame
// and the raw bytes after it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gdp {

// SessionHello/SessionAccept.capabilities name, and the StreamOpen.kind of
// the streams it governs (gdp-spec.md §2.2, §6.7).
inline constexpr const char *kCapabilityFile = "file";
inline constexpr const char *kStreamKindFile = "file";

// How many "file" streams a host lets the client keep open at once
// (Connection::allow_streams()). A client with more files queues them.
inline constexpr uint32_t kFileStreamsAtOnce = 4;

// A FileOffer.name's limit in UTF-8 bytes: NAME_MAX on the file systems
// hosts use.
inline constexpr size_t kMaxFileNameBytes = 255;

// The StreamOpen frame of a "file" stream is a name and two numbers: far
// under this, which is what a receiver holds an opening frame to.
inline constexpr size_t kMaxFileOpenFrameSize = 4096;

// How long a sender waits for the FileReply to its offer before giving
// up on the stream (gdp-spec.md §7.12). A receiver answers at once.
inline constexpr uint32_t kFileReplyTimeoutMs = 30000;

// gdp-spec.md §7.12: non-empty, at most kMaxFileNameBytes, valid UTF-8, no
// '/', '\\' or control character (U+0000-U+001F, U+007F), and not "." or
// "..". Says nothing about NFC, which the sender is responsible for.
bool valid_file_name(const std::string &name);

// The name a receiver tries when `name` is taken: "report (2).pdf" for
// n = 2. The number goes before the last extension; a name whose only dot
// leads it (".bashrc") or ends it has none. Shortened, a whole character
// at a time, to stay within kMaxFileNameBytes. `name` must be valid.
std::string numbered_file_name(const std::string &name, unsigned n);

// The receiving end of a "file" stream: the opening frame (a StreamOpen,
// held to kMaxFileOpenFrameSize), then raw bytes. Feed it everything
// on_data delivers.
class FileStreamReader {
public:
	enum class State {
		kWaiting, // the StreamOpen frame isn't complete yet
		kOpened,  // kind() and params() are valid; later bytes are file data
		kError,   // the opening frame was too large or didn't parse: error_code()
	};

	State feed(const uint8_t *data, size_t len);

	State state() const { return state_; }
	// The StreamOpen's kind and params (gdp-spec.md §2.2), from kOpened on.
	const std::string &kind() const { return kind_; }
	const std::string &params() const { return params_; }
	// File data received since the last call, in order. Once kOpened, the
	// bytes that came in the same feed() as the end of the frame are here.
	std::vector<uint8_t> take_data();
	// gdp-spec.md §12's code for kError (FRAME_TOO_LARGE or MALFORMED_FRAME).
	uint64_t error_code() const { return error_code_; }

private:
	State state_ = State::kWaiting;
	std::vector<uint8_t> buf_;
	std::string kind_;
	std::string params_;
	uint64_t error_code_ = 0;
};

} // namespace gdp
