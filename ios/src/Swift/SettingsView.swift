import SwiftUI

/// The settings screen.
///
/// Deliberately plain: a `Form` in a `NavigationStack`, built from stock
/// `Toggle`/`Slider`/`Picker` rows. On iOS 26 that is what produces correct
/// Liquid Glass — the navigation bar and any sheet chrome are glass, and the
/// content underneath is opaque and scrolls beneath it. Hand-rolling glass
/// behind list rows (which the UIKit screen used to do) both fights the design
/// system and costs a refraction pass per visible row.
///
/// Everything here is a system control, so Dynamic Type, VoiceOver labels and
/// values, high-contrast, and reduce-transparency are all inherited rather than
/// reimplemented.
@MainActor
struct SettingsView: View {
    @State private var model: SettingsModel
    /// Invoked when the user is done; the host controller dismisses.
    private let onFinish: () -> Void

    @State private var showingResetConfirmation = false

    init(scope: SettingsModel.Scope, onFinish: @escaping () -> Void) {
        _model = State(initialValue: SettingsModel(scope: scope))
        self.onFinish = onFinish
    }

    var body: some View {
        NavigationStack {
            Form {
                videoSection
                graphicsSection
                audioSection
                if !model.isPerGame {
                    controlsSection
                    performanceOverlaySection
                    librarySection
                    firmwareSection
                }
                if model.isPerGame {
                    perGameResetSection
                }
            }
            .navigationTitle(model.navigationTitle)
            .navigationBarTitleDisplayMode(model.isPerGame ? .inline : .large)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") {
                        model.save()
                        onFinish()
                    }
                }
            }
        }
    }

    // MARK: - Sections

    private var videoSection: some View {
        Section("Video") {
            Toggle("V-Sync", isOn: $model.vSync)
                .accessibilityHint("Synchronizes presentation to the display.")
        }
    }

    private var graphicsSection: some View {
        Section {
            // A labelled slider rather than a stepper: the multiplier is
            // continuous and the exact number matters less than the direction.
            LabeledContent("Resolution") {
                Text(model.resolutionLabel)
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
            }
            Slider(value: $model.resolutionMultiplier, in: 0.5...2.0, step: 0.25) {
                Text("Resolution multiplier")
            } minimumValueLabel: {
                Text("0.5×").font(.caption2)
            } maximumValueLabel: {
                Text("2×").font(.caption2)
            }
            .accessibilityValue(model.resolutionLabel)

            Toggle("High accuracy", isOn: $model.highAccuracy)
            Toggle("Surface sync", isOn: $model.surfaceSync)
            Toggle("Double buffer", isOn: $model.doubleBuffer)
            Toggle("Async pipeline compilation", isOn: $model.asyncPipelineCompilation)

            Picker("Anisotropic filtering", selection: $model.anisotropicFiltering) {
                ForEach(SettingsModel.anisotropicOptions, id: \.self) { value in
                    Text(SettingsModel.anisotropicLabel(value)).tag(value)
                }
            }
        } header: {
            Text("Graphics")
        }
    }

    private var audioSection: some View {
        Section {
            Toggle("NGS audio", isOn: $model.ngsAudio)
            LabeledContent("Audio backend", value: "SDL")
            Toggle("CPU optimizations", isOn: $model.cpuOptimizations)
        } header: {
            Text("Audio & CPU")
        } footer: {
            Text("NGS is full Vita audio emulation; disable it only while diagnosing a problem.")
        }
    }

    private var controlsSection: some View {
        Section {
            Button("Virtual controls…") {
                // Still the UIKit layout editor: it drags live control views
                // around the on-screen overlay, which has no SwiftUI analogue
                // yet. Presented over this sheet.
                Bridge.presentControllerOptions()
            }
            NavigationLink("Face button layout") {
                FaceButtonLayoutView(model: model)
            }
        } header: {
            Text("Controls")
        } footer: {
            Text("Virtual controls covers opacity, scale, layout, visibility, and physical-pad auto-hide. Some third-party controllers report face buttons in Xbox-style positions; remap them if the wrong button responds.")
        }
    }

    private var performanceOverlaySection: some View {
        Section {
            DefaultsToggle("Show FPS", key: .perfFPS, onEnable: enablePerfOverlay)
            DefaultsToggle("Show frametime", key: .perfFrametime, onEnable: enablePerfOverlay)
            DefaultsToggle("Show frametime graph", key: .perfFrametimeGraph, onEnable: enablePerfOverlay)
            DefaultsToggle("Show RAM usage", key: .perfRAM, onEnable: enablePerfOverlay)
            DefaultsToggle("Show battery %", key: .perfBattery, onEnable: enablePerfOverlay)
            DefaultsToggle("Show live log", key: .perfLog, onEnable: enablePerfOverlay)
        } header: {
            Text("Performance overlay")
        } footer: {
            Text("The overlay appears in-game once any metric is enabled. The live log keeps the last ~250 lines, which is useful when reporting a bug.")
        }
    }

    private var librarySection: some View {
        Section {
            DefaultsToggle("Show title IDs", key: .showTitleIDs, onChange: Bridge.reloadLibraryCells)
            DefaultsToggle("Show version number", key: .showVersion, onChange: Bridge.reloadLibraryCells)
            DefaultsToggle("Show game size", key: .showGameSize, onChange: Bridge.reloadLibraryCells)
        } header: {
            Text("Library")
        }
    }

    /// Enabling any metric un-hides an overlay the user dismissed in-game.
    private func enablePerfOverlay() {
        Bridge.performanceOverlayDidEnableMetric()
    }

    private var firmwareSection: some View {
        Section {
            LabeledContent("Firmware") {
                Text(model.firmwareVersion.isEmpty ? "Not installed" : model.firmwareVersion)
                    .foregroundStyle(.secondary)
            }
            if !model.firmwareReady && !model.missingFirmware.isEmpty {
                // A plain label, not an alert: this is steady-state
                // information, and the library already blocks launching.
                Label(model.missingFirmware, systemImage: "exclamationmark.triangle")
                    .foregroundStyle(.secondary)
            }
            LabeledContent("Version", value: AppInfo.versionDisplay)
            NavigationLink("What's New") {
                ChangelogView()
            }
            Button("Report a bug") {
                Bridge.openBugReportForm()
            }
            Button("Forked from Vita3K") {
                Bridge.open(urlString: "https://github.com/Vita3K/Vita3K")
            }
            Button("Developed by @halcyonpalace") {
                Bridge.open(urlString: "https://x.com/halcyonpalace")
            }
        } header: {
            Text("About")
        }
    }

    private var perGameResetSection: some View {
        Section {
            Button("Use global settings", role: .destructive) {
                showingResetConfirmation = true
            }
            .confirmationDialog(
                "Remove this game's custom settings?",
                isPresented: $showingResetConfirmation,
                titleVisibility: .visible
            ) {
                Button("Use global settings", role: .destructive) {
                    model.resetPerGameOverrides()
                    onFinish()
                }
            } message: {
                Text("This game will follow the global settings the next time it launches.")
            }
        } footer: {
            Text("These settings apply only to this game and take effect the next time it launches.")
        }
    }
}

/// Face-button remapping, split out as its own screen: four related pickers is
/// more than a section should carry, and it keeps the root list short.
@MainActor
private struct FaceButtonLayoutView: View {
    @Bindable var model: SettingsModel

    private static let positions = ["Bottom", "Right", "Left", "Top"]

    var body: some View {
        Form {
            Section {
                picker("Cross", selection: $model.bindCross)
                picker("Circle", selection: $model.bindCircle)
                picker("Square", selection: $model.bindSquare)
                picker("Triangle", selection: $model.bindTriangle)
            } footer: {
                Text("Choose which physical button position triggers each Vita button.")
            }
        }
        .navigationTitle("Face Buttons")
        .navigationBarTitleDisplayMode(.inline)
    }

    private func picker(_ label: String, selection: Binding<Int>) -> some View {
        Picker(label, selection: selection) {
            ForEach(Array(Self.positions.enumerated()), id: \.offset) { index, name in
                Text(name).tag(index)
            }
        }
    }
}
