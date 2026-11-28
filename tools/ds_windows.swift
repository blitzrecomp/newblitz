// Lists a process's windows for tools/ds_harness.sh: "<window id> <y> <height> <title>", one per line (taller than 100).
// BLITZ_DS=1 on the Mac opens two: the game's own ("New Blitz …", the lower screen) and "… upper screen".
// Usage: ds_windows <pid>
import CoreGraphics
import Foundation

guard CommandLine.arguments.count > 1, let pid = Int32(CommandLine.arguments[1]) else {
    FileHandle.standardError.write("usage: ds_windows <pid>\n".data(using: .utf8)!)
    exit(1)
}
let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as? [[String: Any]] ?? []
for w in list where (w[kCGWindowOwnerPID as String] as? Int32) == pid {
    guard let b = w[kCGWindowBounds as String] as? [String: Any], let h = b["Height"] as? Double, h >= 100 else { continue }
    print(w[kCGWindowNumber as String] ?? 0, b["Y"] ?? 0, h, w[kCGWindowName as String] ?? "")
}
