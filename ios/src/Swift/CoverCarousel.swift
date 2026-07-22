import SwiftUI

/// The landscape cover carousel: a centred, endlessly looping row where the
/// focused cover is full size and its neighbours are scaled down and dimmed.
///
/// Looping works the way the UIKit version's did — the games are repeated many
/// times and indices map back with modulo — but there is no seam handling: the
/// row simply is a few hundred covers long and starts in the middle. See
/// `repeatCount`.
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
    let onLaunch: (GameEntry) -> Void
    @ViewBuilder let menu: (GameEntry) -> Menu

    /// Identity of the centred item, as "<repeat>-<titleID>".
    @State private var scrolledID: String?
    @State private var hapticTrigger = 0

    /// How many times the library is repeated. Odd so there is a true middle.
    ///
    /// The row simply *is* this long - there is no seam-jumping. An earlier
    /// version recentred the scroll position onto the middle repeat once the
    /// user drifted far enough, which fought `scrollPosition` and made the row
    /// snap backwards mid-scroll. Starting in the middle of a few hundred
    /// covers is indistinguishable from infinite in practice, and needs no
    /// mechanism that can misfire.
    ///
    /// Capped by total item count: this array is rebuilt on every layout pass,
    /// so a large library must not turn it into tens of thousands of entries.
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
            .onAppear {
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
            let distance = min(abs(frame.midX - viewport.midX) / stride, 1)
            return content
                .scaleEffect(1 - 0.14 * distance)
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
