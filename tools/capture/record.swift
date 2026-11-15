// Records one app's window and only that app's sound to a .mov, for trailer footage (docs/TRAILER_SHOTS.md, tools/capture).
//
//   blitz_record --pid PID --out FILE.mov [--content WxH] [--seconds N] [--fps 60] [--codec hevc|prores]
//
// --content: the game's picture size in points (the window minus its title bar, which sits on top): only that is recorded.
//
// Stops after --seconds, on SIGINT or SIGTERM, or when the app exits, and always finishes the file. Writes FILE.json beside
// it: {"start_ms": <Unix time of the recording's first frame, ms>, "width", "height", "fps"} so tools/capture/cut.py can line
// the game's event log (BLITZ_CAPTURE_EVENTS) up with the movie.
//
// ScreenCaptureKit, macOS 15+ (SCRecordingOutput). The first run asks for Screen & System Audio Recording permission for
// whatever launched it (Terminal, VS Code): System Settings → Privacy & Security.
import AVFoundation
import Foundation
import ScreenCaptureKit

func fail(_ msg: String) -> Never {
    FileHandle.standardError.write(("blitz_record: " + msg + "\n").data(using: .utf8)!)
    exit(1)
}

var pid: pid_t = 0
var outPath = ""
var seconds: Double = 0
var fps: Int32 = 60
var codec = "hevc"
var picture: CGSize? = nil
var it = CommandLine.arguments.dropFirst().makeIterator()
while let a = it.next() {
    switch a {
    case "--pid": pid = pid_t(it.next() ?? "") ?? 0
    case "--out": outPath = it.next() ?? ""
    case "--seconds": seconds = Double(it.next() ?? "") ?? 0
    case "--fps": fps = Int32(it.next() ?? "") ?? 60
    case "--codec": codec = it.next() ?? "hevc"
    case "--content":
        let p = (it.next() ?? "").split(separator: "x").compactMap { Double($0) }
        if p.count == 2 { picture = CGSize(width: p[0], height: p[1]) } else { fail("--content wants WxH") }
    default: fail("unknown argument \(a)")
    }
}
if pid == 0 || outPath.isEmpty { fail("usage: blitz_record --pid PID --out FILE.mov [--seconds N] [--fps 60] [--codec hevc|prores]") }
let outURL = URL(fileURLWithPath: outPath)
try? FileManager.default.removeItem(at: outURL)

final class Recorder: NSObject, SCStreamDelegate, SCRecordingOutputDelegate {
    var stream: SCStream?
    var output: SCRecordingOutput?
    var info: [String: Any] = [:]
    var stopping = false
    var done: CheckedContinuation<Void, Never>?

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        FileHandle.standardError.write("blitz_record: capture stopped: \(error.localizedDescription)\n".data(using: .utf8)!)
        stop()
    }

    func recordingOutputDidStartRecording(_ recordingOutput: SCRecordingOutput) {
        info["start_ms"] = Int64(Date().timeIntervalSince1970 * 1000)
        let data = try! JSONSerialization.data(withJSONObject: info, options: [.prettyPrinted, .sortedKeys])
        try? data.write(to: outURL.deletingPathExtension().appendingPathExtension("json"))
        print("recording \(outURL.path)")
    }

    func recordingOutput(_ recordingOutput: SCRecordingOutput, didFailWithError error: Error) {
        FileHandle.standardError.write("blitz_record: recording failed: \(error.localizedDescription)\n".data(using: .utf8)!)
        finish()
    }

    func recordingOutputDidFinishRecording(_ recordingOutput: SCRecordingOutput) { finish() }

    func finish() {
        if let d = done { done = nil; d.resume() }
    }

    func stop() {
        if stopping { return }
        stopping = true
        Task {
            if let s = stream, let o = output { try? s.removeRecordingOutput(o) }
            try? await stream?.stopCapture()
            // removeRecordingOutput finishes the file; the delegate's DidFinish ends the wait. If it never comes, don't hang.
            try? await Task.sleep(nanoseconds: 5_000_000_000)
            finish()
        }
    }
}

