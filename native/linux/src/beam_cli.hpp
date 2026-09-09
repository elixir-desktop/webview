#pragma once

#include "config.hpp"

namespace beamcli {

// If --edw-rpc or --edw-recover is set, run it and return true with *exit_code.
// Returns false when the host should start the UI.
bool maybe_run_exclusive(const HostConfig& cfg, int* exit_code);

int run_recover(const HostConfig& cfg);
int run_rpc(const HostConfig& cfg, const std::string& expr);

}  // namespace beamcli
