import Foundation

// Identifiable is a Swift protocol, so the bridged Objective-C model types
// adopt it here rather than in TsubomiBridge.h. SwiftUI needs it for stable
// identity in List/ForEach and for the item-based sheet presentations.

extension Trophy: @retroactive Identifiable {
    /// Trophy IDs are unique within a title's trophy set, which is the only
    /// scope any one list covers.
    public var id: Int { trophyID }
}

extension GameEntry: @retroactive Identifiable {
    /// The title ID (e.g. PCSE00120) is the library's natural key: it is what
    /// per-game settings, saves, and trophy data are all filed under.
    public var id: String { titleID }
}
