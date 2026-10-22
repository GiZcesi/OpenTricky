// ot-tool: the settings window and the iCloud sync of OpenTricky's macOS app.
//
//   ot-tool gui [--auto]      the settings window. Exit 0 = Play (start the game), 1 = closed / quit.
//                             --auto: only show it if "show at launch" is on or Option is held; otherwise exit 0 at once.
//   ot-tool sync pull|push    reconcile settings + game saves between the local folder and iCloud Drive (never fails the caller).
//   ot-tool status            one line about where settings and saves live.
//
// Storage. The working copy is always LOCAL (~/Library/Application Support/OpenTricky): the game runs from it, so a missing network
// or an iCloud that is slow to download never stops it. When iCloud Drive is available the settings and the saves are mirrored to
// ~/Library/Mobile Documents/com~apple~CloudDocs/OpenTricky (a plain iCloud Drive folder: no entitlement or developer account
// needed): "pull" before the game starts, "push" after it ends. Newer file wins; a local file about to be replaced is first copied
// to <local>/sync-backups/<time>/; nothing is ever deleted by a sync.
import Foundation
import SwiftUI
import AppKit
import CoreAudio

// MARK: - locations

enum Store {
    static let home = ProcessInfo.processInfo.environment["OT_TEST_HOME"].map { URL(fileURLWithPath: $0) } ?? FileManager.default.homeDirectoryForCurrentUser   // OT_TEST_HOME: for tests
    static let local = home.appendingPathComponent("Library/Application Support/OpenTricky")
    static let cloudDocs = home.appendingPathComponent("Library/Mobile Documents/com~apple~CloudDocs")
    static let cloud = cloudDocs.appendingPathComponent("OpenTricky")
    static let settingsFile = local.appendingPathComponent("display.env")
    static let prefsFile = local.appendingPathComponent("launcher-prefs.txt")      // this machine only, not synced
    static let iniFile = local.appendingPathComponent("settings.ini")             // the game's own settings file ([Keyboard] and [Controller] bindings)
    static let discFile = local.appendingPathComponent("disc-image.txt")           // this machine only, not synced
    /// what is mirrored to iCloud: paths relative to the root
    static let synced = ["display.env", "settings.ini", "hdd"]

    static var cloudAvailable: Bool {
        var isDir: ObjCBool = false
        return FileManager.default.fileExists(atPath: cloudDocs.path, isDirectory: &isDir) && isDir.boolValue
            && FileManager.default.isWritableFile(atPath: cloudDocs.path)
    }
}

// MARK: - prefs (key=value lines)

struct KV {
    var lines: [String]
    init(lines: [String]) { self.lines = lines }
    init(url: URL) { lines = ((try? String(contentsOf: url, encoding: .utf8)) ?? "").replacingOccurrences(of: "\r", with: "").components(separatedBy: "\n") ; if lines.last == "" { lines.removeLast() } }
    static func split(_ l: String) -> (String, String)? {
        let t = l.trimmingCharacters(in: .whitespaces)
        if t.hasPrefix("#") { return nil }
        guard let eq = t.firstIndex(of: "=") else { return nil }
        var v = String(t[t.index(after: eq)...]).trimmingCharacters(in: .whitespaces)
        if v.count >= 2, (v.hasPrefix("'") && v.hasSuffix("'")) || (v.hasPrefix("\"") && v.hasSuffix("\"")) { v = String(v.dropFirst().dropLast()) }
        return (String(t[..<eq]).trimmingCharacters(in: .whitespaces), v)
    }
    func get(_ k: String) -> String? {
        var r: String?
        for l in lines { if let (a, b) = KV.split(l), a == k { r = b } }
        return r
    }
    /// nil removes the setting (the game's own default applies); other lines and comments are kept
    mutating func set(_ k: String, _ v: String?) {
        lines.removeAll { KV.split($0)?.0 == k }
        if let v = v, !v.isEmpty {
            let q = v.contains(where: { " '\"$`\\!&;()<>|*?[]{}~#".contains($0) }) ? "'" + v.replacingOccurrences(of: "'", with: "'\\''") + "'" : v
            lines.append("\(k)=\(q)")
        }
    }
    func write(to url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try (lines.joined(separator: "\n") + "\n").write(to: url, atomically: true, encoding: .utf8)
    }
}

// MARK: - settings.ini (key and button bindings)

struct Ini {
    var lines: [String]
    init(url: URL) { lines = ((try? String(contentsOf: url, encoding: .utf8)) ?? "").replacingOccurrences(of: "\r", with: "").components(separatedBy: "\n"); if lines.last == "" { lines.removeLast() } }
    private func sectionRange(_ sec: String) -> Range<Int>? {
        guard let s = lines.firstIndex(where: { $0.trimmingCharacters(in: .whitespaces).caseInsensitiveCompare("[\(sec)]") == .orderedSame }) else { return nil }
        var e = lines.count
        for i in (s + 1)..<lines.count where lines[i].trimmingCharacters(in: .whitespaces).hasPrefix("[") { e = i; break }
        return s..<e
    }
    func get(_ sec: String, _ key: String) -> String? {
        guard let r = sectionRange(sec) else { return nil }
        for i in r.dropFirst() {
            let l = lines[i].trimmingCharacters(in: .whitespaces)
            if l.hasPrefix(";") || l.hasPrefix("#") { continue }
            if let eq = l.firstIndex(of: "="), l[..<eq].trimmingCharacters(in: .whitespaces).caseInsensitiveCompare(key) == .orderedSame {
                return String(l[l.index(after: eq)...]).trimmingCharacters(in: .whitespaces)
            }
        }
        return nil
    }
    /// nil removes the key (the game's default applies); the rest of the file, comments included, is kept
    mutating func set(_ sec: String, _ key: String, _ value: String?) {
        if let r = sectionRange(sec) {
            for i in r.dropFirst() {
                let l = lines[i].trimmingCharacters(in: .whitespaces)
                if l.hasPrefix(";") || l.hasPrefix("#") { continue }
                if let eq = l.firstIndex(of: "="), l[..<eq].trimmingCharacters(in: .whitespaces).caseInsensitiveCompare(key) == .orderedSame {
                    if let v = value { lines[i] = "\(key)=\(v)" } else { lines.remove(at: i) }
                    return
                }
            }
            if let v = value { lines.insert("\(key)=\(v)", at: r.upperBound) }
        } else if let v = value {
            if let last = lines.last, !last.isEmpty { lines.append("") }
            lines.append("[\(sec)]"); lines.append("\(key)=\(v)")
        }
    }
    func write(to url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try (lines.joined(separator: "\n") + "\n").write(to: url, atomically: true, encoding: .utf8)
    }
}

