#include "fh/util/sysinfo.hpp"

#include <sys/utsname.h>

#include <fstream>
#include <set>
#include <thread>

#include "fh/stats/clock.hpp"

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#ifndef FH_CXX_FLAGS
#define FH_CXX_FLAGS "unknown"
#endif
#ifndef FH_BUILD_TYPE
#define FH_BUILD_TYPE "unknown"
#endif

namespace fh {

namespace {
#if defined(__APPLE__)
std::string sysctl_string(const char* name) {
    char buf[256] = {};
    size_t len = sizeof buf;
    if (sysctlbyname(name, buf, &len, nullptr, 0) != 0) return {};
    return buf;
}
int sysctl_int(const char* name) {
    int v = 0;
    size_t len = sizeof v;
    if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return 0;
    return v;
}
#endif
}  // namespace

SystemInfo collect_system_info() {
    SystemInfo s;
    s.logical_cores = static_cast<int>(std::thread::hardware_concurrency());
#if defined(__APPLE__)
    s.cpu_model = sysctl_string("machdep.cpu.brand_string");
    s.physical_cores = sysctl_int("hw.physicalcpu");
#elif defined(__linux__)
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    std::set<std::pair<std::string, std::string>> cores;  // (physical id, core id)
    std::string phys;
    while (std::getline(in, line)) {
        auto value = [&] { auto p = line.find(':'); return p == std::string::npos ? std::string() : line.substr(p + 2); };
        if (s.cpu_model.empty() && line.rfind("model name", 0) == 0) s.cpu_model = value();
        if (line.rfind("physical id", 0) == 0) phys = value();
        if (line.rfind("core id", 0) == 0) cores.insert({phys, value()});
    }
    s.physical_cores = cores.empty() ? s.logical_cores : static_cast<int>(cores.size());
    if (s.cpu_model.empty()) s.cpu_model = "unknown (" + std::string(
#if defined(__aarch64__)
        "aarch64"
#else
        "unknown arch"
#endif
        ) + ")";
#endif
    struct utsname u {};
    if (uname(&u) == 0) s.os = std::string(u.sysname) + " " + u.release + " " + u.machine;
#if defined(__clang__)
    s.compiler = std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    s.compiler = std::string("gcc ") + __VERSION__;
#endif
    s.flags = FH_CXX_FLAGS;
    s.build_type = FH_BUILD_TYPE;
    s.clock_source = clock_source_name();
    s.clock_hz = ticks_per_second();
    s.clock_resolution_ns = clock_resolution_ns();
    return s;
}

}  // namespace fh
