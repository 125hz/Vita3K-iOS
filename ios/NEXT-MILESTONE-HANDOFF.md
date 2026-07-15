# Vita3K iOS continuation handoff

This is the authoritative continuation note for the Windows-first Vita3K iOS work in `https://github.com/125hz/Vita3K-iOS`, branch `ios-port`. The newest implemented milestone is **Milestone 38** (touch, time, power, and display startup HLE). Milestone 37 is the last milestone accepted on a physical device; Milestone 38 is implemented, portable-tested, and CI-verified but **awaits its physical-device run**, so the next real boundary is unknown until the user tests the Milestone 38 IPA.

Read this file together with:

- `ios/MENU-BOOT-CHECKLIST.md` for the first guest-produced menu acceptance criteria.
- `ios/README.md` for device-test instructions and milestone history.
- `ios/PORTING.md` for upstream gaps and architectural constraints.
- `ios/VION-ANALYSIS.md` for lessons from another iOS emulator frontend.
- `docs/ios-development.md` for local and CI build operations.
- `.github/workflows/ios.yml` and `.ci/package-ios.sh` for the authoritative unsigned IPA path.

## Boundary history at this point

The Milestone 37 physical run (Amagami `PCSG00291`, `patch/eboot.bin`, `module_start 0x810176B9 via lifecycle export`) executed 5,735 instructions and 38 HLE calls, accepted controller sampling mode 2, and stopped at:

```text
NID:          0x1B9C5D14
Name:         sceTouchSetSamplingState
SVC:          0x8109B8E8
LR:           0x81013F85
Arguments:    r0=0x00000000 r1=0x00000001
```

That is `sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START)`, verified against upstream Vita3K (`vita3k/modules/SceTouch/SceTouch.cpp`, `vita3k/touch/`). **This boundary is what Milestone 38 implements. Do not treat it as the next boundary anymore.** The next boundary will be whatever the Milestone 38 device run reports.

## What Milestone 38 implemented

One coherent batch of four upstream-backed startup service families, all bound conditionally on the title's import inventory (nothing is bound that the title does not import), all validated against the upstream sources checked into `vita3k/`:

1. **Touch** (`ios/src/GuestThread.cpp`, `touch_imports` table): `sceTouchSetSamplingState`, `sceTouchGetSamplingState`, `sceTouchGetPanelInfo` (upstream 1920x1088 geometry, rear active-area Y 108..889, force 1..128), `sceTouchGetPixelDensity` (22.0), `sceTouchPeek`, `sceTouchPeek2`, `sceTouchPeekRegion` (upstream ignores the region), `sceTouchRead`, `sceTouchRead2`, `sceTouchEnableTouchForce`, `sceTouchDisableTouchForce`. Peek/read use upstream port/pointer/count validation (`SCE_TOUCH_ERROR_INVALID_ARG` = `0x80350001`, max 64 buffers) and write zero-report neutral `SceTouchData` (0x90 bytes each) with checked guest writes. Peek returns zero buffers while sampling is stopped, exactly like upstream `touch_get`. Read advances one virtual vblank per call because real blocking needs the future scheduler. **No fabricated touches; the UIKit touch bridge is not connected to guest HLE yet.**
2. **Time**: `sceKernelGetProcessTime`, `sceKernelGetProcessTimeLow`, `sceKernelGetProcessTimeWide`, `sceKernelGetSystemTimeWide` (64-bit values returned in r0/r1), `sceRtcGetCurrentTick`, `_sceRtcGetCurrentTick` (checked u64 guest writes; null pointer returns `SCE_RTC_ERROR_INVALID_POINTER` = `0x80251001`). All are driven by one deterministic virtual microsecond clock that starts at zero, increments by one per query, and jumps one vblank period (16,667 us) per vblank wait; RTC ticks add the upstream `RTC_OFFSET` (62,135,596,800,000,000) so the reported date is the epoch, deterministically.
3. **Power**: clock setters (`scePowerSetArmClockFrequency`, bus, GPU, GPU xbar) with upstream negative-frequency validation (`0x802B0000`) and recorded requests; clock getters returning upstream's fixed 444/222/222/166 MHz; deterministic offline battery profile (`scePowerGetBatteryLifePercent`=100, `GetBatteryLifeTime`=INT_MAX, `IsBatteryCharging`=0, `IsBatteryExist`=1, `IsLowBattery`=0, `IsPowerOnline`=0).
4. **Display**: `sceDisplayWaitVblankStart`, `WaitVblankStartCB`, `WaitVblankStartMulti`, `WaitVblankStartMultiCB` (advance a virtual vblank counter; no callbacks can exist because callback creation is still unbound), `sceDisplayGetVcount` (counter & 0xFFFF), `sceDisplayGetRefreshRate` (59.94005), and `_sceDisplaySetFrameBuf`/`sceDisplaySetFrameBuf`/`_sceDisplayGetFrameBuf`/`sceDisplayGetFrameBuf` with the full upstream validation chain (struct size 0x18/0x1C, non-null base, pitch >= width, `SCE_DISPLAY_PIXELFORMAT_A8B8G8R8` only, sync 0/1, minimum 480x272 resolution, pitch >= 480) and recorded framebuffer state. **A recorded SetFrameBuf is bookkeeping only, not presentation.** If the device run reports `Display HLE: ... framebuf base=...`, the title has declared its first framebuffer address — a major signal for the GXM/display phase.