let rec = Recorder()

let content: SCShareableContent
do {
    content = try await SCShareableContent.excludingDesktopWindows(true, onScreenWindowsOnly: true)
} catch {
    fail("can't list windows (\(error.localizedDescription)). Allow screen recording for this terminal: System Settings → Privacy & Security → Screen & System Audio Recording")
}
guard let app = content.applications.first(where: { $0.processID == pid }) else { fail("no app with pid \(pid) on screen") }
guard let window = content.windows.filter({ $0.owningApplication?.processID == pid && $0.windowLayer == 0 && $0.frame.width > 200 })
    .max(by: { $0.frame.width * $0.frame.height < $1.frame.width * $1.frame.height }) else { fail("app \(pid) has no window on screen") }
func overlap(_ d: SCDisplay) -> CGFloat { let r = d.frame.intersection(window.frame); return r.isNull ? 0 : r.width * r.height }
guard let display = content.displays.max(by: { overlap($0) < overlap($1) }) else { fail("no display") }

// The display, only this app's windows, cropped to its window: so the app's own sound is captured with it.
let filter = SCContentFilter(display: display, including: [app], exceptingWindows: [])
let scale = CGFloat(filter.pointPixelScale)
let cfg = SCStreamConfiguration()
var frame = window.frame
if let c = picture, c.width <= frame.width, c.height <= frame.height {
    // The picture is the bottom of the window's frame (the title bar is above it), centred across.
    frame = CGRect(x: frame.minX + (frame.width - c.width) / 2, y: frame.maxY - c.height, width: c.width, height: c.height)
}
cfg.sourceRect = CGRect(x: frame.minX - display.frame.minX, y: frame.minY - display.frame.minY, width: frame.width, height: frame.height)
cfg.width = Int((frame.width * scale).rounded())
cfg.height = Int((frame.height * scale).rounded())
cfg.minimumFrameInterval = CMTime(value: 1, timescale: fps)
cfg.queueDepth = 8
cfg.showsCursor = false
cfg.capturesAudio = true
cfg.sampleRate = 48000
cfg.channelCount = 2
cfg.excludesCurrentProcessAudio = true
FileHandle.standardError.write("blitz_record: window \(window.frame), display \(display.frame), scale \(scale), source \(cfg.sourceRect)\n".data(using: .utf8)!)
rec.info = ["width": cfg.width, "height": cfg.height, "fps": Int(fps), "pid": Int(pid)]

let rc = SCRecordingOutputConfiguration()
rc.outputURL = outURL
rc.outputFileType = .mov
rc.videoCodecType = codec == "prores" ? .proRes422 : .hevc
let output = SCRecordingOutput(configuration: rc, delegate: rec)
let stream = SCStream(filter: filter, configuration: cfg, delegate: rec)
rec.stream = stream
rec.output = output
do {
    try stream.addRecordingOutput(output)
    try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
        stream.startCapture { err in if let err { c.resume(throwing: err) } else { c.resume() } }
    }
} catch {
    fail("can't start capture (\(error.localizedDescription)). Allow screen recording for this terminal: System Settings → Privacy & Security → Screen & System Audio Recording")
}

signal(SIGINT, SIG_IGN)
signal(SIGTERM, SIG_IGN)
var sources: [DispatchSourceProtocol] = []
for sig in [SIGINT, SIGTERM] {
    let s = DispatchSource.makeSignalSource(signal: sig, queue: .main)
    s.setEventHandler { rec.stop() }
    s.resume()
    sources.append(s)
}
let exitWatch = DispatchSource.makeProcessSource(identifier: pid, eventMask: .exit, queue: .main)
exitWatch.setEventHandler { rec.stop() }
exitWatch.resume()
sources.append(exitWatch)
if seconds > 0 {
    DispatchQueue.main.asyncAfter(deadline: .now() + seconds) { rec.stop() }
}

await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in rec.done = c }
print("saved \(outURL.path)")
exit(0)
