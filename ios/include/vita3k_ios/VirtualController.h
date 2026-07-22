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

// SDL virtual-joystick writes for the SwiftUI on-screen controls. `button` and
// `axis` are SDL_GamepadButton / SDL_GamepadAxis raw values. No-ops when the
// virtual joystick has not been attached.
void vita3k_ios_virtual_pad_set_button(int button, bool pressed);
void vita3k_ios_virtual_pad_set_axis(int axis, short value);
void vita3k_ios_virtual_pad_release_all();

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
