import SwiftUI

/// The on-screen controller.
///
/// Draws the controls and hosts the layout editor. It does **not** receive game
/// input for the buttons and sticks: `ControlTouchSurface` sits on top for
/// that, and both ask `ControlsModel` for the same frames, so what is drawn and
/// what is touched cannot disagree. See ControlTouchSurface for why the split
/// exists.
///
/// The menu button and the performance overlay are exceptions: they are plain
/// SwiftUI elements the touch surface passes through to, because they need a
/// tap and a drag rather than the raw multi-touch the game controls need.
@MainActor
struct ControlsOverlayView: View {
    @State private var model = ControlsModel.shared
    let onMenuTap: () -> Void

    /// Read here as well as inside `OverlaySurface` because the container and
    /// the editor's buttons are structural choices, not backgrounds.
    @AppStorage(DefaultsKey.liquidGlassInGame.rawValue) private var liquidGlass = true

    var body: some View {
        // The safe-area inset the core uses to letterbox the game is reported
        // from the hosting controller (ControlsHostingController), which reads
        // UIKit's authoritative view.safeAreaInsets - a SwiftUI GeometryReader
        // here would report zero once the controls go full-bleed.
        controlsLayer
            .onAppear { ControllerHaptics.prepare(model.hapticStrength) }
            .onDisappear { ControllerHaptics.end() }
    }

    private var controlsLayer: some View {
        GeometryReader { proxy in
            let size = proxy.size
            ZStack(alignment: .topLeading) {
                controlsField(in: size)
                    .opacity(model.isEditing ? 1 : model.opacity)

                // Outside the glass container: a stick that appears anywhere
                // and moves has nothing stable to merge its highlights with,
                // and the container's morph would fight its fade.
                floatingSticks(in: size)
                    .opacity(model.opacity)

                // Positioned separately so it can be dragged in the editor and
                // so it is not affected by the controls' opacity.
                performanceOverlay(in: size)

                // Always draggable, and it survives the physical-controller
                // hide - the way back to the menu must not disappear with the
                // touch controls. It does honour its own visibility flag,
                // which is what "Hide Menu Button" sets; the three-finger tap
                // brings it back. It stays on screen in the editor regardless,
                // or a hidden button could never be repositioned or restored.
                if model.isEditing || model.isVisible("menu", in: size) {
                    menuButton(in: size)
                }

                if model.isEditing {
                    editingChrome(in: size)
                }
            }
            .frame(width: size.width, height: size.height)
            .overlay {
                if !model.isEditing {
                    ControlTouchSurface(model: model, onMenuTap: onMenuTap)
                }
            }
        }
        .ignoresSafeArea()
    }

    // MARK: - Controls

    /// Every fixed control, inside a glass container when the material is on.
    ///
    /// The container exists to merge the highlights of adjacent glass shapes;
    /// with the material off there is nothing to merge, and wrapping the field
    /// in it anyway would leave a glass effect group in the hierarchy for a
    /// setting that says there are none.
    @ViewBuilder
    private func controlsField(in size: CGSize) -> some View {
        if liquidGlass {
            // The default spread keeps the shapes apart; the container's
            // merge distance can stay large so adjacent glass blends its
            // highlights the way the system intends.
            GlassEffectContainer(spacing: 18) {
                controlStack(in: size)
            }
        } else {
            controlStack(in: size)
        }
    }

    private func controlStack(in size: CGSize) -> some View {
        ZStack(alignment: .topLeading) {
            ForEach(model.visibleControls(in: size)) { definition in
                controlView(definition, in: size)
            }
        }
        .frame(width: size.width, height: size.height)
    }

    @ViewBuilder
    private func controlView(_ definition: ControlDefinition, in size: CGSize) -> some View {
        if let frame = model.frame(for: definition, in: size) {
            controlBody(definition)
                .frame(width: frame.width, height: frame.height)
                .position(x: frame.midX, y: frame.midY)
                .modifier(EditDragModifier(model: model, definition: definition, size: size))
                .accessibilityLabel(definition.accessibilityLabel)
                .accessibilityAddTraits(.isButton)
        }
    }

