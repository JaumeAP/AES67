//
// make-app-icons.swift
// The app icons, drawn rather than stored.
//
// Four bundles ship from this tree -- the Manager, the Controller, the
// installer and the uninstaller -- and an icon that lives only as a .icns is a
// binary nobody can revise: a change of accent colour means opening an image
// editor and hoping. This draws them with CoreGraphics at every size an .icns
// asks for, so the design is code, a change is a diff, and the small sizes are
// drawn for what they are rather than downsampled from the big one.
//
// The mark is what this software does: a sine wave that leaves the left edge
// continuous, and arrives at the right as separate packets. Audio over IP, in
// one stroke. Behind it a thin vertical line with a dot on top -- the moment a
// PTP grandmaster marks, which is what makes the packets mean anything.
//
//   Manager     the mark, whole.
//   Controller  the mark entering a routing grid, one cell lit: IS-05/IS-08.
//   Install     the mark over a tray, the last packet dropping into it.
//   Uninstall   the same tray, the packet leaving it, in a colour that says so.
//
// Usage: swift make-app-icons.swift <output-directory>
// Writes <output-directory>/<name>.iconset/*.png; iconutil turns each into a
// .icns. scripts/make-app-icons.sh does both.
//
import AppKit

// ── Palette ──────────────────────────────────────────────────────────────
// Studio dark, one signal colour, one network colour. The two accents are far
// enough apart in hue and in lightness to survive a 16-pixel tile and a
// greyscale rendering.
struct Palette {
    let backgroundTop: CGColor
    let backgroundBottom: CGColor
    let signal: CGColor       // the wave
    let packet: CGColor       // what it becomes
    let clock: CGColor        // the PTP instant

    static func rgb(_ r: Double, _ g: Double, _ b: Double, _ a: Double = 1) -> CGColor {
        CGColor(red: r / 255.0, green: g / 255.0, blue: b / 255.0, alpha: a)
    }

    static let studio = Palette(
        backgroundTop: rgb(30, 44, 71),
        backgroundBottom: rgb(12, 19, 33),
        signal: rgb(79, 216, 232),
        packet: rgb(255, 180, 77),
        clock: rgb(255, 255, 255, 0.28))

    /// The installer: the same tile, and a green that reads as "go" without
    /// leaving the family.
    static let install = Palette(
        backgroundTop: rgb(26, 56, 50),
        backgroundBottom: rgb(10, 26, 24),
        signal: rgb(126, 231, 168),
        packet: rgb(255, 205, 110),
        clock: rgb(255, 255, 255, 0.24))

    /// The uninstaller: the packet leaves, and the colour says which direction
    /// this is without a word of text.
    static let uninstall = Palette(
        backgroundTop: rgb(62, 32, 38),
        backgroundBottom: rgb(28, 13, 17),
        signal: rgb(255, 150, 140),
        packet: rgb(255, 196, 120),
        clock: rgb(255, 255, 255, 0.22))
}

// ── The tile ─────────────────────────────────────────────────────────────

/// macOS rounds an app icon at 0.2237 of its side; the content sits inside a
/// margin so nothing touches the corner radius.
func tilePath(_ side: CGFloat) -> CGPath {
    let inset = side * 0.055
    let rect = CGRect(x: inset, y: inset, width: side - 2 * inset, height: side - 2 * inset)
    return CGPath(roundedRect: rect,
                  cornerWidth: rect.width * 0.2237,
                  cornerHeight: rect.height * 0.2237,
                  transform: nil)
}

func drawTile(_ ctx: CGContext, _ side: CGFloat, _ palette: Palette) {
    let path = tilePath(side)
    ctx.saveGState()
    ctx.addPath(path)
    ctx.clip()

    let space = CGColorSpaceCreateDeviceRGB()
    let gradient = CGGradient(colorsSpace: space,
                              colors: [palette.backgroundTop, palette.backgroundBottom] as CFArray,
                              locations: [0, 1])!
    ctx.drawLinearGradient(gradient,
                           start: CGPoint(x: 0, y: side),
                           end: CGPoint(x: 0, y: 0),
                           options: [])

    // A highlight that fades out rather than ending: a band with an edge reads
    // as a seam across the tile, which is what the first draft did.
    if side >= 32 {
        let sheen = CGGradient(colorsSpace: space,
                               colors: [CGColor(red: 1, green: 1, blue: 1, alpha: 0.10),
                                        CGColor(red: 1, green: 1, blue: 1, alpha: 0.0)] as CFArray,
                               locations: [0, 1])!
        ctx.drawLinearGradient(sheen,
                               start: CGPoint(x: 0, y: side),
                               end: CGPoint(x: 0, y: side * 0.55),
                               options: [])
    }
    ctx.restoreGState()
}

