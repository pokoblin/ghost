// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// File transfer, the host's end (session/file_receiver.hpp, gdp-spec.md
// §7.12): a real libgdp client sends files over loopback to a
// FileReceiver writing into a private directory, run on a wl_event_loop
// the way wraith runs it. Covers a file arriving whole with its mtime, a
// taken name, refusals, a short stream, cancelling from either end, a
// protocol violation, and that no partial file is ever left behind.
#include "session/file_receiver.hpp"

#include "gdp/error_codes.hpp"
#include "gdp/file_transfer.hpp"
#include "gdp/framing.hpp"
#include "gdp/transport.hpp"
#include "session.pb.h"

#include <wayland-server-core.h>

#include <dirent.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
#define CHECK(expr)                                                                                          \
	do {                                                                                                     \
		if (!(expr)) {                                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                         \
			g_failures++;                                                                                    \
		}                                                                                                    \
	} while (0)

int64_t now_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1'000'000;
}

struct Loop {
	wl_event_loop *loop = wl_event_loop_create();
	gdp::Transport server;
	gdp::Transport client;
	~Loop() { wl_event_loop_destroy(loop); }

	bool until(int timeout_ms, const std::function<bool()> &done) {
		int64_t start = now_ms();
		for (;;) {
			if (done()) {
				return true;
			}
			struct pollfd fds[3] = {{server.notify_fd(), POLLIN, 0}, {client.notify_fd(), POLLIN, 0},
				{wl_event_loop_get_fd(loop), POLLIN, 0}};
			poll(fds, 3, 10);
			server.dispatch();
			client.dispatch();
			wl_event_loop_dispatch(loop, 0);
			wl_event_loop_dispatch_idle(loop);
			if (now_ms() - start > timeout_ms) {
				return false;
			}
		}
	}
	void run(int ms) {
		until(ms, [] { return false; });
	}
};

std::vector<std::string> list_dir(const std::string &dir) {
	std::vector<std::string> names;
	if (DIR *d = opendir(dir.c_str())) {
		while (dirent *e = readdir(d)) {
			std::string n = e->d_name;
			if (n != "." && n != "..") {
				names.push_back(n);
			}
		}
		closedir(d);
	}
	return names;
}

bool no_partial_files(const std::string &dir) {
	for (const std::string &n : list_dir(dir)) {
		if (n.rfind(".ghost-receiving-", 0) == 0) {
			return false;
		}
	}
	return true;
}

std::string read_file(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string pattern(size_t len, unsigned seed) {
	std::string out(len, '\0');
	uint32_t x = seed * 2654435761u + 1;
	for (size_t i = 0; i < len; i++) {
		x = x * 1103515245u + 12345u;
		out[i] = static_cast<char>(x >> 24);
	}
	return out;
}

// One client-side transfer: the offer, then (on "accepted") `body`, then
// FIN, and every FileReply that comes back.
struct Upload {
	std::unique_ptr<gdp::Stream> stream;
	gdp::FrameReader reader;
	std::vector<gdp::session::FileReply> replies;
	bool fin = false;
	bool reset = false;
	std::string body;
	bool send_on_accept = true;

	bool has(gdp::session::FileReply::ReplyCase c) const {
		for (const auto &r : replies) {
			if (r.reply_case() == c) {
				return true;
			}
		}
		return false;
	}
};

std::unique_ptr<Upload> upload(gdp::Connection &conn, const std::string &name, uint64_t size,
	const std::string &body, int64_t mtime_ms = 0) {
	auto u = std::make_unique<Upload>();
	Upload *p = u.get();
	p->body = body;
	p->stream = conn.open_stream();
	p->stream->on_data = [p](const uint8_t *data, size_t len) {
		gdp::session::FileReply reply;
		p->reader.feed_and_drain(data, len, &reply, [&] {
			p->replies.push_back(reply);
			if (reply.has_accepted() && p->send_on_accept) {
				p->stream->send(reinterpret_cast<const uint8_t *>(p->body.data()), p->body.size(), true);
			}
			return true;
		});
	};
	p->stream->on_fin = [p] { p->fin = true; };
	p->stream->on_reset = [p](uint64_t) { p->reset = true; };
	gdp::session::FileOpen open;
	open.mutable_offer()->set_name(name);
	open.mutable_offer()->set_size(size);
	open.mutable_offer()->set_modified_unix_ms(mtime_ms);
	gdp::session::StreamOpen stream_open;
	stream_open.set_kind(gdp::kStreamKindFile);
	stream_open.set_params(open.SerializeAsString());
	p->stream->send_message(stream_open);
	return u;
}

} // namespace

