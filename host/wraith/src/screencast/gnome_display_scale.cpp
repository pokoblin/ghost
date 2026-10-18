// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/gnome_display_scale.hpp"

#include "screencast/display_scale.hpp"
#include "screencast/sdbus_util.hpp"

#include <systemd/sd-bus.h>

#include "util/log.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace wraith {

namespace {
constexpr const char *kDcService = "org.gnome.Mutter.DisplayConfig";
constexpr const char *kDcPath = "/org/gnome/Mutter/DisplayConfig";
constexpr const char *kDcIface = "org.gnome.Mutter.DisplayConfig";

// What mutter calls every RecordVirtual monitor (meta-virtual-monitor.c).
constexpr const char *kVirtualVendor = "MetaVendor";
constexpr const char *kVirtualProduct = "Virtual remote monitor";

// ApplyMonitorsConfig's method: 0 verify, 1 temporary, 2 persistent.
constexpr uint32_t kMethodTemporary = 1;

struct Monitor {
	std::string connector;
	std::string vendor;
	std::string product;
	std::string serial;
	std::string mode_id; // the current mode's; empty if none is current
	std::vector<double> scales;
};

struct LogicalMonitor {
	int32_t x = 0;
	int32_t y = 0;
	double scale = 1;
	uint32_t transform = 0;
	int primary = 0;
	std::vector<std::string> connectors;
};

struct DisplayState {
	uint32_t serial = 0;
	std::vector<Monitor> monitors;
	std::vector<LogicalMonitor> logical_monitors;
	bool supports_changing_layout_mode = false;
	uint32_t layout_mode = 0;
};

std::string monitor_id(const Monitor &monitor) {
	return monitor.connector + "/" + monitor.serial;
}

// Walks the a{sv} the message is at. `entry(key)` reads the value's
// variant itself and returns > 0, or returns 0 to have it skipped.
template <typename Entry> int read_properties(sd_bus_message *m, Entry entry) {
	int r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
	if (r <= 0) {
		return r < 0 ? r : -EINVAL;
	}
	while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
		const char *key = nullptr;
		r = sd_bus_message_read(m, "s", &key);
		r = r >= 0 ? entry(key) : r;
		r = r == 0 ? sd_bus_message_skip(m, "v") : r;
		r = r >= 0 ? sd_bus_message_exit_container(m) : r;
		if (r < 0) {
			return r;
		}
	}
	return r < 0 ? r : sd_bus_message_exit_container(m);
}

// One a(siiddada{sv}) entry: id, width, height, refresh rate, preferred
// scale, supported scales, properties ("is-current").
int read_mode(sd_bus_message *m, Monitor *monitor) {
	const char *id = nullptr;
	int32_t width = 0;
	int32_t height = 0;
	double refresh = 0;
	double preferred = 0;
	int r = sd_bus_message_read(m, "siidd", &id, &width, &height, &refresh, &preferred);
	const void *scales = nullptr;
	size_t scales_size = 0;
	r = r >= 0 ? sd_bus_message_read_array(m, SD_BUS_TYPE_DOUBLE, &scales, &scales_size) : r;
	if (r < 0) {
		return r;
	}
	int current = 0;
	r = read_properties(m, [&](const char *key) {
		return strcmp(key, "is-current") == 0 ? sd_bus_message_read(m, "v", "b", &current) : 0;
	});
	if (r >= 0 && current) {
		monitor->mode_id = id;
		const auto *first = static_cast<const double *>(scales);
		monitor->scales.assign(first, first + scales_size / sizeof(double));
	}
	return r;
}

// One a((ssss)a(siiddada{sv})a{sv}) entry: the monitor spec, its modes,
// its properties.
int read_monitor(sd_bus_message *m, Monitor *monitor) {
	const char *connector = nullptr;
	const char *vendor = nullptr;
	const char *product = nullptr;
	const char *serial = nullptr;
	int r = sd_bus_message_read(m, "(ssss)", &connector, &vendor, &product, &serial);
	if (r < 0) {
		return r;
	}
	monitor->connector = connector;
	monitor->vendor = vendor;
	monitor->product = product;
	monitor->serial = serial;
	r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(siiddada{sv})");
	if (r < 0) {
		return r;
	}
	while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_STRUCT, "siiddada{sv}")) > 0) {
		r = read_mode(m, monitor);
		r = r >= 0 ? sd_bus_message_exit_container(m) : r;
		if (r < 0) {
			return r;
		}
	}
	r = r >= 0 ? sd_bus_message_exit_container(m) : r;
	return r >= 0 ? sd_bus_message_skip(m, "a{sv}") : r;
}

