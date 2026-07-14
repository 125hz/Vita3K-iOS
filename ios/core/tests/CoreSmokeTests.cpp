#include <vita3k_ios/CoreBridge.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

template <typename T, std::size_t Size>
void write_value(std::array<std::uint8_t, Size> &image, std::size_t offset, T value) {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

} // namespace

int main(int argc, char **argv) {
    std::filesystem::path emitted_fixture;
    if (argc == 3 && std::string_view(argv[1]) == "--emit-fixture") {
        emitted_fixture = argv[2];
    } else if (argc != 1) {
        std::cerr << "Usage: vita3k_ios_core_smoke_tests [--emit-fixture <path>]\n";
        return 64;
    }
    const auto test_root = std::filesystem::temp_directory_path() / "vita3k-ios-core-smoke-test";
    std::error_code error;
    std::filesystem::remove_all(test_root, error);

    const auto status = vita3k::ios::initialize_core(test_root);
    if (!status.linked || !status.self_tests_passed || !status.storage_ready ||
        !status.guest_memory_ready || !status.segment_mapping_ready ||
        !status.loader_pipeline_ready || !status.import_binding_ready ||
        !status.arm_execution_ready ||
        !status.guest_thread_ready ||
        status.arm_test_instruction_count != 7 || status.hle_test_dispatch_count != 1 ||
        status.thread_test_instruction_count != 11 ||
        status.thread_test_hle_dispatch_count != 2 || status.thread_test_exit_status != 42 ||
        status.thread_test_bound_stub_count != 2 ||
        status.guest_memory_size != (1ULL << 32)) {
        std::cerr << status.summary << '\n';
        return 1;
    }

    if (!status.imported_artifacts.empty()) {
        std::cerr << "A fresh import directory should be empty.\n";
        return 2;
    }

    // Three program headers: two adjacent PT_LOAD segments that share one
    // 4 KiB Windows host page, plus one applied PT_SCE_RELA entry.
    std::array<std::uint8_t, 432> elf{};
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
    write_value(elf, 68, static_cast<std::uint32_t>(256));
    write_value(elf, 72, static_cast<std::uint32_t>(0x800));
    write_value(elf, 76, static_cast<std::uint32_t>(5));
    write_value(elf, 80, static_cast<std::uint32_t>(4096));

    // PT_LOAD #1 is byte-adjacent to #0 but has different guest permissions.
    write_value(elf, 84, static_cast<std::uint32_t>(1));
    write_value(elf, 88, static_cast<std::uint32_t>(404));
    write_value(elf, 92, static_cast<std::uint32_t>(0x81000800));
    write_value(elf, 96, static_cast<std::uint32_t>(0x81000800));
    write_value(elf, 100, static_cast<std::uint32_t>(16));
    write_value(elf, 104, static_cast<std::uint32_t>(0x800));
    write_value(elf, 108, static_cast<std::uint32_t>(6));
    write_value(elf, 112, static_cast<std::uint32_t>(4096));

    // PT_SCE_RELA contains one format-0 ABS32 patch for Milestone 5.
    write_value(elf, 116, static_cast<std::uint32_t>(0x60000000));
    write_value(elf, 120, static_cast<std::uint32_t>(420));
    write_value(elf, 132, static_cast<std::uint32_t>(12));
    write_value(elf, 136, static_cast<std::uint32_t>(12));
    write_value(elf, 144, static_cast<std::uint32_t>(4));

    write_value(elf, 150, static_cast<std::uint16_t>(0x0101));
    constexpr char module_name[] = "synthetic-homebrew";
    std::memcpy(elf.data() + 152, module_name, sizeof(module_name) - 1);
    write_value(elf, 184, static_cast<std::uint32_t>(0x80));
    write_value(elf, 188, static_cast<std::uint32_t>(0xA0));
    write_value(elf, 192, static_cast<std::uint32_t>(0xA0));
    write_value(elf, 196, static_cast<std::uint32_t>(0xD4));
    write_value(elf, 200, static_cast<std::uint32_t>(0x1234ABCD));
    write_value(elf, 216, static_cast<std::uint32_t>(0x60));

    constexpr std::uint32_t get_thread_id_nid = 0x0FB972F9;
    constexpr std::uint32_t exit_thread_nid = 0x0C8A38E1;
    const std::array<std::uint32_t, 8> guest_program{
        0xE92D4010u,
        0xEBFFFFF5u,
        0xE1A04000u,
        0xE58D4000u,
        0xE59D2000u,
        0xE8BD4010u,
        0xE300002Au,
        0xEBFFFFF3u
    };
    std::memcpy(elf.data() + 244, guest_program.data(), sizeof(guest_program));

    write_value(elf, 276, static_cast<std::uint16_t>(0x20));
    write_value(elf, 278, static_cast<std::uint16_t>(1));
    write_value(elf, 282, static_cast<std::uint16_t>(1));
    write_value(elf, 292, static_cast<std::uint32_t>(0xAABBCCDD));
    write_value(elf, 300, static_cast<std::uint32_t>(0x810000D4));
    write_value(elf, 304, static_cast<std::uint32_t>(0x810000D8));

    write_value(elf, 308, static_cast<std::uint16_t>(0x34));
    write_value(elf, 310, static_cast<std::uint16_t>(1));
    write_value(elf, 314, static_cast<std::uint16_t>(2));
    write_value(elf, 324, static_cast<std::uint32_t>(0x11223344));
    write_value(elf, 336, static_cast<std::uint32_t>(0x810000DC));
    write_value(elf, 340, static_cast<std::uint32_t>(0x810000E4));
    write_value(elf, 360, static_cast<std::uint32_t>(0x935CD196));
    write_value(elf, 364, static_cast<std::uint32_t>(0x81000040));
    write_value(elf, 368, get_thread_id_nid);
    write_value(elf, 372, exit_thread_nid);
    write_value(elf, 376, static_cast<std::uint32_t>(0x81000040));
    write_value(elf, 380, static_cast<std::uint32_t>(0x81000050));

    for (std::size_t index = 404; index < 420; ++index) {
        elf[index] = static_cast<std::uint8_t>(index - 388);
    }
    write_value(elf, 420, static_cast<std::uint32_t>(0x00000200));
    write_value(elf, 424, static_cast<std::uint32_t>(0x1234));
    write_value(elf, 428, static_cast<std::uint32_t>(0xF0));

    const auto fixture = test_root / "Vita3K" / "imports" / "synthetic-homebrew.elf";
    std::ofstream fixture_stream(fixture, std::ios::binary);
    fixture_stream.write(reinterpret_cast<const char *>(elf.data()), elf.size());
    fixture_stream.close();
    if (!emitted_fixture.empty()) {
        if (!emitted_fixture.parent_path().empty()) {
            std::filesystem::create_directories(emitted_fixture.parent_path(), error);
        }
        if (error || !std::filesystem::copy_file(fixture, emitted_fixture,
                std::filesystem::copy_options::overwrite_existing, error)) {
            std::cerr << "Could not emit the Milestone 8 fixture: " << error.message() << '\n';
            return 4;
        }
    }

    const auto rescanned = vita3k::ios::rescan_imports();
    if (rescanned.imported_artifacts.size() != 1 ||
        rescanned.imported_artifacts.front().kind != "Vita ELF" ||
        !rescanned.imported_artifacts.front().structurally_valid ||
        rescanned.imported_artifacts.front().load_segment_count != 2 ||
        rescanned.imported_artifacts.front().relocation_segment_count != 1 ||
        !rescanned.imported_artifacts.front().loaded ||
        !rescanned.imported_artifacts.front().module_info_valid ||
        !rescanned.imported_artifacts.front().relocations_applied ||
        !rescanned.imported_artifacts.front().module_tables_parsed ||
        !rescanned.imported_artifacts.front().import_stubs_bound ||
        !rescanned.imported_artifacts.front().module_start_valid ||
        !rescanned.imported_artifacts.front().execution_attempted ||
        !rescanned.imported_artifacts.front().thread_exited ||
        rescanned.imported_artifacts.front().relocation_entry_count != 1 ||
        rescanned.imported_artifacts.front().relocation_patch_count != 1 ||
        rescanned.imported_artifacts.front().export_library_count != 1 ||
        rescanned.imported_artifacts.front().import_library_count != 1 ||
        rescanned.imported_artifacts.front().exported_nid_count != 1 ||
        rescanned.imported_artifacts.front().imported_nid_count != 2 ||
        rescanned.imported_artifacts.front().bound_import_stub_count != 2 ||
        rescanned.imported_artifacts.front().module_start_address != 0x81000060 ||
        rescanned.imported_artifacts.front().executed_instruction_count != 11 ||
        rescanned.imported_artifacts.front().hle_dispatch_count != 2 ||
        rescanned.imported_artifacts.front().thread_exit_status != 42 ||
        rescanned.imported_artifacts.front().module_name != "synthetic-homebrew" ||
        rescanned.imported_artifacts.front().module_nid != 0x1234ABCD) {
        std::cerr << rescanned.summary << '\n';
        return 3;
    }

    std::filesystem::remove_all(test_root, error);
    if (!emitted_fixture.empty()) {
        std::cout << "Emitted legal Milestone 8 fixture: " << emitted_fixture << '\n';
    }
    std::cout << rescanned.summary << '\n';
    return 0;
}
