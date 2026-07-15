# Vita3K iOS bootstrap

This directory is an experimental, device-only iOS application for a future Vita3K port. It builds an unsigned IPA containing a UIKit/Metal host, file-backed logging, sandbox storage, and the first cross-compiled slices of upstream Vita3K code. It can transactionally install user-selected app/patch ZIPs, list installed titles, prefer a patch `eboot.bin`, safely prepare a selected Vita SELF, and make one explicitly confirmed interpreter attempt with a hard 65536-instruction ceiling. It also loads and completes the `module_start` of a deliberately tiny legal fixture, executes a tested baseline of compiler-generated Thumb arithmetic, memory, stack, and branch instructions, presents one core-owned Metal diagnostic frame, and captures normalized UIKit/GameController input. It does **not** yet execute general installed games, render Vita graphics, route input to guest services, or provide a production CPU/HLE implementation. See [MENU-BOOT-CHECKLIST.md](MENU-BOOT-CHECKLIST.md) for the remaining first-menu gates.

The separation is intentional: upstream's current Apple target is a macOS desktop application using Qt, Cocoa, SDL, MoltenVK, and desktop-oriented dependency builds. Those pieces cannot simply be linked into an iOS application.

## Current result

- `arm64` iPhone/iPad application bundle
- unsigned IPA produced by GitHub Actions
- UIKit lifecycle and an `MTKView` rendering host
- unified logging plus `Documents/vita3k.log`
- a `vita3k_ios_core` static library containing Vita3K's ARM instruction encoder and NID database
- on-device self-tests proving the upstream core slice is linked and executing
- sandbox directories under `Documents/Vita3K`, with discovery of legal `.vpk`, `.self`, `.elf`, `.bin`, and `.sfo` import candidates
- bounded header probing for Vita SELF/ELF, VPK/ZIP, and PARAM.SFO candidates before any guest-memory mapping
- a portable 4 GiB Vita guest-address-space reservation with one-page commit, read/write, decommit, and protection diagnostics
- validated ELF `PT_LOAD` segment plans with 32-bit address-overflow and file-range checks
- checked guest-segment mapping with file-byte copying, BSS zero-fill, final host-page protection, readback, and unmapping diagnostics
- batch mapping that merges permissions when adjacent Vita segments share a 16 KiB iPhone host page
- loading and full byte readback of the first valid fixed-address plain Vita ELF in `Documents/Vita3K/imports`
- extraction of the Vita module name/NID and inventory of `PT_SCE_RELA` relocation payloads
- bounded application of Vita relocation formats 0–9 and common ARM/Thumb relocation codes, with verified writes to protected guest pages
- bounded parsing of long/short import records, export records, and their function/variable/TLS NID tables
- rewriting and byte-for-byte verification of parsed ARM function-import stubs using Vita3K's unresolved SVC trampoline layout
- a bounded ARM/Thumb interworking harness with register/PC state and a deliberately narrow instruction subset
- a reusable NID-to-HLE dispatcher seam, proven by one synthetic SVC call and verified return-register state
- validated extraction of `module_start` from Vita module metadata, including executable-range and temporary-stack checks
- a bounded single-thread runner implementing minimal `sceKernelGetThreadId` and `sceKernelExitThread` semantics
- a generated Thumb-entry module that observes its guest thread UID and exits with a verified status
- interworking diagnostics covering Thumb PUSH, literal/SP-relative loads and stores, MOV/MOVS, BLX into ARM SVC stubs, and `MOV pc,lr` back to Thumb
- a second fixture compiled in CI by the pinned official `vitasdk/vitasdk` image rather than hand-encoded by this repository
- preferred-address loading of its `ET_SCE_RELEXEC` VELF, including 10 verified relocations and real module/import table parsing
- bounded execution of its compiler-generated Thumb/Thumb-2 prologue, BLX import call, UXTB, and POP return sequence
- a portable `HostDisplay` seam with deterministic attach, acquire, and completion state
- an `MTKViewDelegate` that submits, clears, and presents one core-owned diagnostic frame through a Metal command queue
- renderer status that changes to `passed` only after the command buffer completes on device
- a portable input seam that normalizes UIKit touch positions and bounds controller state
- a GameController adapter for sticks, D-pad, face buttons, shoulders, and triggers
- Vita3K's upstream `packages/sfo.cpp`, refactored to build without Boost/fmt and hardened against malformed table offsets
- imported `param.sfo` reporting for title ID, title, category, and application version
- a narrow C++ `CoreBridge` seam for additional core integration
- an installed-game library, patch preference, diagnostics setting, and bounded Prepare Boot action
- SELF v3 segment-table probing with plain/compressed payload loading and an explicit encrypted-segment stop
- no signing credentials or provisioning profiles in CI

See [PORTING.md](PORTING.md) for the integration backlog and known blockers. See [docs/ios-development.md](../docs/ios-development.md) for the complete Windows-first workflow.

## Milestone 10 fixture test

