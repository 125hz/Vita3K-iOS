# iOS core-porting plan and blocker register

Baseline inspected: upstream Vita3K commit `e8a51a7995812d2393458c3df046b6463f6ad75f` (2026-07-13 checkout).

The CI target now proves that an unsigned, device-native UIKit/Metal application can be built and packaged and that a dependency-free upstream Vita3K core slice cross-compiles. The following work is required before it becomes an emulator.

| Area | Current upstream assumption | iOS work required | First acceptance test |
|---|---|---|---|
| Build graph | Non-Android builds unconditionally configure Qt and `gui-qt` | **Started:** `vita3k_ios_core` links the upstream ARM encoder, NID database, and package/SFO parser without the desktop graph; continue extracting VFS, package, loader, kernel, CPU, and renderer closures | Core slice links and passes on-device self-tests; full loader still pending |
| Frontend/lifecycle | Desktop `main.cpp`, Qt windows, SDL desktop events | Drive initialization, pause, resume, and shutdown from UIKit scenes/controllers | App backgrounds and resumes without losing emulator state |
| Renderer | Apple desktop path uses Vulkan through MoltenVK and a Cocoa-backed `CAMetalLayer` | **Started:** a portable host-display seam now accepts an `MTKView` drawable and reports Metal completion; choose an iOS-compatible MoltenVK build or create a native Metal backend | Physical-device Milestone 11 clear/present confirmed; Vita graphics remain disconnected |
| Input | Desktop paths consume SDL/controller state and frontend events | **Started:** UIKit touch samples are normalized and GameController state is captured behind a portable seam; connect this state to Vita controller/touch HLE | Physical-device Milestone 12 touch/controller diagnostic confirmed |
| CPU/JIT | Dynarmic and memory helpers are built for current desktop/Android hosts | **Started:** a bounded ARM/Thumb runner now completes a tiny real VitaSDK module and implements a tested compiler baseline across Thumb arithmetic, flags, memory, stack, and control-flow families | Physical-device Milestone 19 reached `SUB sp, #8` after the real Amagami wide prologue; general Vita homebrew remains unsupported |
| Guest memory | Core reserves a contiguous 4 GiB region and uses POSIX signals/mach context details on Apple | **Started:** portable 4 GiB reservation, batch mapping, shared-page permission merging, and temporary write/restore transitions for verified relocations; add iOS fault handling | Native tests pass; physical-device Milestone 5 diagnostic pending |
| Dependencies | Boost, FFmpeg, SDL, Qt, Vulkan/MoltenVK and other submodules follow desktop/Android recipes | Inventory each dependency, disable unused ones, and build required libraries for `iphoneos arm64` | Reproducible dependency build with no simulator/macOS slices |
| Filesystem | Desktop paths, dialogs, and writable locations | **Started:** sandbox layout, transactional app/patch installation, title selection, fixed/preferred-address VELF loading, relocations, NID tables, ARM import-stub rewriting, and plain SELF segment loading exist; add alternate relocatable placement, protected SELF decryption, and Vita VFS mapping | Milestone 16 prepares a legal installed SELF and stops safely on encrypted segments |
| Packages/metadata | Full package target depends on crypto, emuenv, FAT/exFAT, io, miniz, psvpfsparser, and vita-toolchain | **Started:** upstream SFO/miniz paths identify and install app/patch archives; extract the protected SELF, PUP, and remaining VFS dependency closures | Physical-device Milestone 15 installed a user-owned base app and patch transactionally |
| Audio | Desktop SDL/cubeb device assumptions | Add an AVAudioEngine or SDL-iOS host path, then connect Vita audio HLE | Bounded audio loopback diagnostic passes |
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
12. **Done for the first host-input seam:** normalize UIKit touch events, capture GameController sticks/buttons, and expose bounded input status to the portable core.
13. **Done for the first upstream package seam:** link Vita3K's real SFO parser, remove its unnecessary Boost/fmt dependency, reject malformed offsets, and identify imported application title metadata.
14. **Done for bounded package inspection:** identify app/patch roots and reject unsafe archive paths.
15. **Done for transactional installation:** commit base-app and patch roots together in the private sandbox and inventory installed titles.
16. **Done for installed-executable preparation:** provide a title library/settings UI, resolve patch-over-base `eboot.bin`, load legal plain SELF segments, and stop explicitly on encrypted segments.
17. **Device accepted:** require explicit confirmation, then attempt the prepared module once under a non-JIT interpreter with a hard 256-instruction ceiling and exact stop diagnostics.
18. **Device accepted:** retain export entry addresses and follow upstream's `NID_MODULE_START` lifecycle override before attempting execution. Amagami reached its real Thumb entry and exposed `0xE92D` at `0x81017580`.
19. **Device accepted:** execute valid Thumb-2 `PUSH.W`/`STMDB sp!` register lists, including high registers. Amagami advanced to `0xB082` at `0x81017584`.
20. **Device accepted:** execute `ADD/SUB sp` plus coherent common Thumb arithmetic, flag, memory, high-register, multiple-transfer, and branch families. Amagami advanced through four instructions and exposed `MOVW`/`STRD`/`MOVT` plus six following halfwords.
21. **Device accepted:** execute complete Thumb-2 immediate `MOVW`/`MOVT` constant construction and immediate `LDRD`/`STRD` offset/pre/post-indexed writeback families. Amagami advanced to its first imported HLE trampoline after ten instructions, but the boundary incorrectly fell back to NID zero.
22. **Implemented pending device acceptance:** resolve inline NIDs from validated `SVC; MOV pc,lr; NID` import trampolines even when unimplemented, and capture the NID name, callsite, return address, arguments, `r12`, and import-inventory status without fabricating HLE results.
23. After bounded interpreter boot is stable, integrate and validate the ARM64 JIT platform layer.

