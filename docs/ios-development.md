# Windows-first iOS development workflow

This guide sets up a Vita3K fork so Windows is the primary editing and testing environment while GitHub Actions supplies the Apple SDK/Xcode build step. The resulting artifact is unsigned. Installation, signing, sideloading, and runtime JIT activation are intentionally outside this repository and guide.

## 1. What this branch can and cannot do

Today the workflow builds a real `arm64` iPhone/iPad IPA containing the iOS application host, a Metal view, lifecycle handling, persistent logs, sandbox storage, the first dependency-free upstream core slice, a 4 GiB guest address space, shared-page-aware segment mapping, preferred-address Vita VELF loading, checked relocation application, NID parsing, ARM import-stub rewriting, a bounded compiled-style module/thread diagnostic, and one core-owned Metal clear/present. It does not yet contain a working Vita emulator because SELF extraction, general relocatable placement, general ARM/Thumb execution, broader HLE, Vita graphics, and the wider dependency graph have not been ported to iOS. `VITA3K_IOS_LINK_CORE=ON` is required.

Use the pipeline as a stable first milestone: every future porting change should keep the bootstrap IPA green while moving one subsystem across the `CoreBridge` boundary.

## 2. One-time accounts and Windows tools

You need:

- a GitHub account;
- Git for Windows;
- Visual Studio 2022 Build Tools or Visual Studio 2022 with Desktop development with C++;
- CMake and Ninja on `PATH`;
- VS Code with the C/C++ and CMake Tools extensions, or another C++ editor;
- enough disk space for Vita3K and its submodules.

Verify the command-line tools in PowerShell:

```powershell
git --version
cmake --version
ninja --version
```

## 3. Create and clone the fork

1. Open `https://github.com/Vita3K/Vita3K` and choose **Fork**.
2. Keep the complete history and create the fork under your GitHub account.
3. In PowerShell, replace `YOUR_GITHUB_NAME` below:

```powershell
git clone --recurse-submodules https://github.com/YOUR_GITHUB_NAME/Vita3K.git
Set-Location Vita3K
git remote add upstream https://github.com/Vita3K/Vita3K.git
git switch -c ios-port
```

If this prepared repository was copied rather than cloned from your own fork, repoint `origin` first:

```powershell
git remote set-url origin https://github.com/YOUR_GITHUB_NAME/Vita3K.git
git remote -v
```

Commit the iOS files and push the branch:

```powershell
git add CMakeLists.txt README.md .ci/package-ios.sh .github/workflows/ios.yml ios docs/ios-development.md
git commit -m "Add experimental unsigned iOS bootstrap pipeline"
git push -u origin ios-port
```

## 4. Run the unsigned IPA build

The workflow runs automatically for relevant pushes to `ios-port`. It can also be started manually:

1. Open the fork on GitHub.
2. Open **Actions**.
3. Select **Build unsigned iOS IPA**.
4. Choose **Run workflow**, select `ios-port`, and run it.
5. Wait for **Unsigned arm64 device IPA** to finish.
6. Open the completed run and download the `Vita3K-iOS-<commit>-unsigned` artifact.
7. Extract the Actions artifact ZIP once; it contains `Vita3K-iOS-unsigned.ipa`.

With GitHub CLI installed, the equivalent Windows commands are:

```powershell
gh workflow run ios.yml --ref ios-port
gh run list --workflow ios.yml --limit 5
gh run watch
New-Item -ItemType Directory -Force artifacts | Out-Null
gh run download --name "Vita3K-iOS-COMMIT_SHA-unsigned" --dir artifacts
```

Replace `COMMIT_SHA` with the full SHA shown by the run. GitHub Actions artifacts are transport ZIPs, so do not rename that outer ZIP to `.ipa`; extract it to obtain the already packaged IPA.

## 5. What CI verifies

The macOS job:

1. configures the repository with the Xcode generator and the `iphoneos` SDK;
2. targets only `arm64` physical devices, not the simulator;
3. disables Xcode code signing;
4. builds `Vita3KiOS`;
5. checks the property list and confirms the executable contains `arm64`;
6. rejects the bundle if `codesign` reports a signature;
7. creates the required `Payload/Vita3K-iOS.app` IPA layout;
8. tests the resulting ZIP and uploads it as an Actions artifact.

No Apple certificates, passwords, provisioning profiles, or GitHub secrets are required for this unsigned build.

## 6. Daily Windows development loop

Keep shared emulator changes testable in the existing Windows target. Build only the part you changed when possible; a complete Vita3K desktop build has a large dependency graph.

