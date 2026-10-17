// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/file_receiver.hpp"

#include "gdp/error_codes.hpp"
#include "screencast/sdbus_util.hpp"
#include "util/config.hpp"
#include "util/log.hpp"

#include <systemd/sd-bus.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include <fcntl.h>
#include <pwd.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace wraith {

namespace {

// Bytes gathered before a write: few enough syscalls, little enough held.
// Well under libgdp's 1 MiB initial stream window, so the client never
// waits on a window this side is still holding for a write.
constexpr size_t kWriteChunk = 256 * 1024;
// Free space left over after the file, checked before accepting one: a
// download directory filled to the last byte breaks the user's session.
constexpr uint64_t kSpaceMargin = 64ull << 20;
// A batch's one notification waits this long after its last file.
constexpr int kNotifyQuietMs = 1000;
// "name (n).ext" candidates tried before giving up.
constexpr unsigned kMaxNumberedNames = 10000;

std::string home_directory() {
	const char *home = getenv("HOME");
	if (home && *home) {
		return home;
	}
	const struct passwd *pw = getpwuid(getuid());
	return pw && pw->pw_dir ? pw->pw_dir : "";
}

// XDG_DOWNLOAD_DIR from user-dirs.dirs: "$HOME/..." or an absolute path.
// Empty if unset, or set to $HOME itself (how user-dirs turns one off).
std::string xdg_download_directory(const std::string &home) {
	const char *config_home = getenv("XDG_CONFIG_HOME");
	std::string path = (config_home && *config_home ? std::string(config_home) : home + "/.config") +
		"/user-dirs.dirs";
	std::ifstream in(path);
	std::string line;
	const std::string key = "XDG_DOWNLOAD_DIR=\"";
	while (std::getline(in, line)) {
		if (line.compare(0, key.size(), key) != 0) {
			continue;
		}
		size_t end = line.rfind('"');
		if (end == std::string::npos || end < key.size()) {
			return "";
		}
		std::string value = line.substr(key.size(), end - key.size());
		if (value.compare(0, 5, "$HOME") == 0) {
			value = home + value.substr(5);
		}
		while (value.size() > 1 && value.back() == '/') {
			value.pop_back();
		}
		if (value.empty() || value[0] != '/' || value == home) {
			return "";
		}
		return value;
	}
	return "";
}

bool usable_directory(const std::string &path) {
	struct stat st;
	return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
		access(path.c_str(), W_OK | X_OK) == 0;
}

// rename() that never replaces `to`: 0, or the errno. RENAME_NOREPLACE
// where the file system has it; else a hard link, which fails on an
// existing name just the same; else (FAT and friends have no links) a
// check and a plain rename, the best those offer.
int rename_no_replace(const std::string &from, const std::string &to) {
	if (renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) {
		return 0;
	}
	if (errno != EINVAL && errno != ENOSYS) {
		return errno;
	}
	if (link(from.c_str(), to.c_str()) == 0) {
		unlink(from.c_str());
		return 0;
	}
	if (errno != EPERM && errno != EOPNOTSUPP) {
		return errno;
	}
	if (access(to.c_str(), F_OK) == 0) {
		return EEXIST;
	}
	return rename(from.c_str(), to.c_str()) == 0 ? 0 : errno;
}

// org.freedesktop.Notifications.Notify, sent without waiting for the reply:
// a notification server that is slow to answer must not hold up the
// session.
void desktop_notification(const std::string &summary, const std::string &body) {
	sd_bus *bus = nullptr;
	sd_bus_message *m = nullptr;
	int r = sd_bus_open_user(&bus);
	r = r >= 0 ? sd_bus_message_new_method_call(bus, &m, "org.freedesktop.Notifications",
					 "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify")
			   : r;
	r = r >= 0 ? sd_bus_message_append(m, "susss", "Ghost", 0u, "folder-download", summary.c_str(), body.c_str())
			   : r;
	r = r >= 0 ? sd_bus_message_append(m, "as", 0) : r;
	r = r >= 0 ? sdbus_append_no_options(m) : r;
	r = r >= 0 ? sd_bus_message_append(m, "i", -1) : r;
	r = r >= 0 ? sd_bus_message_set_expect_reply(m, 0) : r;
	r = r >= 0 ? sd_bus_send(bus, m, nullptr) : r;
	r = r >= 0 ? sd_bus_flush(bus) : r;
	if (r < 0) {
		WLOG_INFO("file: no desktop notification (\"%s\"): %s", summary.c_str(), strerror(-r));
	}
	sd_bus_message_unref(m);
	sd_bus_flush_close_unref(bus);
}

const char *file_error_name(gdp::session::FileErrorCode code) {
	switch (code) {
	case gdp::session::FILE_ERROR_UNSUPPORTED: return "unsupported";
	case gdp::session::FILE_ERROR_BAD_NAME: return "bad name";
	case gdp::session::FILE_ERROR_NO_SPACE: return "no space";
	case gdp::session::FILE_ERROR_REFUSED: return "refused";
	case gdp::session::FILE_ERROR_WRITE_FAILED: return "write failed";
	case gdp::session::FILE_ERROR_SIZE_MISMATCH: return "size mismatch";
	default: return "unspecified";
	}
}

} // namespace

