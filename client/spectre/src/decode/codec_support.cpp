// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "decode/codec_support.hpp"

#include "present/vulkan_device.hpp"
#include "log.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgiformat.h>
#elif defined(__APPLE__)
#include <VideoToolbox/VideoToolbox.h>
#else
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#endif

namespace spectre {

namespace {

// One bit per gdp::VideoCodec a hardware decoder can take.
using CodecSet = unsigned;

CodecSet codec_bit(gdp::VideoCodec codec) {
	switch (codec) {
	case gdp::VideoCodec::H264: return 1u << 0;
	case gdp::VideoCodec::H265: return 1u << 1;
	case gdp::VideoCodec::AV1: return 1u << 2;
	case gdp::VideoCodec::Pyrowave: return 1u << 3;
	case gdp::VideoCodec::Unknown: break;
	}
	return 0;
}

CodecSet vulkan_codecs(VkVideoCodecOperationFlagsKHR ops) {
	CodecSet set = 0;
	if (ops & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
		set |= codec_bit(gdp::VideoCodec::H264);
	}
	if (ops & VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) {
		set |= codec_bit(gdp::VideoCodec::H265);
	}
	if (ops & VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) {
		set |= codec_bit(gdp::VideoCodec::AV1);
	}
	return set;
}

#ifdef SPECTRE_VULKAN_DECODE
// Which of `ops` the decode queue takes at 10 bits: the Main10 and AV1
// profiles asked of vkGetPhysicalDeviceVideoCapabilitiesKHR with 10-bit
// luma and chroma, which is what FFmpeg's Vulkan hwaccel will ask for a
// 10-bit stream. A profile the driver doesn't do is an error from that
// query, not a capability bit, so each is a call of its own.
CodecSet vulkan_ten_bit_codecs(VkInstance instance, VkPhysicalDevice device,
	VkVideoCodecOperationFlagsKHR ops) {
	auto get_caps = (PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR)vkGetInstanceProcAddr(instance,
		"vkGetPhysicalDeviceVideoCapabilitiesKHR");
	if (!get_caps) {
		return 0;
	}
	CodecSet set = 0;
	VkVideoProfileInfoKHR profile{};
	profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
	profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
	profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;

	if (ops & VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) {
		VkVideoDecodeH265ProfileInfoKHR h265{};
		h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
		h265.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN_10;
		VkVideoDecodeH265CapabilitiesKHR h265_caps{};
		h265_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR;
		VkVideoDecodeCapabilitiesKHR decode_caps{};
		decode_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
		decode_caps.pNext = &h265_caps;
		VkVideoCapabilitiesKHR caps{};
		caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
		caps.pNext = &decode_caps;
		profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
		profile.pNext = &h265;
		if (get_caps(device, &profile, &caps) == VK_SUCCESS) {
			set |= codec_bit(gdp::VideoCodec::H265);
		}
	}
	if (ops & VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) {
		VkVideoDecodeAV1ProfileInfoKHR av1{};
		av1.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR;
		av1.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
		av1.filmGrainSupport = VK_FALSE;
		VkVideoDecodeAV1CapabilitiesKHR av1_caps{};
		av1_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR;
		VkVideoDecodeCapabilitiesKHR decode_caps{};
		decode_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
		decode_caps.pNext = &av1_caps;
		VkVideoCapabilitiesKHR caps{};
		caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
		caps.pNext = &decode_caps;
		profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
		profile.pNext = &av1;
		if (get_caps(device, &profile, &caps) == VK_SUCCESS) {
			set |= codec_bit(gdp::VideoCodec::AV1);
		}
	}
	return set;
}
#endif

#ifdef _WIN32
// D3D11VA on the adapter behind `device`, as Decoder::create_native_device()
// would open it: the decoder profiles FFmpeg's d3d11va hwaccels use, each
// with the NV12 output they decode to -- and, into `ten_bit`, the Main10
// / AV1 profiles with a P010 output.
CodecSet native_codecs(VkPhysicalDevice device, CodecSet *ten_bit) {
	*ten_bit = 0;
	VkPhysicalDeviceIDProperties id_props{};
	id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
	VkPhysicalDeviceProperties2 props2{};
	props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	props2.pNext = &id_props;
	vkGetPhysicalDeviceProperties2(device, &props2);

	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory))) {
		return 0;
	}
	IDXGIAdapter1 *match = nullptr;
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT i = 0; !match && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 desc{};
		if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
			memcmp(&desc.AdapterLuid, id_props.deviceLUID, sizeof(desc.AdapterLuid)) == 0) {
			match = adapter;
		} else {
			adapter->Release();
		}
	}
	factory->Release();
	if (!match) {
		return 0;
	}

	ID3D11Device *d3d = nullptr;
	HRESULT hr = D3D11CreateDevice(match, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
		nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
	match->Release();
	if (FAILED(hr)) {
		return 0;
	}
	ID3D11VideoDevice *video = nullptr;
	hr = d3d->QueryInterface(__uuidof(ID3D11VideoDevice), (void **)&video);
	d3d->Release();
	if (FAILED(hr)) {
		return 0;
	}

	struct Profile {
		const GUID *guid;
		gdp::VideoCodec codec;
		DXGI_FORMAT output;
		bool ten_bit;
	};
	static const Profile kProfiles[] = {
		{&D3D11_DECODER_PROFILE_H264_VLD_NOFGT, gdp::VideoCodec::H264, DXGI_FORMAT_NV12, false},
		{&D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, gdp::VideoCodec::H265, DXGI_FORMAT_NV12, false},
		{&D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, gdp::VideoCodec::AV1, DXGI_FORMAT_NV12, false},
		{&D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10, gdp::VideoCodec::H265, DXGI_FORMAT_P010, true},
		{&D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, gdp::VideoCodec::AV1, DXGI_FORMAT_P010, true},
	};
	CodecSet set = 0;
	UINT count = video->GetVideoDecoderProfileCount();
	for (UINT i = 0; i < count; i++) {
		GUID guid{};
		if (FAILED(video->GetVideoDecoderProfile(i, &guid))) {
			continue;
		}
		for (const Profile &p : kProfiles) {
			BOOL supported = FALSE;
			if (guid == *p.guid && SUCCEEDED(video->CheckVideoDecoderFormat(&guid, p.output, &supported)) &&
				supported) {
				if (p.ten_bit) {
					*ten_bit |= codec_bit(p.codec);
				} else {
					set |= codec_bit(p.codec);
				}
			}
		}
	}
	video->Release();
	// 10-bit only counts where the 8-bit profile is there too: the offer
	// lists codecs, and the depth is a refinement of one.
	*ten_bit &= set;
	return set;
}
#elif defined(__APPLE__)
// VideoToolbox, as Decoder::create_native_device() would open it: FFmpeg's
// hwaccel asks for a hardware decoder and fails over to software decode
// without one, so only what the Mac's media engine does counts. (Every Mac
// decodes H.264 in hardware; HEVC needs a 2015+ Intel GPU or Apple
// silicon; AV1 an M3 or later.)
//
// 10 bits: VideoToolbox has no per-profile query. Every Apple silicon
// media engine decodes HEVC Main10 (and AV1 10-bit where it decodes AV1 at
// all, M3 and later); Intel Macs' HEVC is Main-only before Kaby Lake and
// hard to tell apart, so they stay at 8.
CodecSet native_codecs(CodecSet *ten_bit) {
	struct Codec {
		CMVideoCodecType type;
		gdp::VideoCodec codec;
	};
	static const Codec kCodecs[] = {
		{kCMVideoCodecType_H264, gdp::VideoCodec::H264},
		{kCMVideoCodecType_HEVC, gdp::VideoCodec::H265},
		// kCMVideoCodecType_AV1 only exists in the macOS 14 SDK and later.
		{'av01', gdp::VideoCodec::AV1},
	};
	CodecSet set = 0;
	*ten_bit = 0;
	for (const Codec &c : kCodecs) {
		if (VTIsHardwareDecodeSupported(c.type)) {
			set |= codec_bit(c.codec);
#if defined(__aarch64__)
			if (c.codec != gdp::VideoCodec::H264) {
				*ten_bit |= codec_bit(c.codec);
			}
#endif
		}
	}
	return set;
}
#else
// VA-API on `drm_render_node`, as Decoder::create_native_device() would open
// it: a VLD (decode) entrypoint on one of the profiles wraith encodes to.
// Into `ten_bit`: the same at 10 bits -- HEVC's own Main10 profile, and
// AV1 Profile0 (which covers both depths) when its decoder takes 10-bit
// 4:2:0 surfaces (VAConfigAttribRTFormat), as FFmpeg's vaapi hwaccel
// checks before decoding into P010.
CodecSet native_codecs(const char *drm_render_node, CodecSet *ten_bit) {
	*ten_bit = 0;
	int fd = open(drm_render_node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		return 0;
	}
	CodecSet set = 0;
	VADisplay display = vaGetDisplayDRM(fd);
	int major = 0;
	int minor = 0;
	if (display && vaInitialize(display, &major, &minor) == VA_STATUS_SUCCESS) {
		struct Profile {
			VAProfile profile;
			gdp::VideoCodec codec;
			// The surface format the depth needs of this profile's decoder:
			// 0 for the 8-bit entries (any VLD entrypoint does NV12).
			unsigned rt_format;
			bool ten_bit;
		};
		static const Profile kProfiles[] = {
			{VAProfileH264ConstrainedBaseline, gdp::VideoCodec::H264, 0, false},
			{VAProfileH264Main, gdp::VideoCodec::H264, 0, false},
			{VAProfileH264High, gdp::VideoCodec::H264, 0, false},
			{VAProfileHEVCMain, gdp::VideoCodec::H265, 0, false},
			{VAProfileAV1Profile0, gdp::VideoCodec::AV1, 0, false},
			{VAProfileHEVCMain10, gdp::VideoCodec::H265, VA_RT_FORMAT_YUV420_10, true},
			{VAProfileAV1Profile0, gdp::VideoCodec::AV1, VA_RT_FORMAT_YUV420_10, true},
		};
		int profile_count = vaMaxNumProfiles(display);
		std::vector<VAProfile> profiles((size_t)profile_count);
		std::vector<VAEntrypoint> entrypoints((size_t)vaMaxNumEntrypoints(display));
		if (vaQueryConfigProfiles(display, profiles.data(), &profile_count) == VA_STATUS_SUCCESS) {
			for (int i = 0; i < profile_count; i++) {
				for (const Profile &p : kProfiles) {
					if (profiles[i] != p.profile) {
						continue;
					}
					int entrypoint_count = 0;
					if (vaQueryConfigEntrypoints(display, p.profile, entrypoints.data(), &entrypoint_count) !=
						VA_STATUS_SUCCESS) {
						continue;
					}
					bool vld = false;
					for (int e = 0; e < entrypoint_count; e++) {
						vld = vld || entrypoints[e] == VAEntrypointVLD;
					}
					if (!vld) {
						continue;
					}
					if (p.rt_format != 0) {
						VAConfigAttrib attrib{};
						attrib.type = VAConfigAttribRTFormat;
						if (vaGetConfigAttributes(display, p.profile, VAEntrypointVLD, &attrib, 1) !=
								VA_STATUS_SUCCESS ||
							attrib.value == VA_ATTRIB_NOT_SUPPORTED || !(attrib.value & p.rt_format)) {
							continue;
						}
					}
					if (p.ten_bit) {
						*ten_bit |= codec_bit(p.codec);
					} else {
						set |= codec_bit(p.codec);
					}
				}
			}
		}
		vaTerminate(display);
	}
	close(fd);
	*ten_bit &= set;
	return set;
}
#endif

