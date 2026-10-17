// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The hand-written sequence headers at both bit depths (encode/vaapi/
// hevc_bitstream.hpp, av1_obu.hpp): a Main10 SPS says Main10 and 10 bits
// where a Main one says Main and 8, and the AV1 sequence header's
// high_bitdepth follows the depth. Parsed back with a reader of the same
// RBSP syntax, field by field up to the depth fields -- a decoder would
// read the same bits.

#include "encode/vaapi/av1_obu.hpp"
#include "encode/vaapi/hevc_bitstream.hpp"

#include <cstdio>
#include <cstdint>
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

// MSB-first reader for u(n) and ue(v), the inverse of nal::BitWriter.
class BitReader {
public:
	explicit BitReader(const std::vector<uint8_t> &bytes) : bytes_(bytes) {}
	uint32_t bits(int n) {
		uint32_t value = 0;
		for (int i = 0; i < n; i++) {
			size_t byte = pos_ / 8;
			unsigned bit = byte < bytes_.size() ? (bytes_[byte] >> (7 - pos_ % 8)) & 1 : 0;
			value = (value << 1) | bit;
			pos_++;
		}
		return value;
	}
	uint32_t ue() {
		int zeros = 0;
		while (bits(1) == 0 && zeros < 32) {
			zeros++;
		}
		return (1u << zeros) - 1 + bits(zeros);
	}

private:
	const std::vector<uint8_t> &bytes_;
	size_t pos_ = 0;
};

struct ProfileTierLevel {
	uint32_t profile_idc;
	uint32_t compat_flags; // bit i = general_profile_compatibility_flag[i]
	uint32_t level_idc;
};

ProfileTierLevel read_profile_tier_level(BitReader &r) {
	ProfileTierLevel ptl{};
	r.bits(2); // general_profile_space
	r.bits(1); // general_tier_flag
	ptl.profile_idc = r.bits(5);
	for (int i = 0; i < 32; i++) {
		ptl.compat_flags |= r.bits(1) << i;
	}
	r.bits(4);  // progressive, interlaced, non_packed, frame_only
	r.bits(32); // reserved 43 bits ...
	r.bits(11);
	r.bits(1); // general_inbld_flag
	ptl.level_idc = r.bits(8);
	return ptl;
}

struct SpsFields {
	ProfileTierLevel ptl;
	uint32_t chroma_format_idc;
	uint32_t width, height;
	uint32_t bit_depth_luma, bit_depth_chroma;
	// vui_parameters()'s colour description; all 0 when there is none.
	bool vui;
	uint32_t full_range;
	uint32_t colour_primaries, transfer, matrix;
};

SpsFields read_sps(const std::vector<uint8_t> &rbsp) {
	BitReader r(rbsp);
	SpsFields f{};
	r.bits(4); // sps_video_parameter_set_id
	r.bits(3); // sps_max_sub_layers_minus1
	r.bits(1); // sps_temporal_id_nesting_flag
	f.ptl = read_profile_tier_level(r);
	r.ue(); // sps_seq_parameter_set_id
	f.chroma_format_idc = r.ue();
	f.width = r.ue();
	f.height = r.ue();
	if (r.bits(1)) { // conformance_window_flag
		r.ue();
		r.ue();
		r.ue();
		r.ue();
	}
	f.bit_depth_luma = r.ue() + 8;
	f.bit_depth_chroma = r.ue() + 8;
	// Through to vui_parameters_present_flag, as build_sps() writes it
	// (one RPS-free, long-term-free, PCM-free layout).
	r.ue();          // log2_max_pic_order_cnt_lsb_minus4
	if (r.bits(1)) { // sps_sub_layer_ordering_info_present_flag
		r.ue();
		r.ue();
		r.ue();
	}
	for (int i = 0; i < 6; i++) {
		r.ue(); // block sizes and transform depths
	}
	CHECK(r.bits(1) == 0); // scaling_list_enabled_flag
	r.bits(1);             // amp_enabled_flag
	r.bits(1);             // sample_adaptive_offset_enabled_flag
	CHECK(r.bits(1) == 0); // pcm_enabled_flag
	CHECK(r.ue() == 0);    // num_short_term_ref_pic_sets
	CHECK(r.bits(1) == 0); // long_term_ref_pics_present_flag
	r.bits(1);             // sps_temporal_mvp_enabled_flag
	r.bits(1);             // strong_intra_smoothing_enabled_flag
	f.vui = r.bits(1) != 0;
	if (f.vui) {
		CHECK(r.bits(1) == 0); // aspect_ratio_info_present_flag
		CHECK(r.bits(1) == 0); // overscan_info_present_flag
		CHECK(r.bits(1) == 1); // video_signal_type_present_flag
		r.bits(3);             // video_format
		f.full_range = r.bits(1);
		CHECK(r.bits(1) == 1); // colour_description_present_flag
		f.colour_primaries = r.bits(8);
		f.transfer = r.bits(8);
		f.matrix = r.bits(8);
		// Every remaining VUI flag is 0, then sps_extension_present_flag
		// and the trailing stop bit.
		for (int i = 0; i < 7; i++) {
			CHECK(r.bits(1) == 0);
		}
		CHECK(r.bits(1) == 0); // sps_extension_present_flag
		CHECK(r.bits(1) == 1); // rbsp_stop_one_bit
	}
	return f;
}