The GitHub Actions artifact contains the unsigned IPA, the older generated Milestone 9 ELF, and two outputs built from the repository's tiny C program by the pinned official VitaSDK image: `milestone10-vitasdk-homebrew.velf` and `milestone10-vitasdk-homebrew.self`. None contains Sony firmware or game content.

1. Install the Milestone 10 IPA using your normal signing workflow.
2. In the iOS Files app, open **On My iPhone → Vita3K iOS → Vita3K → imports**.
3. Remove the older milestone fixture, then copy only `milestone10-vitasdk-homebrew.velf` into that folder.
4. Open Vita3K iOS and tap **Rescan Imports**.
5. Confirm the artifact reports `MAPPED`, `10 relocation entries/10 verified patches`, `1 function stub rebound`, `6 instructions`, `1 HLE call`, and `module_start returned 1`.

The `.self` is retained as evidence that the same build produced a normal Vita container, but SELF segment extraction remains probe-only; do not use it for this device test. VPK installation, firmware PUP installation, and commercial games also remain outside the executable path.

## Milestone 11 renderer test

1. Install and open the Milestone 11 IPA on a physical iPhone or iPad.
2. Confirm the app background changes to the core-owned blue Metal clear color.
3. Confirm the status reports `Renderer: passed (first core-owned Metal frame presented)`.
4. Optionally repeat the Milestone 10 VELF test; the loader diagnostic and renderer status should both remain visible.

This frame contains no Vita display output. It proves only that the portable core can own a frame request, UIKit can supply the drawable, and Metal can complete a clear/present command buffer on the device.

## Milestone 12 input test

1. Install and open the Milestone 12 IPA on a physical iPhone or iPad.
2. Wait for `Input: ready (tap the blue background; controller waiting)`.
3. Tap or drag on an empty portion of the blue background.
4. Confirm the line changes to `Input: passed` with a positive touch-sample count and normalized `x`/`y` coordinates between 0 and 1.
5. Optionally connect a compatible controller and move a stick or press a button; the controller sample count should increase.

The adapter does not yet implement Vita `SceCtrl` or touch HLE. It proves that real device input reaches a portable core-owned state object without depending on UIKit or GameController types outside the iOS frontend.

## Milestone 13 upstream metadata test

1. Install and open the Milestone 13 IPA.
2. Confirm `Upstream app metadata: passed (Vita3K packages/SFO parser linked)`.
3. Copy `milestone13-synthetic-param.sfo` from the Actions artifact into the app's `Vita3K/imports` folder.
4. Tap **Rescan Imports** and confirm `PARAM.SFO`, `title ID M13TEST01`, and `title Vita3K iOS upstream metadata probe` appear.

## Milestone 14 safe VPK inspection test

1. Install and open the Milestone 14 IPA once so its Files container is visible.
2. From the Actions artifact, copy `milestone14-synthetic-app.vpk` into **On My iPhone → Vita3K iOS → Vita3K → imports**.
3. Tap **Rescan Imports**.
4. Confirm the screen reports `Upstream app archive: passed`, `title ID M14TEST01`, and `planned target ux0/app/M14TEST01`.

This fixture is a tiny legal ZIP/VPK made by the smoke tests. The inspector reads only bounded metadata, inventories entries, and rejects absolute, traversal, backslash, or malformed archive paths. Milestone 14 does not extract the VPK, install it, decrypt `eboot.bin`, or execute it. You may inspect a backup you own, but keep large games out of Git and GitHub Actions.

## Milestone 15 Add Game and transactional installation test

1. Install and open the Milestone 15 IPA.
2. Tap **Add Game ZIP/VPK** and select `milestone15-app-and-patch.zip` from the Actions artifact.
3. Confirm the success alert reports two application roots and six files.
4. Confirm the diagnostic view reports `Installed titles: 1`, title ID `M15TEST01`, and `patch installed`.

The picker also accepts a user-owned ZIP with this layout:

```text
app/PCSG00291/sce_sys/param.sfo
app/PCSG00291/eboot.bin
app/PCSG00291/...
patch/PCSG00291/sce_sys/param.sfo
patch/PCSG00291/eboot.bin
patch/PCSG00291/...
```

Installation runs on a background queue. Every ZIP path is validated, files are decompressed into a private staging directory, sizes/CRC results are checked by miniz, an existing installation is backed up, and the staged app and patch are committed together. A failure rolls the previous installation back. Do not commit game files to this repository or upload them to Actions.
5. Optionally copy only `sce_sys/param.sfo` from a user-owned Amagami dump. It should report title ID `PCSG00291`.

This is application recognition, not installation or execution. Do not upload game dumps, firmware, keys, or extracted copyrighted assets to the public repository or Actions.

## Build contract

The iOS shell is selected before upstream desktop dependencies are configured:

```bash
cmake -S . -B build-ios -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DVITA3K_BUILD_IOS=ON \
  -DVITA3K_IOS_LINK_CORE=ON

cmake --build build-ios --config Release --target Vita3KiOS -- \
  -sdk iphoneos CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO
```

This command requires macOS and Xcode; the GitHub Actions workflow runs it remotely for Windows contributors.