Diagnostics gained `Touch HLE:`, `Time HLE:`, `Power HLE:`, and `Display HLE:` sections plus `Touch/Time/Display boundary:` messages for failed checked guest-memory operations, which stop the run without partial state mutation.

Unimplemented upstream stubs (touch regions/ext variants/device info, `_sceKernelGetSystemTime`, power callbacks, display callbacks/registration, everything else) remain hard diagnostic boundaries.

### Relevant files changed by Milestone 38

- `ios/src/GuestThread.cpp` — the four family tables, verification, bindings, virtual clock, diagnostics.
- `ios/include/vita3k_ios/GuestThread.h` — new result telemetry fields.
- `ios/include/vita3k_ios/CoreBridge.h`, `ios/src/CoreBridge.cpp` — `TitleBootResult` mirrors and propagation.
- `ios/core/tests/CoreSmokeTests.cpp` — 18 new fixtures/assertion blocks (exit codes 174–193) plus `--emit-m38-install-zip-fixture`.
- `.github/workflows/ios.yml` — emits/uploads `milestone38-startup-services.zip`.
- `ios/Info.plist.in` (0.38.0 / 38), `ios/src/AppDelegate.mm` (title), `ios/README.md`, `ios/PORTING.md`, `docs/ios-development.md`, `ios/MENU-BOOT-CHECKLIST.md`, this handoff.

### Milestone 38 test coverage

Portable Windows tests (all in `CoreSmokeTests.cpp`, run via `ctest`) cover: the captured set/get sampling-state startup pair; peek with sampling stopped (zero buffers, no vblank advance); read (one neutral sample, one vblank advance); unmapped and null buffers; invalid port and invalid sampling state; panel geometry read back through guest memory and returned via `sceKernelExitThread` (exit status `0x043F077F` = packed maxAaX/maxAaY); virtual-clock monotonicity across two `sceKernelGetProcessTimeWide` calls; RTC tick success/null/unmapped; power clock request + fixed 444 report + negative rejection; vblank wait + vcount readback; framebuffer set/get success, invalid pixel format, and upstream null-framebuffer success. A failed guest write never partially mutates sample counters or framebuffer state.

## Current verified build

```text
Milestone 38 source commit:  6186b7fa00e79e4a2620ee25c4ed672978d750a8
Actions run:                 29406563073 (success)
Workflow:                    Build unsigned iOS IPA
Artifact:                    Vita3K-iOS-6186b7fa00e79e4a2620ee25c4ed672978d750a8-unsigned
Local IPA:                   test-artifacts/milestone38/Vita3K-iOS-unsigned.ipa
Bundle ID:                   org.vita3k.experimental.ios
Version:                     0.38.0 (build 38)
Signing files:               0
IPA SHA-256:                 8041650E9E8AADF18336C7E757A41BA49961443005479A37FF9057546EA79F61
```

