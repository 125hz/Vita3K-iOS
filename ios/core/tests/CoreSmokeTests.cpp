#include <vita3k_ios/CoreBridge.h>
#include <vita3k_ios/GuestHeap.h>
#include <vita3k_ios/GuestMemory.h>
#include <vita3k_ios/HostDisplay.h>
#include <vita3k_ios/HostInput.h>
#include <vita3k_ios/VitaAppArchive.h>
#include <vita3k_ios/VitaAppMetadata.h>

#include <packages/archive.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

template <typename T, std::size_t Size>
void write_value(std::array<std::uint8_t, Size> &image, std::size_t offset, T value) {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

template <typename T>
void write_value(std::vector<std::uint8_t> &image, std::size_t offset, T value) {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

std::vector<std::uint8_t> make_plain_self(std::span<const std::uint8_t> elf) {
    constexpr std::size_t self_header_size = 128;
    constexpr std::size_t embedded_elf_offset = self_header_size;
    constexpr std::size_t program_header_offset = embedded_elf_offset + 52;
    constexpr std::size_t section_info_offset = program_header_offset + 3 * 32;
    constexpr std::size_t payload_offset = 4096;
    std::vector<std::uint8_t> self(payload_offset + elf.size());
    write_value(self, 0, static_cast<std::uint32_t>(0x00454353));
    write_value(self, 4, static_cast<std::uint32_t>(3));
    write_value(self, 10, static_cast<std::uint16_t>(1));
    write_value(self, 16, static_cast<std::uint64_t>(payload_offset));
    write_value(self, 24, static_cast<std::uint64_t>(elf.size()));
    write_value(self, 32, static_cast<std::uint64_t>(self.size()));
    write_value(self, 64, static_cast<std::uint64_t>(embedded_elf_offset));
    write_value(self, 72, static_cast<std::uint64_t>(program_header_offset));
    write_value(self, 88, static_cast<std::uint64_t>(section_info_offset));
    std::memcpy(self.data() + embedded_elf_offset, elf.data(), 52);
    std::memcpy(self.data() + program_header_offset, elf.data() + 52, 3 * 32);
    for (std::size_t index = 0; index < 3; ++index) {
        std::uint32_t file_offset = 0;
        std::uint32_t file_size = 0;
        std::memcpy(&file_offset, elf.data() + 52 + index * 32 + 4, sizeof(file_offset));
        std::memcpy(&file_size, elf.data() + 52 + index * 32 + 16, sizeof(file_size));
        const auto info = section_info_offset + index * 32;
        write_value(self, info, static_cast<std::uint64_t>(payload_offset + file_offset));
        write_value(self, info + 8, static_cast<std::uint64_t>(file_size));
        write_value(self, info + 16, static_cast<std::uint64_t>(1));
        write_value(self, info + 24, static_cast<std::uint64_t>(2));
    }
    std::memcpy(self.data() + payload_offset, elf.data(), elf.size());
    return self;
}

} // namespace

int main(int argc, char **argv) {
    vita3k::ios::HostDisplay display;
    std::string display_error;
    if (display.attach(0, 1080, display_error)) {
        std::cerr << "The display accepted a zero-width drawable.\n";
        return 7;
    }
    if (!display.attach(1920, 1080, display_error)) {
        std::cerr << display_error << '\n';
        return 8;
    }
    const auto display_frame = display.acquire_frame(1920, 1080, display_error);
    if (!display_frame || display_frame->identifier == 0 || display_frame->width != 1920 || display_frame->height != 1080 || display.acquire_frame(1920, 1080, display_error)) {
        std::cerr << "The display did not enforce a single diagnostic frame in flight.\n";
        return 9;
    }
    if (!display.complete_frame(display_frame->identifier, true, {}, display_error)) {
        std::cerr << display_error << '\n';
        return 10;
    }
    const auto display_status = display.status();
    if (!display_status.attached || display_status.frame_in_flight || !display_status.first_frame_presented || display_status.submitted_frame_count != 1 || display_status.presented_frame_count != 1 || display.acquire_frame(1920, 1080, display_error)) {
        std::cerr << "The display did not retain its first-presented-frame state.\n";
        return 11;
    }

    vita3k::ios::HostInput input;
    std::string input_error;
    if (input.attach_touch_surface(0.0, 1080.0, input_error)) {
        std::cerr << "The input adapter accepted a zero-width touch surface.\n";
        return 15;
    }
    if (!input.attach_touch_surface(1000.0, 500.0, input_error) || !input.submit_touch(7, 250.0, 125.0, vita3k::ios::HostTouchPhase::began, input_error) || !input.submit_touch(7, 250.0, 125.0, vita3k::ios::HostTouchPhase::ended, input_error)) {
        std::cerr << input_error << '\n';
        return 16;
    }
    input.set_controller_connected(true);
    const vita3k::ios::HostControllerSample controller_sample{
        .left_x = 0.25f,
        .left_y = -0.5f,
        .right_x = 2.0f,
        .right_y = -2.0f,
        .buttons = vita3k::ios::host_button_a | vita3k::ios::host_button_dpad_up
    };
    if (!input.submit_controller(controller_sample, input_error)) {
        std::cerr << input_error << '\n';
        return 17;
    }
    const auto input_status = input.status();
    if (!input_status.touch_surface_attached || input_status.touch_active || !input_status.touch_sample_received || !input_status.controller_connected || !input_status.controller_sample_received || input_status.touch_sample_count != 2 || input_status.controller_sample_count != 1 || input_status.last_touch_x != 0.25 || input_status.last_touch_y != 0.25 || input_status.controller.right_x != 1.0f || input_status.controller.right_y != -1.0f || input_status.controller.buttons != controller_sample.buttons) {
        std::cerr << "The portable input adapter produced unexpected state.\n";
        return 18;
    }

    const auto sfo_fixture = vita3k::ios::make_synthetic_param_sfo(
        "M13TEST01", "Vita3K iOS upstream metadata probe");
    const auto parsed_sfo = vita3k::ios::parse_vita_app_metadata(sfo_fixture);
    if (!parsed_sfo.parsed || parsed_sfo.title_id != "M13TEST01" || parsed_sfo.title != "Vita3K iOS upstream metadata probe" || parsed_sfo.category != "gd" || parsed_sfo.app_version != "01.00") {
        std::cerr << parsed_sfo.detail << '\n';
        return 22;
    }
    const std::array<std::uint8_t, 20> malformed_sfo{
        0x00, 0x50, 0x53, 0x46, 0x01, 0x01, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0x7F
    };
    if (vita3k::ios::parse_vita_app_metadata(malformed_sfo).parsed) {
        std::cerr << "The upstream SFO parser accepted malformed table offsets.\n";
        return 23;
    }
    const auto vpk_fixture = vita3k::ios::make_synthetic_vpk();
    const auto parsed_vpk = packages::inspect_archive(vpk_fixture);
    const auto unsafe_vpk = packages::inspect_archive(vita3k::ios::make_synthetic_vpk(true));
    if (!parsed_vpk.valid || parsed_vpk.file_count != 3 || parsed_vpk.applications.size() != 1 || parsed_vpk.applications.front().title_id != "M14TEST01" || parsed_vpk.applications.front().install_target != "ux0/app/M14TEST01" || unsafe_vpk.valid || unsafe_vpk.unsafe_path_count != 1) {
        std::cerr << "The bounded Vita package inspection diagnostic failed.\n";
        return 27;
    }
    const auto install_zip_fixture = vita3k::ios::make_synthetic_install_zip();
    const auto parsed_install_zip = packages::inspect_archive(install_zip_fixture);
    if (!parsed_install_zip.valid || parsed_install_zip.file_count != 6 || parsed_install_zip.applications.size() != 2 || parsed_install_zip.applications[0].title_id != "M15TEST01" || parsed_install_zip.applications[0].install_target != "ux0/app/M15TEST01" || parsed_install_zip.applications[1].install_target != "ux0/patch/M15TEST01") {
        std::cerr << parsed_install_zip.detail << '\n';
        return 30;
    }

    std::filesystem::path emitted_fixture;
    std::filesystem::path emitted_sfo_fixture;
    std::filesystem::path emitted_vpk_fixture;
    std::filesystem::path emitted_install_zip_fixture;
    std::filesystem::path emitted_self_fixture;
    std::filesystem::path emitted_m16_install_zip_fixture;
    std::filesystem::path emitted_m18_install_zip_fixture;
    std::filesystem::path emitted_m19_install_zip_fixture;
    std::filesystem::path emitted_m20_install_zip_fixture;
    std::filesystem::path emitted_m21_install_zip_fixture;
    std::filesystem::path emitted_m22_install_zip_fixture;
    std::filesystem::path emitted_m23_install_zip_fixture;
    std::filesystem::path emitted_m24_install_zip_fixture;
    std::filesystem::path emitted_m25_install_zip_fixture;
    std::filesystem::path emitted_m26_install_zip_fixture;
    std::filesystem::path emitted_m27_install_zip_fixture;
    std::filesystem::path emitted_m28_install_zip_fixture;
    std::filesystem::path emitted_m29_install_zip_fixture;
    std::filesystem::path emitted_m30_install_zip_fixture;
    std::filesystem::path emitted_m31_install_zip_fixture;
    std::filesystem::path emitted_m32_install_zip_fixture;
    std::filesystem::path emitted_m33_install_zip_fixture;
    std::filesystem::path emitted_m34_install_zip_fixture;
    std::filesystem::path emitted_m35_install_zip_fixture;
    std::filesystem::path vitasdk_fixture;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--emit-fixture" && index + 1 < argc) {
            emitted_fixture = argv[++index];
        } else if (argument == "--emit-sfo-fixture" && index + 1 < argc) {
            emitted_sfo_fixture = argv[++index];
        } else if (argument == "--emit-vpk-fixture" && index + 1 < argc) {
            emitted_vpk_fixture = argv[++index];
        } else if (argument == "--emit-install-zip-fixture" && index + 1 < argc) {
            emitted_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-self-fixture" && index + 1 < argc) {
            emitted_self_fixture = argv[++index];
        } else if (argument == "--emit-m16-install-zip-fixture" && index + 1 < argc) {
            emitted_m16_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m18-install-zip-fixture" && index + 1 < argc) {
            emitted_m18_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m19-install-zip-fixture" && index + 1 < argc) {
            emitted_m19_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m20-install-zip-fixture" && index + 1 < argc) {
            emitted_m20_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m21-install-zip-fixture" && index + 1 < argc) {
            emitted_m21_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m22-install-zip-fixture" && index + 1 < argc) {
            emitted_m22_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m23-install-zip-fixture" && index + 1 < argc) {
            emitted_m23_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m24-install-zip-fixture" && index + 1 < argc) {
            emitted_m24_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m25-install-zip-fixture" && index + 1 < argc) {
            emitted_m25_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m26-install-zip-fixture" && index + 1 < argc) {
            emitted_m26_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m27-install-zip-fixture" && index + 1 < argc) {
            emitted_m27_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m28-install-zip-fixture" && index + 1 < argc) {
            emitted_m28_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m29-install-zip-fixture" && index + 1 < argc) {
            emitted_m29_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m30-install-zip-fixture" && index + 1 < argc) {
            emitted_m30_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m31-install-zip-fixture" && index + 1 < argc) {
            emitted_m31_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m32-install-zip-fixture" && index + 1 < argc) {
            emitted_m32_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m33-install-zip-fixture" && index + 1 < argc) {
            emitted_m33_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m34-install-zip-fixture" && index + 1 < argc) {
            emitted_m34_install_zip_fixture = argv[++index];
        } else if (argument == "--emit-m35-install-zip-fixture" && index + 1 < argc) {
            emitted_m35_install_zip_fixture = argv[++index];
        } else if (argument == "--verify-vitasdk" && index + 1 < argc) {
            vitasdk_fixture = argv[++index];
        } else {
            std::cerr << "Usage: vita3k_ios_core_smoke_tests "
                         "[--emit-fixture <path>] [--emit-sfo-fixture <path>] "
                         "[--emit-vpk-fixture <path>] "
                         "[--emit-install-zip-fixture <path>] "
                         "[--emit-self-fixture <path>] "
                         "[--emit-m16-install-zip-fixture <path>] "
                         "[--emit-m18-install-zip-fixture <path>] "
                         "[--emit-m19-install-zip-fixture <path>] "
                         "[--emit-m20-install-zip-fixture <path>] "
                         "[--emit-m21-install-zip-fixture <path>] "
                         "[--emit-m22-install-zip-fixture <path>] "
                         "[--emit-m23-install-zip-fixture <path>] "
                         "[--emit-m24-install-zip-fixture <path>] "
                         "[--emit-m25-install-zip-fixture <path>] "
                         "[--emit-m26-install-zip-fixture <path>] "
                         "[--emit-m27-install-zip-fixture <path>] "
                         "[--emit-m28-install-zip-fixture <path>] "
                         "[--emit-m29-install-zip-fixture <path>] "
                         "[--emit-m30-install-zip-fixture <path>] "
                         "[--emit-m31-install-zip-fixture <path>] "
                         "[--emit-m32-install-zip-fixture <path>] "
                         "[--emit-m33-install-zip-fixture <path>] "
                         "[--emit-m34-install-zip-fixture <path>] "
                         "[--emit-m35-install-zip-fixture <path>] "
                         "[--verify-vitasdk <path>]\n";
            return 64;
        }
    }
    const auto test_root = std::filesystem::temp_directory_path() / "vita3k-ios-core-smoke-test";
    std::error_code error;
    std::filesystem::remove_all(test_root, error);

    vita3k::ios::GuestMemory heap_memory;
    std::string heap_error;
    if (!heap_memory.reserve(1ULL << 32, heap_error)) {
        std::cerr << heap_error << '\n';
        return 106;
    }
    vita3k::ios::GuestHeap heap;
    if (!heap.initialize(heap_memory, 0x90000000u, 64 * 1024, heap_error)) {
        std::cerr << heap_error << '\n';
        return 107;
    }
    const auto aligned_allocation = heap.allocate(24, 256, false, heap_error);
    if (aligned_allocation == 0 || (aligned_allocation & 255u) != 0) {
        std::cerr << heap_error << '\n';
        return 108;
    }
    std::array<std::uint8_t, 24> heap_pattern{};
    for (std::size_t index = 0; index < heap_pattern.size(); ++index) {
        heap_pattern[index] = static_cast<std::uint8_t>(index + 1);
    }
    if (!heap_memory.write(aligned_allocation, heap_pattern, heap_error)) {
        std::cerr << heap_error << '\n';
        return 109;
    }
    const auto resized_allocation = heap.reallocate(aligned_allocation, 80, heap_error);
    std::array<std::uint8_t, 24> resized_prefix{};
    if (resized_allocation == 0
        || !heap_memory.read(resized_allocation, resized_prefix, heap_error)
        || resized_prefix != heap_pattern) {
        std::cerr << heap_error << '\n';
        return 110;
    }
    std::uint32_t usable_size = 0;
    if (!heap.usable_size(resized_allocation, usable_size, heap_error) || usable_size < 80
        || !heap.free(resized_allocation, heap_error)) {
        std::cerr << heap_error << '\n';
        return 111;
    }
    const auto dirty_allocation = heap.allocate(32, 16, false, heap_error);
    const std::array<std::uint8_t, 32> dirty_bytes{0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF};
    if (dirty_allocation == 0
        || !heap_memory.write(dirty_allocation, dirty_bytes, heap_error)
        || !heap.free(dirty_allocation, heap_error)) {
        std::cerr << heap_error << '\n';
        return 112;
    }
    const auto zeroed_allocation = heap.allocate(32, 16, true, heap_error);
    std::array<std::uint8_t, 32> zeroed_bytes{};
    if (zeroed_allocation == 0
        || !heap_memory.read(zeroed_allocation, zeroed_bytes, heap_error)
        || std::ranges::any_of(zeroed_bytes, [](std::uint8_t value) { return value != 0; })
        || !heap.free(zeroed_allocation, heap_error)) {
        std::cerr << heap_error << '\n';
        return 113;
    }
    std::string invalid_alignment_error;
    std::string invalid_free_error;
    if (heap.allocate(16, 3, false, invalid_alignment_error) != 0
        || invalid_alignment_error.find("power of two") == std::string::npos
        || heap.free(0x90001234u, invalid_free_error)
        || invalid_free_error.find("live heap allocation") == std::string::npos
        || heap.stats().live_bytes != 0 || heap.stats().peak_bytes < 80) {
        std::cerr << invalid_alignment_error << ' ' << invalid_free_error << '\n';
        return 114;
    }

    const auto status = vita3k::ios::initialize_core(test_root);
    if (!status.linked || !status.self_tests_passed || !status.upstream_metadata_ready || !status.upstream_archive_ready || !status.package_installer_ready || !status.storage_ready || !status.guest_memory_ready || !status.segment_mapping_ready || !status.loader_pipeline_ready || !status.import_binding_ready || !status.arm_execution_ready || !status.guest_thread_ready || status.renderer_attached || status.renderer_frame_presented || status.summary.find("Renderer: waiting for MTKView host") == std::string::npos || status.input_surface_attached || status.input_touch_received || status.summary.find("Input: waiting for UIKit touch surface") == std::string::npos || status.summary.find("Upstream app metadata: passed") == std::string::npos || status.arm_test_instruction_count != 7 || status.hle_test_dispatch_count != 1 || status.thread_test_instruction_count != 12 || status.thread_test_hle_dispatch_count != 2 || status.thread_test_exit_status != 42 || status.thread_test_bound_stub_count != 2 || status.guest_memory_size != (1ULL << 32)) {
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
    write_value(elf, 216, static_cast<std::uint32_t>(0x61));

    constexpr std::uint32_t get_thread_id_nid = 0x0FB972F9;
    constexpr std::uint32_t exit_thread_nid = 0x0C8A38E1;
    const std::array<std::uint16_t, 9> guest_program{
        0xB510u,
        0x4B04u,
        0x4798u,
        0x4604u,
        0x9400u,
        0x9A00u,
        0x202Au,
        0x4B02u,
        0x4798u
    };
    std::memcpy(elf.data() + 244, guest_program.data(), sizeof(guest_program));
    write_value(elf, 264, static_cast<std::uint32_t>(0x81000040));
    write_value(elf, 268, static_cast<std::uint32_t>(0x81000050));

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
    write_value(elf, 364, static_cast<std::uint32_t>(0x81000061));
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

    const auto plain_self = make_plain_self(elf);
    const auto m16_install_zip = vita3k::ios::make_synthetic_install_zip(
        plain_self, plain_self);
    auto lifecycle_elf = elf;
    write_value(lifecycle_elf, 216, static_cast<std::uint32_t>(0x70));
    const auto lifecycle_self = make_plain_self(lifecycle_elf);
    const auto m18_install_zip = vita3k::ios::make_synthetic_install_zip(
        lifecycle_self, lifecycle_self);
    auto wide_push_elf = lifecycle_elf;
    const std::array<std::uint16_t, 10> wide_push_program{
        0xE92Du,
        0x4100u,
        0x4B03u,
        0x4798u,
        0x4604u,
        0x9400u,
        0x9A00u,
        0x202Au,
        0x4B01u,
        0x4798u
    };
    std::memcpy(wide_push_elf.data() + 244, wide_push_program.data(),
        sizeof(wide_push_program));
    const auto wide_push_self = make_plain_self(wide_push_elf);
    const auto m19_install_zip = vita3k::ios::make_synthetic_install_zip(
        wide_push_self, wide_push_self);
    auto compiler_baseline_elf = wide_push_elf;
    const std::array<std::uint16_t, 12> compiler_baseline_program{
        0xE92Du,
        0x4100u,
        0xB082u,
        0x4B04u,
        0x4798u,
        0x4604u,
        0x9400u,
        0x9A00u,
        0x202Au,
        0x4B02u,
        0x4798u,
        0xBF00u
    };
    std::memcpy(compiler_baseline_elf.data() + 244, compiler_baseline_program.data(),
        sizeof(compiler_baseline_program));
    write_value(compiler_baseline_elf, 268, static_cast<std::uint32_t>(0x81000040));
    write_value(compiler_baseline_elf, 272, static_cast<std::uint32_t>(0x81000050));
    const auto compiler_baseline_self = make_plain_self(compiler_baseline_elf);
    const auto m20_install_zip = vita3k::ios::make_synthetic_install_zip(
        compiler_baseline_self, compiler_baseline_self);
    auto thumb2_compiler_elf = lifecycle_elf;
    const std::array<std::uint16_t, 12> thumb2_compiler_program{
        0xE92Du, // PUSH.W {r8, lr}
        0x4100u,
        0xB082u, // SUB sp, #8
        0xF247u, // MOVW r2, #0x7690
        0x6290u,
        0xE9CDu, // STRD r1, r0, [sp]
        0x1000u,
        0xF2C8u, // MOVT r2, #0x812c
        0x122Cu,
        0x202Au, // MOVS r0, #42
        0x4B00u, // LDR r3, [pc] -> exit-thread import stub
        0x4798u // BLX r3
    };
    std::memcpy(thumb2_compiler_elf.data() + 244, thumb2_compiler_program.data(),
        sizeof(thumb2_compiler_program));
    write_value(thumb2_compiler_elf, 268, static_cast<std::uint32_t>(0x81000050));
    const auto thumb2_compiler_self = make_plain_self(thumb2_compiler_elf);
    const auto m21_install_zip = vita3k::ios::make_synthetic_install_zip(
        thumb2_compiler_self, thumb2_compiler_self);
    auto inline_hle_elf = thumb2_compiler_elf;
    constexpr std::uint32_t diagnostic_unbound_nid = 0x210C0046u;
    write_value(inline_hle_elf, 372, diagnostic_unbound_nid);
    const auto inline_hle_self = make_plain_self(inline_hle_elf);
    const auto m22_install_zip = vita3k::ios::make_synthetic_install_zip(
        inline_hle_self, inline_hle_self);
    auto libc_dso_runtime_elf = lifecycle_elf;
    constexpr std::uint32_t cxa_set_dso_handle_main_nid = 0xBFE02B3Au;
    constexpr std::uint32_t synthetic_dso_handle = 0x81000200u;
    const std::array<std::uint16_t, 5> libc_dso_runtime_program{
        0xB510u, // PUSH {r4, lr}
        0x4804u, // LDR r0, [pc, #16] -> synthetic DSO handle
        0x4B04u, // LDR r3, [pc, #16] -> libc import stub
        0x4798u, // BLX r3
        0xBD10u // POP {r4, pc} -> zero-link return sentinel
    };
    std::memcpy(libc_dso_runtime_elf.data() + 244, libc_dso_runtime_program.data(),
        sizeof(libc_dso_runtime_program));
    write_value(libc_dso_runtime_elf, 264, synthetic_dso_handle);
    write_value(libc_dso_runtime_elf, 268, static_cast<std::uint32_t>(0x81000050));
    write_value(libc_dso_runtime_elf, 372, cxa_set_dso_handle_main_nid);
    const auto libc_dso_runtime_self = make_plain_self(libc_dso_runtime_elf);
    const auto m23_install_zip = vita3k::ios::make_synthetic_install_zip(
        libc_dso_runtime_self, libc_dso_runtime_self);
    auto runtime_family_elf = libc_dso_runtime_elf;
    const std::array<std::uint16_t, 9> runtime_family_program{
        0xB510u, // PUSH {r4, lr}
        0x4804u, // LDR r0, [pc, #16] -> synthetic DSO handle
        0x4B04u, // LDR r3, [pc, #16] -> libc import stub
        0x4798u, // BLX r3
        0xF05Fu, // MOVS.W r11, #0 (captured Amagami instruction)
        0x0B00u,
        0xF8DDu, // LDR.W r10, [sp] (captured Amagami instruction)
        0xA000u,
        0xBD10u // POP {r4, pc} -> zero-link return sentinel
    };
    std::memcpy(runtime_family_elf.data() + 244, runtime_family_program.data(),
        sizeof(runtime_family_program));
    const auto runtime_family_self = make_plain_self(runtime_family_elf);
    const auto m24_install_zip = vita3k::ios::make_synthetic_install_zip(
        runtime_family_self, runtime_family_self);
    auto register_family_elf = libc_dso_runtime_elf;
    const std::array<std::uint16_t, 11> register_family_program{
        0xB510u, // PUSH {r4, lr}
        0x4805u, // LDR r0, [pc, #20] -> relocated synthetic DSO handle
        0x4B05u, // LDR r3, [pc, #20] -> relocated libc import stub
        0x4798u, // BLX r3
        0xF05Fu, // MOVS.W r11, #0
        0x0B00u,
        0xF8DDu, // LDR.W r10, [sp]
        0xA000u,
        0xEBAAu, // SUB.W r2, r10, r2 (captured Amagami instruction)
        0x0202u,
        0xBD10u // POP {r4, pc} -> zero-link return sentinel
    };
    std::memcpy(register_family_elf.data() + 244, register_family_program.data(),
        sizeof(register_family_program));
    write_value(register_family_elf, 268, synthetic_dso_handle);
    write_value(register_family_elf, 272, static_cast<std::uint32_t>(0x81000050));
    const auto register_family_self = make_plain_self(register_family_elf);
    const auto m25_install_zip = vita3k::ios::make_synthetic_install_zip(
        register_family_self, register_family_self);
    auto libc_termination_elf = lifecycle_elf;
    constexpr std::uint32_t aeabi_atexit_nid = 0xEDC939E1u;
    constexpr std::uint32_t cxa_atexit_nid = 0x33B83B70u;
    constexpr std::uint32_t cxa_finalize_nid = 0xB538BF48u;
    constexpr std::uint32_t synthetic_atexit_object = 0x81000240u;
    constexpr std::uint32_t synthetic_atexit_destructor = 0x81000301u;
    const std::array<std::uint16_t, 7> libc_termination_program{
        0xB510u, // PUSH {r4, lr}
        0x4803u, // LDR r0, [pc, #12] -> object / finalize DSO
        0x4903u, // LDR r1, [pc, #12] -> destructor
        0x4A04u, // LDR r2, [pc, #16] -> DSO handle
        0x4B04u, // LDR r3, [pc, #16] -> libc import stub
        0x4798u, // BLX r3
        0xBD10u // POP {r4, pc} -> zero-link return sentinel
    };
    std::memcpy(libc_termination_elf.data() + 244, libc_termination_program.data(),
        sizeof(libc_termination_program));
    write_value(libc_termination_elf, 260, synthetic_atexit_object);
    write_value(libc_termination_elf, 264, synthetic_atexit_destructor);
    write_value(libc_termination_elf, 268, synthetic_dso_handle);
    write_value(libc_termination_elf, 272, static_cast<std::uint32_t>(0x81000050));
    write_value(libc_termination_elf, 372, aeabi_atexit_nid);
    const auto libc_termination_self = make_plain_self(libc_termination_elf);
    const auto m26_install_zip = vita3k::ios::make_synthetic_install_zip(
        libc_termination_self, libc_termination_self);
    auto cxa_atexit_elf = libc_termination_elf;
    write_value(cxa_atexit_elf, 260, synthetic_atexit_destructor);
    write_value(cxa_atexit_elf, 264, synthetic_atexit_object);
    write_value(cxa_atexit_elf, 372, cxa_atexit_nid);
    const auto cxa_atexit_self = make_plain_self(cxa_atexit_elf);
    const auto cxa_atexit_install_zip = vita3k::ios::make_synthetic_install_zip(
        cxa_atexit_self, cxa_atexit_self);
    auto cxa_finalize_elf = libc_termination_elf;
    write_value(cxa_finalize_elf, 260, synthetic_dso_handle);
    write_value(cxa_finalize_elf, 372, cxa_finalize_nid);
    const auto cxa_finalize_self = make_plain_self(cxa_finalize_elf);
    const auto cxa_finalize_install_zip = vita3k::ios::make_synthetic_install_zip(
        cxa_finalize_self, cxa_finalize_self);
    auto multiple_transfer_elf = lifecycle_elf;
    const std::array<std::uint16_t, 4> multiple_transfer_program{
        0xE92Du, // PUSH.W {r4-r8, lr}
        0x41F0u,
        0xE8BDu, // POP.W {r4-r8, pc} (captured Amagami instruction)
        0x81F0u
    };
    std::memcpy(multiple_transfer_elf.data() + 244,
        multiple_transfer_program.data(), sizeof(multiple_transfer_program));
    const auto multiple_transfer_self = make_plain_self(multiple_transfer_elf);
    const auto m27_install_zip = vita3k::ios::make_synthetic_install_zip(
        multiple_transfer_self, multiple_transfer_self);
    constexpr std::uint32_t cxa_guard_abort_nid = 0xD18E461Du;
    constexpr std::uint32_t cxa_guard_acquire_nid = 0xD0310E31u;
    constexpr std::uint32_t cxa_guard_release_nid = 0x4ED1056Fu;
    constexpr std::uint32_t synthetic_guard_address = 0x81000240u;
    const std::array<std::uint16_t, 8> guard_call_program{
        0xB510u, // PUSH {r4, lr}
        0x4803u, // LDR r0, [pc, #12] -> guard word
        0x4B03u, // LDR r3, [pc, #12] -> libc import stub
        0x4798u, // BLX r3
        0xBD10u, // POP {r4, pc} -> zero-link return sentinel
        0xBF00u, 0xBF00u, 0xBF00u
    };
    auto cxa_guard_acquire_elf = lifecycle_elf;
    std::memcpy(cxa_guard_acquire_elf.data() + 244, guard_call_program.data(),
        sizeof(guard_call_program));
    write_value(cxa_guard_acquire_elf, 260, synthetic_guard_address);
    write_value(cxa_guard_acquire_elf, 264, static_cast<std::uint32_t>(0x81000050));
    write_value(cxa_guard_acquire_elf, 372, cxa_guard_acquire_nid);
    const auto cxa_guard_acquire_self = make_plain_self(cxa_guard_acquire_elf);
    const auto m28_install_zip = vita3k::ios::make_synthetic_install_zip(
        cxa_guard_acquire_self, cxa_guard_acquire_self);
    auto cxa_guard_release_elf = cxa_guard_acquire_elf;
    write_value(cxa_guard_release_elf, 372, cxa_guard_release_nid);
    const auto cxa_guard_release_self = make_plain_self(cxa_guard_release_elf);
    const auto cxa_guard_release_install_zip = vita3k::ios::make_synthetic_install_zip(
        cxa_guard_release_self, cxa_guard_release_self);
    auto cxa_guard_abort_elf = cxa_guard_acquire_elf;
    write_value(cxa_guard_abort_elf, 372, cxa_guard_abort_nid);
    const auto cxa_guard_abort_self = make_plain_self(cxa_guard_abort_elf);
    const auto cxa_guard_abort_install_zip = vita3k::ios::make_synthetic_install_zip(
        cxa_guard_abort_self, cxa_guard_abort_self);
    auto initialized_guard_elf = cxa_guard_acquire_elf;
    const std::array<std::uint16_t, 8> initialized_guard_program{
        0xB510u, // PUSH {r4, lr}
        0x4C03u, // LDR r4, [pc, #12] -> guard word
        0x2101u, // MOVS r1, #1
        0x6021u, // STR r1, [r4]
        0x4620u, // MOV r0, r4
        0x4B02u, // LDR r3, [pc, #8] -> libc import stub
        0x4798u, // BLX r3
        0xBD10u // POP {r4, pc}
    };
    std::memcpy(initialized_guard_elf.data() + 244, initialized_guard_program.data(),
        sizeof(initialized_guard_program));
    const auto initialized_guard_self = make_plain_self(initialized_guard_elf);
    const auto initialized_guard_install_zip = vita3k::ios::make_synthetic_install_zip(
        initialized_guard_self, initialized_guard_self);
    auto recursive_guard_elf = cxa_guard_acquire_elf;
    const std::array<std::uint16_t, 8> recursive_guard_program{
        0xB510u, // PUSH {r4, lr}
        0x4C04u, // LDR r4, [pc, #16] -> guard word
        0x4620u, // MOV r0, r4
        0x4B04u, // LDR r3, [pc, #16] -> libc import stub
        0x4798u, // BLX r3
        0x4620u, // MOV r0, r4
        0x4798u, // BLX r3 -> honest recursive-initialization boundary
        0xBD10u
    };
    std::memcpy(recursive_guard_elf.data() + 244, recursive_guard_program.data(),
        sizeof(recursive_guard_program));
    write_value(recursive_guard_elf, 264, synthetic_guard_address);
    write_value(recursive_guard_elf, 268, static_cast<std::uint32_t>(0x81000050));
    const auto recursive_guard_self = make_plain_self(recursive_guard_elf);
    const auto recursive_guard_install_zip = vita3k::ios::make_synthetic_install_zip(
        recursive_guard_self, recursive_guard_self);
    constexpr std::uint32_t memalign_nid = 0xA9363E6Bu;
    constexpr std::uint32_t synthetic_heap_base = 0x90000000u;
    auto libc_heap_elf = lifecycle_elf;
    const std::array<std::uint16_t, 8> libc_heap_program{
        0xB510u, // PUSH {r4, lr}
        0x2010u, // MOVS r0, #16 -> alignment
        0x4903u, // LDR r1, [pc, #12] -> size
        0x4B04u, // LDR r3, [pc, #16] -> libc import stub
        0x4798u, // BLX r3
        0xBD10u, // POP {r4, pc}
        0xBF00u, 0xBF00u
    };
    std::memcpy(libc_heap_elf.data() + 244, libc_heap_program.data(),
        sizeof(libc_heap_program));
    write_value(libc_heap_elf, 264, static_cast<std::uint32_t>(0x180));
    write_value(libc_heap_elf, 268, static_cast<std::uint32_t>(0x81000050));
    write_value(libc_heap_elf, 372, memalign_nid);
    const auto libc_heap_self = make_plain_self(libc_heap_elf);
    const auto m29_install_zip = vita3k::ios::make_synthetic_install_zip(
        libc_heap_self, libc_heap_self);
    auto lr_shift_elf = lifecycle_elf;
    const std::array<std::uint16_t, 7> lr_shift_program{
        0xB510u, // PUSH {r4, lr}
        0x2004u, // MOVS r0, #4
        0x2105u, // MOVS r1, #5
        0x468Eu, // MOV lr, r1
        0xEB00u, // ADD.W r0, r0, lr, LSL #3 (captured Amagami instruction)
        0x00CEu,
        0xBD10u // POP {r4, pc} -> zero-link return sentinel
    };
    std::memcpy(lr_shift_elf.data() + 244, lr_shift_program.data(),
        sizeof(lr_shift_program));
    const auto lr_shift_self = make_plain_self(lr_shift_elf);
    const auto m30_install_zip = vita3k::ios::make_synthetic_install_zip(
        lr_shift_self, lr_shift_self);
    auto execution_progress_elf = lifecycle_elf;
    const std::array<std::uint16_t, 1> execution_progress_program{
        0xE7FEu // B . -> deterministic hot-loop diagnostic
    };
    std::memcpy(execution_progress_elf.data() + 244,
        execution_progress_program.data(), sizeof(execution_progress_program));
    const auto execution_progress_self = make_plain_self(execution_progress_elf);
    const auto m31_install_zip = vita3k::ios::make_synthetic_install_zip(
        execution_progress_self, execution_progress_self);
    constexpr std::uint32_t delete_array_nid = 0x91B0DC47u;
    constexpr std::uint32_t delete_array_nothrow_nid = 0xA7241F09u;
    constexpr std::uint32_t delete_array_placement_nid = 0x3688FFDAu;
    constexpr std::uint32_t delete_nid = 0x72293931u;
    constexpr std::uint32_t delete_nothrow_nid = 0x87EF85FFu;
    constexpr std::uint32_t delete_placement_nid = 0x1EB89099u;
    constexpr std::uint32_t new_array_nid = 0xE7FB2BF4u;
    constexpr std::uint32_t new_array_nothrow_nid = 0x31C62481u;
    constexpr std::uint32_t new_nid = 0xF99ED5ACu;
    constexpr std::uint32_t new_nothrow_nid = 0x0AE71DC3u;
    const auto make_cpp_allocation_archive = [&](std::uint32_t nid,
                                                  std::uint32_t argument) {
        auto elf = lifecycle_elf;
        const std::array<std::uint16_t, 8> program{
            0xB510u, // PUSH {r4, lr}
            0x4803u, // LDR r0, [pc, #12] -> allocation size/delete pointer
            0x4B04u, // LDR r3, [pc, #16] -> C++ runtime import stub
            0x4798u, // BLX r3
            0xBD10u, // POP {r4, pc}
            0xBF00u, 0xBF00u, 0xBF00u
        };
        std::memcpy(elf.data() + 244, program.data(), sizeof(program));
        write_value(elf, 260, argument);
        write_value(elf, 268, static_cast<std::uint32_t>(0x81000050));
        write_value(elf, 372, nid);
        const auto self = make_plain_self(elf);
        return vita3k::ios::make_synthetic_install_zip(self, self);
    };
    const auto m32_install_zip = make_cpp_allocation_archive(new_nid, 8);
    const auto new_array_install_zip = make_cpp_allocation_archive(new_array_nid, 24);
    const auto new_nothrow_install_zip = make_cpp_allocation_archive(new_nothrow_nid, 32);
    const auto new_array_nothrow_install_zip = make_cpp_allocation_archive(
        new_array_nothrow_nid, 40);
    const auto new_throw_failure_install_zip = make_cpp_allocation_archive(
        new_nid, 32 * 1024 * 1024);
    const auto new_nothrow_failure_install_zip = make_cpp_allocation_archive(
        new_nothrow_nid, 32 * 1024 * 1024);
    const auto delete_install_zip = make_cpp_allocation_archive(delete_nid, 0);
    const auto delete_nothrow_install_zip = make_cpp_allocation_archive(
        delete_nothrow_nid, 0);
    const auto delete_placement_install_zip = make_cpp_allocation_archive(
        delete_placement_nid, 8);
    const auto delete_array_install_zip = make_cpp_allocation_archive(delete_array_nid, 0);
    const auto delete_array_nothrow_install_zip = make_cpp_allocation_archive(
        delete_array_nothrow_nid, 0);
    const auto delete_array_placement_install_zip = make_cpp_allocation_archive(
        delete_array_placement_nid, 8);
    constexpr std::uint32_t app_util_init_nid = 0xDAFFE671u;
    constexpr std::uint32_t app_util_shutdown_nid = 0xB220B00Bu;
    constexpr std::uint32_t app_util_error_parameter = 0x80100600u;
    constexpr std::uint32_t app_util_error_not_initialized = 0x80100601u;
    constexpr std::uint32_t synthetic_app_util_init_param = 0x81000240u;
    constexpr std::uint32_t synthetic_app_util_boot_param = 0x81000280u;
    const auto make_app_util_init_archive = [&](std::uint32_t init_param,
                                                std::uint32_t boot_param) {
        auto elf = lifecycle_elf;
        const std::array<std::uint16_t, 8> program{
            0xB510u, // PUSH {r4, lr}
            0x4803u, // LDR r0, [pc, #12] -> SceAppUtilInitParam
            0x4903u, // LDR r1, [pc, #12] -> SceAppUtilBootParam
            0x4B04u, // LDR r3, [pc, #16] -> sceAppUtilInit import stub
            0x4798u, // BLX r3
            0xBD10u, // POP {r4, pc}
            0xBF00u, 0xBF00u
        };
        std::memcpy(elf.data() + 244, program.data(), sizeof(program));
        write_value(elf, 260, init_param);
        write_value(elf, 264, boot_param);
        write_value(elf, 268, static_cast<std::uint32_t>(0x81000050));
        write_value(elf, 372, app_util_init_nid);
        const auto self = make_plain_self(elf);
        return vita3k::ios::make_synthetic_install_zip(self, self);
    };
    const auto m33_install_zip = make_app_util_init_archive(
        synthetic_app_util_init_param, synthetic_app_util_boot_param);
    const auto null_app_util_init_install_zip = make_app_util_init_archive(0, 0);
    const auto invalid_app_util_init_install_zip = make_app_util_init_archive(
        0x70000000u, synthetic_app_util_boot_param);
    const auto uninitialized_app_util_shutdown_install_zip = make_cpp_allocation_archive(
        app_util_shutdown_nid, 0);
    auto app_util_lifecycle_elf = lifecycle_elf;
    const std::array<std::uint16_t, 8> app_util_lifecycle_program{
        0xB510u, // PUSH {r4, lr}
        0x4803u, // LDR r0, [pc, #12] -> init param
        0x4903u, // LDR r1, [pc, #12] -> boot param
        0x4B04u, // LDR r3, [pc, #16] -> init stub
        0x4798u, // BLX r3
        0x4B04u, // LDR r3, [pc, #16] -> shutdown stub
        0x4798u, // BLX r3
        0xBD10u // POP {r4, pc}
    };
    std::memcpy(app_util_lifecycle_elf.data() + 244,
        app_util_lifecycle_program.data(), sizeof(app_util_lifecycle_program));
    write_value(app_util_lifecycle_elf, 260, synthetic_app_util_init_param);
    write_value(app_util_lifecycle_elf, 264, synthetic_app_util_boot_param);
    write_value(app_util_lifecycle_elf, 268, static_cast<std::uint32_t>(0x81000050));
    write_value(app_util_lifecycle_elf, 272, static_cast<std::uint32_t>(0x81000030));
    write_value(app_util_lifecycle_elf, 314, static_cast<std::uint16_t>(3));
    write_value(app_util_lifecycle_elf, 340, static_cast<std::uint32_t>(0x810000C4));
    write_value(app_util_lifecycle_elf, 368, get_thread_id_nid);
    write_value(app_util_lifecycle_elf, 372, app_util_init_nid);
    write_value(app_util_lifecycle_elf, 376, app_util_shutdown_nid);
    // Keep this three-entry table away from guest 0x810000F0, which is the
    // synthetic ELF's deliberate relocation target.
    write_value(app_util_lifecycle_elf, 344, static_cast<std::uint32_t>(0x81000040));
    write_value(app_util_lifecycle_elf, 348, static_cast<std::uint32_t>(0x81000050));
    write_value(app_util_lifecycle_elf, 352, static_cast<std::uint32_t>(0x81000030));
    const auto app_util_lifecycle_self = make_plain_self(app_util_lifecycle_elf);
    const auto app_util_lifecycle_install_zip = vita3k::ios::make_synthetic_install_zip(
        app_util_lifecycle_self, app_util_lifecycle_self);
    constexpr std::uint32_t sysmodule_is_loaded_nid = 0x53099B7Au;
    constexpr std::uint32_t sysmodule_load_nid = 0x79A0160Au;
    constexpr std::uint32_t sysmodule_unload_nid = 0x31D87805u;
    constexpr std::uint32_t sysmodule_np_id = 0x15u;
    constexpr std::uint32_t sysmodule_error_invalid_value = 0x805A1000u;
    constexpr std::uint32_t sysmodule_error_unloaded = 0x805A1001u;
    const auto unloaded_sysmodule_install_zip = make_cpp_allocation_archive(
        sysmodule_is_loaded_nid, sysmodule_np_id);
    const auto invalid_sysmodule_install_zip = make_cpp_allocation_archive(
        sysmodule_load_nid, 0x57u);
    auto sysmodule_lifecycle_elf = lifecycle_elf;
    const std::array<std::uint16_t, 14> sysmodule_lifecycle_program{
        0xB510u, // PUSH {r4, lr}
        0x2015u, // MOVS r0, #0x15 (SCE_SYSMODULE_NP)
        0x4B05u, // LDR r3, [pc, #20] -> load stub
        0x4798u, // BLX r3
        0x2015u, // MOVS r0, #0x15
        0x3310u, // ADDS r3, #16 -> status stub
        0x4798u, // BLX r3
        0x2015u, // MOVS r0, #0x15
        0x3310u, // ADDS r3, #16 -> unload stub
        0x4798u, // BLX r3
        0x2015u, // MOVS r0, #0x15
        0x3B10u, // SUBS r3, #16 -> status stub
        0x4798u, // BLX r3
        0xBD10u // POP {r4, pc}
    };
    std::memcpy(sysmodule_lifecycle_elf.data() + 244,
        sysmodule_lifecycle_program.data(), sizeof(sysmodule_lifecycle_program));
    write_value(sysmodule_lifecycle_elf, 272, static_cast<std::uint32_t>(0x81000030));
    write_value(sysmodule_lifecycle_elf, 314, static_cast<std::uint16_t>(4));
    write_value(sysmodule_lifecycle_elf, 340, static_cast<std::uint32_t>(0x810000C4));
    write_value(sysmodule_lifecycle_elf, 344, static_cast<std::uint32_t>(0x81000020));
    write_value(sysmodule_lifecycle_elf, 348, static_cast<std::uint32_t>(0x81000030));
    write_value(sysmodule_lifecycle_elf, 352, static_cast<std::uint32_t>(0x81000040));
    write_value(sysmodule_lifecycle_elf, 356, static_cast<std::uint32_t>(0x81000050));
    write_value(sysmodule_lifecycle_elf, 368, get_thread_id_nid);
    write_value(sysmodule_lifecycle_elf, 372, sysmodule_load_nid);
    write_value(sysmodule_lifecycle_elf, 376, sysmodule_is_loaded_nid);
    write_value(sysmodule_lifecycle_elf, 380, sysmodule_unload_nid);
    const auto sysmodule_lifecycle_self = make_plain_self(sysmodule_lifecycle_elf);
    const auto m34_install_zip = vita3k::ios::make_synthetic_install_zip(
        sysmodule_lifecycle_self, sysmodule_lifecycle_self);
    constexpr std::uint32_t np_get_service_state_nid = 0x54060DF6u;
    constexpr std::uint32_t np_init_nid = 0x04D9F484u;
    constexpr std::uint32_t np_term_nid = 0x19E40AE1u;
    constexpr std::uint32_t np_trophy_init_nid = 0x34516838u;
    constexpr std::uint32_t np_trophy_term_nid = 0xBFE0F28Fu;
    constexpr std::uint32_t np_error_already_initialized = 0x80550001u;
    constexpr std::uint32_t np_error_invalid_argument = 0x80550003u;
    constexpr std::uint32_t np_trophy_error_not_initialized = 0x80551601u;
    constexpr std::uint32_t np_trophy_error_already_initialized = 0x80551602u;
    constexpr std::uint32_t synthetic_np_service_state = 0x81000240u;
    const auto m35_install_zip = make_cpp_allocation_archive(np_init_nid, 0);
    const auto null_np_service_state_install_zip = make_cpp_allocation_archive(
        np_get_service_state_nid, 0);
    const auto invalid_np_service_state_install_zip = make_cpp_allocation_archive(
        np_get_service_state_nid, 0x70000000u);
    const auto uninitialized_trophy_term_install_zip = make_cpp_allocation_archive(
        np_trophy_term_nid, 0);
    const auto make_duplicate_lifecycle_call_archive = [&](std::uint32_t nid) {
        auto elf = lifecycle_elf;
        const std::array<std::uint16_t, 8> program{
            0xB510u, // PUSH {r4, lr}
            0x2000u, // MOVS r0, #0
            0x4B02u, // LDR r3, [pc, #8] -> import stub
            0x4798u, // BLX r3
            0x2000u, // MOVS r0, #0
            0x4798u, // BLX r3 again
            0xBD10u, // POP {r4, pc}
            0xBF00u
        };
        std::memcpy(elf.data() + 244, program.data(), sizeof(program));
        write_value(elf, 260, static_cast<std::uint32_t>(0x81000050));
        write_value(elf, 372, nid);
        const auto self = make_plain_self(elf);
        return vita3k::ios::make_synthetic_install_zip(self, self);
    };
    const auto duplicate_np_init_install_zip = make_duplicate_lifecycle_call_archive(
        np_init_nid);
    const auto duplicate_trophy_init_install_zip = make_duplicate_lifecycle_call_archive(
        np_trophy_init_nid);
    auto np_lifecycle_elf = lifecycle_elf;
    const std::array<std::uint16_t, 10> np_lifecycle_program{
        0xB510u, // PUSH {r4, lr}
        0x2000u, // MOVS r0, #0 (null communication config)
        0x4B03u, // LDR r3, [pc, #12] -> sceNpInit stub
        0x4798u, // BLX r3
        0x4803u, // LDR r0, [pc, #12] -> service-state output
        0x3310u, // ADDS r3, #16 -> sceNpGetServiceState stub
        0x4798u, // BLX r3
        0x3310u, // ADDS r3, #16 -> sceNpTerm stub
        0x4798u, // BLX r3
        0xBD10u // POP {r4, pc}
    };
    std::memcpy(np_lifecycle_elf.data() + 244,
        np_lifecycle_program.data(), sizeof(np_lifecycle_program));
    write_value(np_lifecycle_elf, 264, static_cast<std::uint32_t>(0x81000030));
    write_value(np_lifecycle_elf, 268, synthetic_np_service_state);
    write_value(np_lifecycle_elf, 314, static_cast<std::uint16_t>(4));
    write_value(np_lifecycle_elf, 340, static_cast<std::uint32_t>(0x810000C4));
    write_value(np_lifecycle_elf, 344, static_cast<std::uint32_t>(0x81000020));
    write_value(np_lifecycle_elf, 348, static_cast<std::uint32_t>(0x81000030));
    write_value(np_lifecycle_elf, 352, static_cast<std::uint32_t>(0x81000040));
    write_value(np_lifecycle_elf, 356, static_cast<std::uint32_t>(0x81000050));
    write_value(np_lifecycle_elf, 368, get_thread_id_nid);
    write_value(np_lifecycle_elf, 372, np_init_nid);
    write_value(np_lifecycle_elf, 376, np_get_service_state_nid);
    write_value(np_lifecycle_elf, 380, np_term_nid);
    const auto np_lifecycle_self = make_plain_self(np_lifecycle_elf);
    const auto np_lifecycle_install_zip = vita3k::ios::make_synthetic_install_zip(
        np_lifecycle_self, np_lifecycle_self);
    auto trophy_lifecycle_elf = lifecycle_elf;
    const std::array<std::uint16_t, 8> trophy_lifecycle_program{
        0xB510u, // PUSH {r4, lr}
        0x2000u, // MOVS r0, #0
        0x4B02u, // LDR r3, [pc, #8] -> sceNpTrophyInit stub
        0x4798u, // BLX r3
        0x3310u, // ADDS r3, #16 -> sceNpTrophyTerm stub
        0x4798u, // BLX r3
        0xBD10u, // POP {r4, pc}
        0xBF00u
    };
    std::memcpy(trophy_lifecycle_elf.data() + 244,
        trophy_lifecycle_program.data(), sizeof(trophy_lifecycle_program));
    write_value(trophy_lifecycle_elf, 260, static_cast<std::uint32_t>(0x81000040));
    write_value(trophy_lifecycle_elf, 314, static_cast<std::uint16_t>(3));
    write_value(trophy_lifecycle_elf, 340, static_cast<std::uint32_t>(0x810000C4));
    write_value(trophy_lifecycle_elf, 344, static_cast<std::uint32_t>(0x81000020));
    write_value(trophy_lifecycle_elf, 348, static_cast<std::uint32_t>(0x81000040));
    write_value(trophy_lifecycle_elf, 352, static_cast<std::uint32_t>(0x81000050));
    write_value(trophy_lifecycle_elf, 368, get_thread_id_nid);
    write_value(trophy_lifecycle_elf, 372, np_trophy_init_nid);
    write_value(trophy_lifecycle_elf, 376, np_trophy_term_nid);
    const auto trophy_lifecycle_self = make_plain_self(trophy_lifecycle_elf);
    const auto trophy_lifecycle_install_zip = vita3k::ios::make_synthetic_install_zip(
        trophy_lifecycle_self, trophy_lifecycle_self);
    if (m16_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 16 SELF installation fixture.\n";
        return 34;
    }
    if (m18_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 18 lifecycle-export fixture.\n";
        return 42;
    }
    if (m19_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 19 Thumb-2 PUSH.W fixture.\n";
        return 47;
    }
    if (m20_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 20 Thumb compiler baseline fixture.\n";
        return 52;
    }
    if (m21_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 21 Thumb-2 compiler batch fixture.\n";
        return 57;
    }
    if (m22_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 22 inline HLE diagnostic fixture.\n";
        return 62;
    }
    if (m23_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 23 libc runtime fixture.\n";
        return 68;
    }
    if (m24_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 24 Thumb-2 runtime-family fixture.\n";
        return 73;
    }
    if (m25_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 25 Thumb-2 register-family fixture.\n";
        return 78;
    }
    if (m26_install_zip.empty() || cxa_atexit_install_zip.empty()
        || cxa_finalize_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 26 libc termination fixtures.\n";
        return 83;
    }
    if (m27_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 27 Thumb-2 multiple-transfer fixture.\n";
        return 94;
    }
    if (m28_install_zip.empty() || cxa_guard_release_install_zip.empty()
        || cxa_guard_abort_install_zip.empty() || initialized_guard_install_zip.empty()
        || recursive_guard_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 28 C++ guard fixtures.\n";
        return 99;
    }
    if (m29_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 29 libc heap fixture.\n";
        return 115;
    }
    if (m30_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 30 LR shifted-register fixture.\n";
        return 118;
    }
    if (m31_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 31 execution-progress fixture.\n";
        return 121;
    }
    if (m32_install_zip.empty() || new_array_install_zip.empty()
        || new_nothrow_install_zip.empty() || new_array_nothrow_install_zip.empty()
        || new_throw_failure_install_zip.empty() || new_nothrow_failure_install_zip.empty()
        || delete_install_zip.empty() || delete_nothrow_install_zip.empty()
        || delete_placement_install_zip.empty() || delete_array_install_zip.empty()
        || delete_array_nothrow_install_zip.empty()
        || delete_array_placement_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 32 C++ allocation fixtures.\n";
        return 124;
    }
    if (m33_install_zip.empty() || null_app_util_init_install_zip.empty()
        || invalid_app_util_init_install_zip.empty()
        || uninitialized_app_util_shutdown_install_zip.empty()
        || app_util_lifecycle_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 33 AppUtil lifecycle fixtures.\n";
        return 132;
    }
    if (m34_install_zip.empty() || unloaded_sysmodule_install_zip.empty()
        || invalid_sysmodule_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 34 Sysmodule lifecycle fixtures.\n";
        return 139;
    }
    if (m35_install_zip.empty() || null_np_service_state_install_zip.empty()
        || invalid_np_service_state_install_zip.empty()
        || uninitialized_trophy_term_install_zip.empty()
        || duplicate_np_init_install_zip.empty()
        || duplicate_trophy_init_install_zip.empty()
        || np_lifecycle_install_zip.empty() || trophy_lifecycle_install_zip.empty()) {
        std::cerr << "Could not create the Milestone 35 NP lifecycle fixtures.\n";
        return 144;
    }
    if (!emitted_self_fixture.empty()) {
        if (!emitted_self_fixture.parent_path().empty()) {
            std::filesystem::create_directories(emitted_self_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_self_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(plain_self.data()),
            static_cast<std::streamsize>(plain_self.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 16 plain SELF fixture.\n";
            return 35;
        }
    }
    if (!emitted_m16_install_zip_fixture.empty()) {
        if (!emitted_m16_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m16_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m16_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m16_install_zip.data()),
            static_cast<std::streamsize>(m16_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 16 installation fixture.\n";
            return 36;
        }
    }
    if (!emitted_m18_install_zip_fixture.empty()) {
        if (!emitted_m18_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m18_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m18_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m18_install_zip.data()),
            static_cast<std::streamsize>(m18_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 18 lifecycle-export fixture.\n";
            return 43;
        }
    }
    if (!emitted_m19_install_zip_fixture.empty()) {
        if (!emitted_m19_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m19_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m19_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m19_install_zip.data()),
            static_cast<std::streamsize>(m19_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 19 Thumb-2 PUSH.W fixture.\n";
            return 48;
        }
    }
    if (!emitted_m20_install_zip_fixture.empty()) {
        if (!emitted_m20_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m20_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m20_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m20_install_zip.data()),
            static_cast<std::streamsize>(m20_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 20 Thumb compiler baseline fixture.\n";
            return 53;
        }
    }
    if (!emitted_m21_install_zip_fixture.empty()) {
        if (!emitted_m21_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m21_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m21_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m21_install_zip.data()),
            static_cast<std::streamsize>(m21_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 21 Thumb-2 compiler batch fixture.\n";
            return 58;
        }
    }
    if (!emitted_m22_install_zip_fixture.empty()) {
        if (!emitted_m22_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m22_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m22_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m22_install_zip.data()),
            static_cast<std::streamsize>(m22_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 22 inline HLE diagnostic fixture.\n";
            return 63;
        }
    }
    if (!emitted_m23_install_zip_fixture.empty()) {
        if (!emitted_m23_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m23_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m23_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m23_install_zip.data()),
            static_cast<std::streamsize>(m23_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 23 libc runtime fixture.\n";
            return 69;
        }
    }
    if (!emitted_m24_install_zip_fixture.empty()) {
        if (!emitted_m24_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m24_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m24_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m24_install_zip.data()),
            static_cast<std::streamsize>(m24_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 24 Thumb-2 runtime-family fixture.\n";
            return 74;
        }
    }
    if (!emitted_m25_install_zip_fixture.empty()) {
        if (!emitted_m25_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m25_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m25_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m25_install_zip.data()),
            static_cast<std::streamsize>(m25_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 25 Thumb-2 register-family fixture.\n";
            return 79;
        }
    }
    if (!emitted_m26_install_zip_fixture.empty()) {
        if (!emitted_m26_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m26_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m26_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m26_install_zip.data()),
            static_cast<std::streamsize>(m26_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 26 libc termination fixture.\n";
            return 84;
        }
    }
    if (!emitted_m27_install_zip_fixture.empty()) {
        if (!emitted_m27_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m27_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m27_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m27_install_zip.data()),
            static_cast<std::streamsize>(m27_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 27 Thumb-2 multiple-transfer fixture.\n";
            return 95;
        }
    }
    if (!emitted_m28_install_zip_fixture.empty()) {
        if (!emitted_m28_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m28_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m28_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m28_install_zip.data()),
            static_cast<std::streamsize>(m28_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 28 C++ guard fixture.\n";
            return 100;
        }
    }
    if (!emitted_m29_install_zip_fixture.empty()) {
        if (!emitted_m29_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m29_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m29_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m29_install_zip.data()),
            static_cast<std::streamsize>(m29_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 29 libc heap fixture.\n";
            return 116;
        }
    }
    if (!emitted_m30_install_zip_fixture.empty()) {
        if (!emitted_m30_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m30_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m30_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m30_install_zip.data()),
            static_cast<std::streamsize>(m30_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 30 LR shifted-register fixture.\n";
            return 119;
        }
    }
    if (!emitted_m31_install_zip_fixture.empty()) {
        if (!emitted_m31_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m31_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m31_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m31_install_zip.data()),
            static_cast<std::streamsize>(m31_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 31 execution-progress fixture.\n";
            return 122;
        }
    }
    if (!emitted_m32_install_zip_fixture.empty()) {
        if (!emitted_m32_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m32_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m32_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m32_install_zip.data()),
            static_cast<std::streamsize>(m32_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 32 C++ allocation fixture.\n";
            return 125;
        }
    }
    if (!emitted_m33_install_zip_fixture.empty()) {
        if (!emitted_m33_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m33_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m33_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m33_install_zip.data()),
            static_cast<std::streamsize>(m33_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 33 AppUtil lifecycle fixture.\n";
            return 133;
        }
    }
    if (!emitted_m34_install_zip_fixture.empty()) {
        if (!emitted_m34_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m34_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m34_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m34_install_zip.data()),
            static_cast<std::streamsize>(m34_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 34 Sysmodule lifecycle fixture.\n";
            return 140;
        }
    }
    if (!emitted_m35_install_zip_fixture.empty()) {
        if (!emitted_m35_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_m35_install_zip_fixture.parent_path(), error);
        }
        std::ofstream output(emitted_m35_install_zip_fixture, std::ios::binary);
        output.write(reinterpret_cast<const char *>(m35_install_zip.data()),
            static_cast<std::streamsize>(m35_install_zip.size()));
        if (error || !output) {
            std::cerr << "Could not emit the Milestone 35 NP lifecycle fixture.\n";
            return 145;
        }
    }

    const auto fixture = test_root / "Vita3K" / "imports" / "synthetic-homebrew.elf";
    std::ofstream fixture_stream(fixture, std::ios::binary);
    fixture_stream.write(reinterpret_cast<const char *>(elf.data()), elf.size());
    fixture_stream.close();
    if (!emitted_fixture.empty()) {
        if (!emitted_fixture.parent_path().empty()) {
            std::filesystem::create_directories(emitted_fixture.parent_path(), error);
        }
        if (error || !std::filesystem::copy_file(fixture, emitted_fixture, std::filesystem::copy_options::overwrite_existing, error)) {
            std::cerr << "Could not emit the Milestone 9 fixture: " << error.message() << '\n';
            return 4;
        }
    }
    if (!emitted_sfo_fixture.empty()) {
        if (!emitted_sfo_fixture.parent_path().empty()) {
            std::filesystem::create_directories(emitted_sfo_fixture.parent_path(), error);
        }
        std::ofstream emitted_sfo_stream(emitted_sfo_fixture, std::ios::binary);
        emitted_sfo_stream.write(reinterpret_cast<const char *>(sfo_fixture.data()),
            static_cast<std::streamsize>(sfo_fixture.size()));
        if (error || !emitted_sfo_stream) {
            std::cerr << "Could not emit the Milestone 13 SFO fixture: "
                      << error.message() << '\n';
            return 24;
        }
    }
    if (!emitted_vpk_fixture.empty()) {
        if (!emitted_vpk_fixture.parent_path().empty()) {
            std::filesystem::create_directories(emitted_vpk_fixture.parent_path(), error);
        }
        std::ofstream emitted_vpk_stream(emitted_vpk_fixture, std::ios::binary);
        emitted_vpk_stream.write(reinterpret_cast<const char *>(vpk_fixture.data()),
            static_cast<std::streamsize>(vpk_fixture.size()));
        if (error || !emitted_vpk_stream) {
            std::cerr << "Could not emit the Milestone 14 VPK fixture: "
                      << error.message() << '\n';
            return 28;
        }
    }
    if (!emitted_install_zip_fixture.empty()) {
        if (!emitted_install_zip_fixture.parent_path().empty()) {
            std::filesystem::create_directories(
                emitted_install_zip_fixture.parent_path(), error);
        }
        std::ofstream emitted_install_stream(emitted_install_zip_fixture, std::ios::binary);
        emitted_install_stream.write(
            reinterpret_cast<const char *>(install_zip_fixture.data()),
            static_cast<std::streamsize>(install_zip_fixture.size()));
        if (error || !emitted_install_stream) {
            std::cerr << "Could not emit the Milestone 15 installation fixture: "
                      << error.message() << '\n';
            return 31;
        }
    }

    const auto rescanned = vita3k::ios::rescan_imports();
    if (rescanned.imported_artifacts.size() != 1 || rescanned.imported_artifacts.front().kind != "Vita ELF" || !rescanned.imported_artifacts.front().structurally_valid || rescanned.imported_artifacts.front().load_segment_count != 2 || rescanned.imported_artifacts.front().relocation_segment_count != 1 || !rescanned.imported_artifacts.front().loaded || !rescanned.imported_artifacts.front().module_info_valid || !rescanned.imported_artifacts.front().relocations_applied || !rescanned.imported_artifacts.front().module_tables_parsed || !rescanned.imported_artifacts.front().import_stubs_bound || !rescanned.imported_artifacts.front().module_start_valid || !rescanned.imported_artifacts.front().module_start_from_export || !rescanned.imported_artifacts.front().execution_attempted || !rescanned.imported_artifacts.front().thread_exited || rescanned.imported_artifacts.front().relocation_entry_count != 1 || rescanned.imported_artifacts.front().relocation_patch_count != 1 || rescanned.imported_artifacts.front().export_library_count != 1 || rescanned.imported_artifacts.front().import_library_count != 1 || rescanned.imported_artifacts.front().exported_nid_count != 1 || rescanned.imported_artifacts.front().imported_nid_count != 2 || rescanned.imported_artifacts.front().bound_import_stub_count != 2 || rescanned.imported_artifacts.front().module_start_address != 0x81000061 || rescanned.imported_artifacts.front().executed_instruction_count != 12 || rescanned.imported_artifacts.front().hle_dispatch_count != 2 || rescanned.imported_artifacts.front().thread_exit_status != 42 || rescanned.imported_artifacts.front().module_name != "synthetic-homebrew" || rescanned.imported_artifacts.front().module_nid != 0x1234ABCD) {
        std::cerr << rescanned.summary << '\n';
        return 3;
    }

    if (!vitasdk_fixture.empty()) {
        std::filesystem::remove(fixture, error);
        const auto real_fixture = test_root / "Vita3K" / "imports" / "milestone10-vitasdk-homebrew.velf";
        std::filesystem::copy_file(vitasdk_fixture, real_fixture,
            std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            std::cerr << "Could not stage the real VitaSDK fixture: " << error.message() << '\n';
            return 5;
        }
        const auto real_status = vita3k::ios::rescan_imports();
        if (real_status.imported_artifacts.size() != 1 || real_status.imported_artifacts.front().kind != "Vita ELF" || !real_status.imported_artifacts.front().structurally_valid || !real_status.imported_artifacts.front().load_attempted || !real_status.imported_artifacts.front().loaded || !real_status.imported_artifacts.front().module_info_valid || !real_status.imported_artifacts.front().relocations_applied || !real_status.imported_artifacts.front().module_tables_parsed || !real_status.imported_artifacts.front().import_stubs_bound || !real_status.imported_artifacts.front().module_start_valid || !real_status.imported_artifacts.front().execution_attempted || real_status.imported_artifacts.front().thread_exited || !real_status.imported_artifacts.front().thread_returned || real_status.imported_artifacts.front().module_name != "m10_homebrew" || real_status.imported_artifacts.front().module_start_address != 0x81000001 || real_status.imported_artifacts.front().executed_instruction_count != 6 || real_status.imported_artifacts.front().hle_dispatch_count != 1 || real_status.imported_artifacts.front().thread_return_value != 1) {
            std::cerr << real_status.summary << '\n';
            return 6;
        }
        std::cout << "Verified real VitaSDK diagnostic:\n"
                  << real_status.summary << '\n';
    }

    const auto imports = test_root / "Vita3K" / "imports";
    std::filesystem::remove_all(imports, error);
    std::filesystem::create_directories(imports, error);
    const auto imported_sfo = imports / "milestone13-synthetic-param.sfo";
    std::ofstream imported_sfo_stream(imported_sfo, std::ios::binary);
    imported_sfo_stream.write(reinterpret_cast<const char *>(sfo_fixture.data()),
        static_cast<std::streamsize>(sfo_fixture.size()));
    imported_sfo_stream.close();
    if (error || !imported_sfo_stream) {
        std::cerr << "Could not stage the Milestone 13 SFO fixture: "
                  << error.message() << '\n';
        return 25;
    }
    const auto sfo_status = vita3k::ios::rescan_imports();
    if (sfo_status.imported_artifacts.size() != 1 || sfo_status.imported_artifacts.front().kind != "PARAM.SFO" || !sfo_status.imported_artifacts.front().app_metadata_parsed || sfo_status.imported_artifacts.front().app_title_id != "M13TEST01" || sfo_status.imported_artifacts.front().app_title != "Vita3K iOS upstream metadata probe" || sfo_status.imported_artifacts.front().app_category != "gd" || sfo_status.imported_artifacts.front().app_version != "01.00" || sfo_status.summary.find("title ID M13TEST01") == std::string::npos) {
        std::cerr << sfo_status.summary << '\n';
        return 26;
    }

    std::filesystem::remove_all(imports, error);
    std::filesystem::create_directories(imports, error);
    const auto imported_vpk = imports / "milestone14-synthetic-app.vpk";
    std::ofstream imported_vpk_stream(imported_vpk, std::ios::binary);
    imported_vpk_stream.write(reinterpret_cast<const char *>(vpk_fixture.data()),
        static_cast<std::streamsize>(vpk_fixture.size()));
    imported_vpk_stream.close();
    const auto vpk_status = vita3k::ios::rescan_imports();
    if (vpk_status.imported_artifacts.size() != 1 || vpk_status.imported_artifacts.front().kind != "VPK/ZIP" || !vpk_status.imported_artifacts.front().archive_inspected || !vpk_status.imported_artifacts.front().archive_valid || vpk_status.imported_artifacts.front().archive_file_count != 3 || vpk_status.imported_artifacts.front().archive_application_count != 1 || vpk_status.imported_artifacts.front().archive_unsafe_path_count != 0 || vpk_status.imported_artifacts.front().app_title_id != "M14TEST01" || vpk_status.imported_artifacts.front().archive_install_target != "ux0/app/M14TEST01" || vpk_status.summary.find("planned target ux0/app/M14TEST01") == std::string::npos) {
        std::cerr << vpk_status.summary << '\n';
        return 29;
    }

    const auto archive_path = test_root / "milestone15-app-and-patch.zip";
    std::ofstream archive_stream(archive_path, std::ios::binary);
    archive_stream.write(reinterpret_cast<const char *>(install_zip_fixture.data()),
        static_cast<std::streamsize>(install_zip_fixture.size()));
    archive_stream.close();
    const auto previous_app = test_root / "Vita3K/ux0/app/M15TEST01/old.txt";
    std::filesystem::create_directories(previous_app.parent_path(), error);
    std::ofstream(previous_app) << "previous installation";
    const auto install_result = vita3k::ios::install_game_archive(archive_path);
    const auto installed_status = vita3k::ios::query_core_status();
    if (!install_result.success || install_result.application_count != 2 || install_result.file_count != 6 || install_result.installed_targets.size() != 2 || std::filesystem::exists(previous_app) || !std::filesystem::is_regular_file(test_root / "Vita3K/ux0/app/M15TEST01/eboot.bin") || !std::filesystem::is_regular_file(test_root / "Vita3K/ux0/patch/M15TEST01/assets/patch.dat") || installed_status.installed_titles.size() != 1 || installed_status.installed_titles.front().title_id != "M15TEST01" || !installed_status.installed_titles.front().patch_installed || installed_status.summary.find("Installed titles: 1") == std::string::npos || installed_status.summary.find("patch installed") == std::string::npos) {
        std::cerr << installed_status.summary << '\n';
        return 32;
    }
    const auto unsafe_archive_path = test_root / "milestone15-unsafe.zip";
    const auto unsafe_archive = vita3k::ios::make_synthetic_vpk(true);
    std::ofstream unsafe_stream(unsafe_archive_path, std::ios::binary);
    unsafe_stream.write(reinterpret_cast<const char *>(unsafe_archive.data()),
        static_cast<std::streamsize>(unsafe_archive.size()));
    unsafe_stream.close();
    const auto unsafe_install = vita3k::ios::install_game_archive(unsafe_archive_path);
    if (unsafe_install.success || !std::filesystem::is_regular_file(test_root / "Vita3K/ux0/app/M15TEST01/eboot.bin")) {
        std::cerr << "An unsafe archive modified the installed title.\n";
        return 33;
    }

    const auto m16_archive_path = test_root / "milestone16-app-and-patch.zip";
    std::ofstream m16_archive_stream(m16_archive_path, std::ios::binary);
    m16_archive_stream.write(reinterpret_cast<const char *>(m16_install_zip.data()),
        static_cast<std::streamsize>(m16_install_zip.size()));
    m16_archive_stream.close();
    const auto m16_install = vita3k::ios::install_game_archive(m16_archive_path);
    const auto m16_installed_status = vita3k::ios::query_core_status();
    if (!m16_install.success || m16_install.file_count != 6 || m16_installed_status.installed_titles.size() != 1 || !m16_installed_status.installed_titles.front().base_eboot_present || !m16_installed_status.installed_titles.front().patch_eboot_present) {
        std::cerr << m16_installed_status.summary << '\n';
        return 37;
    }
    const auto prepared = vita3k::ios::prepare_installed_title("M15TEST01", true);
    const auto prepared_status = vita3k::ios::query_core_status();
    if (!prepared.selected || !prepared.patch_selected || !prepared.probe_valid || !prepared.self_segments_plain || !prepared.loaded || prepared.kind != "Vita SELF" || prepared.load_segment_count != 2 || prepared.module_name != "synthetic-homebrew" || prepared.module_start_address != 0x81000061 || prepared.imported_nid_count != 2 || prepared.bound_import_stub_count != 2 || prepared_status.selected_title_id != "M15TEST01" || !prepared_status.selected_executable_loaded || !prepared_status.selected_boot_available || prepared_status.summary.find("Boot source: patch/eboot.bin") == std::string::npos || prepared_status.summary.find("Executable preparation: Mapped 2") == std::string::npos) {
        std::cerr << prepared.detail << '\n'
                  << prepared_status.summary << '\n';
        return 38;
    }

    const auto controlled_boot = vita3k::ios::attempt_prepared_title_boot(256);
    const auto controlled_boot_status = vita3k::ios::query_core_status();
    if (!controlled_boot.attempted || !controlled_boot.started || !controlled_boot.exited || controlled_boot.returned || controlled_boot.instruction_count != 12 || controlled_boot.hle_dispatch_count != 2 || controlled_boot.exit_status != 42 || controlled_boot_status.selected_boot_available || !controlled_boot_status.selected_boot_attempted || controlled_boot_status.summary.find("Controlled boot attempt: interpreter-only, 256-instruction ceiling") == std::string::npos) {
        std::cerr << controlled_boot.detail << '\n'
                  << controlled_boot_status.summary << '\n';
        return 40;
    }
    const auto repeated_boot = vita3k::ios::attempt_prepared_title_boot(256);
    if (repeated_boot.started || repeated_boot.detail.find("Select the title again") == std::string::npos) {
        std::cerr << "A prepared title was allowed to execute more than once.\n";
        return 41;
    }

    const auto m18_archive_path = test_root / "milestone18-lifecycle-export.zip";
    std::ofstream m18_archive_stream(m18_archive_path, std::ios::binary);
    m18_archive_stream.write(reinterpret_cast<const char *>(m18_install_zip.data()),
        static_cast<std::streamsize>(m18_install_zip.size()));
    m18_archive_stream.close();
    const auto m18_install = vita3k::ios::install_game_archive(m18_archive_path);
    if (!m18_install.success || m18_install.file_count != 6) {
        std::cerr << m18_install.detail << '\n';
        return 44;
    }
    const auto lifecycle_prepared = vita3k::ios::prepare_installed_title("M15TEST01", true);
    const auto lifecycle_prepared_status = vita3k::ios::query_core_status();
    if (!lifecycle_prepared.selected || !lifecycle_prepared.loaded || !lifecycle_prepared.module_start_from_export || lifecycle_prepared.module_start_address != 0x81000061 || lifecycle_prepared.detail.find("via lifecycle export") == std::string::npos || !lifecycle_prepared_status.selected_boot_available) {
        std::cerr << lifecycle_prepared.detail << '\n'
                  << lifecycle_prepared_status.summary << '\n';
        return 45;
    }
    const auto lifecycle_boot = vita3k::ios::attempt_prepared_title_boot(256);
    if (!lifecycle_boot.started || !lifecycle_boot.exited || lifecycle_boot.returned || lifecycle_boot.instruction_count != 12 || lifecycle_boot.hle_dispatch_count != 2 || lifecycle_boot.exit_status != 42) {
        std::cerr << lifecycle_boot.detail << '\n';
        return 46;
    }

    const auto m19_archive_path = test_root / "milestone19-thumb2-wide-push.zip";
    std::ofstream m19_archive_stream(m19_archive_path, std::ios::binary);
    m19_archive_stream.write(reinterpret_cast<const char *>(m19_install_zip.data()),
        static_cast<std::streamsize>(m19_install_zip.size()));
    m19_archive_stream.close();
    const auto m19_install = vita3k::ios::install_game_archive(m19_archive_path);
    if (!m19_install.success || m19_install.file_count != 6) {
        std::cerr << m19_install.detail << '\n';
        return 49;
    }
    const auto wide_push_prepared = vita3k::ios::prepare_installed_title("M15TEST01", true);
    if (!wide_push_prepared.selected || !wide_push_prepared.loaded || !wide_push_prepared.module_start_from_export || wide_push_prepared.module_start_address != 0x81000061) {
        std::cerr << wide_push_prepared.detail << '\n';
        return 50;
    }
    const auto wide_push_boot = vita3k::ios::attempt_prepared_title_boot(256);
    if (!wide_push_boot.started || !wide_push_boot.exited || wide_push_boot.returned || wide_push_boot.instruction_count != 12 || wide_push_boot.hle_dispatch_count != 2 || wide_push_boot.exit_status != 42) {
        std::cerr << wide_push_boot.detail << '\n';
        return 51;
    }

    const auto m20_archive_path = test_root / "milestone20-thumb-compiler-baseline.zip";
    std::ofstream m20_archive_stream(m20_archive_path, std::ios::binary);
    m20_archive_stream.write(reinterpret_cast<const char *>(m20_install_zip.data()),
        static_cast<std::streamsize>(m20_install_zip.size()));
    m20_archive_stream.close();
    const auto m20_install = vita3k::ios::install_game_archive(m20_archive_path);
    if (!m20_install.success || m20_install.file_count != 6) {
        std::cerr << m20_install.detail << '\n';
        return 54;
    }
    const auto compiler_baseline_prepared = vita3k::ios::prepare_installed_title("M15TEST01", true);
    if (!compiler_baseline_prepared.selected || !compiler_baseline_prepared.loaded || !compiler_baseline_prepared.module_start_from_export || compiler_baseline_prepared.module_start_address != 0x81000061) {
        std::cerr << compiler_baseline_prepared.detail << '\n';
        return 55;
    }
    const auto compiler_baseline_boot = vita3k::ios::attempt_prepared_title_boot(256);
    if (!compiler_baseline_boot.started || !compiler_baseline_boot.exited || compiler_baseline_boot.returned || compiler_baseline_boot.instruction_count != 13 || compiler_baseline_boot.hle_dispatch_count != 2 || compiler_baseline_boot.exit_status != 42) {
        std::cerr << compiler_baseline_boot.detail << '\n';
        return 56;
    }

    const auto m21_archive_path = test_root / "milestone21-thumb2-compiler-batch.zip";
    std::ofstream m21_archive_stream(m21_archive_path, std::ios::binary);
    m21_archive_stream.write(reinterpret_cast<const char *>(m21_install_zip.data()),
        static_cast<std::streamsize>(m21_install_zip.size()));
    m21_archive_stream.close();
    const auto m21_install = vita3k::ios::install_game_archive(m21_archive_path);
    if (!m21_install.success || m21_install.file_count != 6) {
        std::cerr << m21_install.detail << '\n';
        return 59;
    }
    const auto thumb2_compiler_prepared = vita3k::ios::prepare_installed_title("M15TEST01", true);
    if (!thumb2_compiler_prepared.selected || !thumb2_compiler_prepared.loaded || !thumb2_compiler_prepared.module_start_from_export || thumb2_compiler_prepared.module_start_address != 0x81000061) {
        std::cerr << thumb2_compiler_prepared.detail << '\n';
        return 60;
    }
    const auto thumb2_compiler_boot = vita3k::ios::attempt_prepared_title_boot(256);
    if (!thumb2_compiler_boot.started || !thumb2_compiler_boot.exited || thumb2_compiler_boot.returned || thumb2_compiler_boot.instruction_count != 9 || thumb2_compiler_boot.hle_dispatch_count != 1 || thumb2_compiler_boot.exit_status != 42) {
        std::cerr << thumb2_compiler_boot.detail << '\n';
        return 61;
    }

    const auto m22_archive_path = test_root / "milestone22-inline-hle-diagnostic.zip";
    std::ofstream m22_archive_stream(m22_archive_path, std::ios::binary);
    m22_archive_stream.write(reinterpret_cast<const char *>(m22_install_zip.data()),
        static_cast<std::streamsize>(m22_install_zip.size()));
    m22_archive_stream.close();
    const auto m22_install = vita3k::ios::install_game_archive(m22_archive_path);
    if (!m22_install.success || m22_install.file_count != 6) {
        std::cerr << m22_install.detail << '\n';
        return 65;
    }
    const auto inline_hle_prepared = vita3k::ios::prepare_installed_title("M15TEST01", true);
    if (!inline_hle_prepared.selected || !inline_hle_prepared.loaded || !inline_hle_prepared.module_start_from_export || inline_hle_prepared.module_start_address != 0x81000061) {
        std::cerr << inline_hle_prepared.detail << '\n';
        return 66;
    }
    const auto inline_hle_boot = vita3k::ios::attempt_prepared_title_boot(256);
    if (!inline_hle_boot.started || inline_hle_boot.exited || inline_hle_boot.returned || inline_hle_boot.instruction_count != 9 || inline_hle_boot.hle_dispatch_count != 0 || inline_hle_boot.last_hle_nid != diagnostic_unbound_nid || inline_hle_boot.last_guest_pc != 0x81000054u || inline_hle_boot.detail.find("__sceAppMgrGetAppState") == std::string::npos || inline_hle_boot.detail.find("module import inventory: present") == std::string::npos || inline_hle_boot.detail.find("r0=0x0000002A") == std::string::npos || inline_hle_boot.detail.find("r2=0x812C7690") == std::string::npos) {
        std::cerr << inline_hle_boot.detail << '\n';
        return 67;
    }

    const auto m23_archive_path = test_root / "milestone23-libc-dso-runtime.zip";
    std::ofstream m23_archive_stream(m23_archive_path, std::ios::binary);
    m23_archive_stream.write(reinterpret_cast<const char *>(m23_install_zip.data()),
        static_cast<std::streamsize>(m23_install_zip.size()));
    m23_archive_stream.close();
    const auto m23_install = vita3k::ios::install_game_archive(m23_archive_path);
    if (!m23_install.success || m23_install.file_count != 6) {
        std::cerr << m23_install.detail << '\n';
        return 70;
    }
    const auto libc_dso_runtime_prepared = vita3k::ios::prepare_installed_title("M15TEST01", true);
    if (!libc_dso_runtime_prepared.selected || !libc_dso_runtime_prepared.loaded || !libc_dso_runtime_prepared.module_start_from_export || libc_dso_runtime_prepared.module_start_address != 0x81000061) {
        std::cerr << libc_dso_runtime_prepared.detail << '\n';
        return 71;
    }
    const auto libc_dso_runtime_boot = vita3k::ios::attempt_prepared_title_boot(256);
    if (!libc_dso_runtime_boot.started || libc_dso_runtime_boot.exited || !libc_dso_runtime_boot.returned || libc_dso_runtime_boot.instruction_count != 7 || libc_dso_runtime_boot.hle_dispatch_count != 1 || libc_dso_runtime_boot.last_hle_nid != cxa_set_dso_handle_main_nid || libc_dso_runtime_boot.libc_dso_handle_main != synthetic_dso_handle || libc_dso_runtime_boot.return_value != 0 || libc_dso_runtime_boot.detail.find("Runtime DSO handle=0x81000200") == std::string::npos) {
        std::cerr << libc_dso_runtime_boot.detail << '\n';
        return 72;
    }

    const auto m24_archive_path = test_root / "milestone24-thumb2-runtime-families.zip";
    std::ofstream m24_archive_stream(m24_archive_path, std::ios::binary);
    m24_archive_stream.write(reinterpret_cast<const char *>(m24_install_zip.data()),
        static_cast<std::streamsize>(m24_install_zip.size()));
    m24_archive_stream.close();
    const auto m24_install = vita3k::ios::install_game_archive(m24_archive_path);
    if (!m24_install.success || m24_install.file_count != 6) {
        std::cerr << m24_install.detail << '\n';
        return 75;
    }
    const auto runtime_family_prepared = vita3k::ios::prepare_installed_title("M15TEST01", true);
    if (!runtime_family_prepared.selected || !runtime_family_prepared.loaded || !runtime_family_prepared.module_start_from_export || runtime_family_prepared.module_start_address != 0x81000061) {
        std::cerr << runtime_family_prepared.detail << '\n';
        return 76;
    }
    const auto runtime_family_boot = vita3k::ios::attempt_prepared_title_boot(256);
    if (!runtime_family_boot.started || runtime_family_boot.exited || !runtime_family_boot.returned || runtime_family_boot.instruction_count != 9 || runtime_family_boot.hle_dispatch_count != 1 || runtime_family_boot.last_hle_nid != cxa_set_dso_handle_main_nid || runtime_family_boot.libc_dso_handle_main != synthetic_dso_handle || runtime_family_boot.return_value != 0) {
        std::cerr << runtime_family_boot.detail << '\n';
        return 77;
    }

    const auto m25_archive_path = test_root / "milestone25-thumb2-register-families.zip";
    std::ofstream m25_archive_stream(m25_archive_path, std::ios::binary);
    m25_archive_stream.write(reinterpret_cast<const char *>(m25_install_zip.data()),
        static_cast<std::streamsize>(m25_install_zip.size()));
    m25_archive_stream.close();
    const auto m25_install = vita3k::ios::install_game_archive(m25_archive_path);
    if (!m25_install.success || m25_install.file_count != 6) {
        std::cerr << m25_install.detail << '\n';
        return 80;
    }
    const auto register_family_prepared = vita3k::ios::prepare_installed_title("M15TEST01", true);
    if (!register_family_prepared.selected || !register_family_prepared.loaded || !register_family_prepared.module_start_from_export || register_family_prepared.module_start_address != 0x81000061) {
        std::cerr << register_family_prepared.detail << '\n';
        return 81;
    }
    const auto register_family_boot = vita3k::ios::attempt_prepared_title_boot(256);
    if (!register_family_boot.started || register_family_boot.exited || !register_family_boot.returned || register_family_boot.instruction_count != 10 || register_family_boot.hle_dispatch_count != 1 || register_family_boot.last_hle_nid != cxa_set_dso_handle_main_nid || register_family_boot.libc_dso_handle_main != synthetic_dso_handle || register_family_boot.return_value != 0) {
        std::cerr << register_family_boot.detail << '\n';
        return 82;
    }

    const auto m26_archive_path = test_root / "milestone26-libc-termination.zip";
    std::ofstream m26_archive_stream(m26_archive_path, std::ios::binary);
    m26_archive_stream.write(reinterpret_cast<const char *>(m26_install_zip.data()),
        static_cast<std::streamsize>(m26_install_zip.size()));
    m26_archive_stream.close();
    const auto m26_install = vita3k::ios::install_game_archive(m26_archive_path);
    if (!m26_install.success || m26_install.file_count != 6) {
        std::cerr << m26_install.detail << '\n';
        return 85;
    }
    const auto libc_termination_prepared = vita3k::ios::prepare_installed_title("M15TEST01", true);
    if (!libc_termination_prepared.selected || !libc_termination_prepared.loaded
        || !libc_termination_prepared.module_start_from_export
        || libc_termination_prepared.module_start_address != 0x81000061) {
        std::cerr << libc_termination_prepared.detail << '\n';
        return 86;
    }
    const auto libc_termination_boot = vita3k::ios::attempt_prepared_title_boot(256);
    if (!libc_termination_boot.started || libc_termination_boot.exited
        || !libc_termination_boot.returned || libc_termination_boot.instruction_count != 9
        || libc_termination_boot.hle_dispatch_count != 1
        || libc_termination_boot.last_hle_nid != aeabi_atexit_nid
        || libc_termination_boot.libc_atexit_registration_count != 1
        || libc_termination_boot.last_libc_atexit_object != synthetic_atexit_object
        || libc_termination_boot.last_libc_atexit_destructor != synthetic_atexit_destructor
        || libc_termination_boot.last_libc_atexit_dso != synthetic_dso_handle
        || libc_termination_boot.return_value != 0
        || libc_termination_boot.detail.find("Libc termination registrations=1") == std::string::npos) {
        std::cerr << libc_termination_boot.detail << '\n';
        return 87;
    }

    const auto cxa_atexit_archive_path = test_root / "milestone26-cxa-atexit.zip";
    std::ofstream cxa_atexit_archive_stream(cxa_atexit_archive_path, std::ios::binary);
    cxa_atexit_archive_stream.write(reinterpret_cast<const char *>(cxa_atexit_install_zip.data()),
        static_cast<std::streamsize>(cxa_atexit_install_zip.size()));
    cxa_atexit_archive_stream.close();
    if (!vita3k::ios::install_game_archive(cxa_atexit_archive_path).success) {
        return 88;
    }
    const auto cxa_atexit_prepared = vita3k::ios::prepare_installed_title("M15TEST01", true);
    if (!cxa_atexit_prepared.loaded) {
        std::cerr << cxa_atexit_prepared.detail << '\n';
        return 89;
    }
    const auto cxa_atexit_boot = vita3k::ios::attempt_prepared_title_boot(256);
    if (!cxa_atexit_boot.returned || cxa_atexit_boot.instruction_count != 9
        || cxa_atexit_boot.hle_dispatch_count != 1
        || cxa_atexit_boot.last_hle_nid != cxa_atexit_nid
        || cxa_atexit_boot.libc_atexit_registration_count != 1
        || cxa_atexit_boot.last_libc_atexit_object != synthetic_atexit_object
        || cxa_atexit_boot.last_libc_atexit_destructor != synthetic_atexit_destructor
        || cxa_atexit_boot.last_libc_atexit_dso != synthetic_dso_handle) {
        std::cerr << cxa_atexit_boot.detail << '\n';
        return 90;
    }

    const auto cxa_finalize_archive_path = test_root / "milestone26-cxa-finalize.zip";
    std::ofstream cxa_finalize_archive_stream(cxa_finalize_archive_path, std::ios::binary);
    cxa_finalize_archive_stream.write(reinterpret_cast<const char *>(cxa_finalize_install_zip.data()),
        static_cast<std::streamsize>(cxa_finalize_install_zip.size()));
    cxa_finalize_archive_stream.close();
    if (!vita3k::ios::install_game_archive(cxa_finalize_archive_path).success) {
        return 91;
    }
    const auto cxa_finalize_prepared = vita3k::ios::prepare_installed_title("M15TEST01", true);
    if (!cxa_finalize_prepared.loaded) {
        std::cerr << cxa_finalize_prepared.detail << '\n';
        return 92;
    }
    const auto cxa_finalize_boot = vita3k::ios::attempt_prepared_title_boot(256);
    if (!cxa_finalize_boot.returned || cxa_finalize_boot.instruction_count != 9
        || cxa_finalize_boot.hle_dispatch_count != 1
        || cxa_finalize_boot.last_hle_nid != cxa_finalize_nid
        || cxa_finalize_boot.libc_finalize_call_count != 1
        || cxa_finalize_boot.last_libc_finalize_dso != synthetic_dso_handle
        || cxa_finalize_boot.return_value != 0
        || cxa_finalize_boot.detail.find("Libc finalize calls=1") == std::string::npos) {
        std::cerr << cxa_finalize_boot.detail << '\n';
        return 93;
    }

    const auto m27_archive_path = test_root / "milestone27-thumb2-multiple-transfer.zip";
    std::ofstream m27_archive_stream(m27_archive_path, std::ios::binary);
    m27_archive_stream.write(reinterpret_cast<const char *>(m27_install_zip.data()),
        static_cast<std::streamsize>(m27_install_zip.size()));
    m27_archive_stream.close();
    const auto m27_install = vita3k::ios::install_game_archive(m27_archive_path);
    if (!m27_install.success || m27_install.file_count != 6) {
        std::cerr << m27_install.detail << '\n';
        return 96;
    }
    const auto multiple_transfer_prepared = vita3k::ios::prepare_installed_title(
        "M15TEST01", true);
    if (!multiple_transfer_prepared.selected || !multiple_transfer_prepared.loaded
        || !multiple_transfer_prepared.module_start_from_export
        || multiple_transfer_prepared.module_start_address != 0x81000061) {
        std::cerr << multiple_transfer_prepared.detail << '\n';
        return 97;
    }
    const auto multiple_transfer_boot = vita3k::ios::attempt_prepared_title_boot(256);
    if (!multiple_transfer_boot.started || multiple_transfer_boot.exited
        || !multiple_transfer_boot.returned
        || multiple_transfer_boot.instruction_count != 2
        || multiple_transfer_boot.hle_dispatch_count != 0
        || multiple_transfer_boot.return_value != 0) {
        std::cerr << multiple_transfer_boot.detail << '\n';
        return 98;
    }

    const auto run_guard_fixture = [&](std::span<const std::uint8_t> archive,
                                       std::string_view filename) {
        const auto archive_path = test_root / filename;
        std::ofstream archive_stream(archive_path, std::ios::binary);
        archive_stream.write(reinterpret_cast<const char *>(archive.data()),
            static_cast<std::streamsize>(archive.size()));
        archive_stream.close();
        const auto install = vita3k::ios::install_game_archive(archive_path);
        if (!install.success) {
            std::cerr << install.detail << '\n';
            return vita3k::ios::TitleBootResult{};
        }
        const auto prepared = vita3k::ios::prepare_installed_title("M15TEST01", true);
        if (!prepared.loaded) {
            std::cerr << prepared.detail << '\n';
            return vita3k::ios::TitleBootResult{};
        }
        return vita3k::ios::attempt_prepared_title_boot(256);
    };
    const auto guard_acquire_boot = run_guard_fixture(
        m28_install_zip, "milestone28-cxa-guard-acquire.zip");
    if (!guard_acquire_boot.returned || guard_acquire_boot.instruction_count != 7
        || guard_acquire_boot.hle_dispatch_count != 1
        || guard_acquire_boot.last_hle_nid != cxa_guard_acquire_nid
        || guard_acquire_boot.libc_guard_acquire_count != 1
        || guard_acquire_boot.libc_guard_initialization_count != 1
        || guard_acquire_boot.libc_guard_recursive_acquire_count != 0
        || guard_acquire_boot.last_libc_guard_address != synthetic_guard_address
        || guard_acquire_boot.last_libc_guard_word != 0
        || guard_acquire_boot.last_libc_guard_result != 1
        || guard_acquire_boot.return_value != 1
        || guard_acquire_boot.detail.find("C++ guards: acquire=1") == std::string::npos) {
        std::cerr << guard_acquire_boot.detail << '\n';
        return 101;
    }
    const auto initialized_guard_boot = run_guard_fixture(
        initialized_guard_install_zip, "milestone28-cxa-guard-initialized.zip");
    if (!initialized_guard_boot.returned || initialized_guard_boot.instruction_count != 10
        || initialized_guard_boot.hle_dispatch_count != 1
        || initialized_guard_boot.last_hle_nid != cxa_guard_acquire_nid
        || initialized_guard_boot.libc_guard_acquire_count != 1
        || initialized_guard_boot.libc_guard_initialization_count != 0
        || initialized_guard_boot.last_libc_guard_address != synthetic_guard_address
        || initialized_guard_boot.last_libc_guard_word != 1
        || initialized_guard_boot.last_libc_guard_result != 0
        || initialized_guard_boot.return_value != 0) {
        std::cerr << initialized_guard_boot.detail << '\n';
        return 102;
    }
    const auto guard_release_boot = run_guard_fixture(
        cxa_guard_release_install_zip, "milestone28-cxa-guard-release.zip");
    if (!guard_release_boot.returned || guard_release_boot.instruction_count != 7
        || guard_release_boot.hle_dispatch_count != 1
        || guard_release_boot.last_hle_nid != cxa_guard_release_nid
        || guard_release_boot.libc_guard_release_count != 1
        || guard_release_boot.last_libc_guard_address != synthetic_guard_address
        || guard_release_boot.last_libc_guard_word != 1
        || guard_release_boot.last_libc_guard_result != 0) {
        std::cerr << guard_release_boot.detail << '\n';
        return 103;
    }
    const auto guard_abort_boot = run_guard_fixture(
        cxa_guard_abort_install_zip, "milestone28-cxa-guard-abort.zip");
    if (!guard_abort_boot.returned || guard_abort_boot.instruction_count != 7
        || guard_abort_boot.hle_dispatch_count != 1
        || guard_abort_boot.last_hle_nid != cxa_guard_abort_nid
        || guard_abort_boot.libc_guard_abort_count != 1
        || guard_abort_boot.last_libc_guard_address != synthetic_guard_address
        || guard_abort_boot.last_libc_guard_word != 0
        || guard_abort_boot.last_libc_guard_result != 0) {
        std::cerr << guard_abort_boot.detail << '\n';
        return 104;
    }
    const auto recursive_guard_boot = run_guard_fixture(
        recursive_guard_install_zip, "milestone28-cxa-guard-recursive.zip");
    if (!recursive_guard_boot.started || recursive_guard_boot.returned
        || recursive_guard_boot.hle_dispatch_count != 2
        || recursive_guard_boot.last_hle_nid != cxa_guard_acquire_nid
        || recursive_guard_boot.libc_guard_acquire_count != 2
        || recursive_guard_boot.libc_guard_initialization_count != 1
        || recursive_guard_boot.libc_guard_recursive_acquire_count != 1
        || recursive_guard_boot.detail.find("recursive initialization was detected") == std::string::npos) {
        std::cerr << recursive_guard_boot.detail << '\n';
        return 105;
    }
    const auto libc_heap_boot = run_guard_fixture(
        m29_install_zip, "milestone29-libc-heap.zip");
    if (!libc_heap_boot.returned || libc_heap_boot.instruction_count != 8
        || libc_heap_boot.hle_dispatch_count != 1
        || libc_heap_boot.last_hle_nid != memalign_nid
        || libc_heap_boot.libc_heap_allocation_count != 1
        || libc_heap_boot.libc_heap_free_count != 0
        || libc_heap_boot.libc_heap_failure_count != 0
        || libc_heap_boot.libc_heap_live_bytes != 0x180
        || libc_heap_boot.libc_heap_peak_bytes != 0x180
        || libc_heap_boot.last_libc_heap_address != synthetic_heap_base
        || libc_heap_boot.last_libc_heap_size != 0x180
        || libc_heap_boot.last_libc_heap_alignment != 16
        || libc_heap_boot.return_value != synthetic_heap_base
        || libc_heap_boot.detail.find("Libc heap: allocations=1") == std::string::npos) {
        std::cerr << libc_heap_boot.detail << '\n';
        return 117;
    }
    const auto lr_shift_boot = run_guard_fixture(
        m30_install_zip, "milestone30-thumb2-lr-shifted-register.zip");
    if (!lr_shift_boot.started || !lr_shift_boot.returned
        || lr_shift_boot.instruction_count != 6
        || lr_shift_boot.hle_dispatch_count != 0
        || lr_shift_boot.return_value != 44) {
        std::cerr << lr_shift_boot.detail << '\n';
        return 120;
    }
    const auto execution_progress_boot = run_guard_fixture(
        m31_install_zip, "milestone31-execution-progress.zip");
    if (!execution_progress_boot.started || execution_progress_boot.returned
        || execution_progress_boot.exited
        || execution_progress_boot.instruction_count != 256
        || execution_progress_boot.unique_pc_count != 1
        || execution_progress_boot.hottest_pc != 0x81000060u
        || execution_progress_boot.hottest_pc_hits != 256
        || execution_progress_boot.non_forward_pc_count != 256
        || execution_progress_boot.detail.find("Hot-loop candidate detected") == std::string::npos
        || execution_progress_boot.detail.find("Registers:") == std::string::npos) {
        std::cerr << execution_progress_boot.detail << '\n';
        return 123;
    }
    const auto cpp_new_boot = run_guard_fixture(
        m32_install_zip, "milestone32-cxx-new.zip");
    if (!cpp_new_boot.returned || cpp_new_boot.instruction_count != 7
        || cpp_new_boot.hle_dispatch_count != 1
        || cpp_new_boot.last_hle_nid != new_nid
        || cpp_new_boot.cxx_new_call_count != 1
        || cpp_new_boot.cxx_new_array_call_count != 0
        || cpp_new_boot.libc_heap_allocation_count != 1
        || cpp_new_boot.libc_heap_live_bytes != 8
        || cpp_new_boot.return_value != synthetic_heap_base
        || cpp_new_boot.detail.find("C++ allocation ABI: new=1") == std::string::npos) {
        std::cerr << cpp_new_boot.detail << '\n';
        return 126;
    }
    const auto cpp_new_array_boot = run_guard_fixture(
        new_array_install_zip, "milestone32-cxx-new-array.zip");
    if (!cpp_new_array_boot.returned || cpp_new_array_boot.last_hle_nid != new_array_nid
        || cpp_new_array_boot.cxx_new_call_count != 0
        || cpp_new_array_boot.cxx_new_array_call_count != 1
        || cpp_new_array_boot.libc_heap_live_bytes != 24
        || cpp_new_array_boot.return_value != synthetic_heap_base) {
        std::cerr << cpp_new_array_boot.detail << '\n';
        return 127;
    }
    const auto cpp_new_nothrow_boot = run_guard_fixture(
        new_nothrow_install_zip, "milestone32-cxx-new-nothrow.zip");
    const auto cpp_new_array_nothrow_boot = run_guard_fixture(
        new_array_nothrow_install_zip, "milestone32-cxx-new-array-nothrow.zip");
    if (!cpp_new_nothrow_boot.returned || cpp_new_nothrow_boot.last_hle_nid != new_nothrow_nid
        || cpp_new_nothrow_boot.cxx_new_call_count != 1
        || cpp_new_nothrow_boot.cxx_nothrow_failure_count != 0
        || cpp_new_nothrow_boot.return_value != synthetic_heap_base
        || !cpp_new_array_nothrow_boot.returned
        || cpp_new_array_nothrow_boot.last_hle_nid != new_array_nothrow_nid
        || cpp_new_array_nothrow_boot.cxx_new_array_call_count != 1
        || cpp_new_array_nothrow_boot.cxx_nothrow_failure_count != 0
        || cpp_new_array_nothrow_boot.return_value != synthetic_heap_base) {
        std::cerr << cpp_new_nothrow_boot.detail << '\n'
                  << cpp_new_array_nothrow_boot.detail << '\n';
        return 128;
    }
    const auto cpp_new_throw_failure_boot = run_guard_fixture(
        new_throw_failure_install_zip, "milestone32-cxx-new-throw-failure.zip");
    const auto cpp_new_nothrow_failure_boot = run_guard_fixture(
        new_nothrow_failure_install_zip, "milestone32-cxx-new-nothrow-failure.zip");
    if (!cpp_new_throw_failure_boot.started || cpp_new_throw_failure_boot.returned
        || cpp_new_throw_failure_boot.libc_heap_failure_count != 1
        || cpp_new_throw_failure_boot.detail.find(
            "guest C++ allocation-failure unwinding is not implemented") == std::string::npos
        || !cpp_new_nothrow_failure_boot.returned
        || cpp_new_nothrow_failure_boot.libc_heap_failure_count != 1
        || cpp_new_nothrow_failure_boot.cxx_nothrow_failure_count != 1
        || cpp_new_nothrow_failure_boot.return_value != 0) {
        std::cerr << cpp_new_throw_failure_boot.detail << '\n'
                  << cpp_new_nothrow_failure_boot.detail << '\n';
        return 129;
    }
    const auto cpp_delete_boot = run_guard_fixture(
        delete_install_zip, "milestone32-cxx-delete.zip");
    const auto cpp_delete_nothrow_boot = run_guard_fixture(
        delete_nothrow_install_zip, "milestone32-cxx-delete-nothrow.zip");
    const auto cpp_delete_placement_boot = run_guard_fixture(
        delete_placement_install_zip, "milestone32-cxx-delete-placement.zip");
    if (!cpp_delete_boot.returned || cpp_delete_boot.last_hle_nid != delete_nid
        || cpp_delete_boot.cxx_delete_call_count != 1
        || cpp_delete_boot.cxx_placement_delete_call_count != 0
        || !cpp_delete_nothrow_boot.returned
        || cpp_delete_nothrow_boot.last_hle_nid != delete_nothrow_nid
        || cpp_delete_nothrow_boot.cxx_delete_call_count != 1
        || !cpp_delete_placement_boot.returned
        || cpp_delete_placement_boot.last_hle_nid != delete_placement_nid
        || cpp_delete_placement_boot.cxx_delete_call_count != 1
        || cpp_delete_placement_boot.cxx_placement_delete_call_count != 1) {
        std::cerr << cpp_delete_boot.detail << '\n'
                  << cpp_delete_nothrow_boot.detail << '\n'
                  << cpp_delete_placement_boot.detail << '\n';
        return 130;
    }
    const auto cpp_delete_array_boot = run_guard_fixture(
        delete_array_install_zip, "milestone32-cxx-delete-array.zip");
    const auto cpp_delete_array_nothrow_boot = run_guard_fixture(
        delete_array_nothrow_install_zip, "milestone32-cxx-delete-array-nothrow.zip");
    const auto cpp_delete_array_placement_boot = run_guard_fixture(
        delete_array_placement_install_zip, "milestone32-cxx-delete-array-placement.zip");
    if (!cpp_delete_array_boot.returned
        || cpp_delete_array_boot.last_hle_nid != delete_array_nid
        || cpp_delete_array_boot.cxx_delete_array_call_count != 1
        || cpp_delete_array_boot.cxx_placement_delete_call_count != 0
        || !cpp_delete_array_nothrow_boot.returned
        || cpp_delete_array_nothrow_boot.last_hle_nid != delete_array_nothrow_nid
        || cpp_delete_array_nothrow_boot.cxx_delete_array_call_count != 1
        || !cpp_delete_array_placement_boot.returned
        || cpp_delete_array_placement_boot.last_hle_nid != delete_array_placement_nid
        || cpp_delete_array_placement_boot.cxx_delete_array_call_count != 1
        || cpp_delete_array_placement_boot.cxx_placement_delete_call_count != 1) {
        std::cerr << cpp_delete_array_boot.detail << '\n'
                  << cpp_delete_array_nothrow_boot.detail << '\n'
                  << cpp_delete_array_placement_boot.detail << '\n';
        return 131;
    }
    const auto app_util_init_boot = run_guard_fixture(
        m33_install_zip, "milestone33-app-util-init.zip");
    if (!app_util_init_boot.returned || app_util_init_boot.instruction_count != 8
        || app_util_init_boot.hle_dispatch_count != 1
        || app_util_init_boot.last_hle_nid != app_util_init_nid
        || !app_util_init_boot.app_util_initialized
        || app_util_init_boot.app_util_init_call_count != 1
        || app_util_init_boot.app_util_shutdown_call_count != 0
        || app_util_init_boot.last_app_util_init_param != synthetic_app_util_init_param
        || app_util_init_boot.last_app_util_boot_param != synthetic_app_util_boot_param
        || app_util_init_boot.last_app_util_work_buffer_size != 0
        || app_util_init_boot.last_app_util_boot_attribute != 0
        || app_util_init_boot.last_app_util_app_version != 0
        || app_util_init_boot.last_app_util_result != 0
        || app_util_init_boot.return_value != 0
        || app_util_init_boot.detail.find(
            "AppUtil lifecycle: initialized=yes, init calls=1") == std::string::npos) {
        std::cerr << app_util_init_boot.detail << '\n';
        return 134;
    }
    const auto null_app_util_init_boot = run_guard_fixture(
        null_app_util_init_install_zip, "milestone33-app-util-null-init.zip");
    if (!null_app_util_init_boot.returned || null_app_util_init_boot.app_util_initialized
        || null_app_util_init_boot.app_util_init_call_count != 1
        || static_cast<std::uint32_t>(null_app_util_init_boot.last_app_util_result)
            != app_util_error_parameter
        || null_app_util_init_boot.return_value != app_util_error_parameter) {
        std::cerr << null_app_util_init_boot.detail << '\n';
        return 135;
    }
    const auto invalid_app_util_init_boot = run_guard_fixture(
        invalid_app_util_init_install_zip, "milestone33-app-util-invalid-init.zip");
    if (!invalid_app_util_init_boot.started || invalid_app_util_init_boot.returned
        || invalid_app_util_init_boot.app_util_initialized
        || invalid_app_util_init_boot.app_util_init_call_count != 1
        || invalid_app_util_init_boot.detail.find(
            "AppUtil boundary: sceAppUtilInit initParam guest-memory validation failed")
            == std::string::npos) {
        std::cerr << invalid_app_util_init_boot.detail << '\n';
        return 136;
    }
    const auto uninitialized_app_util_shutdown_boot = run_guard_fixture(
        uninitialized_app_util_shutdown_install_zip,
        "milestone33-app-util-uninitialized-shutdown.zip");
    if (!uninitialized_app_util_shutdown_boot.returned
        || uninitialized_app_util_shutdown_boot.app_util_initialized
        || uninitialized_app_util_shutdown_boot.app_util_shutdown_call_count != 1
        || static_cast<std::uint32_t>(
            uninitialized_app_util_shutdown_boot.last_app_util_result)
            != app_util_error_not_initialized
        || uninitialized_app_util_shutdown_boot.return_value
            != app_util_error_not_initialized) {
        std::cerr << uninitialized_app_util_shutdown_boot.detail << '\n';
        return 137;
    }
    const auto app_util_lifecycle_boot = run_guard_fixture(
        app_util_lifecycle_install_zip, "milestone33-app-util-lifecycle.zip");
    if (!app_util_lifecycle_boot.returned
        || app_util_lifecycle_boot.instruction_count != 12
        || app_util_lifecycle_boot.hle_dispatch_count != 2
        || app_util_lifecycle_boot.last_hle_nid != app_util_shutdown_nid
        || app_util_lifecycle_boot.app_util_initialized
        || app_util_lifecycle_boot.app_util_init_call_count != 1
        || app_util_lifecycle_boot.app_util_shutdown_call_count != 1
        || app_util_lifecycle_boot.last_app_util_result != 0
        || app_util_lifecycle_boot.return_value != 0
        || app_util_lifecycle_boot.detail.find(
            "initialized=no, init calls=1, shutdown calls=1") == std::string::npos) {
        std::cerr << app_util_lifecycle_boot.detail << '\n';
        return 138;
    }
    const auto unloaded_sysmodule_boot = run_guard_fixture(
        unloaded_sysmodule_install_zip, "milestone34-sysmodule-unloaded.zip");
    if (!unloaded_sysmodule_boot.returned
        || unloaded_sysmodule_boot.hle_dispatch_count != 1
        || unloaded_sysmodule_boot.last_hle_nid != sysmodule_is_loaded_nid
        || unloaded_sysmodule_boot.sysmodule_is_loaded_call_count != 1
        || unloaded_sysmodule_boot.loaded_sysmodule_count != 0
        || unloaded_sysmodule_boot.last_sysmodule_id != sysmodule_np_id
        || static_cast<std::uint32_t>(unloaded_sysmodule_boot.last_sysmodule_result)
            != sysmodule_error_unloaded
        || unloaded_sysmodule_boot.return_value != sysmodule_error_unloaded) {
        std::cerr << unloaded_sysmodule_boot.detail << '\n';
        return 141;
    }
    const auto invalid_sysmodule_boot = run_guard_fixture(
        invalid_sysmodule_install_zip, "milestone34-sysmodule-invalid.zip");
    if (!invalid_sysmodule_boot.returned
        || invalid_sysmodule_boot.hle_dispatch_count != 1
        || invalid_sysmodule_boot.last_hle_nid != sysmodule_load_nid
        || invalid_sysmodule_boot.sysmodule_load_call_count != 1
        || invalid_sysmodule_boot.loaded_sysmodule_count != 0
        || invalid_sysmodule_boot.last_sysmodule_id != 0x57u
        || static_cast<std::uint32_t>(invalid_sysmodule_boot.last_sysmodule_result)
            != sysmodule_error_invalid_value
        || invalid_sysmodule_boot.return_value != sysmodule_error_invalid_value) {
        std::cerr << invalid_sysmodule_boot.detail << '\n';
        return 142;
    }
    const auto sysmodule_lifecycle_boot = run_guard_fixture(
        m34_install_zip, "milestone34-sysmodule-lifecycle.zip");
    if (!sysmodule_lifecycle_boot.returned
        || sysmodule_lifecycle_boot.hle_dispatch_count != 4
        || sysmodule_lifecycle_boot.last_hle_nid != sysmodule_is_loaded_nid
        || sysmodule_lifecycle_boot.sysmodule_load_call_count != 1
        || sysmodule_lifecycle_boot.sysmodule_is_loaded_call_count != 2
        || sysmodule_lifecycle_boot.sysmodule_unload_call_count != 1
        || sysmodule_lifecycle_boot.loaded_sysmodule_count != 0
        || sysmodule_lifecycle_boot.last_sysmodule_id != sysmodule_np_id
        || static_cast<std::uint32_t>(sysmodule_lifecycle_boot.last_sysmodule_result)
            != sysmodule_error_unloaded
        || sysmodule_lifecycle_boot.return_value != sysmodule_error_unloaded
        || sysmodule_lifecycle_boot.detail.find(
            "Sysmodule lifecycle: loaded=0, load calls=1, status calls=2, unload calls=1")
            == std::string::npos) {
        std::cerr << sysmodule_lifecycle_boot.detail << '\n';
        return 143;
    }
    const auto np_init_boot = run_guard_fixture(
        m35_install_zip, "milestone35-np-init.zip");
    if (!np_init_boot.returned || np_init_boot.hle_dispatch_count != 1
        || np_init_boot.last_hle_nid != np_init_nid || !np_init_boot.np_initialized
        || np_init_boot.np_init_call_count != 1 || np_init_boot.last_np_result != 0
        || np_init_boot.return_value != 0
        || np_init_boot.detail.find(
            "NP lifecycle: initialized=yes, init calls=1") == std::string::npos) {
        std::cerr << np_init_boot.detail << '\n';
        return 146;
    }
    const auto null_np_service_state_boot = run_guard_fixture(
        null_np_service_state_install_zip, "milestone35-np-null-service-state.zip");
    if (!null_np_service_state_boot.returned
        || null_np_service_state_boot.np_service_state_call_count != 1
        || static_cast<std::uint32_t>(null_np_service_state_boot.last_np_result)
            != np_error_invalid_argument
        || null_np_service_state_boot.return_value != np_error_invalid_argument) {
        std::cerr << null_np_service_state_boot.detail << '\n';
        return 147;
    }
    const auto invalid_np_service_state_boot = run_guard_fixture(
        invalid_np_service_state_install_zip,
        "milestone35-np-invalid-service-state.zip");
    if (!invalid_np_service_state_boot.started || invalid_np_service_state_boot.returned
        || invalid_np_service_state_boot.np_service_state_call_count != 1
        || invalid_np_service_state_boot.detail.find(
            "NP boundary: sceNpGetServiceState output guest-memory validation failed")
            == std::string::npos) {
        std::cerr << invalid_np_service_state_boot.detail << '\n';
        return 148;
    }
    const auto duplicate_np_init_boot = run_guard_fixture(
        duplicate_np_init_install_zip, "milestone35-np-duplicate-init.zip");
    if (!duplicate_np_init_boot.returned || !duplicate_np_init_boot.np_initialized
        || duplicate_np_init_boot.np_init_call_count != 2
        || static_cast<std::uint32_t>(duplicate_np_init_boot.last_np_result)
            != np_error_already_initialized
        || duplicate_np_init_boot.return_value != np_error_already_initialized) {
        std::cerr << duplicate_np_init_boot.detail << '\n';
        return 149;
    }
    const auto np_lifecycle_boot = run_guard_fixture(
        np_lifecycle_install_zip, "milestone35-np-lifecycle.zip");
    if (!np_lifecycle_boot.returned || np_lifecycle_boot.hle_dispatch_count != 3
        || np_lifecycle_boot.last_hle_nid != np_term_nid
        || np_lifecycle_boot.np_initialized || np_lifecycle_boot.np_trophy_initialized
        || np_lifecycle_boot.np_init_call_count != 1
        || np_lifecycle_boot.np_service_state_call_count != 1
        || np_lifecycle_boot.np_term_call_count != 1
        || np_lifecycle_boot.last_np_service_state_address != synthetic_np_service_state
        || np_lifecycle_boot.last_np_service_state != 1
        || np_lifecycle_boot.last_np_result != 0 || np_lifecycle_boot.return_value != 0
        || np_lifecycle_boot.detail.find(
            "initialized=no, init calls=1, service-state calls=1, term calls=1")
            == std::string::npos) {
        std::cerr << np_lifecycle_boot.detail << '\n';
        return 150;
    }
    const auto uninitialized_trophy_term_boot = run_guard_fixture(
        uninitialized_trophy_term_install_zip,
        "milestone35-trophy-uninitialized-term.zip");
    if (!uninitialized_trophy_term_boot.returned
        || uninitialized_trophy_term_boot.np_trophy_initialized
        || uninitialized_trophy_term_boot.np_trophy_term_call_count != 1
        || static_cast<std::uint32_t>(
            uninitialized_trophy_term_boot.last_np_trophy_result)
            != np_trophy_error_not_initialized
        || uninitialized_trophy_term_boot.return_value
            != np_trophy_error_not_initialized) {
        std::cerr << uninitialized_trophy_term_boot.detail << '\n';
        return 151;
    }
    const auto duplicate_trophy_init_boot = run_guard_fixture(
        duplicate_trophy_init_install_zip, "milestone35-trophy-duplicate-init.zip");
    if (!duplicate_trophy_init_boot.returned
        || !duplicate_trophy_init_boot.np_trophy_initialized
        || duplicate_trophy_init_boot.np_trophy_init_call_count != 2
        || static_cast<std::uint32_t>(
            duplicate_trophy_init_boot.last_np_trophy_result)
            != np_trophy_error_already_initialized
        || duplicate_trophy_init_boot.return_value
            != np_trophy_error_already_initialized) {
        std::cerr << duplicate_trophy_init_boot.detail << '\n';
        return 152;
    }
    const auto trophy_lifecycle_boot = run_guard_fixture(
        trophy_lifecycle_install_zip, "milestone35-trophy-lifecycle.zip");
    if (!trophy_lifecycle_boot.returned || trophy_lifecycle_boot.hle_dispatch_count != 2
        || trophy_lifecycle_boot.last_hle_nid != np_trophy_term_nid
        || trophy_lifecycle_boot.np_trophy_initialized
        || trophy_lifecycle_boot.np_trophy_init_call_count != 1
        || trophy_lifecycle_boot.np_trophy_term_call_count != 1
        || trophy_lifecycle_boot.last_np_trophy_result != 0
        || trophy_lifecycle_boot.return_value != 0
        || trophy_lifecycle_boot.detail.find(
            "Trophy lifecycle: initialized=no, init calls=1, term calls=1")
            == std::string::npos) {
        std::cerr << trophy_lifecycle_boot.detail << '\n';
        return 153;
    }

    auto encrypted_self = compiler_baseline_self;
    write_value(encrypted_self, static_cast<std::size_t>(276 + 24),
        static_cast<std::uint64_t>(1));
    const auto installed_patch_eboot = test_root / "Vita3K/ux0/patch/M15TEST01/eboot.bin";
    std::ofstream encrypted_stream(installed_patch_eboot, std::ios::binary | std::ios::trunc);
    encrypted_stream.write(reinterpret_cast<const char *>(encrypted_self.data()),
        static_cast<std::streamsize>(encrypted_self.size()));
    encrypted_stream.close();
    const auto encrypted_preparation = vita3k::ios::prepare_installed_title("M15TEST01", true);
    if (!encrypted_preparation.selected || !encrypted_preparation.patch_selected || !encrypted_preparation.probe_valid || encrypted_preparation.self_segments_plain || encrypted_preparation.loaded || encrypted_preparation.encrypted_segment_count != 1 || encrypted_preparation.detail.find("Decryption must be integrated") == std::string::npos) {
        std::cerr << encrypted_preparation.detail << '\n';
        return 39;
    }

    if (!vita3k::ios::attach_host_display(1280, 720, display_error)) {
        std::cerr << display_error << '\n';
        return 12;
    }
    const auto bridge_frame = vita3k::ios::acquire_host_display_frame(
        1280, 720, display_error);
    if (!bridge_frame || !vita3k::ios::complete_host_display_frame(bridge_frame->identifier, true, {}, display_error)) {
        std::cerr << (display_error.empty() ? "The core display bridge did not complete a frame."
                                            : display_error)
                  << '\n';
        return 13;
    }
    const auto rendered_status = vita3k::ios::query_core_status();
    if (!rendered_status.renderer_attached || !rendered_status.renderer_frame_presented || rendered_status.renderer_submitted_frame_count != 1 || rendered_status.renderer_presented_frame_count != 1 || rendered_status.summary.find("Renderer: passed") == std::string::npos) {
        std::cerr << rendered_status.summary << '\n';
        return 14;
    }

    if (!vita3k::ios::attach_host_input_surface(1000.0, 500.0, input_error) || !vita3k::ios::submit_host_touch(9, 750.0, 250.0, vita3k::ios::HostTouchPhase::began, input_error)) {
        std::cerr << input_error << '\n';
        return 19;
    }
    vita3k::ios::set_host_controller_connected(true);
    if (!vita3k::ios::submit_host_controller(controller_sample, input_error)) {
        std::cerr << input_error << '\n';
        return 20;
    }
    const auto bridged_input_status = vita3k::ios::query_core_status();
    if (!bridged_input_status.input_surface_attached || !bridged_input_status.input_touch_received || !bridged_input_status.input_controller_connected || !bridged_input_status.input_controller_received || bridged_input_status.input_touch_sample_count != 1 || bridged_input_status.input_controller_sample_count != 1 || bridged_input_status.input_last_touch_x != 0.75 || bridged_input_status.input_last_touch_y != 0.5 || bridged_input_status.summary.find("Input: passed") == std::string::npos) {
        std::cerr << bridged_input_status.summary << '\n';
        return 21;
    }

    std::filesystem::remove_all(test_root, error);
    if (!emitted_fixture.empty()) {
        std::cout << "Emitted legal Milestone 9 fixture: " << emitted_fixture << '\n';
    }
    if (!emitted_sfo_fixture.empty()) {
        std::cout << "Emitted legal Milestone 13 SFO fixture: "
                  << emitted_sfo_fixture << '\n';
    }
    if (!emitted_vpk_fixture.empty()) {
        std::cout << "Emitted legal Milestone 14 VPK fixture: "
                  << emitted_vpk_fixture << '\n';
    }
    if (!emitted_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 15 installation fixture: "
                  << emitted_install_zip_fixture << '\n';
    }
    if (!emitted_self_fixture.empty()) {
        std::cout << "Emitted legal Milestone 16 plain SELF fixture: "
                  << emitted_self_fixture << '\n';
    }
    if (!emitted_m16_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 16 installation fixture: "
                  << emitted_m16_install_zip_fixture << '\n';
    }
    if (!emitted_m18_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 18 lifecycle-export fixture: "
                  << emitted_m18_install_zip_fixture << '\n';
    }
    if (!emitted_m19_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 19 Thumb-2 PUSH.W fixture: "
                  << emitted_m19_install_zip_fixture << '\n';
    }
    if (!emitted_m20_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 20 Thumb compiler baseline fixture: "
                  << emitted_m20_install_zip_fixture << '\n';
    }
    if (!emitted_m21_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 21 Thumb-2 compiler batch fixture: "
                  << emitted_m21_install_zip_fixture << '\n';
    }
    if (!emitted_m22_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 22 inline HLE diagnostic fixture: "
                  << emitted_m22_install_zip_fixture << '\n';
    }
    if (!emitted_m23_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 23 libc runtime fixture: "
                  << emitted_m23_install_zip_fixture << '\n';
    }
    if (!emitted_m24_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 24 Thumb-2 runtime-family fixture: "
                  << emitted_m24_install_zip_fixture << '\n';
    }
    if (!emitted_m25_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 25 Thumb-2 register-family fixture: "
                  << emitted_m25_install_zip_fixture << '\n';
    }
    if (!emitted_m26_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 26 libc termination fixture: "
                  << emitted_m26_install_zip_fixture << '\n';
    }
    if (!emitted_m27_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 27 Thumb-2 multiple-transfer fixture: "
                  << emitted_m27_install_zip_fixture << '\n';
    }
    if (!emitted_m28_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 28 C++ static-initialization guard fixture: "
                  << emitted_m28_install_zip_fixture << '\n';
    }
    if (!emitted_m29_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 29 libc heap fixture: "
                  << emitted_m29_install_zip_fixture << '\n';
    }
    if (!emitted_m30_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 30 LR shifted-register fixture: "
                  << emitted_m30_install_zip_fixture << '\n';
    }
    if (!emitted_m31_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 31 execution-progress fixture: "
                  << emitted_m31_install_zip_fixture << '\n';
    }
    if (!emitted_m32_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 32 C++ allocation fixture: "
                  << emitted_m32_install_zip_fixture << '\n';
    }
    if (!emitted_m33_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 33 AppUtil lifecycle fixture: "
                  << emitted_m33_install_zip_fixture << '\n';
    }
    if (!emitted_m34_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 34 Sysmodule lifecycle fixture: "
                  << emitted_m34_install_zip_fixture << '\n';
    }
    if (!emitted_m35_install_zip_fixture.empty()) {
        std::cout << "Emitted legal Milestone 35 NP lifecycle fixture: "
                  << emitted_m35_install_zip_fixture << '\n';
    }
    std::cout << rescanned.summary << '\n';
    return 0;
}
