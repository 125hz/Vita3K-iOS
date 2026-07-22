import Foundation

// Identifiable is a Swift protocol, so the bridged Objective-C model types
// adopt it here rather than in TsubomiBridge.h. SwiftUI needs it for stable
// identity in List/ForEach and for the item-based sheet presentations.

extension Trophy: @retroactive Identifiable {
    /// Trophy IDs are unique within a title's trophy set, which is the only
    /// scope any one list covers.
    public var id: Int { trophyID }
}