enum Bindings {
    /// name in settings.ini, label, default key, default controller input
    static let keyRows: [(String, String, String, String)] = [
        ("Cross", "Cross (A)", "Space", "A"), ("Circle", "Circle (B)", "Escape", "B"), ("Square", "Square (X)", "C", "X"), ("Triangle", "Triangle (Y)", "V", "Y"),
        ("L1", "L1 (white)", "F", "LB"), ("R1", "R1 (black)", "R", "RB"), ("L2", "L2 (left trigger)", "Q", "LT"), ("R2", "R2 (right trigger)", "E", "RT"),
        ("Start", "Start", "Enter", "Start"), ("Select", "Select (back)", "Tab", "Back"), ("L3", "L3 (left stick press)", "X", "LS"), ("R3", "R3 (right stick press)", "None", "RS"),
        ("DpadUp", "D-pad up", "Up", "DpadUp"), ("DpadDown", "D-pad down", "Down", "DpadDown"), ("DpadLeft", "D-pad left", "Left", "DpadLeft"), ("DpadRight", "D-pad right", "Right", "DpadRight"),
    ]
    static let stickRows: [(String, String, String)] = [
        ("LeftStickUp", "Left stick up", "Up"), ("LeftStickDown", "Left stick down", "Down"), ("LeftStickLeft", "Left stick left", "Left"), ("LeftStickRight", "Left stick right", "Right"),
        ("RightStickUp", "Right stick up", "None"), ("RightStickDown", "Right stick down", "None"), ("RightStickLeft", "Right stick left", "None"), ("RightStickRight", "Right stick right", "None"),
    ]
    static let keys: [String] = ["None"] + (65...90).map { String(UnicodeScalar($0)!) } + (0...9).map { String($0) } + (1...12).map { "F\($0)" } + [
        "Space", "Enter", "Escape", "Tab", "Backspace", "Up", "Down", "Left", "Right", "LeftShift", "RightShift", "LeftCtrl", "RightCtrl",
        "Insert", "Delete", "Home", "End", "PageUp", "PageDown", "Num0", "Num1", "Num2", "Num3", "Num4", "Num5", "Num6", "Num7", "Num8", "Num9",
        "Num*", "Num+", "Num-", "Num.", "Num/", ";", "=", ",", "-", ".", "/", "`", "[", "\\", "]", "'"]
    static let pads = ["None", "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Start", "Back", "LS", "RS", "DpadUp", "DpadDown", "DpadLeft", "DpadRight"]
}

// MARK: - iCloud mirror

enum Sync {
    static let fm = FileManager.default
    static var log: [String] = []

    /// the physical path (URL.resolvingSymlinksInPath keeps /var for /private/var, the enumerator does not)
    static func real(_ u: URL) -> String {
        guard let p = realpath(u.path, nil) else { return u.path }
        defer { free(p) }
        return String(cString: p)
    }

