import Foundation
import SwiftUI

/// Native, package-backed Live Area viewer. It reads the same template.xml and
/// image assets as Vita3K's desktop Live Area instead of substituting library
/// metadata for the game's authored screen.
@MainActor
struct LiveAreaView: View {
    let game: GameEntry
    let onStart: () -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var model = LiveAreaModel()
    @State private var loading = true

    var body: some View {
        NavigationStack {
            ZStack {
                LiveAreaImage(path: model.backgroundPath.isEmpty
                    ? game.wideArtPath
                    : model.backgroundPath, contentMode: .fill)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .ignoresSafeArea()
                LinearGradient(
                    colors: [.black.opacity(0.1), .black.opacity(0.72)],
                    startPoint: .top,
                    endPoint: .bottom
                )
                .ignoresSafeArea()

                ScrollView {
                    VStack(spacing: 22) {
                        LiveAreaImage(
                            path: model.gatePath.isEmpty ? game.iconPath : model.gatePath,
                            contentMode: .fit
                        )
                        .frame(maxWidth: 420)
                        .aspectRatio(16 / 9, contentMode: .fit)
                        .background(.black.opacity(0.22),
                                    in: .rect(cornerRadius: 24, style: .continuous))
                        .clipShape(.rect(cornerRadius: 24, style: .continuous))
                        .shadow(radius: 18, y: 8)

                        Text(game.displayTitle)
                            .font(.title.bold())
                            .foregroundStyle(.white)
                            .multilineTextAlignment(.center)

                        if !model.frames.isEmpty {
                            LazyVGrid(
                                columns: [GridItem(.adaptive(minimum: 240), spacing: 16)],
                                spacing: 16
                            ) {
                                ForEach(model.frames) { frame in
                                    LiveAreaFrameView(frame: frame)
                                }
                            }
                        }
                    }
                    .frame(maxWidth: 760)
                    .padding(24)
                    .frame(maxWidth: .infinity)
                }

                if loading {
                    ProgressView("Loading Live Area…")
                        .padding(20)
                        .background(.regularMaterial,
                                    in: .rect(cornerRadius: 18, style: .continuous))
                }
            }
            .navigationTitle("Live Area")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .safeAreaInset(edge: .bottom) {
                Button(action: onStart) {
                    Label("Start", systemImage: "play.fill")
                        .font(.headline)
                        .frame(maxWidth: 360)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .padding()
                .glassEffect(.regular, in: .capsule)
            }
        }
        .task(id: game.liveAreaContentsPath) {
            model = await LiveAreaLoader.load(
                contentsPath: game.liveAreaContentsPath
            )
            loading = false
        }
    }
}

private struct LiveAreaFrameView: View {
    let frame: LiveAreaFrame

    var body: some View {
        ZStack {
            LiveAreaImage(path: frame.backgroundPath, contentMode: .fill)
            LiveAreaImage(path: frame.imagePath, contentMode: .fit)
                .padding(8)
            if !frame.text.isEmpty {
                VStack {
                    Spacer()
                    Text(frame.text)
                        .font(.subheadline.weight(.medium))
                        .foregroundStyle(.white)
                        .multilineTextAlignment(.center)
                        .padding(10)
                        .frame(maxWidth: .infinity)
                        .background(.black.opacity(0.55))
                }
            }
        }
        .frame(minHeight: 150)
        .background(.black.opacity(0.24))
        .clipShape(.rect(cornerRadius: 20, style: .continuous))
    }
}

@MainActor
private struct LiveAreaImage: View {
    let path: String
    let contentMode: ContentMode
    @State private var image: UIImage?

