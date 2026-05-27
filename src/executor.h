#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <string>
#include <vector>

namespace executor {

// Run a shell command and return its stdout as a single string
std::string run_cmd(const std::string& cmd);

// Run a shell command and return its stdout split into lines
std::vector<std::string> run_cmd_lines(const std::string& cmd);

} // namespace executor

#endif // EXECUTOR_H
