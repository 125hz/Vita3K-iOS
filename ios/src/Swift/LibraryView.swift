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

    /// Rename sheet target; nil when closed.
    @State private var renameTarget: GameEntry?
    /// Delete confirmation target.
    @State private var deleteTarget: GameEntry?

    /// The carousel is the landscape presentation of grid mode. List mode
    /// stays a list in both orientations.
    private var showsCarousel: Bool {
        !library.isListMode && verticalSizeClass == .compact && !library.games.isEmpty
    }

    var body: some View {
        NavigationStack {
            content
                .navigationTitle("Tsubomi")
                .navigationBarTitleDisplayMode(showsCarousel ? .inline : .large)
                .toolbar { toolbarContent }
                .safeAreaInset(edge: .top, spacing: 0) { banners }
                .overlay { busyOverlay }
                // Keep the state's idea of the presentation in step with what
                // is actually drawn, so pad D-pad movement matches what the
                // user sees.
                .onChange(of: showsCarousel, initial: true) { _, carousel in
                    library.focusLayout = carousel ? .carousel
                        : library.isListMode ? .list : .grid
                }
                .onChange(of: library.isListMode) { _, list in
                    library.focusLayout = showsCarousel ? .carousel : (list ? .list : .grid)
                }
        }
        // Cross on the focused game routes through the same gating as a tap.
        .onChange(of: library.padLaunchTarget) { _, target in
            guard let target else { return }
            library.padLaunchTarget = nil
            launch(target)
        }
        // Triangle opens the same actions as the long-press menu.
        .confirmationDialog(
            library.padActionsTarget?.displayTitle ?? "",
            isPresented: Binding(
                get: { library.padActionsTarget != nil },
                set: { if !$0 { library.padActionsTarget = nil } }
            ),
            titleVisibility: .visible
        ) {
            if let game = library.padActionsTarget {
                gameMenu(for: game)
            }
        }
        .sheet(item: $renameTarget) { game in
            RenameSheet(game: game) { library.refreshAfterRename() }
        }
        .confirmationDialog(
            deleteTarget.map { "Delete \($0.displayTitle)?" } ?? "",
            isPresented: Binding(
                get: { deleteTarget != nil },
                set: { if !$0 { deleteTarget = nil } }
            ),
            titleVisibility: .visible
        ) {
            Button("Delete", role: .destructive) {
                if let game = deleteTarget {
                    Bridge.delete(titleID: game.titleID)
                }
                deleteTarget = nil
            }
        } message: {
            Text("The installed game, its update, and DLC are removed from this device. Saves and trophies are kept.")
        }
    }

    // MARK: - Content

    @ViewBuilder
    private var content: some View {
        if library.games.isEmpty {
            ContentUnavailableView {
                Label("No Games", systemImage: "gamecontroller")
            } description: {
                Text("Use + to import a game you legally own (.vpk, .zip or .pkg).")
            }
        } else if showsCarousel {
            CoverCarousel(
                games: library.games,
                dimmed: !library.firmwareReady,
                padFocusedTitleID: Binding(
                    get: { library.focusedTitleID },
                    set: { library.setFocusedTitleID($0) }
                )
            ) { game in
                launch(game)
            } menu: { game in
                gameMenu(for: game)
            }
        } else if library.isListMode {
            listContent
        } else {
            gridContent
        }
    }

    private var listContent: some View {
        // ScrollViewReader so the pad can bring its focused row into view;
        // List's own scrolling has no other way to be driven programmatically.
        ScrollViewReader { scroller in
            List(library.games) { game in
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
            .listStyle(.plain)
            .onChange(of: library.focusedTitleID) { _, focused in
                guard let focused else { return }
                withAnimation(.snappy) { scroller.scrollTo(focused, anchor: .center) }
            }
        }
    }

    private var gridContent: some View {
        ScrollViewReader { scroller in
            GeometryReader { proxy in
                ScrollView {
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 148), spacing: 14)], spacing: 18) {
                        ForEach(library.games) { game in
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
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 12)
                }
                // The pad needs the column count to move up/down by a row.
                // Derived from the same minimum and spacing the adaptive
                // GridItem uses, so the two cannot disagree.
                .onChange(of: proxy.size.width, initial: true) { _, width in
                    library.gridColumnCount = max(1, Int((width - 32 + 14) / (148 + 14)))
                }
            }
            .onChange(of: library.focusedTitleID) { _, focused in
                guard let focused else { return }
                withAnimation(.snappy) { scroller.scrollTo(focused, anchor: .center) }
            }
        }
    }

    // MARK: - Chrome

    @ToolbarContentBuilder
    private var toolbarContent: some ToolbarContent {
        ToolbarItemGroup(placement: .topBarTrailing) {
            Menu {
                Button("Import game (.vpk / .zip / .pkg)", systemImage: "arrow.down.doc") {
                    // Importing a game before firmware exists produces a title
                    // that cannot boot, so the gate lives here rather than at
                    // launch time only.
                    if Bridge.firmwareReadyOrPresentAlert() {
                        Bridge.presentGameImportPicker()
                    }
                }
                Button("Import license (work.bin)", systemImage: "key.fill") {
                    Bridge.presentLicenseImportPicker()
                }
                Button("Import firmware (.PUP)", systemImage: "cpu") {
                    Bridge.presentFirmwareImportPicker()
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

            Button("Refresh", systemImage: "arrow.clockwise") {
                Bridge.refreshLibrary()
            }

            Button("Graphics help", systemImage: "questionmark.circle") {
                Bridge.presentGraphicsHelp()
            }

            Button("Settings", systemImage: "gearshape.fill") {
                Bridge.presentGlobalSettings()
            }
        }

        ToolbarItem(placement: .status) {
            if !library.firmwareVersion.isEmpty {
                Text(library.firmwareVersion)
                    .font(.caption2.monospaced())
                    .foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder
    private var banners: some View {
        VStack(spacing: 8) {
            if !library.jitAvailable {
                // The one surface here that earns a tint: adaptive tinting is
                // meant to mark a single urgent element, and a warning nobody
                // can act around is exactly that.
                Label(
                    "JIT is not ready — open StikDebug, enable JIT, and keep it attached until Tsubomi finishes Preparing JIT.",
                    systemImage: "exclamationmark.triangle.fill"
                )
                .font(.subheadline.weight(.semibold))
                .padding(12)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(.yellow.opacity(0.3), in: .rect(cornerRadius: 16, style: .continuous))
            }
            if let status = library.statusMessage {
                Text(status)
                    .font(.subheadline)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                    .background(.regularMaterial, in: .capsule)
                    .transition(.opacity)
            }
        }
        .padding(.horizontal, 16)
        .animation(.snappy, value: library.statusMessage)
        .animation(.snappy, value: library.jitAvailable)
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
        Button("Import save", systemImage: "square.and.arrow.down") {
            Bridge.presentSaveImportPicker(titleID: game.titleID)
        }
        Button("Export save", systemImage: "square.and.arrow.up") {
            Bridge.exportSave(titleID: game.titleID)
        }
        Button("Rename title", systemImage: "pencil") {
            renameTarget = game
        }
        Button("View trophies", systemImage: "trophy.fill") {
            Bridge.requestTrophies(titleID: game.titleID)
        }
        Button("Game settings", systemImage: "slider.horizontal.3") {
            Bridge.presentSettings(forTitle: game.titleID, displayName: game.displayTitle)
        }
        Button("Custom cover art", systemImage: "photo") {
            Bridge.presentCoverPicker(titleID: game.titleID)
        }
        if game.hasSettingsOverrides {
            Button("Use global settings", systemImage: "arrow.uturn.backward.circle") {
                Bridge.resetSettings(forTitle: game.titleID)
                Bridge.refreshLibrary()
            }
        }
        if game.hasCustomCover {
            Button("Adjust cover crop", systemImage: "crop") {
                Bridge.presentCoverCrop(titleID: game.titleID)
            }
        }
        // Spelled out rather than Button(_:systemImage:role:action:), which is
        // newer than this app's iOS 17 deployment target.
        Button(role: .destructive) {
            deleteTarget = game
        } label: {
            Label("Delete game", systemImage: "trash")
        }
    }

    // MARK: - Actions

    private func launch(_ game: GameEntry) {
        guard Bridge.firmwareReadyOrPresentAlert() else { return }
        guard library.jitAvailable else {
            // Guest execution needs JIT; refuse the boot and explain, rather
            // than letting the launch path hit the missing-debugger crash.
            Bridge.presentJITRequiredAlert()
            return
        }
        Bridge.launch(titleID: game.titleID)
    }
}
