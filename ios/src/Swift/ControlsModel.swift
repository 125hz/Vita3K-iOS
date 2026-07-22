import Foundation
import Observation
import SwiftUI

/// What a single on-screen control does.
enum ControlKind: Equatable {
    /// A gamepad button, by SDL_GamepadButton raw value.
    case button(Int32)
    /// A trigger, by SDL_GamepadAxis raw value. Pressed drives the axis to max.
    case trigger(Int32)
    /// An analog stick, by its two SDL_GamepadAxis raw values.
    case stick(x: Int32, y: Int32)
    /// The in-game menu button. Not a gamepad input.
    case menu
}

/// One control's identity, label and behaviour. Positions live separately in
/// `ControlsModel.placements` because they are per-orientation and user-edited.
struct ControlDefinition: Identifiable, Equatable {
    let id: String
    let label: String
    let accessibilityLabel: String
    let kind: ControlKind

    /// Unscaled size. Matches the UIKit overlay's sizeForElement: so an
    /// existing saved layout keeps the same proportions.
    var baseSize: CGSize {
        switch kind {
        case .stick:
            return CGSize(width: 96, height: 96)
        case .menu:
            return CGSize(width: 46, height: 46)
        case .trigger:
            return CGSize(width: 84, height: 38)
        case .button:
            if id.contains("shoulder") { return CGSize(width: 94, height: 42) }
            if id == "select" || id == "start" { return CGSize(width: 76, height: 34) }
            return CGSize(width: 58, height: 58)
        }
    }

    /// Word labels inside a small capsule need a smaller face than a glyph.
    var usesWordLabel: Bool { id == "select" || id == "start" }
}

/// Normalized placement of one control, in the units the saved JSON uses:
/// x and y are fractions of the overlay's width and height.
struct ControlPlacement: Equatable {
    var x: Double
    var y: Double
    var visible: Bool
}

/// The on-screen controller's configuration and geometry.
///
/// Deliberately the single source of truth for control frames: both the SwiftUI
/// rendering and the raw-touch surface ask this type where a control is, so the
/// thing the user sees and the thing that receives the touch can never drift
/// apart.
@Observable
@MainActor
final class ControlsModel {
    static let shared = ControlsModel()

    // MARK: - Settings

    var opacity: Double = 0.58 { didSet { scheduleSave() } }
    var scale: Double = 1.0 { didSet { scheduleSave() } }
    var hideWhenPhysical = true { didSet { scheduleSave() } }
    var haptics = true { didSet { scheduleSave() } }
    var snapGuides = true { didSet { scheduleSave() } }

    /// Placements per orientation key ("portrait" / "landscape").
    private(set) var layouts: [String: [String: ControlPlacement]] = [:]

    /// True while the user is dragging controls around.
    var isEditing = false
    /// Set while a physical controller is attached; with `hideWhenPhysical`
    /// this hides the overlay.
    var physicalControllerConnected = false

    /// Controls currently held down, for the pressed tint. Touch identity is
    /// owned by the touch surface; this is presentation only.
    var pressedControls: Set<String> = []
    /// Live thumb offsets for the sticks, normalized to -1...1.
    var stickOffsets: [String: CGPoint] = [:]

    /// Guide lines shown while dragging, in overlay coordinates. Nil when the
    /// dragged control is not aligned with anything.
    var verticalGuideX: CGFloat?
    var horizontalGuideY: CGFloat?

    // MARK: - Definitions

