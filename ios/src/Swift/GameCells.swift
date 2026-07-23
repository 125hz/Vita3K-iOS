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
    @AppStorage(DefaultsKey.wideCoverArt.rawValue) private var wideCoverArt = false

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
            .overlay { artwork }
            // clipShape alone does not stop an overflowing overlay from being
            // drawn outside the bounds; clipped() bounds it first.
            .clipped()
            .clipShape(.rect(cornerRadius: cornerRadius, style: .continuous))
            // Keyed on the path plus the art generation: the path alone does
            // not change when an install or license import makes an icon
            // appear where there was none, so the load would never re-run.
            .task(id: "\(game.iconPath)#\(LibraryState.shared.artGeneration)") {
                image = await CoverImageLoader.image(atPath: game.iconPath)
            }
    }

    @ViewBuilder
    private var artwork: some View {
        if let image {
            if wideCoverArt {
                // Wide covers (the Vita LiveArea art is wider than tall) show
                // in full rather than being cropped to a square. The art is
                // fit inside the frame over a blurred fill of itself, the way
                // Apple presents mismatched-aspect artwork - nothing is lost,
                // and the grid stays uniform because the frame is unchanged.
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
                    .blur(radius: 14)
                    .overlay(.black.opacity(0.15))
                    .overlay {
                        Image(uiImage: image)
                            .resizable()
                            .scaledToFit()
                    }
            } else {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
            }
        } else {
            Image(systemName: "gamecontroller.fill")
                .font(.largeTitle)
                .foregroundStyle(.pink)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(.fill.secondary)
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
                    .lineLimit(1)
            }
            // One line, always. This is what made grid cards uneven heights:
            // "30m · 7/22/26, 04:43" wrapped to two lines on some cards and
            // one on others, so a row of otherwise-identical cards did not
            // line up. Trophy count moved to its own line for the same reason
            // - keeping it inline pushed some cards to a second line.
            Text("\(game.playedTimeText)  ·  \(game.lastPlayedText)")
                .lineLimit(1)
                .minimumScaleFactor(0.85)
            if game.trophiesTotal > 0 {
                // Text concatenation rather than an HStack of Image + Text so
                // the glyph shares the digits' baseline at every Dynamic Type
                // size, while keeping its own colour.
                (Text(Image(systemName: "trophy.fill")).foregroundColor(.yellow)
                    + Text(" \(game.trophiesUnlocked)/\(game.trophiesTotal)"))
                    .lineLimit(1)
            }
            if showSize {
                Text(game.sizeText)
                    .lineLimit(1)
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
            // Reserves two lines whether the title needs them or not, so a
            // one-line title and a two-line title produce the same card height
            // and the grid rows line up.
            Text(game.displayTitle)
                .font(.subheadline.weight(.semibold))
                .lineLimit(2, reservesSpace: true)
                .multilineTextAlignment(.leading)
            if showTitleIDs {
                Text(game.titleID)
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
                    .lineLimit(1)
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
