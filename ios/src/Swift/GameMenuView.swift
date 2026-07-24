import SwiftUI

/// The in-game menu, reached from the floating button on the controller
/// overlay.
///
/// A sheet with detents rather than a hand-built card: it comes with the
/// system's own Liquid Glass chrome, drag-to-dismiss, and correct behaviour
/// under Reduce Motion — none of which the UIKit overlay's bespoke panel had.
@MainActor
struct GameMenuView: View {
    let onResume: () -> Void
    let onEditLayout: () -> Void
    let onTrophies: () -> Void
    let onPerformanceHUD: () -> Void
    let onHideMenuButton: () -> Void
    let onQuit: () -> Void

    @State private var confirmingQuit = false
    @AppStorage("tsubomi.orientationLockEnabled")
    private var orientationLockEnabled = false
    @AppStorage("tsubomi.orientationLock")
    private var orientationLock = "portrait"

    var body: some View {
        NavigationStack {
            List {
                Section {
                    row("Resume", subtitle: nil, symbol: "play.fill", action: onResume)
                }
                Section {
                    row("Layout Options",
                        subtitle: "Reposition controls, visibility, scale and opacity",
                        symbol: "gamecontroller.fill",
                        action: onEditLayout)
                    row("Trophies",
                        subtitle: "Progress and unlock dates",
                        symbol: "trophy.fill",
                        action: onTrophies)
                    row("Performance Overlay",
                        subtitle: "FPS, frametime, memory and battery",
                        symbol: "gauge.with.dots.needle.67percent",
                        action: onPerformanceHUD)
                    Toggle(isOn: portraitLock) {
                        Label("Portrait Lock", systemImage: "iphone")
                    }
                    row("Hide Menu Button",
                        subtitle: "Restore it with a three-finger tap",
                        symbol: "eye.slash.fill",
                        action: onHideMenuButton)
                }
                Section {
                    Button(role: .destructive) {
                        confirmingQuit = true
                    } label: {
                        Label("Quit Game", systemImage: "rectangle.portrait.and.arrow.right")
                    }
                } footer: {
                    Text("Quitting returns to the library. Save inside the game first — Tsubomi does not save its state for you.")
                }
            }
            .navigationTitle("Game")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done", action: onResume)
                }
            }
            .confirmationDialog("Quit to the library?",
                                isPresented: $confirmingQuit,
                                titleVisibility: .visible) {
                Button("Quit Game", role: .destructive, action: onQuit)
            } message: {
                // Losing unsaved progress is not recoverable, so this asks
                // first. The UIKit menu quit immediately.
                Text("Any progress since your last in-game save will be lost.")
            }
        }
        .presentationDetents([.medium, .large])
        .presentationDragIndicator(.visible)
        // The game is still rendering behind this; a glass background keeps it
        // visible rather than hiding it behind an opaque sheet.
        .presentationBackground(.regularMaterial)
    }

    private func row(
        _ title: String,
        subtitle: String?,
        symbol: String,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            HStack(spacing: 12) {
                Image(systemName: symbol)
                    .font(.body)
                    .foregroundStyle(.tint)
                    .frame(width: 28)
                VStack(alignment: .leading, spacing: 1) {
                    Text(title)
                        .foregroundStyle(.primary)
                    if let subtitle {
                        Text(subtitle)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                Spacer(minLength: 0)
            }
        }
        .accessibilityElement(children: .combine)
    }

    private var portraitLock: Binding<Bool> {
        Binding(
            get: {
                orientationLockEnabled && orientationLock == "portrait"
            },
            set: { enabled in
                if enabled {
                    orientationLock = "portrait"
                    Bridge.applyOrientationLock("portrait")
                }
                orientationLockEnabled = enabled
                Bridge.setOrientationLockEnabled(enabled)
            }
        )
    }
}