    /// relative paths of regular files under root/rel (for a placeholder ".name.icloud" the real name is reported)
    static func files(root: URL, rel: String) -> [String] {
        let base = root.appendingPathComponent(rel)
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: base.path, isDirectory: &isDir) else {
            let ph = base.deletingLastPathComponent().appendingPathComponent("." + base.lastPathComponent + ".icloud")
            return fm.fileExists(atPath: ph.path) ? [rel] : []
        }
        if !isDir.boolValue { return [rel] }
        var out: [String] = []
        let rootPath = real(root)      // the enumerator reports resolved paths (/var -> /private/var)
        if let e = fm.enumerator(at: URL(fileURLWithPath: real(base)), includingPropertiesForKeys: [.isRegularFileKey], options: []) {
            for case let u as URL in e {
                let isFile = (try? u.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) ?? false
                if !isFile { continue }
                var r = String(u.path.dropFirst(rootPath.count + 1))
                let name = u.lastPathComponent
                if name.hasPrefix(".") && name.hasSuffix(".icloud") {
                    r = String(r.dropLast(name.count)) + String(name.dropFirst().dropLast(7))
                } else if name == ".DS_Store" { continue }
                out.append(r)
            }
        }
        return out
    }

    /// make sure a cloud file is on disk (a placeholder is asked to download; waits a little)
    static func materialize(_ url: URL) -> Bool {
        if fm.fileExists(atPath: url.path) { return true }
        let ph = url.deletingLastPathComponent().appendingPathComponent("." + url.lastPathComponent + ".icloud")
        guard fm.fileExists(atPath: ph.path) else { return false }
        try? fm.startDownloadingUbiquitousItem(at: url)
        for _ in 0..<40 { if fm.fileExists(atPath: url.path) { return true }; Thread.sleep(forTimeInterval: 0.5) }
        log.append("not downloaded yet: \(url.lastPathComponent) (kept the local copy)")
        return false
    }

    static func mtime(_ u: URL) -> Date? { (try? fm.attributesOfItem(atPath: u.path)[.modificationDate]) as? Date }

    static func copy(_ src: URL, _ dst: URL) -> Bool {
        do {
            try fm.createDirectory(at: dst.deletingLastPathComponent(), withIntermediateDirectories: true)
            let tmp = dst.deletingLastPathComponent().appendingPathComponent(".ot-\(UUID().uuidString).tmp")
            try fm.copyItem(at: src, to: tmp)
            if let m = mtime(src) { try? fm.setAttributes([.modificationDate: m], ofItemAtPath: tmp.path) }
            if fm.fileExists(atPath: dst.path) { _ = try fm.replaceItemAt(dst, withItemAt: tmp) } else { try fm.moveItem(at: tmp, to: dst) }
            return true
        } catch { log.append("copy failed: \(dst.lastPathComponent): \(error.localizedDescription)"); return false }
    }

    /// false = a local file exists and could not be saved aside: the caller must not replace it
    static func backupLocal(_ rel: String, stamp: String) -> Bool {
        let src = Store.local.appendingPathComponent(rel)
        guard fm.fileExists(atPath: src.path) else { return true }
        let dst = Store.local.appendingPathComponent("sync-backups/\(stamp)/\(rel)")
        do {
            try fm.createDirectory(at: dst.deletingLastPathComponent(), withIntermediateDirectories: true)
            try fm.copyItem(at: src, to: dst)
            return true
        } catch { log.append("no backup of \(rel), kept it: \(error.localizedDescription)"); return false }
    }

    // What each synced file looked like when both sides last agreed (its modification time): a file that changed on BOTH sides since is a conflict.
    static var stateURL: URL { Store.local.appendingPathComponent("sync-state.json") }
    static func loadState() -> [String: Double] {
        guard let d = try? Data(contentsOf: stateURL), let o = try? JSONSerialization.jsonObject(with: d) as? [String: Double] else { return [:] }
        return o
    }
    static func saveState(_ s: [String: Double]) { if let d = try? JSONSerialization.data(withJSONObject: s) { try? d.write(to: stateURL) } }

    /// Game saves kept after each game that changed them (save-history/<time>/hdd/...), the last 20; the settings window restores one.
    static let historyDir = Store.local.appendingPathComponent("save-history")
    static func snapshot(rels: [String], stamp: String) {
        let saves = rels.filter { $0.hasPrefix("hdd/") }
        guard !saves.isEmpty else { return }
        for rel in saves {
            let src = Store.local.appendingPathComponent(rel), dst = historyDir.appendingPathComponent("\(stamp)/\(rel)")
            try? fm.createDirectory(at: dst.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? fm.copyItem(at: src, to: dst)
        }
        if let all = try? fm.contentsOfDirectory(atPath: historyDir.path).sorted(), all.count > 20 {
            for old in all.dropLast(20) { try? fm.removeItem(at: historyDir.appendingPathComponent(old)) }
        }
    }
    static func historyList() -> [String] { ((try? fm.contentsOfDirectory(atPath: historyDir.path)) ?? []).filter { !$0.hasPrefix(".") }.sorted().reversed() }
    /// Puts a snapshot's saves back (what they replace is first copied to sync-backups); returns a message.
    static func restore(_ stamp: String) -> String {
        let root = historyDir.appendingPathComponent(stamp)
        let rels = files(root: root, rel: "hdd")
        guard !rels.isEmpty else { return "That snapshot is empty." }
        let now = DateFormatter(); now.dateFormat = "yyyyMMdd-HHmmss"; let bk = now.string(from: Date())
        var n = 0
        for rel in rels {
            if !backupLocal(rel, stamp: bk) { continue }
            if copy(root.appendingPathComponent(rel), Store.local.appendingPathComponent(rel)) {
                try? fm.setAttributes([.modificationDate: Date()], ofItemAtPath: Store.local.appendingPathComponent(rel).path)     // newer than the cloud: it wins the next sync
                n += 1
            }
        }
        return "Restored \(n) file(s) from \(stamp); what they replaced is in sync-backups/\(bk)."
    }

    /// pull: cloud -> local where the cloud file is newer or the local one is missing. push: local -> cloud likewise.
    /// A file changed on both sides since the last sync is a conflict: the newer one wins, the other is kept in sync-backups, and sync-notice.txt says so.
    static func run(pull: Bool) -> String {
        guard Prefs.icloudOn else { return "iCloud sync is off: using local storage only" }
        guard Store.cloudAvailable else { return "iCloud Drive is not available: using local storage only" }
        try? fm.createDirectory(at: Store.cloud, withIntermediateDirectories: true)
        let (from, to) = pull ? (Store.cloud, Store.local) : (Store.local, Store.cloud)
        let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"; let stamp = f.string(from: Date())
        var state = loadState(), moved = 0, kept = 0, conflicts: [String] = [], movedRels: [String] = []
        for top in Store.synced {
            for rel in files(root: from, rel: top) {
                let s = from.appendingPathComponent(rel), d = to.appendingPathComponent(rel)
                if pull && !materialize(s) { continue }
                guard fm.fileExists(atPath: s.path), let sm = mtime(s) else { continue }
                if let dm = mtime(d) {
                    if !(sm > dm.addingTimeInterval(2)) { kept += 1; if abs(sm.timeIntervalSince(dm)) <= 2 { state[rel] = dm.timeIntervalSince1970 }; continue }     // 2 s: file systems round times differently
                    if let last = state[rel], dm.timeIntervalSince1970 > last + 2 {          // the side about to be replaced also changed since the last sync
                        conflicts.append("\(rel): this Mac and iCloud both changed it; kept the newer (\(pull ? "iCloud's" : "this Mac's")), the other is in sync-backups")
                        if !pull { _ = saveAside(cloud: d, rel: rel, stamp: stamp) }
                    }
                }
                if pull && !backupLocal(rel, stamp: stamp) { continue }
                if copy(s, d) { moved += 1; movedRels.append(rel); if let m = mtime(d) { state[rel] = m.timeIntervalSince1970 } }
            }
        }
        saveState(state)
        if !pull && moved > 0 { snapshot(rels: movedRels, stamp: stamp) }
        if !conflicts.isEmpty {
            let notice = Store.local.appendingPathComponent("sync-notice.txt")
            let text = "Your settings or saves were changed on two Macs (or on this Mac and in iCloud Drive) since they last synced.\n\n" + conflicts.joined(separator: "\n") + "\n"
            try? text.write(to: notice, atomically: true, encoding: .utf8)
        }
        var line = "iCloud \(pull ? "pull" : "push"): \(moved) updated, \(kept) already current"
        if !conflicts.isEmpty { line += "; CONFLICT: " + conflicts.joined(separator: " | ") }
        if !log.isEmpty { line += "; " + log.joined(separator: "; ") }
        return line
    }

    /// keep iCloud's version of a file that a push is about to replace: sync-backups/<time>/icloud/<rel>
    static func saveAside(cloud: URL, rel: String, stamp: String) -> Bool {
        let dst = Store.local.appendingPathComponent("sync-backups/\(stamp)/icloud/\(rel)")
        do { try fm.createDirectory(at: dst.deletingLastPathComponent(), withIntermediateDirectories: true); try fm.copyItem(at: cloud, to: dst); return true }
        catch { log.append("could not keep iCloud's \(rel): \(error.localizedDescription)"); return false }
    }
}

