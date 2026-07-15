# Vita3K iOS continuation handoff

This is the authoritative continuation note for the Windows-first Vita3K iOS work in `https://github.com/125hz/Vita3K-iOS`, branch `ios-port`. The current completed and physical-device-accepted target is **Milestone 37**. No Milestone 38 implementation has started.

Read this file together with:

- `ios/MENU-BOOT-CHECKLIST.md` for the first guest-produced menu acceptance criteria.
- `ios/README.md` for device-test instructions and milestone history.
- `ios/PORTING.md` for upstream gaps and architectural constraints.
- `ios/VION-ANALYSIS.md` for lessons from another iOS emulator frontend.
- `docs/ios-development.md` for local and CI build operations.
- `.github/workflows/ios.yml` and `.ci/package-ios.sh` for the authoritative unsigned IPA path.

## Current stop point: Milestone 37 accepted

The user tested the Milestone 37 IPA on a physical iOS device with their locally owned Amagami dump:

- Title: `PCSG00291` / `エビコレ+ アマガミ`
- Selected executable: `patch/eboot.bin`
- Module: `PSVita_ProjectTest01`, NID `0xE8A676A4`
- Entry: `module_start 0x810176B9 via lifecycle export`
- Loader: two preferred-address relocatable SELF segments, 44,760/44,760 relocation patches verified, 211 function stubs rebound
- Progress: **5,735 guest instructions and 38 successful HLE calls**
- Milestone 37 controller result: sampling mode 2 accepted; no controller buffers were requested yet
- Preserved state: AppUtil initialized, NP and NP Trophy initialized offline, two sysmodules loaded, Trophy context 1 alive, 11 libc termination registrations, six completed C++ guards, and seven heap allocations / 12,744 live bytes

The first honest unimplemented boundary is now:

```text
NID:          0x1B9C5D14
Name:         sceTouchSetSamplingState
SVC:          0x8109B8E8
LR:           0x81013F85
Arguments:    r0=0x00000000 r1=0x00000001
Observed:     5735 instructions, 38 HLE calls
```

The signature is `sceTouchSetSamplingState(port, state)`. The captured values are consistent with enabling sampling on the front touch panel. Treat that interpretation as a working inference and verify it against upstream/VitaSDK definitions before implementing it. The extra register values shown by the diagnostic are not additional formal arguments.

Do not skip this call, return success from all unknown imports, or patch Amagami-specific addresses. When work resumes, the next bounded batch should be a coherent upstream-backed touch subsystem, not a one-off hardcoded result.

## What exists today

### Build, packaging, and CI

- The root build has an early `VITA3K_BUILD_IOS` gate so iPhoneOS configuration avoids the desktop Qt dependency graph.
- CMake builds a real arm64 `iphoneos` application target named `Vita3KiOS`.
- `.ci/package-ios.sh` stages `Payload/Vita3K-iOS.app`, validates the device binary and plist, rejects signing material, and creates `artifacts/Vita3K-iOS-unsigned.ipa`.
- `.github/workflows/ios.yml` is the only iOS workflow. It supports `workflow_dispatch`, branch/path-filtered pushes, and pull requests.
- GitHub Actions first builds a legal VitaSDK homebrew fixture in a pinned official Docker image on Ubuntu.
- A `macos-15` job runs all portable core tests, configures the Xcode iPhoneOS arm64 target, builds with `CODE_SIGNING_ALLOWED=NO` and `CODE_SIGNING_REQUIRED=NO`, packages the IPA, and uploads the IPA plus every legal synthetic milestone fixture.
- No Apple certificate, provisioning profile, game, firmware, key, or protected extracted content is stored in the repository or Actions secrets.
- Windows remains the primary editing and portable-test environment. The macOS Action is the compile truth for the Apple/iPhoneOS target.

### iOS host shell and storage