// The hardware half: what Vulkan Video and the platform decoder can each
// take on the GPU the presenter will land on, at 8 bits and (`ten_bit`,
// the union over the paths) at 10. Mirrors the gates
// StreamSession::open_window() and Decoder::open() apply to the real thing.
void probe_hardware(const char *drm_render_node, CodecSet *vulkan, CodecSet *native, CodecSet *compute,
	CodecSet *ten_bit) {
	*vulkan = 0;
	*native = 0;
	*compute = 0;
	*ten_bit = 0;

	VkApplicationInfo app_info{};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = "spectre";
	app_info.apiVersion = VK_API_VERSION_1_3;
	std::vector<const char *> instance_extensions;
	VkInstanceCreateInfo instance_info{};
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.flags = VulkanDevice::portability_instance_setup(&instance_extensions);
	instance_info.pApplicationInfo = &app_info;
	instance_info.enabledExtensionCount = (uint32_t)instance_extensions.size();
	instance_info.ppEnabledExtensionNames = instance_extensions.data();
	VkInstance instance = VK_NULL_HANDLE;
	if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
		SLOG_INFO("codec_support: no Vulkan instance, assuming software decode only");
		return;
	}

	bool matched_render_node = false;
	VkPhysicalDevice device =
		VulkanDevice::choose_physical_device(instance, drm_render_node, &matched_render_node);
	if (device) {
#ifdef SPECTRE_HAVE_PYROWAVE
		// PyroWave's compute decode (pyrowave_decode.hpp), whose gate
		// VulkanDevice::create_device() applies too. 10 bits is a 16-bit
		// plane format the GPU has to sample and write.
		if (VulkanDevice::supports_pyrowave(device)) {
			*compute = codec_bit(gdp::VideoCodec::Pyrowave);
			if (VulkanDevice::supports_pyrowave(device, nullptr, true)) {
				*ten_bit |= codec_bit(gdp::VideoCodec::Pyrowave);
			}
		}
#endif
#ifdef SPECTRE_VULKAN_DECODE
		uint32_t decode_family = 0;
		VkVideoCodecOperationFlagsKHR decode_ops = 0;
		std::vector<const char *> extensions;
		if (VulkanDevice::probe_video_decode(device, &decode_family, &decode_ops, &extensions)) {
			*vulkan = vulkan_codecs(decode_ops);
			*ten_bit |= vulkan_ten_bit_codecs(instance, device, decode_ops);
		}
#endif
		if (VulkanDevice::can_import_native_frames(device, matched_render_node)) {
			CodecSet native_ten_bit = 0;
#if defined(_WIN32)
			*native = native_codecs(device, &native_ten_bit);
#elif defined(__APPLE__)
			*native = native_codecs(&native_ten_bit);
#else
			// NVIDIA has no VA-API of its own (see open_window()).
			VkPhysicalDeviceProperties props{};
			vkGetPhysicalDeviceProperties(device, &props);
			constexpr uint32_t kNvidiaVendorId = 0x10de;
			if (props.vendorID != kNvidiaVendorId) {
				*native = native_codecs(drm_render_node, &native_ten_bit);
			}
#endif
			*ten_bit |= native_ten_bit;
		}
	}
	vkDestroyInstance(instance, nullptr);
}

