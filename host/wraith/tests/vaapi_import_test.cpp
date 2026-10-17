// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// VaapiEncoderBase's dmabuf path (encode/vaapi/vaapi_encoder_base.cpp:
// import, then the VPP's RGB -> NV12) against the pixels in the buffer,
// for linear XRGB8888 frames at widths whose rows are and aren't a
// multiple of 64 bytes: can_import() must accept exactly the frames whose
// NV12 luma comes out as the buffer's. iHD's VPP reads a 3296-byte pitch
// (824 px) as 3328, which sheared the picture of every session at such a
// width; a driver that reads it right must not be sent down the read-back
// path for nothing. iHD misreads only on surfaces 128 rows or taller, so
// the frames here are as tall as the smallest output gdp-spec.md §7.2
// allows.
//
// GBM's own linear pitch is width * 4 on Intel but 256-byte aligned on
// radeonsi, where none of these frames would reach the driver probe. A few
// frames are therefore laid out at width * 4 by hand inside a wider buffer,
// so that the probe runs, and is checked, on every driver. A driver may
// refuse such an import (radeonsi does); then can_import() must refuse too.
//
// Needs a render node with a VA-API H.264 encoder (WRAITH_TEST_RENDER_NODE,
// default /dev/dri/renderD128); without one it skips (exit 77).
#include "encode/vaapi/vaapi_h264_encoder.hpp"

#include <gbm.h>
#include <libdrm/drm_fourcc.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

constexpr int kSkip = 77;

// push()'s import and conversion, with the NV12 luma read back out.
class ImportProbe : public wraith::VaapiH264Encoder {
public:
	// The frame's luma as push() would encode it, w * h bytes; empty if
	// any step failed.
	std::vector<uint8_t> luma(const wraith::DmabufFrame &frame) {
		std::vector<uint8_t> out;
		VASurfaceID rgb = import_rgb_surface(frame);
		if (rgb == VA_INVALID_ID) {
			return out;
		}
		VASurfaceID nv12 = convert_to_yuv(rgb);
		vaDestroySurfaces(display_, &rgb, 1);
		VAImage image = {};
		if (nv12 == VA_INVALID_ID || vaDeriveImage(display_, nv12, &image) != VA_STATUS_SUCCESS) {
			return out;
		}
		uint8_t *mapped = nullptr;
		if (vaMapBuffer(display_, image.buf, (void **)&mapped) == VA_STATUS_SUCCESS) {
			uint32_t w = (uint32_t)frame.width, h = (uint32_t)frame.height;
			out.resize((size_t)w * h);
			for (uint32_t y = 0; y < h; y++) {
				memcpy(&out[(size_t)y * w], mapped + image.offsets[0] + (size_t)y * image.pitches[0], w);
			}
			vaUnmapBuffer(display_, image.buf);
		}
		vaDestroyImage(display_, image.image_id);
		return out;
	}
};

std::vector<uint8_t> random_pixels(uint32_t w, uint32_t h, uint32_t seed) {
	std::vector<uint8_t> pixels((size_t)w * h * 4);
	for (size_t i = 0; i < pixels.size(); i++) {
		seed = seed * 1664525u + 1013904223u;
		pixels[i] = (i % 4 == 3) ? 0xff : (uint8_t)(seed >> 24);
	}
	return pixels;
}

// BT.601 studio range, as the VPP is asked for (convert_to_yuv(), 8-bit).
int expected_luma(const uint8_t *bgrx) {
	return 16 + (66 * bgrx[2] + 129 * bgrx[1] + 25 * bgrx[0] + 128) / 256;
}

