# Vita3K iOS bootstrap

This directory is an experimental, device-only iOS application for a future Vita3K port. It builds an unsigned IPA containing a UIKit/Metal host, file-backed logging, sandbox storage, and the first cross-compiled slice of upstream Vita3K code. It now loads and runs the `module_start` of a deliberately tiny compiled-style Vita ELF inside a bounded guest-thread context, but it does **not** yet execute ordinary VitaSDK homebrew or games.

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
- a narrow C++ `CoreBridge` seam for additional core integration
- no signing credentials or provisioning profiles in CI

See [PORTING.md](PORTING.md) for the integration backlog and known blockers. See [docs/ios-development.md](../docs/ios-development.md) for the complete Windows-first workflow.

## Milestone 9 fixture test

The GitHub Actions artifact contains both the unsigned IPA and `milestone9-thumb-homebrew.elf`. The ELF is generated entirely by this repository and contains no Sony firmware or game content.

1. Install the Milestone 9 IPA using your normal signing workflow.
2. In the iOS Files app, open **On My iPhone → Vita3K iOS → Vita3K → imports**.
3. Remove the older milestone fixture, then copy `milestone9-thumb-homebrew.elf` into that folder.
4. Open Vita3K iOS and tap **Rescan Imports**.
5. Confirm the artifact reports `MAPPED`, `2 function stubs rebound`, `12 instructions`, `2 HLE calls`, and `sceKernelExitThread(42) completed`.

Do not use a VPK, SELF/`eboot.bin`, firmware PUP, or commercial game for this test. Those formats remain outside the executable path. The official VitaSDK [`basic_program`](https://github.com/vitasdk/samples/tree/master/basic_program) sample is the intended next real-homebrew target, after relocatable ELF/SELF handling and additional instruction/HLE coverage are implemented.

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

`VITA3K_IOS_LINK_CORE=ON` is now required. The port can reserve guest address space, map a fixed-address plain `ET_SCE_EXEC` file, apply checked relocations, validate its module-info header, inventory import/export NIDs, rewrite ARM function stubs, and execute a Thumb-entry synthetic `module_start` through a single guest-thread context. SELF decoding, relocatable ELF rebasing, general Thumb-2, thread scheduling, broader production HLE coverage, the full CPU backend, renderer, audio, and input remain tracked in `PORTING.md`.

Milestone 9 rewrites parsed imported-function stubs to Vita3K's ARM `SVC; MOV pc,lr; NID` layout. Its Thumb-entry acceptance routine reaches those stubs with `BLX`, returns to Thumb through the low bit in LR, and round-trips the returned thread UID through guest stack memory before exiting. This is intentionally not advertised as a general ARMv7 interpreter: conditional execution, 32-bit Thumb-2, most addressing modes, floating point/NEON, exceptions, atomics, and most data-processing instructions remain unsupported. Only the first valid fixed-address plain ELF is retained in guest memory; relocatable ELF and SELF containers remain probe-only.
