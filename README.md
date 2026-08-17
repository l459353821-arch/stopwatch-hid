# StopWatchHID — M5Stack StopWatch 蓝牙键盘（BLE，无麦克风）

> 在 M5Stack StopWatch（ESP32-S3）上实现：圆形三扇区触摸虚拟按键 + 蓝牙 BLE 键盘。
> 通过**蓝牙**与电脑连接，配对即用，无需 USB 线；本固件**不包含任何麦克风/音频传输**。

---

## ✨ 功能一览

| 模块 | 说明 |
|------|------|
| 🖥️ 圆形饼图触摸 | 三个扇形虚拟按键（复制 / 删除 / 粘贴），带高亮反馈 |
| ⌨️ BLE 键盘 | 饼图 + A/B 物理按键，走蓝牙 BLE（非 USB） |
| 🔗 蓝牙连接 | 设备名 StopWatchHID，Mac「蓝牙」里配对后即用 |

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

> ⚠️ 复制/粘贴默认用 **Cmd+C / Cmd+V**（Mac 习惯）。若目标平台是 Windows / 需要字面 Ctrl 键，
> 改 src/main.cpp 里 handleTouch() 中 sendHID(0x08, 'c') / sendHID(0x08, 'v') 的 0x08（Cmd）为 0x01（Ctrl）即可。

---

## 🔘 物理按键

| 按键 | 动作 | 功能 |
|------|------|------|
| A（黄，GPIO2） | 按住 | 右 Option（⌥）——微信输入法「按住说话」 |
| A（黄） | 松开 | 释放右 Option |
| B（蓝，GPIO1） | 单击 | 回车（Return） |
| B（蓝） | 长按 ≈700ms | 撤销 Cmd+Z |

---

## 🔧 蓝牙方案

借鉴 [voice-coding-badge](https://github.com/Jiaranbb/voice-coding-badge) 的 BLE 连接方案：

- 平台 espressif32 @ 6.12.0 + board = esp32s3box
- -DUSE_NIMBLE + h2zero/NimBLE-Arduino @ 1.4.3 + t-vk/ESP32 BLE Keyboard
- BLE 设备名 StopWatchHID（见 src/main.cpp 中 BleKeyboard bleKeyboard("StopWatchHID", "M5Stack", 100)）

---

## 📁 项目结构

    stopwatch-hid/
    ├── src/main.cpp                        # 固件主程序（全部逻辑）
    ├── platformio.ini                      # PlatformIO 工程配置
    ├── partitions_16mb_large_app.csv       # 16MB 分区表
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
   - 触摸饼图三个扇区：右上复制 / 左上删除（按住连删）/ 下方粘贴。
   - A 键按住 = 右 Option（按住说话），B 键单击 = 回车 / 长按 = 撤销。

---

## 📄 许可

[MIT](./LICENSE)
