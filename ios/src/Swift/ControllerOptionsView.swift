import SwiftUI

/// On-screen controller settings: opacity, size, behaviour, which controls are
/// shown, and the entry point to the layout editor.
///
/// A stock grouped Form, so it matches the app's other settings screens rather
/// than being a bespoke panel as the UIKit version was.
@MainActor
struct ControllerOptionsView: View {
    @State private var model = ControlsModel.shared
    /// Called when the user chooses to reposition controls: the presenter has
    /// to dismiss this sheet so the overlay underneath is reachable.
    let onEditLayout: () -> Void
    let onFinish: () -> Void

    @State private var confirmingReset = false

    /// Which layout the visibility switches apply to. Positions and visibility
    /// are stored per orientation, so editing one must not silently change the
    /// other — this makes which is being edited explicit.
    @State private var editingOrientation: Orientation = .landscape

    private enum Orientation: String, CaseIterable, Identifiable {
        case landscape, portrait
        var id: String { rawValue }
        var title: String { self == .landscape ? "Landscape" : "Portrait" }
        /// A representative size, so ControlsModel resolves the right layout.
        var probeSize: CGSize {
            self == .landscape ? CGSize(width: 2, height: 1) : CGSize(width: 1, height: 2)
        }
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    LabeledContent("Opacity") {
                        Text(percent(model.opacity))
                            .foregroundStyle(.secondary)
                            .monospacedDigit()
                    }
                    Slider(value: $model.opacity, in: 0.15...1.0) {
                        Text("Opacity")
                    } minimumValueLabel: {
                        Image(systemName: "circle.lefthalf.filled").font(.caption2)
                    } maximumValueLabel: {
                        Image(systemName: "circle.fill").font(.caption2)
                    }

                    LabeledContent("Size") {
                        Text(percent(model.scale))
                            .foregroundStyle(.secondary)
                            .monospacedDigit()
                    }
                    Slider(value: $model.scale, in: 0.65...1.45) {
                        Text("Size")
                    } minimumValueLabel: {
                        Image(systemName: "textformat.size.smaller").font(.caption2)
                    } maximumValueLabel: {
                        Image(systemName: "textformat.size.larger").font(.caption2)
                    }
                } header: {
                    Text("Appearance")
                } footer: {
                    Text("Changes apply live to the controls behind this sheet.")
                }

                Section("Behaviour") {
                    Toggle("Hide for physical controller", isOn: $model.hideWhenPhysical)
                    Toggle("Haptic feedback", isOn: $model.haptics)
                    Toggle("Alignment guides", isOn: $model.snapGuides)
                }

                Section {
                    Button {
                        onEditLayout()
                    } label: {
                        Label("Reposition Controls", systemImage: "arrow.up.and.down.and.arrow.left.and.right")
                    }
                    Button(role: .destructive) {
                        confirmingReset = true
                    } label: {
                        Label("Reset Layout", systemImage: "arrow.uturn.backward")
                    }
                } header: {
                    Text("Layout")
                } footer: {
                    Text("Positions are stored separately for landscape and portrait.")
                }

                visibilitySection
            }
            .navigationTitle("Controls")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done", action: onFinish)
                }
            }
            .confirmationDialog("Reset the control layout?",
                                isPresented: $confirmingReset,
                                titleVisibility: .visible) {
                Button("Reset Layout", role: .destructive) {
                    model.resetLayout()
                }
            } message: {
                Text("Both the landscape and portrait layouts return to their defaults.")
            }
        }
        .presentationDetents([.medium, .large])
        .presentationDragIndicator(.visible)
        .presentationBackground(.regularMaterial)
    }

    private var visibilitySection: some View {
        Section {
            Picker("Layout", selection: $editingOrientation) {
                ForEach(Orientation.allCases) { orientation in
                    Text(orientation.title).tag(orientation)
                }
            }
            .pickerStyle(.segmented)

            ForEach(ControlsModel.definitions) { definition in
                Toggle(definition.accessibilityLabel, isOn: visibilityBinding(for: definition))
            }
        } header: {
            Text("Visible Controls")
        } footer: {
            Text("Turn off any control a game does not use. The Vita touchscreen still works through the gaps between controls.")
        }
    }

    private func visibilityBinding(for definition: ControlDefinition) -> Binding<Bool> {
        let size = editingOrientation.probeSize
        return Binding(
            get: { model.isVisible(definition.id, in: size) },
            set: { model.setVisible($0, for: definition.id, in: size) }
        )
    }

    private func percent(_ value: Double) -> String {
        "\(Int((value * 100).rounded()))%"
    }
}
