# Encoding

How wraith turns captured frames into a video stream, and which encoder
it picks. Code: `host/wraith/src/encode/`. PyroWave and lossless
refinement have their own pages: [PyroWave](pyrowave.md),
[refinement](refinement.md).

## Codecs

Everything codec-specific lives in four places; the encoder interface,
datagram framing, reassembly, presentation and input are codec-agnostic.

1. **The wire token**, `libgdp/include/gdp/video_codec.hpp`: the enum
   entry, `video_codec_from_token()` / `video_codec_token()`, and the
   token's place in `all_video_codec_tokens()`, which is the preference
   order.
2. **The encoder backend** in `host/wraith/src/encode/`, registered in
   `encoder_factory.cpp`'s table. A row names the codec, the backend,
   whether it is hardware (skipped under `wraith -F` or
   `encode.force_software`) and whether it needs a render node. Rows for
   one codec are tried in order, so "hardware, then software" is two
   rows.
3. **The decoder mapping**, `client/spectre/src/decode/decoder.cpp`'s
   `find_codec()`. Resolving through `avcodec_find_decoder()` keeps
   `can_decode()` honest on an FFmpeg built without a codec.
4. **The spec**, gdp-spec.md §6.6.

| Codec | Token | wraith | spectre |
|---|---|---|---|
| H.264 | `h264` | VA-API or NVENC, falling back to x264 | FFmpeg, hardware or software |
| H.265 | `h265` | VA-API or NVENC; no software fallback | FFmpeg, hardware only |
| AV1 | `av1` | VA-API or NVENC; no software fallback (off by default) | FFmpeg, hardware only |
| PyroWave | `pyrowave` | Vulkan compute (off by default) | Vulkan compute |

`h264` is the default and the universal floor. Both ends intersect their
preference list with what they can do (`supported_video_codecs()` in
wraith, limited by wraith.toml's `[encode.codecs]`;
`Decoder::can_decode()` in spectre), so a build never offers a codec it
can't serve. wraith opens its encoder before any client attaches, on the
default codec, and reopens it for the codec a session negotiates, trying
the client's preferences in order until one opens.

Every backend codes one slice (one tile for AV1) per picture, with no
B-frames, a single reference, and a key frame every `encode.gop` frames
(0 for key frames only on request) or on request, so a session behaves
the same whichever codec it negotiated.

## Colour

H.264, H.265 and AV1 are limited range, with BT.709 when the client
offered the `color` capability and BT.601, the protocol's default,
otherwise (gdp-spec.md §7.3). A client that offers `color` promises to
apply `OutputDescriptor.color`; spectre before 2026-10-14 assumed BT.601
whatever the descriptor said, and the browser client still does, so the
default stays where they expect it. `GdpSession::session_matrix()`
decides, `EncoderConfig::matrix` carries it, and every backend converts
with it: the VA-API VPP's colour standard (both ends, or radeonsi changes
primaries too), NVENC's VUI (NVENC converts with whatever the VUI names),
and `xrgb_to_i420()`'s coefficients for x264 (scalar and AVX2,
byte-identical, `xrgb_convert_test`). Every stream also states its matrix
in-band through `colour_description()` (`encode/colour_description.hpp`):
BT.709 is primaries 1, transfer 13 (sRGB), matrix 1; BT.601 is SMPTE
170M throughout. A decoder left to guess takes HD video as BT.709
(Firefox does), so every encoder writes it: the H.264 and H.265 VUI, the
AV1 sequence header, x264's and NVENC's own. VA-API's H.265 VUI rides in
the packed SPS only (the VA sequence buffer has no colour fields), which
radeonsi keeps. PyroWave is full-range BT.709 whatever the session.

## Bit depth

