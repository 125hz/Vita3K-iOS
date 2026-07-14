# iOS core-porting plan and blocker register

Baseline inspected: upstream Vita3K commit `e8a51a7995812d2393458c3df046b6463f6ad75f` (2026-07-13 checkout).

The CI target now proves that an unsigned, device-native UIKit/Metal application can be built and packaged and that a dependency-free upstream Vita3K core slice cross-compiles. The following work is required before it becomes an emulator.

| Area | Current upstream assumption | iOS work required | First acceptance test |
|---|---|---|---|
| Build graph | Non-Android builds unconditionally configure Qt and `gui-qt` | **Started:** `vita3k_ios_core` links the upstream ARM encoder and NID database; continue extracting the loader/memory dependency closure | Core slice links and passes on-device self-tests; full loader still pending |
| Frontend/lifecycle | Desktop `main.cpp`, Qt windows, SDL desktop events | Drive initialization, pause, resume, and shutdown from UIKit scenes/controllers | App backgrounds and resumes without losing emulator state |
| Renderer | Apple desktop path uses Vulkan through MoltenVK and a Cocoa-backed `CAMetalLayer` | Choose and package an iOS-compatible MoltenVK build or create a native Metal backend; accept an `MTKView`/`CAMetalLayer` supplied by UIKit | Clear and present one emulator-owned frame on device |
| CPU/JIT | Dynarmic and memory helpers are built for current desktop/Android hosts | Cross-compile Dynarmic for `arm64-apple-ios`; isolate executable-memory allocation, protection transitions, cache invalidation, and exception handling | Execute a deterministic guest-code unit test on device |
| Guest memory | Core reserves a contiguous 4 GiB region and uses POSIX signals/mach context details on Apple | **Started:** portable 4 GiB reservation plus batch segment mapping, shared-page permission merging, BSS zero-fill, readback, and unmapping; add iOS fault handling | Native tests pass; physical-device Milestone 4 diagnostic pending |
| Dependencies | Boost, FFmpeg, SDL, Qt, Vulkan/MoltenVK and other submodules follow desktop/Android recipes | Inventory each dependency, disable unused ones, and build required libraries for `iphoneos arm64` | Reproducible dependency build with no simulator/macOS slices |
| Filesystem | Desktop paths, dialogs, and writable locations | **Started:** sandbox layout, import enumeration, bounded probing, and fixed-address plain ELF loading exist; add document picker, SELF extraction, and Vita VFS mapping | Synthetic on-disk Vita ELF maps, verifies module info, and passes native smoke tests; physical-device import pending |
| Audio/input | Desktop SDL/Qt device and event assumptions | Add AVAudioEngine/SDL-iOS audio path, GameController, and touch input adapters | Controller and audio loopback diagnostics pass |
| Diagnostics | Desktop console/log files and attached debugger | Keep unified log plus rotating/exportable file diagnostics and crash breadcrumbs | A device run produces a useful diagnostic bundle |

## Recommended integration order

1. **Done for the first dependency-free slice:** create `vita3k_ios_core`, cross-compile the ARM encoder/NID database for iPhone `arm64`, and run on-device self-tests.
2. **Done for plain ELF planning:** bounded SELF/ELF validation now emits checked `PT_LOAD` plans; SELF segment payload decoding remains pending.
3. **Done for shared host pages:** batch guest-memory mapping copies file data, zero-fills BSS, merges permissions for segments sharing a 16 KiB host page, reads back, and unmaps test ranges.
4. **Done for fixed-address plain ELF loading:** load the first valid `ET_SCE_EXEC` import, verify every copied byte, parse its 92-byte module-info header, and inventory relocation payloads.
5. Apply `PT_SCE_RELA` relocations, parse module import/export tables, and add legal homebrew loader fixtures before CPU execution.
6. Connect the renderer to the existing `MTKView` host.
7. Add sandboxed storage, controller, touch, and audio adapters.
8. Only after interpreter-mode boot is stable, integrate and validate the ARM64 JIT platform layer.

Current loader limits are intentional: `ET_SCE_RELEXEC` requires rebasing, SELF segments may require container-specific offset/decompression handling, and relocation payloads are inventoried but not applied. A file shown as `MAPPED` is structurally loaded for diagnostics; it is not yet executable.

Keep platform checks narrow. Prefer interfaces such as `HostFilesystem`, `HostDisplay`, `HostAudio`, and `JitMemory` over broad `#ifdef __APPLE__` blocks: on current upstream, `__APPLE__` often means macOS and is not sufficient to identify UIKit/iOS behavior.

## Definition of a real emulator IPA

Do not label the artifact as a functioning Vita3K port until CI links the upstream core and a physical-device diagnostic run can initialize guest memory, load a legal Vita homebrew executable, execute guest code, and present frames. The existing artifact is a build-pipeline/bootstrap milestone only.
