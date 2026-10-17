// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/file_sender.hpp"

#include "gdp/clock.hpp"
#include "gdp/file_transfer.hpp"
#include "gdp/framing.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wraith {

namespace {

// Read pieces: few syscalls, small against the window.
constexpr size_t kReadChunk = 256 * 1024;
// RTT samples at most this often.
constexpr uint64_t kSampleIntervalUs = 50000;
// How soon to look again while the video's backlog holds file data back.
constexpr int kRetryMs = 10;

int64_t mtime_ms(const struct stat &st) {
	return (int64_t)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000;
}

} // namespace

void FileWindow::update(uint32_t rtt_us, uint64_t now_us) {
	if (rtt_us == 0) {
		return;
	}
	base_rtt_us_ = base_rtt_us_ == 0 ? rtt_us : std::min(base_rtt_us_, rtt_us);
	if (rtt_us > base_rtt_us_ + kTargetDelayUs) {
		// At most once a round trip: one queue, one cut.
		if (decreased_ && now_us - last_decrease_us_ < rtt_us) {
			return;
		}
		bytes_ = std::max(kMinimum, bytes_ / 2);
		last_decrease_us_ = now_us;
		decreased_ = true;
	} else {
		bytes_ = std::min(kMaximum, bytes_ + kStep);
	}
}

FileSender::OpenFile::~OpenFile() {
	if (fd >= 0) {
		::close(fd);
	}
}

FileSender::FileSender(struct wl_event_loop *loop, std::function<uint32_t()> rtt_us,
	std::function<bool()> video_queue_clear)
	: loop_(loop), rtt_us_(std::move(rtt_us)), video_queue_clear_(std::move(video_queue_clear)) {
	done_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	done_source_.reset(wl_event_loop_add_fd(
		loop_, done_fd_, WL_EVENT_READABLE,
		[](int, uint32_t, void *data) {
			static_cast<FileSender *>(data)->drain_reads();
			return 0;
		},
		this));
	reader_ = std::thread([this] { reader_main(); });
}

FileSender::~FileSender() {
	// Whatever is still going is reset (gdp::Stream's destructor).
	transfers_.clear();
	retry_timer_.reset();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
		jobs_.clear();
	}
	wake_.notify_one();
	reader_.join();
	done_source_.reset();
	if (done_fd_ >= 0) {
		close(done_fd_);
	}
}

gdp::session::FileList FileSender::set_list(const std::vector<std::string> &paths) {
	list_id_++;
	paths_.clear();
	list_.Clear();
	list_.set_list_id(list_id_);
	for (const std::string &path : paths) {
		if (paths_.size() == gdp::kMaxFileListEntries) {
			break;
		}
		struct stat st;
		std::string name = path.substr(path.find_last_of('/') + 1);
		if (stat(path.c_str(), &st) != 0 || !gdp::valid_file_name(name)) {
			continue;
		}
		bool directory = S_ISDIR(st.st_mode);
		if (!directory && !S_ISREG(st.st_mode)) {
			continue; // a device, a pipe: nothing to send
		}
		auto *entry = list_.add_files();
		entry->set_name(name);
		entry->set_size(directory ? 0 : (uint64_t)st.st_size);
		entry->set_modified_unix_ms(mtime_ms(st));
		entry->set_directory(directory);
		paths_.push_back(path);
	}
	if (!paths_.empty()) {
		WLOG_INFO("file: the clipboard holds %zu copied file(s) for the client", paths_.size());
	}
	return list_;
}