ProfileTierLevel read_vps_ptl(const std::vector<uint8_t> &rbsp) {
	BitReader r(rbsp);
	r.bits(4);  // vps_video_parameter_set_id
	r.bits(2);  // base_layer_internal, base_layer_available
	r.bits(6);  // vps_max_layers_minus1
	r.bits(3);  // vps_max_sub_layers_minus1
	r.bits(1);  // vps_temporal_id_nesting_flag
	r.bits(16); // vps_reserved_0xffff_16bits
	return read_profile_tier_level(r);
}

wraith::hevc::SpsParams sps_params(uint8_t bit_depth) {
	wraith::hevc::SpsParams p{};
	p.bit_depth = bit_depth;
	p.width_px = 1920;
	p.height_px = 1080;
	p.coded_width = 1920;
	p.coded_height = 1088;
	p.level_idc = 123;
	p.log2_min_luma_coding_block_size_minus3 = 0;
	p.log2_diff_max_min_luma_coding_block_size = 3;
	p.log2_min_transform_block_size_minus2 = 0;
	p.log2_diff_max_min_transform_block_size = 3;
	p.max_transform_hierarchy_depth_inter = 2;
	p.max_transform_hierarchy_depth_intra = 2;
	p.log2_max_pic_order_cnt_lsb_minus4 = 4;
	p.amp_enabled = true;
	p.sao_enabled = true;
	p.temporal_mvp_enabled = false;
	p.strong_intra_smoothing_enabled = true;
	p.colour_primaries = 1;
	p.transfer_characteristics = 13;
	p.matrix_coefficients = 1;
	return p;
}

void test_hevc() {
	CHECK(wraith::hevc::profile_idc_for_bit_depth(8) == 1);
	CHECK(wraith::hevc::profile_idc_for_bit_depth(10) == 2);

	SpsFields main = read_sps(wraith::hevc::build_sps(sps_params(8)));
	CHECK(main.ptl.profile_idc == 1);
	CHECK(main.ptl.compat_flags == ((1u << 1) | (1u << 2))); // Main, and Main10 takes it
	CHECK(main.ptl.level_idc == 123);
	CHECK(main.chroma_format_idc == 1);
	CHECK(main.width == 1920 && main.height == 1088);
	CHECK(main.bit_depth_luma == 8 && main.bit_depth_chroma == 8);

	SpsFields main10 = read_sps(wraith::hevc::build_sps(sps_params(10)));
	CHECK(main10.ptl.profile_idc == 2);
	CHECK(main10.ptl.compat_flags == (1u << 2)); // Main10 only
	CHECK(main10.ptl.level_idc == 123);
	CHECK(main10.chroma_format_idc == 1);
	CHECK(main10.bit_depth_luma == 10 && main10.bit_depth_chroma == 10);

	// The VUI's colour description says what sps_params() asked for, studio
	// range, and the SPS ends where the reader expects it to.
	CHECK(main.vui && main10.vui);
	CHECK(main10.full_range == 0);
	CHECK(main10.colour_primaries == 1 && main10.transfer == 13 && main10.matrix == 1);
	wraith::hevc::SpsParams bt601 = sps_params(8);
	bt601.colour_primaries = bt601.transfer_characteristics = bt601.matrix_coefficients = 6;
	SpsFields legacy = read_sps(wraith::hevc::build_sps(bt601));
	CHECK(legacy.colour_primaries == 6 && legacy.transfer == 6 && legacy.matrix == 6);

	// The VPS states the same profile as the SPS it precedes.
	CHECK(read_vps_ptl(wraith::hevc::build_vps(123, 8)).profile_idc == 1);
	CHECK(read_vps_ptl(wraith::hevc::build_vps(123, 10)).profile_idc == 2);
	CHECK(read_vps_ptl(wraith::hevc::build_vps(123, 10)).compat_flags == (1u << 2));
}