`VITA3K_IOS_LINK_CORE=ON` is now required. The port can parse Vita application SFO metadata, transactionally install app/patch archives, reserve guest address space, map fixed-address `ET_SCE_EXEC` files and preferred-address `ET_SCE_RELEXEC` VELFs, inspect and load legal plain SELF segments, apply checked relocations, validate module metadata, inventory import/export NIDs, rewrite ARM function stubs, execute the narrow compiler path used by the Milestone 10 program, present one diagnostic Metal frame, and capture bounded host input. Protected SELF decryption, complete Vita VFS mounting, general relocatable placement, broader Thumb-2, thread scheduling, production HLE coverage, the Vita renderer, audio, and guest input services remain tracked in `PORTING.md`.

Milestone 10 preserves the synthetic tests and adds an independently compiled VitaSDK VELF. The loader maps that VELF at its preferred addresses, applies its real compact relocation stream, parses its genuine module/import tables, rebinds `sceKernelGetThreadId`, and completes `module_start` through six guest instructions. This is intentionally not advertised as a general ARMv7 interpreter: conditional execution, most 32-bit Thumb-2, most addressing modes, floating point/NEON, exceptions, atomics, and most data-processing instructions remain unsupported. Relocatable segments are not yet placed at alternative addresses if their preferred ranges are unavailable, and SELF containers remain probe-only.

Milestone 11 adds only the host-display boundary and the first Metal clear/present. It does not link the upstream Vulkan renderer, decode GXM commands, translate shaders, or display guest output.

Milestone 12 adds only the host-input boundary. UIKit touch and GameController samples are retained for diagnostics, but no Vita guest API can read them yet.

Milestone 13 is the first package-layer extraction from upstream: it compiles the real SFO parser into the iOS core and exposes application metadata.

Milestone 14 adds a reusable package archive inspector backed by upstream miniz and the SFO parser. It streams an archive from disk, rejects unsafe paths, inventories its entries, extracts only bounded `sce_sys/param.sfo` metadata in memory, and computes a planned sandbox target. It deliberately leaves transactional extraction, Vita VFS installation, content decryption, and executable loading for later milestones.

Milestone 15 adds the first end-user import UI and a transactional installer for base-app and patch roots. It creates an installed-title inventory under the private emulated Vita filesystem. It does not decrypt or execute the installed `eboot.bin`; title selection and SELF loader integration are the next milestone.

## Milestone 16 game-library and SELF preparation test

1. Install and open the Milestone 16 IPA. Existing Milestone 15 installations remain in the app container.
2. Open **Settings** and leave **Prefer Installed Patch** enabled.
3. Open **Game Library** and select an installed title such as `PCSG00291`.
4. The app resolves `ux0/patch/<TITLE_ID>/eboot.bin` first and falls back to the base app only when needed.
5. Capture the **Preparation Stopped** or **Executable Prepared** message and the `Executable preparation:` diagnostic line.

The Actions artifact includes `milestone16-app-and-patch.zip`, whose base and patch executables are legal synthetic plain SELF containers. Selecting its `M15TEST01` row must report `Executable Prepared`, two load segments, and module `synthetic-homebrew`. If a protected retail `eboot.bin` reports encrypted segments, no encrypted bytes are mapped or executed. Some user-owned decrypted dumps may already prepare successfully; that result enables the Milestone 17 controlled interpreter attempt.

Keep Amagami, firmware PUPs, keys, and other proprietary content on your device. Do not add them to Git or upload them to GitHub Actions.

## Milestone 17 controlled boot-boundary test

1. Open **Game Library** and select the installed title again so its executable is freshly prepared.
2. Tap **Attempt Boot (65536 Instructions)**.
3. Read the safety prompt, then choose **Run Once**.
4. Capture the **Boot Boundary Captured** alert and the `Controlled boot attempt:` line.

This is interpreter-only and does not enable JIT. The executable can be attempted only once per preparation, and the interpreter stops on the first unsupported instruction, memory fault, unimplemented HLE call, normal return/exit, or the 65536-instruction ceiling. At budget exhaustion it reports PC coverage, hot-loop evidence, recent PCs, registers, and lookahead. For a commercial game, a diagnostic stop is expected and is not a crash or a playable boot. Re-select the title before each later attempt so guest memory is reloaded cleanly.

## Milestone 18 lifecycle-entry resolution test

Milestone 17 exposed `0xF62F7FFF` at `0x810176B8` before executing an instruction from Amagami. Upstream Vita3K does not treat the module-info header's `module_start` field as final: the `NID_MODULE_START` lifecycle export can replace it after the export tables are loaded. Milestone 18 preserves export addresses and applies that same override before binding imports or enabling the bounded attempt.

1. Install the Milestone 18 IPA and open **Game Library**.
2. Select `PCSG00291` with **Prefer Installed Patch** enabled.
3. Confirm **Executable Prepared** includes `module_start ... via lifecycle export`.
4. Tap **Attempt Boot (65536 Instructions)** and choose **Run Once**.
5. Share the new diagnostic text or screenshot. A new unsupported instruction or HLE boundary is expected; a playable frame is not.

