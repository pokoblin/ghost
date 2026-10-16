// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Additional streams over loopback (gdp-spec.md §2.2): the credit the host
// grants with allow_streams() and replaces as streams close, refusal
// without it, FIN and reset in both directions, and the application-driven
// receive window (Stream::consume()) that lets a slow reader hold a sender
// back without starving control and input.
#include "gdp/error_codes.hpp"
#include "gdp/transport.hpp"

// Plain assert()s: make sure a Release build (-DNDEBUG) can't compile them
// away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

struct TempCert {
	std::string cert_path = "/tmp/gdp_additional_stream_cert.pem";
	std::string key_path = "/tmp/gdp_additional_stream_key.pem";

	TempCert() {
		std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + key_path + " -out " + cert_path +
			" -days 1 -nodes -subj /CN=localhost >/tmp/gdp_additional_stream_openssl.log 2>&1";
		if (system(cmd.c_str()) != 0) {
			fprintf(stderr, "additional_stream_test: openssl cert generation failed\n");
			exit(1);
		}
	}
	~TempCert() {
		unlink(cert_path.c_str());
		unlink(key_path.c_str());
	}
};

int64_t now_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1'000'000;
}

// Pumps both transports until `done()` or `timeout_ms`; true if done.
bool pump_until(gdp::Transport &a, gdp::Transport &b, int timeout_ms, const std::function<bool()> &done) {
	int64_t start = now_ms();
	for (;;) {
		if (done()) {
			return true;
		}
		struct pollfd fds[2] = {{a.notify_fd(), POLLIN, 0}, {b.notify_fd(), POLLIN, 0}};
		poll(fds, 2, 20);
		a.dispatch();
		b.dispatch();
		if (now_ms() - start > timeout_ms) {
			return false;
		}
	}
}

// Pumps for a fixed time: for checking that something does *not* happen.
void pump_for(gdp::Transport &a, gdp::Transport &b, int ms) {
	pump_until(a, b, ms, [] { return false; });
}

// The server's view of one additional stream.
struct Received {
	std::unique_ptr<gdp::Stream> stream;
	std::vector<uint8_t> data;
	size_t unconsumed = 0;
	bool fin = false;
	bool reset = false;
	uint64_t reset_code = 0;
};

// The client's view of one additional stream.
struct Sent {
	std::unique_ptr<gdp::Stream> stream;
	std::string reply;
	uint64_t acked = 0;
	bool fin = false;
	bool reset = false;
	uint64_t reset_code = 0;
};

void watch(Sent &s) {
	s.stream->on_data = [&s](const uint8_t *data, size_t len) {
		s.reply.append(reinterpret_cast<const char *>(data), len);
	};
	s.stream->on_acked = [&s](uint64_t acked) {
		assert(acked >= s.acked && "acked went backwards");
		s.acked = acked;
	};
	s.stream->on_fin = [&s] { s.fin = true; };
	s.stream->on_reset = [&s](uint64_t code) {
		s.reset = true;
		s.reset_code = code;
	};
}

std::vector<uint8_t> pattern(size_t len, uint8_t seed) {
	std::vector<uint8_t> out(len);
	for (size_t i = 0; i < len; i++) {
		out[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 13));
	}
	return out;
}

} // namespace

