// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Scrolling in wheel notches (gdp-spec.md §8.2) as whole 1/120ths of a
// notch, the unit of libei's discrete scroll and Linux's high-resolution
// wheel. A touchpad sends a stream of small fractions; rounding each one
// on its own loses or inflates most of them, so what doesn't make a whole
// step is carried into the next call. One per axis. No libei types here,
// so it is unit-tested on its own (host/wraith/tests/scroll_steps_test.cpp).
#pragma once

#include <cstdint>

namespace wraith {

class ScrollSteps {
public:
	// Adds `notches` and returns the whole 1/120 steps to send now,
	// possibly none.
	int32_t add(double notches);

private:
	double carry_ = 0.0;
};

} // namespace wraith
