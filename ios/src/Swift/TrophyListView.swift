import SwiftUI

/// Trophy list for one title.
///
/// A stock inset-grouped `List`, so the sheet's Liquid Glass navigation bar and
/// the scroll-edge behaviour come from the system. Trophy art is loaded through
/// `CoverImageLoader`, which keeps the decode off the main thread — a full
/// trophy set is ~50 images and decoding them inline was what made the old
/// UIKit table stutter on first scroll.
@MainActor
struct TrophyListView: View {
    let collection: TrophyCollection
    let onFinish: () -> Void

    /// Non-nil while a trophy's art is shown full screen.
    @State private var zoomedTrophy: Trophy?

    var body: some View {
        NavigationStack {
            List(collection.trophies) { trophy in
                TrophyRow(trophy: trophy)
                    .contentShape(.rect)
                    .onTapGesture {
                        // Only art worth showing opens the viewer.
                        if !trophy.iconPath.isEmpty {
                            zoomedTrophy = trophy
                        }
                    }
            }
            .listStyle(.insetGrouped)
            .navigationTitle(collection.title)
            .navigationBarTitleDisplayMode(.inline)
            .safeAreaInset(edge: .top) {
                // The UIKit screen used navigationItem.prompt for this. There
                // is no SwiftUI equivalent, and a subtitle bar reads better
                // than folding the count into the title.
                Text(collection.progressText)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 6)
                    .background(.bar)
            }
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done", action: onFinish)
                }
            }
        }
        .fullScreenCover(item: $zoomedTrophy) { trophy in
            TrophyArtView(trophy: trophy) { zoomedTrophy = nil }
        }
    }
}

private struct TrophyRow: View {
    let trophy: Trophy

    var body: some View {
        HStack(spacing: 12) {
            TrophyIcon(trophy: trophy)
            VStack(alignment: .leading, spacing: 2) {
                Text(trophy.name)
                    .font(.headline)
                Text(trophy.detail)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .lineLimit(3)
            }
        }
        .padding(.vertical, 4)
        // One combined element instead of three: VoiceOver should read the
        // trophy as a unit, not stop on the icon.
        .accessibilityElement(children: .combine)
    }
}

private struct TrophyIcon: View {
    let trophy: Trophy

    @State private var image: UIImage?

    var body: some View {
        Group {
            if let image {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFit()
            } else {
                // Placeholder doubles as the permanent state for trophies with
                // no art, so there is never an empty hole in the row.
                Image(systemName: trophy.earned ? "trophy.fill" : "lock.fill")
                    .font(.title2)
                    .foregroundStyle(trophy.earned ? AnyShapeStyle(.yellow) : AnyShapeStyle(.tertiary))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .frame(width: 56, height: 56)
        .clipShape(.rect(cornerRadius: 10, style: .continuous))
        .task(id: trophy.iconPath) {
            guard !trophy.iconPath.isEmpty else { return }
            image = await CoverImageLoader.image(atPath: trophy.iconPath)
        }
    }
}

/// Full-screen trophy art. Tap anywhere to dismiss, matching the old behaviour.
private struct TrophyArtView: View {
    let trophy: Trophy
    let onDismiss: () -> Void

    @State private var image: UIImage?

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            VStack {
                Spacer()
                if let image {
                    Image(uiImage: image)
                        .resizable()
                        .scaledToFit()
                }
                Spacer()
                Text(trophy.name)
                    .font(.headline)
                    .foregroundStyle(.white)
                    .padding(.bottom, 40)
            }
        }
        .contentShape(.rect)
        .onTapGesture(perform: onDismiss)
        .task(id: trophy.iconPath) {
            image = await CoverImageLoader.image(atPath: trophy.iconPath)
        }
        // The art is decorative; the row already announced name and state.
        .accessibilityAddTraits(.isModal)
        .accessibilityAction(.escape, onDismiss)
    }
}
