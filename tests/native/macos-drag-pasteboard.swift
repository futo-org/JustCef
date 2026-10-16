import AppKit

let board = NSPasteboard(name: .drag)
let urls = board.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
print(String(data: try! JSONEncoder().encode(urls.map { $0.path }), encoding: .utf8)!)
