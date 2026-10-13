#pragma once
#include <string>
// Run once per process; stopping/restarting the server is intentionally
// refused.
std::string RunOhosForkServerTests(const std::string& child_library);
