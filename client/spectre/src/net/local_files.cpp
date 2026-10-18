// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "net/local_files.hpp"

#include "gdp/clock.hpp"
#include "gdp/file_transfer.hpp"
#include "gdp/framing.hpp"
#include "log.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace spectre {

namespace {

constexpr size_t kChunk = 256 * 1024;
// File bytes sent but not yet acknowledged, per answer; as for uploads.
constexpr uint64_t kInFlight = 4ull << 20;

std::filesystem::path fs_path(const std::string &utf8) {
	return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8_path(const std::filesystem::path &path) {
	std::u8string s = path.u8string();
	return std::string(s.begin(), s.end());
}

} // namespace

struct LocalFiles::Job {
	uint64_t id = 0;
	std::shared_ptr<gdp::Stream> stream;
	std::shared_ptr<std::ifstream> in; // the worker's alone to read
	uint64_t start = 0;  // the file offset the range begins at
	uint64_t size = 0;   // bytes of the range
	uint64_t header = 0; // the StreamOpen and FileReply frames ahead of them
	uint64_t sent = 0;
	uint64_t acked = 0;
	bool reading = false; // a piece is being read
};

// What answering a fetch needs from the disk, found on the worker.
struct LocalFiles::Found {
	gdp::session::FileErrorCode refused = gdp::session::FILE_ERROR_UNSPECIFIED; // else refuse with it
	std::vector<gdp::LocalEntry> entries; // a listing
	// A file, open.
	std::shared_ptr<std::ifstream> in;
	std::string name;
	uint64_t file_size = 0;
	int64_t modified_unix_ms = 0;
};

// What the worker is asked to look at, all by value: a shared folder's
// path (`share`), or a list entry's local path.
struct LocalFiles::Look {
	bool share = false;
	std::string root, relative; // share
	bool include_hidden = false;
	std::string path, name;     // a list entry
	bool top_level = true;
	bool listing = false;
};

LocalFiles::LocalFiles(OpenStream open_stream, std::function<uint64_t()> now_us)
	: open_stream_(std::move(open_stream)),
	  now_us_(now_us ? std::move(now_us) : std::function<uint64_t()>(gdp::monotonic_us)) {}

LocalFiles::~LocalFiles() {
	cancel_all();
}

void LocalFiles::set_list(const std::vector<std::string> &paths, ListReady ready) {
	uint64_t id = ++requested_list_;
	has_list_ = !paths.empty();
	worker_.run<gdp::LocalFileList>([id, paths] { return gdp::LocalFileList::build(id, paths, true); },
		[this, id, ready = std::move(ready)](gdp::LocalFileList files) {
			if (id != requested_list_) {
				return; // the user copied something else while this was read
			}
			uint64_t now = now_us_();
			auto latest = lists_.find(list_id_);
			if (latest != lists_.end()) {
				latest->second.replaced_us = now;
			}
			list_id_ = id;
			List &list = lists_[id];
			list.files = std::move(files);
			list.last_used_us = now;
			expire_lists(now);
			ready(list.files.listing());
		});
}

void LocalFiles::expire_lists(uint64_t now_us) {
	std::vector<std::pair<uint64_t, uint64_t>> earlier; // (last active, id)
	std::vector<uint64_t> gone;
	for (const auto &[id, list] : lists_) {
		if (list.replaced_us == 0) {
			continue;
		}
		uint64_t active = std::max(list.replaced_us, list.last_used_us);
		if (now_us - active >= kKeptListUs) {
			gone.push_back(id);
		} else {
			earlier.emplace_back(active, id);
		}
	}
	if (earlier.size() > kKeptLists) {
		std::sort(earlier.begin(), earlier.end());
		for (size_t i = 0; i < earlier.size() - kKeptLists; i++) {
			gone.push_back(earlier[i].second);
		}
	}
	for (uint64_t id : gone) {
		lists_.erase(id);
	}
}

LocalFiles::List *LocalFiles::use_list(uint64_t list_id) {
	uint64_t now = now_us_();
	expire_lists(now);
	auto it = lists_.find(list_id);
	if (it == lists_.end()) {
		return nullptr;
	}
	it->second.last_used_us = now;
	return &it->second;
}

uint64_t LocalFiles::add_share(const std::string &path, bool include_hidden, std::string *name) {
	std::error_code ec;
	std::filesystem::path p = fs_path(path);
	if (!std::filesystem::is_directory(p, ec)) {
		return 0;
	}
	std::filesystem::path canonical = std::filesystem::canonical(p, ec);
	if (!ec) {
		p = canonical;
	}
	if (name->empty()) {
		*name = utf8_path(p.filename());
	}
	if (!gdp::valid_file_name(*name)) {
		*name = "Shared folder"; // "/" or a name the host would refuse
	}
	uint64_t id = next_share_++;
	shares_[id] = Share{utf8_path(p), include_hidden};
	return id;
}

void LocalFiles::answer(const gdp::session::FileFetch &fetch) {
	std::unique_ptr<gdp::Stream> stream = open_stream_();
	if (!stream) {
		return; // the host gives up on it after its deadline
	}
	gdp::session::FileOpen open;
	open.mutable_answer()->set_fetch_id(fetch.fetch_id());
	gdp::session::StreamOpen stream_open;
	stream_open.set_kind(gdp::kStreamKindFile);
	stream_open.set_params(open.SerializeAsString());
	stream->send_message(stream_open);

	// What only this thread may look at (the lists, the shares) is looked
	// at here; the disk, on the worker.
	Look l;
	l.listing = fetch.listing();
	if (fetch.share_id() != 0) {
		auto share = shares_.find(fetch.share_id());
		if (share == shares_.end()) {
			refuse(*stream, gdp::session::FILE_ERROR_GONE);
			return;
		}
		l.share = true;
		l.root = share->second.folder;
		l.relative = fetch.path();
		l.include_hidden = share->second.include_hidden;
	} else {
		List *list = use_list(fetch.list_id());
		uint32_t index = fetch.index();
		if (!list || index >= list->files.size() || list->files.is_directory(index) != fetch.listing()) {
			refuse(*stream, gdp::session::FILE_ERROR_GONE); // gone, or a folder's bytes, or a file's contents
			return;
		}
		if (fetch.listing() && list->files.listed(index)) {
			std::vector<gdp::session::FileListing> frames;
			if (!list->files.list_folder(index, &frames)) {
				refuse(*stream, gdp::session::FILE_ERROR_GONE);
				return;
			}
			send_listing(*stream, frames);
			return;
		}
		l.path = list->files.path(index);
		l.name = list->files.listing().files(static_cast<int>(index)).name();
		l.top_level = list->files.is_top_level(index);
	}
	std::shared_ptr<gdp::Stream> held(std::move(stream));
	worker_.run<Found>([l] { return look(l); },
		[this, held, fetch](Found f) { found(held, fetch, std::move(f)); });
}

void LocalFiles::found(std::shared_ptr<gdp::Stream> stream, const gdp::session::FileFetch &fetch, Found f) {
	if (f.refused != gdp::session::FILE_ERROR_UNSPECIFIED) {
		refuse(*stream, f.refused);
		return;
	}
	if (!fetch.listing()) {
		send_file(std::move(stream), fetch, std::move(f));
		return;
	}
	std::vector<gdp::session::FileListing> frames;
	if (fetch.share_id() != 0) {
		// A shared folder's entries have no index; frames within the budget.
		gdp::session::FileListing frame;
		size_t used = 0;
		for (const gdp::LocalEntry &e : f.entries) {
			gdp::session::FileListEntry le = gdp::to_list_entry(e);
			size_t more = le.ByteSizeLong() + 4;
			if (frame.files_size() > 0 && used + more > gdp::kFileListBudgetBytes) {
				frames.push_back(std::move(frame));
				frame = gdp::session::FileListing();
				used = 0;
			}
			*frame.add_files() = std::move(le);
			used += more;
		}
		frames.push_back(std::move(frame));
	} else {
		// A copied folder, read just now: appended to its list (unless it was
		// meanwhile), then answered from it.
		List *list = use_list(fetch.list_id());
		if (!list || !list->files.add_folder(fetch.index(), f.entries) ||
			!list->files.list_folder(fetch.index(), &frames)) {
			refuse(*stream, gdp::session::FILE_ERROR_GONE);
			return;
		}
	}
	send_listing(*stream, frames);
}

void LocalFiles::send_reply(gdp::Stream &stream, const gdp::session::FileReply &reply) {
	stream.send_message(reply);
}

void LocalFiles::refuse(gdp::Stream &stream, gdp::session::FileErrorCode code) {
	gdp::session::FileReply reply;
	reply.mutable_refused()->set_code(code);
	send_reply(stream, reply);
	stream.send(nullptr, 0, true);
	// Dropped by the caller: our side ended with the FIN, so it still goes.
}

void LocalFiles::send_listing(gdp::Stream &stream, const std::vector<gdp::session::FileListing> &frames) {
	for (const auto &frame : frames) {
		gdp::session::FileReply reply;
		*reply.mutable_listing() = frame;
		send_reply(stream, reply);
	}
	stream.send(nullptr, 0, true);
}

void LocalFiles::send_file(std::shared_ptr<gdp::Stream> stream, const gdp::session::FileFetch &fetch, Found f) {
	auto job = std::make_unique<Job>();
	job->in = std::move(f.in);
	job->start = std::min<uint64_t>(fetch.offset(), f.file_size);
	uint64_t left = f.file_size - job->start;
	job->size = fetch.length() == 0 ? left : std::min<uint64_t>(fetch.length(), left);

	gdp::session::FileReply reply;
	auto *sending = reply.mutable_sending();
	sending->set_name(f.name);
	sending->set_size(job->size);
	sending->set_modified_unix_ms(f.modified_unix_ms);
	sending->set_offset(job->start);
	sending->set_file_size(f.file_size);
	// What precedes the file's bytes on the stream: the opening frame
	// answer() sent, and this reply.
	gdp::session::FileOpen open;
	open.mutable_answer()->set_fetch_id(fetch.fetch_id());
	gdp::session::StreamOpen stream_open;
	stream_open.set_kind(gdp::kStreamKindFile);
	stream_open.set_params(open.SerializeAsString());
	std::vector<uint8_t> f1, f2;
	gdp::encode_frame(stream_open, &f1);
	gdp::encode_frame(reply, &f2);
	job->header = f1.size() + f2.size();
	stream->send(f2.data(), f2.size(), job->size == 0);
	if (job->size == 0) {
		return; // its FIN went with the reply
	}

	job->id = next_job_++;
	job->stream = std::move(stream);
	Job *j = job.get();
	j->stream->on_data = [](const uint8_t *, size_t) {}; // the host sends nothing on it
	j->stream->on_reset = [this, j](uint64_t) { remove(j); };
	j->stream->on_acked = [this, j](uint64_t total) {
		j->acked = total > j->header ? total - j->header : 0;
		pump(j);
	};
	jobs_[j->id] = std::move(job);
	pump(j);
}

void LocalFiles::pump(Job *job) {
	if (job->reading || job->sent >= job->size || job->sent - std::min(job->acked, job->sent) >= kInFlight) {
		return;
	}
	size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, job->size - job->sent));
	job->reading = true;
	worker_.run<std::vector<uint8_t>>(
		[in = job->in, offset = job->start + job->sent, n] {
			std::vector<uint8_t> buf(n);
			in->seekg(static_cast<std::streamoff>(offset));
			in->read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(n));
			buf.resize(in->gcount() > 0 ? static_cast<size_t>(in->gcount()) : 0);
			return buf;
		},
		[this, id = job->id, n](std::vector<uint8_t> buf) {
			auto it = jobs_.find(id);
			if (it == jobs_.end()) {
				return; // cancelled meanwhile
			}
			Job *job = it->second.get();
			job->reading = false;
			if (buf.size() != n) {
				// The file shrank: reset rather than send it short (§7.12).
				SLOG_INFO("files: a file the host is reading changed; resetting it");
				job->stream->reset(0);
				remove(job);
				return;
			}
			job->sent += n;
			bool last = job->sent == job->size;
			job->stream->send(buf.data(), n, last);
			if (last) {
				remove(job); // gracefully: our FIN is in
				return;
			}
			pump(job);
		});
}

