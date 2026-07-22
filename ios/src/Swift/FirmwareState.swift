import Foundation
import Observation

/// Live firmware-installation state.
///
/// Onboarding gates each page on one of these flags, and the import that sets
/// them runs asynchronously on the emulator thread. Rather than polling, the
/// core pushes a new settings snapshot through `FirmwareStateBridge` whenever
/// the library refreshes, and the observing views update themselves.
@Observable
@MainActor
final class FirmwareState {
    static let shared = FirmwareState()

    private(set) var preinstalledReady = false
    private(set) var fontPackageReady = false
    private(set) var mainFirmwareReady = false
    /// All three official packages have populated their partitions. Games stay
    /// unavailable until this is true.
    private(set) var allPackagesReady = false

    private init() {
        let settings = Bridge.currentSettings
        apply(
            preinstalled: settings.preinstalledPackageReady,
            font: settings.fontPackageReady,
            mainFirmware: settings.mainFirmwareReady,
            allReady: settings.firmwareReady
        )
    }

    fileprivate func apply(
        preinstalled: Bool,
        font: Bool,
        mainFirmware: Bool,
        allReady: Bool
    ) {
        // Assign only on change: @Observable treats every write as a mutation,
        // and this is called on each library refresh.
        if preinstalledReady != preinstalled { preinstalledReady = preinstalled }
        if fontPackageReady != font { fontPackageReady = font }
        if mainFirmwareReady != mainFirmware { mainFirmwareReady = mainFirmware }
        if allPackagesReady != allReady { allPackagesReady = allReady }
    }
}

/// Objective-C entry point for pushing firmware state in.
///
/// Separate from `FirmwareState` because `@Observable` and `@objc` do not mix:
/// the macro rewrites stored properties into computed ones backed by an
/// observation registrar, which cannot be exposed to the Objective-C runtime.
@objc(TsubomiFirmwareStateBridge)
@MainActor
final class FirmwareStateBridge: NSObject {
    @objc(updateWithPreinstalledReady:fontReady:mainFirmwareReady:allReady:)
    static func update(
        preinstalledReady: Bool,
        fontReady: Bool,
        mainFirmwareReady: Bool,
        allReady: Bool
    ) {
        FirmwareState.shared.apply(
            preinstalled: preinstalledReady,
            font: fontReady,
            mainFirmware: mainFirmwareReady,
            allReady: allReady
        )
    }
}