enum Prefs {
    static var kv: KV { KV(url: Store.prefsFile) }
    static var showAtLaunch: Bool { kv.get("show_settings") != "0" }
    static var icloudOn: Bool { kv.get("icloud_sync") != "0" }
    static func save(show: Bool, icloud: Bool) {
        var k = kv; k.set("show_settings", show ? "1" : "0"); k.set("icloud_sync", icloud ? "1" : "0")
        try? k.write(to: Store.prefsFile)
    }
}

// MARK: - audio outputs

enum AudioOutputs {
    /// names of the output devices (CoreAudio), for the "Audio output" picker; SDL uses the same names
    static func names() -> [String] {
        var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size) == noErr else { return [] }
        var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids) == noErr else { return [] }
        var out: [String] = []
        for id in ids {
            var o = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreams, mScope: kAudioDevicePropertyScopeOutput, mElement: kAudioObjectPropertyElementMain)
            var sz: UInt32 = 0
            guard AudioObjectGetPropertyDataSize(id, &o, 0, nil, &sz) == noErr, sz > 0 else { continue }       // no output streams: an input-only device
            var n = AudioObjectPropertyAddress(mSelector: kAudioObjectPropertyName, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
            var cf: Unmanaged<CFString>?; var nsz = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
            if AudioObjectGetPropertyData(id, &n, 0, nil, &nsz, &cf) == noErr, let c = cf { out.append(c.takeRetainedValue() as String) }
        }
        return out
    }
}

// MARK: - diagnostics

enum Diagnostics {
    static func tail(_ url: URL, lines n: Int) -> String {
        guard let d = try? Data(contentsOf: url) else { return "(none)" }
        let s = String(decoding: d.suffix(200_000), as: UTF8.self)
        return s.components(separatedBy: "\n").suffix(n).joined(separator: "\n")
    }
    /// What a bug report needs: the app and system, the settings, whether iCloud is in use, the last crash report and the end of the last log.
    static func text() -> String {
        let info = ProcessInfo.processInfo
        var s = "OpenTricky diagnostics, \(Date())\n"
        s += "macOS \(info.operatingSystemVersionString), \(info.processorCount) cores, \(info.physicalMemory / 1_073_741_824) GB\n"
        s += "iCloud: \(Store.cloudAvailable && Prefs.icloudOn ? "on (\(Store.cloud.path))" : "off or unavailable")\n\n"
        s += "== display.env ==\n" + ((try? String(contentsOf: Store.settingsFile, encoding: .utf8)) ?? "(none)") + "\n"
        s += "== crash.txt ==\n" + tail(Store.local.appendingPathComponent("crash.txt"), lines: 60) + "\n\n"
        s += "== sync.log (end) ==\n" + tail(Store.local.appendingPathComponent("sync.log"), lines: 15) + "\n\n"
        let log = tail(Store.local.appendingPathComponent("last-run.log"), lines: 400)
        let keep = log.components(separatedBy: "\n").filter { l in !l.hasPrefix("[VSH]") && !l.hasPrefix("[LIVE-PB]") && !l.hasPrefix("[NV2A-NOP]") && !l.contains("  [KERNEL]") }
        s += "== last-run.log (end, noise removed) ==\n" + keep.suffix(120).joined(separator: "\n") + "\n"
        return s
    }
}

// MARK: - settings model

struct Choice: Hashable { let value: String; let label: String }

