# StopWatchHID — M5Stack StopWatch USB 键盘 + 原生 USB 麦克风

> 在 M5Stack StopWatch（ESP32-S3）上实现：圆形三扇区触摸虚拟按键 + USB HID 键盘 + 原生 USB 音频麦克风。
> 一个 USB 口同时提供「麦克风 + 键盘」（USB 复合设备），插上即用，**不需要蓝牙配对**。

---

## ✨ 功能一览

| 模块 | 说明 |
|------|------|
| 🖥️ 圆形饼图触摸 | 三个扇形虚拟按键（复制 / 删除 / 粘贴），带高亮反馈 |
| ⌨️ USB HID 键盘 | A/B 物理按键，走 USB（非蓝牙） |
| 🎙️ 原生 USB 麦克风 | USB Audio Class（UAC1），Mac 输入设备里直接出现 TinyUSB UAC1 |
| 🔌 USB 复合设备 | 同一根 USB 线同时提供麦克风 + 键盘 |

---

## 🛠 硬件

[M5Stack StopWatch](https://docs.m5stack.com/en/core/StopWatch)：

- **SoC**：ESP32-S3R8（16MB Flash / 8MB PSRAM）
- **屏幕**：1.75" 圆形 AMOLED，466×466，CO5300 驱动（QSPI）
- **触摸**：CST820B（SYS_SDA=G47，SYS_SCL=G48，INT=G13）
- **按键**：KEYA（黄）= GPIO2，KEYB（蓝）= GPIO1
- **音频**：ES8311 编解码 + MEMS 麦克风
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

> ⚠️ 关于 Ctrl 还是 Cmd：固件里复制/粘贴默认用 **Cmd+C / Cmd+V**（Mac 习惯）。
> 如果你的目标平台是 Windows / 需要字面 Ctrl 键，改 src/main.cpp 里 handleTouch() 中
> sendHID(0x08, 'c') / sendHID(0x08, 'v') 的 0x08（Cmd）为 0x01（Ctrl）即可。

---

## 🔘 物理按键

| 按键 | 动作 | 功能 |
|------|------|------|
| A（黄，GPIO2） | 按住 | 右 Option（⌥）——微信输入法「按住说话」 |
| A（黄） | 松开 | 释放右 Option |
| B（蓝，GPIO1） | 单击 | 回车（Return） |
| B（蓝） | 长按 ≈700ms | 撤销 Cmd+Z |

---

## 🎙️ 麦克风

- 原生 USB Audio Class（UAC1），48kHz / 16bit / 单声道。
- Mac「系统设置 → 声音 → 输入」里选择 TinyUSB UAC1 即可收到硬件收音。
- **不需要** BlackHole、虚拟声卡或任何中转脚本。

---

## 📁 项目结构

    stopwatch-hid/
    ├── src/
    │   └── main.cpp                        # 固件主程序（全部逻辑）
    ├── boards/
    │   └── m5stack-stopwatch-usb-mic.json  # 自定义板级定义
    ├── platformio.ini                      # PlatformIO 工程配置
    ├── add_bluedroid_api.py                # 构建前置脚本
    ├── partitions_16mb_large_app.csv       # 16MB 分区表
    ├── CMakeLists.txt                      # ESP-IDF 构建入口（可选）
    └── README.md

---

## 🔨 编译

### 依赖

- [PlatformIO](https://platformio.org/)（建议 VS Code 插件，或命令行 pio）
- 平台：pioarduino/platform-espressif32 @ 55.03.311（Arduino-ESP32 core 3.3.11，**必须用这个**——它自带 TinyUSB 音频支持，标准 espressif32@6.x 没有）

### 步骤

    # 克隆/进入项目
    cd stopwatch-hid

    # 编译（默认环境已配置好，只需编译 usb-mic 环境）
    pio run -e m5stack-stopwatch-usb-mic

编译产物：.pio/build/m5stack-stopwatch-usb-mic/firmware.bin

> 📌 两个注意：
> 1. **项目路径不能含中文/空格**。ESP-IDF 的 CMake 在中文路径下会链接失败
>    （报 ld: error: /: read: Is a directory）。请把项目放到纯英文路径（如 /tmp/stopwatch-hid）。
> 2. prepare-bluedroid 环境是早期蓝牙键盘版本的遗留（当前固件已改用 USB HID，不再用蓝牙），
>    可忽略，不影响 m5stack-stopwatch-usb-mic 环境编译。

---

## 🚀 烧录

### 进入下载模式

1. 用 USB-C 线连接设备到电脑。
2. **按住电源键约 2 秒**，直到**绿灯亮起**，松开。
3. 此时电脑出现串口 /dev/cu.usbmodemXXXX。

### 烧录

    # 方式一：PlatformIO 自动烧录（自动找串口）
    pio run -e m5stack-stopwatch-usb-mic -t upload

    # 方式二：指定串口
    pio run -e m5stack-stopwatch-usb-mic -t upload --upload-port /dev/cu.usbmodem2101

### 启动

烧录完成后，**短按一下电源键**即可启动固件。
（软件复位命令在 USB-Serial/JTAG 下载模式下不可靠，物理按键最稳。）

---

## 📖 使用

1. 上电后屏幕显示三色饼图。
2. 用 USB-C 线连 Mac，系统会识别出两个 USB 设备：
   - **键盘**（USB HID）——无需配对
   - **麦克风** TinyUSB UAC1——在输入设备里选中它
3. 触摸饼图三个扇区：右上复制 / 左上删除（按住连删）/ 下方粘贴。
4. A 键按住说话（右 Option），B 键回车 / 长按撤销。

---

## 📄 许可

[MIT](./LICENSE)
