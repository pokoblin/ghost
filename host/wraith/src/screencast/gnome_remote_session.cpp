// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/gnome_remote_session.hpp"

#include "screencast/ei_input.hpp"
#include "screencast/gnome_clipboard.hpp"
#include "screencast/pipewire_capture.hpp"
#include "screencast/sdbus_util.hpp"
#include "util/clock.hpp"

#include <systemd/sd-bus.h>

#include "util/log.hpp"

#include <wayland-server-core.h>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace wraith {

namespace {
constexpr const char *kRdService = "org.gnome.Mutter.RemoteDesktop";
constexpr const char *kRdPath = "/org/gnome/Mutter/RemoteDesktop";
constexpr const char *kRdIface = "org.gnome.Mutter.RemoteDesktop";
constexpr const char *kRdSessionIface = "org.gnome.Mutter.RemoteDesktop.Session";
constexpr const char *kScService = "org.gnome.Mutter.ScreenCast";
constexpr const char *kScPath = "/org/gnome/Mutter/ScreenCast";
constexpr const char *kScIface = "org.gnome.Mutter.ScreenCast";
constexpr const char *kScSessionIface = "org.gnome.Mutter.ScreenCast.Session";
constexpr const char *kScStreamIface = "org.gnome.Mutter.ScreenCast.Stream";
constexpr const char *kDcService = "org.gnome.Mutter.DisplayConfig";
constexpr const char *kDcPath = "/org/gnome/Mutter/DisplayConfig";
constexpr const char *kDcIface = "org.gnome.Mutter.DisplayConfig";
// ApplyMonitorsConfig's "temporary" method: applied, never written to
// monitors.xml.
constexpr uint32_t kDcTemporary = 1;

// What DisplayConfig.GetCurrentState says about the layout, as far as
// setting one monitor's scale needs.
struct DisplayState {
	uint32_t serial = 0;
	struct Monitor {
		std::string connector;
		std::string mode_id; // the current mode's
		int32_t width = 0, height = 0;
		std::vector<double> scales; // the current mode's supported scales
	};
	std::vector<Monitor> monitors;
	struct Logical {
		int32_t x = 0, y = 0;
		double scale = 1.0;
		uint32_t transform = 0;
		int primary = 0;
		std::vector<std::string> connectors;
	};
	std::vector<Logical> logical;
};

// Reads a{sv} and returns whether `key` is in it with boolean true.
bool read_flag(sd_bus_message *m, const char *key) {
	bool found = false;
	if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) {
		return false;
	}
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *name = nullptr;
		sd_bus_message_read(m, "s", &name);
		if (name && strcmp(name, key) == 0 && sd_bus_message_enter_container(m, 'v', "b") > 0) {
			int value = 0;
			sd_bus_message_read(m, "b", &value);
			found = value != 0;
			sd_bus_message_exit_container(m);
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	return found;
}

// GetCurrentState's reply, (ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv}).
bool parse_display_state(sd_bus_message *m, DisplayState *out) {
	if (sd_bus_message_read(m, "u", &out->serial) < 0 ||
		sd_bus_message_enter_container(m, 'a', "((ssss)a(siiddada{sv})a{sv})") < 0) {
		return false;
	}
	while (sd_bus_message_enter_container(m, 'r', "(ssss)a(siiddada{sv})a{sv}") > 0) {
		DisplayState::Monitor monitor;
		const char *connector = nullptr;
		sd_bus_message_read(m, "(ssss)", &connector, nullptr, nullptr, nullptr);
		monitor.connector = connector ? connector : "";
		sd_bus_message_enter_container(m, 'a', "(siiddada{sv})");
		while (sd_bus_message_enter_container(m, 'r', "siiddada{sv}") > 0) {
			const char *id = nullptr;
			int32_t width = 0, height = 0;
			double refresh = 0, preferred = 0;
			sd_bus_message_read(m, "siidd", &id, &width, &height, &refresh, &preferred);
			std::vector<double> scales;
			sd_bus_message_enter_container(m, 'a', "d");
			double scale = 0;
			while (sd_bus_message_read(m, "d", &scale) > 0) {
				scales.push_back(scale);
			}
			sd_bus_message_exit_container(m);
			if (read_flag(m, "is-current")) {
				monitor.mode_id = id ? id : "";
				monitor.width = width;
				monitor.height = height;
				monitor.scales = std::move(scales);
			}
			sd_bus_message_exit_container(m);
		}
		sd_bus_message_exit_container(m);
		sd_bus_message_skip(m, "a{sv}");
		sd_bus_message_exit_container(m);
		out->monitors.push_back(std::move(monitor));
	}
	sd_bus_message_exit_container(m);

	if (sd_bus_message_enter_container(m, 'a', "(iiduba(ssss)a{sv})") < 0) {
		return false;
	}
	while (sd_bus_message_enter_container(m, 'r', "iiduba(ssss)a{sv}") > 0) {
		DisplayState::Logical logical;
		sd_bus_message_read(m, "iidub", &logical.x, &logical.y, &logical.scale, &logical.transform,
			&logical.primary);
		sd_bus_message_enter_container(m, 'a', "(ssss)");
		const char *connector = nullptr;
		while (sd_bus_message_read(m, "(ssss)", &connector, nullptr, nullptr, nullptr) > 0) {
			logical.connectors.push_back(connector ? connector : "");
		}
		sd_bus_message_exit_container(m);
		sd_bus_message_skip(m, "a{sv}");
		sd_bus_message_exit_container(m);
		out->logical.push_back(std::move(logical));
	}
	sd_bus_message_exit_container(m);
	return true;
}

// The linked stream's PipeWireStreamAdded signal fires synchronously
// inside RemoteDesktop.Session.Start() in practice, well under 100 ms;
// 3 s is a generous ceiling, not a tuned value.
constexpr int64_t kNodeIdWaitUs = 3'000'000;

} // namespace