    @ViewBuilder
    private func controlBody(_ definition: ControlDefinition) -> some View {
        // Each leaf reads its own keyed state (offset / pressed) inside its own
        // body, so @Observable scopes the invalidation to just that control.
        // Reading those here, in this parent body, would rebuild the whole
        // overlay on every stick move - the highest-frequency input path.
        switch definition.kind {
        case .stick:
            StickControl(definition: definition, model: model)
        default:
            ControlFace(definition: definition, model: model)
        }
    }

    // MARK: - Floating sticks

    /// Both floating sticks, always present in the hierarchy and each hiding
    /// itself when its finger is up.
    ///
    /// Kept out of this view's own body deliberately: if the parent read the
    /// centres, every touch-down and lift would rebuild the whole overlay,
    /// including the glass container. Each leaf reads its own centre instead.
    private func floatingSticks(in size: CGSize) -> some View {
        ZStack(alignment: .topLeading) {
            FloatingStick(id: ControlsModel.leftStickID, model: model)
            FloatingStick(id: ControlsModel.rightStickID, model: model)
        }
        .frame(width: size.width, height: size.height)
        // The touch surface above owns these fingers; the drawing is a
        // readout of where the stick was raised, never a target.
        .allowsHitTesting(false)
    }

    // MARK: - Menu button

    private func menuButton(in size: CGSize) -> some View {
        let frame = model.frame(for: Self.menuDefinition, in: size) ?? .zero
        return Image(systemName: "ellipsis")
            .font(.system(size: 18, weight: .semibold))
            .foregroundStyle(.primary)
            .frame(width: frame.width, height: frame.height)
            .overlaySurface(.circle)
            .overlay {
                if model.isEditing {
                    Circle().strokeBorder(.tint, lineWidth: 1.5)
                }
            }
            .position(x: frame.midX, y: frame.midY)
            .gesture(menuGesture(in: size))
            .accessibilityLabel("In-game menu")
            .accessibilityAddTraits(.isButton)
    }

    private static let menuDefinition = ControlsModel.definition(for: "menu")!

    /// The menu button both taps (opens the menu) and drags (repositions
    /// itself), so it needs one gesture that tells the two apart. A drag past a
    /// few points is a move; anything shorter is a tap.
    private func menuGesture(in size: CGSize) -> some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { value in
                let moved = abs(value.translation.width) > 6 || abs(value.translation.height) > 6
                guard moved else { return }
                let base = menuDragBase ?? currentMenuCenter(in: size)
                if menuDragBase == nil { menuDragBase = base }
                model.moveControl("menu", to: CGPoint(
                    x: base.x + value.translation.width,
                    y: base.y + value.translation.height
                ), in: size)
            }
            .onEnded { value in
                let moved = abs(value.translation.width) > 6 || abs(value.translation.height) > 6
                if moved {
                    model.endDrag()
                } else {
                    onMenuTap()
                }
                menuDragBase = nil
            }
    }

    @State private var menuDragBase: CGPoint?

    private func currentMenuCenter(in size: CGSize) -> CGPoint {
        let frame = model.frame(for: Self.menuDefinition, in: size) ?? .zero
        return CGPoint(x: frame.midX, y: frame.midY)
    }

    // MARK: - Performance overlay

    private func performanceOverlay(in size: CGSize) -> some View {
        let center = model.perfOverlayCenter(in: size)
        return PerformanceOverlayView(editingProxy: model.isEditing)
            .position(x: center.x, y: center.y)
            .modifier(PerfDragModifier(model: model, size: size))
    }

    // MARK: - Layout editor

    /// The two button styles are separate views rather than one erased style:
    /// `.glass` and `.bordered` are distinct types, and ButtonStyle has no
    /// type-erasing wrapper to choose between them at runtime.
    @ViewBuilder
    private var editorDoneButton: some View {
        if liquidGlass {
            Button("Done", action: finishEditing)
                .buttonStyle(.glassProminent)
                .controlSize(.large)
        } else {
            Button("Done", action: finishEditing)
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
        }
    }

    private func finishEditing() {
        model.endDrag()
        // Through the host: an editing session started from the library with no
        // game running has a preview overlay to tear down.
        ControlsHost.finishLayoutEditing()
    }

    @ViewBuilder
    private func editingChrome(in size: CGSize) -> some View {
        if let x = model.verticalGuideX {
            Rectangle()
                .fill(.tint)
                .frame(width: 1, height: size.height)
                .position(x: x, y: size.height / 2)
                .allowsHitTesting(false)
        }
        if let y = model.horizontalGuideY {
            Rectangle()
                .fill(.tint)
                .frame(width: size.width, height: 1)
                .position(x: size.width / 2, y: y)
                .allowsHitTesting(false)
        }

        VStack {
            editorDoneButton
            Text("Drag the controls and the overlay to reposition them")
                .font(.caption)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .overlaySurface(.capsule)
            Spacer()
        }
        // Clear the notch / Dynamic Island: the overlay is full-bleed, so
        // padding starts at the physical top edge without this inset.
        .padding(.top, model.topSafeInset + 12)
        .frame(width: size.width)
    }
}