The IPA was extracted and verified: `Payload/Vita3K-iOS.app/Vita3K-iOS` is a Mach-O arm64 iPhoneOS binary, the binary `Info.plist` carries the bundle ID/version/build above, and no `_CodeSignature`, provisioning profile, certificate, or key material exists anywhere in the archive. The artifact also contains every legal synthetic fixture through `milestone38-startup-services.zip`.

The last physically accepted build remains Milestone 37:

```text
Source commit: 1b49202f22bc1d3d12f12b378c46f52f955231f9
Actions run:   29402882032
Artifact:      Vita3K-iOS-1b49202f22bc1d3d12f12b378c46f52f955231f9-unsigned
IPA SHA-256:   33A364609BD99ACCB3BA065E746C69C75C39E6E677202EFC7032B66FF421BBFA
```

## Physical device test the user must run next

1. Sign and sideload the Milestone 38 IPA (user handles signing/JIT; nothing changed in that flow).
2. Launch, select `PCSG00291` with **Prefer Installed Patch** enabled.
3. Confirm preparation still reports `module_start 0x810176B9 via lifecycle export`.
4. Run **Attempt Boot (65536 Instructions)** once.
5. Return the complete diagnostic text, especially:
   - the new first-boundary block (NID, name, SVC, LR, r0–r3),
   - the instruction and HLE-call counts,
   - any `Touch HLE:`, `Time HLE:`, `Power HLE:`, `Display HLE:` sections and boundary messages.

The diagnostic to paste back is the whole boot detail string shown in the app; do not summarize it.

## What this is not yet

Nothing in this milestone renders, schedules, or reads files. The project still does not have: a guest process/thread scheduler, TLS, callbacks, waits/timers/mutexes; complete CPU coverage (VFP/NEON/atomics likely gaps); a process-wide memory service; guest VFS mounts and `SceIo`; GXM contexts/shaders/draw translation; guest frames on the `MTKView`; audio; savedata; or a UIKit/GameController-to-guest input bridge. The dark-blue Metal clear remains host diagnostic output.

## Shortest honest roadmap to an Amagami menu

### Phase A: clear the startup-service runway (in progress)

1. ~~Touch family~~ plus time/power/display basics — **done in Milestone 38**.
2. Run the Milestone 38 IPA on device; the run should now cross touch startup and likely time/power/display probing. The next boundary candidates, in rough likelihood order: `SceIo` file access (needs Phase B VFS), `sceGxmInitialize` (Phase C), kernel threading/callback creation (Phase B), AppMgr/CommonDialog state, or an unsupported CPU instruction. Each remains a hard boundary by design.
3. If the next boundary is another small deterministic service family with upstream behavior, batch it the same way (import-conditional bindings, upstream constants, checked guest memory, portable tests, one legal fixture).
4. Produce an import-coverage map of all imported NIDs against upstream Vita3K handlers to size the remaining runway (still not done; useful next).

### Phase B: integrate the runtime foundation

Threads/scheduler seam, process memory/stacks/TLS, synchronization, callbacks, timers, then a read-only `app0:` VFS view with patch overlay plus the minimal real `SceIo` paths. Blind HLE returns cannot substitute: the game must block, wake, and read real bytes.

### Phase C: produce the first guest frame

Integrate/adapt the upstream GXM command/resource path (do not invent an Amagami-specific renderer), connect to MoltenVK/Vulkan or a validated Metal backend, and route the display queue to the existing `MTKView`. Milestone 38's framebuffer bookkeeping gives the display-side anchor.

### Phase D: make the menu usable

Real touch/controller sample bridging (the HLE surface from Milestones 37–38 is ready to receive it), audio, savedata, stability.

