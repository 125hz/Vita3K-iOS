import AVFoundation
import Foundation

/// Native versions of the Cuelume interaction cues used by the library.
///
/// Cuelume ships recipes rather than audio files. These buffers reproduce the
/// recipe layers, exponential envelopes, filters, pitch glides, and shimmer
/// feedback locally so the home screen never needs a network connection or a
/// bundled recording.
///
/// Recipes: https://github.com/Danilaa1/cuelume
/// License: ios/third_party/cuelume-LICENSE.txt
@objc(TsubomiSoundEffects)
@MainActor
final class HomeSoundEffects: NSObject {
    enum Cue: String {
        case tick
        case error
        case press
        case success
        case sparkle
        case loading
        case ready
    }

    private static let shared = HomeSoundEffects()
    private static let enabledKey = DefaultsKey.soundEffects.rawValue

    private let engine = AVAudioEngine()
    private let players = (0..<24).map { _ in AVAudioPlayerNode() }
    private let format = AVAudioFormat(standardFormatWithSampleRate: 48_000, channels: 1)!
    private var nextPlayer = 0
    private var buffers: [Cue: AVAudioPCMBuffer] = [:]

    override private init() {
        super.init()
        for player in players {
            engine.attach(player)
            engine.connect(player, to: engine.mainMixerNode, format: format)
        }
        engine.prepare()
    }

    static func play(_ cue: Cue) {
        shared.play(cue)
    }

    /// Objective-C entry point for import/settings completion callbacks.
    @objc(playNamed:)
    static func play(named name: String) {
        guard let cue = Cue(rawValue: name) else { return }
        play(cue)
    }

    private var isEnabled: Bool {
        let defaults = UserDefaults.standard
        return defaults.object(forKey: Self.enabledKey) as? Bool ?? true
    }

    private func play(_ cue: Cue) {
        guard isEnabled else { return }
        if !engine.isRunning {
            do {
                try engine.start()
            } catch {
                return
            }
        }

        let buffer = buffers[cue] ?? Self.render(cue, format: format)
        buffers[cue] = buffer

        // Prefer a free voice so rapid refresh pairs, carousel ticks, and
        // toolbar presses never cancel a cue that is still audible. The
        // round-robin fallback is only reached after 24 simultaneous sounds.
        let player: AVAudioPlayerNode
        if let idle = players.first(where: { !$0.isPlaying }) {
            player = idle
        } else {
            player = players[nextPlayer]
            nextPlayer = (nextPlayer + 1) % players.count
            player.stop()
        }
        player.scheduleBuffer(buffer)
        player.play()
    }
}

private extension HomeSoundEffects {
    enum Waveform {
        case sine
        case triangle
    }

    enum FilterKind {
        case lowpass
        case bandpass
    }

    struct Tone {
        let waveform: Waveform
        let frequency: Double
        var offset = 0.0
        let attack: Double
        let decay: Double
        let peak: Double
        var glideTo: Double?
        var glideTime: Double?
    }

    struct Noise {
        let filter: FilterKind
        let frequency: Double
        let q: Double
        var offset = 0.0
        let attack: Double
        let decay: Double
        let peak: Double
    }

    enum Layer {
        case tone(Tone)
        case noise(Noise)

        var offset: Double {
            switch self {
            case .tone(let value): value.offset
            case .noise(let value): value.offset
            }
        }

        var attack: Double {
            switch self {
            case .tone(let value): value.attack
            case .noise(let value): value.attack
            }
        }

        var decay: Double {
            switch self {
            case .tone(let value): value.decay
            case .noise(let value): value.decay
            }
        }
    }

    struct Shimmer {
        let delay: Double
        let feedback: Double
        let wet: Double
        let lowpass: Double
    }

    struct Recipe {
        let masterGain: Double
        let layers: [Layer]
        var shimmer: Shimmer?
    }

