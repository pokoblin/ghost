// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/scroll_steps.hpp"

#include <cmath>

namespace wraith {

int32_t ScrollSteps::add(double notches) {
	carry_ += notches * 120.0;
	// A hair toward the next step, so a sum that should be whole (three
	// thirds of a notch) isn't truncated to one step less.
	const double steps = std::trunc(carry_ + std::copysign(1e-6, carry_));
	carry_ -= steps;
	return static_cast<int32_t>(steps);
}

} // namespace wraith
