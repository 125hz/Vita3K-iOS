import SwiftUI

/// The game library: the app's home screen.
///
/// Three presentations of the same list — a grid, a list, and (landscape only,
/// from the grid) a centred cover carousel. Which one is showing is derived
/// from the persisted list/grid choice and the current size class, never
/// stored separately, so a rotation cannot leave the two disagreeing.
@MainActor
struct LibraryView: View {
    @State private var library = LibraryState.shared
    @Environment(\.verticalSizeClass) private var verticalSizeClass
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    /// Rename sheet target; nil when closed.
    @State private var renameTarget: GameEntry?
    /// Delete confirmation target.
    @State private var deleteTarget: GameEntry?

    /// Drives the refresh button's symbol animation; incremented per tap.
    @State private var refreshTick = 0
    /// Blocks repeat taps for the length of the animation.
    @State private var isRefreshing = false

    /// The carousel is the landscape presentation of grid mode. List mode
    /// stays a list in both orientations.
    private var showsCarousel: Bool {
        !library.isListMode && verticalSizeClass == .compact && !library.games.isEmpty
    }

    var body: some View {
        ZStack {
            // Opaque, edge to edge, behind absolutely everything.
            //
            // This is load-bearing, not cosmetic. set_metal_drawables_hidden
            // deliberately does NOT hide a Metal view the library lives inside
            // - hiding an ancestor would hide the library with it and black out
            // the screen - and SDL's drawable is exactly such an ancestor. So
            // the last frame of a quit game is still being rendered behind the
            // library, and the only thing stopping it showing through is the
            // library being fully opaque. The UIKit library painted its own
            // background for this reason; putting it on `content` inside the
            // NavigationStack was not enough, because the stack draws into
            // regions that background does not cover.
            Color(.systemBackground).ignoresSafeArea()

            NavigationStack {
                content
                    // Inline, centred title. iOS left-aligns the large title
                    // and offers no way to centre it; a centred principal item
                    // is the standard way to get a centred, still-prominent
                    // wordmark.
                    .navigationTitle("Tsubomi")
                    .navigationBarTitleDisplayMode(.inline)
                    .toolbar {
                        ToolbarItem(placement: .principal) {
                            Text("Tsubomi")
                                .font(.title2.weight(.bold))
                        }
                    }
                    .toolbar { toolbarContent }
                    .safeAreaInset(edge: .top, spacing: 0) { banners }
                    .overlay { busyOverlay }
                    // Keep the state's idea of the presentation in step with
                    // what is actually drawn, so pad D-pad movement matches
                    // what the user sees.
                    .onChange(of: showsCarousel, initial: true) { _, _ in
                        syncFocusLayout()
                    }
                    .onChange(of: library.isListMode) { _, _ in
                        syncFocusLayout()
                    }
            }
        }
        // Cross on the focused game routes through the same gating as a tap.
        .onChange(of: library.padLaunchTarget) { _, target in
            guard let target else { return }
            library.padLaunchTarget = nil
            launch(target)
        }
        .modifier(PadActionsDialog(target: $library.padActionsTarget, menu: gameMenu(for:)))
        .sheet(item: $renameTarget) { game in
            RenameSheet(game: game) { library.refreshAfterRename() }
        }
        .modifier(DeleteConfirmationDialog(target: $deleteTarget))
    }

    /// Keeps the state's idea of the presentation in step with what is drawn,
    /// so D-pad movement matches what the user sees.
    private func syncFocusLayout() {
        let next: LibraryState.FocusLayout = showsCarousel
            ? .carousel
            : (library.isListMode ? .list : .grid)
        guard next != library.focusLayout else { return }
        library.focusLayout = next
        // Drop the focus ring when the presentation changes. Rotating out of
        // the carousel otherwise left the last-centred game outlined in the
        // grid, which reads as a selection the user did not make - the ring
        // means "the controller is here", and after a rotation it is not.
        library.clearPadFocus()
    }

    // MARK: - Content

    @ViewBuilder
    private var content: some View {
        if library.games.isEmpty {
            ContentUnavailableView {
                Label("No Games", systemImage: "gamecontroller")
            } description: {
                Text("Tap + to import a game")
            }
        } else if showsCarousel {
            carouselContent
        } else if library.isListMode {
            listContent
        } else {
            gridContent
        }
    }

