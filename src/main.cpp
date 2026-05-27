#include "collectors.h"
#include "executor.h"
#include "reporters.h"
#include "perf_collector.h"
#include "flame_graph.h"
#include "utils.h"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n\n"
              << "USE Method: Linux Performance Analysis Tool\n"
              << "Based on Brendan Gregg's USE Method checklist.\n\n"
              << "Options:\n"
              << "  --mode MODE          full | quick | summary (default: full)\n"
              << "  --resource LIST      Comma-separated: cpu,memory,network,storage,software\n"
              << "                       (default: all)\n"
              << "  --output FMT         terminal | json (default: terminal)\n"
              << "  --perf               Perf profiling mode (uses Linux perf)\n"
              << "  --time SECONDS       Profiling duration (default: 30)\n"
              << "  --pid PID            Profile specific process\n"
              << "  --freq HZ            Sampling frequency (default: 99)\n"
              << "  --flame              Generate CPU flame graph SVG\n"
              << "  --flame-output PATH  Flame graph output path\n"
              << "  --help               Show this help\n\n"
              << "Examples:\n"
              << "  " << prog << "                              # Full check, terminal\n"
              << "  " << prog << " --mode quick                   # 60-second quick check\n"
              << "  " << prog << " --resource cpu,mem             # CPU + memory only\n"
              << "  " << prog << " --perf --time 60 --flame        # Profile 60s + flame graph\n"
              << "  " << prog << " --perf --pid 1234 --time 30     # Profile PID 1234\n"
              << "  " << prog << " --perf --flame --flame-out /tmp/f.svg\n";
}

