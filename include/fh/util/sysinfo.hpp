// Host description written next to every benchmark result.
#pragma once

#include <string>

namespace fh {

struct SystemInfo {
    std::string cpu_model;
    int logical_cores = 0;
    int physical_cores = 0;
    std::string os;
    std::string compiler;
    std::string flags;
    std::string build_type;
    std::string clock_source;
    double clock_hz = 0;
    double clock_resolution_ns = 0;  // measured step, may exceed 1/clock_hz
};

SystemInfo collect_system_info();

}  // namespace fh
