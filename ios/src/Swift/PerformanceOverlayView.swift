import SwiftUI

/// Live performance readout shown over a running game.
///
/// Values are pushed in from the emulator loop about once a second rather than
/// polled, so this view does no timer work of its own — it is the screen the
/// device sits on for hours at a time, and a repeating wake-up here would be a
/// battery cost for a diagnostic most players leave off.
@Observable
@MainActor
final class PerformanceState {
    static let shared = PerformanceState()

    private(set) var guestFPS: Double = 0
    private(set) var frametimeMilliseconds: Double = 0
    private(set) var memoryMegabytes: Double = 0
    /// -1 when the system will not report a level.
    private(set) var batteryPercent: Int = -1
    /// Recent frametimes for the graph, oldest first.
    private(set) var frametimeHistory: [Double] = []

    /// Whether the overlay is on screen at all. Driven by the metric toggles
    /// plus the "hidden" flag the in-game panel sets.
    private(set) var isVisible = false

    private static let historyLength = 60

    fileprivate func update(fps: Double, frametime: Double, memory: Double, battery: Int) {
        guestFPS = fps
        frametimeMilliseconds = frametime
        memoryMegabytes = memory
        batteryPercent = battery
        frametimeHistory.append(frametime)
        if frametimeHistory.count > Self.historyLength {
            frametimeHistory.removeFirst(frametimeHistory.count - Self.historyLength)
        }
    }

    fileprivate func setVisible(_ visible: Bool) {
        guard isVisible != visible else { return }
        isVisible = visible
        if !visible { frametimeHistory.removeAll() }
    }
}

@objc(TsubomiPerformanceStateBridge)
@MainActor
final class PerformanceStateBridge: NSObject {
    @objc(updateWithFPS:frametime:memoryMB:batteryPercent:)
    static func update(fps: Double, frametime: Double, memoryMB: Double, batteryPercent: Int) {
        PerformanceState.shared.update(
            fps: fps, frametime: frametime, memory: memoryMB, battery: batteryPercent)
    }

    @objc(setVisible:)
    static func setVisible(_ visible: Bool) {
        PerformanceState.shared.setVisible(visible)
    }
}

/// The readout itself: a single glass capsule, draggable to reposition.
@MainActor
struct PerformanceOverlayView: View {
    /// True while the layout editor is open: the overlay then shows a sample
    /// readout so there is always something to drag, even with every metric
    /// off, and it accepts hits so the drag gesture can land on it.
    var editingProxy: Bool = false

    @State private var state = PerformanceState.shared

    @AppStorage(DefaultsKey.perfFPS.rawValue) private var showFPS = false
    @AppStorage(DefaultsKey.perfFrametime.rawValue) private var showFrametime = false
    @AppStorage(DefaultsKey.perfFrametimeGraph.rawValue) private var showGraph = false
    @AppStorage(DefaultsKey.perfRAM.rawValue) private var showRAM = false
    @AppStorage(DefaultsKey.perfBattery.rawValue) private var showBattery = false

    /// Width of the readout text, so the graph can match it exactly. Measured
    /// rather than using maxWidth: .infinity, which made the whole overlay
    /// stretch to the full screen width.
    @State private var textWidth: CGFloat = 0

    var body: some View {
        if editingProxy || (state.isVisible && hasAnyMetric) {
            VStack(alignment: .leading, spacing: 6) {
                Text(editingProxy && !hasAnyMetric ? "60 FPS · 16.7 ms" : readout)
                    .font(.caption.monospacedDigit().weight(.semibold))
                    .fixedSize()
                    .onGeometryChange(for: CGFloat.self) { $0.size.width } action: { textWidth = $0 }
                if showGraph || (editingProxy && !hasAnyMetric) {
                    // Exactly the readout's width, so the line reaches the box's
                    // right edge without the overlay growing to fill the screen.
                    FrametimeGraph(samples: state.frametimeHistory)
                        .frame(width: textWidth, height: 26)
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            // Non-interactive glass: this sits over a 60fps drawable, and an
            // interactive variant would run a live refraction pass every frame
            // for a readout nobody touches. Follows the in-game material
            // setting, so turning glass off leaves no backdrop read at all.
            .overlaySurface(.rect(cornerRadius: 16, style: .continuous))
            .overlay {
                if editingProxy {
                    RoundedRectangle(cornerRadius: 16, style: .continuous)
                        .strokeBorder(.tint, lineWidth: 1.5)
                }
            }
            // Only hit-testable while editing, so it never intercepts a touch
            // meant for the game or the controls beneath it.
            .allowsHitTesting(editingProxy)
            .accessibilityElement(children: .combine)
            .accessibilityLabel("Performance: \(readout)")
        }
    }

    private var hasAnyMetric: Bool {
        showFPS || showFrametime || showGraph || showRAM || showBattery
    }

    private var readout: String {
        var parts: [String] = []
        if showFPS {
            parts.append("\(Int(state.guestFPS.rounded())) FPS")
        }
        if showFrametime {
            parts.append(state.frametimeMilliseconds > 0
                ? String(format: "%.1f ms", state.frametimeMilliseconds)
                : "-- ms")
        }
        if showRAM {
            parts.append("\(Int(state.memoryMegabytes.rounded())) MB")
        }
        if showBattery, state.batteryPercent >= 0 {
            parts.append("\(state.batteryPercent)%")
        }
        return parts.joined(separator: "  ·  ")
    }
}

/// Minimal frametime history. Deliberately unlabelled: it is a shape to glance
/// at, and axis furniture at this size would cost more legibility than it adds.
private struct FrametimeGraph: View {
    let samples: [Double]

    var body: some View {
        Canvas { context, size in
            guard samples.count > 1 else { return }
            // Fixed 33.3ms ceiling (30fps) rather than auto-scaling: a graph
            // whose scale moves cannot be compared against itself over time.
            let ceiling = 33.3
            let step = size.width / CGFloat(samples.count - 1)
            var path = Path()
            for (index, sample) in samples.enumerated() {
                let normalized = min(sample / ceiling, 1)
                let point = CGPoint(
                    x: CGFloat(index) * step,
                    y: size.height * (1 - normalized)
                )
                if index == 0 {
                    path.move(to: point)
                } else {
                    path.addLine(to: point)
                }
            }
            context.stroke(path, with: .style(.primary), lineWidth: 1.5)
        }
        .accessibilityHidden(true)
    }
}
