import SwiftUI
import UIKit

/// Raw multi-touch capture for the on-screen controls.
///
/// This is the only part of the overlay that is not SwiftUI, for two reasons
/// that are not stylistic:
///
/// 1. **Concurrency.** A Vita layout needs about six simultaneous independent
///    touches (two sticks, d-pad, face buttons, two triggers). SwiftUI gestures
///    are recognised through UIKit gesture recognisers, which arbitrate between
///    each other; concurrent touch-downs on sibling views get delayed or
///    dropped. `touchesBegan/Moved/Ended` has no arbitration - every touch is
///    delivered immediately and tracked by identity.
///
/// 2. **Passthrough.** Vita touchscreen input reaches the game through the gaps
///    between controls, which requires telling UIKit "this point is not mine"
///    from `point(inside:with:)`. A SwiftUI view cannot do that: its host view
///    handles button taps internally, so hit-testing cannot distinguish a
///    control from empty space.
///
/// Everything visible - the controls, their glass, the layout editor - is
/// SwiftUI. This view is transparent and sits on top of it.
struct ControlTouchSurface: UIViewRepresentable {
    let model: ControlsModel
    /// Called when the menu control is tapped.
    let onMenuTap: () -> Void

    func makeUIView(context: Context) -> TouchCaptureView {
        let view = TouchCaptureView()
        view.model = model
        view.onMenuTap = onMenuTap
        return view
    }

    func updateUIView(_ view: TouchCaptureView, context: Context) {
        view.model = model
        view.onMenuTap = onMenuTap
    }

    final class TouchCaptureView: UIView {
        weak var model: ControlsModel?
        var onMenuTap: (() -> Void)?

        /// Which control each active touch is driving. Keyed by the UITouch
        /// itself so a finger keeps its control even if it slides off - which
        /// is what players expect from a physical pad.
        private var activeTouches: [ObjectIdentifier: String] = [:]

        override init(frame: CGRect) {
            super.init(frame: frame)
            isMultipleTouchEnabled = true
            backgroundColor = .clear
            // Never intercepts the system's own gestures.
            isExclusiveTouch = false
        }

        @available(*, unavailable)
        required init?(coder: NSCoder) { fatalError("not used") }

        // MARK: - Passthrough

        /// Only points inside a visible control belong to this view; everything
        /// else falls through to the Metal view underneath, which is how the
        /// game receives Vita touchscreen input.
        ///
        /// With floating sticks on there is no "everything else": the empty
        /// screen is the sticks, so this claims all of it apart from the menu
        /// button, whose own view handles the tap.
        override func point(inside point: CGPoint, with event: UIEvent?) -> Bool {
            guard let model else { return false }
            if control(at: point) != nil { return true }
            guard model.dynamicSticksActive else { return false }
            return model.menuFrame(in: bounds.size)?.contains(point) != true
        }

        private func control(at point: CGPoint) -> ControlDefinition? {
            guard let model, !model.isEditing else { return nil }
            let size = bounds.size
            // Reverse order so the topmost control wins where two overlap.
            for definition in model.visibleControls(in: size).reversed() {
                guard let frame = model.frame(for: definition, in: size) else { continue }
                if frame.contains(point) { return definition }
            }
            return nil
        }

        // MARK: - Touch tracking

        override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
            guard let model else { return }
            for touch in touches {
                let location = touch.location(in: self)
                guard let definition = control(at: location) else {
                    // Empty screen. A control was not hit, so this is either a
                    // floating stick or nothing at all - which is why pressing
                    // a button never raises one.
                    beginDynamicStick(touch: touch, at: location, model: model)
                    continue
                }
                activeTouches[ObjectIdentifier(touch)] = definition.id
                press(definition, at: location)
                if model.haptics, case .button = definition.kind {
                    ControllerHaptics.tick()
                }
            }
        }

