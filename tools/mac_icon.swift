// App icon rendered with AppKit. GPL-2.0-or-later.
// A blue halo outlines the Quad Damage silhouette, which is left as dark negative
// space. The outline is a 2D projection of quaddama.mdl's triangles (x, y in
// unit icon space, y up). No skin, palette or other game data is included.
import AppKit
let outline: [CGFloat] = [
    0.5825,0.5416,0.4963,0.5322,0.5511,0.5200,
    0.5158,0.5133,0.5825,0.5416,0.4913,0.5620,
    0.4994,0.2121,0.5032,0.1300,0.5234,0.2069,
    0.4776,0.2730,0.4994,0.2121,0.4996,0.2827,
    0.4682,0.4964,0.4996,0.2827,0.4979,0.5095,
    0.4591,0.5158,0.4979,0.5095,0.4963,0.5322,
    0.4246,0.5338,0.4963,0.5322,0.4913,0.5620,
    0.4994,0.2121,0.5326,0.2754,0.4996,0.2827,
    0.5077,0.2668,0.4776,0.2730,0.4868,0.2053,
    0.5075,0.4882,0.4682,0.4964,0.4776,0.2730,
    0.5091,0.5051,0.4591,0.5158,0.4682,0.4964,
    0.5158,0.5133,0.4246,0.5338,0.4591,0.5158,
    0.4996,0.2827,0.5420,0.4998,0.4979,0.5095,
    0.5077,0.2668,0.5060,0.2017,0.5234,0.2069,
    0.5075,0.4882,0.5077,0.2668,0.5326,0.2754,
    0.5091,0.5051,0.5075,0.4882,0.5420,0.4998,
    0.5158,0.5133,0.5091,0.5051,0.5511,0.5200,
    0.4979,0.5095,0.5511,0.5200,0.4963,0.5322,
    0.5326,0.2754,0.4994,0.2121,0.5234,0.2069,
    0.5420,0.4998,0.4996,0.2827,0.5326,0.2754,
    0.5511,0.5200,0.4979,0.5095,0.5420,0.4998,
    0.4963,0.5322,0.5825,0.5416,0.4913,0.5620,
    0.4913,0.5620,0.4246,0.5338,0.5158,0.5133,
    0.5032,0.1300,0.4994,0.2121,0.4868,0.2053,
    0.4994,0.2121,0.4776,0.2730,0.4868,0.2053,
    0.4996,0.2827,0.4682,0.4964,0.4776,0.2730,
    0.4979,0.5095,0.4591,0.5158,0.4682,0.4964,
    0.4963,0.5322,0.4246,0.5338,0.4591,0.5158,
    0.5032,0.1300,0.5060,0.2017,0.4868,0.2053,
    0.4868,0.2053,0.5060,0.2017,0.5077,0.2668,
    0.4776,0.2730,0.5077,0.2668,0.5075,0.4882,
    0.4682,0.4964,0.5075,0.4882,0.5091,0.5051,
    0.4591,0.5158,0.5091,0.5051,0.5158,0.5133,
    0.5234,0.2069,0.5060,0.2017,0.5032,0.1300,
    0.5234,0.2069,0.5326,0.2754,0.5077,0.2668,
    0.5326,0.2754,0.5420,0.4998,0.5075,0.4882,
    0.5420,0.4998,0.5511,0.5200,0.5091,0.5051,
    0.5511,0.5200,0.5825,0.5416,0.5158,0.5133,
    0.6855,0.7653,0.6069,0.8281,0.6712,0.7488,
    0.2871,0.6301,0.3272,0.7940,0.3108,0.8260,
    0.3565,0.8418,0.3857,0.8547,0.3108,0.8260,
    0.4333,0.8700,0.3857,0.8547,0.3894,0.8369,
    0.3565,0.8418,0.4375,0.8609,0.4333,0.8700,
    0.7595,0.5623,0.6712,0.7488,0.7134,0.5695,
    0.2888,0.6640,0.3108,0.8260,0.2405,0.6399,
    0.3370,0.6538,0.3565,0.8418,0.2888,0.6640,
    0.3370,0.6538,0.3755,0.8182,0.3272,0.7940,
    0.3385,0.4828,0.2871,0.6301,0.2405,0.6399,
    0.3755,0.8182,0.4375,0.8609,0.3894,0.8369,
    0.3108,0.8260,0.3272,0.7940,0.3894,0.8369,
    0.6098,0.8411,0.5618,0.8171,0.6390,0.7407,
    0.4732,0.4139,0.3385,0.4828,0.3074,0.4131,
    0.3552,0.4342,0.2405,0.6399,0.3074,0.4131,
    0.3871,0.5042,0.2888,0.6640,0.3552,0.4342,
    0.3871,0.5042,0.3370,0.6538,0.2871,0.6301,
    0.6134,0.4119,0.4732,0.4139,0.4735,0.3063,
    0.5234,0.3300,0.3074,0.4131,0.4735,0.3063,
    0.5218,0.4354,0.3552,0.4342,0.5234,0.3300,
    0.5218,0.4354,0.3871,0.5042,0.3385,0.4828,
    0.6653,0.5511,0.6134,0.4119,0.6469,0.3397,
    0.6947,0.3608,0.4735,0.3063,0.6469,0.3397,
    0.6628,0.4337,0.5234,0.3300,0.6947,0.3608,
    0.6628,0.4337,0.5218,0.4354,0.4732,0.4139,
    0.6226,0.7274,0.6653,0.5511,0.7112,0.5410,
    0.7595,0.5623,0.7112,0.5410,0.6469,0.3397,
    0.7134,0.5695,0.6947,0.3608,0.7595,0.5623,
    0.7134,0.5695,0.6134,0.4119,0.6628,0.4337,
    0.6069,0.8281,0.5618,0.8171,0.5581,0.8066,
    0.6855,0.7653,0.6390,0.7407,0.7112,0.5410,
    0.6712,0.7488,0.7595,0.5623,0.6855,0.7653,
    0.6712,0.7488,0.6653,0.5511,0.7134,0.5695,
    0.5581,0.8066,0.6226,0.7274,0.6390,0.7407,
    0.3108,0.8260,0.2405,0.6399,0.2871,0.6301,
    0.6069,0.8281,0.6226,0.7274,0.6712,0.7488,
    0.6069,0.8281,0.6855,0.7653,0.6098,0.8411,
    0.3857,0.8547,0.3565,0.8418,0.4333,0.8700,
    0.6653,0.5511,0.6712,0.7488,0.6226,0.7274,
    0.3108,0.8260,0.2888,0.6640,0.3565,0.8418,
    0.3565,0.8418,0.3370,0.6538,0.3755,0.8182,
    0.3272,0.7940,0.2871,0.6301,0.3370,0.6538,
    0.2405,0.6399,0.3074,0.4131,0.3385,0.4828,
    0.3894,0.8369,0.4375,0.8609,0.4333,0.8700,
    0.4375,0.8609,0.3565,0.8418,0.3755,0.8182,
    0.3894,0.8369,0.3272,0.7940,0.3755,0.8182,
    0.3074,0.4131,0.4735,0.3063,0.4732,0.4139,
    0.2405,0.6399,0.3552,0.4342,0.2888,0.6640,
    0.2888,0.6640,0.3871,0.5042,0.3370,0.6538,
    0.2871,0.6301,0.3385,0.4828,0.3871,0.5042,
    0.4735,0.3063,0.6469,0.3397,0.6134,0.4119,
    0.3074,0.4131,0.5234,0.3300,0.3552,0.4342,
    0.3552,0.4342,0.5218,0.4354,0.3871,0.5042,
    0.3385,0.4828,0.4732,0.4139,0.5218,0.4354,
    0.6469,0.3397,0.7112,0.5410,0.6653,0.5511,
    0.4735,0.3063,0.6947,0.3608,0.5234,0.3300,
    0.5234,0.3300,0.6628,0.4337,0.5218,0.4354,
    0.4732,0.4139,0.6134,0.4119,0.6628,0.4337,
    0.7112,0.5410,0.6390,0.7407,0.6226,0.7274,
    0.6469,0.3397,0.6947,0.3608,0.7595,0.5623,
    0.6947,0.3608,0.7134,0.5695,0.6628,0.4337,
    0.6134,0.4119,0.7134,0.5695,0.6653,0.5511,
    0.3894,0.8369,0.3857,0.8547,0.3108,0.8260,
    0.7112,0.5410,0.7595,0.5623,0.6855,0.7653,
    0.6390,0.7407,0.6855,0.7653,0.6098,0.8411,
    0.5618,0.8171,0.6069,0.8281,0.6098,0.8411,
    0.6390,0.7407,0.5618,0.8171,0.5581,0.8066,
    0.6226,0.7274,0.6069,0.8281,0.5581,0.8066
]
let directory = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
// +1 EV doubles linear-light intensity, rather than doubling encoded RGB.
let exposureGain = 2.0
func exposed(_ value: CGFloat) -> CGFloat {
    let encoded = Double(value)
    let linear = encoded <= 0.04045 ? encoded / 12.92 : pow((encoded + 0.055) / 1.055, 2.4)
    let lifted = min(linear * exposureGain, 1.0)
    return CGFloat(lifted <= 0.0031308 ? lifted * 12.92 : 1.055 * pow(lifted, 1.0 / 2.4) - 0.055)
}
// AppKit renders premultiplied RGBA. Unpremultiply for exposure, then restore
// alpha coverage so the rounded edge stays clean against any Dock background.
var exposureTable = [UInt8](repeating: 0, count: 256 * 256)
for alpha in 1...255 {
    for channel in 0...255 {
        let straight = min(CGFloat(channel) / CGFloat(alpha), 1)
        let premultiplied = exposed(straight) * CGFloat(alpha)
        exposureTable[alpha * 256 + channel] = UInt8(premultiplied.rounded())
    }
}
for (name, size) in [("icon_16x16",16),("icon_16x16@2x",32),("icon_32x32",32),
                     ("icon_32x32@2x",64),("icon_128x128",128),("icon_128x128@2x",256),
                     ("icon_256x256",256),("icon_256x256@2x",512),("icon_512x512",512),("icon_512x512@2x",1024)] {
    let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
        bitmapFormat: [], bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    let graphics = NSGraphicsContext(bitmapImageRep: bitmap)!
    NSGraphicsContext.current = graphics
    let cg = graphics.cgContext
    let s = CGFloat(size)
    let plate = NSBezierPath(roundedRect: NSRect(x:s*0.04,y:s*0.04,width:s*0.92,height:s*0.92), xRadius:s*0.20,yRadius:s*0.20)
    NSGradient(starting: NSColor(calibratedRed: 0.05, green: 0.045, blue: 0.04, alpha: 1),
               ending: NSColor(calibratedRed: 0.13, green: 0.11, blue: 0.10, alpha: 1))!.draw(in: plate, angle: 90)
    plate.addClip()
    // Triangles all wind counter-clockwise; a hairline stroke closes their seams.
    let silhouette = NSBezierPath()
    for i in stride(from: 0, to: outline.count, by: 6) {
        silhouette.move(to: NSPoint(x: outline[i]*s, y: outline[i+1]*s))
        silhouette.line(to: NSPoint(x: outline[i+2]*s, y: outline[i+3]*s))
        silhouette.line(to: NSPoint(x: outline[i+4]*s, y: outline[i+5]*s))
        silhouette.close()
    }
    silhouette.lineWidth = max(s / 512, 0.5)
    silhouette.lineJoinStyle = .round
    let shape = NSColor(calibratedRed: 0.025, green: 0.025, blue: 0.035, alpha: 1)
    for (blur, colour) in [(s*0.14, NSColor(calibratedRed: 0.22, green: 0.40, blue: 0.95, alpha: 1)),
                           (s*0.07, NSColor(calibratedRed: 0.25, green: 0.45, blue: 1.0, alpha: 1)),
                           (s*0.03, NSColor(calibratedRed: 0.35, green: 0.58, blue: 1.0, alpha: 1))] {
        cg.saveGState()
        cg.setShadow(offset: .zero, blur: blur, color: colour.cgColor)
        cg.beginTransparencyLayer(auxiliaryInfo: nil)
        shape.set(); silhouette.fill(); silhouette.stroke()
        cg.endTransparencyLayer()
        cg.restoreGState()
    }
    NSGraphicsContext.restoreGraphicsState()
    let pixels = bitmap.bitmapData!
    for y in 0..<size {
        for x in 0..<size {
            let offset = y * bitmap.bytesPerRow + x * 4
            let alpha = Int(pixels[offset + 3])
            for channel in 0..<3 {
                pixels[offset + channel] = exposureTable[alpha * 256 + Int(pixels[offset + channel])]
            }
        }
    }
    try bitmap.representation(using:.png,properties:[:])!.write(to:directory.appendingPathComponent(name+".png"))
}
