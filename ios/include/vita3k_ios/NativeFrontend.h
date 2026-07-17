// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// Native UIKit frontend layered above SDL's iOS window.

#pragma once

#include <optional>
#include <string>
#include <vector>

struct Vita3KIOSGameEntry {
    std::string title;
    std::string title_id;
    std::string category;
    std::string app_path;
    std::string icon_path;
};

struct Vita3KIOSSettings {
    float resolution_multiplier = 1.0f;
    bool v_sync = true;
    bool fps_hack = false;
    bool cpu_opt = true;
    bool ngs_enable = true;
    bool async_pipeline_compilation = true;
    int anisotropic_filtering = 1;
    // Display-only: installed firmware version shown on the library header.
    std::string firmware_version;
};

enum class Vita3KIOSFrontendActionKind {
    Launch,
    Refresh,
    ApplySettings,
    // app_path carries the local file path of the picked archive/PUP.
    ImportGame,
    ImportFirmware,
    Quit,
};

struct Vita3KIOSFrontendAction {
    Vita3KIOSFrontendActionKind kind = Vita3KIOSFrontendActionKind::Quit;
    std::string app_path;
    Vita3KIOSSettings settings;
};

void vita3k_ios_show_library(const std::vector<Vita3KIOSGameEntry> &games,
    const Vita3KIOSSettings &settings);
void vita3k_ios_update_library(const std::vector<Vita3KIOSGameEntry> &games,
    const Vita3KIOSSettings &settings);
void vita3k_ios_hide_library();
std::optional<Vita3KIOSFrontendAction> vita3k_ios_take_frontend_action();
void vita3k_ios_report_settings_result(const std::vector<std::string> &restart_required);

// In-game performance HUD. Called ~once per second from the frontend loop
// with the guest frame rate; battery/RAM are sampled on the UIKit side. The
// HUD only appears when the user enabled at least one metric in settings.
void vita3k_ios_update_perf_overlay(float guest_fps);
void vita3k_ios_hide_perf_overlay();

// Dismisses the import-in-progress overlay and shows the outcome message.
void vita3k_ios_report_import_result(const std::string &message);
