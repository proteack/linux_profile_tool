#include "executor.h"
#include <cstdio>
#include <memory>
#include <array>
#include <sstream>
#include <stdexcept>
#include <algorithm>

namespace executor {

std::string run_cmd(const std::string& cmd) {
    std::array<char, 4096> buffer;
    std::string result;

    // Redirect stderr too so errors don't clutter output
    std::string full_cmd = cmd + " 2>/dev/null";
    FILE* pipe = popen(full_cmd.c_str(), "r");
    if (!pipe) {
        return "";
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

    int rc = pclose(pipe);
    (void)rc; // ignore exit code — we parse what we got
    return result;
}

std::vector<std::string> run_cmd_lines(const std::string& cmd) {
    std::string output = run_cmd(cmd);
    std::vector<std::string> lines;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

} // namespace executor
