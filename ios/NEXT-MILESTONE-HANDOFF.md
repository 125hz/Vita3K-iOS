# Vita3K iOS next-milestone handoff

Use this file when continuing in a new Codex task or Claude Code session. The repository is `https://github.com/125hz/Vita3K-iOS`, the working branch is `ios-port`, and the current completed implementation target is Milestone 18.

## Copy-paste prompt

```text
Continue the Vita3K iOS port in https://github.com/125hz/Vita3K-iOS on branch ios-port. First read ios/README.md, ios/PORTING.md, docs/ios-development.md, ios/NEXT-MILESTONE-HANDOFF.md, .github/workflows/ios.yml, and the latest git log/diff. Inspect the exact physical-device screenshot or diagnostic I provide from the previous milestone.

Implement exactly one bounded next milestone based on that first real unsupported CPU instruction, memory fault, or unimplemented HLE NID. Do not guess past the captured boundary and do not claim the game is playable. Preserve desktop and Android behavior. Keep platform code behind narrow interfaces. Add or update deterministic legal synthetic regression coverage runnable on Windows. Run the portable core build and tests locally.

Extend the existing .github/workflows/ios.yml workflow; do not create a duplicate workflow. It must retain workflow_dispatch, build an unsigned arm64 iphoneos IPA on a GitHub-hosted macOS runner with CODE_SIGNING_ALLOWED=NO and CODE_SIGNING_REQUIRED=NO, package Payload/Vita3K-iOS.app as artifacts/Vita3K-iOS-unsigned.ipa, and upload that IPA plus all legal testing fixtures. Never upload my game, firmware PUPs, keys, certificates, provisioning profiles, or copyrighted extracted files. Do not sign, sideload, or enable JIT; I handle those steps.

Update the milestone number in ios/Info.plist.in, ios/src/AppDelegate.mm, ios/README.md, ios/PORTING.md, and docs/ios-development.md. Commit and push the completed work to ios-port, monitor the Build unsigned iOS IPA Action until it succeeds, download the exact artifact, compute the IPA SHA-256, and give me concise physical-device test steps plus the next diagnostic I should return. Be transparent about all remaining upstream incompatibilities.
```

## Known Milestone 18 boundary

Milestone 17 prepared Amagami title `PCSG00291` from `patch/eboot.bin`, but its one-shot interpreter attempt executed zero instructions and reported `0xF62F7FFF` at `0x810176B8`. The loader had treated the module-info header's relative `module_start` as final. Upstream Vita3K later replaces that value when it encounters lifecycle export NID `0x935CD196` (`NID_MODULE_START`). Milestone 18 now preserves export entry addresses and performs that override before execution.

The physical-device Milestone 18 acceptance result is not yet known. Do not implement a new CPU instruction from the old `0xF62F7FFF` diagnostic. First install the Milestone 18 artifact, prepare `PCSG00291`, confirm `via lifecycle export`, run the one-shot 256-instruction attempt, and use that new diagnostic for Milestone 19.

## Local Windows verification

```powershell
git switch ios-port
git pull --rebase origin ios-port
cmake -S ios/core -B build-core-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-core-tests --config Release --parallel
ctest --test-dir build-core-tests -C Release --output-on-failure
```

The smoke-test executable also accepts output switches used by CI, including `--emit-m18-install-zip-fixture artifacts/milestone18-lifecycle-export.zip`. Generated build and artifact directories are not source and should not be committed.

## GitHub Actions and artifact verification

```powershell
git add -A
git commit -m "Implement Vita3K iOS core milestone N"
git push origin ios-port

gh run list --repo 125hz/Vita3K-iOS --branch ios-port --workflow "Build unsigned iOS IPA"
gh run watch RUN_ID --repo 125hz/Vita3K-iOS --exit-status
gh run download RUN_ID --repo 125hz/Vita3K-iOS --name ARTIFACT_NAME --dir test-artifacts\milestoneN
Get-FileHash test-artifacts\milestoneN\Vita3K-iOS-unsigned.ipa -Algorithm SHA256
```

The existing Action is authoritative. It builds an unsigned physical-device IPA and includes legal synthetic fixtures. Do not add repository secrets or signing material.

## Milestone discipline

- One captured boundary per milestone.
- Keep the attempt interpreter-only and bounded until interpreter boot is stable.
- A blue Metal clear is not guest rendering.
- Host touch/controller capture is not Vita guest input HLE.
- Keep Amagami, `fontpkg.pup`, `preinstall.pup`, `psvupdat.pup`, keys, and extracted proprietary files local.
- The user handles IPA signing, sideloading, and JIT testing.
