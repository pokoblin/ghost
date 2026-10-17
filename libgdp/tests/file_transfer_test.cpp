// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// gdp/file_transfer.hpp: the name rules, the numbered fallback name, and
// the reader that splits a "file" stream into its StreamOpen and data.
#include "gdp/error_codes.hpp"
#include "gdp/file_transfer.hpp"
#include "gdp/framing.hpp"
#include "session.pb.h"

#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {

void test_valid_file_name() {
	assert(gdp::valid_file_name("report.pdf"));
	assert(gdp::valid_file_name(".bashrc"));
	assert(gdp::valid_file_name("名前 with spaces ✓.txt"));
	assert(gdp::valid_file_name("..."));
	assert(gdp::valid_file_name(std::string(255, 'a')));

	assert(!gdp::valid_file_name(""));
	assert(!gdp::valid_file_name("."));
	assert(!gdp::valid_file_name(".."));
	assert(!gdp::valid_file_name(std::string(256, 'a')));
	assert(!gdp::valid_file_name("a/b"));
	assert(!gdp::valid_file_name("../etc/passwd"));
	assert(!gdp::valid_file_name("a\\b"));
	assert(!gdp::valid_file_name(std::string("a\0b", 3)));
	assert(!gdp::valid_file_name("tab\there"));
	assert(!gdp::valid_file_name("new\nline"));
	assert(!gdp::valid_file_name("del\x7f"));
	// Not UTF-8: a lone continuation byte, a truncated sequence, an overlong
	// '/', a UTF-16 surrogate, past U+10FFFF.
	assert(!gdp::valid_file_name("\x80"));
	assert(!gdp::valid_file_name("\xe2\x9c"));
	assert(!gdp::valid_file_name("\xc0\xaf"));
	assert(!gdp::valid_file_name("\xed\xa0\x80"));
	assert(!gdp::valid_file_name("\xf4\x90\x80\x80"));
	// 255 bytes of 3-byte characters is 85 of them; 86 is too many.
	std::string check;
	for (int i = 0; i < 85; i++) {
		check += "\xe2\x9c\x93";
	}
	assert(gdp::valid_file_name(check));
	assert(!gdp::valid_file_name(check + "\xe2\x9c\x93"));
}

void test_numbered_file_name() {
	assert(gdp::numbered_file_name("report.pdf", 1) == "report (1).pdf");
	assert(gdp::numbered_file_name("archive.tar.gz", 2) == "archive.tar (2).gz");
	assert(gdp::numbered_file_name("README", 3) == "README (3)");
	assert(gdp::numbered_file_name(".bashrc", 1) == ".bashrc (1)");
	assert(gdp::numbered_file_name("trailing.", 1) == "trailing. (1)");

	// Long names shrink by whole characters and stay valid.
	std::string long_ascii = std::string(251, 'a') + ".txt";
	std::string n = gdp::numbered_file_name(long_ascii, 12);
	assert(n.size() <= gdp::kMaxFileNameBytes && gdp::valid_file_name(n));
	assert(n.substr(n.size() - 9) == " (12).txt");

	std::string long_utf8;
	for (int i = 0; i < 84; i++) {
		long_utf8 += "\xe2\x9c\x93";
	}
	long_utf8 += ".md";
	n = gdp::numbered_file_name(long_utf8, 7);
	assert(n.size() <= gdp::kMaxFileNameBytes && gdp::valid_file_name(n));
	assert(n.substr(n.size() - 7) == " (7).md");

	// An extension that alone fills the limit gives way too.
	std::string huge_ext = "a." + std::string(253, 'x');
	n = gdp::numbered_file_name(huge_ext, 1);
	assert(n.size() <= gdp::kMaxFileNameBytes && gdp::valid_file_name(n));
}

std::vector<uint8_t> open_frame(const std::string &name, uint64_t size) {
	gdp::session::FileOpen file_open;
	file_open.mutable_offer()->set_name(name);
	file_open.mutable_offer()->set_size(size);
	gdp::session::StreamOpen open;
	open.set_kind(gdp::kStreamKindFile);
	open.set_params(file_open.SerializeAsString());
	std::vector<uint8_t> frame;
	assert(gdp::encode_frame(open, &frame));
	return frame;
}