// ── The mark ─────────────────────────────────────────────────────────────

// The wave lives between these two, in fractions of the tile's side. Past the
// second one it has become packets.
let waveStart: CGFloat = 0.12
let waveEnd: CGFloat = 0.46

/// Where the wave is at `x`, with `x` in 0...1 across the tile.
///
/// A cycle and a half across its own span: half of one is a bump, one is a
/// curve, and one and a half is unmistakably a wave. `damping` flattens it for
/// the packets, which carry the same phase so the two halves read as one
/// stream that changed form -- but a row that stays a row rather than
/// scattering into the corners.
func waveY(_ x: CGFloat, side: CGFloat, damping: CGFloat = 1.0,
           cycles: CGFloat = 1.5) -> CGFloat {
    let phase = (x - waveStart) / (waveEnd - waveStart) * cycles * 2.0 * .pi
    return side * 0.5 + sin(phase) * side * 0.15 * damping
}

/// Where the continuous becomes discrete: a short dashed tick, not a full-
/// height rule. The first draft drew a line from top to bottom and it read as
/// something dropped on the picture rather than part of it.
func drawClockMark(_ ctx: CGContext, _ side: CGFloat, _ palette: Palette, at x: CGFloat) {
    guard side >= 64 else { return }   // below that it is noise, not a detail
    ctx.setStrokeColor(palette.clock)
    ctx.setLineWidth(max(1, side * 0.014))
    ctx.setLineDash(phase: 0, lengths: [side * 0.035, side * 0.035])
    ctx.move(to: CGPoint(x: x, y: side * 0.26))
    ctx.addLine(to: CGPoint(x: x, y: side * 0.74))
    ctx.strokePath()
    ctx.setLineDash(phase: 0, lengths: [])
}

/// The continuous half: a stroked sine from the left edge to `until`.
func drawWave(_ ctx: CGContext, _ side: CGFloat, _ palette: Palette, until: CGFloat,
              cycles: CGFloat = 1.5) {
    let path = CGMutablePath()
    let steps = max(48, Int(side))
    let from = side * waveStart
    let to = side * until
    for step in 0...steps {
        let t = CGFloat(step) / CGFloat(steps)
        let x = from + (to - from) * t
        let point = CGPoint(x: x, y: waveY(x / side, side: side, cycles: cycles))
        if step == 0 { path.move(to: point) } else { path.addLine(to: point) }
    }
    ctx.setStrokeColor(palette.signal)
    // Thicker at small sizes: a 16-pixel tile has no room for subtlety, and a
    // stroke under a pixel and a half disappears into the background.
    ctx.setLineWidth(side < 32 ? side * 0.105 : side * 0.075)
    ctx.setLineCap(.round)
    ctx.setLineJoin(.round)
    ctx.addPath(path)
    ctx.strokePath()
}