struct GnomeRemoteSession::Impl {
	Config config;
	sd_bus *bus = nullptr;
	std::string rd_path;
	std::string sc_path;
	std::string stream_path;
	uint32_t node_id = 0;
	bool node_id_ready = false;
	bool eis_connected = false;

	sd_bus_slot *node_added_slot = nullptr;
	sd_bus_slot *closed_slot = nullptr;
	sd_bus_slot *monitors_slot = nullptr;
	struct wl_event_source *bus_source = nullptr;
	// Config::scale, until the virtual monitor it is for exists and has it
	// (apply_pending_scale()); 0 once done, or when there is none to set.
	double pending_scale = 0.0;

	std::function<void()> on_closed_cb;

	// RecordVirtual's monitor only appears once the stream negotiates, after
	// open() returns, so the scale waits for DisplayConfig's MonitorsChanged.
	static int on_monitors_changed(sd_bus_message *, void *data, sd_bus_error *) {
		static_cast<Impl *>(data)->apply_pending_scale();
		return 0;
	}

	// Sets the virtual monitor (a "Meta-" connector at config's size) to
	// pending_scale, or the closest scale mutter supports for its mode,
	// over DisplayConfig.ApplyMonitorsConfig -- the call GNOME Settings
	// makes, temporary so nothing is saved. Once per open(): a scale the
	// user changes in the session afterwards stays.
	void apply_pending_scale() {
		if (pending_scale <= 0.0) {
			return;
		}
		sd_bus_error error = SD_BUS_ERROR_NULL;
		sd_bus_message *reply = nullptr;
		int r = sd_bus_call_method(bus, kDcService, kDcPath, kDcIface, "GetCurrentState", &error, &reply, "");
		DisplayState state;
		bool parsed = r >= 0 && parse_display_state(reply, &state);
		sd_bus_message_unref(reply);
		if (!parsed) {
			WLOG_ERROR("screencast: mutter's DisplayConfig.GetCurrentState failed: %s",
				r < 0 ? sdbus_error_text(error, r) : "unexpected reply");
			sd_bus_error_free(&error);
			pending_scale = 0.0;
			return;
		}
		const DisplayState::Monitor *ours = nullptr;
		for (const auto &monitor : state.monitors) {
			if (monitor.connector.rfind("Meta-", 0) == 0 && monitor.width == (int32_t)config.width &&
				monitor.height == (int32_t)config.height) {
				ours = &monitor;
			}
		}
		if (!ours) {
			return; // not negotiated yet: the next MonitorsChanged
		}
		double scale = pending_scale;
		pending_scale = 0.0;
		// mutter takes only the scales it lists for the mode (whole ones
		// without its fractional scaling, 1.25 and the like with it).
		double best = 0.0;
		for (double supported : ours->scales) {
			if (best == 0.0 || std::fabs(supported - scale) < std::fabs(best - scale)) {
				best = supported;
			}
		}
		if (best == 0.0) {
			best = scale;
		}

		sd_bus_message *m = nullptr;
		r = sd_bus_message_new_method_call(bus, &m, kDcService, kDcPath, kDcIface, "ApplyMonitorsConfig");
		r = r >= 0 ? sd_bus_message_append(m, "uu", state.serial, kDcTemporary) : r;
		r = r >= 0 ? sd_bus_message_open_container(m, 'a', "(iiduba(ssa{sv}))") : r;
		bool changed = false;
		for (const auto &logical : state.logical) {
			bool is_ours = false;
			for (const std::string &connector : logical.connectors) {
				is_ours = is_ours || connector == ours->connector;
			}
			double logical_scale = is_ours ? best : logical.scale;
			changed = changed || (is_ours && std::fabs(logical.scale - best) > 1e-3);
			r = r >= 0 ? sd_bus_message_open_container(m, 'r', "iiduba(ssa{sv})") : r;
			r = r >= 0 ? sd_bus_message_append(m, "iidub", logical.x, logical.y, logical_scale,
							 logical.transform, logical.primary)
					   : r;
			r = r >= 0 ? sd_bus_message_open_container(m, 'a', "(ssa{sv})") : r;
			for (const std::string &connector : logical.connectors) {
				std::string mode_id;
				for (const auto &monitor : state.monitors) {
					if (monitor.connector == connector) {
						mode_id = monitor.mode_id;
					}
				}
				r = r >= 0 ? sd_bus_message_open_container(m, 'r', "ssa{sv}") : r;
				r = r >= 0 ? sd_bus_message_append(m, "ss", connector.c_str(), mode_id.c_str()) : r;
				r = r >= 0 ? sdbus_append_no_options(m) : r;
				r = r >= 0 ? sd_bus_message_close_container(m) : r;
			}
			r = r >= 0 ? sd_bus_message_close_container(m) : r; // a(ssa{sv})
			r = r >= 0 ? sd_bus_message_close_container(m) : r; // the logical monitor
		}
		r = r >= 0 ? sd_bus_message_close_container(m) : r;
		r = r >= 0 ? sdbus_append_no_options(m) : r;
		if (r >= 0 && changed) {
			r = sd_bus_call(bus, m, 0, &error, nullptr);
			if (r < 0) {
				WLOG_ERROR("screencast: mutter refused a %.0f%% scale for %s: %s", best * 100.0,
					ours->connector.c_str(), sdbus_error_text(error, r));
			} else {
				WLOG_INFO("screencast: mutter's %s at %.0f%%", ours->connector.c_str(), best * 100.0);
			}
		}
		sd_bus_message_unref(m);
		sd_bus_error_free(&error);
	}