final class Model: ObservableObject {
    // value "" = the game's own default (the key is left out of the file)
    @Published var shape = ""
    @Published var windowH = ""
    @Published var cursorHide = ""
    @Published var upscale = ""
    @Published var aniso = ""
    @Published var minLoad = ""
    @Published var countdown = ""
    @Published var rumble = true
    @Published var audioDevice = ""
    @Published var keyBind: [String: String] = [:]      // settings.ini [Keyboard]
    @Published var padBind: [String: String] = [:]      // settings.ini [Controller]
    @Published var renderH = ""
    @Published var fullscreen = false
    @Published var fps = ""
    @Published var vsync = ""
    @Published var smaa = ""
    @Published var softShadows = false
    @Published var hdr = false
    @Published var gamut = 1.0
    @Published var warm = 0.1
    @Published var boost = 0.35
    @Published var scene = 0.45
    @Published var peak = 1.5
    @Published var buttons = ""
    @Published var hdPath = ""
    @Published var hdMenus = false
    @Published var showAtLaunch = true
    @Published var icloud = true
    @Published var disc = ""
    @Published var message = ""

    var kv = KV(lines: [])

    static let shapes = [Choice(value: "", label: "Automatic (fit the window)"), Choice(value: "4:3", label: "4:3 (original)"), Choice(value: "16:9", label: "16:9"),
                         Choice(value: "21:9", label: "21:9 ultrawide"), Choice(value: "32:9", label: "32:9 super ultrawide")]
    static let heights = [Choice(value: "", label: "1080p (default)"), Choice(value: "720", label: "720p"), Choice(value: "1440", label: "1440p"), Choice(value: "2160", label: "4K (2160p)")]
    static let windowHeights = [Choice(value: "", label: "Large (900 high, default)"), Choice(value: "540", label: "Small (540)"), Choice(value: "720", label: "Medium (720)"),
                                Choice(value: "1080", label: "Extra large (1080)")]
    static let cursorChoices = [Choice(value: "", label: "After 2 seconds (default)"), Choice(value: "1000", label: "After 1 second"), Choice(value: "5000", label: "After 5 seconds"),
                                Choice(value: "0", label: "Never")]
    static let upscaleChoices = [Choice(value: "", label: "Plain (original, default)"), Choice(value: "edge", label: "Edge-adaptive (FSR 1 style)"), Choice(value: "sharp", label: "Sharp bicubic")]
    static let anisoChoices = [Choice(value: "", label: "Off (original, default)"), Choice(value: "2", label: "2x"), Choice(value: "4", label: "4x"), Choice(value: "8", label: "8x"),
                               Choice(value: "16", label: "16x")]
    static let loadChoices = [Choice(value: "", label: "As fast as it loads (original, default)"), Choice(value: "2000", label: "At least 2 seconds"),
                              Choice(value: "3000", label: "At least 3 seconds"), Choice(value: "5000", label: "At least 5 seconds")]
    static let countdownChoices = [Choice(value: "", label: "Original speed (default)"), Choice(value: "1.5", label: "50% longer"), Choice(value: "2", label: "Twice as long")]
    static let fpsChoices = [Choice(value: "", label: "60 (original)"), Choice(value: "120", label: "120"), Choice(value: "144", label: "144"), Choice(value: "240", label: "240"),
                             Choice(value: "monitor", label: "Match my display"), Choice(value: "0", label: "Unlimited")]
    static let vsyncs = [Choice(value: "", label: "Default"), Choice(value: "vsync", label: "On"), Choice(value: "adaptive", label: "Adaptive"), Choice(value: "off", label: "Off")]
    static let smaas = [Choice(value: "", label: "Off"), Choice(value: "low", label: "Low"), Choice(value: "medium", label: "Medium"), Choice(value: "high", label: "High"), Choice(value: "ultra", label: "Ultra")]
    static let buttonSets = [Choice(value: "", label: "The game's own"), Choice(value: "modern", label: "Modern (Xbox Series)"), Choice(value: "playstation", label: "PlayStation"), Choice(value: "ps2", label: "PS2")]

    init() { load() }

    func num(_ k: String, _ d: Double) -> Double { kv.get(k).flatMap(Double.init) ?? d }
    func on(_ v: String?) -> Bool { guard let v = v?.lowercased() else { return false }; return ["1", "on", "yes", "true"].contains(v) }

