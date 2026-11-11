// Host app. macOS only loads a Quick Look extension that lives inside an app,
// so this app exists to carry it; it just shows a short note when opened.
import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    var window: NSWindow!

    func applicationDidFinishLaunching(_ notification: Notification) {
        let label = NSTextField(wrappingLabelWithString:
            "3D Quick Look is installed.\n\nSelect a .3mf, .step, .stp, .stl or .obj file in Finder and press Space.\n\n"
            + "Drag to rotate, pinch or scroll wheel to zoom, two-finger scroll to pan, double-click to reset.")
        label.alignment = .center
        label.frame = NSRect(x: 30, y: 30, width: 400, height: 140)
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 460, height: 200),
                          styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.title = "3D Quick Look"
        window.contentView?.addSubview(label)
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