/// The discrete half: packets marching off to the right, each centred on the
/// wave it came from. Fewer and fatter when the tile is small.
func drawPackets(_ ctx: CGContext, _ side: CGFloat, _ palette: Palette,
                 from: CGFloat, count: Int, cycles: CGFloat = 1.5) {
    // Wider than tall, and all the same size: a packet, not a bar in a chart.
    // The first draft made them tall and let the wave move them, which read as
    // a histogram.
    // Sized and spaced so the last one ends well inside the tile: the draft
    // before this ran the third packet under the rounded corner.
    let width = side * (side < 32 ? 0.12 : 0.085)
    let height = side * (side < 32 ? 0.10 : 0.075)
    let gap = width * (side < 32 ? 0.34 : 0.38)

    for index in 0..<count {
        let x = side * from + CGFloat(index) * (width + gap)
        let centre = waveY((x + width / 2) / side, side: side, damping: 0.45, cycles: cycles)
        let rect = CGRect(x: x, y: centre - height / 2, width: width, height: height)
        // Each one a little fainter: a stream leaving, not a row of bricks.
        // A gentle fade only: below about three quarters the amber turns muddy
        // against this background instead of reading as distance.
        let alpha = 1.0 - CGFloat(index) * 0.11
        ctx.setFillColor(palette.packet.copy(alpha: max(0.78, alpha))!)
        ctx.addPath(CGPath(roundedRect: rect,
                           cornerWidth: height * 0.42, cornerHeight: height * 0.42,
                           transform: nil))
        ctx.fillPath()
    }
}

// ── The four marks ───────────────────────────────────────────────────────

func drawManager(_ ctx: CGContext, _ side: CGFloat) {
    let palette = Palette.studio
    drawTile(ctx, side, palette)
    // One cycle at 16 px rather than one and a half: three crossings in eight
    // pixels is a smear, and what has to survive is "wave, then packets".
    let cycles: CGFloat = side < 32 ? 1.0 : 1.5
    let boundary: CGFloat = side < 32 ? 0.48 : waveEnd
    drawClockMark(ctx, side, palette, at: side * (boundary + 0.045))
    drawWave(ctx, side, palette, until: boundary, cycles: cycles)
    drawPackets(ctx, side, palette, from: side < 32 ? 0.58 : 0.54,
                count: side < 32 ? 2 : 3, cycles: cycles)
}

/// The Controller routes: the wave runs into a grid and one cell is lit. Three
/// by three at full size, two by two when there is no room for nine cells.
func drawController(_ ctx: CGContext, _ side: CGFloat) {
    let palette = Palette.studio
    drawTile(ctx, side, palette)

    // At 16 px a grid is a smudge, so it becomes what a grid is for: one
    // crosspoint, made, with the wave running into it.
    if side < 32 {
        drawWave(ctx, side, palette, until: 0.50, cycles: 1.0)
        let box = side * 0.26
        let rect = CGRect(x: side * 0.60, y: side * 0.5 - box / 2, width: box, height: box)
        ctx.setFillColor(palette.packet)
        ctx.addPath(CGPath(roundedRect: rect, cornerWidth: box * 0.3,
                           cornerHeight: box * 0.3, transform: nil))
        ctx.fillPath()
        return
    }

    drawWave(ctx, side, palette, until: 0.44)

    let cells = 3
    let gridSide = side * 0.32
    let origin = CGPoint(x: side * 0.50, y: side * 0.5 - gridSide / 2)
    let cell = gridSide / CGFloat(cells)
    let pad = cell * 0.18

    for row in 0..<cells {
        for column in 0..<cells {
            let rect = CGRect(x: origin.x + CGFloat(column) * cell + pad,
                              y: origin.y + CGFloat(row) * cell + pad,
                              width: cell - 2 * pad, height: cell - 2 * pad)
            // The cell the wave runs into, lit: the crosspoint that is made.
            // It was the top-left corner in the first draft, which is nowhere
            // near where the signal arrives.
            let lit = (row == cells / 2 && column == 0)
            ctx.setFillColor(lit ? palette.packet
                                 : CGColor(red: 1, green: 1, blue: 1, alpha: 0.18))
            ctx.addPath(CGPath(roundedRect: rect,
                               cornerWidth: rect.width * 0.28,
                               cornerHeight: rect.width * 0.28, transform: nil))
            ctx.fillPath()
        }
    }
}