The Actions artifact also includes `milestone18-lifecycle-export.zip`. Its module header deliberately points at the wrong executable location while its lifecycle export points at a legal synthetic Thumb program. Installing, preparing, and attempting `M15TEST01` from that archive must complete 12 instructions, two HLE calls, and exit with status 42. This fixture contains no game or firmware data.

## Milestone 19 wide Thumb-2 stack-prologue test

The accepted Milestone 18 Amagami attempt entered the lifecycle export at `0x810176B9`, completed the first branch, and reached a wide Thumb-2 stack prologue beginning with `0xE92D` at `0x81017580`. Milestone 19 implements the `PUSH.W` alias of `STMDB sp!` for valid register lists, including high registers, and reports both halfwords for any later unsupported Thumb-2 instruction.

1. Install the Milestone 19 IPA and open **Game Library**.
2. Select `PCSG00291` with **Prefer Installed Patch** enabled and confirm preparation still says `via lifecycle export`.
3. Tap **Attempt Boot (65536 Instructions)** and choose **Run Once**.
4. Share the complete new diagnostic. It should advance past `0xE92D` at `0x81017580`; another CPU, memory, or HLE boundary is expected.

The Actions artifact includes `milestone19-thumb2-wide-push.zip`. Installing and attempting its synthetic `M15TEST01` title must complete 12 instructions and two HLE calls, including `PUSH.W {r8, lr}`, then exit with status 42. This fixture contains no game or firmware data.

## Milestone 20 batched Thumb compiler-baseline test

The accepted Milestone 19 Amagami attempt executed the wide stack save and stopped on `0xB082` at `0x81017584`, which is `SUB sp, #8`. Milestone 20 implements that stack adjustment together with coherent Thumb compiler families instead of advancing one opcode per build: immediate/register arithmetic, shifts, CPSR `N/Z/C/V` updates, data-processing operations, high-register moves/adds/compares, byte/halfword/word memory access, signed loads, address generation, compare-and-branch, multiple-register transfers, and conditional/unconditional branches. Unknown instructions still stop safely; the diagnostic now includes six following halfwords so the next batch can be selected from one device run.

1. Install the Milestone 20 IPA and open **Game Library**.
2. Select `PCSG00291` with **Prefer Installed Patch** enabled and confirm `via lifecycle export`.
3. Tap **Attempt Boot (65536 Instructions)** and choose **Run Once**.
4. Share the complete new diagnostic, including its `Lookahead:` values. It should advance past `0xB082` at `0x81017584`; an unsupported CPU instruction, memory fault, unbound HLE NID, or the instruction ceiling is expected.

The Actions artifact includes `milestone20-thumb-compiler-baseline.zip`. Installing and attempting its synthetic `M15TEST01` title must complete 13 instructions and two HLE calls, including wide `PUSH.W` and `SUB sp, #8`, then exit with status 42. Portable self-tests additionally validate the other batched instruction families and exact CPU/memory state.

## Milestone 21 batched Thumb-2 constant and doubleword-memory test

The accepted Milestone 20 Amagami attempt advanced through the stack allocation and captured a coherent compiler sequence: `F247 6290` (`MOVW r2, #0x7690`), `E9CD 1000` (`STRD r1, r0, [sp]`), and `F2C8 122C` (`MOVT r2, #0x812c`). Milestone 21 implements the complete Thumb-2 immediate `MOVW`/`MOVT` family plus the immediate `LDRD`/`STRD` offset, pre-indexed, post-indexed, and writeback forms. Invalid register combinations still stop without fabricating state, and later unknown instructions retain the bounded six-halfword look-ahead.

1. Install the Milestone 21 IPA and open **Game Library**.
2. Select `PCSG00291` with **Prefer Installed Patch** enabled and confirm `via lifecycle export`.
3. Tap **Attempt Boot (65536 Instructions)** and choose **Run Once**.
4. Share the complete new diagnostic, including `Lookahead:` if present. It should advance past the three captured 32-bit instructions at `0x81017586` through `0x81017592`; the next honest CPU, memory, HLE, return, or instruction-limit boundary is expected.

The Actions artifact includes `milestone21-thumb2-compiler-batch.zip`. Its synthetic `M15TEST01` runs the exact captured `MOVW`/`STRD`/`MOVT` opcodes after the existing wide prologue, reaches the exit-thread import, and completes nine instructions with one HLE call and exit status 42. Portable CPU-state tests additionally cover `LDRD`, pre-indexed store writeback, and post-indexed load writeback.

## Milestone 22 inline HLE identity and call-context test

The accepted Milestone 21 Amagami attempt executed ten instructions and reached the first imported HLE trampoline, but reported NID `0x00000000`. The import binder stores each real NID after the canonical ARM `SVC; MOV pc,lr` pair; the interpreter previously trusted that inline word only when an HLE handler was already registered, so an unimplemented import incorrectly fell back to zero in `r12`. Milestone 22 validates the canonical trampoline independently of handler availability and reports the real inline NID, upstream NID name, SVC address, LR return address, arguments `r0`–`r3`, original `r12`, and whether the NID appears in the module import inventory. It still stops before executing unknown HLE behavior.

