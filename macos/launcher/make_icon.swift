// Draws the app icon (1024x1024 PNG): swiftc make_icon.swift -o make_icon && ./make_icon out.png
import AppKit

let S: CGFloat = 1024
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 1024, pixelsHigh: 1024, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                           isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let ctx = NSGraphicsContext.current!.cgContext

func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> NSColor { NSColor(deviceRed: r / 255, green: g / 255, blue: b / 255, alpha: a) }

// macOS icon shape: rounded square inside the standard margin
let tile = NSRect(x: 100, y: 100, width: 824, height: 824)
let shape = NSBezierPath(roundedRect: tile, xRadius: 185, yRadius: 185)
ctx.saveGState()
shape.addClip()

// sky: deep violet at the top to hot orange at the horizon
NSGradient(colors: [rgb(38, 24, 92), rgb(150, 38, 120), rgb(255, 96, 52), rgb(255, 186, 64)],
           atLocations: [0, 0.45, 0.78, 1], colorSpace: .deviceRGB)!.draw(in: tile, angle: 90)

// sun
rgb(255, 232, 150, 0.95).setFill()
NSBezierPath(ovalIn: NSRect(x: 560, y: 520, width: 190, height: 190)).fill()
rgb(255, 232, 150, 0.25).setFill()
NSBezierPath(ovalIn: NSRect(x: 520, y: 480, width: 270, height: 270)).fill()

// far range, near range (snow with a shaded side)
func peak(_ pts: [(CGFloat, CGFloat)], _ c: NSColor) {
    let p = NSBezierPath(); p.move(to: NSPoint(x: pts[0].0, y: pts[0].1))
    for q in pts.dropFirst() { p.line(to: NSPoint(x: q.0, y: q.1)) }
    p.close(); c.setFill(); p.fill()
}
peak([(80, 100), (80, 380), (260, 540), (360, 440), (520, 600), (700, 430), (830, 520), (950, 380), (950, 100)], rgb(92, 50, 130))
peak([(80, 100), (80, 300), (300, 470), (440, 350), (620, 520), (800, 340), (950, 430), (950, 100)], rgb(236, 240, 255))
peak([(620, 520), (800, 340), (950, 430), (950, 100), (700, 100), (760, 330)], rgb(176, 188, 226))
peak([(300, 470), (440, 350), (520, 100), (330, 100), (380, 300)], rgb(176, 188, 226))

// a jump ramp and the rider's board arcing over it: a bold white slash with a trail
ctx.setLineCap(.round)
rgb(255, 255, 255, 0.35).setStroke()
let trail = NSBezierPath(); trail.lineWidth = 20
trail.move(to: NSPoint(x: 190, y: 330)); trail.curve(to: NSPoint(x: 640, y: 640), controlPoint1: NSPoint(x: 330, y: 330), controlPoint2: NSPoint(x: 520, y: 470)); trail.stroke()
ctx.saveGState()
ctx.translateBy(x: 700, y: 668); ctx.rotate(by: .pi / 5.2)
let board = NSBezierPath(roundedRect: NSRect(x: -150, y: -24, width: 300, height: 48), xRadius: 24, yRadius: 24)
rgb(24, 20, 48).setFill(); board.fill()
rgb(255, 120, 40).setFill(); NSBezierPath(roundedRect: NSRect(x: -150, y: -24, width: 300, height: 14), xRadius: 7, yRadius: 7).fill()
ctx.restoreGState()

// wordmark
let para = NSMutableParagraphStyle(); para.alignment = .center
let font = NSFont.systemFont(ofSize: 210, weight: .black)
let shadow = NSShadow(); shadow.shadowColor = rgb(255, 255, 255, 0.85); shadow.shadowOffset = NSSize(width: 0, height: -6); shadow.shadowBlurRadius = 18
let text = NSAttributedString(string: "OSSX", attributes: [.font: font, .foregroundColor: rgb(54, 20, 104), .paragraphStyle: para, .kern: 6, .shadow: shadow,
                                                           .obliqueness: 0.22])
text.draw(in: NSRect(x: 100, y: 150, width: 824, height: 300))
ctx.restoreGState()

// a thin inner edge for depth
rgb(255, 255, 255, 0.22).setStroke(); shape.lineWidth = 4; shape.stroke()
NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
