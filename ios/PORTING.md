# iOS core-porting plan and blocker register

Baseline inspected: upstream Vita3K commit `e8a51a7995812d2393458c3df046b6463f6ad75f` (2026-07-13 checkout).

The CI target currently proves only that an unsigned, device-native UIKit/Metal application can be built and packaged. The following work is required before it becomes an emulator.

| Area | Current upstream assumption | iOS work required | First acceptance test |
|---|---|---|---|
| Build graph | Non-Android builds unconditionally configure Qt and `gui-qt` | Define a headless/mobile core library with dependency feature flags | Core static library links into `Vita3KiOS` |
| Frontend/lifecycle | Desktop `main.cpp`, Qt windows, SDL desktop events | Drive initialization, pause, resume, and shutdown from UIKit scenes/controllers | App backgrounds and resumes without losing emulator state |
| Renderer | Apple desktop path uses Vulkan through MoltenVK and a Cocoa-backed `CAMetalLayer` | Choose and package an iOS-compatible MoltenVK build or create a native Metal backend; accept an `MTKView`/`CAMetalLayer` supplied by UIKit | Clear and present one emulator-owned frame on device |
| CPU/JIT | Dynarmic and memory helpers are built for current desktop/Android hosts | Cross-compile Dynarmic for `arm64-apple-ios`; isolate executable-memory allocation, protection transitions, cache invalidation, and exception handling | Execute a deterministic guest-code unit test on device |
| Guest memory | Core reserves a contiguous 4 GiB region and uses POSIX signals/mach context details on Apple | Measure iOS virtual-memory behavior; add an iOS memory strategy and platform exception boundary | Allocate guest memory and pass read/write/protect tests |
| Dependencies | Boost, FFmpeg, SDL, Qt, Vulkan/MoltenVK and other submodules follow desktop/Android recipes | Inventory each dependency, disable unused ones, and build required libraries for `iphoneos arm64` | Reproducible dependency build with no simulator/macOS slices |
| Filesystem | Desktop paths, dialogs, and writable locations | Map Vita storage/config/logs to the app sandbox; use document picker/security-scoped URLs where required | Import and enumerate a legal homebrew fixture |
| Audio/input | Desktop SDL/Qt device and event assumptions | Add AVAudioEngine/SDL-iOS audio path, GameController, and touch input adapters | Controller and audio loopback diagnostics pass |
| Diagnostics | Desktop console/log files and attached debugger | Keep unified log plus rotating/exportable file diagnostics and crash breadcrumbs | A device run produces a useful diagnostic bundle |

## Recommended integration order

1. Extract a platform-neutral `vita3k_core` static library without renderer or GUI initialization.
2. Cross-compile only its minimal dependency closure for iPhone `arm64`.
3. Replace `CoreBridge.cpp` with a lifecycle-owning adapter and turn `VITA3K_IOS_LINK_CORE` into a real link switch.
4. Run loader and CPU unit tests against legal homebrew fixtures before adding presentation.
5. Connect the renderer to the existing `MTKView` host.
6. Add sandboxed storage, controller, touch, and audio adapters.
7. Only after interpreter-mode boot is stable, integrate and validate the ARM64 JIT platform layer.

Keep platform checks narrow. Prefer interfaces such as `HostFilesystem`, `HostDisplay`, `HostAudio`, and `JitMemory` over broad `#ifdef __APPLE__` blocks: on current upstream, `__APPLE__` often means macOS and is not sufficient to identify UIKit/iOS behavior.

## Definition of a real emulator IPA

Do not label the artifact as a functioning Vita3K port until CI links the upstream core and a physical-device diagnostic run can initialize guest memory, load a legal Vita homebrew executable, execute guest code, and present frames. The existing artifact is a build-pipeline/bootstrap milestone only.