1. Install the Milestone 22 IPA and open **Game Library**.
2. Select `PCSG00291`, confirm `via lifecycle export`, and run **Attempt Boot (65536 Instructions)** once.
3. Share the entire **Boot Boundary Captured** message or background diagnostic.
4. The result should no longer say only NID `0x00000000`; return the real NID, function name, SVC/LR addresses, and all four argument values so the next milestone can implement the correct HLE family.

The Actions artifact includes `milestone22-inline-hle-diagnostic.zip`. Its legal synthetic title deliberately enters an unimplemented import trampoline with `r12=0`; acceptance requires NID `0x210C0046`, name `__sceAppMgrGetAppState`, a matching module-import inventory entry, and the exact captured argument registers after nine instructions and zero dispatched HLE calls.

## Milestone 23 first stateful libc runtime HLE

The accepted Milestone 22 Amagami attempt identified the first real call as `__cxa_set_dso_handle_main` (NID `0xBFE02B3A`) with `r0=0x812C7690`. Upstream Vita3K implements this function by storing the guest DSO handle in kernel state and returning `void`. Milestone 23 adds that exact stateful behavior to the bounded iOS runtime, returns zero through the import ABI, preserves the last successfully dispatched NID, and resumes guest execution instead of stopping at the known call. No other libc behavior is inferred.

1. Install the Milestone 23 IPA and open **Game Library**.
2. Select `PCSG00291`, keep **Prefer Installed Patch** enabled, and confirm `via lifecycle export`.
3. Tap **Attempt Boot (65536 Instructions)** and choose **Run Once**.
4. Share the entire next alert or background diagnostic. A later CPU instruction, memory fault, unbound HLE, module return, or instruction ceiling is expected; the previous `0xBFE02B3A` boundary should now count as one dispatched HLE call and execution should continue past it.

The Actions artifact includes `milestone23-libc-dso-runtime.zip`. Its legal synthetic title imports `__cxa_set_dso_handle_main`, passes handle `0x81000200`, dispatches exactly one HLE call, records the handle, and returns through the module's zero-link sentinel after seven instructions. See `ios/VION-ANALYSIS.md` for the verified Vion architecture comparison and the larger full-core migration path.

## Milestone 24 broad Thumb-2 runtime-family batch

The accepted Milestone 23 Amagami attempt crossed the first stateful libc HLE, executed 16 instructions and one HLE call, then stopped at `F05F 0B00` (`MOVS.W r11, #0`) at `0x810175BE`. Its look-ahead immediately exposed `F8DD A000` (`LDR.W r10, [sp]`). Milestone 24 implements both complete compiler-facing families rather than only those two encodings:

- Thumb-2 modified-immediate `AND/BIC/ORR/ORN/EOR`, `MOV/MVN`, `ADD/ADC/SBC/SUB/RSB`, and `TST/TEQ/CMN/CMP`, including architectural immediate replication/rotation and exact `N/Z/C/V` updates.
- Thumb-2 byte, halfword, signed-byte, signed-halfword, and word loads/stores using imm12 offsets and checked imm8 pre-index, post-index, add/subtract, and writeback forms.

Unpredictable register combinations, invalid replicated-zero encodings, writeback aliasing, address overflow, and guest-memory faults remain hard stops.

1. Install the Milestone 24 IPA and open **Game Library**.
2. Select `PCSG00291`, keep **Prefer Installed Patch** enabled, and confirm `via lifecycle export`.
3. Run **Attempt Boot (65536 Instructions)** once.
4. Share the complete next boundary. It must advance beyond both `0x810175BE` and `0x810175C2`, retain one successful libc HLE call and the DSO handle, then stop honestly at the next unsupported CPU, memory, HLE, return, or budget boundary.

The Actions artifact includes `milestone24-thumb2-runtime-families.zip`. Its legal synthetic title dispatches the Milestone 23 libc HLE, executes the exact captured `MOVS.W` and `LDR.W`, and returns after nine instructions. Portable tests cover the wider ALU, flags, signed/unsigned memory, indexed writeback, invalid-encoding, and overflow behavior.

## Milestone 25 broad register-operated runtime batch

The accepted Milestone 24 Amagami attempt advanced from 16 to 28 instructions with the libc HLE and DSO handle intact, then stopped at `EBAA 0202` (`SUB.W r2, r10, r2`) at `0x810175EA`. Milestone 25 implements the compiler-facing Thumb-2 shifted-register logical/arithmetic/test family plus its `MOV/MVN`, immediate-shift, rotate, and `RRX` aliases. The same decoder applies architectural shift carry and arithmetic `N/Z/C/V` rules. The unrelated `PKH` encoding and hazardous SP/PC combinations remain explicit stops.

This milestone also adds scaled register-offset byte, halfword, signed-byte, signed-halfword, and word loads/stores. Reserved suffixes, PC offset registers, address overflow, and guest-memory faults remain hard boundaries. Unsupported-instruction look-ahead expands from six to sixteen halfwords so each physical-device result can guide a larger subsequent batch.