    func load() {
        kv = KV(url: Store.settingsFile)
        let s = kv.get("OT_SHAPE") ?? ""; shape = s == "auto" ? "" : s
        renderH = kv.get("OT_RENDER_H") ?? ""
        windowH = kv.get("OT_WINDOW_H") ?? ""; if windowH == "900" { windowH = "" }
        rumble = kv.get("OT_RUMBLE") != "0"
        audioDevice = kv.get("OT_AUDIO_DEVICE") ?? ""
        let ini = Ini(url: Store.iniFile)
        for r in Bindings.keyRows { keyBind[r.0] = ini.get("Keyboard", r.0) ?? r.2; padBind[r.0] = ini.get("Controller", r.0) ?? r.3 }
        for r in Bindings.stickRows { keyBind[r.0] = ini.get("Keyboard", r.0) ?? r.2 }
        minLoad = kv.get("OT_MIN_LOAD_MS") ?? ""; if minLoad == "0" { minLoad = "" }
        countdown = kv.get("OT_COUNTDOWN_SLOW") ?? ""; if countdown == "1" { countdown = "" }
        aniso = kv.get("OT_ANISO") ?? ""; if aniso == "0" || aniso == "1" { aniso = "" }
        upscale = kv.get("OT_UPSCALE") ?? ""; if upscale == "smooth" || upscale == "plain" { upscale = "" }
        cursorHide = kv.get("OT_CURSOR_HIDE_MS") ?? ""; if cursorHide == "2000" { cursorHide = "" }
        fullscreen = on(kv.get("OT_FULLSCREEN"))
        fps = kv.get("OT_FPS") ?? ""; if fps == "60" { fps = "" }
        vsync = kv.get("OT_VSYNC") ?? ""
        let sm = (kv.get("OT_SMAA") ?? "").lowercased(); smaa = ["off", "0", "no", "false"].contains(sm) ? "" : sm
        softShadows = on(kv.get("OT_SOFT_SHADOWS"))
        hdr = on(kv.get("OT_HDR"))
        gamut = num("OT_HDR_GAMUT", 1.0); warm = num("OT_HDR_WARM", 0.1); boost = num("OT_HDR_BOOST", 0.35); scene = num("OT_HDR_SCENE", 0.45); peak = num("OT_HDR_PEAK", 1.5)
        buttons = kv.get("OT_BUTTONS") ?? ""
        hdPath = kv.get("OT_HD_TEXTURES") ?? ""; hdMenus = on(kv.get("OT_HD_MENUS"))
        showAtLaunch = Prefs.showAtLaunch; icloud = Prefs.icloudOn
        disc = (try? String(contentsOf: Store.discFile, encoding: .utf8)) ?? ""
        // a value the pickers do not offer (typed by hand in the file) must survive: add it
    }

    func resetBindings() {
        for r in Bindings.keyRows { keyBind[r.0] = r.2; padBind[r.0] = r.3 }
        for r in Bindings.stickRows { keyBind[r.0] = r.2 }
    }

    func save() -> Bool {
        var k = kv
        k.set("OT_SHAPE", shape.isEmpty ? nil : shape)
        k.set("OT_RENDER_H", renderH.isEmpty ? nil : renderH)
        k.set("OT_WINDOW_H", windowH.isEmpty ? nil : windowH)
        k.set("OT_CURSOR_HIDE_MS", cursorHide.isEmpty ? nil : cursorHide)
        k.set("OT_UPSCALE", upscale.isEmpty ? nil : upscale)
        k.set("OT_ANISO", aniso.isEmpty ? nil : aniso)
        k.set("OT_MIN_LOAD_MS", minLoad.isEmpty ? nil : minLoad)
        k.set("OT_COUNTDOWN_SLOW", countdown.isEmpty ? nil : countdown)
        k.set("OT_RUMBLE", rumble ? nil : "0")
        k.set("OT_AUDIO_DEVICE", audioDevice.isEmpty ? nil : audioDevice)
        k.set("OT_FULLSCREEN", fullscreen ? "1" : nil)
        k.set("OT_FPS", fps.isEmpty ? nil : fps)
        k.set("OT_VSYNC", vsync.isEmpty ? nil : vsync)
        k.set("OT_SMAA", smaa.isEmpty ? nil : smaa)
        k.set("OT_SOFT_SHADOWS", softShadows ? "on" : nil)
        k.set("OT_HDR", hdr ? "on" : nil)
        func tune(_ key: String, _ v: Double, _ d: Double) { k.set(key, hdr && abs(v - d) > 0.001 ? String(format: "%.2f", v) : nil) }
        tune("OT_HDR_GAMUT", gamut, 1.0); tune("OT_HDR_WARM", warm, 0.1); tune("OT_HDR_BOOST", boost, 0.35); tune("OT_HDR_SCENE", scene, 0.45); tune("OT_HDR_PEAK", peak, 1.5)
        k.set("OT_BUTTONS", buttons.isEmpty ? nil : buttons)
        k.set("OT_HD_TEXTURES", hdPath.isEmpty ? nil : hdPath)
        k.set("OT_HD_MENUS", hdMenus && !hdPath.isEmpty ? "1" : nil)
        var ini = Ini(url: Store.iniFile)
        for r in Bindings.keyRows {
            ini.set("Keyboard", r.0, keyBind[r.0] == r.2 ? nil : keyBind[r.0]); ini.set("Controller", r.0, padBind[r.0] == r.3 ? nil : padBind[r.0])
        }
        for r in Bindings.stickRows { ini.set("Keyboard", r.0, keyBind[r.0] == r.2 ? nil : keyBind[r.0]) }
        do {
            try ini.write(to: Store.iniFile)
            try k.write(to: Store.settingsFile)
            kv = k
            Prefs.save(show: showAtLaunch, icloud: icloud)
            if !disc.isEmpty { try disc.write(to: Store.discFile, atomically: true, encoding: .utf8) }
            if icloud { message = Sync.run(pull: false) }      // the settings reach iCloud now, not only after the next game
            return true
        } catch { message = "Could not save: \(error.localizedDescription)"; return false }
    }
}

// MARK: - the window

struct Pick: View {
    let title: String; let choices: [Choice]; @Binding var value: String
    var body: some View {
        Picker(title, selection: $value) {
            if !choices.contains(where: { $0.value == value }) { Text(value).tag(value) }     // a hand-typed value stays selectable
            ForEach(choices, id: \.self) { Text($0.label).tag($0.value) }
        }
    }
}

