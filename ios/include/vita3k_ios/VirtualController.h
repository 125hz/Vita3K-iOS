// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// iOS-only bridge for the native touch controller shown above SDL/Vulkan.

#pragma once

// Creates an SDL virtual gamepad. Call after SDL_INIT_GAMEPAD succeeds.
bool vita3k_ios_attach_virtual_controller();

// Shows/hides the transparent UIKit overlay. Touches outside its buttons pass
// through to SDL, preserving Vita front-touchscreen input.
void vita3k_ios_show_virtual_controller();
void vita3k_ios_hide_virtual_controller();

// Native controller customization, shared by the library settings screen and
// the floating in-game menu.
void vita3k_ios_present_controller_options();

// Drag-to-reposition editor for the on-screen controls. Spins up a preview
// overlay when no game is running, which finish tears back down.
void vita3k_ios_begin_layout_editing();
void vita3k_ios_finish_layout_editing();

// The overlay reports the window's top safe-area inset here; the core reads it
// back through vita3k_ios_safe_area_top_pixels to letterbox the guest image.
void vita3k_ios_report_safe_area_top_pixels(float pixels);

// SDL virtual-joystick writes for the SwiftUI on-screen controls. `button` and
// `axis` are SDL_GamepadButton / SDL_GamepadAxis raw values. No-ops when the
// virtual joystick has not been attached.
void vita3k_ios_virtual_pad_set_button(int button, bool pressed);
void vita3k_ios_virtual_pad_set_axis(int axis, short value);
void vita3k_ios_virtual_pad_release_all();

// Vita front-touchscreen passthrough. The dynamic joystick turns this off: the
// on-screen controller then owns every touch, and a finger that still reached
// SDL would land on the guest's touch panel as a contact the player never
// intended. Read from the iOS event loop, written from the main thread.
void vita3k_ios_set_vita_touchscreen_enabled(bool enabled);
bool vita3k_ios_vita_touchscreen_enabled();

// Re-opens the floating in-game menu (no-op when no game overlay is active).
// Used by sub-screens (controller options, trophies, performance HUD) so
// their Back action returns to the menu instead of dropping to the game.
void vita3k_ios_present_game_menu();

// Notifies the controller layer that a sub-screen opened from the in-game
// menu was dismissed; re-presents the menu when appropriate.
void vita3k_ios_submenu_dismissed();

// Physical-pad state excludes Vita3K's own SDL virtual joystick. When enabled
// in ios_controls.json, only the touch controls auto-hide; the menu stays.
void vita3k_ios_set_physical_controller_connected(bool connected);

// Updated by the UIKit overlay during layout and safe to read from the
// Vulkan render thread. The value is in drawable pixels.
float vita3k_ios_safe_area_top_pixels();

// Releases all buttons and removes the SDL virtual gamepad.
void vita3k_ios_detach_virtual_controller();
