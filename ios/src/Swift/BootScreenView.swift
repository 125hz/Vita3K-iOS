import SwiftUI
import UIKit

/// Shown from the moment SDL has a window until the library is ready.
///
/// Startup does real work before the library can appear — reserving the 4GB
/// guest arena, allocating the JIT region pool, scanning installed titles — and
/// until now the user watched an unexplained black screen for several seconds.
/// The system launch screen cannot help: it is static and is dismissed as soon
/// as the process is alive, long before any of that finishes.
@MainActor
struct BootScreenView: View {
    /// Cycles the tagline so a long start reads as progress rather than a hang.
    @State private var phase = 0
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private static let phases = [
        "Starting Tsubomi",
        "Preparing memory",
        "Reading your library",
    ]

    var body: some View {
        ZStack {
            Color(.systemBackground).ignoresSafeArea()

            VStack(spacing: 22) {
                Image(systemName: "camera.macro")
                    .font(.system(size: 64))
                    .foregroundStyle(.pink)
                    .symbolEffect(.pulse)
                    .accessibilityHidden(true)

                Text("Tsubomi")
                    .font(.largeTitle.weight(.bold))

                VStack(spacing: 10) {
                    ProgressView()
                    Text(Self.phases[min(phase, Self.phases.count - 1)])
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .contentTransition(.opacity)
                }
            }
        }
        .task {
            // Advances on a plain sleep loop rather than a Combine timer
            // publisher: Task.sleep throws on cancellation, so this unwinds
            // cleanly when the boot screen is replaced. An autoconnected
            // Timer publisher keeps firing into a view that is on its way out.
            //
            // The stages are time-based, not real progress: the startup work
            // happens on the emulator thread and reports none. The labels
            // follow the order things actually happen in, and the screen is
            // replaced the moment the library is ready whichever is showing.
            while phase < Self.phases.count - 1 {
                do {
                    try await Task.sleep(for: .milliseconds(1400))
                } catch {
                    return
                }
                if reduceMotion {
                    phase += 1
                } else {
                    withAnimation(.snappy) { phase += 1 }
                }
            }
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Starting Tsubomi")
    }
}

@objc(TsubomiBootScreenHost)
@MainActor
final class BootScreenHost: NSObject {
    @objc static func bootScreenViewController() -> UIViewController {
        let controller = UIHostingController(rootView: BootScreenView())
        // Opaque: it covers SDL's freshly created, still-black drawable.
        controller.view.backgroundColor = .systemBackground
        controller.view.isOpaque = true
        return controller
    }
}