std::string file_receive_directory() {
	if (!config().files.receive) {
		return "";
	}
	std::string home = home_directory();
	std::string dir = config().files.directory;
	if (!dir.empty()) {
		if (dir == "~") {
			dir = home;
		} else if (dir.compare(0, 2, "~/") == 0) {
			dir = home + dir.substr(1);
		}
		return usable_directory(dir) ? dir : "";
	}
	dir = xdg_download_directory(home);
	if (usable_directory(dir)) {
		return dir;
	}
	dir = home.empty() ? "" : home + "/Downloads";
	return usable_directory(dir) ? dir : "";
}

FileReceiver::PartFile::~PartFile() {
	if (fd >= 0) {
		close(fd);
	}
	if (!committed && !path.empty()) {
		unlink(path.c_str());
	}
}

FileReceiver::FileReceiver(struct wl_event_loop *loop, std::string directory,
	std::function<void(uint64_t)> on_protocol_error, Notifier notifier)
	: loop_(loop), directory_(std::move(directory)), on_protocol_error_(std::move(on_protocol_error)),
	  notifier_(notifier ? std::move(notifier) : Notifier(desktop_notification)) {
	// Received files get the permissions a newly created file would.
	mode_t mask = umask(0);
	umask(mask);
	file_mode_ = 0666 & ~mask;

	done_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	done_source_.reset(wl_event_loop_add_fd(
		loop_, done_fd_, WL_EVENT_READABLE,
		[](int, uint32_t, void *data) {
			static_cast<FileReceiver *>(data)->drain_completions();
			return 0;
		},
		this));
	writer_ = std::thread([this] { writer_main(); });
}

FileReceiver::~FileReceiver() {
	if (!transfers_.empty()) {
		WLOG_INFO("file: %zu transfer(s) cancelled: the client went away", transfers_.size());
	}
	// Resets every stream still open and lets go of its file; a file the
	// writer still has a job for is deleted when that job is done with it.
	transfers_.clear();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
		jobs_.clear();
	}
	wake_.notify_one();
	writer_.join();
	done_source_.reset();
	if (done_fd_ >= 0) {
		close(done_fd_);
	}
	notify_timer_.reset();
	// Whatever arrived just before the client left is still news.
	if (!unannounced_.empty()) {
		notify();
	}
}