Hardware encoders reconstruct every block on the 8-bit studio-range grid
(219 levels), so a dark gradient comes back as 1-level contour steps
however the encoder is tuned: on the RX 9070 XT and the N100, VBR, CQP 12
and quality level 1 all landed at the same ~51.5 dB and the same steps
(2026-10-14). The same encoders in Main10 produce a clean ramp from the
same 8-bit source, at the same throughput and within a few percent of the
bitrate. So the depth is encoder precision, not capture precision: the
desktop stays 8-bit, the VPP converts it to P010 instead of NV12, and the
codec carries 10 bits.

The client declares the codecs it decodes at 10 bits in
`SessionHello.codec_formats` (probed, never assumed:
`client/spectre/src/decode/codec_support.cpp`), and
`GdpSession::open_session_encoder()` asks `gdp::negotiate_bit_depth()`
for the deepest declared depth, then opens the encoder at it, falling
back to 8 when no backend opens (`EncoderConfig::bit_depth`). The
factory's table carries each row's maximum (`codec_supports_bit_depth()`):
10 for H.265 and AV1 (VA-API and NVENC) and for PyroWave, 8 for every
H.264 row (no hardware decodes High 10). `[encode] bit_depth = 8`
pins the baseline. `OutputDescriptor.color.bit_depth` says what the
session got, and a resolution change renegotiates the same way. Software
decoders never declare 10, so a GPU-less client stays at 8 at no cost.

VA-API at 10 bits: `VAProfileHEVCMain10` (the profile is Main10's own;
AV1's Profile 0 covers both depths and the encode config's
`VAConfigAttribRTFormat` says whether the entrypoint takes
`VA_RT_FORMAT_YUV420_10`), P010 input and DPB pools, a doubled coded
buffer, `bit_depth_*_minus8 = 2` in the sequence parameters and in the
packed SPS (`general_profile_idc = 2`, `wraith_bitstream_test`), AV1's
`high_bitdepth`. QP fields are unchanged: slice QP is an index, and
`QpBdOffset` is the decoder's business. Verified on the RX 9070 XT
(radeonsi, `EncSlice`): h265 and av1 sessions negotiate 10 bits, ffmpeg
reads the `-e` dump's base stream as `yuv420p10le` and decodes every
frame, spectre decodes it through VA-API (P010 import) and Vulkan Video;
pyrowave negotiates 10 bits and decodes into 16-bit planes. A resize
whose reopen can't get Main10 (forced with an `LD_PRELOAD` shim failing
the second `vaCreateConfig` for it) falls back to 8 bits and tells
spectre so in `DisplaysChanged`. A client on lavapipe (no GPU) declares
h264 at 8 bits only and gets 8-bit BT.709 h264.

## Choosing the render node

