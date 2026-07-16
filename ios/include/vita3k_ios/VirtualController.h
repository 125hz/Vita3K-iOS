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

// Releases all buttons and removes the SDL virtual gamepad.
void vita3k_ios_detach_virtual_controller();
