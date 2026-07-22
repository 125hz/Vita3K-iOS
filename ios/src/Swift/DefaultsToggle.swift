import SwiftUI

/// The UserDefaults keys shared with the Objective-C++ frontend.
///
/// Centralised because both sides read them: a typo in a raw string here would
/// silently decouple a switch from the code that acts on it, with no compile
/// error. The values must match the literals used in NativeFrontend.mm,
/// VirtualController.mm and UpstreamMain.cpp.
enum DefaultsKey: String {
    case perfFPS = "vita3k.perf.fps"
    case perfFrametime = "vita3k.perf.frametime"
    case perfFrametimeGraph = "vita3k.perf.frametimeGraph"
    case perfRAM = "vita3k.perf.ram"
    case perfBattery = "vita3k.perf.battery"
    case perfLog = "vita3k.perf.log"

    case showTitleIDs = "tsubomi.showTitleIds"
    case showVersion = "tsubomi.showVersion"
    case showGameSize = "tsubomi.showGameSize"

    /// Value used when the key has never been written.
    ///
    /// Must match the Objective-C readers exactly: show_title_ids(),
    /// show_version() and show_game_size() all treat an absent key as YES, so
    /// defaulting these to false here would show the switch off while the
    /// library was still drawing the data.
    var defaultValue: Bool {
        switch self {
        case .showTitleIDs, .showVersion, .showGameSize:
            return true
        case .perfFPS, .perfFrametime, .perfFrametimeGraph, .perfRAM, .perfBattery, .perfLog:
            // The in-game overlay stays off until the user asks for a metric.
            return false
        }
    }
}

/// A `Toggle` backed directly by UserDefaults.
///
/// These settings are read by the Objective-C++ side on demand rather than
/// travelling through the settings action, so they apply immediately and are
/// not part of the Done-button commit. `@AppStorage` gives that for free and
/// keeps the switch in sync if the same key is changed elsewhere — the in-game
/// HUD panel writes the same perf keys.
struct DefaultsToggle: View {
    private let title: String
    private let onChange: (() -> Void)?
    private let onEnable: (() -> Void)?
    @AppStorage private var isOn: Bool

    /// - Parameters:
    ///   - onChange: run after any change (used to redraw library cells).
    ///   - onEnable: run only when switching on (used to un-hide the overlay).
    init(
        _ title: String,
        key: DefaultsKey,
        onChange: (() -> Void)? = nil,
        onEnable: (() -> Void)? = nil
    ) {
        self.title = title
        self.onChange = onChange
        self.onEnable = onEnable
        _isOn = AppStorage(wrappedValue: key.defaultValue, key.rawValue)
    }

    var body: some View {
        Toggle(title, isOn: $isOn)
            .onChange(of: isOn) { _, newValue in
                if newValue { onEnable?() }
                onChange?()
            }
    }
}
