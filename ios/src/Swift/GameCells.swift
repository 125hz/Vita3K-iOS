import SwiftUI

extension View {
    /// The game-controller focus indicator.
    ///
    /// Drawn only while a pad is actually driving the library, so touch users
    /// never see a ring. Uses `.accentColor` rather than a fixed colour so it
    /// follows the system tint and stays visible in both appearances, and sits
    /// outside the content (a stroke inset would clip the cover art).
    func padFocusRing(isFocused: Bool) -> some View {
        overlay {
            RoundedRectangle(cornerRadius: 22, style: .continuous)
                .strokeBorder(Color.accentColor, lineWidth: 3)
                .opacity(isFocused ? 1 : 0)
                .padding(-4)
        }
        .animation(.snappy(duration: 0.15), value: isFocused)
        .accessibilityAddTraits(isFocused ? .isSelected : [])
    }
}

/// Cover art with a placeholder, used by every library presentation.
@MainActor
struct GameCover: View {
    let game: GameEntry
    var cornerRadius: CGFloat = 18

    @State private var image: UIImage?

    var body: some View {
        // Color.clear defines the square; the art is an overlay on top of it.
        //
        // Applying .aspectRatio to the image itself does not work: a
        // .scaledToFill() image reports its *filled* size as its ideal size,
        // so the cover grew past the cell and pushed the grid columns and list
        // rows apart. Sizing an empty shape and overlaying the image means the
        // layout never sees the image's intrinsic size at all.
        Color.clear
            .aspectRatio(1, contentMode: .fit)
            .overlay {
                if let image {
                    Image(uiImage: image)
                        .resizable()
                        .scaledToFill()
                } else {
                    Image(systemName: "gamecontroller.fill")
                        .font(.largeTitle)
                        .foregroundStyle(.pink)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .background(.fill.secondary)
                }
            }
            // clipShape alone does not stop an overflowing overlay from being
            // drawn outside the bounds; clipped() bounds it first.
            .clipped()
            .clipShape(.rect(cornerRadius: cornerRadius, style: .continuous))
            // Keyed on the path so a cover replaced from the context menu
            // reloads without the row having to be rebuilt.
            .task(id: game.iconPath) {
                image = await CoverImageLoader.image(atPath: game.iconPath)
            }
    }
}

/// Shared metadata lines, honouring the library display toggles.
@MainActor
private struct GameMetadata: View {
    let game: GameEntry

    @AppStorage(DefaultsKey.showVersion.rawValue) private var showVersion = true
    @AppStorage(DefaultsKey.showGameSize.rawValue) private var showSize = true

    var body: some View {
        VStack(alignment: .leading, spacing: 1) {
            if showVersion {
                Text(game.versionText)
            }
            // Baseline-aligned: an HStack centres its children vertically, so
            // the trophy glyph (whose bounding box is taller than the digits)
            // sat visibly lower than the text beside it.
            HStack(alignment: .firstTextBaseline, spacing: 4) {
                Text("\(game.playedTimeText)  ·  \(game.lastPlayedText)")
                if game.trophiesTotal > 0 {
                    // Inline in the played line, as in the UIKit cell. Built by
                    // Text concatenation rather than an HStack of Image + Text
                    // so the glyph shares the digits' baseline at every Dynamic
                    // Type size, while keeping its own colour.
                    Text(Image(systemName: "trophy.fill")).foregroundColor(.yellow)
                        + Text(" \(game.trophiesUnlocked)/\(game.trophiesTotal)")
                }
            }
            if showSize {
                Text(game.sizeText)
            }
        }
        .font(.caption)
        .foregroundStyle(.secondary)
    }
}

/// Grid cell: a content-fill card. Glass belongs to the navigation layer, so
/// cards deliberately use a grouped-content fill instead of a live backdrop
/// per visible row.
@MainActor
struct GameCard: View {
    let game: GameEntry

    @AppStorage(DefaultsKey.showTitleIDs.rawValue) private var showTitleIDs = true

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            GameCover(game: game)
            Text(game.displayTitle)
                .font(.subheadline.weight(.semibold))
                .lineLimit(2)
                .multilineTextAlignment(.leading)
            if showTitleIDs {
                Text(game.titleID)
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
            GameMetadata(game: game)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(10)
        .background(Color(.secondarySystemGroupedBackground),
                    in: .rect(cornerRadius: 26, style: .continuous))
        .accessibilityElement(children: .combine)
    }
}

/// List row: sits directly on the background like a system list — no fill, no
/// forced palette, so every colour adapts to light and dark.
@MainActor
struct GameRow: View {
    let game: GameEntry

    @AppStorage(DefaultsKey.showTitleIDs.rawValue) private var showTitleIDs = true

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            GameCover(game: game, cornerRadius: 8)
                .frame(width: 56, height: 56)
                // The row's text can be three lines tall; without this the
                // cover is asked to match that height and stops being square.
                .fixedSize()
            VStack(alignment: .leading, spacing: 2) {
                Text(game.displayTitle)
                    .font(.headline)
                    .lineLimit(1)
                if showTitleIDs {
                    Text(game.titleID)
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                }
                GameMetadata(game: game)
            }
            Spacer(minLength: 0)
        }
        .padding(.vertical, 4)
        .contentShape(.rect)
        .accessibilityElement(children: .combine)
    }
}
