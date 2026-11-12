import Cocoa
import Quartz
import SceneKit

/// SceneKit view with orbit / pan / zoom camera control.
/// Drag = rotate, two-finger scroll = pan, pinch or mouse wheel = zoom, double-click = reset.
final class ModelView: SCNView {
    private var homeTransform: SCNMatrix4?

    func show(_ model: ModelScene) {
        scene = model.scene
        pointOfView = model.cameraNode
        homeTransform = model.cameraNode.transform
        defaultCameraController.interactionMode = .orbitTurntable
        defaultCameraController.target = SCNVector3Zero
    }

    override func scrollWheel(with event: NSEvent) {
        // Trackpads keep SceneKit's default behaviour; a plain mouse wheel zooms.
        guard !event.hasPreciseScrollingDeltas, let pov = pointOfView else {
            super.scrollWheel(with: event)
            return
        }
        let distance = CGFloat(simd_length(pov.simdPosition))
        pov.localTranslate(by: SCNVector3(0, 0, -event.scrollingDeltaY * distance * 0.04))
    }

    override func mouseDown(with event: NSEvent) {
        if event.clickCount == 2, let home = homeTransform {
            SCNTransaction.begin()
            SCNTransaction.animationDuration = 0.3
            pointOfView?.transform = home
            SCNTransaction.commit()
            defaultCameraController.target = SCNVector3Zero
        } else {
            super.mouseDown(with: event)
        }
    }
}

final class PreviewViewController: NSViewController, QLPreviewingController {
    private var modelView: ModelView { view as! ModelView }

    override func loadView() {
        let v = ModelView(frame: NSRect(x: 0, y: 0, width: 900, height: 650),
                          options: [SCNView.Option.preferredDevice.rawValue: SceneBuilder.device])
        v.allowsCameraControl = true
        v.antialiasingMode = .multisampling4X
        v.backgroundColor = NSColor(name: nil) { appearance in
            appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
                ? NSColor(white: 0.17, alpha: 1) : NSColor(white: 0.92, alpha: 1)
        }
        view = v
    }

    /// Points the loader's mesh cache at this extension's own Caches folder (inside its sandbox container).
    private static let cacheConfigured: Void = {
        guard let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first else { return }
        let dir = caches.appendingPathComponent("meshes")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        ql3d_set_cache_dir(dir.path)
    }()

    func preparePreviewOfFile(at url: URL, completionHandler handler: @escaping (Error?) -> Void) {
        _ = Self.cacheConfigured
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                let model = try SceneBuilder.build(url: url)
                DispatchQueue.main.async {
                    self.modelView.show(model)
                    handler(nil)
                }
            } catch {
                DispatchQueue.main.async { handler(error) }
            }
        }
    }
}