    static func recipe(for cue: Cue) -> Recipe {
        switch cue {
        case .sparkle:
            Recipe(masterGain: 0.5, layers: [
                .tone(Tone(waveform: .sine, frequency: 1760, attack: 0.003, decay: 0.09, peak: 0.045)),
                .tone(Tone(waveform: .sine, frequency: 2217, offset: 0.045, attack: 0.003, decay: 0.09, peak: 0.04)),
                .tone(Tone(waveform: .sine, frequency: 2637, offset: 0.09, attack: 0.003, decay: 0.1, peak: 0.038)),
                .tone(Tone(waveform: .sine, frequency: 3520, offset: 0.135, attack: 0.003, decay: 0.12, peak: 0.032)),
            ], shimmer: Shimmer(delay: 0.07, feedback: 0.35, wet: 0.22, lowpass: 6000))
        case .tick:
            Recipe(masterGain: 0.4, layers: [
                .noise(Noise(filter: .bandpass, frequency: 5400, q: 1.8, attack: 0.001, decay: 0.018, peak: 0.14)),
                .tone(Tone(waveform: .sine, frequency: 2600, attack: 0.001, decay: 0.012, peak: 0.018)),
            ])
        case .press:
            Recipe(masterGain: 0.4, layers: [
                .noise(Noise(filter: .bandpass, frequency: 1700, q: 1.4, attack: 0.001, decay: 0.02, peak: 0.13)),
            ])
        case .success:
            Recipe(masterGain: 0.5, layers: [
                .tone(Tone(waveform: .sine, frequency: 880, attack: 0.004, decay: 0.09, peak: 0.06)),
                .tone(Tone(waveform: .sine, frequency: 1108.73, offset: 0.06, attack: 0.004, decay: 0.1, peak: 0.06)),
                .tone(Tone(waveform: .sine, frequency: 1318.51, offset: 0.12, attack: 0.004, decay: 0.18, peak: 0.07)),
            ], shimmer: Shimmer(delay: 0.1, feedback: 0.22, wet: 0.16, lowpass: 4500))
        case .error:
            Recipe(masterGain: 0.42, layers: [
                .noise(Noise(filter: .bandpass, frequency: 850, q: 1.1, attack: 0.001, decay: 0.035, peak: 0.13)),
                .tone(Tone(waveform: .triangle, frequency: 440, offset: 0.025, attack: 0.004, decay: 0.09, peak: 0.045)),
                .tone(Tone(waveform: .triangle, frequency: 349.23, offset: 0.1, attack: 0.004, decay: 0.14, peak: 0.04)),
            ])
        case .loading:
            Recipe(masterGain: 0.42, layers: [
                .noise(Noise(filter: .lowpass, frequency: 1400, q: 0.6, attack: 0.035, decay: 0.14, peak: 0.035)),
                .tone(Tone(waveform: .sine, frequency: 420, attack: 0.025, decay: 0.18, peak: 0.05,
                           glideTo: 630, glideTime: 0.18)),
            ], shimmer: Shimmer(delay: 0.11, feedback: 0.18, wet: 0.12, lowpass: 2800))
        case .ready:
            Recipe(masterGain: 0.45, layers: [
                .noise(Noise(filter: .bandpass, frequency: 3200, q: 1.7, attack: 0.001, decay: 0.018, peak: 0.1)),
                .tone(Tone(waveform: .sine, frequency: 659.25, offset: 0.025, attack: 0.012, decay: 0.2, peak: 0.05)),
                .tone(Tone(waveform: .sine, frequency: 987.77, offset: 0.025, attack: 0.012, decay: 0.22, peak: 0.035)),
            ], shimmer: Shimmer(delay: 0.13, feedback: 0.2, wet: 0.13, lowpass: 3600))
        }
    }

    static func render(_ cue: Cue, format: AVAudioFormat) -> AVAudioPCMBuffer {
        let recipe = recipe(for: cue)
        let sampleRate = format.sampleRate
        let sourceEnd = recipe.layers.map { $0.offset + $0.attack + $0.decay + 0.05 }.max() ?? 0.1
        let shimmerTail: Double
        if let shimmer = recipe.shimmer, shimmer.feedback > 0, shimmer.feedback < 1 {
            shimmerTail = shimmer.delay
                * (1 + ceil(log(0.001) / log(shimmer.feedback)))
        } else {
            shimmerTail = recipe.shimmer?.delay ?? 0
        }
        let frameCount = max(1, Int(ceil((sourceEnd + shimmerTail + 0.05) * sampleRate)))
        var dry = [Double](repeating: 0, count: frameCount)

        for layer in recipe.layers {
            switch layer {
            case .tone(let tone):
                render(tone, into: &dry, sampleRate: sampleRate)
            case .noise(let noise):
                render(noise, into: &dry, sampleRate: sampleRate)
            }
        }

        for index in dry.indices {
            dry[index] *= recipe.masterGain
        }
        var output = dry
        if let shimmer = recipe.shimmer {
            apply(shimmer, source: dry, output: &output, sampleRate: sampleRate)
        }

        let buffer = AVAudioPCMBuffer(
            pcmFormat: format,
            frameCapacity: AVAudioFrameCount(frameCount)
        )!
        buffer.frameLength = AVAudioFrameCount(frameCount)
        let channel = buffer.floatChannelData![0]
        for index in output.indices {
            channel[index] = Float(max(-1, min(1, output[index])))
        }
        return buffer
    }