void FileSender::serve(std::unique_ptr<gdp::Stream> stream, const gdp::session::FileRequest &request) {
	uint32_t index = request.index();
	if (request.list_id() != list_id_ || index >= paths_.size() || list_.files((int)index).directory()) {
		refuse(std::move(stream), gdp::session::FILE_ERROR_GONE);
		return;
	}
	const std::string &path = paths_[index];
	auto file = std::make_shared<OpenFile>();
	file->fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	struct stat st;
	if (file->fd < 0 || fstat(file->fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		bool gone = file->fd < 0 ? (errno == ENOENT || errno == ENOTDIR) : true;
		WLOG_INFO("file: can't send \"%s\": %s", path.c_str(), file->fd < 0 ? strerror(errno) : "not a file");
		refuse(std::move(stream),
			gone ? gdp::session::FILE_ERROR_GONE : gdp::session::FILE_ERROR_READ_FAILED);
		return;
	}

	auto owned = std::make_unique<Transfer>();
	Transfer *t = owned.get();
	t->id = next_transfer_++;
	t->stream = std::move(stream);
	t->file = std::move(file);
	t->name = list_.files((int)index).name();
	t->size = (uint64_t)st.st_size;

	gdp::session::FileReply reply;
	auto *sending = reply.mutable_sending();
	sending->set_name(t->name);
	sending->set_size(t->size);
	sending->set_modified_unix_ms(mtime_ms(st));
	std::vector<uint8_t> frame;
	gdp::encode_frame(reply, &frame);
	t->header = frame.size();
	t->stream->send(frame.data(), frame.size(), t->size == 0);

	// The client sends nothing after its request but its FIN.
	t->stream->on_data = [](const uint8_t *, size_t) {};
	t->stream->on_fin = [] {};
	t->stream->on_reset = [this, t](uint64_t) {
		WLOG_INFO("file: the client stopped \"%s\" after %llu of %llu bytes", t->name.c_str(),
			(unsigned long long)t->acked, (unsigned long long)t->size);
		remove(t);
	};
	t->stream->on_acked = [this, t](uint64_t total) { acknowledged(t, total); };
	WLOG_INFO("file: sending \"%s\" (%llu bytes)", t->name.c_str(), (unsigned long long)t->size);
	if (t->size == 0) {
		return; // its FIN went with the reply; the stream goes gracefully
	}
	transfers_[t->id] = std::move(owned);
	pump();
}

void FileSender::refuse(std::unique_ptr<gdp::Stream> stream, gdp::session::FileErrorCode code) {
	gdp::session::FileReply reply;
	reply.mutable_refused()->set_code(code);
	stream->send_message(reply);
	stream->send(nullptr, 0, true);
	// Dropped here: our side ended with the FIN, so the reply still goes.
}

void FileSender::acknowledged(Transfer *t, uint64_t total) {
	uint64_t file_bytes = total > t->header ? total - t->header : 0;
	t->acked = std::max(t->acked, std::min(file_bytes, t->size));
	uint64_t now = gdp::monotonic_us();
	if (now - last_sample_us_ >= kSampleIntervalUs) {
		last_sample_us_ = now;
		window_.update(rtt_us_(), now);
	}
	pump();
}

uint64_t FileSender::in_flight() const {
	uint64_t total = 0;
	for (const auto &[id, t] : transfers_) {
		total += t->asked - std::min(t->acked, t->asked);
	}
	return total;
}

void FileSender::pump() {
	if (transfers_.empty()) {
		return;
	}
	// The video first: file data queued now would only wait in front of
	// its frames.
	if (!video_queue_clear_()) {
		if (!retry_timer_) {
			retry_timer_.reset(wl_event_loop_add_timer(
				loop_,
				[](void *data) {
					auto *self = static_cast<FileSender *>(data);
					self->retry_timer_.reset();
					self->pump();
					return 0;
				},
				this));
			wl_event_source_timer_update(retry_timer_.get(), kRetryMs);
		}
		return;
	}
	for (auto &[id, t] : transfers_) {
		if (t->reading || t->asked >= t->size) {
			continue;
		}
		uint64_t flying = in_flight();
		if (flying >= window_.bytes()) {
			break;
		}
		size_t len = (size_t)std::min<uint64_t>({kReadChunk, t->size - t->asked, window_.bytes() - flying});
		ReadJob job{t->id, t->file, t->asked, len};
		t->asked += len;
		t->reading = true;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			jobs_.push_back(std::move(job));
		}
		wake_.notify_one();
	}
}

void FileSender::drain_reads() {
	uint64_t count;
	ssize_t ignored = read(done_fd_, &count, sizeof(count));
	(void)ignored;
	std::vector<ReadDone> done;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		done.swap(done_);
	}
	for (ReadDone &d : done) {
		auto it = transfers_.find(d.transfer);
		if (it == transfers_.end()) {
			continue; // cancelled meanwhile
		}
		Transfer *t = it->second.get();
		t->reading = false;
		if (d.short_read) {
			// The file shrank, or a read failed: what was promised in
			// FileSending can't be kept.
			WLOG_ERROR("file: \"%s\" ended early while being sent; resetting it", t->name.c_str());
			t->stream->reset(0);
			remove(t);
			continue;
		}
		t->sent += d.data.size();
		bool last = t->sent == t->size;
		t->stream->send(d.data.data(), d.data.size(), last);
		if (last) {
			WLOG_INFO("file: sent \"%s\" (%llu bytes)", t->name.c_str(), (unsigned long long)t->size);
			remove(t); // gracefully: the FIN is in
		}
	}
	pump();
}

void FileSender::remove(Transfer *t) {
	transfers_.erase(t->id);
}

void FileSender::reader_main() {
	for (;;) {
		ReadJob job;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
			if (stopping_) {
				return;
			}
			job = std::move(jobs_.front());
			jobs_.pop_front();
		}
		ReadDone done{job.transfer, std::vector<uint8_t>(job.len), false};
		size_t got = 0;
		while (got < job.len) {
			ssize_t n = pread(job.file->fd, done.data.data() + got, job.len - got, (off_t)(job.offset + got));
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				break;
			}
			got += (size_t)n;
		}
		done.short_read = got < job.len;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			done_.push_back(std::move(done));
		}
		uint64_t one = 1;
		ssize_t ignored = write(done_fd_, &one, sizeof(one));
		(void)ignored;
	}
}

} // namespace wraith
