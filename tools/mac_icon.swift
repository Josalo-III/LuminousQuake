// Owned vector icon rendered with AppKit. GPL-2.0-or-later.
import AppKit
let directory = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
for (name, size) in [("icon_16x16",16),("icon_16x16@2x",32),("icon_32x32",32),
                     ("icon_32x32@2x",64),("icon_128x128",128),("icon_128x128@2x",256),
                     ("icon_256x256",256),("icon_256x256@2x",512),("icon_512x512",512),("icon_512x512@2x",1024)] {
    let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
    let s = CGFloat(size)
    NSColor(calibratedRed: 0.10, green: 0.075, blue: 0.055, alpha: 1).setFill()
    NSBezierPath(roundedRect: NSRect(x:s*0.04,y:s*0.04,width:s*0.92,height:s*0.92), xRadius:s*0.20,yRadius:s*0.20).fill()
    NSColor(calibratedRed: 0.90, green: 0.57, blue: 0.23, alpha: 1).setStroke()
    let ring = NSBezierPath(ovalIn:NSRect(x:s*0.24,y:s*0.27,width:s*0.52,height:s*0.52))
    ring.lineWidth=s*0.075; ring.stroke()
    let stem=NSBezierPath(); stem.move(to:NSPoint(x:s*0.50,y:s*0.48)); stem.line(to:NSPoint(x:s*0.50,y:s*0.15))
    stem.lineWidth=s*0.075; stem.stroke()
    NSGraphicsContext.restoreGraphicsState()
    try bitmap.representation(using:.png,properties:[:])!.write(to:directory.appendingPathComponent(name+".png"))
}
