#pragma once

#include "config.hpp"

namespace beamcli {

bool maybe_run_exclusive(const HostConfig& cfg, int* exit_code);
int run_recover(const HostConfig& cfg);
int run_rpc(const HostConfig& cfg, const std::string& expr);

}  // namespace beamcli