struct SettingsView: View {
    @StateObject var m = Model()
    var play: () -> Void
    var live = false          // opened from inside the running game: no Play, changes apply the next time the game starts
    var body: some View {
        VStack(spacing: 0) {
            Form {
                Section("Display") {
                    Pick(title: "Screen shape", choices: Model.shapes, value: $m.shape)
                    Pick(title: "Render resolution", choices: Model.heights, value: $m.renderH)
                    Pick(title: "Upscaling when the window is larger", choices: Model.upscaleChoices, value: $m.upscale)
                    Pick(title: "Window size", choices: Model.windowHeights, value: $m.windowH)
                    Toggle("Start in full screen", isOn: $m.fullscreen)
                    Pick(title: "Hide the mouse pointer", choices: Model.cursorChoices, value: $m.cursorHide)
                    Text("In the game: F11 or Alt+Enter full screen, Cmd+1…4 window sizes, F9 / F10 smaller / larger (these leave full screen), F12 screenshot, Cmd+, or F2 (or the controller's PlayStation button, or Share+Options together) this window.").font(.caption).foregroundStyle(.secondary)
                }
                Section("Frame rate") {
                    Pick(title: "Frame rate limit", choices: Model.fpsChoices, value: $m.fps)
                    Pick(title: "VSync", choices: Model.vsyncs, value: $m.vsync)
                    Text("Above 60 the game draws extra in-between frames; it needs a fast Mac and a high-refresh display to show.").font(.caption).foregroundStyle(.secondary)
                }
                Section("Picture quality") {
                    Pick(title: "Anti-aliasing (SMAA)", choices: Model.smaas, value: $m.smaa)
                    Pick(title: "Texture filtering (anisotropic)", choices: Model.anisoChoices, value: $m.aniso)
                    Toggle("Soft shadows", isOn: $m.softShadows)
                    Toggle("HDR / wide colour (needs an HDR display)", isOn: $m.hdr)
                    if m.hdr {
                        DisclosureGroup("HDR tuning") {
                            Slider(value: $m.gamut, in: 0...1) { Text("Colour range \(m.gamut, specifier: "%.2f")") }
                            Slider(value: $m.scene, in: 0...1) { Text("Share for the 3D picture \(m.scene, specifier: "%.2f")") }
                            Slider(value: $m.boost, in: 0...1) { Text("Highlight lift \(m.boost, specifier: "%.2f")") }
                            Slider(value: $m.warm, in: 0...0.5) { Text("Warm colours \(m.warm, specifier: "%.2f")") }
                            Slider(value: $m.peak, in: 1...3) { Text("Peak brightness \(m.peak, specifier: "%.1f")x") }
                        }
                    }
                }
                Section("Pacing") {
                    Pick(title: "Loading screens stay up", choices: Model.loadChoices, value: $m.minLoad)
                    Pick(title: "Start-gate countdown lasts", choices: Model.countdownChoices, value: $m.countdown)
                    Text("From an SSD a track loads in under a second; the console took many. These give the loading screen and the 3-2-1 time to be seen.").font(.caption).foregroundStyle(.secondary)
                }
                Section("Textures and controls") {
                    Pick(title: "Button pictures", choices: Model.buttonSets, value: $m.buttons)
                    Toggle("Controller vibration", isOn: $m.rumble)
                    Picker("Audio output", selection: $m.audioDevice) {
                        Text("System default (follows your choice in macOS)").tag("")
                        ForEach(AudioOutputs.names(), id: \.self) { Text($0).tag($0) }
                        if !m.audioDevice.isEmpty && !AudioOutputs.names().contains(m.audioDevice) { Text(m.audioDevice).tag(m.audioDevice) }
                    }
                    HStack {
                        Text("HD texture pack"); Spacer()
                        Text(m.hdPath.isEmpty ? "none" : (m.hdPath as NSString).lastPathComponent).foregroundStyle(.secondary).lineLimit(1)
                        Button("Choose…") { chooseFolder { m.hdPath = $0 } }
                        if !m.hdPath.isEmpty { Button("Clear") { m.hdPath = "" } }
                    }
                    Toggle("Use HD textures in the menus too", isOn: $m.hdMenus).disabled(m.hdPath.isEmpty)
                }
                Section("Controls") {
                    DisclosureGroup("Keyboard") {
                        ForEach(Bindings.keyRows, id: \.0) { r in
                            Picker(r.1, selection: Binding(get: { m.keyBind[r.0] ?? r.2 }, set: { m.keyBind[r.0] = $0 })) { ForEach(Bindings.keys, id: \.self) { Text($0).tag($0) } }
                        }
                        ForEach(Bindings.stickRows, id: \.0) { r in
                            Picker(r.1, selection: Binding(get: { m.keyBind[r.0] ?? r.2 }, set: { m.keyBind[r.0] = $0 })) { ForEach(Bindings.keys, id: \.self) { Text($0).tag($0) } }
                        }
                    }
                    DisclosureGroup("Controller buttons") {
                        ForEach(Bindings.keyRows, id: \.0) { r in
                            Picker(r.1, selection: Binding(get: { m.padBind[r.0] ?? r.3 }, set: { m.padBind[r.0] = $0 })) { ForEach(Bindings.pads, id: \.self) { Text($0).tag($0) } }
                        }
                    }
                    Button("Reset keys and buttons to the defaults") { m.resetBindings() }
                    Text("Named after the PlayStation buttons of the original game. Cross and Start always keep a key.").font(.caption).foregroundStyle(.secondary)
                }
                Section("Game disc and storage") {
                    HStack {
                        Text("Disc image"); Spacer()
                        Text(m.disc.isEmpty ? "not chosen yet" : (m.disc as NSString).lastPathComponent).foregroundStyle(.secondary).lineLimit(1)
                        Button("Choose…") { chooseFile { m.disc = $0 } }
                    }
                    Toggle("Keep settings and game saves in iCloud Drive", isOn: $m.icloud).disabled(!Store.cloudAvailable)
                    Text(storageNote(m.icloud)).font(.caption).foregroundStyle(.secondary)
                    DisclosureGroup("Save history (restore an earlier save)") {
                        let h = Sync.historyList()
                        if h.isEmpty { Text("None yet: a snapshot is kept after each game that changed a save.").font(.caption).foregroundStyle(.secondary) }
                        ForEach(h.prefix(10), id: \.self) { s in
                            HStack { Text(s); Spacer(); Button("Restore") { m.message = Sync.restore(s) } }
                        }
                    }
                    HStack {
                        Button("Show local folder") { NSWorkspace.shared.open(Store.local) }
                        if Store.cloudAvailable { Button("Show iCloud folder") { try? FileManager.default.createDirectory(at: Store.cloud, withIntermediateDirectories: true); NSWorkspace.shared.open(Store.cloud) } }
                    }
                    HStack {
                        Button("Copy diagnostics") {
                            NSPasteboard.general.clearContents(); NSPasteboard.general.setString(Diagnostics.text(), forType: .string)
                            m.message = "Diagnostics copied: paste them into a bug report."
                        }
                        Button("Show last log") { NSWorkspace.shared.open(Store.local.appendingPathComponent("last-run.log")) }
                    }
                    Toggle("Show this window every time the game starts (hold Option to open it if off)", isOn: $m.showAtLaunch)
                }
            }.formStyle(.grouped)
            if !m.message.isEmpty { Text(m.message).font(.caption).foregroundStyle(.secondary).padding(.horizontal) }
            HStack {
                Button(live ? "Close" : "Quit") { exit(live ? 0 : 1) }.keyboardShortcut(.cancelAction)
                if live { Text("Changes apply the next time the game starts.").font(.caption).foregroundStyle(.secondary) }
                Spacer()
                if live { Button("Save") { if m.save() { m.message = "Saved. " + m.message } }.keyboardShortcut(.defaultAction) }
                else {
                    Button("Save") { _ = m.save() }
                    Button("Save and Play") { if m.save() { play() } }.keyboardShortcut(.defaultAction)
                }
            }.padding()
        }.frame(minWidth: 540, minHeight: 640)
    }

