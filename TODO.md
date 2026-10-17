# TODO

Known gaps, unverified paths and bugs, grouped by area. Design limits
that are accepted rather than open are in each design doc's
Limitations section.

## Login and sessions

- Login latency has not been measured; the target is about 2 s.
- ghostlogin ignores Plasma Login Manager (`plasmalogin`,
  `plasmalogin-autologin` aren't in `DEFAULT_SERVICES`), and outside
  Debian nothing hooks ghostlogin in: docs/install/host.md should say
  which line to add to the display manager's PAM stack.

## Veil

- Gateway sessions are untested with spectre's own feedback loop: the
  netem matrix (loss and delay on each leg, against direct), a 4K
  session's latency, settled bitrate and refinement through the gateway.
- The browser client is untested in Safari.
- The browser client has no microphone: add `getUserMedia` capture and
  Opus through WebCodecs `AudioEncoder` on datagram channel 0x03
  (gdp-spec.md §10.1), offer `microphone`, and a mute control.
- Firefox's VA-API decode ignores the H.264/H.265 crop
  ([browser client](docs/design/browser-client.md#limitations)): find
  the cause or file it upstream, then drop `video.js`'s software-decode
  workaround.

## wraith

- NVENC's AV1 encode (8 and 10 bits) needs an Ada or newer GPU to test.
- KDE: a popup menu (desktop right-click, Konsole's window menu)
  sometimes doesn't appear until the pointer moves over it (KWin 6.7.5,
  NVIDIA). Suspected KWin bug (a repaint dropped when no PipeWire buffer
  is free; draining every queued buffer didn't help): confirm with
  `QT_LOGGING_RULES=kwin_screencast.debug=true`, then report upstream.
- On NVIDIA, a failed GPU read-back in the ext backend retries for good
  (its host-memory fallback needs LINEAR buffers): fall back to wl_shm
  capture instead.
- Per-buffer damage tracking in the ext backend, so the compositor
  copies only what changed.
- Steam Input doesn't work in a session (no `/dev/uinput`). Plan: a
  per-session uinput stand-in from ghostseat (CUSE), bind-mounted over
  `/dev/uinput` by a `user@.service` drop-in. ghostseat creates each
  device itself (`ghost/uid-<uid>/...` phys, handed over by the udev rule,
  raw-controller key policy). Not the `input` group or a uinput ACL
  (ADR 0006).
- A preflight check that turns "this user can't open any DRM node" into a
  clear error instead of a 30 s session-start timeout.

## Client

- Raw controllers (gdp-spec.md §8.6) are untested end to end: a PS4 pad
  over Bluetooth (touchpad, motion, light bar, rumble, Steam Input), a
  Steam Controller, a Switch Pro pad (does `hid-nintendo` pass ghostd's
  key check?), the fallback on `HidRejected`, and spectre on Windows and
  macOS.
- spectre's 10-bit and BT.709 paths are untested beyond RADV: Vulkan
  Video on NVIDIA; on Windows D3D11VA's P010 input, R10G10B10A2 output
  and the software path's BT.709 coefficients; VideoToolbox's P010 copy
  on a Mac.
