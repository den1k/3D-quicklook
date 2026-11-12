import Foundation
import Metal
import SceneKit

struct ModelScene {
    let scene: SCNScene
    let cameraNode: SCNNode
    let triangleCount: Int
}

struct ModelLoadError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

enum SceneBuilder {
    /// The GPU the mesh buffers are created on; views and renderers showing the scene must use the same one.
    static let device = MTLCreateSystemDefaultDevice()!

    /// Loads a model file and builds a ready-to-display scene. Safe to call off the main thread.
    static func build(url: URL) throws -> ModelScene {
        var mesh = QL3DMesh()
        guard ql3d_load(url.path, &mesh) == 1 else {
            let message = withUnsafeBytes(of: &mesh.error) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
            throw ModelLoadError(message: message)
        }
        let n = mesh.vertexCount

        // Everything goes straight into Metal buffers so SceneKit has nothing to copy on the first frame:
        // positions and normals are used in place, colors and indices are generated directly into GPU memory.
        func inPlace(_ pointer: UnsafeMutableRawPointer, bytes: Int) -> MTLBuffer {
            let padded = (bytes + 16383) & ~16383
            if let b = device.makeBuffer(bytesNoCopy: pointer, length: padded, options: .storageModeShared,
                                         deallocator: { p, _ in free(p) }) { return b }
            defer { free(pointer) }
            return device.makeBuffer(bytes: pointer, length: bytes, options: .storageModeShared)!
        }
        let positions = inPlace(mesh.positions, bytes: n * 12)
        let normals = inPlace(mesh.normals, bytes: n * 12)
        let colors = device.makeBuffer(length: n * 12, options: .storageModeShared)!
        let indices = device.makeBuffer(length: n * 4, options: .storageModeShared)!

        // SceneKit shades in linear space and takes vertex colors as linear floats.
        var lut = [Float](repeating: 0, count: 256)
        for i in 0..<256 {
            let c = Float(i) / 255
            // Floor at a very dark gray so pure-black parts still show their shape under lighting.
            lut[i] = max(c <= 0.04045 ? c / 12.92 : powf((c + 0.055) / 1.055, 2.4), 0.02)
        }
        let colorBuf = colors.contents().bindMemory(to: Float.self, capacity: n * 3)
        let indexBuf = indices.contents().bindMemory(to: UInt32.self, capacity: n)
        let src = mesh.colors!
        let slices = max(1, min(ProcessInfo.processInfo.activeProcessorCount, n / 100_000))
        lut.withUnsafeBufferPointer { lut in
            DispatchQueue.concurrentPerform(iterations: slices) { s in
                for v in (n * s / slices)..<(n * (s + 1) / slices) {
                    colorBuf[v * 3] = lut[Int(src[v * 4])]
                    colorBuf[v * 3 + 1] = lut[Int(src[v * 4 + 1])]
                    colorBuf[v * 3 + 2] = lut[Int(src[v * 4 + 2])]
                    indexBuf[v] = UInt32(v)
                }
            }
        }
        free(mesh.colors)

        func source(_ buffer: MTLBuffer, _ semantic: SCNGeometrySource.Semantic) -> SCNGeometrySource {
            SCNGeometrySource(buffer: buffer, vertexFormat: .float3, semantic: semantic, vertexCount: n,
                              dataOffset: 0, dataStride: 12)
        }
        let element = SCNGeometryElement(buffer: indices, primitiveType: .triangles, primitiveCount: n / 3, bytesPerIndex: 4)
        let geometry = SCNGeometry(sources: [source(positions, .vertex), source(normals, .normal), source(colors, .color)],
                                   elements: [element])
        let material = SCNMaterial()
        material.lightingModel = .blinn
        material.diffuse.contents = NSColor.white
        material.specular.contents = NSColor(white: 0.3, alpha: 1)
        material.shininess = 0.15
        material.isDoubleSided = true
        geometry.materials = [material]

        // Center the model on the origin and turn Z-up models upright (SceneKit is Y-up).
        let lo = SIMD3<Float>(mesh.boundsMin.0, mesh.boundsMin.1, mesh.boundsMin.2)
        let hi = SIMD3<Float>(mesh.boundsMax.0, mesh.boundsMax.1, mesh.boundsMax.2)
        let center = (lo + hi) / 2
        var half = (hi - lo) / 2

        let modelNode = SCNNode(geometry: geometry)
        modelNode.simdPosition = -center
        let pivot = SCNNode()
        pivot.addChildNode(modelNode)
        if mesh.zUp == 1 {
            pivot.eulerAngles.x = -.pi / 2
            half = SIMD3(half.x, half.z, half.y)
        }

        // Three-quarter view from the front, backed off just far enough to fit the bounding box.
        let camera = SCNCamera()
        camera.fieldOfView = 30
        camera.automaticallyAdjustsZRange = true
        let cameraNode = SCNNode()
        cameraNode.camera = camera
        let dir = simd_normalize(SIMD3<Float>(0.55, 0.5, 0.8))
        let right = simd_normalize(simd_cross(SIMD3<Float>(0, 1, 0), dir))
        let up = simd_cross(dir, right)
        let tanV = tan(15 * Float.pi / 180), tanH = tanV * 1.3
        var distance: Float = 1e-6
        for i in 0..<8 {
            let c = half * SIMD3<Float>(i & 1 == 0 ? -1 : 1, i & 2 == 0 ? -1 : 1, i & 4 == 0 ? -1 : 1)
            distance = max(distance, simd_dot(c, dir) + max(abs(simd_dot(c, up)) / tanV, abs(simd_dot(c, right)) / tanH))
        }
        cameraNode.simdPosition = dir * distance * 1.1
        cameraNode.look(at: SCNVector3Zero)

        // Headlight that follows the camera, angled slightly so faces separate visually.
        let key = SCNLight()
        key.type = .directional
        key.intensity = 620
        let keyNode = SCNNode()
        keyNode.light = key
        keyNode.eulerAngles = SCNVector3(-0.35, 0.3, 0)
        cameraNode.addChildNode(keyNode)

        let ambient = SCNLight()
        ambient.type = .ambient
        ambient.intensity = 300
        let ambientNode = SCNNode()
        ambientNode.light = ambient

        let scene = SCNScene()
        scene.rootNode.addChildNode(pivot)
        scene.rootNode.addChildNode(cameraNode)
        scene.rootNode.addChildNode(ambientNode)
        return ModelScene(scene: scene, cameraNode: cameraNode, triangleCount: n / 3)
    }
}
