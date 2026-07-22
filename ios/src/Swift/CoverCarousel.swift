import SwiftUI

/// The landscape cover carousel: a centred, snapping row of covers where the
/// focused one is full size and its neighbours are scaled down and dimmed.
///
/// The UIKit version faked an endless list by repeating the games 400 times
/// and mapping indices back with modulo. That is not needed here: a
/// `ScrollView` with `.scrollTargetBehavior(.viewAligned)` and scroll position
/// tracking gives the snap and the focus for free, and the covers-beside-the-
/// selection dimming is a `scrollTransition`, which is applied by the system
/// on the *first* layout — which is what fixes the original bug where the
/// neighbours only darkened after the first scroll.
///
/// Losing the infinite loop is deliberate: it existed to make the row feel
/// endless, but it also meant the list had 400× as many items, and a real
/// library is small enough that the ends are reachable either way.
@MainActor
struct CoverCarousel<Menu: View>: View {
    let games: [GameEntry]
    /// Firmware missing — every cover is held back at reduced opacity.
    let dimmed: Bool
    /// Pad focus. Two-way: scrolling by touch moves the pad's focus so the two
    /// never disagree, and D-pad input scrolls the row.
    @Binding var padFocusedTitleID: String?
    let onLaunch: (GameEntry) -> Void
    @ViewBuilder let menu: (GameEntry) -> Menu

    /// Title ID of the cover nearest the centre, driven by the scroll view.
    @State private var focusedID: String?
    /// Feedback fires on change of this; `.selection` is the picker-detent
    /// feel, not an impact, which would read as a collision.
    @State private var hapticTrigger = 0

    var body: some View {
        GeometryReader { proxy in
            let side = coverSide(in: proxy.size)
            ScrollView(.horizontal) {
                LazyHStack(spacing: 18) {
                    ForEach(games) { game in
                        cover(game, side: side)
                            .id(game.titleID)
                    }
                }
                .scrollTargetLayout()
                // Side padding centres the first and last covers, so every
                // game can reach the focused position.
                .padding(.horizontal, max(0, (proxy.size.width - side) / 2))
            }
            .scrollTargetBehavior(.viewAligned)
            .scrollPosition(id: $focusedID, anchor: .center)
            .scrollIndicators(.hidden)
        }
        .onChange(of: focusedID) { oldValue, newValue in
            // Skip the initial assignment: adopting a focus the user did not
            // move to should not tick.
            guard oldValue != nil, newValue != nil else { return }
            hapticTrigger += 1
            // Touch scrolling drives the pad focus too, so picking the
            // controller back up continues from the visible cover.
            if padFocusedTitleID != nil, padFocusedTitleID != newValue {
                padFocusedTitleID = newValue
            }
        }
        // D-pad input moves the pad focus; scroll the row to match.
        .onChange(of: padFocusedTitleID) { _, focused in
            guard let focused, focused != focusedID else { return }
            withAnimation(.snappy) { focusedID = focused }
        }
        .sensoryFeedback(.selection, trigger: hapticTrigger)
        .opacity(dimmed ? 0.55 : 1)
    }

    private func cover(_ game: GameEntry, side: CGFloat) -> some View {
        VStack(spacing: 10) {
            GameCover(game: game)
                .frame(width: side, height: side)
            Text(game.displayTitle)
                .font(.subheadline.weight(.semibold))
                .lineLimit(1)
            Text("\(game.playedTimeText)  ·  \(game.lastPlayedText)")
                .font(.caption2)
                .foregroundStyle(.secondary)
                .lineLimit(1)
        }
        .frame(width: side)
        // Applied during the first layout pass, so the covers beside the
        // initially-centred one are already scaled and dimmed before the user
        // touches anything.
        .scrollTransition(.interactive, axis: .horizontal) { content, phase in
            content
                .scaleEffect(phase.isIdentity ? 1 : 0.78)
                .opacity(phase.isIdentity ? 1 : 0.5)
        }
        .contentShape(.rect)
        .onTapGesture { onLaunch(game) }
        .contextMenu { menu(game) }
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isButton)
    }

    /// Matches the UIKit sizing: as tall as the space under the header allows,
    /// capped at about a third of the width so several covers stay visible.
    private func coverSide(in size: CGSize) -> CGFloat {
        max(120, min(size.height - 90, size.width * 0.34))
    }
}
