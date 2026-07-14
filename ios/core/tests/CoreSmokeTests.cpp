#include <vita3k_ios/CoreBridge.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

template <typename T, std::size_t Size>
void write_value(std::array<std::uint8_t, Size> &image, std::size_t offset, T value) {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

} // namespace

int main() {
    const auto test_root = std::filesystem::temp_directory_path() / "vita3k-ios-core-smoke-test";
    std::error_code error;
    std::filesystem::remove_all(test_root, error);

    const auto status = vita3k::ios::initialize_core(test_root);
    if (!status.linked || !status.self_tests_passed || !status.storage_ready ||
        !status.guest_memory_ready || !status.segment_mapping_ready ||
        status.guest_memory_size != (1ULL << 32)) {
        std::cerr << status.summary << '\n';
        return 1;
    }

    if (!status.imported_artifacts.empty()) {
        std::cerr << "A fresh import directory should be empty.\n";
        return 2;
    }

    // Three program headers: two adjacent PT_LOAD segments that share one
    // 4 KiB Windows host page, plus one PT_SCE_RELA inventory entry.
    std::array<std::uint8_t, 300> elf{};
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
    write_value(elf, 28, static_cast<std::uint32_t>(52));
    write_value(elf, 40, static_cast<std::uint16_t>(52));
    write_value(elf, 42, static_cast<std::uint16_t>(32));
    write_value(elf, 44, static_cast<std::uint16_t>(3));

    // PT_LOAD #0 contains a complete 0x5C-byte Vita module-info header.
    write_value(elf, 52, static_cast<std::uint32_t>(1));
    write_value(elf, 56, static_cast<std::uint32_t>(148));
    write_value(elf, 60, static_cast<std::uint32_t>(0x81000000));
    write_value(elf, 64, static_cast<std::uint32_t>(0x81000000));
    write_value(elf, 68, static_cast<std::uint32_t>(128));
    write_value(elf, 72, static_cast<std::uint32_t>(0x800));
    write_value(elf, 76, static_cast<std::uint32_t>(5));
    write_value(elf, 80, static_cast<std::uint32_t>(4096));

    // PT_LOAD #1 is byte-adjacent to #0 but has different guest permissions.
    write_value(elf, 84, static_cast<std::uint32_t>(1));
    write_value(elf, 88, static_cast<std::uint32_t>(276));
    write_value(elf, 92, static_cast<std::uint32_t>(0x81000800));
    write_value(elf, 96, static_cast<std::uint32_t>(0x81000800));
    write_value(elf, 100, static_cast<std::uint32_t>(16));
    write_value(elf, 104, static_cast<std::uint32_t>(0x800));
    write_value(elf, 108, static_cast<std::uint32_t>(6));
    write_value(elf, 112, static_cast<std::uint32_t>(4096));

    // PT_SCE_RELA is inventoried but deliberately not applied in Milestone 4.
    write_value(elf, 116, static_cast<std::uint32_t>(0x60000000));
    write_value(elf, 120, static_cast<std::uint32_t>(292));
    write_value(elf, 132, static_cast<std::uint32_t>(8));
    write_value(elf, 136, static_cast<std::uint32_t>(8));
    write_value(elf, 144, static_cast<std::uint32_t>(4));

    write_value(elf, 150, static_cast<std::uint16_t>(0x0101));
    constexpr char module_name[] = "synthetic-homebrew";
    std::memcpy(elf.data() + 152, module_name, sizeof(module_name) - 1);
    write_value(elf, 200, static_cast<std::uint32_t>(0x1234ABCD));
    for (std::size_t index = 276; index < 292; ++index) {
        elf[index] = static_cast<std::uint8_t>(index - 260);
    }

    const auto fixture = test_root / "Vita3K" / "imports" / "synthetic-homebrew.elf";
    std::ofstream fixture_stream(fixture, std::ios::binary);
    fixture_stream.write(reinterpret_cast<const char *>(elf.data()), elf.size());
    fixture_stream.close();

    const auto rescanned = vita3k::ios::rescan_imports();
    if (rescanned.imported_artifacts.size() != 1 ||
        rescanned.imported_artifacts.front().kind != "Vita ELF" ||
        !rescanned.imported_artifacts.front().structurally_valid ||
        rescanned.imported_artifacts.front().load_segment_count != 2 ||
        rescanned.imported_artifacts.front().relocation_segment_count != 1 ||
        !rescanned.imported_artifacts.front().loaded ||
        !rescanned.imported_artifacts.front().module_info_valid ||
        rescanned.imported_artifacts.front().module_name != "synthetic-homebrew" ||
        rescanned.imported_artifacts.front().module_nid != 0x1234ABCD) {
        std::cerr << rescanned.summary << '\n';
        return 3;
    }

    std::filesystem::remove_all(test_root, error);
    std::cout << rescanned.summary << '\n';
    return 0;
}