1. Install the Milestone 25 IPA and select `PCSG00291` with **Prefer Installed Patch** enabled.
2. Confirm preparation still reports `module_start 0x810176B9 via lifecycle export`.
3. Run **Attempt Boot (65536 Instructions)** once.
4. Share the complete next boundary and all sixteen look-ahead halfwords. Execution must advance through `SUB.W` at `0x810175EA`, preserve the libc HLE/DSO state, and then stop honestly at the next CPU, memory, HLE, return, or instruction-ceiling boundary.

The Actions artifact includes `milestone25-thumb2-register-families.zip`. Its legal synthetic title crosses the libc call and Milestone 24 sequence, executes the exact captured `SUB.W`, and returns after ten instructions. Portable tests additionally cover shifted flags, rotate/`RRX`, arithmetic carry/overflow behavior, scaled register memory, sign extension, invalid encodings, and overflow.

## Milestone 26 libc termination-registration cluster

The accepted Milestone 25 Amagami attempt advanced from 28 to 72 instructions with the DSO handle intact, then reached `__aeabi_atexit` (NID `0xEDC939E1`) through the canonical inline import trampoline. The captured call passed object `0x810DFB24`, Thumb destructor `0x8107CBE5`, and DSO handle `0x812C7690` in `r0`-`r2`.

Milestone 26 binds the complete closely related `__aeabi_atexit`, `__cxa_atexit`, and `__cxa_finalize` trio whenever the title imports them. Both registration ABIs are normalized into object/destructor/DSO records, return zero, and preserve the latest values in the boot diagnostic. Finalization records its DSO request but deliberately does not invoke guest destructor callbacks: upstream Vita3K's current handlers are zero-return compatibility stubs, and this bounded runner cannot safely re-enter arbitrary guest callbacks from HLE yet.

1. Install the Milestone 26 IPA and select `PCSG00291` with **Prefer Installed Patch** enabled.
2. Confirm preparation reports `module_start 0x810176B9 via lifecycle export`.
3. Run **Attempt Boot (65536 Instructions)** once.
4. Share the complete next boundary. It should exceed 72 instructions, report two successful HLE calls and `Libc termination registrations=1` with the three captured values, then stop honestly at the next CPU, memory, HLE, return, or instruction-ceiling boundary.

The Actions artifact includes `milestone26-libc-termination.zip`. Its legal synthetic title dispatches `__aeabi_atexit`, records object `0x81000240`, destructor `0x81000301`, and DSO `0x81000200`, then returns after nine instructions. Portable regressions separately exercise the `__cxa_atexit` argument order and `__cxa_finalize` diagnostic state.

## Milestone 27 Thumb-2 multiple-transfer family

The accepted Milestone 26 Amagami attempt advanced from 72 to 106 instructions, completed four HLE calls and three libc termination registrations, then stopped at `E8BD 81F0` (`POP.W {r4-r8, pc}`) at `0x81001CC8`. This is the `LDMIA sp!` function-epilogue alias.

Milestone 27 replaces the earlier one-off wide-push decoder with the complete safe Thumb-2 increment-after/decrement-before load/store-multiple family. It supports high registers, optional writeback, `PUSH.W`/`POP.W`, and PC loads with ARM/Thumb interworking or the diagnostic zero-link return. Empty or one-register lists, SP in the list, store-PC, simultaneous LR/PC loads, writeback/base aliasing, overflow/underflow, and unmapped ranges remain explicit boundaries. Each contiguous transfer is performed as one checked memory operation so a failed range does not partially update registers or memory.

1. Install the Milestone 27 IPA and select `PCSG00291` with **Prefer Installed Patch** enabled.
2. Confirm preparation still reports `module_start 0x810176B9 via lifecycle export`.
3. Run **Attempt Boot (65536 Instructions)** once.
4. Share the complete next boundary and all look-ahead halfwords. It should advance beyond 106 instructions while retaining four HLE calls, DSO `0x812C7690`, and three termination registrations.

The Actions artifact includes `milestone27-thumb2-multiple-transfer.zip`. Its legal synthetic title executes `PUSH.W {r4-r8, lr}` followed by Amagami's exact `POP.W {r4-r8, pc}` and returns after two instructions. Portable tests additionally cover all four transfer modes, writeback, high registers, PC interworking, zero-link return, invalid lists, range overflow, and guest faults.

## Milestone 28 C++ static-initialization guards

The accepted Milestone 27 Amagami attempt advanced to 122 instructions and four HLE calls, then reached `__cxa_guard_acquire` (NID `0xD0310E31`) for guard word `0x812C75D8`. Milestone 28 binds the complete 32-bit Arm `__cxa_guard_acquire`, `__cxa_guard_release`, and `__cxa_guard_abort` protocol. Acquire reads the aligned 4-byte guest word and returns one only to the initializer owner; release preserves the word while setting initialized bit zero; abort releases ownership without claiming initialization. The bounded single-thread runtime tracks ownership explicitly and stops on recursive acquisition, invalid alignment, or inaccessible guest memory instead of fabricating progress. Cross-thread semaphore scheduling remains outside this runner.

