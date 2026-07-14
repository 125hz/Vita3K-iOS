#include <vita3k_ios/VitaAppArchive.h>

#include <vita3k_ios/VitaAppMetadata.h>

#include <miniz.h>

#include <array>
#include <string_view>

namespace vita3k::ios {

std::vector<std::uint8_t> make_synthetic_vpk(bool unsafe_path) {
    const auto sfo = make_synthetic_param_sfo(
        "M14TEST01", "Vita3K iOS archive inspection probe");
    constexpr std::array<std::uint8_t, 16> diagnostic_eboot{
        0x4D, 0x31, 0x34, 0x20, 0x44, 0x49, 0x41, 0x47,
        0x4E, 0x4F, 0x53, 0x54, 0x49, 0x43, 0x00, 0x00
    };
    constexpr std::string_view notice =
        "Legal Vita3K iOS Milestone 14 package-inspection fixture.\n";

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0))
        return {};

    const auto add = [&zip](const char *name, const void *data, std::size_t size) {
        return mz_zip_writer_add_mem(&zip, name, data, size, MZ_BEST_SPEED) == MZ_TRUE;
    };
    const char *notice_path = unsafe_path ? "../escape.txt" : "assets/readme.txt";
    if (!add("sce_sys/param.sfo", sfo.data(), sfo.size()) ||
        !add("eboot.bin", diagnostic_eboot.data(), diagnostic_eboot.size()) ||
        !add(notice_path, notice.data(), notice.size())) {
        mz_zip_writer_end(&zip);
        return {};
    }

    void *archive_data = nullptr;
    std::size_t archive_size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &archive_data, &archive_size)) {
        mz_zip_writer_end(&zip);
        return {};
    }
    std::vector<std::uint8_t> result(
        static_cast<const std::uint8_t *>(archive_data),
        static_cast<const std::uint8_t *>(archive_data) + archive_size);
    mz_free(archive_data);
    mz_zip_writer_end(&zip);
    return result;
}

std::vector<std::uint8_t> make_synthetic_install_zip() {
    const auto app_sfo = make_synthetic_param_sfo(
        "M15TEST01", "Vita3K iOS transactional base app", "gd");
    const auto patch_sfo = make_synthetic_param_sfo(
        "M15TEST01", "Vita3K iOS transactional patch", "gp");
    constexpr std::string_view app_eboot = "M15 synthetic base eboot";
    constexpr std::string_view patch_eboot = "M15 synthetic patch eboot";
    constexpr std::string_view base_asset = "base asset";
    constexpr std::string_view patch_asset = "patch asset";

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0))
        return {};
    const auto add = [&zip](const char *name, const void *data, std::size_t size) {
        return mz_zip_writer_add_mem(&zip, name, data, size, MZ_BEST_SPEED) == MZ_TRUE;
    };
    if (!add("app/M15TEST01/sce_sys/param.sfo", app_sfo.data(), app_sfo.size()) ||
        !add("app/M15TEST01/eboot.bin", app_eboot.data(), app_eboot.size()) ||
        !add("app/M15TEST01/assets/base.dat", base_asset.data(), base_asset.size()) ||
        !add("patch/M15TEST01/sce_sys/param.sfo", patch_sfo.data(), patch_sfo.size()) ||
        !add("patch/M15TEST01/eboot.bin", patch_eboot.data(), patch_eboot.size()) ||
        !add("patch/M15TEST01/assets/patch.dat", patch_asset.data(), patch_asset.size())) {
        mz_zip_writer_end(&zip);
        return {};
    }
    void *archive_data = nullptr;
    std::size_t archive_size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &archive_data, &archive_size)) {
        mz_zip_writer_end(&zip);
        return {};
    }
    std::vector<std::uint8_t> result(
        static_cast<const std::uint8_t *>(archive_data),
        static_cast<const std::uint8_t *>(archive_data) + archive_size);
    mz_free(archive_data);
    mz_zip_writer_end(&zip);
    return result;
}

} // namespace vita3k::ios