- The app has a sandboxed Vita directory layout, persistent settings, import rescanning, and an installed-title inventory.
- ZIP/VPK app and patch roots are inspected and installed transactionally.
- `sce_sys/param.sfo` metadata is parsed with upstream-compatible code.
- The game library can select a title and prefer `ux0/patch/<TITLE_ID>/eboot.bin` over the base executable.
- The current UIKit shell already contains a diagnostic home view, game-library table, settings view, import/install controls, and an `MTKView` host surface.
- The dark-blue screen is a deliberately temporary diagnostic presentation. Its Metal clear is host-owned and is **not** a Vita guest frame.

### Loader and execution foundation

- Plain or already-decrypted SELF load segments are mapped at checked guest addresses.
- Relocation tables, imports, exports, and function-stub rebinding are implemented and regression-tested.
- Export symbol addresses are preserved, and `NID_MODULE_START` (`0x935CD196`) overrides the module-info entry when present. Diagnostics must continue to say `via lifecycle export` for Amagami.
- A bounded interpreter crosses Thumb, Thumb-2, and ARM modes, recognizes canonical inline import trampolines, and reports the exact first unsupported CPU, memory, or HLE boundary.
- Unknown instructions and NIDs remain hard boundaries. The runner does not fabricate guest state to look farther ahead.
- The one-shot budget is 65,536 instructions and includes PC/register/loop telemetry. It remains a diagnostic runner, not a production process scheduler or JIT.

### Implemented CPU/runtime/HLE runway

The legal fixture history in CI records the following progression:

- Milestones 9-16: legal ARM/VitaSDK fixtures, SFO/archive inspection, transactional installation, SELF preparation, and installed-title selection.
- Milestone 18: correct lifecycle-export entry resolution.
- Milestones 19-21: wide Thumb-2 prologue, common Thumb compiler baseline, constants, and doubleword memory families.
- Milestone 22: exact inline import NID identity and call-context reporting.
- Milestones 23-30: libc DSO state, broad Thumb-2 runtime/register/multiple-transfer families, termination registration, C++ guards, a bounded heap, and correct LR shifted-register behavior.
- Milestone 31: expanded execution telemetry proving startup advanced beyond the earlier 4,096-instruction ceiling rather than spinning.
- Milestone 32: C++ scalar/array allocation and deletion ABI coverage.
- Milestone 33: validated AppUtil initialization/shutdown state.
- Milestone 34: public sysmodule load/status/unload bookkeeping.
- Milestone 35: offline NP Manager and NP Trophy startup lifecycle.
- Milestone 36: bounded Trophy context/handle objects without fabricated trophy data.
- Milestone 37: 19 controller startup/read exports with checked guest writes and neutral centered samples until real host input is connected.

The repository has deterministic Windows-runnable tests and a legal installable fixture for each relevant milestone. These fixtures test emulator behavior; they are not commercial-game content.

## What this is not yet

This project does **not** currently boot Vita games normally. It prepares Amagami and executes roughly 5.7 thousand real startup instructions in one bounded diagnostic thread before stopping at the first unknown dependency. It does not yet have the complete guest process, kernel, filesystem, or graphics stack needed to reach and display a menu.

Specifically, it does not yet provide:

- A real Vita process/thread scheduler, TLS, callbacks, waits, timers, mutexes, semaphores, or multi-thread guard behavior.
- Complete ARM/Thumb/VFP/NEON/atomic instruction coverage for every title path.
- A process-wide memory/page service, stack growth, or production allocator.
- Vita VFS mounts and general `SceIo` access for game scripts, textures, fonts, savedata, and system paths.
- Broad AppMgr, display, time, power, font, common-dialog, savedata, and other service behavior.
- A GXM graphics context, guest render targets, command processing, shaders, textures, synchronization, or display queue.
- Guest-produced frames on the `MTKView`.
- Guest audio, persistent save data, or a complete UIKit/GameController-to-Vita input bridge.

## Shortest honest roadmap to an Amagami menu

The roadmap should no longer be thought of as an arbitrary count of one-NID milestones. Use larger, coherent, independently tested subsystem batches.

### Phase A: clear the startup-service runway

