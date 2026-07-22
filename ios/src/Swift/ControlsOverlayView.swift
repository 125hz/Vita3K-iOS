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

    var body: some View {
        // The safe-area inset the core uses to letterbox the game is reported
        // from the hosting controller (ControlsHostingController), which reads
        // UIKit's authoritative view.safeAreaInsets - a SwiftUI GeometryReader
        // here would report zero once the controls go full-bleed.
        controlsLayer
            .onAppear { ControllerHaptics.prepare() }
            .onDisappear { ControllerHaptics.end() }
    }

    private var controlsLayer: some View {
        GeometryReader { proxy in
            let size = proxy.size
            ZStack(alignment: .topLeading) {
                // Small merge distance: at the default spread the shapes stay
                // distinct, but the container still lets adjacent glass blend
                // its highlights rather than compositing hard edges.
                GlassEffectContainer(spacing: 6) {
                    ZStack(alignment: .topLeading) {
                        ForEach(model.visibleControls(in: size)) { definition in
                            controlView(definition, in: size)
                        }
                    }
                    .frame(width: size.width, height: size.height)
                }
                .opacity(model.isEditing ? 1 : model.opacity)

                // Positioned separately so it can be dragged in the editor and
                // so it is not affected by the controls' opacity.
                performanceOverlay(in: size)

                // Always shown, always draggable - the way back to the menu
                // must survive the physical-controller hide and be movable
                // without entering the editor.
                menuButton(in: size)

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
        switch definition.kind {
        case .stick:
            StickControl(
                definition: definition,
                offset: model.stickOffsets[definition.id] ?? .zero
            )
        default:
            ControlFace(
                definition: definition,
                isPressed: model.pressedControls.contains(definition.id)
            )
        }
    }

    // MARK: - Menu button

    private func menuButton(in size: CGSize) -> some View {
        let frame = model.frame(for: Self.menuDefinition, in: size) ?? .zero
        return Image(systemName: "ellipsis")
            .font(.system(size: 18, weight: .semibold))
            .foregroundStyle(.primary)
            .frame(width: frame.width, height: frame.height)
            .glassEffect(.regular, in: .circle)
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
            Button("Done") {
                model.endDrag()
                // Through the host: an editing session started from the library
                // with no game running has a preview overlay to tear down.
                ControlsHost.finishLayoutEditing()
            }
            .buttonStyle(.glassProminent)
            .controlSize(.large)
            Text("Drag the controls and the overlay to reposition them")
                .font(.caption)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .glassEffect(.regular, in: .capsule)
            Spacer()
        }
        .padding(.top, 12)
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
    let isPressed: Bool

    var body: some View {
        Text(definition.label)
            .font(.system(size: definition.usesWordLabel ? 10 : 17, weight: .bold))
            .minimumScaleFactor(0.7)
            .lineLimit(1)
            .foregroundStyle(isPressed ? AnyShapeStyle(.tint) : AnyShapeStyle(.primary))
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            // Pressed state tints the material itself rather than painting an
            // opaque chip over it, which is what Liquid Glass expects.
            .glassEffect(isPressed ? .regular.tint(.accentColor) : .regular, in: shape)
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
    /// Normalized -1...1 offset published by the touch surface.
    let offset: CGPoint

    var body: some View {
        GeometryReader { proxy in
            let side = min(proxy.size.width, proxy.size.height)
            let thumbSide = side * 0.46
            let travel = (side - thumbSide) / 2
            ZStack {
                Circle()
                    .fill(.clear)
                    .glassEffect(.regular, in: .circle)
                Circle()
                    .fill(.thinMaterial)
                    .frame(width: thumbSide, height: thumbSide)
                    .offset(x: offset.x * travel, y: offset.y * travel)
                    // No animation: the thumb must track the finger exactly.
            }
        }
    }
}
