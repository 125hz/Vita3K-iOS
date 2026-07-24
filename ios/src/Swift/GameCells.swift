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
    /// Whether this placement may adopt the cover's natural (wide) aspect when
    /// the Wide cover art setting is on. Grid, carousel, and regular list pass
    /// true; Compact list explicitly forces the square icon.
    var allowsWide: Bool = false
    /// Compact list always uses icon0.png, regardless of the global wide-art
    /// preference.
    var forcesIcon: Bool = false
    /// Normal list can explicitly request pic0.png without inheriting the
    /// grid/carousel Wide cover art switch.
    var forcesWide: Bool = false
    @State private var image: UIImage?
    @AppStorage(DefaultsKey.wideCoverArt.rawValue) private var wideCoverArt = true

    /// The Vita banner art (pic0.png) is wider than tall, so square-cropping
    /// loses its sides. In wide mode the cover uses a fixed 16:9 frame;
    /// otherwise it uses icon0.png in a square.
    private var wide: Bool {
        !forcesIcon && (forcesWide || (allowsWide && wideCoverArt))
    }
    private var artPath: String {
        if forcesIcon || !wide {
            return game.iconPath
        }
        return game.wideArtPath.isEmpty ? game.iconPath : game.wideArtPath
    }

    var body: some View {
        // Color.clear defines the frame; the art is an overlay on top of it.
        //
        // Applying .aspectRatio to the image itself does not work: a
        // .scaledToFill() image reports its *filled* size as its ideal size,
        // so the cover grew past the cell and pushed the grid columns and list
        // rows apart. Sizing an empty shape and overlaying the image means the
        // layout never sees the image's intrinsic size at all.
        Color.clear
            .aspectRatio(coverAspect, contentMode: .fit)
            .overlay { artwork }
            // clipShape alone does not stop an overflowing overlay from being
            // drawn outside the bounds; clipped() bounds it first.
            .clipped()
            .clipShape(.rect(cornerRadius: cornerRadius, style: .continuous))
            // Keyed on path and art generation: the path alone does not change
            // when an install/license makes art appear.
            .task(id: "\(artPath)#\(LibraryState.shared.artGeneration)#\(wideCoverArt)") {
                image = await CoverImageLoader.image(atPath: artPath)
            }
    }

    /// Vita pic0 artwork is authored for a 16:9 banner. Fixing that frame keeps
    /// every card identical even if an individual game's source file reports
    /// unusual pixel dimensions.
    private var coverAspect: CGFloat {
        wide ? 16.0 / 9.0 : 1
    }

    @ViewBuilder
    private var artwork: some View {
        if let image {
            // Overscan wide banners by one percent. Some packaged pic0 images
            // carry square edge pixels that peek through the continuous mask;
            // this tiny crop hides them without visibly changing composition.
            Image(uiImage: image)
                .resizable()
                .aspectRatio(contentMode: .fill)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .scaleEffect(wide ? 1.01 : 1)
                .background(.fill.secondary)
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
                (Text(Image(systemName: "trophy.fill")).foregroundColor(.secondary)
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
            GameCover(game: game, allowsWide: true)
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
        // A little more room at the bottom so the last metadata line does not
        // sit right against the card edge.
        .padding(EdgeInsets(top: 10, leading: 10, bottom: 14, trailing: 10))
        // The library uses systemBackground, not a grouped-list background.
        // secondarySystemGroupedBackground is white in Light Mode and made the
        // card disappear into its white parent. This matching non-grouped
        // semantic level stays distinct in both appearances.
        .background(Color(.secondarySystemBackground),
                    in: .rect(cornerRadius: 26, style: .continuous))
        .accessibilityElement(children: .combine)
    }
}

/// List row: sits directly on the background like a system list — no fill, no
/// forced palette, so every colour adapts to light and dark.
///
/// Two densities. The default row has a large cover and the metadata on its own
/// lines. Compact shrinks the cover and folds the facts onto one line, the way
/// a dense system list (Files, Mail preview) trades detail for rows-per-screen.
@MainActor
struct GameRow: View {
    let game: GameEntry

    @AppStorage(DefaultsKey.showTitleIDs.rawValue) private var showTitleIDs = true
    @AppStorage(DefaultsKey.showGameSize.rawValue) private var showSize = true
    @AppStorage(DefaultsKey.compactList.rawValue) private var compact = false
    @AppStorage(NormalListArtwork.defaultsKey)
    private var normalListArtwork = NormalListArtwork.coverArt.rawValue

    private var normalListUsesCoverArt: Bool {
        normalListArtwork != NormalListArtwork.gameIcon.rawValue
    }

    var body: some View {
        if compact {
            compactRow
        } else {
            regularRow
        }
    }

    private var regularRow: some View {
        HStack(alignment: .top, spacing: 12) {
            GameCover(
                game: game,
                cornerRadius: 10,
                forcesIcon: !normalListUsesCoverArt,
                forcesWide: normalListUsesCoverArt
            )
                .frame(width: normalListUsesCoverArt ? 96 : 72)
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

    private var compactRow: some View {
        HStack(spacing: 10) {
            GameCover(game: game, cornerRadius: 7, forcesIcon: true)
                .frame(width: 40, height: 40)
                .fixedSize()
            VStack(alignment: .leading, spacing: 1) {
                Text(game.displayTitle)
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(1)
                compactSubtitle
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Spacer(minLength: 0)
        }
        .padding(.vertical, 2)
        .contentShape(.rect)
        .accessibilityElement(children: .combine)
    }

    /// One line: play time and last-played date first, followed by optional
    /// size and trophy count. The requested history remains visible before
    /// lower-priority metadata is truncated on narrow screens.
    private var compactSubtitle: Text {
        var line = Text("\(game.playedTimeText)  ·  \(game.lastPlayedText)")
        if showSize {
            line = line + Text("  ·  \(game.sizeText)")
        }
        if game.trophiesTotal > 0 {
            line = line
                + Text("  ")
                + Text(Image(systemName: "trophy.fill")).foregroundColor(.secondary)
                + Text(" \(game.trophiesUnlocked)/\(game.trophiesTotal)")
        }
        return line
    }
}