        override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
            guard let model else { return }
            for touch in touches {
                guard let id = activeTouches[ObjectIdentifier(touch)],
                      let definition = ControlsModel.definition(for: id),
                      case .stick = definition.kind
                else { continue }
                // Only sticks track movement. A button keeps its press while
                // the finger slides, matching a physical pad.
                updateStick(definition, touch: touch, model: model)
            }
        }

        /// Raises a floating stick centred exactly where the finger landed, so
        /// the first movement from that point is the deflection - no jump.
        private func beginDynamicStick(touch: UITouch, at location: CGPoint, model: ControlsModel) {
            guard let id = model.dynamicStickID(at: location, in: bounds.size) else { return }
            activeTouches[ObjectIdentifier(touch)] = id
            model.dynamicStickCenters[id] = location
            model.stickOffsets[id] = .zero
        }

        override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
            release(touches)
        }

        override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
            release(touches)
        }

        private func release(_ touches: Set<UITouch>) {
            guard let model else { return }
            for touch in touches {
                guard let id = activeTouches.removeValue(forKey: ObjectIdentifier(touch)),
                      let definition = ControlsModel.definition(for: id)
                else { continue }
                switch definition.kind {
                case .button(let button):
                    ControllerInput.setButton(button, pressed: false)
                    model.pressedControls.remove(id)
                case .trigger(let axis):
                    ControllerInput.setAxis(axis, value: ControllerInput.axisMin)
                    model.pressedControls.remove(id)
                case .stick(let xAxis, let yAxis):
                    ControllerInput.setAxis(xAxis, value: 0)
                    ControllerInput.setAxis(yAxis, value: 0)
                    model.stickOffsets[id] = .zero
                    // Lowers the floating stick and frees its half of the
                    // screen for the next touch. A no-op for a fixed stick.
                    model.dynamicStickCenters[id] = nil
                case .menu:
                    model.pressedControls.remove(id)
                    onMenuTap?()
                }
            }
        }

        private func press(_ definition: ControlDefinition, at location: CGPoint) {
            guard let model else { return }
            switch definition.kind {
            case .button(let button):
                ControllerInput.setButton(button, pressed: true)
                model.pressedControls.insert(definition.id)
            case .trigger(let axis):
                ControllerInput.setAxis(axis, value: ControllerInput.axisMax)
                model.pressedControls.insert(definition.id)
            case .stick:
                model.pressedControls.insert(definition.id)
            case .menu:
                model.pressedControls.insert(definition.id)
            }
        }

        private func updateStick(_ definition: ControlDefinition, touch: UITouch, model: ControlsModel) {
            guard case .stick(let xAxis, let yAxis) = definition.kind else { return }
            // A floating stick pivots around wherever it was raised; a fixed
            // one around its place in the layout.
            let center: CGPoint
            let radius: CGFloat
            if let dynamicCenter = model.dynamicStickCenters[definition.id] {
                center = dynamicCenter
                radius = model.dynamicStickDiameter / 2
            } else if let frame = model.frame(for: definition, in: bounds.size) {
                center = CGPoint(x: frame.midX, y: frame.midY)
                radius = frame.width / 2
            } else {
                return
            }
            let location = touch.location(in: self)
            var dx = (location.x - center.x) / radius
            var dy = (location.y - center.y) / radius
            // Clamp to the unit circle so a diagonal is not stronger than a
            // cardinal direction, which is what an analogue stick does.
            let magnitude = (dx * dx + dy * dy).squareRoot()
            if magnitude > 1 {
                dx /= magnitude
                dy /= magnitude
            }
            model.stickOffsets[definition.id] = CGPoint(x: dx, y: dy)
            ControllerInput.setAxis(xAxis, value: ControllerInput.axisValue(dx))
            ControllerInput.setAxis(yAxis, value: ControllerInput.axisValue(dy))
        }
    }
}

/// Thin wrapper over the SDL virtual joystick, which lives on the
/// Objective-C++ side.
@MainActor
enum ControllerInput {
    static let axisMax: Int16 = 32767
    static let axisMin: Int16 = -32768

    static func axisValue(_ normalized: CGFloat) -> Int16 {
        let clamped = min(max(normalized, -1), 1)
        return Int16(clamped * CGFloat(axisMax))
    }

    static func setButton(_ button: Int32, pressed: Bool) {
        VirtualPad.setButton(button, pressed: pressed)
    }

    static func setAxis(_ axis: Int32, value: Int16) {
        VirtualPad.setAxis(axis, value: value)
    }

    /// Drops every input, for when a session pauses with controls held.
    static func releaseAll() {
        VirtualPad.releaseAllInputs()
    }

    /// Whether finger events still reach the guest's front touch panel. Turned
    /// off while floating sticks own the whole screen.
    static func setVitaTouchscreenEnabled(_ enabled: Bool) {
        VirtualPad.setVitaTouchscreenEnabled(enabled)
    }
}

/// Button-press feedback. Created once and kept warm only while the overlay is
/// on screen; a generator held across a whole game session would keep the
/// Taptic Engine powered for no reason.
@MainActor
enum ControllerHaptics {
    private static var generator: UIImpactFeedbackGenerator?

    static func prepare() {
        if generator == nil {
            generator = UIImpactFeedbackGenerator(style: .light)
        }
        generator?.prepare()
    }

    static func tick() {
        if generator == nil { prepare() }
        generator?.impactOccurred(intensity: 0.55)
    }

    static func end() {
        generator = nil
    }
}