// One a(iiduba(ssss)a{sv}) entry: position, scale, transform, primary,
// the monitor specs in it, properties.
int read_logical_monitor(sd_bus_message *m, LogicalMonitor *logical) {
	int r = sd_bus_message_read(m, "iidub", &logical->x, &logical->y, &logical->scale, &logical->transform,
		&logical->primary);
	r = r >= 0 ? sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(ssss)") : r;
	if (r < 0) {
		return r;
	}
	const char *connector = nullptr;
	const char *vendor = nullptr;
	const char *product = nullptr;
	const char *serial = nullptr;
	while ((r = sd_bus_message_read(m, "(ssss)", &connector, &vendor, &product, &serial)) > 0) {
		logical->connectors.push_back(connector);
	}
	r = r >= 0 ? sd_bus_message_exit_container(m) : r;
	return r >= 0 ? sd_bus_message_skip(m, "a{sv}") : r;
}

int read_state(sd_bus_message *m, DisplayState *state) {
	int r = sd_bus_message_read(m, "u", &state->serial);
	r = r >= 0 ? sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "((ssss)a(siiddada{sv})a{sv})") : r;
	if (r < 0) {
		return r;
	}
	while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_STRUCT, "(ssss)a(siiddada{sv})a{sv}")) > 0) {
		state->monitors.emplace_back();
		r = read_monitor(m, &state->monitors.back());
		r = r >= 0 ? sd_bus_message_exit_container(m) : r;
		if (r < 0) {
			return r;
		}
	}
	r = r >= 0 ? sd_bus_message_exit_container(m) : r;
	r = r >= 0 ? sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(iiduba(ssss)a{sv})") : r;
	if (r < 0) {
		return r;
	}
	while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_STRUCT, "iiduba(ssss)a{sv}")) > 0) {
		state->logical_monitors.emplace_back();
		r = read_logical_monitor(m, &state->logical_monitors.back());
		r = r >= 0 ? sd_bus_message_exit_container(m) : r;
		if (r < 0) {
			return r;
		}
	}
	r = r >= 0 ? sd_bus_message_exit_container(m) : r;
	if (r < 0) {
		return r;
	}
	return read_properties(m, [&](const char *key) {
		if (strcmp(key, "layout-mode") == 0) {
			return sd_bus_message_read(m, "v", "u", &state->layout_mode);
		}
		if (strcmp(key, "supports-changing-layout-mode") == 0) {
			int supports = 0;
			int r = sd_bus_message_read(m, "v", "b", &supports);
			state->supports_changing_layout_mode = supports;
			return r;
		}
		return 0;
	});
}

bool get_current_state(sd_bus *bus, DisplayState *state) {
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = nullptr;
	int r = sd_bus_call_method(bus, kDcService, kDcPath, kDcIface, "GetCurrentState", &error, &reply, "");
	if (r < 0) {
		WLOG_ERROR("screencast: DisplayConfig.GetCurrentState failed: %s", sdbus_error_text(error, r));
		sd_bus_error_free(&error);
		return false;
	}
	sd_bus_error_free(&error);
	r = read_state(reply, state);
	sd_bus_message_unref(reply);
	if (r < 0) {
		WLOG_ERROR("screencast: DisplayConfig.GetCurrentState reply didn't parse: %s", strerror(-r));
		return false;
	}
	return true;
}

// The whole current configuration with `scaled`'s scale changed: every
// logical monitor left out of ApplyMonitorsConfig would be disabled.
int append_config(sd_bus_message *m, const DisplayState &state, const LogicalMonitor *scaled, double scale) {
	int r = sd_bus_message_append(m, "uu", state.serial, kMethodTemporary);
	r = r >= 0 ? sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "(iiduba(ssa{sv}))") : r;
	for (const LogicalMonitor &logical : state.logical_monitors) {
		r = r >= 0 ? sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "iiduba(ssa{sv})") : r;
		r = r >= 0 ? sd_bus_message_append(m, "iidub", logical.x, logical.y,
						 &logical == scaled ? scale : logical.scale, logical.transform, logical.primary)
				   : r;
		r = r >= 0 ? sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "(ssa{sv})") : r;
		for (const std::string &connector : logical.connectors) {
			const Monitor *monitor = nullptr;
			for (const Monitor &candidate : state.monitors) {
				if (candidate.connector == connector) {
					monitor = &candidate;
				}
			}
			if (!monitor || monitor->mode_id.empty()) {
				return -ENODEV;
			}
			r = r >= 0 ? sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "ssa{sv}") : r;
			r = r >= 0 ? sd_bus_message_append(m, "ss", connector.c_str(), monitor->mode_id.c_str()) : r;
			r = r >= 0 ? sdbus_append_no_options(m) : r;
			r = r >= 0 ? sd_bus_message_close_container(m) : r; // (ssa{sv})
		}
		r = r >= 0 ? sd_bus_message_close_container(m) : r; // a(ssa{sv})
		r = r >= 0 ? sd_bus_message_close_container(m) : r; // (iiduba(ssa{sv}))
	}
	r = r >= 0 ? sd_bus_message_close_container(m) : r;
	if (r < 0) {
		return r;
	}
	// Passed on unchanged where it can be set at all: mutter refuses a
	// layout-mode it doesn't let change.
	if (!state.supports_changing_layout_mode) {
		return sdbus_append_no_options(m);
	}
	return sdbus_append_one_option(m, "layout-mode", "u",
		[&](sd_bus_message *msg) { return sd_bus_message_append(msg, "u", state.layout_mode); });
}

