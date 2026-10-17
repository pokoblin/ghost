# File transfer

Files both ways behind the `file` capability (gdp-spec.md §7.12): the
client offers each of its files on an additional stream of its own
(§2.2), the host accepts it into the user's download directory; and the
host lists the files its user copies, which the client fetches the same
way, one stream per file. Code: libgdp's additional streams
(`transport.hpp`, `src/transport/quic_conn.cpp`) and `file_transfer.hpp`,
which both ends share; `host/wraith/src/session/file_receiver.*` and
`file_sender.*` on the host, and the clipboard sink
(`session/clipboard_sync.*`) for what was copied.

The client opens every stream, both ways (a fetch is a request on a
stream of the client's), so the host never opens streams and the client
keeps granting none.

## Additional streams in libgdp

The transport refused every stream but control and input until this.
What it does now:

- **Credit.** A host grants nothing beyond control and input until
  `allow_streams()`, called once `file` is in effect; the client's
  `open_stream()` waits for that credit like any QUIC stream. The grant
  counts streams open at once: each one that closes is replaced
  (`ngtcp2_conn_extend_max_streams_bidi` from `stream_close`), since
  `MAX_STREAMS` is cumulative and ngtcp2 only replaces streams it never
  reported open.
- **Receive window.** Control and input hand their flow-control credit
  back on receipt, as before. An additional stream hands back its
  stream-level credit only through `Stream::consume()`, so a reader
  slower than the network (a disk) holds the sender back instead of
  filling memory. Connection-level credit still comes back on receipt for
  every stream: with it tied to `consume()` too, four stalled file
  streams could use up the 4 MiB connection window and block the
  client's StatsReports and key presses.
- **Order.** The write loop takes the lowest stream ID with data, so
  control (0) and input (4) always go ahead of file data (8 on).
- **End of a stream.** `on_fin`, `on_reset` and `on_acked` (coalesced to
  one event per dispatch) are reported for additional streams. Dropping a
  `Stream` resets it, except one whose own direction already ended with a
  FIN: that one is let go gracefully, so a last `FileReply` still
  arrives, and the peer is only asked to stop sending. `reset()` cancels.
- **IDs.** Client stream IDs are predicted on the host thread (8, 12,
  ...) and every open goes through the same queue in order. A stream
  cancelled while still waiting for credit is opened anyway when its
  turn comes and reset at once, or every stream queued behind it would
  get the wrong ID.

ngtcp2 1.16 (Ubuntu 26.04's) has no callback for a received
`STOP_SENDING`. A stream that closes with an application error code the
application hasn't heard about yet is reported through `on_reset` when it
closes: that is how the host learns that a client cancelled a fetch whose
request direction had already ended with its FIN (a `RESET_STREAM` after
a FIN changes nothing on the receiving side).

## The host's receiver

`FileReceiver` is made per session when `file` is in effect, and
destroyed with it.

- **Where.** `[files] directory` in wraith.toml, else
  `XDG_DOWNLOAD_DIR` from `user-dirs.dirs`, else `~/Downloads`; it must
  exist and be writable, or the capability isn't offered. Settled as the
  client attaches.
- **Writing.** Off the event loop, on one writer thread per session:
  the event loop also encodes the desktop, and a slow or full disk must
  not stall it. Received bytes are gathered into 256 KiB writes; credit
  goes back to the client only once a write is done. A cancelled
  transfer's file is closed and deleted by whichever of the event loop
  and the writer lets go of it last.
- **Never over an existing file.** The file is written as
  `.ghost-receiving-XXXXXX` (`mkostemp`) in the target directory, then
  renamed into place with `RENAME_NOREPLACE`, trying `name (1).ext`,
  `name (2).ext` ... while the name is taken. Without
  `RENAME_NOREPLACE` (some network file systems) a hard link does the
  same; without links (FAT) a check and a plain rename. A temporary
  file left by a crash is hidden, and the next session doesn't clean it
  up.
- **Telling the client.** `saved` carries the name the file got and the
  folder, as an absolute path, so the client can say where to find it.
- **Permissions and time.** `0666 & ~umask`, like any new file; the
  modification time from the offer.
- **Space.** The offer is refused unless the file plus 64 MiB fits.
- **Notification.** One per batch through
  `org.freedesktop.Notifications`, a second after the last transfer of
  the batch ends ("Received report.pdf", "Received 3 files"), sent
  without waiting for the reply.
- **Protocol errors** (a malformed or oversized opening frame, a byte
  before "accepted") close the connection with `MALFORMED_FRAME` or
  `FRAME_TOO_LARGE`, from an idle callback: the receiver finds out
  inside a stream callback, where the session can't be torn down.

## Copied files, to the client

- **What was copied.** Both clipboard backends read
  `x-special/gnome-copied-files` (Nautilus, first line "copy" or "cut")
  or `text/uri-list` ahead of any text when the selection offers it, and
  the sink turns local `file://` URIs into paths (`copied_file_paths()`
  in libgdp). With a session that can list them, the paths go out as a
  `FileList` and not as text; without one (no `file`, or `[files] send =
  false`) they go as their text, one a line, as before. Copying text
  afterwards takes the list back with an empty one.
- **The list.** `FileSender::set_list()` stats each path: folders are
  listed but flagged, anything else that isn't a regular file (devices,
  pipes) or has an unusable name is left out, at most 1000 entries. A new
  list makes the old one unfetchable; a fetch already going keeps going.
- **Sending.** A request is answered from the latest list by index:
  the file is opened then, and `FileSending` carries its size at that
  moment. A thread of its own reads 256 KiB at a time (`pread`), so a
  cold disk doesn't stall the encoder. A file that comes up short while
  being read is reset rather than sent short.
- **Pacing.** File data competes with the video for the downlink.
  Nothing more is read while the video's send queue has a backlog
  (`GdpSession::send_queue_clear()`, retried every 10 ms), and the bytes in
  flight follow `FileWindow`: LEDBAT-like, growing while the RTT stays
  within 25 ms of the lowest seen and halving (once a round trip) above
  it, between 256 KiB and 16 MiB. The client's uploads use the same rule.

## Limitations

- No folders either way, no resumption: a connection lost mid-transfer
  loses the partial file.
- File data and the client's StatsReports share the client's uplink. A
  client that sends faster than its uplink carries delays its reports
  and makes the host's rate control think the path got worse; pacing is
  the client's job.
