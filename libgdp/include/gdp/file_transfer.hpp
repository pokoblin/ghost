// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// File transfer rules (gdp-spec.md §7.12), shared by both ends so they
// can't disagree about which names cross the wire: the capability and
// stream kind, the name rules, the "name (1).ext" a receiver falls back
// to, the readers that split a "file" stream into its one frame and the
// raw bytes after it, and how a desktop's clipboard says which files were
// copied.
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
// up on the stream (gdp-spec.md §7.12). A receiver answers at once. The
// same for a client waiting for the host's answer to a request.
inline constexpr uint32_t kFileReplyTimeoutMs = 30000;

// The most entries a FileList carries; a host lists the first this many.
inline constexpr size_t kMaxFileListEntries = 1000;

// The clipboard formats that say "these files were copied", best first:
// GNOME's (whose first line says copy or cut) and the standard URI list.
// Empty if `offered` has neither.
std::string select_copied_files_mime(const std::vector<std::string> &offered);

// The local paths in a copied-files clipboard payload of `mime` (one of the
// above): file:// URIs with no host or "localhost", percent-decoded, in
// order. Anything else (another scheme, a remote host, a NUL) is skipped.
std::vector<std::string> copied_file_paths(const std::string &mime, const std::string &data);

// gdp-spec.md §7.12: non-empty, at most kMaxFileNameBytes, valid UTF-8, no
// '/', '\\' or control character (U+0000-U+001F, U+007F), and not "." or
// "..". Says nothing about NFC, which the sender is responsible for.
bool valid_file_name(const std::string &name);

// The name a receiver tries when `name` is taken: "report (2).pdf" for
// n = 2. The number goes before the last extension; a name whose only dot
// leads it (".bashrc") or ends it has none. Shortened, a whole character
// at a time, to stay within kMaxFileNameBytes. `name` must be valid.
std::string numbered_file_name(const std::string &name, unsigned n);

// One §3.1 frame, then raw bytes: what each end of a "file" stream reads
// first. Feed it everything on_data delivers; FrameReader would take the
// raw bytes after the frame into its own buffer.
class FrameThenRawReader {
public:
	enum class State {
		kWaiting, // the frame isn't complete yet
		kFramed,  // frame() is valid; later bytes are raw
		kError,   // the frame was longer than the limit: error_code()
	};

	explicit FrameThenRawReader(size_t max_frame_size) : max_frame_size_(max_frame_size) {}

	State feed(const uint8_t *data, size_t len);
	State state() const { return state_; }
	// The frame's protobuf bytes, from kFramed on.
	const std::string &frame() const { return frame_; }
	// Raw bytes received since the last call, in order; once kFramed, those
	// that came in the same feed() as the end of the frame are here.
	std::vector<uint8_t> take_data();
	// gdp-spec.md §12's FRAME_TOO_LARGE for kError.
	uint64_t error_code() const { return error_code_; }

private:
	size_t max_frame_size_;
	State state_ = State::kWaiting;
	std::vector<uint8_t> buf_;
	std::string frame_;
	uint64_t error_code_ = 0;
};

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
	FrameThenRawReader reader_{kMaxFileOpenFrameSize};
	State state_ = State::kWaiting;
	std::string kind_;
	std::string params_;
	uint64_t error_code_ = 0;
};

} // namespace gdp