	static int on_pipewire_stream_added(sd_bus_message *m, void *data, sd_bus_error *) {
		auto *impl = static_cast<Impl *>(data);
		uint32_t node_id = 0;
		if (sd_bus_message_read(m, "u", &node_id) >= 0) {
			impl->node_id = node_id;
			impl->node_id_ready = true;
		}
		return 0;
	}

	static int on_session_closed(sd_bus_message *, void *data, sd_bus_error *) {
		auto *impl = static_cast<Impl *>(data);
		if (impl->on_closed_cb) {
			impl->on_closed_cb();
		}
		return 0;
	}

	// CreateSession on org.gnome.Mutter.RemoteDesktop -- no arguments,
	// returns the new session's object path.
	bool create_remote_desktop_session() {
		sd_bus_error error = SD_BUS_ERROR_NULL;
		sd_bus_message *reply = nullptr;
		int r = sd_bus_call_method(bus, kRdService, kRdPath, kRdIface, "CreateSession", &error, &reply, "");
		if (r < 0) {
			WLOG_ERROR("screencast: RemoteDesktop.CreateSession failed: %s", sdbus_error_text(error, r));
			sd_bus_error_free(&error);
			return false;
		}
		const char *path = nullptr;
		r = sd_bus_message_read(reply, "o", &path);
		if (r >= 0 && path) {
			rd_path = path;
		}
		sd_bus_message_unref(reply);
		sd_bus_error_free(&error);
		return r >= 0 && !rd_path.empty();
	}