    /// Two-way with LibraryState so touch scrolling and D-pad focus agree.
    private var carouselFocus: Binding<String?> {
        Binding(
            get: { library.focusedTitleID },
            set: { library.setFocusedTitleID($0) }
        )
    }

    private var carouselContent: some View {
        CoverCarousel(
            games: library.games,
            dimmed: !library.firmwareReady,
            padFocusedTitleID: carouselFocus,
            stepAccumulator: library.carouselStepAccumulator,
            onLaunch: launch,
            menu: gameMenu(for:)
        )
    }

    /// Split out for the same type-checking reason as `gridCell`.
    private func listRow(_ game: GameEntry) -> some View {
        Button {
            launch(game)
        } label: {
            GameRow(game: game)
        }
        .buttonStyle(.plain)
        .opacity(library.firmwareReady ? 1 : 0.55)
        .contextMenu { gameMenu(for: game) }
        .padFocusRing(isFocused: library.focusedTitleID == game.titleID)
        .id(game.titleID)
    }

    @AppStorage(DefaultsKey.compactList.rawValue) private var compactList = false

    private var listContent: some View {
        // ScrollViewReader so the pad can bring its focused row into view;
        // List's own scrolling has no other way to be driven programmatically.
        ScrollViewReader { scroller in
            List(library.games) { game in
                listRow(game)
                    // Tighter insets in compact mode so the smaller rows pack
                    // closer together, which is the point of the density.
                    .listRowInsets(compactList
                        ? EdgeInsets(top: 2, leading: 16, bottom: 2, trailing: 16)
                        : EdgeInsets(top: 6, leading: 16, bottom: 6, trailing: 16))
            }
            .listStyle(.plain)
            .onChange(of: library.focusedTitleID) { _, focused in
                scrollToFocused(focused, using: scroller)
            }
        }
    }

    // Grid metrics, named so the layout and the pad's column arithmetic below
    // cannot drift apart.
    private static let gridMinimumWidth: CGFloat = 148
    private static let gridSpacing: CGFloat = 14
    private static let gridHorizontalPadding: CGFloat = 16

    private static let gridColumns = [
        GridItem(.adaptive(minimum: gridMinimumWidth), spacing: gridSpacing)
    ]

    /// Split out of `gridContent`, and annotated, because inferring the type of
    /// the whole ScrollViewReader/GeometryReader/ScrollView/LazyVGrid/ForEach
    /// nest in one expression defeats the type checker.
    private func gridCell(_ game: GameEntry) -> some View {
        Button {
            launch(game)
        } label: {
            GameCard(game: game)
        }
        .buttonStyle(.plain)
        .opacity(library.firmwareReady ? 1 : 0.55)
        .contextMenu { gameMenu(for: game) }
        .padFocusRing(isFocused: library.focusedTitleID == game.titleID)
        .id(game.titleID)
    }

    private var gridScroll: some View {
        ScrollView {
            LazyVGrid(columns: Self.gridColumns, spacing: 18) {
                ForEach(library.games) { game in
                    gridCell(game)
                }
            }
            .padding(.horizontal, Self.gridHorizontalPadding)
            .padding(.vertical, 12)
        }
    }

    private var gridContent: some View {
        ScrollViewReader { scroller in
            gridScroll
                // onGeometryChange rather than wrapping in a GeometryReader:
                // GeometryReader is greedy and ignores the safe area, which
                // collapsed the large navigation title and pushed the first
                // row up under the toolbar. This reads the same width without
                // taking part in layout.
                .onGeometryChange(for: CGFloat.self) { proxy in
                    proxy.size.width
                } action: { width in
                    library.gridColumnCount = Self.columnCount(forWidth: width)
                }
                .onChange(of: library.focusedTitleID) { _, focused in
                    scrollToFocused(focused, using: scroller)
                }
        }
    }

    private static func columnCount(forWidth width: CGFloat) -> Int {
        let usable = width - gridHorizontalPadding * 2 + gridSpacing
        return max(1, Int(usable / (gridMinimumWidth + gridSpacing)))
    }

    private func scrollToFocused(_ focused: String?, using scroller: ScrollViewProxy) {
        guard let focused else { return }
        if reduceMotion {
            scroller.scrollTo(focused, anchor: .center)
        } else {
            withAnimation(.snappy) { scroller.scrollTo(focused, anchor: .center) }
        }
    }

    // MARK: - Chrome

