#include <vita3k_ios/CoreBridge.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

template <typename T>
void write_value(std::array<std::uint8_t, 52> &image, std::size_t offset, T value) {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

} // namespace

int main() {
    const auto test_root = std::filesystem::temp_directory_path() / "vita3k-ios-core-smoke-test";
    std::error_code error;
    std::filesystem::remove_all(test_root, error);

    const auto status = vita3k::ios::initialize_core(test_root);
    if (!status.linked || !status.self_tests_passed || !status.storage_ready) {
        std::cerr << status.summary << '\n';
        return 1;
    }

    if (!status.imported_artifacts.empty()) {
        std::cerr << "A fresh import directory should be empty.\n";
        return 2;
    }

    std::array<std::uint8_t, 52> elf{};
    elf[0] = 0x7F;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 1;
    elf[5] = 1;
    elf[6] = 1;
    write_value(elf, 16, static_cast<std::uint16_t>(0xFE00));
    write_value(elf, 18, static_cast<std::uint16_t>(0x28));
    write_value(elf, 20, static_cast<std::uint32_t>(1));
    write_value(elf, 40, static_cast<std::uint16_t>(elf.size()));
    write_value(elf, 42, static_cast<std::uint16_t>(32));

    const auto fixture = test_root / "Vita3K" / "imports" / "synthetic-homebrew.elf";
    std::ofstream fixture_stream(fixture, std::ios::binary);
    fixture_stream.write(reinterpret_cast<const char *>(elf.data()), elf.size());
    fixture_stream.close();

    const auto rescanned = vita3k::ios::rescan_imports();
    if (rescanned.imported_artifacts.size() != 1 ||
        rescanned.imported_artifacts.front().kind != "Vita ELF" ||
        !rescanned.imported_artifacts.front().structurally_valid) {
        std::cerr << rescanned.summary << '\n';
        return 3;
    }

    std::filesystem::remove_all(test_root, error);
    std::cout << status.summary << '\n';
    return 0;
}