bool apply_scale(sd_bus *bus, const DisplayState &state, const LogicalMonitor *scaled, double scale) {
	sd_bus_message *m = nullptr;
	int r = sd_bus_message_new_method_call(bus, &m, kDcService, kDcPath, kDcIface, "ApplyMonitorsConfig");
	r = r >= 0 ? append_config(m, state, scaled, scale) : r;
	if (r < 0) {
		WLOG_ERROR("screencast: building DisplayConfig.ApplyMonitorsConfig failed: %s", strerror(-r));
		sd_bus_message_unref(m);
		return false;
	}
	sd_bus_error error = SD_BUS_ERROR_NULL;
	r = sd_bus_call(bus, m, 0, &error, nullptr);
	sd_bus_message_unref(m);
	if (r < 0) {
		WLOG_ERROR("screencast: DisplayConfig.ApplyMonitorsConfig(scale %g) failed: %s", scale,
			sdbus_error_text(error, r));
	}
	sd_bus_error_free(&error);
	return r >= 0;
}

bool is_virtual_monitor(const Monitor &monitor) {
	return monitor.vendor == kVirtualVendor && monitor.product == kVirtualProduct;
}

} // namespace

struct GnomeDisplayScale::Impl {
	sd_bus *bus = nullptr;
	sd_bus_slot *monitors_changed_slot = nullptr;
	// Virtual monitors that were there before this session started.
	std::set<std::string> not_ours;
	std::string state_path;
	ScaleMemory memory{0};

	static int on_monitors_changed(sd_bus_message *, void *data, sd_bus_error *) {
		static_cast<Impl *>(data)->update();
		return 0;
	}

	void update() {
		DisplayState state;
		if (!get_current_state(bus, &state)) {
			return;
		}
		const Monitor *ours = nullptr;
		for (const Monitor &monitor : state.monitors) {
			if (is_virtual_monitor(monitor) && !not_ours.count(monitor_id(monitor))) {
				ours = &monitor;
				break;
			}
		}
		if (!ours) {
			return; // not made yet (capture not consumed), or gone
		}
		const LogicalMonitor *logical = nullptr;
		for (const LogicalMonitor &candidate : state.logical_monitors) {
			for (const std::string &connector : candidate.connectors) {
				if (connector == ours->connector) {
					logical = &candidate;
				}
			}
		}
		if (!logical) {
			return; // disabled
		}

		std::string id = monitor_id(*ours);
		ScaleMemory::Decision decision = memory.observe(id, logical->scale, ours->scales);
		switch (decision.action) {
		case ScaleMemory::Action::None: break;
		case ScaleMemory::Action::Apply:
			if (apply_scale(bus, state, logical, decision.scale)) {
				WLOG_INFO("screencast: %s set to scale %g (remembered %g)", id.c_str(), decision.scale,
					memory.remembered());
			} else {
				memory.apply_failed(logical->scale);
			}
			break;
		case ScaleMemory::Action::Remember:
			if (save_scale(state_path, decision.scale)) {
				WLOG_INFO("screencast: %s scale changed to %g, remembered in %s", id.c_str(), decision.scale,
					state_path.c_str());
			} else {
				WLOG_ERROR("screencast: %s scale changed to %g, but writing %s failed: %s", id.c_str(),
					decision.scale, state_path.empty() ? "(no HOME)" : state_path.c_str(), strerror(errno));
			}
			break;
		}
	}
};

GnomeDisplayScale::GnomeDisplayScale(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

GnomeDisplayScale::~GnomeDisplayScale() {
	if (impl_->monitors_changed_slot) {
		sd_bus_slot_unref(impl_->monitors_changed_slot);
	}
}

std::unique_ptr<GnomeDisplayScale> GnomeDisplayScale::create(struct sd_bus *bus) {
	auto impl = std::make_unique<Impl>();
	impl->bus = bus;
	impl->state_path = scale_state_path(getenv("XDG_STATE_HOME"), getenv("HOME"));
	double remembered = 0;
	if (load_scale(impl->state_path, &remembered)) {
		impl->memory = ScaleMemory(remembered);
		WLOG_INFO("screencast: remembered display scale %g (%s)", remembered, impl->state_path.c_str());
	}

	DisplayState state;
	if (get_current_state(bus, &state)) {
		for (const Monitor &monitor : state.monitors) {
			if (is_virtual_monitor(monitor)) {
				impl->not_ours.insert(monitor_id(monitor));
			}
		}
	}

	int r = sd_bus_match_signal(bus, &impl->monitors_changed_slot, kDcService, kDcPath, kDcIface,
		"MonitorsChanged", Impl::on_monitors_changed, impl.get());
	if (r < 0) {
		WLOG_ERROR("screencast: subscribing to DisplayConfig.MonitorsChanged failed: %s", strerror(-r));
		return nullptr;
	}
	return std::unique_ptr<GnomeDisplayScale>(new GnomeDisplayScale(std::move(impl)));
}

} // namespace wraith
