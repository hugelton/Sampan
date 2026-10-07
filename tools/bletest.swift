// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
//
// macOS: build as a small app bundle so Bluetooth permission can be granted
// (see tools/README-tests.md). Measures latency both ways over a generic
// CoreBluetooth connection (macOS gives such connections a 30 ms interval).
import CoreBluetooth
import CoreMIDI
import Foundation

// End-to-end check of the BLE <-> USB MIDI bridge:
//   BLE write (note on/off)  -> expect it on the USB MIDI source "Sampan"
//   USB MIDI send (CC 20 99) -> expect it as a BLE notification
let svc = CBUUID(string: "03B80E5A-EDE8-4B33-A751-6CE34EC4C700")
let chrU = CBUUID(string: "7772E5DB-3868-4112-A1A9-F2669D106BF3")
func hex(_ d: [UInt8]) -> String { d.map { String(format: "%02X", $0) }.joined(separator: " ") }
func log(_ s: String) { print(String(format: "%.3f ", Date().timeIntervalSince1970.truncatingRemainder(dividingBy: 1000)) + s); fflush(stdout) }

var client = MIDIClientRef(), inPort = MIDIPortRef(), outPort = MIDIPortRef(), usbDest = MIDIEndpointRef()
MIDIClientCreate("bletest" as CFString, nil, nil, &client)
MIDIInputPortCreateWithProtocol(client, "in" as CFString, ._1_0, &inPort) { list, _ in
    var p = list.pointee.packet
    for _ in 0..<list.pointee.numPackets {
        let n = Int(p.wordCount)
        withUnsafeBytes(of: p.words) { raw in
            let w = raw.bindMemory(to: UInt32.self)
            for k in 0..<n {
                let st = UInt8((w[k] >> 16) & 0xFF), key = UInt8((w[k] >> 8) & 0xFF)
                if st == 0x90, let t = bleSent[key] {
                    let ms = Date().timeIntervalSince(t) * 1000
                    DispatchQueue.main.async { bleToUsb.append(ms) }
                }
            }
        }
        p = MIDIEventPacketNext(&p).pointee
    }
}
MIDIOutputPortCreate(client, "out" as CFString, &outPort)
func name(_ e: MIDIEndpointRef) -> String { var n: Unmanaged<CFString>?; MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &n); return (n?.takeRetainedValue() as String?) ?? "" }
for i in 0..<MIDIGetNumberOfSources() where name(MIDIGetSource(i)) == "Sampan" { MIDIPortConnectSource(inPort, MIDIGetSource(i), nil) }
for i in 0..<MIDIGetNumberOfDestinations() where name(MIDIGetDestination(i)) == "Sampan" { usbDest = MIDIGetDestination(i) }
log("usb destination found: \(usbDest != 0)")

var bleSent = [UInt8: Date](), usbSent = [UInt8: Date]()
var bleToUsb = [Double](), usbToBle = [Double]()
func stats(_ name: String, _ v: [Double]) {
    let s = v.sorted()
    guard !s.isEmpty else { log(name + ": nothing arrived"); return }
    log(String(format: "%@: n=%d min %.1f median %.1f max %.1f ms", name, s.count, s.first!, s[s.count / 2], s.last!))
}
func report() { stats("BLE->USB", bleToUsb); stats("USB->BLE", usbToBle) }
func usbSendQuiet(_ b: [UInt8]) {
    var list = MIDIPacketList(); let p = MIDIPacketListInit(&list)
    _ = MIDIPacketListAdd(&list, 1024, p, 0, b.count, b)
    MIDISend(outPort, usbDest, &list)
}
func usbSend(_ b: [UInt8]) {
    var list = MIDIPacketList(); let p = MIDIPacketListInit(&list)
    _ = MIDIPacketListAdd(&list, 1024, p, 0, b.count, b)
    MIDISend(outPort, usbDest, &list)
    log("usb-out " + hex(b))
}

class C: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var cm: CBCentralManager!, per: CBPeripheral?, chr: CBCharacteristic?
    override init() { super.init(); cm = CBCentralManager(delegate: self, queue: nil) }
    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        log("bluetooth state \(c.state.rawValue)")
        if c.state == .poweredOn { c.scanForPeripherals(withServices: [svc]) }
    }
    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral, advertisementData a: [String: Any], rssi: NSNumber) {
        log("found \(p.name ?? "?") rssi \(rssi)")
        per = p; c.stopScan(); c.connect(p)
    }
    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        log("connected"); p.delegate = self; p.discoverServices([svc])
    }
    func peripheral(_ p: CBPeripheral, didDiscoverServices e: Error?) {
        for s in p.services ?? [] { p.discoverCharacteristics([chrU], for: s) }
    }
    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error e: Error?) {
        for ch in s.characteristics ?? [] where ch.uuid == chrU { chr = ch; p.setNotifyValue(true, for: ch) }
    }
    func peripheral(_ p: CBPeripheral, didUpdateNotificationStateFor ch: CBCharacteristic, error e: Error?) {
        log("notify on \(ch.isNotifying) mtu \(p.maximumWriteValueLength(for: .withoutResponse) + 3)")
        let n = 40, gap = 0.12
        for i in 0..<n {   // BLE -> USB: note on, key 20 + i
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0 + Double(i) * gap) {
                let key = UInt8(20 + i)
                bleSent[key] = Date()
                p.writeValue(Data([0x80, 0x80, 0x90, key, 0x40]), for: ch, type: .withoutResponse)
            }
        }
        for i in 0..<n {   // USB -> BLE: CC 21, value i
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5 + Double(n + i) * gap) {
                usbSent[UInt8(i)] = Date()
                usbSendQuiet([0xB0, 0x15, UInt8(i)])
            }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.5 + Double(2 * n) * gap) { report() }
    }
    func peripheral(_ p: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error e: Error?) {
        let d = [UInt8](ch.value ?? Data())
        // Strip the header and the timestamp before each status byte, then
        // look at every message in the packet.
        var raw = [UInt8](), afterTs = false
        for b in d.dropFirst() {
            if b & 0x80 != 0 && !afterTs { afterTs = true; continue }
            afterTs = false; raw.append(b)
        }
        var k = 0
        while k + 2 < raw.count {
            if raw[k] == 0xB0 && raw[k + 1] == 0x15, let t = usbSent[raw[k + 2]] {
                usbToBle.append(Date().timeIntervalSince(t) * 1000); k += 3
            } else { k += 1 }
        }
    }
}
let c = C()
RunLoop.main.run(until: Date().addingTimeInterval(Double(CommandLine.arguments.dropFirst().first ?? "12") ?? 12))
if let p = c.per { c.cm.cancelPeripheralConnection(p) }
log("done")