    static let definitions: [ControlDefinition] = [
        // SDL_GamepadButton / SDL_GamepadAxis raw values, mirrored from the
        // enum in SDL3's SDL_gamepad.h. Kept here rather than bridged because
        // they are a stable ABI and importing SDL into Swift is not worth it.
        ControlDefinition(id: "triangle", label: "△", accessibilityLabel: "Triangle", kind: .button(3)),
        ControlDefinition(id: "circle", label: "○", accessibilityLabel: "Circle", kind: .button(1)),
        ControlDefinition(id: "cross", label: "×", accessibilityLabel: "Cross", kind: .button(0)),
        ControlDefinition(id: "square", label: "□", accessibilityLabel: "Square", kind: .button(2)),
        ControlDefinition(id: "dpad_up", label: "↑", accessibilityLabel: "D-pad up", kind: .button(11)),
        ControlDefinition(id: "dpad_down", label: "↓", accessibilityLabel: "D-pad down", kind: .button(12)),
        ControlDefinition(id: "dpad_left", label: "←", accessibilityLabel: "D-pad left", kind: .button(13)),
        ControlDefinition(id: "dpad_right", label: "→", accessibilityLabel: "D-pad right", kind: .button(14)),
        ControlDefinition(id: "left_shoulder", label: "L", accessibilityLabel: "Left shoulder", kind: .button(9)),
        ControlDefinition(id: "right_shoulder", label: "R", accessibilityLabel: "Right shoulder", kind: .button(10)),
        ControlDefinition(id: "left_trigger", label: "L2", accessibilityLabel: "Left trigger", kind: .trigger(4)),
        ControlDefinition(id: "right_trigger", label: "R2", accessibilityLabel: "Right trigger", kind: .trigger(5)),
        ControlDefinition(id: "select", label: "SELECT", accessibilityLabel: "Select", kind: .button(4)),
        ControlDefinition(id: "start", label: "START", accessibilityLabel: "Start", kind: .button(6)),
        ControlDefinition(id: "left_stick", label: "", accessibilityLabel: "Left analog stick", kind: .stick(x: 0, y: 1)),
        ControlDefinition(id: "right_stick", label: "", accessibilityLabel: "Right analog stick", kind: .stick(x: 2, y: 3)),
        ControlDefinition(id: "menu", label: "", accessibilityLabel: "In-game menu", kind: .menu),
    ]

    static func definition(for id: String) -> ControlDefinition? {
        definitions.first { $0.id == id }
    }

    // MARK: - Geometry

    /// Which saved layout applies to a given overlay size.
    static func orientationKey(for size: CGSize) -> String {
        size.height > size.width ? "portrait" : "landscape"
    }

    func placement(_ id: String, in size: CGSize) -> ControlPlacement? {
        layouts[Self.orientationKey(for: size)]?[id]
    }

    /// The control's rect in overlay coordinates.
    ///
    /// The one function both the view and the touch surface use.
    func frame(for definition: ControlDefinition, in size: CGSize) -> CGRect? {
        guard let placement = placement(definition.id, in: size) else { return nil }
        // The menu button is not user-scaled: it is chrome, not an input, and
        // scaling it with the pad made it collide with the screen edge.
        let controlScale = definition.kind == .menu ? 1.0 : scale
        let width = definition.baseSize.width * controlScale
        let height = definition.baseSize.height * controlScale
        var center = CGPoint(x: placement.x * size.width, y: placement.y * size.height)
        // Keep the control on screen even if a saved layout or a scale change
        // would push it off the edge.
        center.x = min(max(center.x, width / 2), size.width - width / 2)
        center.y = min(max(center.y, height / 2), size.height - height / 2)
        return CGRect(x: center.x - width / 2, y: center.y - height / 2, width: width, height: height)
    }

    /// Controls that should be drawn and hit-tested, in draw order. Excludes
    /// the menu button, which is always shown and is rendered separately (it is
    /// the way back to the in-game menu, so it must survive both the
    /// physical-controller hide and the touch surface's control filtering).
    func visibleControls(in size: CGSize) -> [ControlDefinition] {
        if hideWhenPhysical && physicalControllerConnected && !isEditing {
            return []
        }
        return Self.definitions.filter { definition in
            guard definition.kind != .menu else { return false }
            guard let placement = placement(definition.id, in: size) else { return false }
            return placement.visible
        }
    }

    // MARK: - Performance overlay position

    /// Normalized centre per orientation, like the controls. Kept here so the
    /// layout editor can drag it with the same machinery.
    private(set) var perfPositions: [String: CGPoint] = [:]

    private static func defaultPerfPosition(_ orientation: String) -> CGPoint {
        // Below the notch, out of the way of the thumbs.
        orientation == "portrait" ? CGPoint(x: 0.5, y: 0.13) : CGPoint(x: 0.3, y: 0.12)
    }