On a multi-GPU host, libdrm lists render nodes in readdir order, which
can change across reboots, so "the first node" may be the iGPU on one
boot and the discrete GPU on the next. `preferred_render_node()`
(`util/render_node.cpp`) therefore probes every render node for VA-API
encode entrypoints and NVENC codecs (`hardware_encodable_codecs()`,
using each hardware row's own profile) and takes the node that encodes
the most preferred enabled codec, keeping listed order on a tie.
`WLR_RENDER_DRM_DEVICE` in wraith's environment overrides the probe. The
journal logs what each node can encode and which was chosen
(`render node: ...`).

## VA-API

The three VA-API backends share everything that isn't the codec.
`VaapiEncoderBase` (`encode/vaapi/vaapi_encoder_base.*`) owns the
display, the VPP RGB→NV12 conversion, both surface pools, the dmabuf
import, the CPU upload path and the coded-buffer drain.
`VaapiH264Encoder`, `VaapiHevcEncoder` and `VaapiAv1Encoder` add their
parameter buffers, their header writer (`h264_bitstream.*`,
`hevc_bitstream.*` over the shared RBSP code in `nal_bitstream.*`;
`av1_obu.*` for AV1's OBUs) and their level selection.

Per frame, the captured RGB dmabuf is imported as a VA surface (no copy),
converted to NV12 by VPP, and encoded. The CPU upload path pins its
surface to `VA_FOURCC_BGRX`.

**Input and reconstruction surfaces stay separate.** Input surfaces are
only ever the `vaBeginPicture` render target; `CurrPic` and
`ReferenceFrames` name a dedicated DPB pool. Mesa's
`va/picture_h264_enc.c` turns whatever surface is named as `CurrPic` into
a DPB surface by destroying and reallocating its buffer without updating
the encoder's target, so naming the input surface there leaves the
encoder reading freed memory.

**Rate control** is VBR. The rate-control and frame-rate parameter
buffers are re-sent with every IDR, not only after `open()` and
`set_bitrate()`: radeonsi takes the frame rate from each new sequence
parameter buffer over the misc buffer's, and otherwise budgets every
frame for its default rate, overshooting the target. Macroblock-level
rate control is always off (`mb_rate_control` = 2): iHD's low-power
H.265 encoder has it on by default and raises QP on blocks it rates as
simple, so the dark gradient of a wallpaper came out as flat stepped
squares that no later frame refined; off, it keeps its grain, as
ffmpeg's `hevc_vaapi` (which also sends 2) encodes it. iHD's H.264
encoder and radeonsi encode the same either way.

**H.264.** SPS and PPS are written by wraith. The SPS carries a VUI with
the session's colour description ([colour](#colour)) and `bitstream_restriction`
(no reordering), or some hardware decoders hold several frames before
output. radeonsi also needs a packed slice header
every frame, only to learn `nal_ref_idc` and `nal_unit_type`.

**H.265 on radeonsi.** The driver regenerates the VPS, SPS and PPS it
emits from what it parses out of the packed headers wraith supplies, and
emits none at all for a picture without a packed slice header
(`radeon_vcn_enc_encode_headers` returns early when `num_slices == 0`),
so the slice header is sent even though the driver writes the real one.
The frame is padded to whole 64-px coding tree blocks rather than the
8-px minimum coding block, because the driver otherwise pads to its own
CTB size and emits no conformance window for the difference.

**AV1 on radeonsi.** Mesa takes the whole sequence header and much of
the frame header by parsing the OBUs wraith writes
(`av1_sequence_header()` / `av1_frame_header()` in `picture_av1_enc.c`),
so picture size, colour description, order-hint bits, refresh flags and
reference indices exist only in `av1_obu.*`. The frame header is what
the driver counts as the picture's "slice", and a temporal delimiter OBU
goes ahead of it as the one header copied through verbatim. AV1 has no
cropping syntax and radeonsi rewrites the sequence header with its own
8-aligned size, so the AV1 backend pads to exactly 8 pixels and
replicates the edge into the padding. spectre crops it off using
`SessionAccept`'s display size; any other consumer of the stream sees up
to 7 extra columns or rows.

**Intel (iHD).** The free iHD build offers only the low-power entrypoint
(`EncSliceLP`) for H.264 and H.265; `find_encode_entrypoint()` takes
`EncSlice` where there is one and `EncSliceLP` otherwise, for the encoder
and the render-node probe alike. iHD writes a packed slice header into
the stream as given and appends the hardware's slice data after
`bit_length` bits, so H.264's slice header is sent bit-exact, without
`rbsp_trailing_bits()` (CAVLC slice data isn't byte-aligned). For H.265
the SPS follows what the driver reports: transform depth from
`VAConfigAttribEncHEVCBlockSizes` (iHD allows 2; announcing 3 made every
picture undecodable from its first CTB) and AMP, SAO and strong intra
smoothing from `VAConfigAttribEncHEVCFeatures`. iHD reports
`VA_PREDICTION_DIRECTION_BI_NOT_EMPTY` (no P slices), so P pictures go out
as generalised-P: a P picture whose slices are B, both lists holding the
previous picture, as ffmpeg's `hevc_vaapi` sends them. QP groups are as
small as the smallest coding block (`diff_cu_qp_delta_depth` = CTB to
min CB): the low-power encoder changes QP at that granularity, and with
whole-CTB groups the decoder applied the wrong QP, which shows as ghosts
of older text rather than decode errors. iHD and iris (the GL driver
behind `DmabufReader`) keep separate GEM handle tables, so the reader
opens a DRM file of its own: sharing the encoder's left a refined
session's tile shaders reading blank buffers. iHD (26.1.2 and 26.4.1 at
least) reads an imported linear surface at its pitch rounded up to 64
bytes and doesn't fail the import, so mutter's linear buffers at a width
that isn't a multiple of 16 (`width * 4` bytes a row) came out sheared.
The first time such a pitch turns up, `can_import()` converts a small
frame with one to see whether the driver does this; if it does, or if the
driver won't import the probe's frame at all (radeonsi refuses, though
its own linear pitches are 256-byte aligned so it never sees one from a
compositor), it turns such frames down, and `ScreencastHost` reads them
back through `DmabufReader` and hands them to `push_cpu()` instead.

## NVENC

`encode/nvenc/`: one `NvencEncoder` for all three codecs. NVENC takes the
codec as a GUID and writes every header itself, so none of the VA-API
bitstream code has an equivalent. It is compiled into every build and
costs nothing without NVIDIA hardware: libcuda and libnvidia-encode are
loaded with `dlopen()` through the vendored nv-codec-headers loader
(`host/wraith/third_party/ffnvcodec`, MIT, API 12.2, driver 550.54 or
newer; an older driver logs that and the NVENC rows don't open), and a render node whose PCI vendor isn't NVIDIA is refused before
either library loads. Its rows come after VA-API's for each codec and
don't need a render node; without one, NVENC uses the first CUDA device.

Configuration mirrors VA-API's: preset P4 with ultra-low-latency tuning,
I and P frames only, VBR with the ceiling as both average and maximum and
a one-second VBV, headers on every IDR. A 10-bit session (H.265 Main10
or AV1) keeps the 8-bit input and sets `outputBitDepth` to 10, so NVENC
widens it as VA-API's VPP does into P010. Input is packed RGB, and NVENC
converts it to YUV with the matrix and range the VUI names, BT.709 when
it names none. So the H.264 and H.265 VUI, like AV1's sequence header,
state the session's matrix, limited range ([colour](#colour)); left
unsignalled, the picture decodes darker than the lossless tiles.

Two input paths, fixed at open unless the first fails:

- **Zero-copy** (`push()`): CUDA can't import a dmabuf, so
  `CudaDmabufImporter` goes through Vulkan. The dmabuf is imported as a
  `VkImage` (explicit modifier; its implicit fence waited on through
  `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` and a sync-fd semaphore) and copied
  into a Vulkan buffer that CUDA imports once as external memory and
  NVENC registers. Each capture buffer is imported once and cached by
  its dmabuf inode (at most 8, least recently used evicted), re-imported
  only if its layout changes. The Vulkan device is matched to the CUDA
  device by UUID. If an import fails at runtime, the encoder asks for CPU
  frames from then on.
- **CPU upload** (`push_cpu()`): `cuMemcpy2D` into the same kind of
  buffer. Used when the importer can't start, when
  `encode.nvenc_zero_copy = false`, and under lossless refinement on a
  CPU path.

NVENC is asynchronous, like VA-API and PyroWave: two slots, each with
its own input buffer, registration and bitstream buffer. `push()` copies into a free
slot and submits; a worker thread blocks in `nvEncLockBitstream()` and
signals `completion_fd()`; `poll()` hands finished packets out in order
on the session's thread. `ready_for_frame()` is false while both slots
are busy, so the session skips frames and re-delivers the newest when a
slot frees. A caller that doesn't ask for asynchronous mode gets the
synchronous one.

Consumer GeForce drivers cap concurrent NVENC sessions per system, which
a multi-user host will reach; the open error says so.

## Software

x264 (`encode/software/`), used when no hardware row opens or `-F` /
`encode.force_software` forces it. H.264 only.

- `xrgb_convert.cpp` has an AVX2 XRGB→I420 conversion that must stay
  byte-identical to the scalar path (`xrgb_convert_test`).
- Threads are `SLICED`, not `FRAME`: frame threading breaks the
  per-call pts that `push_cpu()` stamps.
- Preset `superfast`: `ultrafast` drops intra 4x4 prediction, which
  ruins text, and slower presets add latency for no visible gain.
- No 4:4:4: VA-API decoders can't decode it. Every encoder sends 4:2:0
  SDR at 8 or 10 bits ([bit depth](#bit-depth)); the wire can negotiate
  4:4:4 and HDR, but no encoder produces them.

## Limitations

- NVENC at 10 bits has run as H.265 only (an RTX 3070 Ti, driver 615;
  AV1 needs Ada). ffprobe reads the stream as Main 10 `yuv420p10le`
  tagged BT.709. The coded YUV of pure red, blue and grey matches BT.709
  limited range within 2 of 1023, and a released spectre's session
  matches BT.601 exactly, so NVENC's RGB conversion follows the matrix
  the VUI names at both depths. At 1080p60 on moving content 10 bits
  costs nothing measurable: 60 fps, 3.4-3.5 ms encode and ~39 Mbit/s
  at both depths, no skipped frames. The API documents the H.265
  widening as done in software, but the encode time didn't move.
- 10-bit BT.709 has run on two encoders, the RX 9070 XT (radeonsi) and
  an N100 (iHD's low-power Main10 encoder, which needed none of the
  workarounds its 8-bit one did), and on both the picture checked out by
  eye on a client's screen (2026-10-14). Mixed versions meet on 8-bit
  BT.601 both ways: a released spectre against a new wraith, and a new
  spectre against a released wraith. On the N100 at 1080p60, 10 bits
  cost nothing measurable on moving content (61 fps, 44.5 Mbit/s and 0
  skipped frames at both depths, wraith's CPU within 2%). On near-static
  dark content each P-frame is about 600 bytes larger (1.4 against
  1.2 Mbit/s): the extra precision is spent on the dark gradient's
  residual, which is the point. On the client, VA-API and Vulkan
  Video decode on RADV and PyroWave's 16-bit planes (direct and staged)
  have run; D3D11VA and VideoToolbox haven't.
- NVENC has run on one GPU, an RTX 3070 Ti (Ampere): zero-copy and CPU
  upload input, colours, a still desktop's bitrate near zero, resizes,
  4K at about 60 fps once encoding overlapped the next frame's copy, and
  `set_bitrate()` (a 5 Mbit/s throttle: 80 -> 3 Mbps in about 5 s, frames
  34 -> 8 KB, back to full rate once lifted). Not yet seen: AV1, which
  needs Ada or newer (Ampere declines it and the session settles on the
  next codec).
- NVIDIA lowers its clocks under a light load, and NVENC slows with
  them: a 4K frame's encode went from 9 to 17 ms on an RTX 3070 Ti. Pinning clocks
  (`nvidia-smi -lgc`) is the host administrator's choice; wraith doesn't.
- Intel has run on one machine, an N100 (Alder Lake-N, iHD 26.1.2, Mesa
  26.0): H.264 and H.265 on both the non-free driver (`EncSlice`) and the
  free one (`EncSliceLP` only), with refinement, and H.265 Main10 with
  BT.709 on the non-free one. Not yet seen: AV1 on Intel (Arc or Meteor
  Lake and newer), and 10-bit on the free driver's `EncSliceLP`.
- AV1 can't crop, so non-multiple-of-8 sizes carry padding (cropped by
  spectre).
- On a driver that misreads them (iHD 26.1.2 and 26.4.1, at least), a
  VA-API session whose captured frames have a pitch that isn't a
  multiple of 64 bytes (linear buffers at a width that isn't a multiple
  of 16) reads every frame back and uploads it rather than importing
  it: a copy to host memory and back, about 1.5 MB a frame at 824x460.
- Software encode is H.264 only, by decision.