```powershell
git switch ios-port
git pull --rebase
git submodule update --init --recursive

cmake --preset windows-ninja
cmake --build --preset windows-ninja --config Debug
ctest --preset windows-ninja -C Debug --output-on-failure
```

Preset names can change upstream. Check the current list with `cmake --list-presets` and use the Windows preset present in `CMakePresets.json`. Do not assume that passing an iOS toolchain to Windows will work: Apple frameworks, the iPhone SDK, asset tools, and linker are available only in Xcode on macOS.

After local tests:

```powershell
git add -A
git commit -m "Describe one iOS porting milestone"
git push
```

Then inspect the GitHub Actions log and download the new IPA artifact.

## 7. Device-oriented diagnostics

The bootstrap logger writes each message to both Apple unified logging and `Documents/vita3k.log`. File sharing and opening documents in place are enabled in `Info.plist`, making the log suitable for retrieval from the app's Documents area after a test run.

Every subsystem integrated into `CoreBridge` should log:

- build commit and configuration;
- lifecycle transition;
- last guest module and guest program counter;
- memory allocation/protection failure and host error;
- shader translation and pipeline compilation errors;
- last render pass/command-buffer milestone;
- active guest thread and fatal exception breadcrumb.

Keep logging bounded before the emulator core is enabled: add rotation (for example, three 5 MiB files) before high-frequency CPU or renderer tracing. Never log game keys, account credentials, or copyrighted game content.

For the Milestone 10 loader diagnostic, download `milestone10-vitasdk-homebrew.velf` from the same Actions artifact as the IPA. Remove the older fixture from `Documents/Vita3K/imports`, copy the VELF through the Files app, then tap **Rescan Imports**. A successful diagnostic reports `MAPPED`, 10 verified relocation patches, one rebound function stub, six executed instructions across Thumb/Thumb-2 and ARM modes, one HLE call, and a return value of 1. The accompanying SELF is probe-only; VPK, firmware, and commercial-game files remain outside the executable path.

For the Milestone 11 renderer diagnostic, launch the IPA on a physical device and wait for `Renderer: passed (first core-owned Metal frame presented)`. The blue background is a Metal clear color supplied by `HostDisplay`; it is not Vita graphics output. The portable smoke test validates frame ownership and completion bookkeeping, while the device build validates the actual MetalKit command queue and drawable presentation.

Keep firmware files such as `fontpkg.pup`, `preinstall.pup`, and the system update PUP on the local device/PC. Do not add them to Git, CI caches, or Actions artifacts. Firmware selection and extraction will be added after the iOS build includes the upstream crypto, package, FAT/exFAT, and psvpfs dependencies needed by `install_pup`.

## 8. Keeping the fork current

Sync in a dedicated commit so upstream changes are easy to separate from the iOS port:

```powershell
git fetch upstream
git switch ios-port
git merge --no-ff upstream/master
git submodule update --init --recursive
git push
```

Resolve conflicts by preserving the early iOS-only branch in the root `CMakeLists.txt`; without it, configuration falls into upstream's desktop Qt/MoltenVK graph.

## 9. How to assign useful Codex milestones

Ask for one verifiable subsystem at a time and require the Windows build plus the unsigned-IPA workflow to remain green. A useful task template is:

> On branch `ios-port`, implement the next item in `ios/PORTING.md`: [one item]. Preserve desktop and Android behavior. Add a platform interface instead of broad Apple conditionals, add tests runnable on Windows where possible, keep `VITA3K_IOS_LINK_CORE=ON` building, and state exactly what remains stubbed. Do not add signing, sideloading, JIT activation, proprietary firmware, or game files.

Good early assignments are dependency inventory, extracting a headless core target, sandbox path mapping, a renderer surface interface, or a deterministic CPU/memory unit-test harness. “Build the whole emulator” is too broad to diagnose when CI fails.

## 10. Troubleshooting CI

- **CMake enters the desktop Qt build:** confirm `-DCMAKE_SYSTEM_NAME=iOS` and `-DVITA3K_BUILD_IOS=ON` are both present.
- **No `.app` found:** inspect the build step for the actual Xcode configuration and SDK; the packaging script accepts only a `*-iphoneos` device bundle.
- **Bundle is signed:** remove inherited signing settings. The packager intentionally refuses signed input.
- **Workflow does not run after a push:** it watches the `ios-port` branch and iOS/build files. Start it manually for other branches or adjust the branch filter in your fork.
- **Core self-tests fail:** capture `Documents/vita3k.log`; the current tests cover Vita3K's ARM instruction encoder and NID database.
