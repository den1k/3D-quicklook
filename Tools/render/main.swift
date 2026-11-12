// Renders a model to a PNG using the same scene the Quick Look extension shows.
// Usage: ql3d-render <model> <out.png>
import AppKit
import Metal
import SceneKit

let args = CommandLine.arguments
guard args.count == 3 else {
    print("usage: ql3d-render <model> <out.png>")
    exit(2)
}
do {
    let start = Date()
    let model = try SceneBuilder.build(url: URL(fileURLWithPath: args[1]))
    let loaded = Date()
    model.scene.background.contents = NSColor(white: 0.92, alpha: 1)
    let renderer = SCNRenderer(device: SceneBuilder.device, options: nil)
    renderer.scene = model.scene
    renderer.pointOfView = model.cameraNode
    let image = renderer.snapshot(atTime: 0, with: CGSize(width: 1200, height: 800), antialiasingMode: .multisampling4X)
    let rep = NSBitmapImageRep(data: image.tiffRepresentation!)!
    try rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: args[2]))
    print(String(format: "%d triangles, scene built in %.0f ms, first frame in %.0f ms",
                 model.triangleCount, loaded.timeIntervalSince(start) * 1000, Date().timeIntervalSince(loaded) * 1000))
} catch {
    print("error: \(error.localizedDescription)")
    exit(1)
}
