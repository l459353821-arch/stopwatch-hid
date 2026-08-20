# StopWatchHID — M5Stack StopWatch 蓝牙键盘（BLE，无麦克风）

> 在 M5Stack StopWatch（ESP32-S3）上实现：圆形三扇区触摸虚拟按键 + 蓝牙 BLE 键盘 + 触控震动反馈 + 电源键熄屏/亮屏。
> 通过**蓝牙**与电脑连接，配对即用，无需 USB 线；本固件**不包含任何麦克风/音频传输**。

---

## ✨ 功能一览

| 模块 | 说明 |
|------|------|
| 🖥️ 圆形饼图触摸 | 三个扇形虚拟按键（复制 / 删除 / 粘贴），带高亮反馈 + **震动反馈** |
| 🌀 震动反馈 | 触控饼图虚拟按键命中动作时，内置马达短震（M5IOE1 PYG9 PWM） |
| 💡 电源键=熄屏/亮屏 | 电源键**单击**切换熄屏/亮屏；双击=关机；长按(≈2s)=下载模式 |
| ⌨️ BLE 键盘 | 饼图 + A/B 物理按键，走蓝牙 BLE（非 USB） |
| 🔗 蓝牙连接 | 设备名 StopWatchHID，Mac「蓝牙」里配对后即用 |
| 🔋 电量显示 | 底部电池图标 + 百分比，每 5s 刷新；充电绿 / 低电红，并上报 Mac 蓝牙菜单 |
| 🧮 DeepSeek 余额 | 电量旁显示 DeepSeek API 余额（Mac 抓取后通过 BLE 推送） |

---

## 🛠 硬件

