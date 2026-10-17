// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The host's end of file transfer (gdp-spec.md §7.12): every "file" stream
// the viewer opens, from its offer to the file in the download directory.
//
// A file is written to a hidden temporary file next to where it will end
// up, by a thread of its own so a slow disk never stalls the event loop
// that also encodes the desktop, and is renamed into place -- never over
// an existing file -- once every byte is down. The stream's receive window
// is handed back only as bytes reach the disk (gdp::Stream::consume()), so
// a disk slower than the network holds the client back instead of filling
// memory.
#pragma once

#include "gdp/file_transfer.hpp"
#include "gdp/transport.hpp"
#include "session.pb.h"
#include "util/event_source.hpp"

#include <wayland-server-core.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

namespace wraith {

// Where received files go: wraith.toml's [files] directory if set, else
// the user's download directory (XDG_DOWNLOAD_DIR from user-dirs.dirs),
// else ~/Downloads. Empty when that isn't an existing directory the user
// can write to, or [files] receive is off: then the host doesn't offer
// the "file" capability.
std::string file_receive_directory();

class FileReceiver {
public:
	// Shows one batch's notification; the default sends it to the desktop
	// (org.freedesktop.Notifications).
	using Notifier = std::function<void(const std::string &summary, const std::string &body)>;

	// `on_protocol_error` gets the gdp-spec.md §12 code to close the
	// connection with when the client breaks §7.12 (a malformed or
	// oversized opening frame, bytes before "accepted"). Called from inside
	// a stream callback: the caller must close later, not from inside it.
	FileReceiver(struct wl_event_loop *loop, std::string directory,
		std::function<void(uint64_t error_code)> on_protocol_error, Notifier notifier = nullptr);
	// Cancels whatever is still arriving; its partial files are deleted.
	~FileReceiver();

	FileReceiver(const FileReceiver &) = delete;
	FileReceiver &operator=(const FileReceiver &) = delete;

	// An additional stream the viewer opened (gdp::Connection::on_stream).
	void add_stream(std::unique_ptr<gdp::Stream> stream);

	// A stream whose FileOpen is a request for a copied file (§7.12) goes
	// here, its callbacks still this receiver's until it sets its own.
	// Unset, requests are refused as unsupported.
	std::function<void(std::unique_ptr<gdp::Stream>, const gdp::session::FileRequest &)> on_request;

	// Files stored, transfers still running: for logs and tests.
	size_t files_saved() const { return files_saved_; }
	size_t active_transfers() const { return transfers_.size(); }

private:
	// A temporary file being written. Shared with the writer thread's
	// queued jobs, so a cancelled transfer's file is closed and deleted
	// only once no write to it is still pending -- whichever side lets go
	// last does it.
	struct PartFile {
		int fd = -1;
		std::string path;
		bool committed = false; // renamed into place: keep it
		~PartFile();
	};

	struct Transfer {
		FileReceiver *owner = nullptr;
		uint64_t id = 0;
		std::unique_ptr<gdp::Stream> stream;
		gdp::FileStreamReader reader;
		bool accepted = false;
		bool fin = false;
		std::string name;
		uint64_t size = 0;
		int64_t modified_unix_ms = 0;
		std::shared_ptr<PartFile> part;
		uint64_t received = 0;        // file bytes off the wire
		std::vector<uint8_t> pending; // received, not yet handed to the writer
		uint64_t writing = 0;         // handed to the writer, not yet written
	};

	struct WriteJob {
		uint64_t transfer = 0;
		std::shared_ptr<PartFile> part;
		std::vector<uint8_t> data;
	};
	struct WriteDone {
		uint64_t transfer = 0;
		size_t len = 0;
		int error = 0; // errno, 0 on success
	};

	void handle_data(Transfer *t, const uint8_t *data, size_t len);
	void handle_fin(Transfer *t);
	void handle_reset(Transfer *t, uint64_t error_code);
	// The StreamOpen is in: refuse, or accept and start the file.
	void handle_offer(Transfer *t);
	// Hands what has piled up to the writer: once there is enough for a
	// write worth making, or everything once the FIN is in.
	void flush(Transfer *t);
	// Every byte is on disk: rename into place, answer "saved".
	void finish(Transfer *t);
	void refuse(Transfer *t, gdp::session::FileErrorCode code);
	void fail(Transfer *t, gdp::session::FileErrorCode code);
	void protocol_error(Transfer *t, uint64_t error_code);
	// Drops the transfer: its stream is let go (gracefully once our side
	// ended with a FIN, else reset) and its partial file deleted.
	void remove(Transfer *t);

	void writer_main();
	void drain_completions();
	void schedule_notification();
	void notify();

	struct wl_event_loop *loop_;
	std::string directory_;
	// directory_ as FileSaved tells the client: absolute, no trailing '/'.
	std::string shown_directory_;
	std::function<void(uint64_t)> on_protocol_error_;
	Notifier notifier_;
	mode_t file_mode_ = 0644;

	std::map<uint64_t, std::unique_ptr<Transfer>> transfers_;
	uint64_t next_id_ = 1;
	size_t files_saved_ = 0;

	// The writer thread: jobs in, completions out through an eventfd.
	std::thread writer_;
	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<WriteJob> jobs_;
	std::vector<WriteDone> done_;
	bool stopping_ = false;
	int done_fd_ = -1;
	EventSource done_source_;

	// The desktop notification for a batch: names saved since the last
	// one, shown once nothing has arrived for a moment.
	std::vector<std::string> unannounced_;
	EventSource notify_timer_;
};

} // namespace wraith
