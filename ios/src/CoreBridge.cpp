#include <vita3k_ios/CoreBridge.h>

namespace vita3k::ios {

CoreStatus query_core_status() {
    return {
        .linked = false,
        .summary = "Bootstrap IPA only: the upstream Vita3K core is not iOS-compatible or linked yet."
    };
}

} // namespace vita3k::ios
