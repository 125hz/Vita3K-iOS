import Foundation
import Observation

/// Everything the library screen renders, pushed in by the core.
///
/// The emulator thread owns the truth here; the UI never polls. Each core
/// refresh replaces `games` wholesale, which is cheap because the entries are
/// immutable value-ish objects and SwiftUI diffs them by title ID.
@Observable
@MainActor
final class LibraryState {
    static let shared = LibraryState()

    private(set) var games: [GameEntry] = []
    /// Installed firmware version, shown in the header. Empty when none.
    private(set) var firmwareVersion = ""
    /// All three official packages are installed. Games cannot boot otherwise
    /// and are dimmed in the list.
    private(set) var firmwareReady = false
    /// A JIT-enabling debugger is attached. Games cannot boot without it.
    private(set) var jitAvailable = true

    /// Bumped whenever cached cover art may be stale.
    ///
    /// The art cache is keyed by file path, and installing a license (or an
    /// update) can make a title's icon appear at a path that was previously
    /// empty. Nothing about the entry changes, so neither the cache nor the
    /// view's load task would re-run on their own - the cover stayed blank
    /// until the app restarted. Cells fold this into their task identity.
    private(set) var artGeneration = 0

    /// Transient toast under the header, cleared automatically.
    private(set) var statusMessage: String?
    /// Blocking progress overlay ("Booting…", "Deleting game…").
    private(set) var busyMessage: String?

    /// Grid vs list. The landscape carousel is a presentation of grid mode,
    /// not a third setting.
    var isListMode: Bool {
        didSet {
            guard isListMode != oldValue else { return }
            UserDefaults.standard.set(isListMode, forKey: Self.listModeKey)
        }
    }

    /// Matches the key the Objective-C frontend used, so the user's choice
    /// survives the migration.
    private static let listModeKey = "tsubomi.libraryListMode"

    // MARK: - Game controller focus
    //
    // Replaces Vita3KPadNavigator's spatial walk over UIView subviews, which
    // has no SwiftUI equivalent. The navigator now only reads the controller
    // and forwards intent here; this type owns which game is focused and the
    // view draws a ring around it.

    /// Title ID of the pad-focused game, or nil when the pad is not driving.
    /// The ring is only drawn while this is non-nil, so touch users never see
    /// a focus indicator.
    private(set) var focusedTitleID: String?

    /// Columns currently rendered by the grid, measured by the view. Needed so
    /// up/down move a whole row rather than one item.
    var gridColumnCount = 1

    /// Set when the pad asks for the game-actions menu; the view presents it
    /// and clears this.
    var padActionsTarget: GameEntry?

    /// Raised each time the pad activates a game, so the view can launch it
    /// through the same gating path a tap uses.
    var padLaunchTarget: GameEntry?

    /// Carousel D-pad stepping, as a running net-steps total rather than a
    /// token + direction. Two rapid presses can coalesce into a single
    /// observation, and a token only told the carousel "something moved" - it
    /// stepped once for two presses, so the internal position and the visible
    /// one drifted apart and the next press looked like it skipped a game. An
    /// accumulator lets the carousel step by the true delta since it last read
    /// it, so coalesced presses still move the right number of covers.
    private(set) var carouselStepAccumulator = 0

    private var statusDismissal: Task<Void, Never>?

    private init() {
        // Key absent means list, which was the previous default.
        isListMode = UserDefaults.standard.object(forKey: Self.listModeKey) as? Bool ?? true
    }

    // MARK: - Focus movement

    /// Moves the pad focus. `dx`/`dy` are -1, 0 or 1 in list coordinates.
    ///
    /// The first press in any direction only reveals the focus ring on the
    /// first game rather than moving, so the user can see where they are
    /// before anything scrolls.
    fileprivate func moveFocus(dx: Int, dy: Int, layout: FocusLayout) {
        guard !games.isEmpty else { return }
        guard let current = focusedIndex else {
            focusedTitleID = games.first?.titleID
            return
        }
        let step: Int
        switch layout {
        case .list:
            // A list has one column: vertical moves by one, horizontal is inert.
            step = dy
        case .carousel:
            // The carousel is a single horizontal row.
            step = dx
        case .grid:
            step = dx + dy * max(1, gridColumnCount)
        }
        guard step != 0 else { return }
        if layout == .carousel {
            // The carousel handles its own motion so the row keeps travelling
            // in one direction across repeats rather than snapping to the
            // start when the game index wraps. focusedTitleID is updated by
            // the carousel from whatever ends up centred.
            carouselStepAccumulator += step
            return
        }
        // Grid and list are finite lists, where wrapping from the bottom back
        // to the top is disorienting.
        let next = min(max(current + step, 0), games.count - 1)
        guard next != current else { return }
        focusedTitleID = games[next].titleID
    }

    fileprivate func activateFocused() {
        guard let game = focusedGame else { return }
        padLaunchTarget = game
    }

    fileprivate func showActionsForFocused() {
        guard let game = focusedGame else { return }
        padActionsTarget = game
    }

    /// Drops the ring — the library went off screen, or a sheet took over.
    fileprivate func clearFocus() {
        focusedTitleID = nil
    }

