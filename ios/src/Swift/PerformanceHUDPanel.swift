import SwiftUI

/// Which performance metrics the in-game overlay shows.
///
/// Reachable from the in-game menu so a metric can be turned on mid-session
/// without leaving the game. Writes the same UserDefaults keys as the settings
/// screen, so the two are always in agreement.
@MainActor
struct PerformanceHUDPanel: View {
    let onFinish: () -> Void

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    DefaultsToggle("Frame rate", key: .perfFPS, onEnable: unhide)
                    DefaultsToggle("Frametime", key: .perfFrametime, onEnable: unhide)
                    DefaultsToggle("Frametime graph", key: .perfFrametimeGraph, onEnable: unhide)
                    DefaultsToggle("Memory", key: .perfRAM, onEnable: unhide)
                    DefaultsToggle("Battery", key: .perfBattery, onEnable: unhide)
                } header: {
                    Text("Show")
                } footer: {
                    Text("The overlay appears as soon as any metric is on. Battery is reported by iOS in steps of a few percent.")
                }

                Section {
                    DefaultsToggle("Live log", key: .perfLog, onEnable: unhide)
                } footer: {
                    Text("Keeps the last few hundred log lines on screen. Useful when capturing a bug report.")
                }
            }
            .navigationTitle("Performance Overlay")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done", action: onFinish)
                }
            }
        }
        .presentationDetents([.medium])
        .presentationDragIndicator(.visible)
        .presentationBackground(.regularMaterial)
    }

    /// Turning any metric on clears the flag that hid the overlay wholesale,
    /// so enabling something here always makes it appear.
    private func unhide() {
        Bridge.performanceOverlayDidEnableMetric()
    }
}