	bool get_session_id(std::string *out) {
		sd_bus_error error = SD_BUS_ERROR_NULL;
		char *session_id = nullptr;
		int r = sd_bus_get_property_string(bus, kRdService, rd_path.c_str(), kRdSessionIface, "SessionId",
			&error, &session_id);
		if (r < 0) {
			WLOG_ERROR("screencast: reading RemoteDesktop.Session.SessionId failed: %s",
				sdbus_error_text(error, r));
			sd_bus_error_free(&error);
			return false;
		}
		sd_bus_error_free(&error);
		*out = session_id ? session_id : "";
		free(session_id);
		return !out->empty();
	}

	// ScreenCast.CreateSession({'remote-desktop-session-id': session_id})
	// -- the a{sv} dict with one string-valued entry links the two
	// sessions (docs/design/capture-backends.md#gnome).
	bool create_linked_screencast_session(const std::string &session_id) {
		sd_bus_message *m = nullptr;
		int r = sd_bus_message_new_method_call(bus, &m, kScService, kScPath, kScIface, "CreateSession");
		if (r < 0) {
			return false;
		}
		r = sdbus_append_one_option(m, "remote-desktop-session-id", "s",
			[&](sd_bus_message *msg) { return sd_bus_message_append(msg, "s", session_id.c_str()); });
		if (r < 0) {
			sd_bus_message_unref(m);
			return false;
		}

		sd_bus_error error = SD_BUS_ERROR_NULL;
		sd_bus_message *reply = nullptr;
		r = sd_bus_call(bus, m, 0, &error, &reply);
		sd_bus_message_unref(m);
		if (r < 0) {
			WLOG_ERROR("screencast: ScreenCast.CreateSession(linked) failed: %s", sdbus_error_text(error, r));
			sd_bus_error_free(&error);
			return false;
		}
		const char *path = nullptr;
		r = sd_bus_message_read(reply, "o", &path);
		if (r >= 0 && path) {
			sc_path = path;
		}
		sd_bus_message_unref(reply);
		sd_bus_error_free(&error);
		return r >= 0 && !sc_path.empty();
	}

	// ScreenCast.Session.RecordVirtual({'cursor-mode': uint32(2)}) --
	// cursor mode 2 = metadata: embed SPA_META_Cursor in each frame rather
	// than painting the cursor into the pixels.
	bool record_virtual() {
		sd_bus_message *m = nullptr;
		int r = sd_bus_message_new_method_call(bus, &m, kScService, sc_path.c_str(), kScSessionIface,
			"RecordVirtual");
		if (r < 0) {
			return false;
		}
		r = sdbus_append_one_option(m, "cursor-mode", "u",
			[](sd_bus_message *msg) { return sd_bus_message_append(msg, "u", (uint32_t)2); });
		if (r < 0) {
			sd_bus_message_unref(m);
			return false;
		}

		sd_bus_error error = SD_BUS_ERROR_NULL;
		sd_bus_message *reply = nullptr;
		r = sd_bus_call(bus, m, 0, &error, &reply);
		sd_bus_message_unref(m);
		if (r < 0) {
			WLOG_ERROR("screencast: ScreenCast.Session.RecordVirtual failed: %s", sdbus_error_text(error, r));
			sd_bus_error_free(&error);
			return false;
		}
		const char *path = nullptr;
		r = sd_bus_message_read(reply, "o", &path);
		if (r >= 0 && path) {
			stream_path = path;
		}
		sd_bus_message_unref(reply);
		sd_bus_error_free(&error);
		return r >= 0 && !stream_path.empty();
	}

