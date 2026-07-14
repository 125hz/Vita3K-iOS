#include <vita3k_ios/CoreBridge.h>
#include <vita3k_ios/HostDisplay.h>
#include <vita3k_ios/HostInput.h>
#include <vita3k_ios/VitaAppMetadata.h>
#include <vita3k_ios/VitaAppArchive.h>

#include <packages/archive.h>

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
    if (!display_frame || display_frame->identifier == 0 ||
        display_frame->width != 1920 || display_frame->height != 1080 ||
        display.acquire_frame(1920, 1080, display_error)) {
        std::cerr << "The display did not enforce a single diagnostic frame in flight.\n";
        return 9;
    }
    if (!display.complete_frame(display_frame->identifier, true, {}, display_error)) {
        std::cerr << display_error << '\n';
        return 10;
    }
    const auto display_status = display.status();
    if (!display_status.attached || display_status.frame_in_flight ||
        !display_status.first_frame_presented ||
        display_status.submitted_frame_count != 1 ||
        display_status.presented_frame_count != 1 ||
        display.acquire_frame(1920, 1080, display_error)) {
        std::cerr << "The display did not retain its first-presented-frame state.\n";
        return 11;
    }

    vita3k::ios::HostInput input;
    std::string input_error;
    if (input.attach_touch_surface(0.0, 1080.0, input_error)) {
        std::cerr << "The input adapter accepted a zero-width touch surface.\n";
        return 15;
    }
    if (!input.attach_touch_surface(1000.0, 500.0, input_error) ||
        !input.submit_touch(7, 250.0, 125.0, vita3k::ios::HostTouchPhase::began,
            input_error) ||
        !input.submit_touch(7, 250.0, 125.0, vita3k::ios::HostTouchPhase::ended,
            input_error)) {
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
    if (!input_status.touch_surface_attached || input_status.touch_active ||
        !input_status.touch_sample_received || !input_status.controller_connected ||
        !input_status.controller_sample_received || input_status.touch_sample_count != 2 ||
        input_status.controller_sample_count != 1 || input_status.last_touch_x != 0.25 ||
        input_status.last_touch_y != 0.25 || input_status.controller.right_x != 1.0f ||
        input_status.controller.right_y != -1.0f ||
        input_status.controller.buttons != controller_sample.buttons) {
        std::cerr << "The portable input adapter produced unexpected state.\n";
        return 18;
    }

    const auto sfo_fixture = vita3k::ios::make_synthetic_param_sfo(
        "M13TEST01", "Vita3K iOS upstream metadata probe");
    const auto parsed_sfo = vita3k::ios::parse_vita_app_metadata(sfo_fixture);
    if (!parsed_sfo.parsed || parsed_sfo.title_id != "M13TEST01" ||
        parsed_sfo.title != "Vita3K iOS upstream metadata probe" ||
        parsed_sfo.category != "gd" || parsed_sfo.app_version != "01.00") {
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
    if (!parsed_vpk.valid || parsed_vpk.file_count != 3 ||
        parsed_vpk.applications.size() != 1 ||
        parsed_vpk.applications.front().title_id != "M14TEST01" ||
        parsed_vpk.applications.front().install_target != "ux0/app/M14TEST01" ||
        unsafe_vpk.valid || unsafe_vpk.unsafe_path_count != 1) {
        std::cerr << "The bounded Vita package inspection diagnostic failed.\n";
        return 27;
    }
    const auto install_zip_fixture = vita3k::ios::make_synthetic_install_zip();
    const auto parsed_install_zip = packages::inspect_archive(install_zip_fixture);
    if (!parsed_install_zip.valid || parsed_install_zip.file_count != 6 ||
        parsed_install_zip.applications.size() != 2 ||
        parsed_install_zip.applications[0].title_id != "M15TEST01" ||
        parsed_install_zip.applications[0].install_target != "ux0/app/M15TEST01" ||
        parsed_install_zip.applications[1].install_target != "ux0/patch/M15TEST01") {
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
                         "[--verify-vitasdk <path>]\n";
            return 64;
        }
    }
    const auto test_root = std::filesystem::temp_directory_path() / "vita3k-ios-core-smoke-test";
    std::error_code error;
    std::filesystem::remove_all(test_root, error);

    const auto status = vita3k::ios::initialize_core(test_root);
    if (!status.linked || !status.self_tests_passed || !status.upstream_metadata_ready ||
        !status.upstream_archive_ready ||
        !status.package_installer_ready ||
        !status.storage_ready ||
        !status.guest_memory_ready || !status.segment_mapping_ready ||
        !status.loader_pipeline_ready || !status.import_binding_ready ||
        !status.arm_execution_ready ||
        !status.guest_thread_ready ||
        status.renderer_attached || status.renderer_frame_presented ||
        status.summary.find("Renderer: waiting for MTKView host") == std::string::npos ||
        status.input_surface_attached || status.input_touch_received ||
        status.summary.find("Input: waiting for UIKit touch surface") == std::string::npos ||
        status.summary.find("Upstream app metadata: passed") == std::string::npos ||
        status.arm_test_instruction_count != 7 || status.hle_test_dispatch_count != 1 ||
        status.thread_test_instruction_count != 12 ||
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
        !rescanned.imported_artifacts.front().module_start_from_export ||
        !rescanned.imported_artifacts.front().execution_attempted ||
        !rescanned.imported_artifacts.front().thread_exited ||
        rescanned.imported_artifacts.front().relocation_entry_count != 1 ||
        rescanned.imported_artifacts.front().relocation_patch_count != 1 ||
        rescanned.imported_artifacts.front().export_library_count != 1 ||
        rescanned.imported_artifacts.front().import_library_count != 1 ||
        rescanned.imported_artifacts.front().exported_nid_count != 1 ||
        rescanned.imported_artifacts.front().imported_nid_count != 2 ||
        rescanned.imported_artifacts.front().bound_import_stub_count != 2 ||
        rescanned.imported_artifacts.front().module_start_address != 0x81000061 ||
        rescanned.imported_artifacts.front().executed_instruction_count != 12 ||
        rescanned.imported_artifacts.front().hle_dispatch_count != 2 ||
        rescanned.imported_artifacts.front().thread_exit_status != 42 ||
        rescanned.imported_artifacts.front().module_name != "synthetic-homebrew" ||
        rescanned.imported_artifacts.front().module_nid != 0x1234ABCD) {
        std::cerr << rescanned.summary << '\n';
        return 3;
    }

    if (!vitasdk_fixture.empty()) {
        std::filesystem::remove(fixture, error);
        const auto real_fixture = test_root / "Vita3K" / "imports" /
            "milestone10-vitasdk-homebrew.velf";
        std::filesystem::copy_file(vitasdk_fixture, real_fixture,
            std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            std::cerr << "Could not stage the real VitaSDK fixture: " << error.message() << '\n';
            return 5;
        }
        const auto real_status = vita3k::ios::rescan_imports();
        if (real_status.imported_artifacts.size() != 1 ||
            real_status.imported_artifacts.front().kind != "Vita ELF" ||
            !real_status.imported_artifacts.front().structurally_valid ||
            !real_status.imported_artifacts.front().load_attempted ||
            !real_status.imported_artifacts.front().loaded ||
            !real_status.imported_artifacts.front().module_info_valid ||
            !real_status.imported_artifacts.front().relocations_applied ||
            !real_status.imported_artifacts.front().module_tables_parsed ||
            !real_status.imported_artifacts.front().import_stubs_bound ||
            !real_status.imported_artifacts.front().module_start_valid ||
            !real_status.imported_artifacts.front().execution_attempted ||
            real_status.imported_artifacts.front().thread_exited ||
            !real_status.imported_artifacts.front().thread_returned ||
            real_status.imported_artifacts.front().module_name != "m10_homebrew" ||
            real_status.imported_artifacts.front().module_start_address != 0x81000001 ||
            real_status.imported_artifacts.front().executed_instruction_count != 6 ||
            real_status.imported_artifacts.front().hle_dispatch_count != 1 ||
            real_status.imported_artifacts.front().thread_return_value != 1) {
            std::cerr << real_status.summary << '\n';
            return 6;
        }
        std::cout << "Verified real VitaSDK diagnostic:\n" << real_status.summary << '\n';
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
    if (sfo_status.imported_artifacts.size() != 1 ||
        sfo_status.imported_artifacts.front().kind != "PARAM.SFO" ||
        !sfo_status.imported_artifacts.front().app_metadata_parsed ||
        sfo_status.imported_artifacts.front().app_title_id != "M13TEST01" ||
        sfo_status.imported_artifacts.front().app_title !=
            "Vita3K iOS upstream metadata probe" ||
        sfo_status.imported_artifacts.front().app_category != "gd" ||
        sfo_status.imported_artifacts.front().app_version != "01.00" ||
        sfo_status.summary.find("title ID M13TEST01") == std::string::npos) {
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
    if (vpk_status.imported_artifacts.size() != 1 ||
        vpk_status.imported_artifacts.front().kind != "VPK/ZIP" ||
        !vpk_status.imported_artifacts.front().archive_inspected ||
        !vpk_status.imported_artifacts.front().archive_valid ||
        vpk_status.imported_artifacts.front().archive_file_count != 3 ||
        vpk_status.imported_artifacts.front().archive_application_count != 1 ||
        vpk_status.imported_artifacts.front().archive_unsafe_path_count != 0 ||
        vpk_status.imported_artifacts.front().app_title_id != "M14TEST01" ||
        vpk_status.imported_artifacts.front().archive_install_target !=
            "ux0/app/M14TEST01" ||
        vpk_status.summary.find("planned target ux0/app/M14TEST01") == std::string::npos) {
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
    if (!install_result.success || install_result.application_count != 2 ||
        install_result.file_count != 6 || install_result.installed_targets.size() != 2 ||
        std::filesystem::exists(previous_app) ||
        !std::filesystem::is_regular_file(
            test_root / "Vita3K/ux0/app/M15TEST01/eboot.bin") ||
        !std::filesystem::is_regular_file(
            test_root / "Vita3K/ux0/patch/M15TEST01/assets/patch.dat") ||
        installed_status.installed_titles.size() != 1 ||
        installed_status.installed_titles.front().title_id != "M15TEST01" ||
        !installed_status.installed_titles.front().patch_installed ||
        installed_status.summary.find("Installed titles: 1") == std::string::npos ||
        installed_status.summary.find("patch installed") == std::string::npos) {
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
    if (unsafe_install.success ||
        !std::filesystem::is_regular_file(
            test_root / "Vita3K/ux0/app/M15TEST01/eboot.bin")) {
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
    if (!m16_install.success || m16_install.file_count != 6 ||
        m16_installed_status.installed_titles.size() != 1 ||
        !m16_installed_status.installed_titles.front().base_eboot_present ||
        !m16_installed_status.installed_titles.front().patch_eboot_present) {
        std::cerr << m16_installed_status.summary << '\n';
        return 37;
    }
    const auto prepared = vita3k::ios::prepare_installed_title("M15TEST01", true);
    const auto prepared_status = vita3k::ios::query_core_status();
    if (!prepared.selected || !prepared.patch_selected || !prepared.probe_valid ||
        !prepared.self_segments_plain || !prepared.loaded ||
        prepared.kind != "Vita SELF" || prepared.load_segment_count != 2 ||
        prepared.module_name != "synthetic-homebrew" ||
        prepared.module_start_address != 0x81000061 ||
        prepared.imported_nid_count != 2 || prepared.bound_import_stub_count != 2 ||
        prepared_status.selected_title_id != "M15TEST01" ||
        !prepared_status.selected_executable_loaded ||
        !prepared_status.selected_boot_available ||
        prepared_status.summary.find("Boot source: patch/eboot.bin") == std::string::npos ||
        prepared_status.summary.find("Executable preparation: Mapped 2") ==
            std::string::npos) {
        std::cerr << prepared.detail << '\n' << prepared_status.summary << '\n';
        return 38;
    }

    const auto controlled_boot = vita3k::ios::attempt_prepared_title_boot(256);
    const auto controlled_boot_status = vita3k::ios::query_core_status();
    if (!controlled_boot.attempted || !controlled_boot.started ||
        !controlled_boot.exited || controlled_boot.returned ||
        controlled_boot.instruction_count != 12 ||
        controlled_boot.hle_dispatch_count != 2 ||
        controlled_boot.exit_status != 42 ||
        controlled_boot_status.selected_boot_available ||
        !controlled_boot_status.selected_boot_attempted ||
        controlled_boot_status.summary.find(
            "Controlled boot attempt: interpreter-only, 256-instruction ceiling") ==
            std::string::npos) {
        std::cerr << controlled_boot.detail << '\n' << controlled_boot_status.summary << '\n';
        return 40;
    }
    const auto repeated_boot = vita3k::ios::attempt_prepared_title_boot(256);
    if (repeated_boot.started || repeated_boot.detail.find("Select the title again") ==
            std::string::npos) {
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
    const auto lifecycle_prepared =
        vita3k::ios::prepare_installed_title("M15TEST01", true);
    const auto lifecycle_prepared_status = vita3k::ios::query_core_status();
    if (!lifecycle_prepared.selected || !lifecycle_prepared.loaded ||
        !lifecycle_prepared.module_start_from_export ||
        lifecycle_prepared.module_start_address != 0x81000061 ||
        lifecycle_prepared.detail.find("via lifecycle export") == std::string::npos ||
        !lifecycle_prepared_status.selected_boot_available) {
        std::cerr << lifecycle_prepared.detail << '\n'
                  << lifecycle_prepared_status.summary << '\n';
        return 45;
    }
    const auto lifecycle_boot = vita3k::ios::attempt_prepared_title_boot(256);
    if (!lifecycle_boot.started || !lifecycle_boot.exited || lifecycle_boot.returned ||
        lifecycle_boot.instruction_count != 12 ||
        lifecycle_boot.hle_dispatch_count != 2 || lifecycle_boot.exit_status != 42) {
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
    const auto wide_push_prepared =
        vita3k::ios::prepare_installed_title("M15TEST01", true);
    if (!wide_push_prepared.selected || !wide_push_prepared.loaded ||
        !wide_push_prepared.module_start_from_export ||
        wide_push_prepared.module_start_address != 0x81000061) {
        std::cerr << wide_push_prepared.detail << '\n';
        return 50;
    }
    const auto wide_push_boot = vita3k::ios::attempt_prepared_title_boot(256);
    if (!wide_push_boot.started || !wide_push_boot.exited || wide_push_boot.returned ||
        wide_push_boot.instruction_count != 12 ||
        wide_push_boot.hle_dispatch_count != 2 || wide_push_boot.exit_status != 42) {
        std::cerr << wide_push_boot.detail << '\n';
        return 51;
    }

    auto encrypted_self = wide_push_self;
    write_value(encrypted_self, static_cast<std::size_t>(276 + 24),
        static_cast<std::uint64_t>(1));
    const auto installed_patch_eboot =
        test_root / "Vita3K/ux0/patch/M15TEST01/eboot.bin";
    std::ofstream encrypted_stream(installed_patch_eboot, std::ios::binary | std::ios::trunc);
    encrypted_stream.write(reinterpret_cast<const char *>(encrypted_self.data()),
        static_cast<std::streamsize>(encrypted_self.size()));
    encrypted_stream.close();
    const auto encrypted_preparation =
        vita3k::ios::prepare_installed_title("M15TEST01", true);
    if (!encrypted_preparation.selected || !encrypted_preparation.patch_selected ||
        !encrypted_preparation.probe_valid || encrypted_preparation.self_segments_plain ||
        encrypted_preparation.loaded || encrypted_preparation.encrypted_segment_count != 1 ||
        encrypted_preparation.detail.find("Decryption must be integrated") ==
            std::string::npos) {
        std::cerr << encrypted_preparation.detail << '\n';
        return 39;
    }

    if (!vita3k::ios::attach_host_display(1280, 720, display_error)) {
        std::cerr << display_error << '\n';
        return 12;
    }
    const auto bridge_frame = vita3k::ios::acquire_host_display_frame(
        1280, 720, display_error);
    if (!bridge_frame || !vita3k::ios::complete_host_display_frame(
            bridge_frame->identifier, true, {}, display_error)) {
        std::cerr << (display_error.empty() ? "The core display bridge did not complete a frame."
                                           : display_error) << '\n';
        return 13;
    }
    const auto rendered_status = vita3k::ios::query_core_status();
    if (!rendered_status.renderer_attached || !rendered_status.renderer_frame_presented ||
        rendered_status.renderer_submitted_frame_count != 1 ||
        rendered_status.renderer_presented_frame_count != 1 ||
        rendered_status.summary.find("Renderer: passed") == std::string::npos) {
        std::cerr << rendered_status.summary << '\n';
        return 14;
    }

    if (!vita3k::ios::attach_host_input_surface(1000.0, 500.0, input_error) ||
        !vita3k::ios::submit_host_touch(9, 750.0, 250.0,
            vita3k::ios::HostTouchPhase::began, input_error)) {
        std::cerr << input_error << '\n';
        return 19;
    }
    vita3k::ios::set_host_controller_connected(true);
    if (!vita3k::ios::submit_host_controller(controller_sample, input_error)) {
        std::cerr << input_error << '\n';
        return 20;
    }
    const auto bridged_input_status = vita3k::ios::query_core_status();
    if (!bridged_input_status.input_surface_attached ||
        !bridged_input_status.input_touch_received ||
        !bridged_input_status.input_controller_connected ||
        !bridged_input_status.input_controller_received ||
        bridged_input_status.input_touch_sample_count != 1 ||
        bridged_input_status.input_controller_sample_count != 1 ||
        bridged_input_status.input_last_touch_x != 0.75 ||
        bridged_input_status.input_last_touch_y != 0.5 ||
        bridged_input_status.summary.find("Input: passed") == std::string::npos) {
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
    std::cout << rescanned.summary << '\n';
    return 0;
}