Current loader limits are intentional: `ET_SCE_RELEXEC` segments are tried only at their preferred addresses, while SELF segments may require container-specific offset/decompression handling. Imported ARM function stubs are rewritten, but variable/TLS imports and Thumb import stubs are not bound. The loader resolves `NID_MODULE_START` from the export entry table before a valid module is attempted with a 65536-instruction ceiling; unsupported code stops with a detailed reason and bounded look-ahead. Budget exhaustion reports PC coverage, hot-loop evidence, recent PCs, registers, and lookahead. The interpreter now covers common 16-bit Thumb compiler families, one 32-bit BL/BLX form, wide load/store multiple including `PUSH.W`/`POP.W`, `MOVW`/`MOVT`, and immediate `LDRD`/`STRD`. IT blocks, much of Thumb-2, floating point/NEON, exceptions, atomics, and production scheduling remain unsupported. The concrete critical path to a guest-produced title screen is tracked in `MENU-BOOT-CHECKLIST.md`.

The relocation parser implements upstream formats 0–9 and the common ARM/Thumb codes, but the current deterministic acceptance fixture exercises format 0 (`ABS32`). Other compact formats still require representative legal fixtures before they should be considered device-validated.

## Milestone 23 libc startup boundary

The bounded runtime now implements only Amagami's captured `__cxa_set_dso_handle_main` boundary. It matches upstream Vita3K by saving `r0` as the process-wide libc DSO handle, returns through the normal import trampoline, and continues until the next honest boundary. The CI artifact's `milestone23-libc-dso-runtime.zip` verifies the state mutation, successful NID retention, one HLE dispatch, and clean module return. This is not a general libc implementation or a full game boot loop; the Vion comparison in `ios/VION-ANALYSIS.md` identifies the full scheduler, module, VFS, Unicorn, GXM, and renderer integration still required.

## Milestone 24 broad Thumb-2 runtime families

The bounded interpreter now accepts the complete modified-immediate ALU family and the compiler-facing wide single-register load/store family anchored by Amagami's `F05F 0B00` and `F8DD A000` sequence. Immediate expansion, arithmetic/logical flags, signed loads, access widths, pre/post indexing, add/subtract addressing, and writeback are state-tested. Unpredictable aliases, invalid encodings, overflow, and unmapped accesses stop without changing unknown state. The legal `milestone24-thumb2-runtime-families.zip` fixture crosses the libc HLE and both captured instructions before returning cleanly.

## Milestone 25 register-operated Thumb-2 runtime families

The bounded interpreter now accepts the scalar logical, arithmetic, test, move, and immediate-shift portions of Thumb-2 data processing with a shifted register, including exact shift carry and arithmetic flags. It also accepts scaled register-offset byte/halfword/word loads and stores with signed load extension. Amagami's exact `EBAA 0202` subtraction is covered alongside negative tests for `PKH`, invalid SP/PC operands, reserved memory suffixes, address overflow, and guest faults. The next unsupported boundary includes sixteen following halfwords to accelerate coherent-family selection without skipping guest execution.

## Milestone 26 libc termination registration

The bounded HLE surface now clears Amagami's captured `__aeabi_atexit` boundary and proactively covers the ABI-equivalent `__cxa_atexit` plus `__cxa_finalize`. Registration calls retain normalized object, destructor, and DSO values for diagnostics while returning upstream's current zero result. Finalization is observable but does not execute guest callbacks, matching upstream Vita3K's present stub rather than inventing teardown behavior. The legal `milestone26-libc-termination.zip` fixture proves import dispatch, argument retention, the return value, and a clean module return; separate portable cases verify both registration argument orders and finalize state.

## Milestone 27 Thumb-2 multiple transfers

