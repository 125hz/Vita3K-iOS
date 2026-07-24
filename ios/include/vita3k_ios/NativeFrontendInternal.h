// Internal seam between NativeFrontend.mm (which owns the frontend action
// queue and the per-title settings store) and TsubomiBridge.mm (which turns
// those into the Objective-C facade the SwiftUI layer talks to).
//
// Not part of the frontend's public API in NativeFrontend.h: nothing outside
// the iOS UI layer should be queueing actions or reading per-title defaults
// directly.

#pragma once

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include <optional>
#include <string>
#include <vector>

#include "vita3k_ios/NativeFrontend.h"

namespace vita3k_ios_internal {

// Hands an action to the emulator thread, which drains it through
// vita3k_ios_take_frontend_action(). Only one action is pending at a time -
// queueing a second one before the core picks the first up replaces it, which
// matches how the UI is driven (one user intent at a time).
void queue_frontend_action(Vita3KIOSFrontendAction action);

// The global settings the core last reported, as shown in the library header.
Vita3KIOSSettings current_global_settings();

// Per-title overrides. Any field the title has never overridden comes from
// `fallback`, so a title saved before a setting existed adopts the global one.
Vita3KIOSSettings load_title_settings(NSString *title_id, const Vita3KIOSSettings &fallback);
void store_title_settings(NSString *title_id, const Vita3KIOSSettings &settings);
void clear_title_settings(NSString *title_id);

// Shared cover/trophy art cache. Deliberately the same cache the remaining
// UIKit screens use: two caches would double the memory held against the
// emulator's guest allocations and evict independently.
//
// `ready` runs on the main thread and only when the image was NOT already
// resident; a resident image is returned directly.
UIImage *cached_art(NSString *path, void (^ready)(UIImage *));
void invalidate_cached_art(NSString *path);

// Formats a trophy collection for display. Returns TsubomiTrophyCollection *,
// declared as id here so this header stays includable from plain C++ TUs.
id bridge_trophies(const Vita3KIOSTrophyCollection &collection);

// Presents the system document picker for a firmware .PUP.
void present_firmware_picker();

// Re-reads the library display toggles and redraws the visible cells.
void reload_library();

// Per-title frontend state that lives in NSUserDefaults rather than in the
// core's game entry.
NSString *display_title_for(NSString *title_id, NSString *original);
void set_display_title(NSString *title_id, NSString *title);
bool title_has_settings(NSString *title_id);

// Hex dump used when a package's title is not valid UTF-8.
std::string hex_bytes_for_log(const std::string &value);

// The games the core last reported, and a lookup by title id. Returns nullopt
// when the title is not installed (a stale row the user tapped mid-refresh).
std::vector<Vita3KIOSGameEntry> current_games();
std::optional<Vita3KIOSGameEntry> game_for_title(NSString *title_id);

// Bridged library entries, in the order the core reported them. Returns
// NSArray<TsubomiGameEntry *> *, declared as id to keep this header C++-safe.
id bridge_games();

// Bridged copy of the core's last settings snapshot. Returns
// TsubomiSettings *, declared as id to keep this header C++-safe.
id bridge_settings();

// Document pickers owned by NativeFrontend.mm.
void present_game_picker();
void present_license_import_picker();
void present_save_import_picker(NSString *title_id);

// The library's firmware gate: presents an explanatory alert and returns false
// when the three official packages are not all installed.
bool firmware_ready_or_alert();

// The graphics-help explainer from the library header.
void show_graphics_help();

// Explains that JIT must be attached before a game can boot.
void show_jit_required_alert();

// Presents a settings sheet over the library. `title_id` empty means global.
void present_settings_sheet(NSString *title_id, NSString *display_name);

} // namespace vita3k_ios_internal