[M5Stack StopWatch](https://docs.m5stack.com/en/core/StopWatch)：

- **SoC**：ESP32-S3R8（16MB Flash / 8MB PSRAM）
- **屏幕**：1.75" 圆形 AMOLED，466×466，CO5300 驱动（QSPI）
- **触摸**：CST820B（SYS_SDA=G47，SYS_SCL=G48，INT=G13）
- **按键**：KEYA（黄）= GPIO2，KEYB（蓝）= GPIO1
- **电源**：M5PM1 多级电源管理；**扩展 IO**：M5IOE1

---

## 🖥️ 屏幕饼图虚拟按键

饼图圆心 (233,233)，半径 165px，外圈白圆，三条白线把圆三等分。

| 扇区 | 颜色 | 单击 | 长按 | 发送按键 |
|------|------|------|------|----------|
| 右上 | 🟠 橙 #E87722 | 复制 | — | Cmd+C |
| 左上 | 🔵 蓝 #2E6DB4 | 删一个字符 | 按住 >200ms 后每 100ms 连删，直到抬手 | Backspace |
| 下方 | ⚪ 灰 #6E6E6E | 粘贴 | — | Cmd+V |

**触摸交互参数**（见 src/main.cpp）：

- 触摸去抖：40ms
- 每次触摸只触发一次动作（蓝色长按除外）
- 触摸命中时对应扇区闪白约 130ms 后恢复
- 蓝色长按期间高亮持续显示，直到手指抬起
- **震动反馈**：复制/粘贴/退格动作触发时内置马达短震（非阻塞）
  - 复制/粘贴：25ms；蓝色单击退格 / 长按首个退格：20ms；连删节拍（每 100ms）：12ms
  - 强度统一 176（0..255，马达启动阈值约 56% 占空比，实测 176 稳定无丢震）
  - 驱动方式：M5Unified 内置 `M5.Power.setVibration(level)`（M5IOE1 PWM1 → IO9/PYG9）；
    **马达供电来自 3V3_L3B 电源轨**，初始化时先把 M5IOE1 G8（PYB_L3B_EN）置高打开该轨，否则马达无电
  - 方案参考 [codex-micro-stopwatch](https://github.com/digitsisyph/codex-micro-stopwatch)（MIT）

> ⚠️ 复制/粘贴默认用 **Cmd+C / Cmd+V**（Mac 习惯）。若目标平台是 Windows / 需要字面 Ctrl 键，
> 改 src/main.cpp 里 handleTouch() 中 sendHID(0x08, 'c') / sendHID(0x08, 'v') 的 0x08（Cmd）为 0x01（Ctrl）即可。

---

## 🔘 物理按键

| 按键 | 动作 | 功能 |
|------|------|------|
| A（黄，GPIO2） | 短按 | Shift+回车 —— 聊天窗口换行（不发送） |
| A（黄） | 长按 ≈500ms | 语音功能（按住右 Option ⌥，松开释放） |
| B（蓝，GPIO1） | 单击 | 回车（Return） |
| B（蓝） | 长按 ≈700ms | 撤销 Cmd+Z |

## 💡 电源键逻辑（M5PM1）

| 操作 | 行为 |
|------|------|
| **单击** | **熄屏 / 亮屏 切换**（短按 40ms~1s 判定为单击） |
| 双击 | 关机（M5PM1 硬件行为，保留不变） |
| 长按 ≈2s | 进入下载模式（M5PM1 硬件行为，保留不变，用于烧录） |

> 固件启动时通过 M5PM1 调用 `setSingleResetDisable(true)`，把出厂默认的「单击=硬件复位」改为软件处理：单击只切换 `M5.Display.setBrightness(0/120)`，不影响 BLE 连接与电池上报。熄屏期间触摸被忽略，避免误触发复制/删除/粘贴。

---

## 🔧 蓝牙方案

借鉴 [voice-coding-badge](https://github.com/Jiaranbb/voice-coding-badge) 的 BLE 连接方案：

- 平台 espressif32 @ 6.12.0 + board = esp32s3box
- -DUSE_NIMBLE + h2zero/NimBLE-Arduino @ 1.4.3 + t-vk/ESP32 BLE Keyboard
- BLE 设备名 StopWatchHID（见 src/main.cpp 中 BleKeyboard bleKeyboard("StopWatchHID", "M5Stack", 100)）

---

## 🧮 DeepSeek 余额显示（Mac 推送）

手表没有 Wi-Fi，DeepSeek 余额由 Mac 抓取后通过 BLE 自定义特征推送到手表，显示在电量旁边（风格同电量）。

- 固件侧：BLE 服务 0xFFF0 / 特征 0xFFF1，收到余额字符串后显示为 `DS 12.34`。
- Mac 侧：`balance-client/`（Swift）负责 HTTP 抓取 + CoreBluetooth 写入。

### 构建与使用

    cd balance-client
    swift build

    export DEEPSEEK_API_KEY=sk-xxxx

    # 抓取并打印余额
    .build/debug/ds-balance --balance

    # 抓取后写入手表
    .build/debug/ds-balance --push-balance

    # 每 60 秒抓取并持续推送
    .build/debug/ds-balance --watch

> 余额接口为 `GET https://api.deepseek.com/user/balance`（Bearer 认证），取 `balance_infos` 里 CNY 的 `total_balance`。
> 在受限沙箱环境里构建若报 sandbox 错误，用 `swift build --disable-sandbox` 即可。

### 定时自动推送（launchd，可选）

想让 Mac 每 5 分钟自动把余额推送到手表（开机自启、不用开终端）：

    bash balance-client/install-launchd.sh <DEEPSEEK_API_KEY>

- 任务名：com.stopwatch.ds-balance，每 300 秒执行一次 `ds-balance --push-balance`
- 日志：~/Library/Logs/ds-balance.out.log / ds-balance.err.log
- 卸载：

      launchctl bootout gui/$(id -u)/com.stopwatch.ds-balance
      rm ~/Library/LaunchAgents/com.stopwatch.ds-balance.plist

---

## 📁 项目结构

    stopwatch-hid/
    ├── src/main.cpp                        # 固件主程序（全部逻辑）
    ├── platformio.ini                      # PlatformIO 工程配置
    ├── partitions_16mb_large_app.csv       # 16MB 分区表
    ├── balance-client/                     # Mac 端 DeepSeek 余额抓取 + BLE 推送（Swift）
    └── README.md

---

## 🔨 编译

### 依赖

- [PlatformIO](https://platformio.org/)（VS Code 插件或命令行 pio）
- 平台：espressif32 @ 6.12.0（Arduino-ESP32 core 3.x）

### 步骤

    # 进入项目
    cd stopwatch-hid

    # 编译
    pio run -e m5stack-stopwatch

编译产物：.pio/build/m5stack-stopwatch/firmware.bin

> 📌 项目路径请尽量使用纯英文路径，避免个别工具链在中文/空格路径下链接失败。

---

## 🚀 烧录

### 进入下载模式

1. 用 USB-C 线连接设备到电脑。
2. **按住电源键约 2 秒**，直到**绿灯亮起**，松开。
3. 此时电脑出现串口 /dev/cu.usbmodemXXXX。

### 烧录

    # PlatformIO 自动烧录（自动找串口）
    pio run -e m5stack-stopwatch -t upload

    # 或指定串口
    pio run -e m5stack-stopwatch -t upload --upload-port /dev/cu.usbmodemXXXX

### 启动

烧录完成后，**短按一下电源键**即可启动固件。
（软件复位命令在 USB-Serial/JTAG 下载模式下不可靠，物理按键最稳。）

---

## 📖 使用

1. 上电后屏幕显示三色饼图。
2. 打开 Mac「系统设置 → 蓝牙」，找到并连接 **StopWatchHID**。
3. 配对成功后：
   - 触摸饼图三个扇区：右上复制 / 左上删除（按住连删）/ 下方粘贴，每个动作都会**震动反馈**。
   - A 键短按 = Shift+回车（聊天换行），A 键长按 ≈500ms = 语音（右 Option）；B 键单击 = 回车 / 长按 = 撤销。
   - **电源键单击 = 熄屏/亮屏**；双击 = 关机；长按 ≈2s 进入下载模式。
4. 屏幕底部显示电量（电池图标 + 百分比）；充电时变绿并带闪电，电量 ≤20% 时变红。
   真实电量也会通过 BLE Battery Service 上报，Mac「蓝牙」菜单里能看到百分比。
5. DeepSeek 余额显示在电量旁（默认 `DS --`）；Mac 上运行 balance-client 推送后即显示余额。

---

## 📄 许可

[MIT](./LICENSE)