	bool start_session() {
		sd_bus_error error = SD_BUS_ERROR_NULL;
		int r = sd_bus_call_method(bus, kRdService, rd_path.c_str(), kRdSessionIface, "Start", &error,
			nullptr, "");
		if (r < 0) {
			WLOG_ERROR("screencast: RemoteDesktop.Session.Start failed: %s", sdbus_error_text(error, r));
		}
		sd_bus_error_free(&error);
		return r >= 0;
	}

	// Blocks (processing the bus) until node_id_ready or the deadline.
	// The match has to be installed before Start() (see open()); a
	// signal that arrives between Start() and this wait is still queued
	// by sd-bus and dispatched here.
	bool wait_for_node_id() {
		int64_t deadline = monotonic_now_us() + kNodeIdWaitUs;
		while (!node_id_ready) {
			int r = sd_bus_process(bus, nullptr);
			if (r < 0) {
				return false;
			}
			if (r > 0) {
				continue;
			}
			int64_t remaining = deadline - monotonic_now_us();
			if (remaining <= 0) {
				return false;
			}
			sd_bus_wait(bus, (uint64_t)remaining);
		}
		return true;
	}

	static void bus_readable(void *data) {
		auto *impl = static_cast<Impl *>(data);
		while (sd_bus_process(impl->bus, nullptr) > 0) {}
	}
};

GnomeRemoteSession::GnomeRemoteSession() : impl_(std::make_unique<Impl>()) {}
GnomeRemoteSession::~GnomeRemoteSession() {
	close();
}

bool GnomeRemoteSession::open(const Config &config) {
	impl_->config = config;
	impl_->on_closed_cb = on_closed;

	if (sd_bus_open_user(&impl_->bus) < 0) {
		WLOG_ERROR("screencast: sd_bus_open_user failed");
		return false;
	}

	if (!impl_->create_remote_desktop_session()) {
		return false;
	}
	std::string session_id;
	if (!impl_->get_session_id(&session_id)) {
		return false;
	}
	if (!impl_->create_linked_screencast_session(session_id)) {
		return false;
	}
	if (!impl_->record_virtual()) {
		return false;
	}

	// Subscribed before Start() -- Start() starts the linked stream
	// synchronously and a missed signal is not redelivered
	// (docs/design/capture-backends.md#gnome).
	sd_bus_match_signal(impl_->bus, &impl_->node_added_slot, kScService, impl_->stream_path.c_str(),
		kScStreamIface, "PipeWireStreamAdded", Impl::on_pipewire_stream_added, impl_.get());
	sd_bus_match_signal(impl_->bus, &impl_->closed_slot, kRdService, impl_->rd_path.c_str(), kRdSessionIface,
		"Closed", Impl::on_session_closed, impl_.get());
	impl_->pending_scale = config.scale;
	if (impl_->pending_scale > 0.0) {
		sd_bus_match_signal(impl_->bus, &impl_->monitors_slot, kDcService, kDcPath, kDcIface, "MonitorsChanged",
			Impl::on_monitors_changed, impl_.get());
	}

	if (!impl_->start_session()) {
		return false;
	}
	if (!impl_->wait_for_node_id()) {
		WLOG_ERROR("screencast: PipeWireStreamAdded never arrived");
		return false;
	}

	impl_->bus_source = wl_event_loop_add_fd(
		config.event_loop, sd_bus_get_fd(impl_->bus), WL_EVENT_READABLE,
		[](int, uint32_t, void *data) {
			Impl::bus_readable(data);
			return 0;
		},
		impl_.get());
	return true;
}