    /// Confirms a manual rescan finished. Short, because a rescan that found
    /// nothing new is the normal case and does not deserve a lingering banner.
    func showRefreshedToast() {
        showStatus("Library refreshed", duration: .seconds(2))
    }

    fileprivate func bumpArtGeneration() {
        artGeneration &+= 1
    }

    /// Public counterpart to `clearFocus`, for the view layer.
    func clearPadFocus() {
        focusedTitleID = nil
    }

    /// Let the carousel write the focus back when the user scrolls by touch,
    /// keeping the ring and the centred cover in agreement.
    func setFocusedTitleID(_ titleID: String?) {
        guard focusedTitleID != titleID else { return }
        focusedTitleID = titleID
    }

    var focusedGame: GameEntry? {
        guard let focusedTitleID else { return nil }
        return games.first { $0.titleID == focusedTitleID }
    }

    private var focusedIndex: Int? {
        guard let focusedTitleID else { return nil }
        return games.firstIndex { $0.titleID == focusedTitleID }
    }

    /// Which movement model applies, decided by the view's current presentation.
    enum FocusLayout {
        case list
        case grid
        case carousel
    }

    /// The view records its presentation here so the navigator, which cannot
    /// see the size class, moves focus correctly.
    var focusLayout: FocusLayout = .list

    /// A rename only changes a NSUserDefaults override, so the core has no new
    /// data to send — re-derive the entries from the snapshot it already gave
    /// us so the new name appears immediately.
    func refreshAfterRename() {
        games = Bridge.libraryEntries()
    }

    // MARK: - Core -> UI

    fileprivate func apply(games: [GameEntry], settings: EmulatorSettings) {
        self.games = games
        firmwareVersion = settings.firmwareVersion
        firmwareReady = settings.firmwareReady
    }

    fileprivate func setJITAvailable(_ available: Bool) {
        guard jitAvailable != available else { return }
        jitAvailable = available
    }

    /// Shows a toast for `duration` seconds. A second message replaces the
    /// first and restarts the timer rather than stacking.
    fileprivate func showStatus(_ message: String, duration: Duration = .seconds(6)) {
        statusMessage = message
        statusDismissal?.cancel()
        statusDismissal = Task { [weak self] in
            try? await Task.sleep(for: duration)
            guard !Task.isCancelled else { return }
            self?.statusMessage = nil
        }
    }

    fileprivate func setBusy(_ message: String?) {
        busyMessage = message
    }
}

/// Objective-C entry point for pushing library state in.
///
/// Split out for the same reason as FirmwareStateBridge: @Observable rewrites
/// stored properties into computed ones, which cannot be exposed to @objc.
@objc(TsubomiLibraryStateBridge)
@MainActor
final class LibraryStateBridge: NSObject {

    /// `games` is NSArray<TsubomiGameEntry *> * from bridge_games().
    @objc(updateWithGames:settings:)
    static func update(games: [GameEntry], settings: EmulatorSettings) {
        LibraryState.shared.apply(games: games, settings: settings)
    }

    @objc(setJITAvailable:)
    static func setJITAvailable(_ available: Bool) {
        LibraryState.shared.setJITAvailable(available)
    }

    @objc(showStatusMessage:)
    static func showStatus(_ message: String) {
        LibraryState.shared.showStatus(message)
    }

    /// Pass nil to dismiss.
    @objc(setBusyMessage:)
    static func setBusy(_ message: String?) {
        LibraryState.shared.setBusy(message)
    }

    /// Re-derive the entries from the core's last snapshot, for frontend-only
    /// changes the core has no new data for (a rename, a new custom cover).
    @objc static func refreshEntries() {
        LibraryState.shared.refreshAfterRename()
    }

    /// Drops cached cover art and forces every cell to reload it. Called after
    /// an install or a license import, which can make an icon appear at a path
    /// that was empty when it was last read.
    @objc static func invalidateArt() {
        Bridge.invalidateArt(atPath: nil)
        LibraryState.shared.bumpArtGeneration()
    }

    // MARK: - Game controller

    /// D-pad. `dx`/`dy` are -1, 0 or 1; the state maps them onto whichever
    /// presentation is showing.
    @objc(moveFocusByX:y:)
    static func moveFocus(x dx: Int, y dy: Int) {
        let state = LibraryState.shared
        state.moveFocus(dx: dx, dy: dy, layout: state.focusLayout)
    }

    /// Cross — launch the focused game, through the same firmware/JIT gating a
    /// tap goes through.
    @objc static func activateFocused() {
        LibraryState.shared.activateFocused()
    }

    /// Triangle — open the game-actions menu for the focused game.
    @objc static func showActionsForFocused() {
        LibraryState.shared.showActionsForFocused()
    }

    /// Circle, or the library going off screen.
    @objc static func clearFocus() {
        LibraryState.shared.clearFocus()
    }

    /// True when a game is focused, so the navigator knows whether Circle
    /// should clear the ring or be passed on.
    @objc static var hasFocus: Bool {
        LibraryState.shared.focusedTitleID != nil
    }
}
