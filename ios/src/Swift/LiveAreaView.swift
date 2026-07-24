import Foundation
import SwiftUI

/// Package-backed Live Area rendered on the Vita's 960 x 544 reference canvas.
///
/// The package template does not describe a flowing document. Its style name
/// selects fixed frame and gate coordinates, so laying those assets out in a
/// vertical SwiftUI stack makes the page taller than both an iPhone portrait
/// screen and the original Vita viewport. Keeping one authored canvas and
/// aspect-fitting it preserves the desktop renderer's composition in every
/// window size.
@MainActor
struct LiveAreaView: View {
    let game: GameEntry
    let onStart: () -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var model = LiveAreaModel()
    @State private var loading = true

    var body: some View {
        NavigationStack {
            GeometryReader { proxy in
                let scale = min(
                    proxy.size.width / LiveAreaLayout.referenceSize.width,
                    proxy.size.height / LiveAreaLayout.referenceSize.height
                )

                ZStack {
                    Color.black

                    LiveAreaCanvas(game: game, model: model, onStart: onStart)
                        .frame(
                            width: LiveAreaLayout.referenceSize.width,
                            height: LiveAreaLayout.referenceSize.height
                        )
                        .scaleEffect(scale)
                        .frame(
                            width: LiveAreaLayout.referenceSize.width * scale,
                            height: LiveAreaLayout.referenceSize.height * scale
                        )

                    if loading {
                        ProgressView("Loading Live Area…")
                            .padding(20)
                            .background(
                                .regularMaterial,
                                in: .rect(cornerRadius: 18, style: .continuous)
                            )
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            .background(.black)
            .navigationTitle("Live Area")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
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

@MainActor
private struct LiveAreaCanvas: View {
    let game: GameEntry
    let model: LiveAreaModel
    let onStart: () -> Void

    private var layout: LiveAreaStyleLayout {
        LiveAreaLayout.styles[model.style] ?? LiveAreaLayout.styles["a1"]!
    }

    private var gateRect: CGRect {
        CGRect(
            x: LiveAreaLayout.referenceSize.width - layout.gatePosition.x,
            y: LiveAreaLayout.referenceSize.height - layout.gatePosition.y,
            width: 280,
            height: 158
        )
    }

    var body: some View {
        ZStack(alignment: .topLeading) {
            Color.black

            LiveAreaImage(
                path: model.backgroundPath.isEmpty ? game.wideArtPath : model.backgroundPath,
                contentMode: .fill
            )
            .frame(width: 840, height: 500)
            .clipped()
            .position(x: 480, y: 294)

            ForEach(model.frames) { frame in
                if let frameLayout = layout.frames[frame.id] {
                    LiveAreaFrameView(frame: frame)
                        .frame(
                            width: frameLayout.size.width,
                            height: frameLayout.size.height
                        )
                        .position(
                            x: LiveAreaLayout.referenceSize.width
                                - frameLayout.position.x
                                + frameLayout.size.width / 2,
                            y: LiveAreaLayout.referenceSize.height
                                - frameLayout.position.y
                                + frameLayout.size.height / 2
                        )
                }
            }

            LiveAreaImage(
                path: model.gatePath.isEmpty ? game.iconPath : model.gatePath,
                contentMode: .fill
            )
            .frame(width: gateRect.width, height: gateRect.height)
            .clipped()
            .background(Color(red: 47 / 255, green: 51 / 255, blue: 50 / 255))
            .clipShape(.rect(cornerRadius: 10))
            .overlay {
                RoundedRectangle(cornerRadius: 10)
                    .stroke(Color(white: 0.75), lineWidth: 3)
            }
            .position(x: gateRect.midX, y: gateRect.midY)

            Button(action: onStart) {
                Text("Start")
                    .font(.system(size: 14, weight: .bold))
                    .foregroundStyle(.white)
                    .frame(width: 120, height: 32)
                    .background(
                        Color(red: 20 / 255, green: 168 / 255, blue: 222 / 255),
                        in: .rect(cornerRadius: 10)
                    )
            }
            .buttonStyle(.plain)
            .position(
                x: gateRect.midX,
                y: gateRect.maxY - 8 - 16
            )
        }
        .frame(
            width: LiveAreaLayout.referenceSize.width,
            height: LiveAreaLayout.referenceSize.height
        )
        .clipped()
    }
}

private struct LiveAreaFrameView: View {
    let frame: LiveAreaFrame

    var body: some View {
        ZStack {
            LiveAreaImage(path: frame.backgroundPath, contentMode: .fill)
                .clipped()
            LiveAreaImage(path: frame.imagePath, contentMode: .fit)
            if !frame.text.isEmpty {
                Text(frame.text)
                    .font(.system(size: 12))
                    .foregroundStyle(.white)
                    .multilineTextAlignment(.center)
                    .minimumScaleFactor(0.5)
                    .padding(4)
            }
        }
        .background(.clear)
        .clipped()
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
    var style = "a1"
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

private struct LiveAreaFrameLayout {
    let position: CGPoint
    let size: CGSize
}

private struct LiveAreaStyleLayout {
    let gatePosition: CGPoint
    let frames: [String: LiveAreaFrameLayout]
}

/// Coordinates copied from Vita3K's LiveAreaWidget. `position` values use the
/// Vita template's bottom/right-based coordinate system; LiveAreaCanvas applies
/// the same REF_W - x / REF_H - y conversion as the desktop renderer.
private enum LiveAreaLayout {
    static let referenceSize = CGSize(width: 960, height: 544)

    private static func frame(
        _ x: CGFloat,
        _ y: CGFloat,
        _ width: CGFloat,
        _ height: CGFloat
    ) -> LiveAreaFrameLayout {
        LiveAreaFrameLayout(
            position: CGPoint(x: x, y: y),
            size: CGSize(width: width, height: height)
        )
    }

    static let styles: [String: LiveAreaStyleLayout] = [
        "a1": LiveAreaStyleLayout(
            gatePosition: CGPoint(x: 620, y: 361),
            frames: [
                "frame1": frame(900, 414, 260, 260),
                "frame2": frame(320, 414, 260, 260),
                "frame3": frame(900, 154, 840, 150),
            ]
        ),
        "a2": LiveAreaStyleLayout(
            gatePosition: CGPoint(x: 620, y: 395),
            frames: [
                "frame1": frame(900, 404, 260, 400),
                "frame2": frame(320, 404, 260, 400),
                "frame3": frame(640, 204, 320, 200),
            ]
        ),
        "a3": LiveAreaStyleLayout(
            gatePosition: CGPoint(x: 620, y: 395),
            frames: [
                "frame1": frame(900, 414, 260, 200),
                "frame2": frame(320, 414, 260, 200),
                "frame3": frame(900, 214, 260, 210),
                "frame4": frame(640, 214, 320, 210),
                "frame5": frame(320, 214, 260, 210),
            ]
        ),
        "a4": LiveAreaStyleLayout(
            gatePosition: CGPoint(x: 620, y: 395),
            frames: [
                "frame1": frame(900, 414, 260, 200),
                "frame2": frame(320, 414, 260, 200),
                "frame3": frame(900, 214, 840, 70),
                "frame4": frame(900, 144, 840, 70),
                "frame5": frame(900, 74, 840, 70),
            ]
        ),
        "a5": LiveAreaStyleLayout(
            gatePosition: CGPoint(x: 380, y: 395),
            frames: [
                "frame1": frame(900, 412, 480, 68),
                "frame2": frame(900, 344, 480, 68),
                "frame3": frame(900, 276, 480, 68),
                "frame4": frame(900, 208, 480, 68),
                "frame5": frame(900, 140, 480, 68),
                "frame6": frame(900, 72, 480, 68),
                "frame7": frame(420, 214, 360, 210),
            ]
        ),
        "psmobile": LiveAreaStyleLayout(
            gatePosition: CGPoint(x: 380, y: 345),
            frames: [
                "frame1": frame(866, 414, 446, 154),
                "frame2": frame(866, 249.5, 446, 109),
                "frame3": frame(866, 119, 196, 58),
                "frame4": frame(866, 34, 772, 30),
            ]
        ),
    ]
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
        if elementName == "livearea", let style = attributeDict["style"], !style.isEmpty {
            model.style = style
        }
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