/// Drag-to-reposition a control, active only in edit mode.
///
/// The offset accumulates on a raw centre that snapping never writes back to,
/// so a control that has snapped can still be pulled away smoothly instead of
/// re-snapping on every tick.
private struct EditDragModifier: ViewModifier {
    let model: ControlsModel
    let definition: ControlDefinition
    let size: CGSize

    @State private var rawCenter: CGPoint?

    func body(content: Content) -> some View {
        content
            .overlay {
                if model.isEditing {
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .strokeBorder(.tint, lineWidth: 1.5)
                        .allowsHitTesting(false)
                }
            }
            .gesture(model.isEditing ? dragGesture : nil)
    }

    private var dragGesture: some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { value in
                let base = rawCenter ?? model.frame(for: definition, in: size).map {
                    CGPoint(x: $0.midX, y: $0.midY)
                } ?? .zero
                if rawCenter == nil { rawCenter = base }
                model.moveControl(definition.id, to: CGPoint(
                    x: base.x + value.translation.width,
                    y: base.y + value.translation.height
                ), in: size)
            }
            .onEnded { _ in
                model.endDrag()
                rawCenter = nil
            }
    }
}

/// Drag-to-reposition the performance overlay, active only in edit mode.
private struct PerfDragModifier: ViewModifier {
    let model: ControlsModel
    let size: CGSize

    @State private var base: CGPoint?

    func body(content: Content) -> some View {
        content.gesture(model.isEditing ? gesture : nil)
    }

    private var gesture: some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { value in
                let start = base ?? model.perfOverlayCenter(in: size)
                if base == nil { base = start }
                model.movePerfOverlay(to: CGPoint(
                    x: start.x + value.translation.width,
                    y: start.y + value.translation.height
                ), in: size)
            }
            .onEnded { _ in
                model.save()
                base = nil
            }
    }
}

/// A button, shoulder, trigger, or word-labelled control.
private struct ControlFace: View {
    let definition: ControlDefinition
    var model: ControlsModel

    @AppStorage(DefaultsKey.coloredFaceButtons.rawValue) private var coloredFaceButtons = true

    /// PlayStation face-button colours, for the four face glyphs only.
    private var faceColor: Color? {
        guard coloredFaceButtons else { return nil }
        switch definition.id {
        case "cross": return .blue
        case "circle": return .red
        // A lighter pink so the square reads clearly different from the red
        // circle next to it.
        case "square": return Color(red: 1.0, green: 0.66, blue: 0.86)
        case "triangle": return .green
        default: return nil
        }
    }

