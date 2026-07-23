import SwiftUI

/// The landscape cover carousel: a centred, endlessly looping row where the
/// focused cover is full size and its neighbours are scaled down and dimmed.
///
/// Looping works the way the UIKit version's did — the games are repeated many
/// times and indices map back with modulo — but there is no seam handling: the
/// row simply is a few hundred covers long and starts in the middle. See
/// `rebuildItems`.
///
/// The dimming is computed from each cover's distance to the viewport centre
/// with `visualEffect`; see the note in `cover(_:side:)`.
@MainActor
struct CoverCarousel<Menu: View>: View {
    let games: [GameEntry]
    /// Firmware missing — every cover is held back at reduced opacity.
    let dimmed: Bool
    /// Pad focus. Two-way: scrolling by touch moves the pad's focus so the two
    /// never disagree, and D-pad input scrolls the row.
    @Binding var padFocusedTitleID: String?
    /// D-pad stepping from LibraryState. The token changes on each press; the
    /// direction says which way. Passed in rather than read from a shared
    /// object because the carousel otherwise has no reference to it.
    let stepToken: Int
    let stepDirection: Int
    let onLaunch: (GameEntry) -> Void
    @ViewBuilder let menu: (GameEntry) -> Menu

    /// Identity of the centred item, as "<repeat>-<titleID>".
    @State private var scrolledID: String?
    @State private var hapticTrigger = 0

    /// The repeated item list, cached. Rebuilt only when the games change - not
    /// on every body pass. body re-runs on each scroll settle and every haptic
    /// tick, and rebuilding several hundred structs (each with a String id) on
    /// each of those was avoidable churn while browsing.
    @State private var items: [Item] = []
    @State private var middleRepeat = 0

    private struct Item: Identifiable {
        let repeatIndex: Int
        let game: GameEntry
        var id: String { "\(repeatIndex)-\(game.titleID)" }
    }

    /// Games' identity, so the cache rebuilds when the library actually changes.
    private var gamesKey: String { games.map(\.titleID).joined(separator: ",") }

    private func rebuildItems() {
        // The row simply *is* this long; there is no seam-jumping. Starting in
        // the middle of a few hundred covers is indistinguishable from infinite
        // in practice. Capped by total item count so a large library does not
        // become tens of thousands of entries.
        guard games.count > 1 else {
            middleRepeat = 0
            items = games.map { Item(repeatIndex: 0, game: $0) }
            return
        }
        var target = max(3, min(101, 600 / games.count))
        if target.isMultiple(of: 2) { target += 1 }
        middleRepeat = target / 2
        items = (0..<target).flatMap { repeatIndex in
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
            }
            // safeAreaPadding on the scroll view, NOT padding inside its
            // content. Padding applied after scrollTargetLayout() wraps the
            // target layout in a larger container, so the snap positions were
            // measured on that container instead of the covers - advancing one
            // game took most of a screen-width of drag. This insets the content
            // while leaving the scroll targets measured on the covers.
            .safeAreaPadding(.horizontal, max(0, (proxy.size.width - side) / 2))
            .scrollTargetBehavior(.viewAligned)
            .scrollPosition(id: $scrolledID, anchor: .center)
            .scrollIndicators(.hidden)
            .onChange(of: gamesKey, initial: true) { _, _ in
                rebuildItems()
                if scrolledID == nil, let first = games.first {
                    scrolledID = "\(middleRepeat)-\(first.titleID)"
                }
            }
        }
        .onChange(of: scrolledID) { oldValue, newValue in
            guard let newValue, let titleID = Self.titleID(from: newValue) else { return }
            if oldValue != nil {
                hapticTrigger += 1
            }
            // Touch scrolling drives the pad focus too, so picking the
            // controller back up continues from the visible cover.
            if padFocusedTitleID != titleID {
                padFocusedTitleID = titleID
            }
        }
        // D-pad steps by one cover in the flat, repeated list - crossing a
        // game boundary continues into the next repeat rather than wrapping
        // the game index back to the start, so holding a direction keeps
        // travelling one way. Moving the scroll target directly (not through
        // the focus id) is also what keeps rapid presses in sync: each press
        // advances exactly one detent instead of racing a focus round-trip.
        .onChange(of: stepToken) { _, _ in
            stepCarousel(by: stepDirection)
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
        // Measured from real geometry, not from scroll phases or the tracked
        // centre id.
        //
        // scrollTransition reported identity for every visible cover whatever
        // threshold was used. Comparing against `scrolledID` then failed in a
        // way that looked identical on device: if that id never matches, every
        // cover gets the dimmed branch, and a uniform dim is indistinguishable
        // from no dim at all, because the effect only reads as contrast.
        //
        // visualEffect hands over a GeometryProxy at render time, so the
        // distance from the viewport centre can be computed directly. No
        // threshold semantics, no dependency on the scroll position binding
        // writing back, and it tracks the drag continuously instead of
        // settling per cover.
        .visualEffect { content, proxy in
            let frame = proxy.frame(in: .scrollView(axis: .horizontal))
            let viewport = proxy.bounds(of: .scrollView(axis: .horizontal)) ?? .zero
            // 0 at the centre, 1 once a full cover away.
            let stride = max(frame.width + 18, 1)
            // Double, not CGFloat: brightness and saturation take Double, and
            // mixing the two makes the arithmetic ambiguous.
            let distance = Double(min(abs(frame.midX - viewport.midX) / stride, 1))
            return content
                .scaleEffect(CGFloat(1 - 0.14 * distance))
                // Held back, not hidden: the neighbours are still browsable
                // covers, so this is a hierarchy cue rather than a disabled
                // state. Brightness rather than opacity, so a cover dims
                // instead of going translucent against the background.
                .brightness(-0.22 * distance)
                .saturation(1 - 0.2 * distance)
        }
        .contentShape(.rect)
        .onTapGesture { onLaunch(game) }
        .contextMenu { menu(game) }
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isButton)
    }

    /// Advances the centred cover by `direction` items in the flat repeated
    /// list. Because the list is one long sequence of repeats, +1 past the
    /// last game continues into the first game of the next repeat rather than
    /// jumping backwards.
    private func stepCarousel(by direction: Int) {
        guard direction != 0,
              let current = scrolledID,
              let index = items.firstIndex(where: { $0.id == current })
        else { return }
        let next = index + direction
        guard items.indices.contains(next) else { return }
        withAnimation(.snappy(duration: 0.16)) { scrolledID = items[next].id }
    }

    private static func titleID(from id: String) -> String? {
        guard let separator = id.firstIndex(of: "-") else { return nil }
        return String(id[id.index(after: separator)...])
    }

    /// Bigger than before (0.34 → 0.42 of the width, and less vertical
    /// reserve): the covers are the whole point of this view, so they should
    /// dominate it.
    private func coverSide(in size: CGSize) -> CGFloat {
        max(140, min(size.height - 70, size.width * 0.42))
    }
}
