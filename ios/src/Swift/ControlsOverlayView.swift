import SwiftUI

/// The on-screen controller.
///
/// Draws the controls and hosts the layout editor. It does **not** receive game
/// input: `ControlTouchSurface` sits on top for that, and both ask
/// `ControlsModel` for the same frames, so what is drawn and what is touched
/// cannot disagree. See ControlTouchSurface for why the split exists.
@MainActor
struct ControlsOverlayView: View {
    @State private var model = ControlsModel.shared
    let onMenuTap: () -> Void

    var body: some View {
        GeometryReader { proxy in
            let size = proxy.size
            ZStack(alignment: .topLeading) {
                // Glass elements that belong together are grouped so the system
                // can blend and morph them as one surface rather than
                // compositing seventeen independent backdrops.
                GlassEffectContainer(spacing: 18) {
                    // ZStack inside the container: the controls are absolutely
                    // positioned, so they need an overlapping layout rather
                    // than the container's own stacking.
                    ZStack(alignment: .topLeading) {
                        ForEach(model.visibleControls(in: size)) { definition in
                            controlView(definition, in: size)
                        }
                    }
                    .frame(width: size.width, height: size.height)
                }
                .opacity(model.isEditing ? 1 : model.opacity)

                if model.isEditing {
                    editingChrome(in: size)
                }
            }
            .frame(width: size.width, height: size.height)
            // The touch surface only exists outside edit mode; while editing,
            // SwiftUI's own drag gestures need the touches.
            .overlay {
                if !model.isEditing {
                    ControlTouchSurface(model: model, onMenuTap: onMenuTap)
                }
            }
        }
        .ignoresSafeArea()
        // The core letterboxes the guest image below the notch and reads this
        // through vita3k_ios_safe_area_top_pixels; it cannot ask SwiftUI.
        .background {
            GeometryReader { proxy in
                Color.clear.onChange(of: proxy.safeAreaInsets.top, initial: true) { _, top in
                    let scale = UIScreen.main.nativeScale
                    SafeAreaReporter.topPixels = Float(top * scale)
                }
            }
        }
        // The three-finger tap that restores a hidden menu button stays a
        // UIGestureRecognizer on the window (see vita3k_ios_show_virtual_controller):
        // SwiftUI has no multi-finger tap gesture, and it has to fire even
        // where this view passes touches through to the game.
        .onAppear { ControllerHaptics.prepare() }
        .onDisappear { ControllerHaptics.end() }
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
        case .menu:
            Image(systemName: "ellipsis")
                .font(.system(size: 18, weight: .semibold))
                .foregroundStyle(.primary)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .glassEffect(.regular, in: .circle)
        default:
            ControlFace(
                definition: definition,
                isPressed: model.pressedControls.contains(definition.id)
            )
        }
    }

    // MARK: - Layout editor

    @ViewBuilder
    private func editingChrome(in size: CGSize) -> some View {
        // Alignment guides, shown only while a drag is actually aligned.
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
                // Through the host, not by clearing isEditing directly: when
                // editing was started from the library with no game running,
                // the overlay is a preview that has to be torn down too.
                ControlsHost.finishLayoutEditing()
            }
            .buttonStyle(.glassProminent)
            .controlSize(.large)
            Text("Drag controls to reposition them")
                .font(.caption)
                .foregroundStyle(.secondary)
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .glassEffect(.regular, in: .capsule)
            Spacer()
        }
        .padding(.top, 12)
        .frame(width: size.width)
    }
}

/// Drag-to-reposition, active only in edit mode.
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
                    // A visible target while editing, so controls that are
                    // hidden behind the game read as draggable.
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
                let moved = CGPoint(
                    x: base.x + value.translation.width,
                    y: base.y + value.translation.height
                )
                model.moveControl(definition.id, to: moved, in: size)
            }
            .onEnded { value in
                if let base = rawCenter {
                    rawCenter = CGPoint(
                        x: base.x + value.translation.width,
                        y: base.y + value.translation.height
                    )
                }
                model.endDrag()
                rawCenter = nil
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
            // The thumb travels within the well, not to its edge.
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
                    // Easing it here would read as input lag.
            }
        }
    }
}
