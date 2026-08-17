import CoreBluetooth
import Darwin
import Foundation

let deviceName = "StopWatchHID"
let balanceServiceUUID = CBUUID(string: "0000fff0-0000-1000-8000-00805f9b34fb")  // 0xFFF0
let balanceCharUUID = CBUUID(string: "0000fff1-0000-1000-8000-00805f9b34fb")    // 0xFFF1
let hidServiceUUID = CBUUID(string: "1812")                                       // HID

enum ClientError: LocalizedError {
    case bluetooth(String)
    case missingAPIKey

    var errorDescription: String? {
        switch self {
        case .bluetooth(let message): return message
        case .missingAPIKey: return "请先设置环境变量 DEEPSEEK_API_KEY"
        }
    }
}

private func usage() -> String {
    """
    用法:
      ds-balance --balance         抓取并打印余额
      ds-balance --push-balance    抓取后写入 StopWatch BLE
      ds-balance --watch           每 60 秒抓取并写入（持续）
    API key 从环境变量 DEEPSEEK_API_KEY 读取。
    """
}

// MARK: - BLE 余额写入（CoreBluetooth，借鉴 codex-watch-companion）

final class BLEBalanceWriter: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private var manager: CBCentralManager!
    private var peripheral: CBPeripheral?
    private let payload: Data
    private let verbose: Bool
    private var finished = false
    private var success = false
    private var failureReason: String?

    init(payload: Data, verbose: Bool) {
        self.payload = payload
        self.verbose = verbose
        super.init()
    }

    func write(timeout: TimeInterval = 25) throws {
        manager = CBCentralManager(delegate: self, queue: nil)
        let deadline = Date().addingTimeInterval(timeout)
        while !finished && Date() < deadline {
            RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.1))
        }
        manager.stopScan()
        if !finished {
            if let peripheral { manager.cancelPeripheralConnection(peripheral) }
            throw ClientError.bluetooth("未在 \(Int(timeout)) 秒内找到并连上 StopWatchHID")
        }
        if let failureReason { throw ClientError.bluetooth(failureReason) }
        if !success { throw ClientError.bluetooth("余额写入未完成") }
    }

    private func finish(success: Bool, reason: String? = nil) {
        guard !finished else { return }
        finished = true
        self.success = success
        self.failureReason = reason
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            // 优先找系统里已经连上的 HID 设备（配对后一直连着，可能不再广播）
            let connected = central.retrieveConnectedPeripherals(withServices: [hidServiceUUID, balanceServiceUUID])
            if let peripheral = connected.first {
                use(peripheral, central: central)
                return
            }
            if verbose { print("扫描 StopWatchHID …") }
            central.scanForPeripherals(withServices: nil)
        case .unauthorized:
            finish(success: false, reason: "macOS 未授权此程序使用蓝牙")
        case .poweredOff:
            finish(success: false, reason: "Mac 蓝牙已关闭")
        case .unsupported:
            finish(success: false, reason: "此 Mac 不支持 CoreBluetooth")
        default:
            break
        }
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        guard self.peripheral == nil else { return }
        let name = (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? peripheral.name ?? ""
        guard name == deviceName else { return }
        use(peripheral, central: central)
    }

    private func use(_ peripheral: CBPeripheral, central: CBCentralManager) {
        self.peripheral = peripheral
        peripheral.delegate = self
        central.stopScan()
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        if verbose { print("已连接 \(peripheral.name ?? deviceName)，发现余额服务…") }
        peripheral.discoverServices([balanceServiceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        finish(success: false, reason: "连接失败：\(error?.localizedDescription ?? "未知")")
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        if !finished { finish(success: false, reason: "连接断开") }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error { finish(success: false, reason: "发现服务失败：\(error.localizedDescription)"); return }
        guard let service = peripheral.services?.first(where: { $0.uuid == balanceServiceUUID }) else {
            finish(success: false, reason: "未找到余额服务 0xFFF0。请确认手表右下角显示 DS --（最新固件）；\n若已显示，请在 Mac 蓝牙里忘记 StopWatchHID 后重新配对，再重试。")
            return
        }
        peripheral.discoverCharacteristics([balanceCharUUID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        if let error { finish(success: false, reason: "发现特征失败：\(error.localizedDescription)"); return }
        guard let characteristic = service.characteristics?.first(where: { $0.uuid == balanceCharUUID }) else {
            finish(success: false, reason: "未找到余额特征 0xFFF1")
            return
        }
        if characteristic.properties.contains(.writeWithoutResponse) {
            peripheral.writeValue(payload, for: characteristic, type: .withoutResponse)
            // 无响应写入：留一点时间把包发出去后即视为成功
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { [weak self] in
                self?.finish(success: true)
            }
        } else if characteristic.properties.contains(.write) {
            peripheral.writeValue(payload, for: characteristic, type: .withResponse)
        } else {
            finish(success: false, reason: "特征不支持写入")
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error {
            finish(success: false, reason: "写入失败：\(error.localizedDescription)")
        } else {
            finish(success: true)
        }
    }
}

// MARK: - 主逻辑

func pushBalance(_ balance: String) throws {
    let writer = BLEBalanceWriter(payload: Data(balance.utf8), verbose: true)
    try writer.write()
    print("[push] balance='\(balance)' -> \(deviceName)")
}

func main() {
    let args = CommandLine.arguments
    guard args.count >= 2 else {
        print(usage())
        exit(2)
    }
    let mode = args[1]

    guard ["--balance", "--push-balance", "--watch"].contains(mode) else {
        print(usage())
        exit(2)
    }

    guard let apiKey = ProcessInfo.processInfo.environment["DEEPSEEK_API_KEY"], !apiKey.isEmpty else {
        fputs("错误：请先设置环境变量 DEEPSEEK_API_KEY\n", stderr)
        exit(1)
    }

    let client = DeepSeekBalanceClient(apiKey: apiKey)

    switch mode {
    case "--balance":
        do {
            let balance = try client.fetchBalance()
            print("CNY 余额: \(balance)")
        } catch {
            fputs("错误：\(error.localizedDescription)\n", stderr)
            exit(1)
        }
    case "--push-balance":
        do {
            let balance = try client.fetchBalance()
            try pushBalance(balance)
        } catch {
            fputs("错误：\(error.localizedDescription)\n", stderr)
            exit(1)
        }
    case "--watch":
        while true {
            do {
                let balance = try client.fetchBalance()
                try pushBalance(balance)
            } catch {
                fputs("错误：\(error.localizedDescription)（60s 后重试）\n", stderr)
            }
            Thread.sleep(forTimeInterval: 60)
        }
    default:
        print(usage())
        exit(2)
    }
}

main()
