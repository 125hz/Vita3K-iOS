# Vita3K iOS bootstrap

This directory is an experimental, device-only iOS application for a future Vita3K port. It builds an unsigned IPA containing a UIKit/Metal host, file-backed logging, sandbox storage, and the first cross-compiled slices of upstream Vita3K code. It can transactionally install user-selected app/patch ZIPs, list installed titles, prefer a patch `eboot.bin`, safely prepare a selected Vita SELF, and make one explicitly confirmed interpreter attempt with a hard 256-instruction ceiling. It also loads and completes the `module_start` of a deliberately tiny legal fixture, executes a tested baseline of compiler-generated Thumb arithmetic, memory, stack, and branch instructions, presents one core-owned Metal diagnostic frame, and captures normalized UIKit/GameController input. It does **not** yet execute general installed games, render Vita graphics, route input to guest services, or provide a production CPU/HLE implementation.

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
2. Tap **Attempt Boot (256 Instructions)**.
3. Read the safety prompt, then choose **Run Once**.
4. Capture the **Boot Boundary Captured** alert and the `Controlled boot attempt:` line.

This is interpreter-only and does not enable JIT. The executable can be attempted only once per preparation, and the interpreter stops on the first unsupported instruction, memory fault, unimplemented HLE call, normal return/exit, or the 256-instruction ceiling. For a commercial game, an early diagnostic stop is expected and is not a crash or a playable boot. Re-select the title before each later attempt so guest memory is reloaded cleanly.

## Milestone 18 lifecycle-entry resolution test

Milestone 17 exposed `0xF62F7FFF` at `0x810176B8` before executing an instruction from Amagami. Upstream Vita3K does not treat the module-info header's `module_start` field as final: the `NID_MODULE_START` lifecycle export can replace it after the export tables are loaded. Milestone 18 preserves export addresses and applies that same override before binding imports or enabling the bounded attempt.

1. Install the Milestone 18 IPA and open **Game Library**.
2. Select `PCSG00291` with **Prefer Installed Patch** enabled.
3. Confirm **Executable Prepared** includes `module_start ... via lifecycle export`.
4. Tap **Attempt Boot (256 Instructions)** and choose **Run Once**.
5. Share the new diagnostic text or screenshot. A new unsupported instruction or HLE boundary is expected; a playable frame is not.

The Actions artifact also includes `milestone18-lifecycle-export.zip`. Its module header deliberately points at the wrong executable location while its lifecycle export points at a legal synthetic Thumb program. Installing, preparing, and attempting `M15TEST01` from that archive must complete 12 instructions, two HLE calls, and exit with status 42. This fixture contains no game or firmware data.

## Milestone 19 wide Thumb-2 stack-prologue test

The accepted Milestone 18 Amagami attempt entered the lifecycle export at `0x810176B9`, completed the first branch, and reached a wide Thumb-2 stack prologue beginning with `0xE92D` at `0x81017580`. Milestone 19 implements the `PUSH.W` alias of `STMDB sp!` for valid register lists, including high registers, and reports both halfwords for any later unsupported Thumb-2 instruction.

1. Install the Milestone 19 IPA and open **Game Library**.
2. Select `PCSG00291` with **Prefer Installed Patch** enabled and confirm preparation still says `via lifecycle export`.
3. Tap **Attempt Boot (256 Instructions)** and choose **Run Once**.
4. Share the complete new diagnostic. It should advance past `0xE92D` at `0x81017580`; another CPU, memory, or HLE boundary is expected.

The Actions artifact includes `milestone19-thumb2-wide-push.zip`. Installing and attempting its synthetic `M15TEST01` title must complete 12 instructions and two HLE calls, including `PUSH.W {r8, lr}`, then exit with status 42. This fixture contains no game or firmware data.

## Milestone 20 batched Thumb compiler-baseline test

The accepted Milestone 19 Amagami attempt executed the wide stack save and stopped on `0xB082` at `0x81017584`, which is `SUB sp, #8`. Milestone 20 implements that stack adjustment together with coherent Thumb compiler families instead of advancing one opcode per build: immediate/register arithmetic, shifts, CPSR `N/Z/C/V` updates, data-processing operations, high-register moves/adds/compares, byte/halfword/word memory access, signed loads, address generation, compare-and-branch, multiple-register transfers, and conditional/unconditional branches. Unknown instructions still stop safely; the diagnostic now includes six following halfwords so the next batch can be selected from one device run.

1. Install the Milestone 20 IPA and open **Game Library**.
2. Select `PCSG00291` with **Prefer Installed Patch** enabled and confirm `via lifecycle export`.
3. Tap **Attempt Boot (256 Instructions)** and choose **Run Once**.
4. Share the complete new diagnostic, including its `Lookahead:` values. It should advance past `0xB082` at `0x81017584`; an unsupported CPU instruction, memory fault, unbound HLE NID, or the 256-instruction ceiling is expected.

The Actions artifact includes `milestone20-thumb-compiler-baseline.zip`. Installing and attempting its synthetic `M15TEST01` title must complete 13 instructions and two HLE calls, including wide `PUSH.W` and `SUB sp, #8`, then exit with status 42. Portable self-tests additionally validate the other batched instruction families and exact CPU/memory state.

## Milestone 21 batched Thumb-2 constant and doubleword-memory test

The accepted Milestone 20 Amagami attempt advanced through the stack allocation and captured a coherent compiler sequence: `F247 6290` (`MOVW r2, #0x7690`), `E9CD 1000` (`STRD r1, r0, [sp]`), and `F2C8 122C` (`MOVT r2, #0x812c`). Milestone 21 implements the complete Thumb-2 immediate `MOVW`/`MOVT` family plus the immediate `LDRD`/`STRD` offset, pre-indexed, post-indexed, and writeback forms. Invalid register combinations still stop without fabricating state, and later unknown instructions retain the bounded six-halfword look-ahead.

1. Install the Milestone 21 IPA and open **Game Library**.
2. Select `PCSG00291` with **Prefer Installed Patch** enabled and confirm `via lifecycle export`.
3. Tap **Attempt Boot (256 Instructions)** and choose **Run Once**.
4. Share the complete new diagnostic, including `Lookahead:` if present. It should advance past the three captured 32-bit instructions at `0x81017586` through `0x81017592`; the next honest CPU, memory, HLE, return, or instruction-limit boundary is expected.

The Actions artifact includes `milestone21-thumb2-compiler-batch.zip`. Its synthetic `M15TEST01` runs the exact captured `MOVW`/`STRD`/`MOVT` opcodes after the existing wide prologue, reaches the exit-thread import, and completes nine instructions with one HLE call and exit status 42. Portable CPU-state tests additionally cover `LDRD`, pre-indexed store writeback, and post-indexed load writeback.
