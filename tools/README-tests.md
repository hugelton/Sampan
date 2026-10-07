# Device test helpers (macOS)

- `midilisten.swift`: print MIDI from sources named "Sampan".
  `swiftc -O -o midilisten midilisten.swift && ./midilisten 60`
- `bletest.swift`: connect to Sampan over BLE MIDI and check the bridge both
  ways (BLE write -> USB MIDI in, USB MIDI out -> BLE notification). It needs
  Bluetooth permission, so it runs as an app bundle:

```
swiftc -O -o bletest bletest.swift
mkdir -p bletest.app/Contents/MacOS && cp bletest bletest.app/Contents/MacOS/
# Info.plist with CFBundleExecutable=bletest, LSUIElement, NSBluetoothAlwaysUsageDescription
codesign -s - --force bletest.app
open -W --stdout out.txt --stderr err.txt bletest.app --args 15
```

- `midilat.swift`: latency through the bridge using CoreMIDI only. Connect
  Sampan first in Audio MIDI Setup (MIDI Studio, Bluetooth configuration);
  this is the path real use takes, and macOS then picks a 7.5 ms interval.
  `swiftc -O -o midilat midilat.swift && ./midilat 60`