    func perfOverlayCenter(in size: CGSize) -> CGPoint {
        let key = Self.orientationKey(for: size)
        let normalized = perfPositions[key] ?? Self.defaultPerfPosition(key)
        return CGPoint(x: normalized.x * size.width, y: normalized.y * size.height)
    }

    func movePerfOverlay(to rawCenter: CGPoint, in size: CGSize) {
        let key = Self.orientationKey(for: size)
        let x = min(max(rawCenter.x / size.width, 0.05), 0.95)
        let y = min(max(rawCenter.y / size.height, 0.03), 0.97)
        perfPositions[key] = CGPoint(x: x, y: y)
        scheduleSave()
    }

    // MARK: - Editing

    /// Moves a control to a new centre, snapping to other visible controls'
    /// axes when the guides are on, and publishes the guide positions.
    func moveControl(_ id: String, to rawCenter: CGPoint, in size: CGSize) {
        guard let definition = Self.definition(for: id) else { return }
        var center = rawCenter
        verticalGuideX = nil
        horizontalGuideY = nil

        if snapGuides {
            // Snapping is display-only and deliberately narrow: the drag
            // accumulates on the raw centre (the caller's job), so a snapped
            // control can still be pulled away without a flick.
            let threshold: CGFloat = 6
            for other in visibleControls(in: size) where other.id != id {
                guard let otherFrame = frame(for: other, in: size) else { continue }
                if abs(otherFrame.midX - center.x) < threshold {
                    center.x = otherFrame.midX
                    verticalGuideX = center.x
                }
                if abs(otherFrame.midY - center.y) < threshold {
                    center.y = otherFrame.midY
                    horizontalGuideY = center.y
                }
            }
        }

        let controlScale = definition.kind == .menu ? 1.0 : scale
        let halfWidth = definition.baseSize.width * controlScale / 2
        let halfHeight = definition.baseSize.height * controlScale / 2
        center.x = min(max(center.x, halfWidth), size.width - halfWidth)
        center.y = min(max(center.y, halfHeight), size.height - halfHeight)

        let key = Self.orientationKey(for: size)
        layouts[key]?[id]?.x = center.x / size.width
        layouts[key]?[id]?.y = center.y / size.height
        scheduleSave()
    }

    func endDrag() {
        verticalGuideX = nil
        horizontalGuideY = nil
        save()
    }

    func setVisible(_ visible: Bool, for id: String, in size: CGSize) {
        layouts[Self.orientationKey(for: size)]?[id]?.visible = visible
        scheduleSave()
    }

    func isVisible(_ id: String, in size: CGSize) -> Bool {
        placement(id, in: size)?.visible ?? false
    }

    /// The menu button is written per orientation like everything else, but is
    /// set for both at once - it is chrome, and hiding it in one orientation
    /// only to have it reappear on rotation would read as a bug.
    func setMenuVisible(_ visible: Bool, in orientation: String) {
        layouts[orientation]?["menu"]?.visible = visible
        scheduleSave()
    }

    var isMenuVisibleInAnyLayout: Bool {
        layouts.values.contains { $0["menu"]?.visible == true }
    }

    /// Restores the built-in layout for both orientations.
    func resetLayout() {
        layouts = Self.defaultLayouts()
        save()
    }

    // MARK: - Persistence

    private var saveTask: Task<Void, Never>?

