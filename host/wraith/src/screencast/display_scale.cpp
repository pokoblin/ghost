// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/display_scale.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wraith {

namespace {
// mutter's own bounds (MINIMUM_SCALE_FACTOR, MAXIMUM_SCALE_FACTOR).
constexpr double kMinScale = 1.0;
constexpr double kMaxScale = 4.0;
// Its scales are floats on the way through, so 1.3333333730697632
// is the 4/3 it means.
constexpr double kScaleEpsilon = 1e-4;

// mkdir -p for every directory above `path`'s last component.
bool make_parent_dirs(const std::string &path) {
	for (size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1)) {
		std::string dir = path.substr(0, slash);
		if (mkdir(dir.c_str(), 0700) < 0 && errno != EEXIST) {
			return false;
		}
	}
	return true;
}
} // namespace

double closest_supported_scale(double wanted, const std::vector<double> &supported) {
	double best = 0;
	for (double scale : supported) {
		double distance = std::fabs(scale - wanted);
		double best_distance = std::fabs(best - wanted);
		if (best == 0 || distance < best_distance - kScaleEpsilon ||
			(distance < best_distance + kScaleEpsilon && scale < best)) {
			best = scale;
		}
	}
	return best;
}

bool same_scale(double a, double b) {
	return std::fabs(a - b) < kScaleEpsilon;
}

std::string scale_state_path(const char *xdg_state_home, const char *home) {
	if (xdg_state_home && xdg_state_home[0] == '/') {
		return std::string(xdg_state_home) + "/wraith/gnome-scale";
	}
	if (home && home[0] == '/') {
		return std::string(home) + "/.local/state/wraith/gnome-scale";
	}
	return "";
}

std::string format_scale(double scale) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%.9g\n", scale);
	return buf;
}

bool parse_scale(const std::string &text, double *out) {
	const char *start = text.c_str();
	char *end = nullptr;
	errno = 0;
	double scale = strtod(start, &end);
	if (end == start || errno != 0) {
		return false;
	}
	for (; *end; end++) {
		if (*end != ' ' && *end != '\t' && *end != '\n' && *end != '\r') {
			return false;
		}
	}
	// NaN fails both comparisons.
	if (!(scale >= kMinScale - kScaleEpsilon && scale <= kMaxScale + kScaleEpsilon)) {
		return false;
	}
	*out = scale;
	return true;
}

bool load_scale(const std::string &path, double *out) {
	std::ifstream in(path);
	if (!in) {
		return false;
	}
	std::stringstream text;
	text << in.rdbuf();
	return parse_scale(text.str(), out);
}

bool save_scale(const std::string &path, double scale) {
	if (path.empty() || !make_parent_dirs(path)) {
		return false;
	}
	std::string tmp = path + ".tmp";
	int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		return false;
	}
	std::string text = format_scale(scale);
	bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size();
	ok = close(fd) == 0 && ok;
	if (!ok || rename(tmp.c_str(), path.c_str()) < 0) {
		unlink(tmp.c_str());
		return false;
	}
	return true;
}

ScaleMemory::Decision ScaleMemory::observe(const std::string &monitor, double current,
	const std::vector<double> &supported) {
	if (monitor != monitor_) {
		monitor_ = monitor;
		expected_ = current;
		if (remembered_ <= 0) {
			return {};
		}
		double target = closest_supported_scale(remembered_, supported);
		if (target <= 0 || same_scale(target, current)) {
			return {};
		}
		expected_ = target;
		return {Action::Apply, target};
	}
	if (same_scale(current, expected_)) {
		return {};
	}
	expected_ = current;
	if (same_scale(current, remembered_)) {
		return {};
	}
	remembered_ = current;
	return {Action::Remember, current};
}

} // namespace wraith
