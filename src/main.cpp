#include "collectors.h"
#include "reporters.h"
#include "utils.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n\n"
              << "USE Method: Linux Performance Analysis Tool\n"
              << "Based on Brendan Gregg's USE Method checklist.\n\n"
              << "Options:\n"
              << "  --mode MODE      full | quick | summary (default: full)\n"
              << "  --resource LIST  Comma-separated: cpu,memory,network,storage,software\n"
              << "                   (default: all)\n"
              << "  --output FMT     terminal | json (default: terminal)\n"
              << "  --help           Show this help\n\n"
              << "Examples:\n"
              << "  " << prog << "                    # Full check, terminal output\n"
              << "  " << prog << " --mode quick         # 60-second quick check\n"
              << "  " << prog << " --resource cpu,mem   # CPU + memory only\n"
              << "  " << prog << " --output json        # JSON output\n"
              << "  " << prog << " --mode summary       # Only show problems\n";
}

int main(int argc, char* argv[]) {
    std::string mode = "full";
    std::string output_fmt = "terminal";
    std::vector<std::string> resources;

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
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

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
