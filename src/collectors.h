#ifndef COLLECTORS_H
#define COLLECTORS_H

#include <vector>
#include "types.h"

// CPU
ResourceResult collect_cpu();

// Memory
ResourceResult collect_memory();

// Network interfaces
std::vector<ResourceResult> collect_network();

// Storage devices + capacity
std::vector<ResourceResult> collect_storage();

// Software resources (FD, tasks, locks)
ResourceResult collect_software();

// Quick 60s check (lightweight subset)
std::vector<ResourceResult> collect_quick();

// Discover which resources to check
std::vector<ResourceResult> collect_all(const std::vector<std::string>& resources);

#endif // COLLECTORS_H
