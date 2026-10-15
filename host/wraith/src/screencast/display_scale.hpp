// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The GNOME backend's remembered display scale
// (docs/design/capture-backends.md#display-scale), minus the D-Bus:
// which supported scale to apply, where the remembered one is kept, and
// which scale changes are the user's to remember.
//
// mutter matches a saved monitors.xml configuration on the monitor's
// serial and needs its mode too, and every RecordVirtual monitor is new:
// its serial counts up from 1 with each one a gnome-shell creates, and
// its mode is whatever size the client asked for. So the scale set in
// Settings is saved but almost never found again, and each new virtual
// monitor starts at 1.0. wraith remembers the scale itself instead and
// re-applies it to each new monitor (GnomeDisplayScale). No sd-bus types
// here, so this is a pure, independently unit-testable translation unit
// (host/wraith/tests/display_scale_test.cpp).
#pragma once

#include <string>
#include <vector>

namespace wraith {

// The scale in `supported` (a mode's supported scales, from
// DisplayConfig.GetCurrentState) closest to `wanted`; of two equally
// close, the smaller. Not every mode supports every scale -- mutter only
// offers scales that divide it into a whole logical size -- so a
// remembered 1.333 may come out as 1.25 or 1.5 on another size. 0 if
// `supported` is empty.
double closest_supported_scale(double wanted, const std::vector<double> &supported);

// Two scales as mutter reports them: doubles that went through a float.
bool same_scale(double a, double b);

// `$XDG_STATE_HOME/wraith/gnome-scale`, or `$HOME/.local/state/wraith/
// gnome-scale` when XDG_STATE_HOME is unset or not absolute (the XDG
// base directory rule). Empty if neither gives a path. Takes the two
// variables' values (null when unset) rather than reading them, for the
// tests.
std::string scale_state_path(const char *xdg_state_home, const char *home);

// The file's whole content: one decimal number and a newline.
std::string format_scale(double scale);
// False for anything that isn't one number in mutter's 1..4 scale range,
// surrounding whitespace aside.
bool parse_scale(const std::string &text, double *out);

// False if the file is missing or doesn't parse.
bool load_scale(const std::string &path, double *out);
// Creates the missing directories (0700) and replaces the file
// atomically (a temporary file renamed over it). False on any failure.
bool save_scale(const std::string &path, double scale);

// Decides, from each state wraith's own virtual monitor is seen in, what
// to do about its scale. The point is telling the user's change (made in
// Settings, to remember) from everything else that moves the scale: a
// new monitor's default, a monitors.xml configuration mutter happened to
// match, and wraith's own apply.
//
// A monitor seen for the first time is settled: if a remembered scale
// is set and differs from its current one, the answer is Apply with the
// closest supported scale. From then on the scale it was settled at is
// expected, and anything else seen on that monitor is the user's: the
// answer is Remember. Seeing the expected scale is never a change, so an
// apply that had to round (1.333 to 1.25) doesn't overwrite what is
// remembered.
class ScaleMemory {
public:
	enum class Action { None, Apply, Remember };
	struct Decision {
		Action action = Action::None;
		double scale = 0;
	};

	// `remembered` 0: nothing remembered yet, a new monitor keeps the
	// scale mutter gave it.
	explicit ScaleMemory(double remembered) : remembered_(remembered) {}

	// `monitor` identifies the virtual monitor (its connector and
	// serial), `current` is its logical monitor's scale and `supported`
	// its current mode's supported scales.
	Decision observe(const std::string &monitor, double current, const std::vector<double> &supported);
	// The Apply observe() asked for didn't happen: the monitor stays at
	// `current`, which is then the expected scale.
	void apply_failed(double current) { expected_ = current; }

	double remembered() const { return remembered_; }

private:
	double remembered_;
	std::string monitor_;
	double expected_ = 0;
};

} // namespace wraith
