// Prints the CGWindowID of the first on-screen window owned by a process, so screenshots can capture
// that window only: screencapture -l <id>.
//   winid            first window of any process named BlitzRecompiled
//   winid NAME       first window of any process with that name
//   winid NAME PID   only windows of that process, whatever its name (several instances, or the
//                    2026 flavour's "Blitz 2026" bundle name)
import CoreGraphics
import Foundation

let args = CommandLine.arguments
let owner = args.count > 1 ? args[1] : "BlitzRecompiled"
let pid = args.count > 2 ? Int(args[2]) : nil
let windows = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] ?? []
for w in windows where (w[kCGWindowLayer as String] as? Int) == 0 {
    if let pid {
        if (w[kCGWindowOwnerPID as String] as? Int) != pid { continue }
    } else if (w[kCGWindowOwnerName as String] as? String) != owner {
        continue
    }
    if let id = w[kCGWindowNumber as String] as? Int { print(id); exit(0) }
}
exit(1)
