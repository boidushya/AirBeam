import Cocoa

class AppDelegate: NSObject, NSApplicationDelegate {
    let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    let pauseFile = NSString("~/.airbeam-pause").expandingTildeInPath
    var timer: Timer?

    func applicationDidFinishLaunching(_ notification: Notification) {
        updateIcon()
        buildMenu()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            self?.updateIcon()
            self?.buildMenu()
        }
    }

    var isPaused: Bool {
        FileManager.default.fileExists(atPath: pauseFile)
    }

    func updateIcon() {
        let symbolName = isPaused ? "speaker.slash.fill" : "speaker.wave.2.fill"
        if let image = NSImage(systemSymbolName: symbolName, accessibilityDescription: "AirBeam") {
            image.isTemplate = true
            statusItem.button?.image = image
        }
    }

    func buildMenu() {
        let menu = NSMenu()

        let statusItem = NSMenuItem(title: isPaused ? "AirBeam: Paused" : "AirBeam: Active", action: nil, keyEquivalent: "")
        statusItem.isEnabled = false
        menu.addItem(statusItem)

        menu.addItem(NSMenuItem.separator())

        let toggleTitle = isPaused ? "Resume Auto-Switch" : "Pause Auto-Switch"
        let toggleItem = NSMenuItem(title: toggleTitle, action: #selector(toggle), keyEquivalent: "t")
        toggleItem.target = self
        menu.addItem(toggleItem)

        menu.addItem(NSMenuItem.separator())

        let quitItem = NSMenuItem(title: "Quit", action: #selector(quit), keyEquivalent: "q")
        quitItem.target = self
        menu.addItem(quitItem)

        self.statusItem.menu = menu
    }

    @objc func toggle() {
        if isPaused {
            try? FileManager.default.removeItem(atPath: pauseFile)
        } else {
            FileManager.default.createFile(atPath: pauseFile, contents: nil)
        }
        updateIcon()
        buildMenu()
    }

    @objc func quit() {
        NSApp.terminate(nil)
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let delegate = AppDelegate()
app.delegate = delegate
app.run()
