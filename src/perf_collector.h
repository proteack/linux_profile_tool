#ifndef PERF_COLLECTOR_H
#define PERF_COLLECTOR_H

#include <string>
#include <vector>
#include "types.h"

struct PerfHotspot {
    std::string  symbol;
    std::string  comm;
    std::string  dso;
    double       overhead; // percentage 0-100
    bool         is_kernel;
};

struct AggregatedStack {
    std::vector<std::string> frames; // leaf → root
    uint64_t                 count;
    std::string              comm;
};

struct PerfResult {
    std::vector<PerfHotspot>  hotspots;
    double                    kernel_pct;
    double                    user_pct;
    double                    idle_pct;
    std::vector<AggregatedStack> stacks;
    std::vector<std::string>  anomalies;
    std::string               error_msg;     // stderr from perf on failure
    int                       duration;
    int                       pid;
    int                       freq;
    bool                      has_data;
};

bool perf_available();

// Returns package name like "linux-tools-5.4.0-150-generic"
std::string perf_tool_package_hint();

// Run perf profiling and collect results
PerfResult collect_perf(int duration_sec, int pid = -1, int freq = 99);

// Run perf profiling and also collect stack data for flame graph
PerfResult collect_perf_with_stacks(int duration_sec, int pid = -1, int freq = 99);

// Anomaly detection on perf results (also called internally)
std::vector<std::string> detect_perf_anomalies(const PerfResult& result);

#endif // PERF_COLLECTOR_H
