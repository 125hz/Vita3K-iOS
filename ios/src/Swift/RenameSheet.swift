import SwiftUI

/// Frontend-only rename for a library title.
///
/// A sheet rather than an alert with a text field: an alert cannot show the
/// cover, and clearing the name to restore the packaged title needs an
/// explanation that does not fit in an alert's message.
@MainActor
struct RenameSheet: View {
    let game: GameEntry
    let onSave: () -> Void

    @State private var name: String
    @Environment(\.dismiss) private var dismiss
    @FocusState private var nameFocused: Bool

    init(game: GameEntry, onSave: @escaping () -> Void) {
        self.game = game
        self.onSave = onSave
        _name = State(initialValue: game.displayTitle)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Title", text: $name)
                        .focused($nameFocused)
                        .submitLabel(.done)
                        .onSubmit(save)
                } footer: {
                    Text("Leave this empty to restore the title from the game package.")
                }
            }
            .navigationTitle("Rename")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save", action: save)
                }
            }
            .onAppear { nameFocused = true }
        }
        .presentationDetents([.medium])
    }

    private func save() {
        // An empty string clears the override on the bridge side.
        Bridge.setDisplayTitle(name.trimmingCharacters(in: .whitespacesAndNewlines),
                               forTitle: game.titleID)
        onSave()
        dismiss()
    }
}