    static func render(_ tone: Tone, into output: inout [Double], sampleRate: Double) {
        let start = Int(tone.offset * sampleRate)
        let audibleFrames = Int(ceil((tone.attack + tone.decay) * sampleRate))
        var phase = 0.0
        for local in 0..<audibleFrames where start + local < output.count {
            let elapsed = Double(local) / sampleRate
            let frequency: Double
            if let glideTo = tone.glideTo {
                let progress = min(1, elapsed / (tone.glideTime ?? tone.attack + tone.decay))
                frequency = tone.frequency * pow(glideTo / tone.frequency, progress)
            } else {
                frequency = tone.frequency
            }
            phase += 2 * Double.pi * frequency / sampleRate
            let sample: Double
            switch tone.waveform {
            case .sine:
                sample = sin(phase)
            case .triangle:
                sample = 2 / Double.pi * asin(sin(phase))
            }
            output[start + local] += sample * envelope(
                elapsed: elapsed, attack: tone.attack, decay: tone.decay, peak: tone.peak
            )
        }
    }

    static func render(_ noise: Noise, into output: inout [Double], sampleRate: Double) {
        let start = Int(noise.offset * sampleRate)
        let audibleFrames = Int(ceil((noise.attack + noise.decay) * sampleRate))
        var filter = Biquad(kind: noise.filter, frequency: noise.frequency, q: noise.q,
                             sampleRate: sampleRate)
        for local in 0..<audibleFrames where start + local < output.count {
            let elapsed = Double(local) / sampleRate
            let white = Double.random(in: -1...1)
            output[start + local] += filter.process(white) * envelope(
                elapsed: elapsed, attack: noise.attack, decay: noise.decay, peak: noise.peak
            )
        }
    }

    static func envelope(elapsed: Double, attack: Double, decay: Double, peak: Double) -> Double {
        let floor = 0.0001
        if elapsed < attack {
            return floor * pow(peak / floor, elapsed / max(attack, 0.000_001))
        }
        let progress = min(1, (elapsed - attack) / max(decay, 0.000_001))
        return peak * pow(floor / peak, progress)
    }

    static func apply(
        _ shimmer: Shimmer,
        source: [Double],
        output: inout [Double],
        sampleRate: Double
    ) {
        let delayFrames = max(1, Int(shimmer.delay * sampleRate))
        var delayed = [Double](repeating: 0, count: output.count + delayFrames)
        for index in source.indices {
            delayed[index + delayFrames] += source[index]
        }
        var filter = Biquad(kind: .lowpass, frequency: shimmer.lowpass, q: 0.707,
                             sampleRate: sampleRate)
        for index in output.indices {
            let filtered = filter.process(delayed[index])
            output[index] += filtered * shimmer.wet
            let feedbackIndex = index + delayFrames
            if feedbackIndex < delayed.count {
                delayed[feedbackIndex] += filtered * shimmer.feedback
            }
        }
    }

    struct Biquad {
        let b0: Double
        let b1: Double
        let b2: Double
        let a1: Double
        let a2: Double
        var x1 = 0.0
        var x2 = 0.0
        var y1 = 0.0
        var y2 = 0.0

        init(kind: FilterKind, frequency: Double, q: Double, sampleRate: Double) {
            let omega = 2 * Double.pi * min(frequency, sampleRate * 0.49) / sampleRate
            let cosine = cos(omega)
            let alpha = sin(omega) / (2 * max(q, 0.001))
            let rawB0: Double
            let rawB1: Double
            let rawB2: Double
            switch kind {
            case .lowpass:
                rawB0 = (1 - cosine) / 2
                rawB1 = 1 - cosine
                rawB2 = (1 - cosine) / 2
            case .bandpass:
                rawB0 = alpha
                rawB1 = 0
                rawB2 = -alpha
            }
            let a0 = 1 + alpha
            b0 = rawB0 / a0
            b1 = rawB1 / a0
            b2 = rawB2 / a0
            a1 = -2 * cosine / a0
            a2 = (1 - alpha) / a0
        }

        mutating func process(_ input: Double) -> Double {
            let result = b0 * input + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2 = x1
            x1 = input
            y2 = y1
            y1 = result
            return result
        }
    }
}
