# Vita3K iOS bootstrap

This directory is an experimental, device-only iOS application for a future Vita3K port. It builds an unsigned IPA containing a UIKit/Metal host, file-backed logging, sandbox storage, and the first cross-compiled slice of upstream Vita3K code. It now loads and completes the `module_start` of a deliberately tiny program built by the real VitaSDK toolchain, then clears and presents one core-owned Metal diagnostic frame. It does **not** yet execute general Vita homebrew, render Vita graphics, or run games.

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

`VITA3K_IOS_LINK_CORE=ON` is now required. The port can reserve guest address space, map fixed-address `ET_SCE_EXEC` files and preferred-address `ET_SCE_RELEXEC` VELFs, apply checked relocations, validate module metadata, inventory import/export NIDs, rewrite ARM function stubs, execute the narrow compiler path used by the Milestone 10 program, and present one diagnostic Metal frame. General relocatable placement, SELF payload extraction, broader Thumb-2, thread scheduling, production HLE coverage, the Vita renderer, audio, and input remain tracked in `PORTING.md`.

Milestone 10 preserves the synthetic tests and adds an independently compiled VitaSDK VELF. The loader maps that VELF at its preferred addresses, applies its real compact relocation stream, parses its genuine module/import tables, rebinds `sceKernelGetThreadId`, and completes `module_start` through six guest instructions. This is intentionally not advertised as a general ARMv7 interpreter: conditional execution, most 32-bit Thumb-2, most addressing modes, floating point/NEON, exceptions, atomics, and most data-processing instructions remain unsupported. Relocatable segments are not yet placed at alternative addresses if their preferred ranges are unavailable, and SELF containers remain probe-only.

Milestone 11 adds only the host-display boundary and the first Metal clear/present. It does not link the upstream Vulkan renderer, decode GXM commands, translate shaders, or display guest output.
