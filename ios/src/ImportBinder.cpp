#include <vita3k_ios/ImportBinder.h>

#include <vita3k_ios/GuestMemory.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace vita3k::ios {

ImportBindingResult bind_import_stubs(GuestMemory &memory,
    std::span<const ImportedFunctionStub> functions) {
    ImportBindingResult result;
    std::vector<std::uint32_t> bound_addresses;
    bound_addresses.reserve(functions.size());

    for (const auto &function : functions) {
        if (function.nid == 0 || function.stub_address == 0 ||
            (function.stub_address & 3u) != 0) {
            result.detail = "An imported function has a zero or misaligned NID/stub address.";
            return result;
        }
        if (std::ranges::find(bound_addresses, function.stub_address) != bound_addresses.end()) {
            result.detail = "Two imported functions reference the same stub address.";
            return result;
        }

        // This is Vita3K's unresolved ARM import trampoline: SVC enters the
        // HLE dispatcher, MOV pc,lr returns to the caller, and the trailing
        // word identifies the imported NID to the SVC hook.
        const std::array<std::uint32_t, 3> trampoline{
            0xEF000000u,
            0xE1A0F00Eu,
            function.nid
        };
        std::array<std::uint8_t, sizeof(trampoline)> bytes{};
        std::memcpy(bytes.data(), trampoline.data(), bytes.size());
        std::string error;
        if (!memory.write(function.stub_address, bytes, error)) {
            result.detail = "Could not rewrite an imported function stub: " + error;
            return result;
        }
        std::array<std::uint8_t, sizeof(trampoline)> observed{};
        if (!memory.read(function.stub_address, observed, error) || observed != bytes) {
            result.detail = "Imported function stub readback failed: " + error;
            return result;
        }
        bound_addresses.push_back(function.stub_address);
        ++result.bound_function_count;
    }

    result.success = true;
    std::ostringstream detail;
    detail << "Rewrote and verified " << result.bound_function_count
           << " ARM import stubs.";
    result.detail = detail.str();
    return result;
}

} // namespace vita3k::ios