/// A tray, and the packet going into it or coming out of it. The arrow is the
/// whole message at 16 px, so it is drawn first and biggest.
func drawTrayMark(_ ctx: CGContext, _ side: CGFloat, _ palette: Palette, installing: Bool) {
    drawTile(ctx, side, palette)

    // At 16 px the wave costs contrast and gives nothing: an installer has one
    // thing to say, and it is the direction. The arrow takes the whole tile.
    let small = side < 32
    if !small {
        drawWave(ctx, side, palette, until: 0.42)
    }

    let centreX = small ? side * 0.5 : side * 0.63
    let trayY = side * (small ? 0.24 : 0.28)
    let trayHalf = side * (small ? 0.22 : 0.15)
    ctx.setStrokeColor(palette.signal)
    ctx.setLineWidth(side * (small ? 0.085 : 0.065))
    ctx.setLineCap(.round)
    ctx.move(to: CGPoint(x: centreX - trayHalf, y: trayY))
    ctx.addLine(to: CGPoint(x: centreX + trayHalf, y: trayY))
    ctx.strokePath()

    // The arrow: down into the tray to install, up out of it to remove.
    let near = side * (small ? 0.12 : 0.10)
    let far = side * (small ? 0.50 : 0.42)
    let tip = installing ? CGPoint(x: centreX, y: trayY + near)
                         : CGPoint(x: centreX, y: trayY + far)
    let tail = installing ? CGPoint(x: centreX, y: trayY + far)
                          : CGPoint(x: centreX, y: trayY + near)
    ctx.setStrokeColor(palette.packet)
    ctx.setLineWidth(side * (small ? 0.11 : 0.075))
    ctx.move(to: tail)
    ctx.addLine(to: tip)
    ctx.strokePath()

    let head = side * (small ? 0.16 : 0.11)
    let direction: CGFloat = installing ? 1 : -1
    ctx.move(to: CGPoint(x: tip.x - head, y: tip.y + head * direction))
    ctx.addLine(to: tip)
    ctx.addLine(to: CGPoint(x: tip.x + head, y: tip.y + head * direction))
    ctx.strokePath()
}

func drawInstall(_ ctx: CGContext, _ side: CGFloat) {
    drawTrayMark(ctx, side, Palette.install, installing: true)
}

func drawUninstall(_ ctx: CGContext, _ side: CGFloat) {
    drawTrayMark(ctx, side, Palette.uninstall, installing: false)
}

// ── Writing the iconset ──────────────────────────────────────────────────

/// Every size an .icns carries, and the name iconutil expects for each.
let iconSizes: [(name: String, pixels: Int)] = [
    ("icon_16x16", 16), ("icon_16x16@2x", 32),
    ("icon_32x32", 32), ("icon_32x32@2x", 64),
    ("icon_128x128", 128), ("icon_128x128@2x", 256),
    ("icon_256x256", 256), ("icon_256x256@2x", 512),
    ("icon_512x512", 512), ("icon_512x512@2x", 1024),
]

func render(_ pixels: Int, _ draw: (CGContext, CGFloat) -> Void) -> Data? {
    let space = CGColorSpaceCreateDeviceRGB()
    guard let ctx = CGContext(data: nil, width: pixels, height: pixels,
                              bitsPerComponent: 8, bytesPerRow: 0, space: space,
                              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
        return nil
    }
    ctx.setAllowsAntialiasing(true)
    ctx.setShouldAntialias(true)
    draw(ctx, CGFloat(pixels))
    guard let image = ctx.makeImage() else { return nil }
    return NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:])
}

let arguments = CommandLine.arguments
guard arguments.count == 2 else {
    FileHandle.standardError.write("usage: make-app-icons.swift <output-directory>\n".data(using: .utf8)!)
    exit(2)
}
let outputDirectory = URL(fileURLWithPath: arguments[1], isDirectory: true)

let icons: [(String, (CGContext, CGFloat) -> Void)] = [
    ("AES67Manager", drawManager),
    ("AES67Controller", drawController),
    ("AES67Install", drawInstall),
    ("AES67Uninstall", drawUninstall),
]

for (name, draw) in icons {
    let iconset = outputDirectory.appendingPathComponent("\(name).iconset", isDirectory: true)
    try? FileManager.default.removeItem(at: iconset)
    try! FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)

    for size in iconSizes {
        guard let png = render(size.pixels, draw) else {
            FileHandle.standardError.write("failed to render \(name) at \(size.pixels)\n".data(using: .utf8)!)
            exit(1)
        }
        try! png.write(to: iconset.appendingPathComponent("\(size.name).png"))
    }
    print("drew \(name).iconset")
}
