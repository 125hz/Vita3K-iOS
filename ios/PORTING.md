# iOS core-porting plan and blocker register

Baseline inspected: upstream Vita3K commit `e8a51a7995812d2393458c3df046b6463f6ad75f` (2026-07-13 checkout).

The CI target now proves that an unsigned, device-native UIKit/Metal application can be built and packaged and that a dependency-free upstream Vita3K core slice cross-compiles. The following work is required before it becomes an emulator.

| Area | Current upstream assumption | iOS work required | First acceptance test |
|---|---|---|---|
| Build graph | Non-Android builds unconditionally configure Qt and `gui-qt` | **Started:** `vita3k_ios_core` links the upstream ARM encoder and NID database; continue extracting the loader/memory dependency closure | Core slice links and passes on-device self-tests; full loader still pending |
| Frontend/lifecycle | Desktop `main.cpp`, Qt windows, SDL desktop events | Drive initialization, pause, resume, and shutdown from UIKit scenes/controllers | App backgrounds and resumes without losing emulator state |
| Renderer | Apple desktop path uses Vulkan through MoltenVK and a Cocoa-backed `CAMetalLayer` | Choose and package an iOS-compatible MoltenVK build or create a native Metal backend; accept an `MTKView`/`CAMetalLayer` supplied by UIKit | Clear and present one emulator-owned frame on device |
| CPU/JIT | Dynarmic and memory helpers are built for current desktop/Android hosts | **Started:** a bounded ARM-mode execution/HLE diagnostic now runs from guest memory; next connect the production CPU/HLE/thread interfaces, then cross-compile Dynarmic for `arm64-apple-ios` and isolate executable-memory/cache behavior | Physical-device Milestone 6 diagnostic passes; mapped homebrew entry-point test pending |
| Guest memory | Core reserves a contiguous 4 GiB region and uses POSIX signals/mach context details on Apple | **Started:** portable 4 GiB reservation, batch mapping, shared-page permission merging, and temporary write/restore transitions for verified relocations; add iOS fault handling | Native tests pass; physical-device Milestone 5 diagnostic pending |
| Dependencies | Boost, FFmpeg, SDL, Qt, Vulkan/MoltenVK and other submodules follow desktop/Android recipes | Inventory each dependency, disable unused ones, and build required libraries for `iphoneos arm64` | Reproducible dependency build with no simulator/macOS slices |
| Filesystem | Desktop paths, dialogs, and writable locations | **Started:** sandbox layout, import enumeration, bounded probing, fixed-address plain ELF loading, relocations, and module NID-table parsing exist; add document picker, SELF extraction, and Vita VFS mapping | Synthetic on-disk Vita ELF maps, relocates, parses one import/export table, and passes native tests; physical-device import pending |
| Audio/input | Desktop SDL/Qt device and event assumptions | Add AVAudioEngine/SDL-iOS audio path, GameController, and touch input adapters | Controller and audio loopback diagnostics pass |
| Diagnostics | Desktop console/log files and attached debugger | Keep unified log plus rotating/exportable file diagnostics and crash breadcrumbs | A device run produces a useful diagnostic bundle |

## Recommended integration order

1. **Done for the first dependency-free slice:** create `vita3k_ios_core`, cross-compile the ARM encoder/NID database for iPhone `arm64`, and run on-device self-tests.
2. **Done for plain ELF planning:** bounded SELF/ELF validation now emits checked `PT_LOAD` plans; SELF segment payload decoding remains pending.
3. **Done for shared host pages:** batch guest-memory mapping copies file data, zero-fills BSS, merges permissions for segments sharing a 16 KiB host page, reads back, and unmaps test ranges.
4. **Done for fixed-address plain ELF loading:** load the first valid `ET_SCE_EXEC` import, verify every copied byte, parse its 92-byte module-info header, and inventory relocation payloads.
5. **Done for the dependency-free loader:** apply bounded `PT_SCE_RELA` formats 0–9, verify each protected-memory patch, and parse long/short import plus export NID tables.
6. **Done for the execution seam:** fetch and execute a seven-instruction synthetic ARM routine, preserve register/PC state, route one SVC/NID through a diagnostic HLE binding, and verify its return value.
7. Connect a mapped legal homebrew entry point to production thread state and the minimum real Vita3K HLE handlers required to return cleanly.
8. Connect the renderer to the existing `MTKView` host.
9. Add sandboxed storage, controller, touch, and audio adapters.
10. Only after interpreter-mode boot is stable, integrate and validate the ARM64 JIT platform layer.

Current loader limits are intentional: `ET_SCE_RELEXEC` requires rebasing and SELF segments may require container-specific offset/decompression handling. Imported NIDs are inventoried but not yet bound to production Vita3K HLE handlers. A file shown as `MAPPED` is structurally loaded and relocated for diagnostics; its entry point is not launched. The Milestone 6 interpreter recognizes only the narrow instruction subset used by its synthetic acceptance program and must not be treated as a general Vita CPU backend.

The relocation parser implements upstream formats 0–9 and the common ARM/Thumb codes, but the current deterministic acceptance fixture exercises format 0 (`ABS32`). Other compact formats still require representative legal fixtures before they should be considered device-validated.

## Firmware packages

Do not commit firmware PUP files to this public repository or upload them as Actions artifacts. The future iOS installer should select user-owned files locally and write only extracted virtual-filesystem content inside the app sandbox. Upstream `install_pup` currently depends on the packages/crypto layers, OpenSSL, vita-toolchain key handling, FAT/exFAT extraction, miniz, and psvpfsparser; those dependencies are not part of the current iOS core slice. `fontpkg.pup`, `preinstall.pup`, and the system update PUP should therefore remain local until that bounded installer milestone is implemented.

Keep platform checks narrow. Prefer interfaces such as `HostFilesystem`, `HostDisplay`, `HostAudio`, and `JitMemory` over broad `#ifdef __APPLE__` blocks: on current upstream, `__APPLE__` often means macOS and is not sufficient to identify UIKit/iOS behavior.

## Definition of a real emulator IPA

Do not label the artifact as a functioning Vita3K port until CI links the upstream core and a physical-device diagnostic run can initialize guest memory, load a legal Vita homebrew executable, execute guest code, and present frames. The existing artifact is a build-pipeline/bootstrap milestone only.
