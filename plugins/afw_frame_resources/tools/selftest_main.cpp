// selftest_main.cpp
//
// Standalone offline runner for the tracker's tier-1 self-test. Links only the uevr-free core
// (FrameResourceTracker.cpp + FrameResourceLog.cpp) so the state machine can be verified on the
// build machine without launching a game. Returns 0 on PASS, 1 on FAIL.
#include "FrameResourceLog.hpp"
#include "FrameResourceTracker.hpp"

#include <cstdio>
#include <string>

int main() {
    afw_fr::set_log_level(afw_fr::LogLevel::Info);

    std::string report;
    const bool ok = afw_fr::FrameResourceTracker::run_self_test(&report);
    std::printf("[FrameResources] %s\n", report.c_str());
    return ok ? 0 : 1;
}
