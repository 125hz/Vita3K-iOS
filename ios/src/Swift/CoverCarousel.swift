import SwiftUI

/// The landscape cover carousel: a centred, endlessly looping row where the
/// focused cover is full size and its neighbours are scaled down and dimmed.
///
/// Looping works the way the UIKit version's did — the games are repeated many
/// times and indices map back with modulo — but the seam is handled by silently
/// recentring the scroll position onto the equivalent cover in the middle
/// repeat once scrolling settles. The content either side is identical, so the
/// jump is invisible.
///
/// The dimming is a `scrollTransition`, which the system evaluates during the
/// first layout pass. That is what makes the neighbours arrive already dimmed
/// instead of only after the first scroll.
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

    /// Identity of the centred item, as "<repeat>-<titleID>".
    @State private var scrolledID: String?
    @State private var hapticTrigger = 0
    /// Suppresses the focus/haptic side effects while recentring the loop.
    @State private var isRecentring = false

    /// How many times the library is repeated. Odd so there is a true middle,
    /// and enough that a user cannot reach an end between settles — but capped
    /// by total item count, since this array is rebuilt on every layout pass
    /// and a large library would otherwise make it tens of thousands of
    /// entries.
    private var repeatCount: Int {
        guard games.count > 1 else { return 1 }
        let target = max(3, min(101, 600 / games.count))
        return target.isMultiple(of: 2) ? target + 1 : target
    }

    private var middleRepeat: Int { repeatCount / 2 }

    private struct Item: Identifiable {
        let repeatIndex: Int
        let game: GameEntry
        var id: String { "\(repeatIndex)-\(game.titleID)" }
    }

    private var items: [Item] {
        // A single-game library has nothing to loop through; repeating it would
        // just let the user scroll past copies of the same cover.
        guard games.count > 1 else {
            return games.map { Item(repeatIndex: middleRepeat, game: $0) }
        }
        return (0..<repeatCount).flatMap { repeatIndex in
            games.map { Item(repeatIndex: repeatIndex, game: $0) }
        }
    }

    var body: some View {
        GeometryReader { proxy in
            let side = coverSide(in: proxy.size)
            ScrollView(.horizontal) {
                LazyHStack(spacing: 18) {
                    ForEach(items) { item in
                        cover(item.game, side: side)
                            .id(item.id)
                    }
                }
                .scrollTargetLayout()
                // Side padding centres the first and last covers, so every
                // game can reach the focused position.
                .padding(.horizontal, max(0, (proxy.size.width - side) / 2))
            }
            .scrollTargetBehavior(.viewAligned)
            .scrollPosition(id: $scrolledID, anchor: .center)
            .scrollIndicators(.hidden)
            .onAppear {
                if scrolledID == nil, let first = games.first {
                    scrolledID = "\(middleRepeat)-\(first.titleID)"
                }
            }
        }
        .onChange(of: scrolledID) { oldValue, newValue in
            guard !isRecentring else { return }
            guard let newValue, let titleID = Self.titleID(from: newValue) else { return }
            if oldValue != nil {
                hapticTrigger += 1
            }
            // Touch scrolling drives the pad focus too, so picking the
            // controller back up continues from the visible cover.
            if padFocusedTitleID != titleID {
                padFocusedTitleID = titleID
            }
            recentreIfNeeded(newValue)
        }
        // D-pad input moves the pad focus; scroll the row to match, staying in
        // whichever repeat is currently on screen so the row does not jump.
        .onChange(of: padFocusedTitleID) { _, focused in
            guard let focused,
                  let current = scrolledID,
                  Self.titleID(from: current) != focused,
                  let repeatIndex = Self.repeatIndex(from: current)
            else { return }
            withAnimation(.snappy) { scrolledID = "\(repeatIndex)-\(focused)" }
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
        // Continuous falloff rather than a binary identity check: phase.value
        // runs about -1...1 across the visible span, so a cover dims and
        // shrinks progressively as it leaves the centre instead of snapping
        // between two states.
        .scrollTransition(.interactive, axis: .horizontal) { content, phase in
            content
                .scaleEffect(1 - min(abs(phase.value), 1) * 0.22)
                .opacity(1 - min(abs(phase.value), 1) * 0.5)
        }
        .contentShape(.rect)
        .onTapGesture { onLaunch(game) }
        .contextMenu { menu(game) }
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isButton)
    }

    /// Jumps back to the middle repeat when the user has drifted towards an
    /// end. The cover under the centre is identical, so nothing moves visually.
    private func recentreIfNeeded(_ current: String) {
        guard games.count > 1,
              let repeatIndex = Self.repeatIndex(from: current),
              let titleID = Self.titleID(from: current),
              abs(repeatIndex - middleRepeat) > repeatCount / 4
        else { return }
        isRecentring = true
        // No animation: this must be an instantaneous swap, not a scroll.
        scrolledID = "\(middleRepeat)-\(titleID)"
        // Cleared on the next runloop turn so the assignment above does not
        // re-enter onChange and fire a haptic for a move the user did not make.
        Task { @MainActor in isRecentring = false }
    }

    private static func repeatIndex(from id: String) -> Int? {
        guard let separator = id.firstIndex(of: "-") else { return nil }
        return Int(id[id.startIndex..<separator])
    }

    private static func titleID(from id: String) -> String? {
        guard let separator = id.firstIndex(of: "-") else { return nil }
        return String(id[id.index(after: separator)...])
    }

    /// Matches the UIKit sizing: as tall as the space under the header allows,
    /// capped at about a third of the width so several covers stay visible.
    private func coverSide(in size: CGSize) -> CGFloat {
        max(120, min(size.height - 90, size.width * 0.34))
    }
}
