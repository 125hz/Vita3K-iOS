// This translation unit gives the unsigned iOS shell a real reference to the
// upstream Vita3K entry layer. It deliberately starts no emulation yet; its job
// in the first upstream-core stage is to make the final device link prove that
// interface.cpp and its complete library dependency graph are linkable.

#include "../../vita3k/interface.h"

#include <cstdint>

namespace {

using RunAppEntry = ExitCode (*)(EmuEnvState &, std::int32_t);

__attribute__((used)) const std::uintptr_t upstream_core_link_anchor =
    reinterpret_cast<std::uintptr_t>(static_cast<RunAppEntry>(&run_app));

} // namespace
