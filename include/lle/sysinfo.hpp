#pragma once

#include <cstdio>
#include <cstring>
#include <string>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#endif

namespace lle {

inline std::string host_description() {
#if defined(__APPLE__)
    char buf[256];
    std::size_t len = sizeof buf;
    if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0) return std::string(buf);
#elif defined(__linux__)
    if (std::FILE* f = std::fopen("/proc/cpuinfo", "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            if (std::strncmp(line, "model name", 10) == 0) {
                std::fclose(f);
                const char* v = std::strchr(line, ':');
                std::string s = v ? v + 2 : line;
                while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
                return s;
            }
        }
        std::fclose(f);
    }
#endif
    return "unknown";
}

// Hardware tick of the clock behind steady_clock, in ns. On Apple Silicon the timebase is
// 125/3, i.e. a 24 MHz counter, even though steady_clock reports nanoseconds.
inline double timer_tick_ns() {
#if defined(__APPLE__)
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return static_cast<double>(tb.numer) / static_cast<double>(tb.denom);
#else
    return 1.0;
#endif
}

}  // namespace lle
