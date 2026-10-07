// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
//
// Latency through the Sampan bridge using CoreMIDI only: the BLE side must be
// connected in Audio MIDI Setup (Bluetooth configuration). The two endpoints
// named "Sampan" are told apart by their driver.
import CoreMIDI
import Foundation

func str(_ o: MIDIObjectRef, _ k: CFString) -> String {
    var s: Unmanaged<CFString>?; MIDIObjectGetStringProperty(o, k, &s); return (s?.takeRetainedValue() as String?) ?? ""
}
func driver(_ e: MIDIEndpointRef) -> String {
    var ent = MIDIEntityRef(), dev = MIDIDeviceRef()
    MIDIEndpointGetEntity(e, &ent); MIDIEntityGetDevice(ent, &dev)
    return str(dev, kMIDIPropertyDriverOwner)
}
func log(_ s: String) { print(s); fflush(stdout) }

var usbSrc = MIDIEndpointRef(), usbDst = MIDIEndpointRef(), bleSrc = MIDIEndpointRef(), bleDst = MIDIEndpointRef()
for i in 0..<MIDIGetNumberOfSources() {
    let e = MIDIGetSource(i); guard str(e, kMIDIPropertyDisplayName).contains("Sampan") else { continue }
    let d = driver(e); log("source \(str(e, kMIDIPropertyDisplayName)) driver \(d)")
    if d.lowercased().contains("bluetooth") { bleSrc = e } else { usbSrc = e }
}
for i in 0..<MIDIGetNumberOfDestinations() {
    let e = MIDIGetDestination(i); guard str(e, kMIDIPropertyDisplayName).contains("Sampan") else { continue }
    let d = driver(e); log("dest   \(str(e, kMIDIPropertyDisplayName)) driver \(d)")
    if d.lowercased().contains("bluetooth") { bleDst = e } else { usbDst = e }
}
guard usbSrc != 0, usbDst != 0, bleSrc != 0, bleDst != 0 else { log("need both USB and BLE Sampan endpoints"); exit(1) }

var client = MIDIClientRef(), inPort = MIDIPortRef(), outPort = MIDIPortRef()
var sent = [String: UInt64](), lat = [String: [Double]]()
let lock = NSLock()
func now() -> UInt64 { clock_gettime_nsec_np(CLOCK_UPTIME_RAW) }
MIDIClientCreate("midilat" as CFString, nil, nil, &client)
MIDIInputPortCreateWithProtocol(client, "in" as CFString, ._1_0, &inPort) { list, ref in
    let side = ref == UnsafeMutableRawPointer(bitPattern: 1) ? "BLE->USB" : "USB->BLE"
    var p = list.pointee.packet
    for _ in 0..<list.pointee.numPackets {
        let n = Int(p.wordCount)
        withUnsafeBytes(of: p.words) { raw in
            let w = raw.bindMemory(to: UInt32.self)
            for k in 0..<n {
                let st = (w[k] >> 16) & 0xF0, key = (w[k] >> 8) & 0xFF
                guard st == 0x90 else { continue }
                let id = "\(side) \(key)", t = now()
                lock.lock(); if let s = sent[id] { lat[side, default: []].append(Double(t - s) / 1e6) }; lock.unlock()
            }
        }
        p = MIDIEventPacketNext(&p).pointee
    }
}
MIDIOutputPortCreate(client, "out" as CFString, &outPort)
// What arrives on the USB source came in over BLE, and the other way round.
MIDIPortConnectSource(inPort, usbSrc, UnsafeMutableRawPointer(bitPattern: 1))
MIDIPortConnectSource(inPort, bleSrc, UnsafeMutableRawPointer(bitPattern: 2))

func send(_ dst: MIDIEndpointRef, _ b: [UInt8]) {
    var list = MIDIPacketList(); let p = MIDIPacketListInit(&list)
    _ = MIDIPacketListAdd(&list, 1024, p, 0, b.count, b); MIDISend(outPort, dst, &list)
}
let n = Int(CommandLine.arguments.dropFirst().first ?? "60") ?? 60
for i in 0..<n {
    let key = UInt8(20 + i % 80)
    lock.lock(); sent["BLE->USB \(key)"] = now(); lock.unlock()
    send(bleDst, [0x90, key, 0x40]); send(bleDst, [0x80, key, 0])
    Thread.sleep(forTimeInterval: 0.1)
    lock.lock(); sent["USB->BLE \(key)"] = now(); lock.unlock()
    send(usbDst, [0x90, key, 0x40]); send(usbDst, [0x80, key, 0])
    Thread.sleep(forTimeInterval: 0.1)
}
Thread.sleep(forTimeInterval: 0.5)
for side in ["BLE->USB", "USB->BLE"] {
    let s = (lat[side] ?? []).sorted()
    if s.isEmpty { log("\(side): nothing arrived"); continue }
    log(String(format: "%@: n=%d min %.1f median %.1f p90 %.1f max %.1f ms", side, s.count, s[0], s[s.count / 2], s[s.count * 9 / 10], s[s.count - 1]))
}
