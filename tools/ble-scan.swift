// SPDX-License-Identifier: GPL-3.0-only
// The Mac side of `node tools/op.mjs log --scan`: listens for the Bluetooth test image's advertisement and never connects.
//   ble-scan SECONDS [SERVICE-UUID]
// One JSON object per line: every sighting of the target, then {"done":...} with the adapter state and the other devices seen.
import CoreBluetooth
import Foundation

let seconds = Double(CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "20") ?? 20
let filter = CommandLine.arguments.count > 2 ? [CBUUID(string: CommandLine.arguments[2])] : nil
let midi = "03B80E5A-EDE8-4B33-A751-6CE34EC4C700"

func emit(_ o: [String: Any]) {
  print(String(data: try! JSONSerialization.data(withJSONObject: o), encoding: .utf8)!)
}

final class Scanner: NSObject, CBCentralManagerDelegate {
  var central: CBCentralManager!
  var others = Set<UUID>()
  func finish() {
    emit(["done": true, "auth": CBCentralManager.authorization.rawValue, "state": central.state.rawValue, "others": others.count])
    exit(0)
  }
  func centralManagerDidUpdateState(_ c: CBCentralManager) {
    switch c.state {
    case .poweredOn: c.scanForPeripherals(withServices: filter, options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
    case .unknown, .resetting: break
    default: finish()                                        // off, unauthorized or unsupported: the scan cannot run
    }
  }
  func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral, advertisementData ad: [String: Any], rssi: NSNumber) {
    let name = (ad[CBAdvertisementDataLocalNameKey] as? String) ?? p.name ?? ""
    let services = (ad[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID])?.map { $0.uuidString } ?? []
    if name.contains("OpenPocket") || services.contains(midi) {
      emit(["id": p.identifier.uuidString, "name": name, "rssi": rssi.intValue, "services": services,
            "connectable": (ad[CBAdvertisementDataIsConnectable] as? NSNumber)?.boolValue ?? false])
    } else { others.insert(p.identifier) }
  }
}

let scanner = Scanner()
scanner.central = CBCentralManager(delegate: scanner, queue: nil)
DispatchQueue.main.asyncAfter(deadline: .now() + seconds) { scanner.finish() }
RunLoop.main.run()
