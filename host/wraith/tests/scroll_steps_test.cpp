// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Plain-assert unit test (same convention as the other host/wraith/tests/):
// ScrollSteps turns notches into whole 1/120 steps without losing or
// inventing any of a touchpad's small fractions.
#include "screencast/scroll_steps.hpp"

// These tests are plain assert()s: make sure a Release build (-DNDEBUG)
// can't compile them away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>

using namespace wraith;

namespace {

void test_whole_notches_pass_through() {
	ScrollSteps s;
	assert(s.add(1.0) == 120);
	assert(s.add(-3.0) == -360);
	assert(s.add(0.0) == 0);
}

void test_fractions_of_a_step_are_carried() {
	ScrollSteps s;
	// A third of a step each: nothing, nothing, then one.
	assert(s.add(1.0 / 360.0) == 0);
	assert(s.add(1.0 / 360.0) == 0);
	assert(s.add(1.0 / 360.0) == 1);
}

// A touchpad's stream adds up to what was sent, step for step.
void test_a_stream_of_fractions_loses_nothing() {
	ScrollSteps s;
	int32_t total = 0;
	for (int i = 0; i < 1000; ++i) {
		total += s.add(0.013);
	}
	assert(total == 1560); // 13 notches
}

void test_thirds_add_up_to_a_whole_notch() {
	ScrollSteps s;
	int32_t total = 0;
	for (int i = 0; i < 3; ++i) {
		total += s.add(1.0 / 3.0);
	}
	assert(total == 120);
}

void test_reversing_uses_up_the_carry_first() {
	ScrollSteps s;
	assert(s.add(0.5 / 120.0) == 0);
	// Half a step back cancels the half carried; the next whole one goes out.
	assert(s.add(-0.5 / 120.0) == 0);
	assert(s.add(-1.0 / 120.0) == -1);
}

void test_negative_fractions_are_carried() {
	ScrollSteps s;
	assert(s.add(-0.6 / 120.0) == 0);
	assert(s.add(-0.6 / 120.0) == -1);
}

} // namespace

int main() {
	test_whole_notches_pass_through();
	test_fractions_of_a_step_are_carried();
	test_a_stream_of_fractions_loses_nothing();
	test_thirds_add_up_to_a_whole_notch();
	test_reversing_uses_up_the_carry_first();
	test_negative_fractions_are_carried();
	std::puts("scroll_steps_test: all tests passed");
	return 0;
}
