#pragma once

#include <string>

namespace vita3k::ios {

struct CoreStatus {
    bool linked;
    std::string summary;
};

CoreStatus query_core_status();

} // namespace vita3k::ios