1. Install the Milestone 28 IPA and select `PCSG00291` with **Prefer Installed Patch** enabled.
2. Confirm preparation reports `module_start 0x810176B9 via lifecycle export`.
3. Run **Attempt Boot (65536 Instructions)** once.
4. Share the complete next boundary. It should exceed 122 instructions, report at least five successful HLE calls, preserve the DSO and termination-registration state, and include a `C++ guards:` summary.

The Actions artifact includes `milestone28-cxa-guards.zip`. Its legal synthetic title exercises first acquisition and returns one. Portable regressions also validate the already-initialized zero result, release bit-setting, abort behavior, and an honest recursive-initialization boundary.

## Milestone 29 bounded libc guest heap

The accepted Milestone 28 Amagami attempt advanced from 122 to 133 instructions and from four to five HLE calls. Its C++ guard acquired successfully, then startup called `memalign` (NID `0xA9363E6B`) with alignment `0x10` and size `0x180`.

Milestone 29 adds a host-page-aligned 16 MiB guest heap arena at `0x90000000` and binds the coherent `calloc`, `malloc`, `memalign`, `free`, `realloc`, and `malloc_usable_size` family when imported. The allocator splits and coalesces blocks, reuses freed ranges, preserves bytes across growth, zeroes `calloc` storage, validates power-of-two alignment, tracks live/peak usage, and stops explicitly on invalid pointers, artificial arena exhaustion, mapping failure, or arithmetic overflow. It is a bounded single-thread startup heap, not yet upstream Vita3K's process-wide page allocator.

1. Install the Milestone 29 IPA and select `PCSG00291` with **Prefer Installed Patch** enabled.
2. Confirm preparation reports `module_start 0x810176B9 via lifecycle export`.
3. Run **Attempt Boot (65536 Instructions)** once.
4. Share the complete next boundary. It should exceed 133 instructions, report at least six successful HLE calls, preserve the DSO/termination/guard state, and include `Libc heap: allocations=1` with address `0x90000000`, size `384`, and alignment `16`.

The Actions artifact includes `milestone29-libc-heap.zip`. Its legal synthetic title reproduces `memalign(0x10, 0x180)`, receives aligned writable guest address `0x90000000`, and returns cleanly. Portable regressions cover alignment, resize preservation, usable size, free/coalescing, zeroed reuse, and invalid alignment.

## Milestone 30 LR shifted-register execution runway

The accepted Milestone 29 device run proved the first real heap allocation, advanced to 138 instructions and six HLE calls, then stopped at `EB00 00CE`: `ADD.W r0, r0, lr, LSL #3`. The shifted-register family was already implemented, but its architectural register validation incorrectly classified `lr` as a bad general register. Milestone 30 permits `lr` as source, shifted source, and destination across the complete logical/arithmetic/test family while continuing to reject `sp` and `pc` outside their defined aliases. Portable tests cover all three legal LR positions plus the preserved invalid cases.

The physical-device ceiling is also raised from 256 to 4096 interpreted instructions. This remains a one-shot, non-JIT, stop-on-first-unknown diagnostic; it only reduces the chance that a long run of already-supported startup code ends at an artificial budget boundary.

1. Install the Milestone 30 IPA and select `PCSG00291` with **Prefer Installed Patch** enabled.
2. Confirm preparation reports `module_start 0x810176B9 via lifecycle export`.
3. Run **Attempt Boot (65536 Instructions)** once.
4. Share the complete next boundary. It should exceed 138 instructions and retain six HLE calls, the DSO/termination/guard summaries, and the successful 384-byte heap allocation.

The Actions artifact includes `milestone30-thumb2-lr-shifted-register.zip`. Its legal synthetic title executes the exact captured wide add using `lr`, returns 44, and preserves explicit rejection of invalid `sp`/`pc` encodings.

## Milestone 31 instrumented execution runway

The accepted Milestone 30 device run did not encounter another unsupported instruction or HLE boundary. It consumed all 4096 instructions, completed 14 HLE calls, registered five termination callbacks, completed three guard acquisitions/two releases, and made three successful allocations totaling 11904 live bytes. This proves substantially more startup code is already supported, but the old boundary could not distinguish continued progress from a guest spin loop.

Milestone 31 raises the one-shot ceiling to 65536 instructions and instruments every bounded run with unique-PC count, hottest PC and hit count, non-forward transfer count, the eight most recent PCs, all general registers/CPSR, and Thumb lookahead. A concentrated PC set is labeled only as a **hot-loop candidate**; it is not skipped or treated as successful. This tells the next milestone whether to add CPU/HLE coverage or begin the required scheduling/synchronization seam.

1. Install the Milestone 31 IPA and select `PCSG00291` with **Prefer Installed Patch** enabled.
2. Confirm preparation reports `module_start 0x810176B9 via lifecycle export`.
3. Run **Attempt Boot (65536 Instructions)** once.
4. Share the entire boundary, especially `execution progress`, `Recent PCs`, `Registers`, and `Lookahead`.