1. Implement the reached touch family as one batch: sampling state/mode plus the read/peek APIs actually imported by the title, with checked buffers and neutral host state until the UIKit bridge is connected.
2. Produce an import-coverage map for all 214 imported NIDs and compare it with upstream Vita3K handlers.
3. Proactively batch the high-confidence startup families that Amagami imports and is likely to reach next: time/process info, AppMgr/event state, display setup, power, and minimal I/O entry points.
4. Continue stopping on the first genuinely unknown behavior. A batch may cover adjacent upstream-backed calls, but it must not turn every unimplemented import into success.

This phase ends when startup reaches a real kernel wait/thread boundary, file access, or graphics initialization rather than another trivial initialization setter.

### Phase B: integrate the runtime foundation

1. Replace the single diagnostic thread with the upstream-style process/thread model and scheduler seam.
2. Integrate process memory, stacks/TLS, synchronization, callbacks, and timers needed by the reached path.
3. Mount a read-only `app0:` view with patch overlay semantics, then connect the minimum real `SceIo` path required to load Amagami's own menu assets. Add writable savedata only when reached.

This is a major architectural tranche. Blind HLE returns cannot substitute for it because the game must be able to block, wake, create work, and read its own data correctly.

### Phase C: produce the first guest frame

1. Integrate or adapt the upstream GXM command/resource path instead of inventing an Amagami-specific renderer.
2. Connect Vita surfaces, shaders, textures, buffers, draw state, and synchronization to an iOS-capable backend: upstream Vulkan through MoltenVK or a validated Metal backend.
3. Route the guest display queue/framebuffer to the existing `MTKView`.
4. Load the legally local game/font/system assets required by the title screen without committing them.

The first-menu goal is complete only when a recognizable Amagami title/menu frame is generated from guest GXM state and reproduced after a clean install. A host-drawn imitation, screenshot, synthetic texture, or blue clear does not count.

### Phase D: make the menu usable and the app presentable

After the first guest frame, connect real touch/controller samples, audio, savedata, suspend/resume, and longer-run stability. These are required for playability but do not all block the first visible frame.

## Acceleration policy

The following shortcuts are valid and should make progress materially faster:

- Batch complete API families when they share state and have upstream/VitaSDK behavior to copy.
- Use the title's full import inventory to implement obvious startup clusters before waiting for each one to become the next boundary.
- Reuse upstream Vita3K kernel, VFS, HLE, GXM, and renderer behavior behind narrow iOS adapters.
- Use deterministic offline behavior where it is semantically valid: signed-out NP state, neutral input, read-only app data, and bounded object IDs.
- Automate boundary logging and keep one physical-device run capable of exposing the next real subsystem edge.
- Treat kernel/VFS/GXM integration as major milestones rather than continuing a long chain of tiny leaf-call patches.

The following are not acceptable shortcuts:

- Returning zero for every unknown NID.
- Treating unsupported CPU instructions as no-ops or forcibly advancing the PC.
- Hardcoding Amagami addresses, title IDs, control flow, file contents, or expected return values.
- Pretending a wait completed without scheduler state, or pretending a file read succeeded without real bytes.
- Drawing a fake Amagami menu in UIKit/Metal and calling it guest rendering.
- Uploading the user's game, firmware, keys, decrypted protected content, certificates, or provisioning profiles.

These invalid shortcuts can produce a screenshot quickly but corrupt guest state and make later failures impossible to diagnose.

## UI plan

There are two separate UI jobs:

1. **Emulator shell UI:** library cards/icons, install progress, launch controls, settings, compatibility/status, and diagnostics in a separate sheet or log view. The current UIKit library/settings code is a usable foundation, and this redesign can begin at any time.
2. **In-game UI:** the full-screen surface showing frames produced by the Vita guest. This cannot be completed before Phase C's GXM/display pipeline exists.

For the shortest route to the Amagami menu, do not let shell polish block runtime/VFS/GXM work. The recommended point for a dedicated shell redesign is after Phase A reaches the first stable I/O/kernel/GXM boundary, or in parallel with those core integrations. At that point the default launch experience should be a real library screen; the diagnostic text should move into an optional developer panel. Once guest presentation works, launching a title should transition to the full-screen guest surface.