### Strategic note for whoever continues

The `vita3k/` tree in this repository contains the complete upstream emulator (kernel, modules, VFS, GXM, renderers). The `ios/` core is a from-scratch bounded diagnostic runner that reuses upstream headers/NIDs but reimplements execution. Phase A is tractable this way, but Phases B and C amount to reimplementing large upstream subsystems. Before starting Phase B from scratch, seriously evaluate lifting upstream subsystems (kernel/threading, VFS, GXM) behind the existing narrow iOS interfaces instead of continuing the reimplementation — that is very likely the faster honest path to a real menu, and the acceleration policy below explicitly allows it.

## Acceleration policy

Valid shortcuts:

- Batch complete API families with upstream/VitaSDK behavior to copy (Milestone 38 batched four).
- Use the title's full import inventory to implement obvious startup clusters proactively.
- Reuse upstream Vita3K kernel, VFS, HLE, GXM, and renderer code behind narrow iOS adapters.
- Deterministic offline behavior where semantically valid: signed-out NP, neutral input, empty touch, fixed clocks, virtual monotonic time, read-only app data.
- Keep one physical-device run capable of exposing the next real subsystem edge.

Forbidden shortcuts (unchanged):

- Returning success for unknown NIDs; skipping unsupported CPU instructions.
- Hardcoding Amagami addresses, title IDs, control flow, file contents, or expected return values.
- Pretending waits completed or file reads succeeded without real state/bytes.
- Drawing a fake Amagami menu in UIKit/Metal and calling it guest rendering.
- Uploading the user's game, firmware, keys, decrypted protected content, certificates, or provisioning profiles.

## Local Windows verification

```powershell
git switch ios-port
git pull --rebase origin ios-port
cmake -S ios/core -B build-core-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-core-tests --config Release --parallel 2 -- /nodeReuse:false
ctest --test-dir build-core-tests -C Release --output-on-failure
git diff --check
git status --short
```

## GitHub Actions: authoritative unsigned IPA procedure

Always extend `.github/workflows/ios.yml`; never create a duplicate iOS workflow. Keep the workflow name exactly `Build unsigned iOS IPA`. The workflow must retain: `workflow_dispatch`, the `ios-port` push trigger and path filters, the pinned VitaSDK Docker fixture job, the `macos-15` arm64 job, portable tests plus all legal fixtures (now through `milestone38-startup-services.zip`), `VITA3K_BUILD_IOS=ON`, `VITA3K_IOS_LINK_CORE=ON`, `CODE_SIGNING_ALLOWED=NO`, `CODE_SIGNING_REQUIRED=NO`, packaging via `.ci/package-ios.sh`, and the `${{ github.sha }}`-named artifact upload.

```powershell
git add -A
git commit -m "Implement Vita3K iOS core milestone N"
git push origin ios-port

gh run list --repo 125hz/Vita3K-iOS --branch ios-port --workflow "Build unsigned iOS IPA" --limit 5
gh run watch RUN_ID --repo 125hz/Vita3K-iOS --exit-status
gh run download RUN_ID --repo 125hz/Vita3K-iOS --name ARTIFACT_NAME --dir test-artifacts\milestoneN
Get-FileHash test-artifacts\milestoneN\Vita3K-iOS-unsigned.ipa -Algorithm SHA256
```

Verify before reporting completion: extract with `tar -xf`, confirm `Payload/Vita3K-iOS.app/Vita3K-iOS` is arm64 iPhoneOS, parse `Info.plist` (bundle `org.vita3k.experimental.ios`, correct version/build), confirm zero signing material (`_CodeSignature`, `embedded.mobileprovision`, `.p12`, `.cer`, keys), and record commit/run/artifact/path/SHA-256. A green Action alone is not a handoff.

## Milestone update checklist

