import UIKit

/// Async front end to the shared art cache.
///
/// The cache, the background decode, and the memory-warning eviction all live
/// on the Objective-C side (see `Bridge.loadArt(atPath:completion:)`) so the
/// SwiftUI screens and the not-yet-migrated UIKit ones share one cache rather
/// than each holding their own copy of a large library's cover art.
@MainActor
enum CoverImageLoader {
    /// - Returns: the decoded image, or nil when the path is empty or the file
    ///   cannot be decoded. Callers keep their placeholder on nil.
    static func image(atPath path: String) async -> UIImage? {
        await withCheckedContinuation { continuation in
            // The bridge guarantees exactly one completion call, on the main
            // thread — including for empty paths and decode failures, either
            // of which would otherwise leak this continuation.
            Bridge.loadArt(atPath: path) { image in
                continuation.resume(returning: image)
            }
        }
    }
}