If visual polish is prioritized over first-menu speed, a shell redesign can be the next dedicated milestone, but it will not make Amagami execute or render sooner.

## Last verified Milestone 37 build

```text
Source commit: 1b49202f22bc1d3d12f12b378c46f52f955231f9
Actions run:   29402882032
Workflow:      Build unsigned iOS IPA
Artifact:      Vita3K-iOS-1b49202f22bc1d3d12f12b378c46f52f955231f9-unsigned
Local IPA:     test-artifacts/milestone37/Vita3K-iOS-milestone37-unsigned.ipa
Bundle ID:     org.vita3k.experimental.ios
Version:       0.37.0 (build 37)
IPA SHA-256:   33A364609BD99ACCB3BA065E746C69C75C39E6E677202EFC7032B66FF421BBFA
Signing files: 0
```

That IPA and hash belong to the accepted code commit above. A later documentation-only commit is not a new emulator milestone and must not replace this record.

## Local Windows verification

Use PowerShell from the repository root:

```powershell
git switch ios-port
git pull --rebase origin ios-port
cmake -S ios/core -B build-core-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-core-tests --config Release --parallel 2 -- /nodeReuse:false
ctest --test-dir build-core-tests -C Release --output-on-failure
git diff --check
git status --short
```

Windows validates the portable core and repository hygiene. It does not replace the macOS iPhoneOS build.

## GitHub Actions: authoritative unsigned IPA procedure

Always extend `.github/workflows/ios.yml`; never create a duplicate iOS workflow. Keep the workflow name exactly `Build unsigned iOS IPA`.

The workflow must retain:

- `workflow_dispatch` for manual builds.
- The `ios-port` push trigger and existing path filters.
- The pinned official VitaSDK Docker fixture job.
- The `macos-15` iPhoneOS arm64 job.
- Portable core tests and all legal synthetic fixtures.
- CMake options `VITA3K_BUILD_IOS=ON` and `VITA3K_IOS_LINK_CORE=ON`.
- `CODE_SIGNING_ALLOWED=NO` and `CODE_SIGNING_REQUIRED=NO`.
- Packaging through `.ci/package-ios.sh` to `artifacts/Vita3K-iOS-unsigned.ipa`.
- Artifact upload named with `${{ github.sha }}` and containing the IPA plus legal fixtures.

For an implementation milestone:

```powershell
git add -A
git commit -m "Implement Vita3K iOS core milestone N"
git push origin ios-port

gh run list --repo 125hz/Vita3K-iOS --branch ios-port --workflow "Build unsigned iOS IPA" --limit 5
gh run watch RUN_ID --repo 125hz/Vita3K-iOS --exit-status
gh run view RUN_ID --repo 125hz/Vita3K-iOS --log-failed
gh run download RUN_ID --repo 125hz/Vita3K-iOS --name ARTIFACT_NAME --dir test-artifacts\milestoneN
Get-FileHash test-artifacts\milestoneN\Vita3K-iOS-unsigned.ipa -Algorithm SHA256
```

Do not rely on the local `gh` default repository; pass `--repo 125hz/Vita3K-iOS` explicitly.

Verify the downloaded artifact before reporting completion:

1. Extract the IPA with `tar -xf` or another ZIP-capable tool. PowerShell `Expand-Archive` can reject the `.ipa` extension.
2. Confirm `Payload/Vita3K-iOS.app/Vita3K-iOS` exists and is arm64 iPhoneOS output.
3. Parse `Info.plist` and record bundle ID, short version, and build number.
4. Confirm no `_CodeSignature`, `embedded.mobileprovision`, `.p12`, `.cer`, or private-key material exists.
5. Record the exact source commit, run ID, artifact name, IPA path, and SHA-256.

Do not stop at a green Action. A code milestone is handed off only after the artifact is downloaded and verified.

## Milestone update checklist

For every code milestone:

- Anchor the work in the exact physical-device CPU, memory, HLE, return, or budget boundary.
- Prefer one coherent subsystem batch over a single trivial NID.
- Add deterministic portable tests and a legal synthetic installable fixture.
- Update the fixture emission/upload list in `.github/workflows/ios.yml` when a new fixture is added.
- Bump the milestone/version consistently in `ios/Info.plist.in`, `ios/src/AppDelegate.mm`, `ios/README.md`, `ios/PORTING.md`, and `docs/ios-development.md`.
- Update this handoff and `ios/MENU-BOOT-CHECKLIST.md` with the accepted result and remaining critical path.
- Run the Windows tests, commit, push, monitor the Action, download the artifact, verify it, and report its SHA-256.
- Give the user concise device steps and ask for the complete next diagnostic.

Documentation-only changes do not create a new milestone or require a version bump.

## Legal and operational boundaries

- Keep Amagami, any other game, `fontpkg.pup`, `preinstall.pup`, `psvupdat.pup`, keys, and extracted proprietary files local.
- Never commit or upload certificates, provisioning profiles, Apple credentials, or signing secrets.
- The user handles signing, sideloading, entitlement/JIT setup, and physical-device execution.
- Codex owns source changes, portable verification, GitHub Actions monitoring, artifact download, unsigned-package verification, and exact handoff records.
- Preserve desktop and Android behavior. Keep iOS-specific host behavior behind narrow interfaces.
- Be explicit that a prepared executable, blue Metal clear, host input capture, or synthetic fixture is not a booted/rendered commercial game.

## Copy-paste continuation prompt

```text
Continue the Vita3K iOS port in https://github.com/125hz/Vita3K-iOS on branch ios-port. First read ios/NEXT-MILESTONE-HANDOFF.md, ios/MENU-BOOT-CHECKLIST.md, ios/README.md, ios/PORTING.md, ios/VION-ANALYSIS.md, docs/ios-development.md, .github/workflows/ios.yml, and the latest git log/diff.

Milestone 37 is complete and accepted on a physical device. Amagami PCSG00291 from patch/eboot.bin entered module_start 0x810176B9 via lifecycle export, successfully executed 5735 instructions and 38 HLE calls, accepted controller sampling mode 2, then stopped at sceTouchSetSamplingState (NID 0x1B9C5D14), SVC 0x8109B8E8, LR 0x81013F85, with r0=0 and r1=1. Do not reimplement Milestone 37 and do not skip the touch call.

Before editing, inspect upstream Vita3K/VitaSDK behavior and the title's full import inventory. Implement a coherent, deterministic touch startup/read subsystem batch rather than a one-off hardcoded return. Proactively include only adjacent imported touch APIs whose behavior can be validated. Unknown CPU, memory, and HLE behavior must remain a hard boundary. Do not hardcode Amagami data or claim the game is playable.

Add or update deterministic legal Windows-runnable tests and the legal installable fixture. Preserve desktop/Android behavior and keep platform code behind narrow interfaces. Run the portable Windows build/tests. Update the milestone/version in ios/Info.plist.in, ios/src/AppDelegate.mm, ios/README.md, ios/PORTING.md, docs/ios-development.md, ios/MENU-BOOT-CHECKLIST.md, and this handoff.

Extend the existing .github/workflows/ios.yml only. Commit and push to ios-port, monitor the Build unsigned iOS IPA Action until it succeeds, download and inspect the exact artifact, record commit/run/artifact/bundle/version/unsigned status/IPA SHA-256, and provide concise physical-device steps. Never upload game or firmware content, keys, certificates, or provisioning profiles. Do not sign, sideload, or enable JIT; the user handles those steps.
```

## Definition of the menu goal

Do not claim success until all four conditions hold:

1. Amagami reaches a stable event/main loop without an unsupported CPU, memory, or HLE boundary.
2. At least one frame is generated from guest GXM state and presented by the iOS host.
3. The frame contains recognizable Amagami title/menu assets, not a host clear or synthetic image.
4. A clean install reproduces the frame without patching guest return values or skipping unknown instructions.
