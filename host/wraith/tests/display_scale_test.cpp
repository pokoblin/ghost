// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Plain-assert unit test (same convention as the other host/wraith/tests/):
// the GNOME backend's remembered display scale
// (screencast/display_scale.hpp) -- which supported scale a remembered
// one becomes, the state file, and which scale changes ScaleMemory takes
// for the user's. The D-Bus half (GnomeDisplayScale) is exercised end to
// end against a real gnome-shell.
#include "screencast/display_scale.hpp"

// These tests are plain assert()s: make sure a Release build (-DNDEBUG)
// can't compile them away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

using namespace wraith;

namespace {

using Action = ScaleMemory::Action;

// What mutter offered on the test host: a 2864x1606 virtual monitor
// supports only whole scales, a 3840x2056 one fractional ones too.
const std::vector<double> kWhole = {1.0, 2.0};
const std::vector<double> kFractional = {1.0, 1.25, 1.3333333730697632, 1.5, 1.75, 2.0};

void test_closest_supported_scale_exact() {
	assert(closest_supported_scale(2.0, kWhole) == 2.0);
	assert(closest_supported_scale(1.0, kWhole) == 1.0);
	// The float mutter hands back for 4/3 still counts as 4/3.
	assert(closest_supported_scale(4.0 / 3.0, kFractional) == 1.3333333730697632);
}

void test_closest_supported_scale_rounds_to_nearest() {
	// 1.333 on a mode without it: 1.0 is closer than 2.0.
	assert(closest_supported_scale(1.3333333730697632, kWhole) == 1.0);
	assert(closest_supported_scale(1.75, kWhole) == 2.0);
	assert(closest_supported_scale(3.0, kWhole) == 2.0);
	assert(closest_supported_scale(1.4, {1.0, 1.25, 1.5, 2.0}) == 1.5);
}

// Equally close: the smaller, the less crowded desktop.
void test_closest_supported_scale_tie_goes_to_smaller() {
	assert(closest_supported_scale(1.5, kWhole) == 1.0);
	assert(closest_supported_scale(1.5, {2.0, 1.0}) == 1.0);
}

void test_closest_supported_scale_empty() {
	assert(closest_supported_scale(2.0, {}) == 0);
}

void test_state_path_prefers_xdg_state_home() {
	assert(scale_state_path("/x/state", "/home/u") == "/x/state/wraith/gnome-scale");
	assert(scale_state_path(nullptr, "/home/u") == "/home/u/.local/state/wraith/gnome-scale");
	// A relative XDG_STATE_HOME is invalid and ignored, as the spec says.
	assert(scale_state_path("state", "/home/u") == "/home/u/.local/state/wraith/gnome-scale");
	assert(scale_state_path("", "/home/u") == "/home/u/.local/state/wraith/gnome-scale");
	assert(scale_state_path(nullptr, nullptr).empty());
	assert(scale_state_path(nullptr, "relative").empty());
}

void test_parse_scale() {
	double scale = 0;
	assert(parse_scale("2\n", &scale) && scale == 2.0);
	assert(parse_scale(" 1.25 ", &scale) && scale == 1.25);
	assert(parse_scale("1.33333337", &scale) && same_scale(scale, 4.0 / 3.0));
	assert(parse_scale("4", &scale) && scale == 4.0);
	scale = 7;
	assert(!parse_scale("", &scale));
	assert(!parse_scale("\n", &scale));
	assert(!parse_scale("two", &scale));
	assert(!parse_scale("2x", &scale));
	assert(!parse_scale("2 3", &scale));
	assert(!parse_scale("0.5", &scale));
	assert(!parse_scale("5", &scale));
	assert(!parse_scale("nan", &scale));
	assert(!parse_scale("inf", &scale));
	assert(scale == 7); // untouched on failure
}

// What format_scale() writes, parse_scale() reads back as the same scale.
void test_format_scale_round_trips() {
	for (double written : {1.0, 1.25, 1.3333333730697632, 1.5, 1.6666666269302368, 1.75, 2.0, 3.0}) {
		std::string text = format_scale(written);
		assert(!text.empty() && text.back() == '\n');
		double read = 0;
		assert(parse_scale(text, &read));
		assert(same_scale(read, written));
	}
	assert(format_scale(2.0) == "2\n");
}

// save_scale() makes the directories it needs and replaces the file;
// load_scale() reads it back, and refuses a missing or broken one.
void test_save_and_load() {
	char base[] = "/tmp/wraith-display-scale-XXXXXX";
	assert(mkdtemp(base));
	std::string path = scale_state_path(base, nullptr);

	double scale = 0;
	assert(!load_scale(path, &scale));

	assert(save_scale(path, 2.0));
	assert(load_scale(path, &scale) && scale == 2.0);
	assert(save_scale(path, 1.3333333730697632));
	assert(load_scale(path, &scale) && same_scale(scale, 4.0 / 3.0));

	struct stat st{};
	assert(stat((std::string(base) + "/wraith").c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
	assert(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
	assert(access((path + ".tmp").c_str(), F_OK) != 0);

	{
		std::ofstream out(path);
		out << "garbage\n";
	}
	assert(!load_scale(path, &scale));

	assert(!save_scale("", 2.0));

	unlink(path.c_str());
	rmdir((std::string(base) + "/wraith").c_str());
	rmdir(base);
}

// Nothing remembered: a new monitor keeps mutter's scale, and only a
// later change on it is remembered.
void test_memory_nothing_remembered() {
	ScaleMemory memory(0);
	auto d = memory.observe("Meta-0/0x000001", 1.0, kWhole);
	assert(d.action == Action::None);
	d = memory.observe("Meta-0/0x000001", 1.0, kWhole);
	assert(d.action == Action::None);
	d = memory.observe("Meta-0/0x000001", 2.0, kWhole);
	assert(d.action == Action::Remember && d.scale == 2.0);
	assert(memory.remembered() == 2.0);
}

// The sequence the test host showed: a new monitor at mutter's default
// 1.0 is applied the remembered 2, the state after the apply is not a
// change, and the next monitor (a resize) gets 2 again.
void test_memory_applies_to_each_new_monitor() {
	ScaleMemory memory(2.0);
	auto d = memory.observe("Meta-0/0x000001", 1.0, kWhole);
	assert(d.action == Action::Apply && d.scale == 2.0);
	d = memory.observe("Meta-0/0x000001", 2.0, kWhole);
	assert(d.action == Action::None);
	d = memory.observe("Meta-0/0x000002", 1.0, kWhole);
	assert(d.action == Action::Apply && d.scale == 2.0);
	assert(memory.remembered() == 2.0);
}

// A monitor already at the remembered scale (mutter matched a
// monitors.xml configuration) needs no apply.
void test_memory_already_at_remembered_scale() {
	ScaleMemory memory(2.0);
	auto d = memory.observe("Meta-0/0x000003", 2.0, kWhole);
	assert(d.action == Action::None);
	d = memory.observe("Meta-0/0x000003", 2.0, kWhole);
	assert(d.action == Action::None);
}

// A mutter-matched configuration at another scale is not the user's:
// the remembered one is applied over it, and nothing is remembered.
void test_memory_matched_configuration_is_not_remembered() {
	ScaleMemory memory(2.0);
	auto d = memory.observe("Meta-0/0x000004", 1.3333333730697632, kFractional);
	assert(d.action == Action::Apply && d.scale == 2.0);
	assert(memory.remembered() == 2.0);
}

// An apply that had to round must not overwrite what's remembered: the
// next monitor that supports 4/3 gets 4/3 again.
void test_memory_rounded_apply_keeps_the_remembered_scale() {
	ScaleMemory memory(1.3333333730697632);
	auto d = memory.observe("Meta-0/0x000001", 2.0, kWhole);
	assert(d.action == Action::Apply && d.scale == 1.0);
	d = memory.observe("Meta-0/0x000001", 1.0, kWhole);
	assert(d.action == Action::None);
	assert(same_scale(memory.remembered(), 4.0 / 3.0));
	d = memory.observe("Meta-0/0x000002", 1.0, kFractional);
	assert(d.action == Action::Apply && same_scale(d.scale, 4.0 / 3.0));
}

// The user changing the scale in Settings, after wraith's apply, is
// remembered; changing it back is a change too.
void test_memory_user_change_after_apply() {
	ScaleMemory memory(2.0);
	memory.observe("Meta-0/0x000001", 1.0, kFractional);
	memory.observe("Meta-0/0x000001", 2.0, kFractional);
	auto d = memory.observe("Meta-0/0x000001", 1.3333333730697632, kFractional);
	assert(d.action == Action::Remember && same_scale(d.scale, 4.0 / 3.0));
	d = memory.observe("Meta-0/0x000001", 1.3333333730697632, kFractional);
	assert(d.action == Action::None);
	d = memory.observe("Meta-0/0x000001", 1.0, kFractional);
	assert(d.action == Action::Remember && d.scale == 1.0);
	assert(memory.remembered() == 1.0);
}

// A failed apply leaves the monitor where it was; seeing it there is
// then no change of the user's.
void test_memory_failed_apply() {
	ScaleMemory memory(2.0);
	auto d = memory.observe("Meta-0/0x000001", 1.0, kWhole);
	assert(d.action == Action::Apply);
	memory.apply_failed(1.0);
	d = memory.observe("Meta-0/0x000001", 1.0, kWhole);
	assert(d.action == Action::None);
	assert(memory.remembered() == 2.0);
}

// No supported scales reported for the mode: nothing to apply.
void test_memory_no_supported_scales() {
	ScaleMemory memory(2.0);
	auto d = memory.observe("Meta-0/0x000001", 1.0, {});
	assert(d.action == Action::None);
}

} // namespace

int main() {
	test_closest_supported_scale_exact();
	test_closest_supported_scale_rounds_to_nearest();
	test_closest_supported_scale_tie_goes_to_smaller();
	test_closest_supported_scale_empty();
	test_state_path_prefers_xdg_state_home();
	test_parse_scale();
	test_format_scale_round_trips();
	test_save_and_load();
	test_memory_nothing_remembered();
	test_memory_applies_to_each_new_monitor();
	test_memory_already_at_remembered_scale();
	test_memory_matched_configuration_is_not_remembered();
	test_memory_rounded_apply_keeps_the_remembered_scale();
	test_memory_user_change_after_apply();
	test_memory_failed_apply();
	test_memory_no_supported_scales();
	printf("display_scale_test: ok\n");
	return 0;
}