The Actions artifact includes `milestone31-execution-progress.zip`. Its legal synthetic title deliberately executes `B .`; portable tests prove the diagnostic reports one unique/hottest PC, 256 hits, 256 non-forward transfers, and a hot-loop candidate without fabricating forward progress.

## Milestone 32 C++ allocation ABI

The accepted Milestone 31 Amagami run proved startup was still advancing: it crossed the old 4096-instruction ceiling, executed 5637 instructions and 30 HLE calls, completed six static initializers, and then reached `_Znwj` (32-bit `operator new`, NID `0xF99ED5AC`) with size 8. This was an unbound runtime import, not a hot loop or CPU failure.

Milestone 32 binds the complete ten-export 32-bit C++ allocation family from the Vita NID database: scalar and array `new`, their nothrow overloads, scalar and array `delete`, matching nothrow deletes, and placement-delete cleanup overloads. Successful allocations use the existing checked guest heap; regular deletion validates and releases a live guest block; null deletion and placement deletion are ABI-correct no-ops. Nothrow exhaustion returns null and records a failure. A throwing allocation failure remains an explicit boundary because guest exception/new-handler unwinding is not implemented.

1. Install the Milestone 32 IPA and select `PCSG00291` with **Prefer Installed Patch** enabled.
2. Confirm preparation still reports `module_start 0x810176B9 via lifecycle export`.
3. Run **Attempt Boot (65536 Instructions)** once.
4. Share the entire next boundary. It should exceed 5637 instructions and 30 HLE calls, and the detail should include `C++ allocation ABI: new=1` unless later startup performs more allocation calls first.

The Actions artifact includes `milestone32-cxx-allocation.zip`. Its legal synthetic title calls the exact captured `_Znwj(8)` import and returns writable guest address `0x90000000`. Portable regressions cover all ten exports, both successful nothrow variants, null and placement deletion, nothrow exhaustion, and the explicit throwing-allocation boundary.

## Milestone 33 AppUtil lifecycle

The accepted Milestone 32 run completed `_Znwj(8)` exactly: heap allocations rose to seven, live bytes rose to 12744, and the new C++ ABI diagnostic reported one successful scalar allocation. Amagami then advanced to 5676 instructions and 31 HLE calls before reaching `sceAppUtilInit` (NID `0xDAFFE671`) with non-null guest parameters at `0x812C82B8` and `0x812C828C`.

Milestone 33 implements a bounded AppUtil lifecycle rather than returning a blind success. It validates the VitaSDK-defined `0x40`-byte `SceAppUtilInitParam` and `0x28`-byte `SceAppUtilBootParam` ranges, enforces zeroed reserved fields, records work-buffer size and boot metadata, tracks initialized state, and pairs initialization with `sceAppUtilShutdown`. Null parameters and invalid lifecycle state return documented AppUtil errors; an unmapped non-null guest range remains an explicit memory boundary. Upstream Vita3K currently leaves both lifecycle exports unimplemented, so this narrow behavior is isolated to the diagnostic iOS runner. Savedata, app events, system parameters, and mount services remain unbound until reached and implemented with real backing state.

1. Install the Milestone 33 IPA and select `PCSG00291` with **Prefer Installed Patch** enabled.
2. Confirm preparation still reports `module_start 0x810176B9 via lifecycle export`.
3. Run **Attempt Boot (65536 Instructions)** once.
4. Share the entire next boundary. It should exceed 5676 instructions and 31 HLE calls and include `AppUtil lifecycle: initialized=yes, init calls=1` unless startup later shuts it down.

The Actions artifact includes `milestone33-app-util-lifecycle.zip`. Its legal synthetic title initializes AppUtil with correctly sized zeroed guest structures and returns cleanly. Portable companions cover null parameters, unmapped pointers, shutdown-before-init, and a complete init/shutdown sequence with three rebound imports.

## Milestone 34 public Sysmodule lifecycle

The accepted Milestone 33 device run initialized AppUtil successfully and advanced to 5683 instructions and 32 HLE calls before reaching `sceSysmoduleLoadModule(0x15)` (NID `0x79A0160A`). Module `0x15` is `SCE_SYSMODULE_NP`. Milestone 34 batches the complete public lifecycle family: `sceSysmoduleLoadModule`, `sceSysmoduleIsLoaded`, and `sceSysmoduleUnloadModule`. The bounded runner records loaded IDs per boot, preserves idempotent loads, reports unloaded and invalid-ID errors with upstream codes, and exposes call/state diagnostics. In the upstream-compatible HLE path, loading records availability without claiming that proprietary firmware code executed; the first NP service call remains an honest boundary.

1. Install the Milestone 34 IPA and select `PCSG00291` with **Prefer Installed Patch** enabled.
2. Confirm preparation still reports `module_start 0x810176B9 via lifecycle export`.
3. Run **Attempt Boot (65536 Instructions)** once.
4. Share the complete next boundary and the `Sysmodule lifecycle:` diagnostic.

The Actions artifact includes `milestone34-sysmodule-lifecycle.zip`. Its legal synthetic title proves load, loaded-status, unload, and unloaded-status ordering in one run. Portable companions cover unloaded status and invalid module IDs.