void LocalFiles::remove(Job *job) {
	jobs_.erase(job->id);
}

void LocalFiles::cancel_all() {
	for (auto &[id, job] : jobs_) {
		job->stream->reset(0);
	}
	jobs_.clear();
}

LocalFiles::Found LocalFiles::look(const Look &l) {
	Found f;
	std::string path = l.path;
	f.name = l.name;
	if (l.share) {
		path = gdp::resolve_beneath(l.root, l.relative, l.include_hidden);
		if (l.relative.empty()) {
			// The shared folder itself, which resolve_beneath() found to be a
			// folder; not stat_local(), as a drive (C:\) or "/" has no name.
			if (path.empty() || !l.listing) {
				f.refused = gdp::session::FILE_ERROR_GONE;
				return f;
			}
		} else {
			gdp::LocalEntry entry;
			if (path.empty() || !gdp::stat_local(path, &entry) || entry.directory != l.listing) {
				f.refused = gdp::session::FILE_ERROR_GONE; // gone, or a folder's bytes, or a file's contents
				return f;
			}
			f.name = entry.name;
		}
	} else if (!l.top_level) {
		// Inside a copied folder, a file or folder that has become a link
		// since it was listed is not followed.
		std::error_code ec;
		if (std::filesystem::is_symlink(std::filesystem::symlink_status(fs_path(path), ec))) {
			f.refused = gdp::session::FILE_ERROR_READ_FAILED;
			return f;
		}
	}
	if (l.listing) {
		f.entries = gdp::read_local_folder(path, !l.share || l.include_hidden);
		return f;
	}
	std::error_code ec;
	f.file_size = std::filesystem::file_size(fs_path(path), ec);
	if (!ec) {
		f.in = std::make_shared<std::ifstream>(fs_path(path), std::ios::binary);
	}
	if (ec || !*f.in) {
		f.in.reset();
		f.refused = gdp::session::FILE_ERROR_READ_FAILED;
		return f;
	}
	gdp::LocalEntry entry;
	if (gdp::stat_local(path, &entry, true)) {
		f.modified_unix_ms = entry.modified_unix_ms;
	}
	return f;
}

} // namespace spectre
