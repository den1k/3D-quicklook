import Foundation
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
    /// Loads a model file and builds a ready-to-display scene. Safe to call off the main thread.
    static func build(url: URL) throws -> ModelScene {
        var mesh = QL3DMesh()
        guard ql3d_load(url.path, &mesh) == 1 else {
            let message = withUnsafeBytes(of: &mesh.error) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
            throw ModelLoadError(message: message)
        }
        let n = mesh.vertexCount

        // Positions and normals are handed to SceneKit without copying.
        let positions = Data(bytesNoCopy: mesh.positions, count: n * 12, deallocator: .free)
        let normals = Data(bytesNoCopy: mesh.normals, count: n * 12, deallocator: .free)

        // SceneKit shades in linear space and takes vertex colors as linear floats.
        var lut = [Float](repeating: 0, count: 256)
        for i in 0..<256 {
            let c = Float(i) / 255
            // Floor at a very dark gray so pure-black parts still show their shape under lighting.
            lut[i] = max(c <= 0.04045 ? c / 12.92 : powf((c + 0.055) / 1.055, 2.4), 0.02)
        }
        let colorBuf = UnsafeMutablePointer<Float>.allocate(capacity: n * 3)
        let src = mesh.colors!
        for v in 0..<n {
            colorBuf[v * 3] = lut[Int(src[v * 4])]
            colorBuf[v * 3 + 1] = lut[Int(src[v * 4 + 1])]
            colorBuf[v * 3 + 2] = lut[Int(src[v * 4 + 2])]
        }
        free(mesh.colors)
        let colors = Data(bytesNoCopy: colorBuf, count: n * 12, deallocator: .custom { p, _ in p.deallocate() })

        let indexBuf = UnsafeMutablePointer<UInt32>.allocate(capacity: n)
        for i in 0..<n { indexBuf[i] = UInt32(i) }
        let indices = Data(bytesNoCopy: indexBuf, count: n * 4, deallocator: .custom { p, _ in p.deallocate() })

        func source(_ data: Data, _ semantic: SCNGeometrySource.Semantic) -> SCNGeometrySource {
            SCNGeometrySource(data: data, semantic: semantic, vectorCount: n, usesFloatComponents: true,
                              componentsPerVector: 3, bytesPerComponent: 4, dataOffset: 0, dataStride: 12)
        }
        let element = SCNGeometryElement(data: indices, primitiveType: .triangles, primitiveCount: n / 3, bytesPerIndex: 4)
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
