// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// GnomeDisplayScale: keeps the scale of GnomeRemoteSession's virtual
// monitor across logins and resizes
// (docs/design/capture-backends.md#display-scale), over mutter's
// `org.gnome.Mutter.DisplayConfig` -- the API GNOME Settings itself uses.
//
//   GetCurrentState() -> (serial, monitors, logical monitors, properties)
//   ApplyMonitorsConfig(serial, method, logical monitors, properties)
//   signal MonitorsChanged()
//
// RecordVirtual makes the monitor only once its PipeWire stream is being
// consumed, about 100 ms after Start() and not at all while capture is
// paused, so nothing here waits for it: every MonitorsChanged is looked
// at, and the first one that shows a new "Virtual remote monitor" is
// where the remembered scale is applied (ScaleMemory,
// screencast/display_scale.hpp). A change seen on it after that is the
// user's (Settings applies it, mutter emits MonitorsChanged) and is
// remembered. The apply is temporary (method 1): it adds nothing to
// monitors.xml. Every failure is only logged; the session runs on at
// whatever scale mutter chose.
#pragma once

#include <memory>

struct sd_bus;

namespace wraith {

class GnomeDisplayScale {
public:
	// Call before RemoteDesktop.Session.Start(): the virtual monitors that
	// already exist then are not this session's. Loads the remembered
	// scale and subscribes to MonitorsChanged on `bus`, which must
	// outlive this object and be processed for the signal to arrive.
	// Null if the subscription fails.
	static std::unique_ptr<GnomeDisplayScale> create(struct sd_bus *bus);
	~GnomeDisplayScale();
	GnomeDisplayScale(const GnomeDisplayScale &) = delete;
	GnomeDisplayScale &operator=(const GnomeDisplayScale &) = delete;

private:
	struct Impl;
	explicit GnomeDisplayScale(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl_;
};

} // namespace wraith