int main() {
	const std::string cert = "/tmp/wraith_file_receiver_cert.pem";
	const std::string key = "/tmp/wraith_file_receiver_key.pem";
	std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + key + " -out " + cert +
		" -days 1 -nodes -subj /CN=localhost >/dev/null 2>&1";
	if (system(cmd.c_str()) != 0) {
		fprintf(stderr, "file_receiver_test: openssl failed\n");
		return 1;
	}
	char dir_template[] = "/tmp/wraith_file_receiver_XXXXXX";
	const std::string dir = mkdtemp(dir_template);

	Loop loop;
	std::vector<std::pair<std::string, std::string>> notifications;
	std::vector<uint64_t> protocol_errors;
	auto receiver = std::make_unique<wraith::FileReceiver>(
		loop.loop, dir, [&](uint64_t code) { protocol_errors.push_back(code); },
		[&](const std::string &summary, const std::string &body) { notifications.push_back({summary, body}); });

	std::unique_ptr<gdp::Connection> server_conn;
	CHECK(loop.server.listen(44351, "gdp-test/1", cert, key, [&](std::unique_ptr<gdp::Connection> conn) {
		server_conn = std::move(conn);
		server_conn->allow_streams(gdp::kFileStreamsAtOnce);
		server_conn->on_stream = [&](std::unique_ptr<gdp::Stream> s) { receiver->add_stream(std::move(s)); };
	}));
	auto client = loop.client.connect("127.0.0.1", 44351, "gdp-test/1");
	bool connected = false;
	client->on_state_changed = [&](gdp::ConnectionState s) {
		connected = connected || s == gdp::ConnectionState::kConnected;
	};
	CHECK(loop.until(5000, [&] { return connected; }));
	client->open_control_stream();
	client->open_input_stream();

	using R = gdp::session::FileReply;

	// A file arrives whole, with its modification time, and one
	// notification follows once the batch is quiet.
	const std::string report = pattern(3 << 20, 1);
	const int64_t mtime_ms = 1700000000123;
	auto a = upload(*client, "report.pdf", report.size(), report, mtime_ms);
	CHECK(loop.until(10000, [&] { return a->fin; }));
	CHECK(a->has(R::kAccepted) && a->has(R::kSaved));
	CHECK(a->replies.back().saved().name() == "report.pdf");
	CHECK(a->replies.back().saved().directory() == dir); // where the client's user will look
	CHECK(read_file(dir + "/report.pdf") == report);
	struct stat st;
	CHECK(stat((dir + "/report.pdf").c_str(), &st) == 0);
	CHECK(st.st_mtim.tv_sec == mtime_ms / 1000 && st.st_mtim.tv_nsec == (mtime_ms % 1000) * 1000000);
	CHECK(notifications.empty()); // not yet: the batch might go on
	CHECK(loop.until(3000, [&] { return notifications.size() == 1; }));
	CHECK(notifications.size() == 1 && notifications[0].first == "Received report.pdf");

	// The same name again, twice at once: neither overwrites anything.
	auto b = upload(*client, "report.pdf", 5, "first");
	auto c = upload(*client, "report.pdf", 6, "second");
	CHECK(loop.until(5000, [&] { return b->fin && c->fin; }));
	CHECK(b->has(R::kSaved) && c->has(R::kSaved));
	std::string b_name = b->replies.back().saved().name();
	std::string c_name = c->replies.back().saved().name();
	CHECK(b_name != c_name && b_name != "report.pdf" && c_name != "report.pdf");
	CHECK(read_file(dir + "/report.pdf") == report);
	CHECK(read_file(dir + "/" + b_name) == "first" && read_file(dir + "/" + c_name) == "second");
	CHECK(loop.until(3000, [&] { return notifications.size() == 2; }));
	CHECK(notifications.size() == 2 && notifications[1].first == "Received 2 files");

	// An empty file is a file too.
	auto empty = upload(*client, "empty.txt", 0, "");
	CHECK(loop.until(5000, [&] { return empty->fin; }));
	CHECK(empty->has(R::kSaved));
	CHECK(stat((dir + "/empty.txt").c_str(), &st) == 0 && st.st_size == 0);
	// Its own batch: announced before the transfers below start one more.
	CHECK(loop.until(3000, [&] { return notifications.size() == 3; }));
	CHECK(notifications.size() == 3 && notifications[2].first == "Received empty.txt");

	// Refused: a name with a path in it, and a FileOpen case this host
	// doesn't know. Nothing is created for either.
	size_t before = list_dir(dir).size();
	auto bad = upload(*client, "../escape.txt", 3, "bad");
	CHECK(loop.until(5000, [&] { return bad->fin; }));
	CHECK(bad->replies.size() == 1 && bad->replies[0].has_refused());
	CHECK(bad->replies[0].refused().code() == gdp::session::FILE_ERROR_BAD_NAME);
	{
		auto u = std::make_unique<Upload>();
		Upload *p = u.get();
		p->stream = client->open_stream();
		p->stream->on_data = [p](const uint8_t *data, size_t len) {
			gdp::session::FileReply reply;
			p->reader.feed_and_drain(data, len, &reply, [&] {
				p->replies.push_back(reply);
				return true;
			});
		};
		p->stream->on_fin = [p] { p->fin = true; };
		gdp::session::StreamOpen open;
		open.set_kind(gdp::kStreamKindFile);
		open.set_params(gdp::session::FileOpen().SerializeAsString());
		p->stream->send_message(open);
		CHECK(loop.until(5000, [&] { return p->fin; }));
		CHECK(p->replies.size() == 1 && p->replies[0].has_refused());
		CHECK(p->replies[0].refused().code() == gdp::session::FILE_ERROR_UNSUPPORTED);
	}
	CHECK(list_dir(dir).size() == before);

	// Fewer bytes than offered: failed, and the partial file is gone.
	auto short_file = upload(*client, "short.bin", 10, "12345");
	CHECK(loop.until(5000, [&] { return short_file->fin; }));
	CHECK(short_file->has(R::kFailed));
	CHECK(short_file->replies.back().failed().code() == gdp::session::FILE_ERROR_SIZE_MISMATCH);

	// More bytes than offered: the same.
	auto long_file = upload(*client, "long.bin", 3, "12345");
	CHECK(loop.until(5000, [&] { return long_file->fin; }));
	CHECK(long_file->has(R::kFailed));
	CHECK(loop.until(2000, [&] { return no_partial_files(dir); }));
	CHECK(stat((dir + "/short.bin").c_str(), &st) != 0 && stat((dir + "/long.bin").c_str(), &st) != 0);

	// The client cancels part way: nothing is left behind.
	{
		std::string big = pattern(8 << 20, 2);
		auto u = upload(*client, "cancelled.bin", big.size(), "");
		u->send_on_accept = false;
		CHECK(loop.until(5000, [&] { return u->has(R::kAccepted); }));
		u->stream->send(reinterpret_cast<const uint8_t *>(big.data()), 1 << 20);
		CHECK(loop.until(5000, [&] { return !no_partial_files(dir); }));
		u->stream->reset(0);
		CHECK(loop.until(5000, [&] { return receiver->active_transfers() == 0 && no_partial_files(dir); }));
		CHECK(stat((dir + "/cancelled.bin").c_str(), &st) != 0);
	}

	// The session ends mid-transfer (the receiver goes with it): the
	// partial file is deleted, the file saved before it is announced.
	{
		auto first = upload(*client, "kept.txt", 4, "kept");
		CHECK(loop.until(5000, [&] { return first->fin; }));
		std::string big = pattern(4 << 20, 3);
		auto u = upload(*client, "unfinished.bin", big.size() + 1, big);
		u->send_on_accept = false;
		CHECK(loop.until(5000, [&] { return u->has(R::kAccepted); }));
		u->stream->send(reinterpret_cast<const uint8_t *>(big.data()), big.size());
		CHECK(loop.until(5000, [&] { return !no_partial_files(dir); }));
		size_t shown = notifications.size();
		receiver.reset();
		CHECK(no_partial_files(dir));
		CHECK(stat((dir + "/kept.txt").c_str(), &st) == 0 && stat((dir + "/unfinished.bin").c_str(), &st) != 0);
		CHECK(notifications.size() == shown + 1 && notifications.back().first == "Received kept.txt");
		receiver = std::make_unique<wraith::FileReceiver>(
			loop.loop, dir, [&](uint64_t code) { protocol_errors.push_back(code); },
			[&](const std::string &summary, const std::string &body) { notifications.push_back({summary, body}); });
	}

	// Bytes before "accepted": a protocol violation, for the session to
	// close the connection over.
	{
		gdp::session::FileOpen open;
		open.mutable_offer()->set_name("eager.bin");
		open.mutable_offer()->set_size(4);
		gdp::session::StreamOpen stream_open;
		stream_open.set_kind(gdp::kStreamKindFile);
		stream_open.set_params(open.SerializeAsString());
		std::vector<uint8_t> bytes;
		gdp::encode_frame(stream_open, &bytes);
		bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
		auto s = client->open_stream();
		s->send(bytes.data(), bytes.size());
		CHECK(loop.until(5000, [&] { return !protocol_errors.empty(); }));
		CHECK(protocol_errors.size() == 1 &&
			protocol_errors[0] == static_cast<uint64_t>(gdp::ErrorCode::kMalformedFrame));
		CHECK(stat((dir + "/eager.bin").c_str(), &st) != 0);
	}

	CHECK(no_partial_files(dir));
	receiver.reset();
	client.reset();
	server_conn.reset();
	std::error_code ignored;
	std::filesystem::remove_all(dir, ignored);
	unlink(cert.c_str());
	unlink(key.c_str());
	if (g_failures) {
		fprintf(stderr, "file_receiver_test: %d check(s) failed\n", g_failures);
		return 1;
	}
	printf("file_receiver_test: all checks passed\n");
	return 0;
}
