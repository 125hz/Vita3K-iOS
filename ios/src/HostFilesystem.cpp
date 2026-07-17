#include <vita3k_ios/HostFilesystem.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>

namespace vita3k::ios {
namespace {

bool is_import_candidate(const std::filesystem::path &path) {
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    constexpr std::array supported_extensions{
        ".bin", ".elf", ".self", ".sfo", ".velf", ".vpk", ".zip"
    };
    return std::ranges::find(supported_extensions, extension) != supported_extensions.end();
}

} // namespace

void scan_imports(HostStorage &storage) {
    storage.imported_files.clear();
    if (!storage.ready) {
        return;
    }

    std::error_code error;
    for (std::filesystem::directory_iterator iterator(storage.imports, error), end;
         iterator != end && !error; iterator.increment(error)) {
        if (iterator->is_regular_file(error) && !error && is_import_candidate(iterator->path())) {
            storage.imported_files.push_back(*iterator);
        }
    }

    if (error) {
        storage.error = "Could not scan imports: " + error.message();
        return;
    }

    std::ranges::sort(storage.imported_files, {}, [](const auto &entry) {
        return entry.path().filename().string();
    });
}

HostStorage initialize_host_storage(const std::filesystem::path &documents_root) {
    HostStorage storage;
    storage.root = documents_root / "Tsubomi";
    storage.imports = storage.root / "imports";

    constexpr std::array directories{
        "imports",
        "logs",
        "ux0/app",
        "ux0/patch",
        "ux0/addcont",
        "ux0/data",
        "ux0/user"
    };

    std::error_code error;
    for (const auto *directory : directories) {
        std::filesystem::create_directories(storage.root / directory, error);
        if (error) {
            storage.error = "Could not create " + std::string(directory) + ": " + error.message();
            return storage;
        }
    }

    storage.ready = true;
    scan_imports(storage);
    return storage;
}

} // namespace vita3k::ios