// spectre -X's token for the platform decoder.
#if defined(_WIN32)
constexpr const char *kNativeDecodeToken = "d3d11va";
#elif defined(__APPLE__)
constexpr const char *kNativeDecodeToken = "videotoolbox";
#else
constexpr const char *kNativeDecodeToken = "vaapi";
#endif

} // namespace

bool v4l2_m2m_decodes(gdp::VideoCodec codec) {
#if defined(__linux__)
	uint32_t want = codec == gdp::VideoCodec::H264 ? V4L2_PIX_FMT_H264
		: codec == gdp::VideoCodec::H265           ? V4L2_PIX_FMT_HEVC
												   : 0;
	if (!want) {
		return false;
	}
	for (int n = 0; n < 64; n++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/video%d", n);
		int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			continue;
		}
		v4l2_capability cap{};
		bool found = false;
		if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
			uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
			// The coded side is the OUTPUT queue of a memory-to-memory device.
			for (v4l2_buf_type type : {V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_BUF_TYPE_VIDEO_OUTPUT}) {
				bool m2m = type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? (caps & V4L2_CAP_VIDEO_M2M_MPLANE)
																	 : (caps & V4L2_CAP_VIDEO_M2M);
				v4l2_fmtdesc desc{};
				desc.type = type;
				while (m2m && !found && ioctl(fd, VIDIOC_ENUM_FMT, &desc) == 0) {
					found = desc.pixelformat == want;
					desc.index++;
				}
			}
		}
		close(fd);
		if (found) {
			return true;
		}
	}
	return false;
