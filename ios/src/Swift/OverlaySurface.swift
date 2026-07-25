import SwiftUI

/// The material the in-game overlay draws its surfaces with.
///
/// Liquid Glass works by reading back what is behind it. Behind the on-screen
/// controller is a game redrawing the whole screen, so that read can never be
/// cached the way it is over a static list: every glass surface costs a
/// backdrop sample on every frame, for as long as the session lasts. With
/// fifteen or so controls on screen that is a standing GPU cost measured in
/// hours of play.
///
/// It stays on by default - it is the design the app is built around - but a
/// player who would rather have the battery can turn it off, and these surfaces
/// become a plain fill and a hairline, which read nothing back at all.
///
/// Deliberately scoped to the in-game overlay. The library, onboarding and the
/// settings sheets sit over static content, where the effect costs almost
/// nothing and is what the app looks like.
struct OverlaySurface<S: Shape>: ViewModifier {
    let shape: S
    /// A pressed control tints its material rather than painting an opaque chip
    /// over it, which is what both materials here are asked to do.
    var tinted = false

    @AppStorage(DefaultsKey.liquidGlassInGame.rawValue) private var liquidGlass = true

    @ViewBuilder
    func body(content: Content) -> some View {
        if liquidGlass {
            content.glassEffect(tinted ? .regular.tint(.accentColor) : .regular, in: shape)
        } else {
            content
                // Glass resolves its own contrast against whatever is behind
                // it; a flat fill cannot. Pinning the subtree to dark keeps
                // `.primary` glyphs white on this translucent black surface in
                // either system appearance.
                .environment(\.colorScheme, .dark)
                .background {
                    shape
                        .fill(tinted
                            ? AnyShapeStyle(Color.accentColor.opacity(0.5))
                            : AnyShapeStyle(Color.black.opacity(0.45)))
                        .overlay(shape.stroke(Color.white.opacity(0.22), lineWidth: 1))
                }
        }
    }
}

extension View {
    /// Draws this in-game overlay element on the current surface material.
    func overlaySurface(_ shape: some Shape, tinted: Bool = false) -> some View {
        modifier(OverlaySurface(shape: shape, tinted: tinted))
    }
}
