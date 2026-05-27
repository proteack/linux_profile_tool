#ifndef TYPES_H
#define TYPES_H

#include <string>
#include <vector>

enum class Status { OK, WARNING, DANGER, INFO, NA };

struct Metric {
    std::string name;       // "CPU Utilization"
    std::string value;      // "72.3%"
    Status status;          // WARNING
    std::string detail;     // "us=45.2% sy=22.1% st=5.0%"
    std::string command;    // "vmstat 1 2"
};

struct ResourceResult {
    std::string resource;   // "CPU"
    std::vector<Metric> metrics;
};

struct Threshold {
    double warning;
    double danger;
};

#endif // TYPES_H