    /// The ✕ and △ glyphs are visually smaller than ○ and □ at the same point
    /// size, so they are bumped up to look the same weight.
    private var glyphSize: CGFloat {
        if definition.usesWordLabel { return 10 }
        switch definition.id {
        case "cross", "triangle": return 21
        default: return 17
        }
    }

    var body: some View {
        // Read inside this leaf's body so only this control invalidates when
        // its own pressed state changes.
        let isPressed = model.pressedControls.contains(definition.id)
        // A coloured face glyph keeps its colour when pressed (the glass tint
        // provides the press feedback); everything else follows the tint on
        // press, primary otherwise.
        let glyphStyle: AnyShapeStyle = faceColor.map { AnyShapeStyle($0) }
            ?? (isPressed ? AnyShapeStyle(.tint) : AnyShapeStyle(.primary))
        return Text(definition.label)
            .font(.system(size: glyphSize, weight: .bold))
            .minimumScaleFactor(0.7)
            .lineLimit(1)
            .foregroundStyle(glyphStyle)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            // Pressed state tints the material itself rather than painting an
            // opaque chip over it, which is what Liquid Glass expects.
            .overlaySurface(shape, tinted: isPressed)
            .animation(.easeOut(duration: 0.08), value: isPressed)
    }

    /// Circles and capsules keep circular corners: a continuous curve at a
    /// radius of half the height is a squircle, not a pill.
    private var shape: some Shape {
        definition.baseSize.width == definition.baseSize.height ? AnyShape(.circle) : AnyShape(.capsule)
    }
}

/// An analogue stick: a well with a thumb that follows the touch.
private struct StickControl: View {
    let definition: ControlDefinition
    var model: ControlsModel

    var body: some View {
        // Read the offset in this leaf's body so a thumb move invalidates only
        // this stick, not the whole overlay.
        let offset = model.stickOffsets[definition.id] ?? .zero
        return GeometryReader { proxy in
            StickFace(side: min(proxy.size.width, proxy.size.height), offset: offset)
        }
    }
}

/// A stick raised under the finger, at the point it landed.
///
/// Present in the hierarchy whether or not it is up, so raising one never
/// invalidates the overlay around it - only this leaf.
private struct FloatingStick: View {
    let id: String
    var model: ControlsModel

    var body: some View {
        let center = model.dynamicStickCenters[id]
        return ZStack {
            if let center {
                StickFace(side: model.dynamicStickDiameter,
                          offset: model.stickOffsets[id] ?? .zero)
                    .position(x: center.x, y: center.y)
                    // Materialises rather than snapping in, which is what the
                    // material does everywhere else in the app.
                    .transition(.opacity.combined(with: .scale(scale: 0.9)))
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        // Scoped to the centre: the thumb's own movement must stay unanimated
        // so it tracks the finger exactly.
        .animation(.easeOut(duration: 0.12), value: center)
        .accessibilityHidden(true)
    }
}

/// The shared face of both stick kinds: a glass well with a thumb in it.
private struct StickFace: View {
    let side: CGFloat
    let offset: CGPoint

    @AppStorage(DefaultsKey.liquidGlassInGame.rawValue) private var liquidGlass = true

    var body: some View {
        let thumbSide = side * 0.46
        let travel = (side - thumbSide) / 2
        return ZStack {
            Color.clear
                .overlaySurface(.circle)
            Circle()
                // The thumb is a second backdrop read on top of the well's.
                // With the material off it is a flat disc instead.
                .fill(liquidGlass ? AnyShapeStyle(.thinMaterial) : AnyShapeStyle(Color.white.opacity(0.3)))
                .frame(width: thumbSide, height: thumbSide)
                .offset(x: offset.x * travel, y: offset.y * travel)
                // No animation: the thumb must track the finger exactly.
        }
        .frame(width: side, height: side)
    }
}
