# Vion iOS boot-path analysis

This note records the source-level comparison requested after Milestone 22. It is based on Vion commit [`d11138f2`](https://github.com/vion-app-org/Vion/commit/d11138f2eb42bc8d92994b5e27e92e2778d478ea) on branch `native-ios-port`, not on claims inferred from screenshots.

## What Vion actually boots

Vion does not contain a small independent Vita loader. Its Objective-C++ bridge calls the normal Vita3K application flow: configure app state, initialize the renderer, call `app::late_init`, `load_app`, `run_app`, then start the renderer thread. The implementation is visible in [`vita3k/ios_bridge.cpp`](https://github.com/vion-app-org/Vion/blob/d11138f2eb42bc8d92994b5e27e92e2778d478ea/vita3k/ios_bridge.cpp). Consequently, its boot path carries Vita3K's scheduler, kernel state, module/HLE implementations, virtual filesystem, firmware services, GXM translation, and renderer rather than replacing them with iOS-specific stubs.

The important iOS CPU change is a Unicorn interpreter adapter in [`vita3k/cpu/src/unicorn_cpu.cpp`](https://github.com/vion-app-org/Vion/blob/d11138f2eb42bc8d92994b5e27e92e2778d478ea/vita3k/cpu/src/unicorn_cpu.cpp). It configures an ARM Cortex-A9, maps Vita3K's shared 4 GiB guest address space into Unicorn, and returns SVC events to Vita3K's existing HLE dispatch loop. This avoids a Dynarmic JIT dependency on iOS; it does not avoid the rest of the emulator.

For graphics, Vion builds Vita3K's Vulkan renderer against MoltenVK and supplies a `CAMetalLayer` through its window callbacks. Its iOS target and framework graph are in [`vita3k/gui-ios/CMakeLists.txt`](https://github.com/vion-app-org/Vion/blob/d11138f2eb42bc8d92994b5e27e92e2778d478ea/vita3k/gui-ios/CMakeLists.txt). Swift/UIKit code owns the library and controls, while guest rendering remains the upstream Vulkan/GXM path.

## What is reusable here

- Adopt captured HLE functions with their exact upstream Vita3K state semantics. Milestone 23 does this for `__cxa_set_dso_handle_main`.
- Keep interpreter execution and SVC-to-HLE handoff behind the existing narrow CPU interface. A Unicorn-backed engine is a viable future replacement for the growing diagnostic interpreter.
- Migrate toward the full upstream scheduler, kernel/module registry, VFS, firmware services, GXM, and renderer as coherent subsystems. These are why Vion can progress beyond process startup.
- Keep iOS presentation behind a layer callback. The current Metal host diagnostic is not a substitute for Vita3K's guest renderer.

## Why the Vion commit is not being copied wholesale

The public commit adds `.gitmodules` declarations for `external/unicorn` and `external/ffmpeg-ios`, while its recorded tree does not contain the corresponding gitlink entries. Its CMake target nevertheless references both directories. The public repository also exposes no successful Actions run or release proving a reproducible iOS build. It additionally contains local signing/team and dependency assumptions that do not belong in this unsigned CI pipeline.

Both projects are GPL-2.0 licensed, so the main blocker is engineering and reproducibility rather than license incompatibility. Importing Vion as if it were a complete binary dependency would conceal missing components and make failures harder to attribute. The safe acceleration path is to port upstream-compatible subsystems in testable slices, then replace the bounded interpreter with a full interpreter/scheduler loop once its memory and HLE contracts are covered.

## Migration order after the captured startup calls

1. Continue binding the small, stateful C/C++ runtime initialization cluster only when Amagami reaches each real NID and upstream semantics are known.
2. Add persistent process/thread scheduling so HLE waits and thread creation can resume guest execution rather than ending a one-shot run.
3. Integrate a maintained ARMv7 interpreter backend such as Unicorn behind `ArmInterpreter`, retaining deterministic tests and no JIT requirement.
4. Bring in the required upstream kernel modules, VFS/firmware services, and module loader as a coherent runtime rather than hundreds of ad hoc return-value stubs.
5. Connect the upstream GXM/Vulkan renderer to MoltenVK and the existing iOS layer host; only then can a guest-produced menu frame appear.

Milestone 23 is step 1, not evidence that the title is near a rendered menu. It is valuable because it crosses the first real HLE boundary with upstream-correct state and exposes the next dependency without fabricating success.