void test_reader() {
	using State = gdp::FileStreamReader::State;
	std::vector<uint8_t> frame = open_frame("photo.jpg", 5);
	std::vector<uint8_t> stream = frame;
	for (uint8_t b : {1, 2, 3, 4, 5}) {
		stream.push_back(b);
	}

	// A byte at a time: waiting until the frame is whole, then data.
	{
		gdp::FileStreamReader reader;
		std::vector<uint8_t> data;
		for (size_t i = 0; i < stream.size(); i++) {
			State s = reader.feed(&stream[i], 1);
			assert(s == (i + 1 < frame.size() ? State::kWaiting : State::kOpened));
			auto more = reader.take_data();
			data.insert(data.end(), more.begin(), more.end());
		}
		assert(reader.kind() == gdp::kStreamKindFile);
		gdp::session::FileOpen parsed;
		assert(parsed.ParseFromString(reader.params()));
		assert(parsed.offer().name() == "photo.jpg" && parsed.offer().size() == 5);
		assert((data == std::vector<uint8_t>{1, 2, 3, 4, 5}));
	}

	// All at once: the data after the frame is there straight away.
	{
		gdp::FileStreamReader reader;
		assert(reader.feed(stream.data(), stream.size()) == State::kOpened);
		assert((reader.take_data() == std::vector<uint8_t>{1, 2, 3, 4, 5}));
		assert(reader.take_data().empty());
	}

	// A length prefix past the limit, and a frame that doesn't parse.
	{
		gdp::FileStreamReader reader;
		uint8_t big[] = {0x01, 0x10, 0x00, 0x00}; // 4097
		assert(reader.feed(big, sizeof(big)) == State::kError);
		assert(reader.error_code() == static_cast<uint64_t>(gdp::ErrorCode::kFrameTooLarge));
		assert(reader.take_data().empty());
	}
	{
		gdp::FileStreamReader reader;
		uint8_t junk[] = {0x02, 0x00, 0x00, 0x00, 0xff, 0xff};
		assert(reader.feed(junk, sizeof(junk)) == State::kError);
		assert(reader.error_code() == static_cast<uint64_t>(gdp::ErrorCode::kMalformedFrame));
	}
}

void test_copied_files() {
	using V = std::vector<std::string>;
	assert(gdp::select_copied_files_mime({"text/plain", "text/uri-list", "x-special/gnome-copied-files"}) ==
		"x-special/gnome-copied-files");
	assert(gdp::select_copied_files_mime({"UTF8_STRING", "text/uri-list"}) == "text/uri-list");
	assert(gdp::select_copied_files_mime({"text/plain"}).empty());

	// Nautilus: the action first, then one URI a line, percent-encoded.
	assert((gdp::copied_file_paths("x-special/gnome-copied-files",
				"copy\nfile:///home/dws/a.txt\nfile:///home/dws/My%20Files/%E2%9C%93.pdf") ==
		V{"/home/dws/a.txt", "/home/dws/My Files/\xe2\x9c\x93.pdf"}));
	assert((gdp::copied_file_paths("x-special/gnome-copied-files", "cut\nfile:///tmp/x\n") == V{"/tmp/x"}));
	// RFC 2483: CRLF, comments; localhost is local, other hosts and schemes aren't.
	assert((gdp::copied_file_paths("text/uri-list",
				"# a comment\r\nfile:///a\r\nfile://localhost/b\r\nfile://other/c\r\nhttps://x/y\r\n") ==
		V{"/a", "/b"}));
	// A first line that is a URI counts in a URI list, not in GNOME's format.
	assert((gdp::copied_file_paths("text/uri-list", "file:///only") == V{"/only"}));
	assert(gdp::copied_file_paths("x-special/gnome-copied-files", "file:///only").empty());
	// No NULs in a path, malformed escapes kept as they are, other formats nothing.
	assert(gdp::copied_file_paths("text/uri-list", "file:///a%00b").empty());
	assert((gdp::copied_file_paths("text/uri-list", "file:///100%zz") == V{"/100%zz"}));
	assert(gdp::copied_file_paths("text/plain", "file:///a").empty());
}

void test_frame_then_raw() {
	using State = gdp::FrameThenRawReader::State;
	gdp::session::FileReply reply;
	reply.mutable_sending()->set_name("a.txt");
	reply.mutable_sending()->set_size(3);
	std::vector<uint8_t> bytes;
	assert(gdp::encode_frame(reply, &bytes));
	size_t frame_size = bytes.size();
	bytes.insert(bytes.end(), {7, 8, 9});
	gdp::FrameThenRawReader reader(1024);
	assert(reader.feed(bytes.data(), frame_size - 1) == State::kWaiting);
	assert(reader.feed(bytes.data() + frame_size - 1, 2) == State::kFramed);
	gdp::session::FileReply parsed;
	assert(parsed.ParseFromString(reader.frame()) && parsed.sending().name() == "a.txt");
	assert((reader.take_data() == std::vector<uint8_t>{7}));
	reader.feed(bytes.data() + frame_size + 1, 2);
	assert((reader.take_data() == std::vector<uint8_t>{8, 9}));

	gdp::FrameThenRawReader small(4);
	assert(small.feed(bytes.data(), bytes.size()) == State::kError);
	assert(small.error_code() == static_cast<uint64_t>(gdp::ErrorCode::kFrameTooLarge));
}

} // namespace

int main() {
	test_valid_file_name();
	test_numbered_file_name();
	test_reader();
	test_copied_files();
	test_frame_then_raw();
	printf("file_transfer_test: all checks passed\n");
	return 0;
}
