# Vita3K iOS bootstrap

This directory is an experimental, device-only iOS application for a future Vita3K port. It builds an unsigned IPA containing a UIKit/Metal host, file-backed logging, sandbox storage, and the first cross-compiled slices of upstream Vita3K code. It now loads and completes the `module_start` of a deliberately tiny program built by the real VitaSDK toolchain, clears and presents one core-owned Metal diagnostic frame, captures normalized UIKit/GameController input, and uses Vita3K's real package/SFO parser to identify application metadata. It does **not** yet install VPKs, extract commercial SELF payloads, execute general Vita homebrew, render Vita graphics, route input to guest services, or run games.

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

`VITA3K_IOS_LINK_CORE=ON` is now required. The port can parse Vita application SFO metadata using upstream code, reserve guest address space, map fixed-address `ET_SCE_EXEC` files and preferred-address `ET_SCE_RELEXEC` VELFs, apply checked relocations, validate module metadata, inventory import/export NIDs, rewrite ARM function stubs, execute the narrow compiler path used by the Milestone 10 program, present one diagnostic Metal frame, and capture bounded host input. VPK installation, Vita VFS mounting, general relocatable placement, SELF payload extraction, broader Thumb-2, thread scheduling, production HLE coverage, the Vita renderer, audio, and guest input services remain tracked in `PORTING.md`.

Milestone 10 preserves the synthetic tests and adds an independently compiled VitaSDK VELF. The loader maps that VELF at its preferred addresses, applies its real compact relocation stream, parses its genuine module/import tables, rebinds `sceKernelGetThreadId`, and completes `module_start` through six guest instructions. This is intentionally not advertised as a general ARMv7 interpreter: conditional execution, most 32-bit Thumb-2, most addressing modes, floating point/NEON, exceptions, atomics, and most data-processing instructions remain unsupported. Relocatable segments are not yet placed at alternative addresses if their preferred ranges are unavailable, and SELF containers remain probe-only.

Milestone 11 adds only the host-display boundary and the first Metal clear/present. It does not link the upstream Vulkan renderer, decode GXM commands, translate shaders, or display guest output.

Milestone 12 adds only the host-input boundary. UIKit touch and GameController samples are retained for diagnostics, but no Vita guest API can read them yet.

Milestone 13 is the first package-layer extraction from upstream: it compiles the real SFO parser into the iOS core and exposes application metadata.

Milestone 14 adds a reusable package archive inspector backed by upstream miniz and the SFO parser. It streams an archive from disk, rejects unsafe paths, inventories its entries, extracts only bounded `sce_sys/param.sfo` metadata in memory, and computes a planned sandbox target. It deliberately leaves transactional extraction, Vita VFS installation, content decryption, and executable loading for later milestones.

Milestone 15 adds the first end-user import UI and a transactional installer for base-app and patch roots. It creates an installed-title inventory under the private emulated Vita filesystem. It does not decrypt or execute the installed `eboot.bin`; title selection and SELF loader integration are the next milestone.
