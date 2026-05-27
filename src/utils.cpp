#include "utils.h"
#include <sys/utsname.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace utils {

// ----- ANSI color codes -----
static const char* RESET   = "\033[0m";
static const char* RED     = "\033[31m";
static const char* GREEN   = "\033[32m";
static const char* YELLOW  = "\033[33m";
static const char* BLUE    = "\033[34m";
static const char* GRAY    = "\033[90m";

std::string colorize(const std::string& text, Status s) {
    return color_status(s) + text + RESET;
}

std::string color_reset() { return RESET; }

std::string color_status(Status s) {
    switch (s) {
        case Status::OK:      return GREEN;
        case Status::WARNING: return YELLOW;
        case Status::DANGER:  return RED;
        case Status::INFO:    return BLUE;
        case Status::NA:      return GRAY;
    }
    return RESET;
}

std::string progress_bar(double percent, int width) {
    int filled = static_cast<int>(std::round(percent / 100.0 * width));
    filled = std::max(0, std::min(width, filled));

    std::string bar;
    for (int i = 0; i < width; i++) {
        if (i < filled) {
            bar += "█";
        } else if (i == filled && filled < width) {
            bar += "░";
        } else {
            bar += "░";
        }
    }
    return bar;
}

bool is_installed(const std::string& cmd) {
    std::string check = "which " + cmd + " 2>/dev/null";
    FILE* pipe = popen(check.c_str(), "r");
    if (!pipe) return false;
    char buf[256];
    bool found = fgets(buf, sizeof(buf), pipe) != nullptr;
    pclose(pipe);
    return found;
}

std::string strip(const std::string& s) {
    auto start = std::find_if_not(s.begin(), s.end(),
        [](unsigned char c) { return std::isspace(c); });
    auto end = std::find_if_not(s.rbegin(), s.rend(),
        [](unsigned char c) { return std::isspace(c); }).base();
    return (start < end) ? std::string(start, end) : "";
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> tokens;
    std::istringstream stream(s);
    std::string token;
    while (std::getline(stream, token, delim)) {
        tokens.push_back(token);
    }
    return tokens;
}

double parse_percent(const std::string& s) {
    std::string cleaned = strip(s);
    // Remove trailing %
    if (!cleaned.empty() && cleaned.back() == '%') {
        cleaned.pop_back();
    }
    try {
        return std::stod(cleaned);
    } catch (...) {
        return -1.0;
    }
}

double parse_number(const std::string& s) {
    std::string cleaned = strip(s);
    try {
        return std::stod(cleaned);
    } catch (...) {
        return -1.0;
    }
}

Status classify(double value, const Threshold& t, bool higher_is_worse) {
    if (value < 0) return Status::NA;
    if (higher_is_worse) {
        if (value >= t.danger)  return Status::DANGER;
        if (value >= t.warning) return Status::WARNING;
        return Status::OK;
    } else {
        // lower_is_worse (e.g., free memory)
        if (value <= t.danger)  return Status::DANGER;
        if (value <= t.warning) return Status::WARNING;
        return Status::OK;
    }
}

std::string timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::localtime(&t);
    std::ostringstream oss;
    oss << std::put_time(tm, "%F %T");
    return oss.str();
}

std::string pad_right(const std::string& s, int width) {
    if ((int)s.length() >= width) return s.substr(0, width);
    return s + std::string(width - s.length(), ' ');
}

int count_issues(const std::vector<ResourceResult>& results) {
    int count = 0;
    for (const auto& r : results) {
        for (const auto& m : r.metrics) {
            if (m.status == Status::WARNING || m.status == Status::DANGER) {
                count++;
            }
        }
    }
    return count;
}

} // namespace utils