int main(int argc, char* argv[]) {
    std::string mode = "full";
    std::string output_fmt = "terminal";
    std::vector<std::string> resources;

    // Perf profiling options
    bool perf_mode = false;
    int perf_duration = 30;
    int perf_pid = -1;
    int perf_freq = 99;
    bool gen_flame = false;
    std::string flame_output;

    // Simple argument parsing
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output_fmt = argv[++i];
        } else if (arg == "--resource" && i + 1 < argc) {
            std::string list = argv[++i];
            auto parts = utils::split(list, ',');
            for (auto& p : parts) {
                std::string r = utils::strip(p);
                if (!r.empty()) resources.push_back(r);
            }
        } else if (arg == "--perf") {
            perf_mode = true;
        } else if (arg == "--time" && i + 1 < argc) {
            perf_duration = std::atoi(argv[++i]);
            if (perf_duration < 1) perf_duration = 1;
        } else if (arg == "--pid" && i + 1 < argc) {
            perf_pid = std::atoi(argv[++i]);
        } else if (arg == "--freq" && i + 1 < argc) {
            perf_freq = std::atoi(argv[++i]);
            if (perf_freq < 1) perf_freq = 1;
        } else if (arg == "--flame") {
            gen_flame = true;
        } else if (arg == "--flame-output" && i + 1 < argc) {
            flame_output = argv[++i];
            gen_flame = true;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    // ================================================================
    // PERF PROFILING MODE
    // ================================================================
    if (perf_mode) {
        if (!perf_available()) {
            std::cerr << "Error: perf is not usable on this system.\n"
                      << "  Try: sudo apt install " << perf_tool_package_hint() << "\n";
            return 1;
        }

        std::cout << "\n" << utils::color_status(Status::INFO)
                  << "=== Perf Profiling ==="
                  << utils::color_reset() << "\n";

        if (perf_pid > 0)
            std::cout << "Target: PID " << perf_pid << "\n";
        else
            std::cout << "Target: system-wide (-a)\n";

        std::cout << "Duration: " << perf_duration << "s"
                  << "  Freq: " << perf_freq << " Hz"
                  << (gen_flame ? "  + Flame Graph" : "")
                  << "\n";

        std::cout << "Profiling... (this will take "
                  << perf_duration << " seconds)\n" << std::flush;

        PerfResult result;
        if (gen_flame) {
            result = collect_perf_with_stacks(perf_duration, perf_pid, perf_freq);
        } else {
            result = collect_perf(perf_duration, perf_pid, perf_freq);
        }

        if (!result.has_data) {
            std::cerr << utils::color_status(Status::DANGER)
                      << "\nPerf profiling failed.\n"
                      << utils::color_reset();
            if (!result.error_msg.empty()) {
                std::string err = result.error_msg;
                while (!err.empty() && (err.back() == '\n' || err.back() == ' '))
                    err.pop_back();

                // Detect kernel-version mismatch and suggest the right package
                bool kernel_mismatch = (err.find("not found for kernel") != std::string::npos);

                std::cerr << "perf stderr:\n";
                auto err_lines = utils::split(err, '\n');
                for (size_t i = 0; i < err_lines.size() && i < 20; i++) {
                    std::string l = utils::strip(err_lines[i]);
                    if (!l.empty())
                        std::cerr << "  " << l << "\n";
                }

                if (kernel_mismatch) {
                    std::cerr << "\n"
                              << "  Install: sudo apt install "
                              << perf_tool_package_hint() << "\n";
                }
            } else {
                std::cerr << "Check that:\n"
                          << "  1. perf is installed (apt install linux-tools-$(uname -r))\n"
                          << "  2. You have root / CAP_PERFMON\n"
                          << "  3. /proc/sys/kernel/perf_event_paranoid <= 1\n"
                          << "  4. Kernel lockdown is not enabled\n";
            }
            return 1;
        }

        // --- print hotspots ---
        std::cout << "\n" << utils::color_status(Status::INFO)
                  << "--- Top CPU Consumers ---"
                  << utils::color_reset() << "\n";
        std::cout << "  " << utils::colorize("Kernel: ", Status::INFO)
                  << std::fixed << std::setprecision(1)
                  << result.kernel_pct << "%"
                  << "  " << utils::colorize("User: ", Status::INFO)
                  << result.user_pct << "%"
                  << "  " << utils::colorize("Idle: ", Status::INFO)
                  << result.idle_pct << "%\n\n";

        // Table header
        std::cout << "  " << utils::color_status(Status::INFO)
                  << utils::pad_right("Overhead", 9)
                  << utils::pad_right("Process", 18)
                  << utils::pad_right("Symbol", 40)
                  << "Location"
                  << utils::color_reset() << "\n";
        std::cout << "  " << std::string(90, '-') << "\n";

        int shown = 0;
        for (const auto& hs : result.hotspots) {
            if (shown++ >= 30) break;
            Status st = hs.overhead > 20.0 ? Status::DANGER
                      : hs.overhead > 10.0 ? Status::WARNING
                      : Status::OK;

            std::ostringstream pct_str;
            pct_str << std::fixed << std::setprecision(1) << hs.overhead << "%";
            std::string sym = hs.symbol;
            if (sym.length() > 38) sym = sym.substr(0, 35) + "...";

            std::cout << "  "
                      << utils::colorize(utils::pad_right(pct_str.str(), 9), st) << " "
                      << utils::pad_right(hs.comm.length() > 17
                            ? hs.comm.substr(0, 16) + "."
                            : hs.comm, 18)
                      << utils::pad_right(sym, 40)
                      << (hs.is_kernel ? "[kernel]" : "[user]")
                      << "\n";
        }
        if (result.hotspots.size() > 30) {
            std::cout << "  ... and " << (result.hotspots.size() - 30)
                      << " more\n";
        }

        // --- anomalies ---
        if (!result.anomalies.empty()) {
            std::cout << "\n" << utils::color_status(Status::DANGER)
                      << "--- Anomalies Detected ---"
                      << utils::color_reset() << "\n";
            for (const auto& a : result.anomalies) {
                Status ast = Status::WARNING;
                if (a.find("HIGH CPU") == 0 || a.find("LOCK CONTENTION") == 0)
                    ast = Status::DANGER;
                std::cout << "  " << utils::colorize("!", ast) << " " << a << "\n";
            }
        } else {
            std::cout << "\n" << utils::color_status(Status::OK)
                      << "  No significant anomalies detected.\n"
                      << utils::color_reset();
        }

        // --- flame graph ---
        if (gen_flame && !result.stacks.empty()) {
            std::cout << "\n" << utils::color_status(Status::INFO)
                      << "--- Generating Flame Graph ---"
                      << utils::color_reset() << "\n";
            std::cout << "  Stack frames: " << result.stacks.size() << "\n";

            std::string out_path = generate_flame_graph(result.stacks, flame_output);
            if (!out_path.empty()) {
                std::cout << "  " << utils::colorize("SVG:", Status::OK)
                          << " " << out_path << "\n";

                // Check for external SVG viewer / browser
                if (utils::is_installed("xdg-open")) {
                    std::cout << "  Opening...\n" << std::flush;
                    std::string open_cmd = "xdg-open " + out_path + " 2>/dev/null &";
                    executor::run_cmd(open_cmd);
                }
            } else {
                std::cout << "  " << utils::color_status(Status::DANGER)
                          << "Failed to generate flame graph.\n"
                          << utils::color_reset();
            }
        } else if (gen_flame && result.stacks.empty()) {
            std::cout << "\n" << utils::color_status(Status::WARNING)
                      << "  No stack data available for flame graph.\n"
                      << "  Try a longer sampling duration or check perf permissions.\n"
                      << utils::color_reset();
        }

        // Summary
        std::cout << "\n" << std::string(50, '=') << "\n";
        std::cout << "Perf Summary: "
                  << result.hotspots.size() << " functions, "
                  << result.anomalies.size() << " anomaly(ies)\n";

        return 0;
    }

    // ================================================================
    // NORMAL USE METHOD MODE
    // ================================================================

    // Validate mode
    if (mode != "full" && mode != "quick" && mode != "summary") {
        std::cerr << "Invalid mode: " << mode << " (use full, quick, or summary)\n";
        return 1;
    }

    // Validate output format
    if (output_fmt != "terminal" && output_fmt != "json") {
        std::cerr << "Invalid output format: " << output_fmt << " (use terminal or json)\n";
        return 1;
    }

    // Check for required commands
    std::vector<std::string> required = {"vmstat", "mpstat"};
    for (auto& cmd : required) {
        if (!utils::is_installed(cmd)) {
            std::cerr << "Warning: '" << cmd << "' not found. Install sysstat package.\n";
        }
    }

    // Collect data
    std::vector<ResourceResult> results;
    if (mode == "quick") {
        results = collect_quick();
    } else {
        results = collect_all(resources);
    }

    // Report
    if (output_fmt == "json") {
        JsonReporter reporter;
        reporter.report(results);
    } else {
        TableReporter reporter(mode != "summary");
        reporter.report(results);
    }

    return 0;
}
