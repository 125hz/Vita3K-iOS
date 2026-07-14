# iOS core-porting plan and blocker register

Baseline inspected: upstream Vita3K commit `e8a51a7995812d2393458c3df046b6463f6ad75f` (2026-07-13 checkout).

The CI target now proves that an unsigned, device-native UIKit/Metal application can be built and packaged and that a dependency-free upstream Vita3K core slice cross-compiles. The following work is required before it becomes an emulator.

| Area | Current upstream assumption | iOS work required | First acceptance test |
|---|---|---|---|
| Build graph | Non-Android builds unconditionally configure Qt and `gui-qt` | **Started:** `vita3k_ios_core` links the upstream ARM encoder and NID database; continue extracting the loader/memory dependency closure | Core slice links and passes on-device self-tests; full loader still pending |
| Frontend/lifecycle | Desktop `main.cpp`, Qt windows, SDL desktop events | Drive initialization, pause, resume, and shutdown from UIKit scenes/controllers | App backgrounds and resumes without losing emulator state |
| Renderer | Apple desktop path uses Vulkan through MoltenVK and a Cocoa-backed `CAMetalLayer` | Choose and package an iOS-compatible MoltenVK build or create a native Metal backend; accept an `MTKView`/`CAMetalLayer` supplied by UIKit | Clear and present one emulator-owned frame on device |
| CPU/JIT | Dynarmic and memory helpers are built for current desktop/Android hosts | Cross-compile Dynarmic for `arm64-apple-ios`; isolate executable-memory allocation, protection transitions, cache invalidation, and exception handling | Execute a deterministic guest-code unit test on device |
| Guest memory | Core reserves a contiguous 4 GiB region and uses POSIX signals/mach context details on Apple | **Started:** portable 4 GiB reservation plus page commit/read-write/protect diagnostic; add range allocator, segment mapping, and iOS fault handling | Native tests pass; physical-device diagnostic pending |
| Dependencies | Boost, FFmpeg, SDL, Qt, Vulkan/MoltenVK and other submodules follow desktop/Android recipes | Inventory each dependency, disable unused ones, and build required libraries for `iphoneos arm64` | Reproducible dependency build with no simulator/macOS slices |
| Filesystem | Desktop paths, dialogs, and writable locations | **Started:** sandbox layout, import enumeration, and bounded SELF/ELF/VPK/SFO header probing exist; add document picker and Vita VFS mapping | Synthetic Vita ELF passes native smoke test; real fixture/device import pending |
| Audio/input | Desktop SDL/Qt device and event assumptions | Add AVAudioEngine/SDL-iOS audio path, GameController, and touch input adapters | Controller and audio loopback diagnostics pass |
| Diagnostics | Desktop console/log files and attached debugger | Keep unified log plus rotating/exportable file diagnostics and crash breadcrumbs | A device run produces a useful diagnostic bundle |

## Recommended integration order

1. **Done for the first dependency-free slice:** create `vita3k_ios_core`, cross-compile the ARM encoder/NID database for iPhone `arm64`, and run on-device self-tests.
2. **Done for plain ELF planning:** bounded SELF/ELF validation now emits checked `PT_LOAD` plans; SELF segment payload decoding remains pending.
3. **Started:** portable guest-memory reservation and page-protection diagnostics exist; next map validated plain-ELF segments and zero-fill their BSS ranges.
4. Run loader and CPU unit tests against legal homebrew fixtures before adding presentation.
5. Connect the renderer to the existing `MTKView` host.
6. Add sandboxed storage, controller, touch, and audio adapters.
7. Only after interpreter-mode boot is stable, integrate and validate the ARM64 JIT platform layer.

Keep platform checks narrow. Prefer interfaces such as `HostFilesystem`, `HostDisplay`, `HostAudio`, and `JitMemory` over broad `#ifdef __APPLE__` blocks: on current upstream, `__APPLE__` often means macOS and is not sufficient to identify UIKit/iOS behavior.

## Definition of a real emulator IPA

Do not label the artifact as a functioning Vita3K port until CI links the upstream core and a physical-device diagnostic run can initialize guest memory, load a legal Vita homebrew executable, execute guest code, and present frames. The existing artifact is a build-pipeline/bootstrap milestone only.
