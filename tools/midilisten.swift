// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
import CoreMIDI
import Foundation

// List MIDI sources, then print every message from ones named *Sampan* for N seconds.
let seconds = Double(CommandLine.arguments.dropFirst().first ?? "10") ?? 10
var client = MIDIClientRef(), port = MIDIPortRef()
MIDIClientCreate("listen" as CFString, nil, nil, &client)
MIDIInputPortCreateWithProtocol(client, "in" as CFString, ._1_0, &port) { list, _ in
    var p = list.pointee.packet
    for _ in 0..<list.pointee.numPackets {
        // A packet can hold several 32-bit MIDI 1.0 messages.
        let n = Int(p.wordCount)
        withUnsafeBytes(of: p.words) { raw in
            let words = raw.bindMemory(to: UInt32.self)
            for k in 0..<n {
                let w = words[k]
                print(String(format: "%@ midi %02X %02X %02X", ISO8601DateFormatter().string(from: Date()),
                             (w >> 16) & 0xFF, (w >> 8) & 0xFF, w & 0xFF))
            }
        }
        fflush(stdout)
        p = MIDIEventPacketNext(&p).pointee
    }
}
for i in 0..<MIDIGetNumberOfSources() {
    let src = MIDIGetSource(i)
    var name: Unmanaged<CFString>?
    MIDIObjectGetStringProperty(src, kMIDIPropertyDisplayName, &name)
    let n = (name?.takeRetainedValue() as String?) ?? "?"
    print("source:", n)
    if n.contains("Sampan") { MIDIPortConnectSource(port, src, nil) }
}
fflush(stdout)
RunLoop.main.run(until: Date().addingTimeInterval(seconds))