int main() {
	TempCert cert;
	gdp::Transport server_transport;
	gdp::Transport client_transport;
	const uint16_t port = 44341;
	const char *alpn = "gdp-test/1";

	std::unique_ptr<gdp::Connection> server_conn;
	gdp::Stream *server_control = nullptr;
	std::string server_control_text;
	std::vector<std::unique_ptr<Received>> received;
	bool take_streams = false;

	auto on_stream = [&](std::unique_ptr<gdp::Stream> stream) {
		auto r = std::make_unique<Received>();
		Received *p = r.get();
		p->stream = std::move(stream);
		p->stream->on_data = [p](const uint8_t *data, size_t len) {
			p->data.insert(p->data.end(), data, data + len);
			p->unconsumed += len;
		};
		p->stream->on_fin = [p] { p->fin = true; };
		p->stream->on_reset = [p](uint64_t code) {
			p->reset = true;
			p->reset_code = code;
		};
		received.push_back(std::move(r));
	};

	bool listen_ok = server_transport.listen(port, alpn, cert.cert_path, cert.key_path,
		[&](std::unique_ptr<gdp::Connection> conn) {
			server_conn = std::move(conn);
			server_conn->on_control_stream = [&](gdp::Stream *s) {
				server_control = s;
				s->on_data = [&](const uint8_t *data, size_t len) {
					server_control_text.append(reinterpret_cast<const char *>(data), len);
				};
			};
			server_conn->on_stream = [&](std::unique_ptr<gdp::Stream> stream) {
				if (take_streams) {
					on_stream(std::move(stream));
				}
				// Otherwise dropped here: reset with code 0, like an
				// application that looked and said no.
			};
		});
	assert(listen_ok);

	auto client_conn = client_transport.connect("127.0.0.1", port, alpn);
	bool connected = false;
	client_conn->on_state_changed = [&](gdp::ConnectionState state) {
		connected = connected || state == gdp::ConnectionState::kConnected;
	};
	assert(pump_until(client_transport, server_transport, 5000, [&] { return connected; }));
	assert(!client_conn->open_stream() && "an additional stream before control and input");
	gdp::Stream *control = client_conn->open_control_stream();
	assert(client_conn->open_input_stream());
	assert(!server_conn || !server_conn->open_stream());

	// 1. Before allow_streams() the host grants no credit: the stream waits,
	// and what is sent on it is held, not refused.
	Sent first;
	first.stream = client_conn->open_stream();
	assert(first.stream);
	watch(first);
	const char *hello = "StreamOpen would go here";
	first.stream->send(reinterpret_cast<const uint8_t *>(hello), strlen(hello));
	control->send(reinterpret_cast<const uint8_t *>("ping"), 4);
	assert(pump_until(client_transport, server_transport, 5000, [&] { return server_control_text == "ping"; }));
	pump_for(client_transport, server_transport, 200);
	assert(received.empty() && !first.reset && "a stream got through without credit");

	// 2. allow_streams(): the waiting stream opens, its data arrives.
	take_streams = true;
	server_conn->allow_streams(2);
	assert(pump_until(client_transport, server_transport, 5000,
		[&] { return received.size() == 1 && received[0]->data.size() == strlen(hello); }));
	assert(memcmp(received[0]->data.data(), hello, strlen(hello)) == 0);

	// 3. A clean finish both ways: FIN from the client, reply and FIN from
	// the host, then both let go -- and neither side sees a reset.
	first.stream->send(nullptr, 0, true);
	assert(pump_until(client_transport, server_transport, 5000, [&] { return received[0]->fin; }));
	const char *reply = "saved";
	received[0]->stream->send(reinterpret_cast<const uint8_t *>(reply), strlen(reply), true);
	assert(pump_until(client_transport, server_transport, 5000, [&] { return first.fin; }));
	assert(first.reply == reply);
	assert(first.acked == strlen(hello) && "the host acked everything it read");
	first.stream.reset();
	received[0]->stream.reset();
	pump_for(client_transport, server_transport, 200);
	assert(!first.reset && !received[0]->reset);

	// 4. The host replaces closed streams: with an allowance of 2, three
	// more streams one after another all open (the first one's slot came
	// back when it closed).
	for (int i = 0; i < 3; i++) {
		Sent s;
		s.stream = client_conn->open_stream();
		watch(s);
		s.stream->send(reinterpret_cast<const uint8_t *>("x"), 1, true);
		size_t want = received.size() + 1;
		assert(pump_until(client_transport, server_transport, 5000,
			[&] { return received.size() == want && received.back()->fin; }));
		received.back()->stream->send(nullptr, 0, true);
		assert(pump_until(client_transport, server_transport, 5000, [&] { return s.fin; }));
		s.stream.reset();
		received.back()->stream.reset();
	}

	// 5. A stream the host's application drops is reset; the client hears
	// the code. Then one reset with UNSUPPORTED, as for an unknown kind.
	{
		take_streams = false;
		Sent s;
		s.stream = client_conn->open_stream();
		watch(s);
		s.stream->send(reinterpret_cast<const uint8_t *>("y"), 1);
		assert(pump_until(client_transport, server_transport, 5000, [&] { return s.reset; }));
		assert(s.reset_code == 0);
		take_streams = true;

		Sent u;
		u.stream = client_conn->open_stream();
		watch(u);
		u.stream->send(reinterpret_cast<const uint8_t *>("z"), 1);
		size_t want = received.size() + 1;
		assert(pump_until(client_transport, server_transport, 5000, [&] { return received.size() == want; }));
		received.back()->stream->reset(static_cast<uint64_t>(gdp::ErrorCode::kUnsupported));
		assert(pump_until(client_transport, server_transport, 5000, [&] { return u.reset; }));
		assert(u.reset_code == static_cast<uint64_t>(gdp::ErrorCode::kUnsupported));
	}

	// 6. The receive window follows consume(): two 6 MiB streams whose
	// reader takes nothing stall well short of their data, and while they
	// do -- together more than the connection's own 4 MiB window -- the
	// control stream still gets through. Then the reader catches up, and
	// everything arrives intact.
	{
		const size_t kSize = 6 << 20;
		const size_t kChunk = 256 << 10;
		struct Feed {
			Sent sent;
			std::vector<uint8_t> payload;
			size_t queued = 0;
		};
		Feed feeds[2];
		size_t base = received.size();
		for (int i = 0; i < 2; i++) {
			feeds[i].payload = pattern(kSize, static_cast<uint8_t>(i * 91 + 3));
			feeds[i].sent.stream = client_conn->open_stream();
			watch(feeds[i].sent);
		}
		// A sender that keeps at most 2 MiB beyond what was acked queued,
		// topping up as acks come in.
		auto top_up = [&] {
			for (auto &f : feeds) {
				while (f.queued < kSize && f.queued - f.sent.acked < (2u << 20)) {
					size_t n = std::min(kChunk, kSize - f.queued);
					f.sent.stream->send(f.payload.data() + f.queued, n, f.queued + n == kSize);
					f.queued += n;
				}
			}
		};
		top_up();
		pump_until(client_transport, server_transport, 1500, [&] {
			top_up();
			return false;
		});
		assert(received.size() == base + 2);
		for (int i = 0; i < 2; i++) {
			size_t got = received[base + i]->data.size();
			assert(got > 0 && got <= (2u << 20) && "the stream window didn't hold the sender back");
			assert(feeds[i].sent.acked <= got && "acked data the host never received");
		}
		server_control_text.clear();
		control->send(reinterpret_cast<const uint8_t *>("still here"), 10);
		assert(pump_until(client_transport, server_transport, 5000,
			[&] { return server_control_text == "still here"; }) &&
			"control blocked behind stalled additional streams");

		// The reader catches up: consume as it arrives.
		bool done = pump_until(client_transport, server_transport, 20000, [&] {
			top_up();
			for (size_t i = base; i < base + 2; i++) {
				Received &r = *received[i];
				if (r.unconsumed) {
					r.stream->consume(r.unconsumed);
					r.unconsumed = 0;
				}
			}
			return received[base]->fin && received[base + 1]->fin;
		});
		assert(done && "the transfer never finished once the reader consumed");
		for (int i = 0; i < 2; i++) {
			assert(received[base + i]->data == feeds[i].payload);
		}
		// Finished both ways, so their two slots of the allowance come back
		// for the next stream. (Half-open, they would hold them.)
		for (int i = 0; i < 2; i++) {
			received[base + i]->stream->send(nullptr, 0, true);
		}
		assert(pump_until(client_transport, server_transport, 5000,
			[&] { return feeds[0].sent.fin && feeds[1].sent.fin; }));
		for (int i = 0; i < 2; i++) {
			assert(feeds[i].sent.acked == kSize);
			feeds[i].sent.stream.reset();
			received[base + i]->stream.reset();
		}

		// 7. A client that gives up mid-way: the host hears the reset, with
		// the code the client chose.
		Sent cancel;
		cancel.stream = client_conn->open_stream();
		watch(cancel);
		std::vector<uint8_t> some = pattern(64 << 10, 1);
		cancel.stream->send(some.data(), some.size());
		size_t want = received.size() + 1;
		assert(pump_until(client_transport, server_transport, 5000,
			[&] { return received.size() == want && !received.back()->data.empty(); }));
		cancel.stream->reset(0);
		assert(pump_until(client_transport, server_transport, 5000, [&] { return received.back()->reset; }));
		assert(received.back()->reset_code == 0);
		assert(!received.back()->fin);
	}

	(void)server_control;
	printf("additional_stream_test: all checks passed\n");
	return 0;
}
