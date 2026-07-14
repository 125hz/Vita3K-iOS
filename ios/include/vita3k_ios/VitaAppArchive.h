#pragma once

#include <cstdint>
#include <vector>

namespace vita3k::ios {

// Creates a tiny, legal VPK-shaped ZIP used only to prove that package metadata
// can cross the iOS bridge. It contains no Sony or game data.
std::vector<std::uint8_t> make_synthetic_vpk(bool unsafe_path = false);
std::vector<std::uint8_t> make_synthetic_install_zip();

} // namespace vita3k::ios