    var body: some View {
        Group {
            if let image {
                Image(uiImage: image)
                    .resizable()
                    .aspectRatio(contentMode: contentMode)
            } else {
                Color.clear
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .task(id: path) {
            guard !path.isEmpty else {
                image = nil
                return
            }
            image = await CoverImageLoader.image(atPath: path)
        }
    }
}

private struct LiveAreaModel: Sendable {
    var backgroundPath = ""
    var gatePath = ""
    var frames: [LiveAreaFrame] = []
}

private struct LiveAreaFrame: Identifiable, Sendable {
    let id: String
    var backgroundPath = ""
    var imagePath = ""
    var text = ""
}

private enum LiveAreaLoader {
    static func load(contentsPath: String) async -> LiveAreaModel {
        await Task.detached(priority: .userInitiated) {
            guard !contentsPath.isEmpty else { return LiveAreaModel() }
            let template = URL(fileURLWithPath: contentsPath)
                .appendingPathComponent("template.xml")
            guard let parser = XMLParser(contentsOf: template) else {
                return LiveAreaModel()
            }
            let delegate = LiveAreaTemplateParser(contentsPath: contentsPath)
            parser.delegate = delegate
            guard parser.parse() else { return LiveAreaModel() }
            return delegate.model
        }.value
    }
}

/// Extracts the first authored variant from each package element. Language and
/// country variants remain package-ordered, matching the desktop fallback when
/// no exact locale match is available.
private final class LiveAreaTemplateParser: NSObject, XMLParserDelegate {
    private let contentsPath: String
    private var stack: [String] = []
    private var captureName: String?
    private var capturedText = ""
    private var currentFrame: LiveAreaFrame?
    private var currentFrameElement = ""
    private var currentFrameDepth = 0
    private(set) var model = LiveAreaModel()

    init(contentsPath: String) {
        self.contentsPath = contentsPath
    }

    func parser(
        _ parser: XMLParser,
        didStartElement elementName: String,
        namespaceURI: String?,
        qualifiedName qName: String?,
        attributes attributeDict: [String: String] = [:]
    ) {
        stack.append(elementName)
        if let id = attributeDict["id"], currentFrame == nil {
            currentFrame = LiveAreaFrame(id: id)
            currentFrameElement = elementName
            currentFrameDepth = stack.count
        }
        if ["background", "image", "startup-image", "text"].contains(elementName) {
            captureName = elementName
            capturedText = ""
        }
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) {
        if captureName != nil {
            capturedText += string
        }
    }

    func parser(
        _ parser: XMLParser,
        didEndElement elementName: String,
        namespaceURI: String?,
        qualifiedName qName: String?
    ) {
        if captureName == elementName {
            consumeCapture(named: elementName, value: capturedText)
            captureName = nil
            capturedText = ""
        }
        if currentFrame != nil,
           currentFrameElement == elementName,
           currentFrameDepth == stack.count {
            if let frame = currentFrame,
               !frame.backgroundPath.isEmpty
                || !frame.imagePath.isEmpty
                || !frame.text.isEmpty {
                model.frames.append(frame)
            }
            currentFrame = nil
            currentFrameElement = ""
            currentFrameDepth = 0
        }
        if !stack.isEmpty {
            stack.removeLast()
        }
    }

    private func consumeCapture(named name: String, value: String) {
        let value = value.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !value.isEmpty else { return }

        if stack.contains("livearea-background"), name == "image",
           model.backgroundPath.isEmpty {
            model.backgroundPath = resolve(value)
            return
        }
        if stack.contains("gate"),
           name == "image" || name == "startup-image",
           model.gatePath.isEmpty {
            model.gatePath = resolve(value)
            return
        }
        guard currentFrame != nil, stack.contains("liveitem") else { return }
        switch name {
        case "background":
            if currentFrame?.backgroundPath.isEmpty == true {
                currentFrame?.backgroundPath = resolve(value)
            }
        case "image":
            if currentFrame?.imagePath.isEmpty == true {
                currentFrame?.imagePath = resolve(value)
            }
        case "text":
            if currentFrame?.text.isEmpty == true {
                currentFrame?.text = value
            }
        default:
            break
        }
    }

    private func resolve(_ name: String) -> String {
        (contentsPath as NSString).appendingPathComponent(name)
    }
}
