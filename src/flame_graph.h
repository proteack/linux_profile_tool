#ifndef FLAME_GRAPH_H
#define FLAME_GRAPH_H

#include <string>
#include <vector>
#include "perf_collector.h"

// Generate an SVG flame graph from aggregated stack data.
// Returns the output path on success, empty string on failure.
std::string generate_flame_graph(const std::vector<AggregatedStack>& stacks,
                                 const std::string& output_path = "");

#endif // FLAME_GRAPH_H
