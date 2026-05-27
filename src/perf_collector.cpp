#include "perf_collector.h"
#include "executor.h"
#include "utils.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

static std::string perf_data_file() {
    return "/tmp/use-linux-perf-perf.data";
}

// -------------------------------------------------------------------
// Check whether perf is installed and usable
// -------------------------------------------------------------------
bool perf_available() {
    return utils::is_installed("perf");
}

// -------------------------------------------------------------------
// Run perf record
// -------------------------------------------------------------------
static bool run_perf_record(int duration_sec, int pid, int freq) {
    std::string data_file = perf_data_file();
    ::unlink(data_file.c_str());

    std::string cmd = "perf record -a -g -F " + std::to_string(freq);
    if (pid > 0) {
        cmd = "perf record -g -F " + std::to_string(freq) + " -p " + std::to_string(pid);
    }
    cmd += " -o " + data_file + " -- sleep " + std::to_string(duration_sec);
    cmd += " 2>/dev/null";

    executor::run_cmd(cmd);

    struct stat st;
    if (::stat(data_file.c_str(), &st) != 0 || st.st_size == 0) {
        return false;
    }
    return true;
}

// -------------------------------------------------------------------
// Parse perf report --stdio output
// NOTE: does NOT clean up perf.data — caller handles that
// -------------------------------------------------------------------
static void parse_perf_report_into(PerfResult& result) {
    std::string data_file = perf_data_file();
    std::string report = executor::run_cmd(
        "perf report -i " + data_file + " --stdio --no-header 2>/dev/null");

    if (report.empty()) {
        result.has_data = false;
        return;
    }
    result.has_data = true;

    // Parse the report line by line
    // Format:
    //    Overhead  Command   Shared Object     Symbol
    //    .......   ........  ................  ......................
    //    25.37%   swapper   [kernel.kallsyms]  [k] native_safe_halt
    //     8.42%   swapper   [kernel.kallsyms]  [k] intel_idle
    //     5.21%   mysqld    mysqld             [.] my_func

    double kernel_total = 0.0, user_total = 0.0, idle_total = 0.0;

    auto lines = utils::split(report, '\n');
    for (const auto& line : lines) {
        std::string l = utils::strip(line);
        if (l.empty() || l[0] == '#' || l.find("=====") != std::string::npos) {
            continue;
        }
        if (!std::isdigit(l[0]) && l[0] != '.') continue;

        auto tokens = utils::split(l, ' ');
        std::vector<std::string> cols;
        for (auto& t : tokens) {
            std::string s = utils::strip(t);
            if (!s.empty()) cols.push_back(s);
        }
        if (cols.size() < 4) continue;

        std::string pct_str = cols[0];
        if (pct_str.back() != '%') continue;
        double pct = utils::parse_percent(pct_str);
        if (pct < 0.01) continue;

        std::string comm = cols[1];
        std::string dso = cols[2];

        std::string symbol;
        bool is_kernel = false;
        for (size_t i = 3; i < cols.size(); i++) {
            if (cols[i] == "[k]" || cols[i] == "[.]" || cols[i] == "[g]") {
                is_kernel = (cols[i] == "[k]" || cols[i] == "[g]");
                continue;
            }
            if (cols[i][0] == '[') continue;
            if (!symbol.empty()) symbol += " ";
            symbol += cols[i];
        }
        if (symbol.empty()) symbol = "<" + dso + ">";

        if (is_kernel) {
            kernel_total += pct;
            if (symbol.find("native_safe_halt") != std::string::npos ||
                symbol.find("intel_idle") != std::string::npos ||
                symbol.find("cpu_idle") != std::string::npos ||
                symbol.find("cpuidle") != std::string::npos ||
                symbol.find("poll_idle") != std::string::npos) {
                idle_total += pct;
            }
        } else {
            user_total += pct;
        }

        result.hotspots.push_back({symbol, comm, dso, pct, is_kernel});
    }

    result.kernel_pct = kernel_total;
    result.user_pct = user_total;
    result.idle_pct = idle_total;

    result.anomalies = detect_perf_anomalies(result);
}

