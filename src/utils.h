#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <vector>
#include "types.h"

namespace utils {

// ANSI color wrappers
std::string colorize(const std::string& text, Status s);
std::string color_reset();
std::string color_status(Status s);

// Progress bar: "████████░░ 72.3%"
std::string progress_bar(double percent, int width = 20);

// Check if a command exists on the system
bool is_installed(const std::string& cmd);

// Trim whitespace from both ends
std::string strip(const std::string& s);

// Split string by delimiter
std::vector<std::string> split(const std::string& s, char delim);

// Parse percentage string like "72.3%" or " 45.2 "
double parse_percent(const std::string& s);

// Parse a number from string
double parse_number(const std::string& s);

// Classify a value against thresholds
Status classify(double value, const Threshold& t, bool higher_is_worse = true);

// Get current timestamp string
std::string timestamp();

// Count how many metrics have WARNING or DANGER status
int count_issues(const std::vector<ResourceResult>& results);

// Right-pad a string to at least `width` characters
std::string pad_right(const std::string& s, int width);

} // namespace utils

#endif // UTILS_H