    @ToolbarContentBuilder
    private var toolbarContent: some ToolbarContent {
        // Bottom bar rather than the navigation bar: these are the library's
        // primary actions, and on a phone held one-handed the bottom edge is
        // the reachable one. The system gives the group its own Liquid Glass
        // container here.
        ToolbarItemGroup(placement: .bottomBar) {
            Menu {
                Button {
                    // Importing a game before firmware exists produces a title
                    // that cannot boot, so the gate lives here rather than at
                    // launch time only.
                    if Bridge.firmwareReadyOrPresentAlert() {
                        Bridge.presentGameImportPicker()
                    }
                } label: {
                    Label("Import game (.vpk / .zip / .pkg)", systemImage: "arrow.down.doc")
                }
                Button {
                    Bridge.presentLicenseImportPicker()
                } label: {
                    Label("Import license (work.bin)", systemImage: "key.fill")
                }
                Button {
                    Bridge.presentFirmwareImportPicker()
                } label: {
                    Label("Import firmware (.PUP)", systemImage: "cpu")
                }
            } label: {
                Label("Add", systemImage: "plus")
            }

            Button {
                library.isListMode.toggle()
            } label: {
                Label(
                    library.isListMode ? "Grid view" : "List view",
                    systemImage: library.isListMode ? "square.grid.2x2" : "list.bullet"
                )
            }

            Button {
                refresh()
            } label: {
                Label("Refresh", systemImage: "arrow.clockwise")
            }
            // The rescan is usually instant, so without feedback the button
            // looks inert. Spinning the glyph and following with a toast makes
            // it clear something happened.
            .symbolEffect(.rotate, value: refreshTick)
            .disabled(isRefreshing)

            // The graphics-help button is gone: the same explanation now lives
            // in Settings, next to the switches it talks about.

            Button {
                Bridge.presentGlobalSettings()
            } label: {
                Label("Settings", systemImage: "gearshape.fill")
            }
        }

        // The firmware version indicator that used to sit here is gone: it is
        // shown in Settings > About, and the library header is worth more as
        // space for covers.
    }

    // Its own View so a toast appearing or auto-dismissing invalidates only
    // this bar, not the whole library body (which would otherwise re-derive
    // the grid/list/carousel content on every toast timeout).
    private var banners: some View {
        LibraryBanners(library: library)
    }

    @ViewBuilder
    private var busyOverlay: some View {
        if let message = library.busyMessage {
            ZStack {
                // Blocks interaction with the list underneath while a boot,
                // import or delete is in flight.
                Color.black.opacity(0.35).ignoresSafeArea()
                VStack(spacing: 14) {
                    ProgressView()
                    Text(message)
                        .font(.subheadline)
                        .multilineTextAlignment(.center)
                }
                .padding(24)
                .background(.regularMaterial, in: .rect(cornerRadius: 20, style: .continuous))
                .padding(40)
            }
            .transition(.opacity)
        }
    }

    // MARK: - Per-game menu

    @ViewBuilder
    private func gameMenu(for game: GameEntry) -> some View {
        Button {
            Bridge.presentSaveImportPicker(titleID: game.titleID)
        } label: {
            Label("Import save", systemImage: "square.and.arrow.down")
        }
        Button {
            Bridge.exportSave(titleID: game.titleID)
        } label: {
            Label("Export save", systemImage: "square.and.arrow.up")
        }
        Button {
            renameTarget = game
        } label: {
            Label("Rename title", systemImage: "pencil")
        }
        Button {
            Bridge.requestTrophies(titleID: game.titleID)
        } label: {
            Label("View trophies", systemImage: "trophy.fill")
        }
        Button {
            Bridge.presentSettings(forTitle: game.titleID, displayName: game.displayTitle)
        } label: {
            Label("Game settings", systemImage: "slider.horizontal.3")
        }
        Button {
            Bridge.presentCoverPicker(titleID: game.titleID)
        } label: {
            Label("Custom cover art", systemImage: "photo")
        }
        if game.hasSettingsOverrides {
            Button {
                Bridge.resetSettings(forTitle: game.titleID)
                Bridge.refreshLibrary()
            } label: {
                Label("Use global settings", systemImage: "arrow.uturn.backward.circle")
            }
        }
        // Always offered, not only for custom covers: the crop editor falls
        // back to the game's packaged art, so the built-in cover can be
        // reframed too. This matched the old menu and regressed when it was
        // gated on hasCustomCover.
        Button {
            Bridge.presentCoverCrop(titleID: game.titleID)
        } label: {
            Label("Adjust cover crop", systemImage: "crop")
        }
        if game.hasCustomCover {
            Button {
                Bridge.resetCoverArt(titleID: game.titleID)
            } label: {
                Label("Reset cover art", systemImage: "arrow.uturn.backward")
            }
        }
        // Spelled out rather than Button(_:systemImage:role:action:) so the
        // destructive button matches the label-closure form used by the rest
        // of this menu.
        Button(role: .destructive) {
            deleteTarget = game
        } label: {
            Label("Delete game", systemImage: "trash")
        }
    }