// -------------------------------------------------------------------
// Parse perf report output for folded stack data (flame graph input)
// Prefers folded format; falls back to parsing perf script output.
// NOTE: does NOT clean up perf.data — caller handles that.
// -------------------------------------------------------------------
static void add_stack_data(PerfResult& result) {
    std::string data_file = perf_data_file();

    // Try folded format first (perf report -g folded):
    //   comm;sym_root;sym_1;...;sym_leaf <count>
    std::string folded = executor::run_cmd(
        "perf report -i " + data_file + " -g folded --no-header -s comm 2>/dev/null");

    if (!folded.empty()) {
        auto lines = utils::split(folded, '\n');
        for (const auto& line : lines) {
            std::string l = utils::strip(line);
            if (l.empty()) continue;

            size_t last_space = l.rfind(' ');
            if (last_space == std::string::npos) continue;

            std::string stack_str = l.substr(0, last_space);
            std::string count_str = l.substr(last_space + 1);

            uint64_t count = 0;
            try { count = std::stoull(count_str); } catch (...) { continue; }
            if (count == 0) continue;

            // folded format: comm;func_root;...;func_leaf
            auto parts = utils::split(stack_str, ';');
            if (parts.size() < 2) continue;

            AggregatedStack as;
            as.comm = utils::strip(parts[0]);
            // Reverse from root→leaf to leaf→root for SVG rendering
            for (int i = (int)parts.size() - 1; i >= 1; i--) {
                as.frames.push_back(utils::strip(parts[i]));
            }
            as.count = count;
            result.stacks.push_back(as);
        }
    }

    if (result.stacks.empty()) {
        // Fallback: parse standard perf script output
        std::string script_out = executor::run_cmd(
            "perf script -i " + data_file + " 2>/dev/null");
        if (!script_out.empty()) {
            std::vector<std::string> current_stack;
            std::string current_comm;
            bool in_sample = false;

            auto script_lines = utils::split(script_out, '\n');
            for (size_t i = 0; i < script_lines.size(); i++) {
                std::string l = script_lines[i];

                if (l.empty()) {
                    if (!current_stack.empty() && in_sample) {
                        AggregatedStack as;
                        as.comm = current_comm;
                        as.frames = current_stack;
                        as.count = 1;
                        result.stacks.push_back(as);
                    }
                    current_stack.clear();
                    in_sample = false;
                    continue;
                }

                if (!l.empty() && l[0] != ' ') {
                    size_t space_pos = l.find(' ');
                    current_comm = (space_pos != std::string::npos)
                                       ? l.substr(0, space_pos) : "";
                    in_sample = true;
                    current_stack.clear();
                    continue;
                }

                if (!in_sample) continue;

                l = utils::strip(l);
                if (l.empty()) continue;

                // Extract symbol from "address symbol (dso)"
                size_t addr_end = l.find(' ');
                if (addr_end == std::string::npos) continue;
                std::string rest = utils::strip(l.substr(addr_end));
                if (rest.empty()) continue;

                size_t paren = rest.rfind('(');
                std::string symbol;
                if (paren != std::string::npos) {
                    symbol = utils::strip(rest.substr(0, paren));
                } else {
                    symbol = rest;
                }

                if (!symbol.empty()) {
                    current_stack.push_back(symbol);
                }
            }

            // Flush last sample
            if (!current_stack.empty() && in_sample) {
                AggregatedStack as;
                as.comm = current_comm;
                as.frames = current_stack;
                as.count = 1;
                result.stacks.push_back(as);
            }
        }
    }
}