    /// Dragging writes on every frame; coalesce so the JSON is not rewritten
    /// sixty times a second.
    private func scheduleSave() {
        saveTask?.cancel()
        saveTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(400))
            guard !Task.isCancelled else { return }
            self?.save()
        }
    }

    private static var configURL: URL {
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        return documents
            .appendingPathComponent("Tsubomi")
            .appendingPathComponent("ios_controls.json")
    }

    private init() {
        layouts = Self.defaultLayouts()
        load()
    }

    private func load() {
        guard let data = try? Data(contentsOf: Self.configURL),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return }

        opacity = root["opacity"] as? Double ?? opacity
        scale = root["scale"] as? Double ?? scale
        hideWhenPhysical = root["hideWhenPhysical"] as? Bool ?? hideWhenPhysical
        haptics = root["haptics"] as? Bool ?? haptics
        snapGuides = root["snapGuides"] as? Bool ?? snapGuides

        // Merge rather than replace: a layout saved by an older build may not
        // contain every control, and those must keep their built-in position
        // instead of vanishing.
        if let perf = root["perf"] as? [String: [String: Double]] {
            for (orientation, point) in perf {
                if let x = point["x"], let y = point["y"] {
                    perfPositions[orientation] = CGPoint(x: x, y: y)
                }
            }
        }

        guard let savedLayouts = root["layouts"] as? [String: [String: [String: Any]]] else { return }
        for (orientation, saved) in savedLayouts {
            for (id, values) in saved {
                guard layouts[orientation]?[id] != nil else { continue }
                if let x = values["x"] as? Double { layouts[orientation]?[id]?.x = x }
                if let y = values["y"] as? Double { layouts[orientation]?[id]?.y = y }
                if let visible = values["visible"] as? Bool { layouts[orientation]?[id]?.visible = visible }
            }
        }
    }

    func save() {
        var encodedLayouts: [String: [String: [String: Any]]] = [:]
        for (orientation, placements) in layouts {
            var encoded: [String: [String: Any]] = [:]
            for (id, placement) in placements {
                encoded[id] = ["x": placement.x, "y": placement.y, "visible": placement.visible]
            }
            encodedLayouts[orientation] = encoded
        }
        var encodedPerf: [String: [String: Double]] = [:]
        for (orientation, point) in perfPositions {
            encodedPerf[orientation] = ["x": point.x, "y": point.y]
        }
        let root: [String: Any] = [
            "opacity": opacity,
            "scale": scale,
            "hideWhenPhysical": hideWhenPhysical,
            "haptics": haptics,
            "snapGuides": snapGuides,
            // Kept for compatibility with the layout migration the UIKit
            // overlay wrote; nothing reads it any more.
            "layoutVersion": 4,
            "layouts": encodedLayouts,
            "perf": encodedPerf,
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: root, options: [.prettyPrinted]) else { return }
        let url = Self.configURL
        try? FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? data.write(to: url, options: .atomic)
    }

    // MARK: - Defaults

    /// The built-in layouts, matching the UIKit overlay's v4 defaults exactly
    /// so an upgrade does not move anyone's controls.
    private static func defaultLayouts() -> [String: [String: ControlPlacement]] {
        func p(_ x: Double, _ y: Double) -> ControlPlacement {
            ControlPlacement(x: x, y: y, visible: true)
        }
        // Spread wider than the original defaults so the Liquid Glass shapes
        // do not merge into each other: the d-pad and face clusters have more
        // gap between their members, and the two halves sit further out.
        return [
            "landscape": [
                "dpad_up": p(0.12, 0.60), "dpad_down": p(0.12, 0.88),
                "dpad_left": p(0.05, 0.74), "dpad_right": p(0.19, 0.74),
                "triangle": p(0.88, 0.60), "cross": p(0.88, 0.88),
                "square": p(0.81, 0.74), "circle": p(0.95, 0.74),
                "left_trigger": p(0.07, 0.11), "right_trigger": p(0.93, 0.11),
                "left_shoulder": p(0.07, 0.26), "right_shoulder": p(0.93, 0.26),
                "select": p(0.42, 0.93), "start": p(0.58, 0.93),
                "left_stick": p(0.30, 0.72), "right_stick": p(0.70, 0.72),
                "menu": p(0.95, 0.13),
            ],
            "portrait": [
                "dpad_up": p(0.19, 0.57), "dpad_down": p(0.19, 0.75),
                "dpad_left": p(0.08, 0.66), "dpad_right": p(0.30, 0.66),
                "triangle": p(0.81, 0.57), "cross": p(0.81, 0.75),
                "square": p(0.70, 0.66), "circle": p(0.92, 0.66),
                "left_shoulder": p(0.13, 0.49), "right_shoulder": p(0.87, 0.49),
                "left_trigger": p(0.13, 0.41), "right_trigger": p(0.87, 0.41),
                "select": p(0.37, 0.95), "start": p(0.63, 0.95),
                "left_stick": p(0.22, 0.86), "right_stick": p(0.78, 0.86),
                "menu": p(0.92, 0.50),
            ],
        ]
    }
}