// A `w` x `h` frame in an `alloc_w` wide buffer: a pitch other than GBM's
// own choice for `w`. `pitch` lays the rows out that many bytes apart and
// tells VA so, whatever pitch GBM picked (which must be at least that);
// 0 uses GBM's.
void check_width(gbm_device *gbm, int fd, uint32_t w, uint32_t alloc_w, uint32_t h, uint32_t pitch = 0) {
	ImportProbe probe;
	wraith::EncoderConfig config;
	config.codec = gdp::VideoCodec::H264;
	config.width = w;
	config.height = h;
	config.drm_fd = fd;
	if (!probe.open(config)) {
		fprintf(stderr, "%ux%u: can't open the encoder\n", w, h);
		g_failures++;
		return;
	}
	// Spare rows, so that a driver reading at a wider pitch stays inside it.
	gbm_bo *bo =
		gbm_bo_create(gbm, alloc_w, h + 16, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
	if (!bo) {
		fprintf(stderr, "%u (in %u) x %u: can't make a linear dmabuf\n", w, alloc_w, h);
		g_failures++;
		return;
	}
	std::vector<uint8_t> pixels = random_pixels(w, h, w * 7919 + alloc_w);
	uint32_t stride = 0;
	void *map_data = nullptr;
	auto *map =
		static_cast<uint8_t *>(gbm_bo_map(bo, 0, 0, alloc_w, h, GBM_BO_TRANSFER_WRITE, &stride, &map_data));
	CHECK(map != nullptr);
	if (pitch == 0) {
		pitch = stride;
	} else if (pitch > stride) {
		fprintf(stderr, "%u (in %u) x %u: GBM's pitch %u is under the %u asked for\n", w, alloc_w, h, stride,
			pitch);
		g_failures++;
		if (map) {
			gbm_bo_unmap(bo, map_data);
		}
		gbm_bo_destroy(bo);
		return;
	}
	if (map) {
		for (uint32_t y = 0; y < h; y++) {
			memcpy(map + (size_t)y * pitch, &pixels[(size_t)y * w * 4], (size_t)w * 4);
		}
		gbm_bo_unmap(bo, map_data);
	}
	wraith::DmabufFrame f;
	f.width = (int32_t)w;
	f.height = (int32_t)h;
	f.format = DRM_FORMAT_XRGB8888;
	f.modifier = DRM_FORMAT_MOD_LINEAR;
	f.n_planes = 1;
	f.fd[0] = gbm_bo_get_fd(bo);
	f.offset[0] = gbm_bo_get_offset(bo, 0);
	f.stride[0] = pitch;

	if (map) {
		std::vector<uint8_t> got = probe.luma(f);
		// GBM's own layout must import; one laid out by hand may be
		// refused, and an empty result then counts as not read right.
		CHECK(!got.empty() || pitch != stride);
		// Rounding differs between drivers by a step or two; a misread
		// row is off by far more nearly everywhere.
		size_t differ = 0;
		for (size_t i = 0; i < got.size(); i++) {
			differ += abs(got[i] - expected_luma(&pixels[i * 4])) > 3;
		}
		bool read_right = !got.empty() && differ == 0;
		bool accepted = probe.can_import(f);
		if (accepted != read_right) {
			fprintf(stderr, "%ux%u, %u-byte pitch: can_import() is %s, %zu of %zu luma samples wrong\n", w, h,
				f.stride[0], accepted ? "true" : "false", differ, got.size());
		}
		CHECK(accepted == read_right);
	}
	close(f.fd[0]);
	gbm_bo_destroy(bo);
	probe.close();
}

} // namespace

int main() {
	const char *node = getenv("WRAITH_TEST_RENDER_NODE");
	if (!node) {
		node = "/dev/dri/renderD128";
	}
	int fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("vaapi_import_test: no %s, skipped\n", node);
		return kSkip;
	}
	gbm_device *gbm = gbm_create_device(fd);
	int result = kSkip;
	{
		ImportProbe probe;
		wraith::EncoderConfig config;
		config.codec = gdp::VideoCodec::H264;
		config.width = 256;
		config.height = 64;
		config.drm_fd = fd;
		bool have_encoder = gbm && probe.open(config);
		probe.close();
		if (!have_encoder) {
			printf("vaapi_import_test: no VA-API H.264 encoder on %s, skipped\n", node);
		} else {
			// Every even width from 768 to 872: rows of width * 4 bytes,
			// a multiple of 64 only every 16 px.
			constexpr uint32_t kHeight = 320;
			for (uint32_t w = 768; w <= 872; w += 2) {
				check_width(gbm, fd, w, w, kHeight);
			}
			check_width(gbm, fd, 824, 832, kHeight); // 3328-byte pitch
			check_width(gbm, fd, 824, 840, kHeight); // 3360
			check_width(gbm, fd, 1920, 1920, kHeight);
			// width * 4 by hand, so the driver probe runs on every driver.
			check_width(gbm, fd, 808, 832, kHeight, 808 * 4); // 3232
			check_width(gbm, fd, 824, 832, kHeight, 824 * 4); // 3296
			check_width(gbm, fd, 856, 896, kHeight, 856 * 4); // 3424
			result = g_failures ? 1 : 0;
		}
	}
	if (gbm) {
		gbm_device_destroy(gbm);
	}
	close(fd);
	if (result == 1) {
		fprintf(stderr, "vaapi_import_test: %d check(s) failed\n", g_failures);
	} else if (result == 0) {
		printf("vaapi_import_test: ok\n");
	}
	return result;
}
