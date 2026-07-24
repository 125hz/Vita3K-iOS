import Foundation
import Observation

/// Process-lifetime latch for an intentionally undiscoverable support surface.
///
/// Nothing is written to defaults or disk. Terminating the process destroys
/// the singleton and closes the surface again.
@Observable
@MainActor
final class RuntimeLatch {
    static let shared = RuntimeLatch()

    private struct Pulse {
        var origin: ContinuousClock.Instant?
        var count = 0
        var complete = false
    }

    private let clock = ContinuousClock()
    private static let span =
        MemoryLayout<UInt32>.size - MemoryLayout<UInt8>.size
    private static let quota =
        MemoryLayout<UInt64>.size + MemoryLayout<UInt16>.size
    private var channels: [UInt32: Pulse] = [:]
    private(set) var revealed = false

    private init() {}

    /// Returns true only for the transition that opens the support surface.
    func record(_ token: UInt32) -> Bool {
        guard !revealed else { return false }
        let now = clock.now
        var pulse = channels[token] ?? Pulse()
        if let origin = pulse.origin,
           now - origin <= .seconds(Double(Self.span)) {
            pulse.count += 1
        } else {
            pulse.origin = now
            pulse.count = 1
        }
        if pulse.count >= Self.quota {
            pulse.complete = true
            pulse.origin = nil
            pulse.count = 0
        }
        channels[token] = pulse

        let opened = channels.count == 2 && channels.values.allSatisfy(\.complete)
        guard opened else { return false }
        revealed = true
        channels.removeAll(keepingCapacity: false)
        return true
    }
}