void FileReceiver::add_stream(std::unique_ptr<gdp::Stream> stream) {
	auto owned = std::make_unique<Transfer>();
	Transfer *t = owned.get();
	t->owner = this;
	t->id = next_id_++;
	t->stream = std::move(stream);
	t->stream->on_data = [this, t](const uint8_t *data, size_t len) { handle_data(t, data, len); };
	t->stream->on_fin = [this, t] { handle_fin(t); };
	t->stream->on_reset = [this, t](uint64_t code) { handle_reset(t, code); };
	transfers_[t->id] = std::move(owned);
	// Part of the batch still arriving: its notification waits for this one.
	notify_timer_.reset();
}

void FileReceiver::handle_data(Transfer *t, const uint8_t *data, size_t len) {
	if (!t->accepted) {
		// The opening frame: nothing to write, so its window comes straight
		// back.
		t->stream->consume(len);
		switch (t->reader.feed(data, len)) {
		case gdp::FileStreamReader::State::kWaiting: return;
		case gdp::FileStreamReader::State::kError: protocol_error(t, t->reader.error_code()); return;
		case gdp::FileStreamReader::State::kOpened: break;
		}
		if (!t->reader.take_data().empty()) {
			// gdp-spec.md §7.12: not one byte before "accepted".
			protocol_error(t, static_cast<uint64_t>(gdp::ErrorCode::kMalformedFrame));
			return;
		}
		handle_offer(t);
		return;
	}
	if (len > t->size - t->received) {
		fail(t, gdp::session::FILE_ERROR_SIZE_MISMATCH);
		return;
	}
	t->received += len;
	t->pending.insert(t->pending.end(), data, data + len);
	flush(t);
}

void FileReceiver::handle_fin(Transfer *t) {
	if (!t->accepted) {
		// Its direction is over before the offer was even whole.
		protocol_error(t, static_cast<uint64_t>(gdp::ErrorCode::kMalformedFrame));
		return;
	}
	t->fin = true;
	if (t->received != t->size) {
		fail(t, gdp::session::FILE_ERROR_SIZE_MISMATCH);
		return;
	}
	flush(t);
	if (t->writing == 0 && t->pending.empty()) {
		finish(t);
	}
}

void FileReceiver::handle_reset(Transfer *t, uint64_t error_code) {
	if (t->accepted) {
		WLOG_INFO("file: \"%s\" cancelled by the client after %llu of %llu bytes (code %llu)", t->name.c_str(),
			(unsigned long long)t->received, (unsigned long long)t->size, (unsigned long long)error_code);
	}
	remove(t);
}

void FileReceiver::handle_offer(Transfer *t) {
	if (t->reader.kind() != gdp::kStreamKindFile) {
		// gdp-spec.md §2.2: a kind this host doesn't implement.
		WLOG_INFO("file: refusing a stream of kind \"%s\"", t->reader.kind().c_str());
		t->stream->reset(static_cast<uint64_t>(gdp::ErrorCode::kUnsupported));
		remove(t);
		return;
	}
	gdp::session::FileOpen open;
	if (!open.ParseFromString(t->reader.params())) {
		protocol_error(t, static_cast<uint64_t>(gdp::ErrorCode::kMalformedFrame));
		return;
	}
	if (!open.has_offer()) {
		refuse(t, gdp::session::FILE_ERROR_UNSUPPORTED);
		return;
	}
	t->name = open.offer().name();
	t->size = open.offer().size();
	t->modified_unix_ms = open.offer().modified_unix_ms();
	if (!gdp::valid_file_name(t->name)) {
		refuse(t, gdp::session::FILE_ERROR_BAD_NAME);
		return;
	}
	struct statvfs vfs;
	if (statvfs(directory_.c_str(), &vfs) == 0) {
		uint64_t free_bytes = (uint64_t)vfs.f_bavail * vfs.f_frsize;
		if (free_bytes < kSpaceMargin || free_bytes - kSpaceMargin < t->size) {
			refuse(t, gdp::session::FILE_ERROR_NO_SPACE);
			return;
		}
	}
	// Hidden, next to where the file will end up, so the rename into place
	// never crosses a file system.
	std::string templ = directory_ + "/.ghost-receiving-XXXXXX";
	std::vector<char> path(templ.begin(), templ.end());
	path.push_back('\0');
	int fd = mkostemp(path.data(), O_CLOEXEC);
	if (fd < 0) {
		WLOG_ERROR("file: can't create a file in %s: %s", directory_.c_str(), strerror(errno));
		refuse(t, gdp::session::FILE_ERROR_WRITE_FAILED);
		return;
	}
	fchmod(fd, file_mode_);
	t->part = std::make_shared<PartFile>();
	t->part->fd = fd;
	t->part->path = path.data();
	t->accepted = true;

	gdp::session::FileReply reply;
	reply.mutable_accepted();
	t->stream->send_message(reply);
	WLOG_INFO("file: receiving \"%s\" (%llu bytes)", t->name.c_str(), (unsigned long long)t->size);
}