#else
	(void)codec;
	return false;
#endif
}

std::vector<std::string> probe_decode_paths(const char *drm_render_node) {
	CodecSet vulkan = 0;
	CodecSet native = 0;
	CodecSet compute = 0;
	CodecSet ten_bit = 0;
	probe_hardware(drm_render_node, &vulkan, &native, &compute, &ten_bit);
	std::vector<std::string> result;
	for (const std::string &token : gdp::all_video_codec_tokens()) {
		if (!Decoder::can_decode(token)) {
			continue;
		}
		CodecSet bit = codec_bit(gdp::video_codec_from_token(token));
		if (vulkan & bit) {
			result.push_back("vulkan:" + token);
		}
		if (native & bit) {
			result.push_back(std::string(kNativeDecodeToken) + ":" + token);
		}
		if (compute & bit) {
			result.push_back("compute:" + token);
		}
		if (v4l2_m2m_decodes(gdp::video_codec_from_token(token))) {
			result.push_back("v4l2:" + token);
		}
		if (Decoder::can_decode_in_software(token)) {
			result.push_back("software:" + token);
		}
	}
	return result;
}

DecodableCodecs probe_decodable_codecs(DecodeBackend backend, const char *drm_render_node) {
	CodecSet vulkan = 0;
	CodecSet native = 0;
	CodecSet compute = 0;
	CodecSet ten_bit = 0;
	if (backend != DecodeBackend::Software) {
		probe_hardware(drm_render_node, &vulkan, &native, &compute, &ten_bit);
	}

	DecodableCodecs result;
	for (const std::string &token : gdp::all_video_codec_tokens()) {
		// Every path decodes through FFmpeg's decoder for the codec, the
		// hardware ones included (as its hwaccel) -- except pyrowave, which
		// is its own (can_decode() says whether it's built in).
		if (!Decoder::can_decode(token)) {
			continue;
		}
		gdp::VideoCodec codec = gdp::video_codec_from_token(token);
		CodecSet bit = codec_bit(codec);
		// Software decode only ever covers h264 (see can_decode_in_software()).
		bool software = Decoder::can_decode_in_software(token);
		bool on_vulkan = (vulkan & bit) != 0;
		bool on_native = (native & bit) != 0;
		bool on_compute = (compute & bit) != 0;
		bool on_v4l2 = backend != DecodeBackend::Software && v4l2_m2m_decodes(codec);
		bool on_ten_bit = (ten_bit & bit) != 0;
		SLOG_DEBUG("codec_support: %s -- software %s, vulkan %s, %s %s, compute %s, v4l2 %s, 10-bit %s",
			token.c_str(), software ? "yes" : "no", on_vulkan ? "yes" : "no", kNativeDecodeName,
			on_native ? "yes" : "no", on_compute ? "yes" : "no", on_v4l2 ? "yes" : "no",
			on_ten_bit ? "yes" : "no");
		if (software || on_vulkan || on_native || on_compute || on_v4l2) {
			result.codecs.push_back(token);
			if (on_ten_bit) {
				result.ten_bit.push_back(token);
			}
		}
	}
	return result;
}

} // namespace spectre