The bounded interpreter now implements Thumb-2 `STMIA`, `LDMIA`, `STMDB`, and `LDMDB` with optional writeback, including the wide push/pop aliases and high-register lists. A PC load performs interworking and recognizes the existing zero-link diagnostic sentinel. Transfers use one contiguous checked guest-memory operation, and architectural unpredictable cases remain hard stops. Amagami's exact `E8BD 81F0` epilogue is covered by both CPU-state tests and the legal `milestone27-thumb2-multiple-transfer.zip` title.

## Milestone 28 C++ static-initialization guards

The bounded runtime now implements the complete Arm 32-bit `__cxa_guard_acquire`, `__cxa_guard_release`, and `__cxa_guard_abort` cluster reached by Amagami after 122 instructions. Guard words are checked as aligned 4-byte guest objects; acquire observes initialized bit zero and grants one owner, release sets that bit while preserving the remainder of the word, and abort only releases ownership. Invalid memory and recursive initialization remain explicit execution boundaries. The legal `milestone28-cxa-guards.zip` fixture covers first acquisition, while portable companion cases cover initialized, release, abort, and recursion paths.

## Milestone 29 bounded libc guest heap

The bounded runtime now maps a separate 16 MiB read/write guest arena and services the standard `calloc`, `malloc`, `memalign`, `free`, `realloc`, and `malloc_usable_size` imports. This clears Amagami's captured `memalign(16, 384)` startup boundary with a real writable guest pointer rather than a host pointer or fabricated address. Block splitting, alignment padding, coalescing, zeroing, resize copying, pointer validation, and live/peak diagnostics are state-tested. Cross-thread allocation, arena growth, errno, mspaces, and integration with upstream Vita3K's process memory allocator remain future work.

## Milestone 30 LR shifted-register execution runway

The accepted Milestone 29 run completed `memalign`, reached 138 instructions and six HLE calls, then stopped on legal `EB00 00CE` (`ADD.W r0, r0, lr, LSL #3`). Armv7's bad-register rule excludes `sp` and `pc`, not `lr`; the local decoder had grouped all three high registers together. The complete Thumb-2 shifted-register ALU/test family now permits LR in source, shifted-source, and destination positions while retaining explicit rejection of SP/PC misuse. The one-shot UI budget increases to 4096 interpreted instructions so already-supported startup code can advance farther without removing the stop-on-first-unknown safety boundary.

## Milestone 31 instrumented execution runway

The accepted Milestone 30 run exhausted all 4096 instructions without another unknown opcode or HLE call. It completed 14 HLE calls, five termination registrations, three guard acquisitions/two releases, and three heap allocations totaling 11904 live bytes. Milestone 31 expands the one-shot ceiling to 65536 and records unique/hottest PC counts, non-forward transfers, recent PCs, registers, CPSR, and lookahead. Concentrated execution is reported as a hot-loop candidate rather than skipped; the next device result can therefore distinguish useful initialization progress from the first scheduler/synchronization requirement.

## Milestone 32 C++ allocation ABI

The Milestone 31 device run advanced to 5637 instructions and 30 HLE calls before reaching `_Znwj` (NID `0xF99ED5AC`) for an 8-byte scalar allocation. The bounded runtime now binds the coherent Itanium C++ ABI family exported by Vita libc: scalar/array `new`, nothrow `new`, scalar/array `delete`, nothrow deletes, and placement deletes. Allocations and real deletions share the checked guest heap; null and placement deletion are no-ops; nothrow exhaustion returns zero. Throwing exhaustion remains a hard boundary until guest exception and new-handler unwinding exist. The legal `milestone32-cxx-allocation.zip` reproduces the exact captured call, and portable companion cases exercise every export and failure mode.

## Milestone 33 AppUtil lifecycle

The accepted Milestone 32 device run completed the 8-byte C++ allocation and advanced to 5676 instructions/31 HLE calls before reaching `sceAppUtilInit` (NID `0xDAFFE671`). The iOS diagnostic runner now validates the official VitaSDK `SceAppUtilInitParam` (`0x40`) and `SceAppUtilBootParam` (`0x28`) layouts, reserved bytes, and mapped guest ranges; records their inputs; and tracks a paired `sceAppUtilShutdown` lifecycle. Null and state errors use documented AppUtil codes, while unmapped pointers stop execution. This does not fabricate app events, savedata, mounts, or host system settings, and it remains a narrow local implementation because upstream Vita3K currently marks both lifecycle functions unimplemented. CI emits `milestone33-app-util-lifecycle.zip`; portable cases cover success, state errors, pointer faults, and init/shutdown ordering.

## Milestone 34 public Sysmodule lifecycle