    // MARK: - Actions

    /// Rescans installed titles, with visible feedback.
    ///
    /// The core replaces the library wholesale and usually finishes before the
    /// next frame, so there is nothing to wait on - the delay here exists only
    /// so the spin is perceptible rather than a single flicker.
    private func refresh() {
        guard !isRefreshing else { return }
        isRefreshing = true
        refreshTick += 1
        Bridge.refreshLibrary()
        Task {
            try? await Task.sleep(for: .milliseconds(650))
            isRefreshing = false
            library.showRefreshedToast()
        }
    }

    private func launch(_ game: GameEntry) {
        guard Bridge.firmwareReadyOrPresentAlert() else { return }
        guard library.jitAvailable else {
            // Guest execution needs JIT; refuse the boot and explain, rather
            // than letting the launch path hit the missing-debugger crash.
            Bridge.presentJITRequiredAlert()
            return
        }
        guard library.beginLaunch(game) else { return }
        Bridge.launch(titleID: game.titleID)
    }
}

// The two item-driven dialogs are modifiers rather than inline calls on `body`.
// Each needs a Binding<Bool> synthesised from an optional plus a ViewBuilder,
// and inlining both pushed `body` past what the type checker will solve.

/// Triangle on the focused game: the same actions as the long-press menu.
private struct PadActionsDialog<Menu: View>: ViewModifier {
    @Binding var target: GameEntry?
    @ViewBuilder let menu: (GameEntry) -> Menu

    private var isPresented: Binding<Bool> {
        Binding(get: { target != nil }, set: { if !$0 { target = nil } })
    }

    func body(content: Content) -> some View {
        content.confirmationDialog(
            target?.displayTitle ?? "",
            isPresented: isPresented,
            titleVisibility: .visible
        ) {
            if let target {
                menu(target)
            }
        }
    }
}

private struct DeleteConfirmationDialog: ViewModifier {
    @Binding var target: GameEntry?

    private var isPresented: Binding<Bool> {
        Binding(get: { target != nil }, set: { if !$0 { target = nil } })
    }

    private var title: String {
        target.map { "Delete \($0.displayTitle)?" } ?? ""
    }

    func body(content: Content) -> some View {
        content.confirmationDialog(title, isPresented: isPresented, titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                if let target {
                    Bridge.delete(titleID: target.titleID)
                }
                target = nil
            }
        } message: {
            Text("The installed game, its update, and DLC are removed from this device. Saves and trophies are kept.")
        }
    }
}

/// The JIT-warning banner and status toast, split out of LibraryView so a
/// toast timing out re-renders only this bar rather than the whole library.
@MainActor
private struct LibraryBanners: View {
    var library: LibraryState

    var body: some View {
        VStack(spacing: 8) {
            if !library.jitAvailable {
                // Yellow-tinted glass: adaptive tinting marks a single urgent
                // element, and a warning nobody can act around is exactly that.
                Label(
                    "JIT is not ready — open StikDebug, enable JIT, and keep it attached until Tsubomi finishes Preparing JIT.",
                    systemImage: "exclamationmark.triangle.fill"
                )
                .font(.subheadline.weight(.semibold))
                .padding(12)
                .frame(maxWidth: .infinity, alignment: .leading)
                .glassEffect(.regular.tint(.yellow), in: .rect(cornerRadius: 16, style: .continuous))
            }
            if let status = library.statusMessage {
                Text(status)
                    .font(.subheadline)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                    .glassEffect(.regular, in: .capsule)
                    .transition(.opacity)
            }
        }
        .padding(.horizontal, 16)
        .animation(.snappy, value: library.statusMessage)
        .animation(.snappy, value: library.jitAvailable)
    }
}