    func storageNote(_ on: Bool) -> String {
        if !Store.cloudAvailable { return "iCloud Drive is not available on this Mac: settings and saves stay in \(Store.local.path)." }
        if !on { return "Local only: \(Store.local.path)" }
        return "Worked on locally (so the game never waits for the network), and mirrored to iCloud Drive › OpenTricky before and after each game. The newer copy wins; a replaced copy is kept in sync-backups."
    }
    func chooseFolder(_ done: (String) -> Void) {
        let p = NSOpenPanel(); p.canChooseDirectories = true; p.canChooseFiles = false; p.prompt = "Choose"
        if p.runModal() == .OK, let u = p.url { done(u.path) }
    }
    func chooseFile(_ done: (String) -> Void) {
        let p = NSOpenPanel(); p.canChooseDirectories = false; p.canChooseFiles = true; p.message = "Choose your SSX Tricky (USA) Xbox disc image"
        if p.runModal() == .OK, let u = p.url { done(u.path) }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func applicationWillTerminate(_ notification: Notification) { exit(args.contains("--live") ? 0 : 1) }       // closed without Play: the game does not start
    func applicationWillFinishLaunching(_ notification: Notification) { NSApp.setActivationPolicy(.regular) }      // a bare helper process is not a foreground app by default
    func applicationDidFinishLaunching(_ notification: Notification) { NSApp.activate(ignoringOtherApps: true) }
}

// MARK: - entry

let args = CommandLine.arguments
switch args.count > 1 ? args[1] : "gui" {
case "sync":
    print(Sync.run(pull: (args.count > 2 ? args[2] : "pull") == "pull")); exit(0)
case "selftest":      // load the settings, change some, save, and print the file (used by the tests; does not touch iCloud)
    let m = Model(); m.icloud = false; m.fps = "120"; m.softShadows = true; m.shape = ""; m.hdPath = "/tmp/HD pack's"; m.hdMenus = true; m.keyBind["Cross"] = "Z"; m.padBind["Circle"] = "X"; m.keyBind["Start"] = "Enter"; m.gamut = 0.8; m.hdr = true
    exit(m.save() ? 0 : 1)
case "restore":
    print(Sync.restore(args.count > 2 ? args[2] : "")); exit(0)
case "diagnostics":
    print(Diagnostics.text()); exit(0)
case "outputs":
    print(AudioOutputs.names().joined(separator: "\n")); exit(0)
case "status":
    print("local: \(Store.local.path); iCloud: \(Store.cloudAvailable && Prefs.icloudOn ? Store.cloud.path : "off/unavailable")"); exit(0)
default:
    let force = args.contains("--settings") || args.contains("--live")
    if args.contains("--auto") && !force && !Prefs.showAtLaunch && !NSEvent.modifierFlags.contains(.option) { exit(0) }
    let app = NSApplication.shared
    let delegate = AppDelegate()
    app.delegate = delegate
    app.setActivationPolicy(.regular)
    let win = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 600, height: 740), styleMask: [.titled, .closable, .miniaturizable, .resizable],
                       backing: .buffered, defer: false)
    win.title = "SSX Tricky Settings"
    win.contentView = NSHostingView(rootView: SettingsView(play: { exit(0) }, live: args.contains("--live")))
    win.minSize = NSSize(width: 540, height: 640)
    win.center()
    win.isReleasedWhenClosed = false
    win.makeKeyAndOrderFront(nil)
    app.activate(ignoringOtherApps: true)
    app.run()
}