The accepted Milestone 33 device run reported successful AppUtil initialization, then reached `sceSysmoduleLoadModule(0x15)` at 5683 instructions/32 HLE calls. Milestone 34 implements the coherent public load/status/unload family with per-run loaded-module state and upstream `INVALID_VALUE`/`UNLOADED` results. The HLE path records module availability but does not execute or emulate proprietary sysmodule binaries. This allows initialization to advance through normal module bookkeeping while preserving the first actual NP service dependency as a hard boundary. CI emits `milestone34-sysmodule-lifecycle.zip`, and portable tests cover the complete state transition, an unloaded query, and invalid IDs.

## Milestone 35 NP and Trophy startup lifecycle

The accepted Milestone 34 run loaded NP and NP Trophy and reached `sceNpInit(nullptr, nullptr)` at 5692 instructions/34 HLE calls. Milestone 35 implements stateful NP init/term, checked offline service-state output, and the paired Trophy init/term lifecycle. Communication config/ID pointers are bounded guest reads when present; service state is a bounded guest write. Duplicate and invalid-state calls return upstream errors, and NP termination also clears Trophy state. Network callbacks, online identity, trophy contexts, and TRP access remain unbound rather than being faked. CI emits `milestone35-np-lifecycle.zip` with portable lifecycle, memory-boundary, and state-error coverage.

## Milestone 36 NP Trophy objects

The accepted Milestone 35 run initialized NP and Trophy, then reached `sceNpTrophyCreateContext` at 5712 instructions/36 HLE calls with all three pointers mapped. Milestone 36 implements the coherent context/handle object family: create/destroy context and create/destroy/abort handle. Context creation validates guest output and the 12-byte Communication ID, returns a checked guest-visible ID, and records the opaque startup object. Object registries are cleared by Trophy or NP termination. This remains deliberately separate from TRP parsing and trophy data queries, which require the real guest VFS. CI emits `milestone36-trophy-objects.zip` with nine portable success, state, validation, and memory-boundary paths.

## Milestone 37 controller startup HLE

The accepted Milestone 36 device run created Trophy context 1 and reached `sceCtrlSetSamplingMode(2)` at 5722 instructions/37 HLE calls. Milestone 37 binds the complete upstream-implemented controller startup/read cluster imported by normal titles: sampling-mode get/set and extended variants, handheld port/wireless queries, multi-controller support, and twelve read/peek buffer forms. The diagnostic core validates modes, ports, counts, and guest ranges and emits upstream controller errors. Before host samples are bridged into guest HLE, it returns neutral centered controller data; negative APIs retain inverted-button semantics. Read calls produce one diagnostic sample because real vblank blocking requires the future scheduler/display queue. CI emits `milestone37-controller-hle.zip` with set/get, buffered-sample, and invalid-boundary coverage.

## Milestone 38 touch, time, power, and display startup HLE

The accepted Milestone 37 device run accepted controller sampling mode 2 and reached `sceTouchSetSamplingState(0, 1)` at 5,735 instructions/38 HLE calls. Milestone 38 batches four upstream-backed startup families in one milestone instead of waiting for each to become the next physical boundary: the touch family (sampling state, panel info, pixel density, force mode, peek/read with empty neutral samples), the process/system/RTC time family on a deterministic virtual microsecond clock, the power clock/battery family with upstream fixed clock reports and an offline battery profile, and the display family (vblank wait/vcount on a virtual vblank counter, refresh rate, and validated framebuffer set/get bookkeeping). All guest memory access is checked; a failed guest write stops the run as a hard boundary without partially mutating state. Touch reads advance the shared virtual vblank exactly once per call because real blocking requires the future scheduler; peek honors upstream stopped-sampling semantics by returning zero buffers. Unimplemented upstream stubs (regions, ext variants, device info) remain hard boundaries. CI emits `milestone38-startup-services.zip` with portable coverage of every family's success, state, validation, and memory-boundary paths.

## Firmware packages

Do not commit firmware PUP files to this public repository or upload them as Actions artifacts. The future iOS installer should select user-owned files locally and write only extracted virtual-filesystem content inside the app sandbox. Upstream `install_pup` currently depends on the packages/crypto layers, OpenSSL, vita-toolchain key handling, FAT/exFAT extraction, miniz, and psvpfsparser; those dependencies are not part of the current iOS core slice. `fontpkg.pup`, `preinstall.pup`, and the system update PUP should therefore remain local until that bounded installer milestone is implemented.

Keep platform checks narrow. Prefer interfaces such as `HostFilesystem`, `HostDisplay`, `HostAudio`, and `JitMemory` over broad `#ifdef __APPLE__` blocks: on current upstream, `__APPLE__` often means macOS and is not sufficient to identify UIKit/iOS behavior.

## Definition of a real emulator IPA

Do not label the artifact as a functioning Vita3K port until a physical-device diagnostic can initialize guest memory, load a legal Vita homebrew executable, execute guest code, and present guest-renderer output. Milestone 11 presents only a diagnostic clear; the existing artifact remains a build-pipeline/bootstrap milestone.
