// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>

namespace craw::tui {

int runTui(const std::string& startDir, const std::vector<std::string>& preload, const std::string& outputDir);

}  // namespace craw::tui
