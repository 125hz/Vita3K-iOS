# iOS core-porting plan and blocker register

Baseline inspected: upstream Vita3K commit `e8a51a7995812d2393458c3df046b6463f6ad75f` (2026-07-13 checkout).

The CI target now proves that an unsigned, device-native UIKit/Metal application can be built and packaged and that a dependency-free upstream Vita3K core slice cross-compiles. The following work is required before it becomes an emulator.

| Area | Current upstream assumption | iOS work required | First acceptance test |
|---|---|---|---|
| Build graph | Non-Android builds unconditionally configure Qt and `gui-qt` | **Started:** `vita3k_ios_core` links the upstream ARM encoder and NID database; continue extracting the loader/memory dependency closure | Core slice links and passes on-device self-tests; full loader still pending |
| Frontend/lifecycle | Desktop `main.cpp`, Qt windows, SDL desktop events | Drive initialization, pause, resume, and shutdown from UIKit scenes/controllers | App backgrounds and resumes without losing emulator state |
| Renderer | Apple desktop path uses Vulkan through MoltenVK and a Cocoa-backed `CAMetalLayer` | **Started:** a portable host-display seam now accepts an `MTKView` drawable and reports Metal completion; choose an iOS-compatible MoltenVK build or create a native Metal backend | Physical-device Milestone 11 clear/present pending; Vita graphics remain disconnected |
| CPU/JIT | Dynarmic and memory helpers are built for current desktop/Android hosts | **Started:** a bounded ARM/Thumb runner now completes a tiny real VitaSDK module through its compiler-generated BLX/UXTB/POP path; continue adding instructions from progressively richer samples | Physical-device Milestone 10 diagnostic pending; general Vita homebrew still unsupported |
| Guest memory | Core reserves a contiguous 4 GiB region and uses POSIX signals/mach context details on Apple | **Started:** portable 4 GiB reservation, batch mapping, shared-page permission merging, and temporary write/restore transitions for verified relocations; add iOS fault handling | Native tests pass; physical-device Milestone 5 diagnostic pending |
| Dependencies | Boost, FFmpeg, SDL, Qt, Vulkan/MoltenVK and other submodules follow desktop/Android recipes | Inventory each dependency, disable unused ones, and build required libraries for `iphoneos arm64` | Reproducible dependency build with no simulator/macOS slices |
| Filesystem | Desktop paths, dialogs, and writable locations | **Started:** sandbox layout, import enumeration, fixed/preferred-address VELF loading, relocations, NID tables, and ARM import-stub rewriting exist; add alternate relocatable placement, SELF extraction, and Vita VFS mapping | Real VitaSDK Milestone 10 VELF applies 10 relocations, rebinds its stub, and returns on device |
| Audio/input | Desktop SDL/Qt device and event assumptions | Add AVAudioEngine/SDL-iOS audio path, GameController, and touch input adapters | Controller and audio loopback diagnostics pass |
| Diagnostics | Desktop console/log files and attached debugger | Keep unified log plus rotating/exportable file diagnostics and crash breadcrumbs | A device run produces a useful diagnostic bundle |

## Recommended integration order

1. **Done for the first dependency-free slice:** create `vita3k_ios_core`, cross-compile the ARM encoder/NID database for iPhone `arm64`, and run on-device self-tests.
2. **Done for plain ELF planning:** bounded SELF/ELF validation now emits checked `PT_LOAD` plans; SELF segment payload decoding remains pending.
3. **Done for shared host pages:** batch guest-memory mapping copies file data, zero-fills BSS, merges permissions for segments sharing a 16 KiB host page, reads back, and unmaps test ranges.
4. **Done for fixed-address plain ELF loading:** load the first valid `ET_SCE_EXEC` import, verify every copied byte, parse its 92-byte module-info header, and inventory relocation payloads.
5. **Done for the dependency-free loader:** apply bounded `PT_SCE_RELA` formats 0–9, verify each protected-memory patch, and parse long/short import plus export NID tables.
6. **Done for the execution seam:** fetch and execute a seven-instruction synthetic ARM routine, preserve register/PC state, route one SVC/NID through a diagnostic HLE binding, and verify its return value.
7. **Done for the loaded-entry seam:** validate the module metadata's `module_start`, run a synthetic mapped entry in a bounded guest-thread context, expose its UID, and exit with a verified status through two minimal kernel HLE handlers.
8. **Done for compiled ARM call plumbing:** rewrite parsed function imports to upstream-style SVC trampolines and execute B/BL, PUSH/POP, word LDR/STR, and return sequences in a generated legal ELF fixture.
9. **Done for the first interworking seam:** enter a generated module in Thumb mode, use compact compiler-style stack/data instructions, cross into rebound ARM import stubs with BLX, and return to Thumb through LR.
10. **Done for the first real VitaSDK seam:** compile a tiny C module in CI with a pinned official VitaSDK image, map its VELF at preferred addresses, apply its real relocation stream, bind its import, and complete its compiler-generated entry/return path.
11. **Done for the first host-display seam:** connect the core to the existing `MTKView`, submit one core-owned clear, present its drawable, and report command-buffer completion.
12. Add sandboxed storage, controller, touch, and audio adapters.
13. Only after interpreter-mode boot is stable, integrate and validate the ARM64 JIT platform layer.

Current loader limits are intentional: `ET_SCE_RELEXEC` segments are tried only at their preferred addresses, while SELF segments may require container-specific offset/decompression handling. Imported ARM function stubs are rewritten, but variable/TLS imports and Thumb import stubs are not bound. A valid ELF with a `module_start` is attempted with a 256-instruction ceiling; unsupported code stops with a detailed reason. The Milestone 10 interpreter recognizes only the narrow ARM/Thumb subset used by its two acceptance programs, including one 32-bit BL/BLX form. General Thumb-2 remains unsupported, so this must not be treated as a production Vita CPU backend or scheduler.

The relocation parser implements upstream formats 0–9 and the common ARM/Thumb codes, but the current deterministic acceptance fixture exercises format 0 (`ABS32`). Other compact formats still require representative legal fixtures before they should be considered device-validated.

## Firmware packages

Do not commit firmware PUP files to this public repository or upload them as Actions artifacts. The future iOS installer should select user-owned files locally and write only extracted virtual-filesystem content inside the app sandbox. Upstream `install_pup` currently depends on the packages/crypto layers, OpenSSL, vita-toolchain key handling, FAT/exFAT extraction, miniz, and psvpfsparser; those dependencies are not part of the current iOS core slice. `fontpkg.pup`, `preinstall.pup`, and the system update PUP should therefore remain local until that bounded installer milestone is implemented.

Keep platform checks narrow. Prefer interfaces such as `HostFilesystem`, `HostDisplay`, `HostAudio`, and `JitMemory` over broad `#ifdef __APPLE__` blocks: on current upstream, `__APPLE__` often means macOS and is not sufficient to identify UIKit/iOS behavior.

## Definition of a real emulator IPA

Do not label the artifact as a functioning Vita3K port until a physical-device diagnostic can initialize guest memory, load a legal Vita homebrew executable, execute guest code, and present guest-renderer output. Milestone 11 presents only a diagnostic clear; the existing artifact remains a build-pipeline/bootstrap milestone.