void FileReceiver::flush(Transfer *t) {
	if (t->pending.empty() || (t->pending.size() < kWriteChunk && !t->fin)) {
		return;
	}
	WriteJob job;
	job.transfer = t->id;
	job.part = t->part;
	job.data.swap(t->pending);
	t->writing += job.data.size();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		jobs_.push_back(std::move(job));
	}
	wake_.notify_one();
}

void FileReceiver::finish(Transfer *t) {
	PartFile &part = *t->part;
	if (t->modified_unix_ms > 0) {
		struct timespec times[2];
		times[0].tv_sec = 0;
		times[0].tv_nsec = UTIME_OMIT; // atime: now's as good as any
		times[1].tv_sec = t->modified_unix_ms / 1000;
		times[1].tv_nsec = (t->modified_unix_ms % 1000) * 1000000;
		futimens(part.fd, times);
	}
	int closed = close(part.fd);
	part.fd = -1;
	if (closed != 0) {
		WLOG_ERROR("file: writing \"%s\" failed: %s", t->name.c_str(), strerror(errno));
		fail(t, errno == ENOSPC || errno == EDQUOT ? gdp::session::FILE_ERROR_NO_SPACE
												   : gdp::session::FILE_ERROR_WRITE_FAILED);
		return;
	}
	std::string saved;
	int err = 0;
	for (unsigned n = 0; n < kMaxNumberedNames; n++) {
		std::string candidate = n == 0 ? t->name : gdp::numbered_file_name(t->name, n);
		err = rename_no_replace(part.path, directory_ + "/" + candidate);
		if (err == 0) {
			saved = candidate;
			break;
		}
		if (err != EEXIST) {
			break;
		}
	}
	if (saved.empty()) {
		WLOG_ERROR("file: can't put \"%s\" in %s: %s", t->name.c_str(), directory_.c_str(), strerror(err));
		fail(t, gdp::session::FILE_ERROR_WRITE_FAILED);
		return;
	}
	part.committed = true;

	gdp::session::FileReply reply;
	reply.mutable_saved()->set_name(saved);
	t->stream->send_message(reply);
	t->stream->send(nullptr, 0, true);
	if (saved == t->name) {
		WLOG_INFO("file: saved \"%s\" (%llu bytes)", saved.c_str(), (unsigned long long)t->size);
	} else {
		WLOG_INFO("file: saved \"%s\" as \"%s\" (%llu bytes)", t->name.c_str(), saved.c_str(),
			(unsigned long long)t->size);
	}
	files_saved_++;
	unannounced_.push_back(saved);
	remove(t);
	schedule_notification();
}

void FileReceiver::refuse(Transfer *t, gdp::session::FileErrorCode code) {
	WLOG_INFO("file: refusing \"%s\": %s", t->name.c_str(), file_error_name(code));
	gdp::session::FileReply reply;
	reply.mutable_refused()->set_code(code);
	t->stream->send_message(reply);
	t->stream->send(nullptr, 0, true);
	remove(t);
}

