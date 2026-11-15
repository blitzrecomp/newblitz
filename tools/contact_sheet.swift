// Tile screenshots into one labelled grid, to review a run at a glance.
// Usage: contact_sheet <out.png> <columns> <tile width> <image>...
// Each tile is labelled with its file name (without extension).
import AppKit

let args = CommandLine.arguments
guard args.count >= 5, let cols = Int(args[2]), let tileW = Double(args[3]) else {
    FileHandle.standardError.write("usage: contact_sheet <out.png> <columns> <tile width> <image>...\n".data(using: .utf8)!)
    exit(1)
}
let paths = Array(args[4...])
let images = paths.compactMap { NSImage(contentsOfFile: $0) }
guard let first = images.first else { exit(1) }
let tileH = tileW * Double(first.size.height / first.size.width)
let labelH = 22.0
let rows = (images.count + cols - 1) / cols
let size = NSSize(width: Double(cols) * tileW, height: Double(rows) * (tileH + labelH))

guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size.width), pixelsHigh: Int(size.height),
                                 bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                 colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else { exit(1) }
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
NSColor(white: 0.15, alpha: 1).setFill()
NSRect(origin: .zero, size: size).fill()
let attrs: [NSAttributedString.Key: Any] = [.font: NSFont.boldSystemFont(ofSize: 15), .foregroundColor: NSColor.white]
for (i, img) in images.enumerated() {
    let col = i % cols, row = i / cols
    let x = Double(col) * tileW
    let y = size.height - Double(row + 1) * (tileH + labelH)
    img.draw(in: NSRect(x: x, y: y, width: tileW, height: tileH))
    let name = (paths[i] as NSString).lastPathComponent.replacingOccurrences(of: ".png", with: "")
    (name as NSString).draw(at: NSPoint(x: x + 6, y: y + tileH + 3), withAttributes: attrs)
}
NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: args[1]))