- Anchor the work in the exact physical-device boundary; prefer one coherent upstream-backed batch.
- Add deterministic portable tests and a legal synthetic installable fixture; extend the workflow's emission/upload list.
- Bump milestone/version in `ios/Info.plist.in`, `ios/src/AppDelegate.mm`, `ios/README.md`, `ios/PORTING.md`, `docs/ios-development.md`; update `ios/MENU-BOOT-CHECKLIST.md` and this handoff.
- Run Windows tests, commit, push, watch the Action, download and verify the artifact, record its SHA-256.
- Give the user concise device steps and request the complete next diagnostic.
- Documentation-only changes do not bump versions.

## Legal and operational boundaries

- Keep Amagami, any other game, firmware PUPs, keys, and extracted proprietary files local; never commit or upload them.
- Never commit certificates, provisioning profiles, Apple credentials, or signing secrets.
- The user handles signing, sideloading, entitlement/JIT setup, and physical-device execution.
- The implementing agent owns source changes, portable verification, Actions monitoring, artifact download/verification, and exact handoff records.
- Preserve desktop and Android behavior; keep iOS host behavior behind narrow interfaces.
- A prepared executable, blue Metal clear, host input capture, or synthetic fixture is not a booted or rendered commercial game.

## Copy-paste continuation prompt

```text
Continue the Vita3K iOS port in https://github.com/125hz/Vita3K-iOS on branch ios-port. First read ios/NEXT-MILESTONE-HANDOFF.md, ios/MENU-BOOT-CHECKLIST.md, ios/README.md, ios/PORTING.md, ios/VION-ANALYSIS.md, docs/ios-development.md, .github/workflows/ios.yml, and the latest git log/diff. Treat the checked-in handoff and source as authoritative over this prompt.

Milestone 38 (touch + time + power + display startup HLE) is implemented, portable-tested, and CI-verified. Its physical-device result determines the next boundary. If the user has not yet tested the Milestone 38 IPA, ask them to run the device test in the handoff's "Physical device test" section and return the complete diagnostic. Do not reimplement Milestones 37-38 and do not assume what the next boundary is.

When the user returns the diagnostic: verify the reported NID against upstream Vita3K/VitaSDK in the vita3k/ tree, then implement the next coherent upstream-backed batch. Bind only imported NIDs, keep unknown CPU/memory/HLE behavior as hard boundaries, use checked guest memory with no partial mutation on failure, and never hardcode Amagami data. If the boundary is kernel threading, SceIo/VFS, or GXM initialization, stop patching leaf calls: plan the corresponding Phase B/C subsystem integration and strongly consider reusing the upstream vita3k/ subsystems behind the existing narrow iOS interfaces instead of reimplementing them.

Add deterministic legal Windows-runnable tests and a legal installable fixture for anything implemented. Preserve desktop/Android behavior. Run the portable Windows build/tests (cmake -S ios/core -B build-core-tests; build; ctest; git diff --check). Bump milestone/version consistently in ios/Info.plist.in, ios/src/AppDelegate.mm, ios/README.md, ios/PORTING.md, docs/ios-development.md, ios/MENU-BOOT-CHECKLIST.md, and rewrite ios/NEXT-MILESTONE-HANDOFF.md for the next agent.

Extend the existing .github/workflows/ios.yml only. Commit and push to ios-port, monitor the "Build unsigned iOS IPA" run with gh --repo 125hz/Vita3K-iOS, download and verify the exact artifact (arm64 iPhoneOS, org.vita3k.experimental.ios, no signing material), record commit/run/artifact/version/SHA-256, and give the user concise device steps plus the exact diagnostic to return. Never upload game or firmware content, keys, certificates, or provisioning profiles. Do not sign, sideload, or enable JIT; the user handles those.
```

## Definition of the menu goal

Do not claim success until all four conditions hold:

1. Amagami reaches a stable event/main loop without an unsupported CPU, memory, or HLE boundary.
2. At least one frame is generated from guest GXM state and presented by the iOS host.
3. The frame contains recognizable Amagami title/menu assets, not a host clear or synthetic image.
4. A clean install reproduces the frame without patching guest return values or skipping unknown instructions.