void FileReceiver::fail(Transfer *t, gdp::session::FileErrorCode code) {
	WLOG_INFO("file: \"%s\" failed after %llu of %llu bytes: %s", t->name.c_str(),
		(unsigned long long)t->received, (unsigned long long)t->size, file_error_name(code));
	gdp::session::FileReply reply;
	reply.mutable_failed()->set_code(code);
	t->stream->send_message(reply);
	t->stream->send(nullptr, 0, true);
	remove(t);
}

void FileReceiver::protocol_error(Transfer *t, uint64_t error_code) {
	WLOG_ERROR("file: the client broke the file stream's protocol (closing with %llu)",
		(unsigned long long)error_code);
	auto on_protocol_error = on_protocol_error_;
	remove(t);
	if (on_protocol_error) {
		on_protocol_error(error_code);
	}
}

void FileReceiver::remove(Transfer *t) {
	// Our direction already ended with a FIN: the stream goes gracefully,
	// the reply still arrives and the client is only asked to stop sending.
	// Otherwise it is reset (gdp::Stream's destructor).
	transfers_.erase(t->id);
	schedule_notification();
}

void FileReceiver::writer_main() {
	for (;;) {
		WriteJob job;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
			if (stopping_) {
				return;
			}
			job = std::move(jobs_.front());
			jobs_.pop_front();
		}
		int error = 0;
		size_t done = 0;
		while (done < job.data.size()) {
			ssize_t n = write(job.part->fd, job.data.data() + done, job.data.size() - done);
			if (n < 0) {
				if (errno == EINTR) {
					continue;
				}
				error = errno;
				break;
			}
			done += static_cast<size_t>(n);
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			done_.push_back({job.transfer, job.data.size(), error});
		}
		uint64_t one = 1;
		ssize_t ignored = write(done_fd_, &one, sizeof(one));
		(void)ignored;
		// `job` goes here, outside the lock: if its transfer was cancelled
		// meanwhile, this was the last hold on the file, which is deleted.
	}
}

void FileReceiver::drain_completions() {
	uint64_t count;
	ssize_t ignored = read(done_fd_, &count, sizeof(count));
	(void)ignored;
	std::vector<WriteDone> done;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		done.swap(done_);
	}
	for (const WriteDone &d : done) {
		auto it = transfers_.find(d.transfer);
		if (it == transfers_.end()) {
			continue; // cancelled or failed meanwhile
		}
		Transfer *t = it->second.get();
		t->writing -= d.len;
		if (d.error) {
			WLOG_ERROR("file: writing \"%s\" failed: %s", t->name.c_str(), strerror(d.error));
			fail(t, d.error == ENOSPC || d.error == EDQUOT ? gdp::session::FILE_ERROR_NO_SPACE
														   : gdp::session::FILE_ERROR_WRITE_FAILED);
			continue;
		}
		// On disk: the client may send that much more.
		t->stream->consume(d.len);
		if (t->fin && t->writing == 0 && t->pending.empty()) {
			finish(t);
		}
	}
}

void FileReceiver::schedule_notification() {
	if (!transfers_.empty() || unannounced_.empty() || notify_timer_) {
		return;
	}
	notify_timer_.reset(wl_event_loop_add_timer(
		loop_,
		[](void *data) {
			auto *self = static_cast<FileReceiver *>(data);
			self->notify_timer_.reset();
			self->notify();
			return 0;
		},
		this));
	wl_event_source_timer_update(notify_timer_.get(), kNotifyQuietMs);
}

void FileReceiver::notify() {
	std::vector<std::string> names;
	names.swap(unannounced_);
	std::string summary =
		names.size() == 1 ? "Received " + names[0] : "Received " + std::to_string(names.size()) + " files";
	std::string where = directory_;
	std::string home = home_directory();
	if (!home.empty() && where.compare(0, home.size(), home) == 0 &&
		(where.size() == home.size() || where[home.size()] == '/')) {
		where = "~" + where.substr(home.size());
	}
	notifier_(summary, "Saved in " + where);
}

} // namespace wraith
