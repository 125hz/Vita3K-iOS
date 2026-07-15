# Amagami first-menu checklist

This checklist tracks the shortest honest path to a first guest-produced Amagami menu frame. A blue host clear, a synthetic fixture, or a title that only reaches `module_start` does not satisfy the goal.

## Foundation already working

- [x] Build and package an unsigned arm64 iPhoneOS IPA in CI.
- [x] Install app and patch archives transactionally and select `patch/eboot.bin`.
- [x] Map the title's SELF segments, apply relocations, parse imports/exports, and resolve the lifecycle `module_start`.
- [x] Rebind imported function stubs and cross ARM/Thumb import trampolines.
- [x] Execute 5676 real Amagami instructions and 31 HLE calls before an honest unbound service import.
- [x] Use the 65536-instruction telemetry run to prove startup is still advancing rather than trapped in a hot loop.
- [x] Provide startup libc state for DSO handling, termination registration, C++ guards, and a writable guest heap.
- [x] Provide validated, stateful AppUtil initialization/shutdown lifecycle calls.
- [x] Provide public Sysmodule load/status/unload bookkeeping for reached HLE modules.
- [x] Attach a Metal view and present host-owned diagnostic frames.

## Must happen before the title can reach its menu logic

- [ ] **CPU coverage:** continue implementing only the real instruction boundaries reached after the telemetry run proved startup is advancing. Remaining likely families include more Thumb-2/ARM data processing, IT blocks, multiplication/division, atomics, VFP/NEON, exceptions, and complete flag/interworking behavior.
- [ ] **Kernel execution model:** replace the single bounded startup thread with guest processes, multiple threads, scheduling, waits, mutexes/semaphores, callbacks, timers, and TLS.
- [ ] **HLE coverage:** implement every imported kernel/libc/service NID actually reached. Unknown NIDs must remain hard diagnostic boundaries rather than synthetic successes.
- [ ] **Guest virtual filesystem:** mount app, patch, savedata, and system-device paths and connect guest `SceIo` calls so the title can read its scripts, textures, configuration, and other menu assets.
- [ ] **Process memory:** integrate a process-wide allocator/page service, stack/TLS growth, and thread-safe allocation behavior beyond the current bounded startup arena.
- [ ] **Required system services:** supply the title's reached time, controller, display, app-manager, power, font, savedata, and common-dialog calls with upstream-compatible state.

## Must happen before a menu can be visible on the iPhone

- [ ] **GXM command path:** create guest graphics contexts, render targets, surfaces, memory maps, command lists, and synchronization objects.
- [ ] **Shader and draw translation:** connect Vita GXM shaders, textures, buffers, blending, depth/stencil, and draw calls to an iOS-capable renderer such as the upstream Vulkan path through MoltenVK or a validated Metal backend.
- [ ] **Guest frame presentation:** route the title's display queue/framebuffer to the existing `MTKView`. The current blue clear is host diagnostic output only.
- [ ] **Font and asset rendering:** load the font/system data and game assets required by the title screen without committing proprietary firmware or game content.

## Needed immediately after the first visible menu

- [ ] Route UIKit/GameController samples into Vita controller/touch HLE so the menu can be operated.
- [ ] Connect Vita audio/NGS calls to an iOS audio backend.
- [ ] Add save-data persistence, suspend/resume, error dialogs, and longer-run stability.
- [ ] Replace the interpreter-only diagnostic path with the validated production scheduler/JIT path where iOS policy and entitlements allow it.

## First-menu acceptance test

All of the following must be true:

- [ ] Amagami reaches a stable event/main loop without an unsupported CPU, memory, or HLE boundary.
- [ ] At least one frame is generated from guest GXM state and presented by the iOS host.
- [ ] The frame contains recognizable Amagami title/menu assets rather than a host clear or synthetic test image.
- [ ] The same clean install reproduces the frame without patching guest return values or skipping unknown instructions.

The remaining work cannot be converted into a trustworthy fixed milestone count yet. Each physical-device boundary reveals the next CPU or service dependency, while the graphics and scheduler integrations are multi-milestone subsystems. The current critical path is: finish startup execution, establish real threads/VFS services, initialize GXM, then present the first guest frame.