void GnomeRemoteSession::close() {
	if (impl_->bus_source) {
		wl_event_source_remove(impl_->bus_source);
		impl_->bus_source = nullptr;
	}
	if (impl_->bus && !impl_->rd_path.empty()) {
		sd_bus_call_method(impl_->bus, kRdService, impl_->rd_path.c_str(), kRdSessionIface, "Stop", nullptr,
			nullptr, "");
	}
	if (impl_->node_added_slot) {
		sd_bus_slot_unref(impl_->node_added_slot);
		impl_->node_added_slot = nullptr;
	}
	if (impl_->closed_slot) {
		sd_bus_slot_unref(impl_->closed_slot);
		impl_->closed_slot = nullptr;
	}
	if (impl_->monitors_slot) {
		sd_bus_slot_unref(impl_->monitors_slot);
		impl_->monitors_slot = nullptr;
	}
	impl_->pending_scale = 0.0;
	if (impl_->bus) {
		sd_bus_unref(impl_->bus);
		impl_->bus = nullptr;
	}
	impl_->rd_path.clear();
	impl_->sc_path.clear();
	impl_->stream_path.clear();
	impl_->node_id_ready = false;
	impl_->eis_connected = false;
}

uint32_t GnomeRemoteSession::pipewire_node_id() const {
	return impl_->node_id;
}

bool GnomeRemoteSession::connect_eis(int *out_fd) {
	sd_bus_message *m = nullptr;
	int r = sd_bus_message_new_method_call(impl_->bus, &m, kRdService, impl_->rd_path.c_str(),
		kRdSessionIface, "ConnectToEIS");
	if (r < 0) {
		return false;
	}
	r = sdbus_append_no_options(m);
	if (r < 0) {
		sd_bus_message_unref(m);
		return false;
	}

	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = nullptr;
	r = sd_bus_call(impl_->bus, m, 0, &error, &reply);
	sd_bus_message_unref(m);
	if (r < 0) {
		WLOG_ERROR("screencast: RemoteDesktop.Session.ConnectToEIS failed: %s", sdbus_error_text(error, r));
		sd_bus_error_free(&error);
		return false;
	}
	sd_bus_error_free(&error);

	int fd = -1;
	r = sd_bus_message_read(reply, "h", &fd);
	// ei_setup_backend_fd takes ownership of whatever it is handed, so it
	// gets its own copy of the message's borrowed fd.
	int dup_fd = r >= 0 ? sdbus_dup_fd(fd) : -1;
	sd_bus_message_unref(reply);
	if (dup_fd < 0) {
		WLOG_ERROR("screencast: ConnectToEIS reply had no usable fd");
		return false;
	}
	*out_fd = dup_fd;
	impl_->eis_connected = true;
	return true;
}

std::unique_ptr<FrameSource> GnomeRemoteSession::make_frame_source() {
	return std::make_unique<PipeWireCapture>(pipewire_node_id());
}

std::unique_ptr<ClipboardSink> GnomeRemoteSession::make_clipboard_sink() {
	return GnomeClipboard::create(impl_->bus, impl_->rd_path, impl_->config.event_loop);
}

std::unique_ptr<ScreencastInput> GnomeRemoteSession::make_input_sink() {
	// ConnectToEIS is cheap to retry (it works immediately once the linked
	// session is up, docs/design/capture-backends.md), so a null here just means
	// "ask again next tick".
	int eis_fd = -1;
	if (!connect_eis(&eis_fd)) {
		return nullptr;
	}
	auto input = std::make_unique<EiInput>();
	EiInput::Config config;
	config.event_loop = impl_->config.event_loop;
	config.output_width = impl_->config.width;
	config.output_height = impl_->config.height;
	config.eis_fd = eis_fd;
	config.discrete_scroll_whole_notches_only = true;
	if (!input->open(config)) {
		return nullptr;
	}
	return input;
}

} // namespace wraith