// -------------------------------------------------------------------
// Anomaly detection
// -------------------------------------------------------------------
std::vector<std::string> detect_perf_anomalies(const PerfResult& result) {
    std::vector<std::string> anomalies;

    if (!result.has_data) {
        anomalies.push_back("No perf data collected — cannot analyze.");
        return anomalies;
    }

    // 1. Individual hot functions
    for (const auto& hs : result.hotspots) {
        if (hs.overhead <= 20.0) continue;
        std::string msg = "HIGH CPU: '" + hs.symbol + "' in " + hs.comm
                          + " consumes " + std::to_string((int)hs.overhead) + "% CPU";
        if (hs.is_kernel) msg += " [kernel]";
        anomalies.push_back(msg);
    }

    // 2. Kernel / user ratio
    double total = result.kernel_pct + result.user_pct;
    if (total > 10.0) {
        double kr = result.kernel_pct / total * 100.0;
        if (kr > 70.0) {
            anomalies.push_back("HIGH KERNEL TIME: " + std::to_string((int)kr)
                                + "% of CPU spent in kernel — possible syscall bottleneck");
        }
    }

    // 3. Idle
    if (result.idle_pct > 90.0) {
        anomalies.push_back("IDLE: system is " + std::to_string((int)result.idle_pct)
                            + "% idle — low utilization");
    }

    // 4. Top-5 mostly kernel?
    int kernel_top = 0, ntop = 0;
    for (const auto& hs : result.hotspots) {
        if (ntop++ >= 5) break;
        if (hs.is_kernel) kernel_top++;
    }
    if (kernel_top >= 4 && ntop >= 5) {
        anomalies.push_back("Top functions are mostly kernel — heavy system call activity detected");
    }

    // 5. swapper dominating
    double swapper_total = 0.0;
    for (const auto& hs : result.hotspots) {
        if (hs.comm == "swapper") swapper_total += hs.overhead;
    }
    if (swapper_total > 70.0) {
        anomalies.push_back("IDLE: swapper accounts for " + std::to_string((int)swapper_total)
                            + "% of samples — system is mostly idle");
    }

    // 6. Pattern-based symbol checks
    for (const auto& hs : result.hotspots) {
        if (hs.overhead < 5.0) continue;
        std::string lower = hs.symbol;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower.find("lock") != std::string::npos ||
            lower.find("spin") != std::string::npos ||
            lower.find("mutex") != std::string::npos ||
            lower.find("contend") != std::string::npos) {
            anomalies.push_back("LOCK CONTENTION: '" + hs.symbol + "' at "
                                + std::to_string((int)hs.overhead) + "% — possible locking bottleneck");
        }
        if (lower.find("alloc") != std::string::npos ||
            lower.find("malloc") != std::string::npos ||
            lower.find("free") != std::string::npos) {
            anomalies.push_back("ALLOC HEAVY: '" + hs.symbol + "' at "
                                + std::to_string((int)hs.overhead) + "% — frequent memory allocation");
        }
        if (lower.find("copy") != std::string::npos ||
            lower.find("memcpy") != std::string::npos ||
            lower.find("copy_user") != std::string::npos) {
            anomalies.push_back("DATA COPY: '" + hs.symbol + "' at "
                                + std::to_string((int)hs.overhead) + "% — heavy data movement");
        }
        if (lower.find("tlb") != std::string::npos ||
            lower.find("page_fault") != std::string::npos ||
            lower.find("do_page") != std::string::npos) {
            anomalies.push_back("PAGE MANAGEMENT: '" + hs.symbol + "' at "
                                + std::to_string((int)hs.overhead) + "% — TLB/page activity high");
        }
    }

    return anomalies;
}

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------
PerfResult collect_perf(int duration_sec, int pid, int freq) {
    PerfResult result;
    result.duration = duration_sec;
    result.pid = pid;
    result.freq = freq;
    result.has_data = false;

    if (!run_perf_record(duration_sec, pid, freq)) {
        return result;
    }

    parse_perf_report_into(result);
    ::unlink(perf_data_file().c_str());  // ← cleanup here
    return result;
}

PerfResult collect_perf_with_stacks(int duration_sec, int pid, int freq) {
    PerfResult result;
    result.duration = duration_sec;
    result.pid = pid;
    result.freq = freq;
    result.has_data = false;

    if (!run_perf_record(duration_sec, pid, freq)) {
        return result;
    }

    parse_perf_report_into(result);
    // Only try stack parsing if the report succeeded
    if (result.has_data) {
        add_stack_data(result);
    }

    ::unlink(perf_data_file().c_str());  // ← cleanup here
    return result;
}
