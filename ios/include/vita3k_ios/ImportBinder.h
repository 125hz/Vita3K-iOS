#pragma once

#include <cstddef>
#include <span>
#include <string>

#include <vita3k_ios/ModuleTableParser.h>

namespace vita3k::ios {

class GuestMemory;

struct ImportBindingResult {
    bool success = false;
    std::size_t bound_function_count = 0;
    std::string detail;
};

ImportBindingResult bind_import_stubs(GuestMemory &memory,
    std::span<const ImportedFunctionStub> functions);

} // namespace vita3k::ios