// sequence_header_obu() as far as color_config()'s high_bitdepth, given
// what build_sequence_header() writes before it.
struct SeqFields {
	uint32_t seq_profile;
	uint32_t seq_level_idx;
	uint32_t max_width, max_height;
	uint32_t high_bitdepth;
	uint32_t color_primaries;
};

SeqFields read_sequence_header(const std::vector<uint8_t> &obu) {
	// OBU header (1 byte, has_size_field set) and a LEB128 size.
	size_t pos = 1;
	while (pos < obu.size() && (obu[pos] & 0x80)) {
		pos++;
	}
	pos++;
	std::vector<uint8_t> payload(obu.begin() + (ptrdiff_t)pos, obu.end());
	BitReader r(payload);
	SeqFields f{};
	f.seq_profile = r.bits(3);
	r.bits(1);  // still_picture
	r.bits(1);  // reduced_still_picture_header
	r.bits(1);  // timing_info_present_flag
	r.bits(1);  // initial_display_delay_present_flag
	r.bits(5);  // operating_points_cnt_minus_1
	r.bits(12); // operating_point_idc[0]
	f.seq_level_idx = r.bits(5);
	if (f.seq_level_idx > 7) {
		r.bits(1); // seq_tier[0]
	}
	int width_bits = (int)r.bits(4) + 1;
	int height_bits = (int)r.bits(4) + 1;
	f.max_width = r.bits(width_bits) + 1;
	f.max_height = r.bits(height_bits) + 1;
	r.bits(1);             // frame_id_numbers_present_flag
	r.bits(10);            // use_128x128_superblock ... enable_ref_frame_mvs
	CHECK(r.bits(1) == 1); // seq_choose_screen_content_tools
	r.bits(1);             // seq_choose_integer_mv
	r.bits(1);             // seq_force_integer_mv
	r.bits(3);             // order_hint_bits_minus_1
	r.bits(1);             // enable_superres
	r.bits(1);             // enable_cdef
	r.bits(1);             // enable_restoration
	f.high_bitdepth = r.bits(1);
	r.bits(1);             // mono_chrome
	CHECK(r.bits(1) == 1); // color_description_present_flag
	f.color_primaries = r.bits(8);
	return f;
}

void test_av1() {
	wraith::av1::SeqParams p{};
	p.coded_width = 1368;
	p.coded_height = 768;
	p.seq_level_idx = 8;
	p.order_hint_bits = 8;
	p.enable_cdef = true;
	p.color_primaries = 6;
	p.transfer_characteristics = 6;
	p.matrix_coefficients = 6;
	p.full_range = false;

	p.high_bitdepth = false;
	SeqFields eight = read_sequence_header(wraith::av1::build_sequence_header(p));
	CHECK(eight.seq_profile == 0);
	CHECK(eight.seq_level_idx == 8);
	CHECK(eight.max_width == 1368 && eight.max_height == 768);
	CHECK(eight.high_bitdepth == 0);
	CHECK(eight.color_primaries == 6);

	p.high_bitdepth = true;
	SeqFields ten = read_sequence_header(wraith::av1::build_sequence_header(p));
	CHECK(ten.seq_profile == 0); // 10-bit 4:2:0 is still profile 0
	CHECK(ten.max_width == 1368 && ten.max_height == 768);
	CHECK(ten.high_bitdepth == 1);
	CHECK(ten.color_primaries == 6);
}

} // namespace

int main() {
	test_hevc();
	test_av1();
	if (g_failures) {
		fprintf(stderr, "bitstream_test: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("bitstream_test: all checks passed\n");
	return 0;
}
