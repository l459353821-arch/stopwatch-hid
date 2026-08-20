/*
 * StopWatchHID — M5Stack StopWatch 三扇区饼图 BLE 键盘（无麦克风）
 *
 * 修改版：
 *   - 连接方式由 USB HID 改为 BLE 蓝牙键盘。
 *     蓝牙方案借鉴 voice-coding-badge 仓库：
 *       平台 espressif32 @ 6.12.0 + board esp32s3box
 *       + -DUSE_NIMBLE + h2zero/NimBLE-Arduino + t-vk/ESP32 BLE Keyboard
 *   - 完全移除原生 USB 麦克风（本固件不做任何音频传输）。
 *   - 保留：三扇区饼图触摸（复制/删除/粘贴）+ A/B 物理按键。
 *   - 新增：饼图虚拟按键触摸时震动马达反馈（M5IOE1 PYG9 PWM）。
 *   - 新增：电源键单击改为熄屏/亮屏切换（M5PM1，单击不复位）。
 *
 * 饼图触摸：复制(Cmd+C) / 删除(Backspace 按住连删) / 粘贴(Cmd+V)，命中即震动
 * 物理按键：BtnA 短按=Shift+回车（聊天换行）、长按=语音功能(右Option)；BtnB 单击=回车、长按=撤销(Cmd+Z)
 * 电源键：单击=熄屏/亮屏；双击=关机；长按(≈2s)=进入下载模式（均为 M5PM1 硬件行为）
 */
#include <Arduino.h>
#include <M5Unified.h>
#include <math.h>
#include <string.h>
#include <BleKeyboard.h>
#include <NimBLEDevice.h>
#include <M5PM1.h>
#include <utility/M5IOE1_Class.hpp>

// ===================== BLE 键盘 + DeepSeek 余额接收（借鉴 voice-coding-badge） =====================
static char gBalance[32] = {0};
static volatile bool gBalanceDirty = false;
static portMUX_TYPE gBalanceMux = portMUX_INITIALIZER_UNLOCKED;

class BalanceCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    if (v.size() >= sizeof(gBalance)) v.resize(sizeof(gBalance) - 1);
    portENTER_CRITICAL(&gBalanceMux);
    strncpy(gBalance, v.c_str(), sizeof(gBalance) - 1);
    gBalance[sizeof(gBalance) - 1] = '\0';
    portEXIT_CRITICAL(&gBalanceMux);
    gBalanceDirty = true;
    Serial.printf("[Balance] %s\n", gBalance);
  }
};
static BalanceCharCallbacks gBalanceCallbacks;

class DebugBleKeyboard : public BleKeyboard {
 public:
  DebugBleKeyboard(std::string name, std::string mfr, uint8_t battery)
      : BleKeyboard(name, mfr, battery) {}

 protected:
  void onStarted(BLEServer* pServer) override {
    // 自定义 GATT 服务：Mac 端余额助手把 DeepSeek 余额写进 0xFFF1
    NimBLEService* svc = pServer->createService(NimBLEUUID((uint16_t)0xFFF0));
    NimBLECharacteristic* ch = svc->createCharacteristic(
        NimBLEUUID((uint16_t)0xFFF1), NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    ch->setCallbacks(&gBalanceCallbacks);
    svc->start();
    Serial.println("[Balance] GATT 0xFFF0 ready (balance 0xFFF1)");
  }
  void onConnect(BLEServer* pServer) override {
    BleKeyboard::onConnect(pServer);
    // 保持广播，让 Mac 余额助手作为第二个 central 仍能连接（NimBLE 最多 3 连接）
    NimBLEDevice::startAdvertising();
  }
  void onDisconnect(BLEServer* pServer) override {
    BleKeyboard::onDisconnect(pServer);
    NimBLEDevice::startAdvertising();
  }
};

DebugBleKeyboard bleKeyboard("StopWatchHID", "M5Stack", 100);

// ===================== 饼图几何（动态，适配屏幕实际分辨率） =====================
static int32_t  kCx     = 233;
static int32_t  kCy     = 233;
static int32_t  kRadius = 165;
constexpr uint32_t kColorOrange = 0xE87722;
constexpr uint32_t kColorBlue   = 0x2E6DB4;
constexpr uint32_t kColorGray   = 0x6E6E6E;
constexpr uint32_t kColorWhite  = 0xFFFFFF;
constexpr uint32_t kColorBlack  = 0x000000;

constexpr uint32_t kTouchDebounceMs  = 40;   // 触摸去抖
constexpr uint32_t kBlueHoldMs       = 200;  // 蓝色长按阈值（超过才开始连删）
constexpr uint32_t kBlueRepeatMs     = 100;  // 蓝色长按连删间隔
constexpr uint32_t kHighlightFlashMs = 130;  // 触摸命中白闪时长
constexpr uint32_t kBtnAHoldMs  = 500;   // A 键长按（≥500ms 触发语音功能）
constexpr uint32_t kBtnBHoldMs  = 700;   // B 键长按阈值

constexpr uint32_t kBatteryRefreshMs = 5000;  // 电量刷新间隔（ms）
constexpr uint8_t  kLowBatteryLevel  = 20;    // 低电量阈值（%）

// 触觉反馈强度（0..255，M5.Power.setVibration 语义，同 codex-micro-stopwatch）
// 马达启动阈值约 56% 占空比，176 与物理按键手感一致：稳定、无明显丢震
constexpr uint8_t  kHapticIntensity   = 176;

// ===================== 震动马达 + 电源键（M5IOE1 / M5PM1，参考 codex-micro-stopwatch） =====================
// 震动马达在 M5IOE1 的 PYG9(IO9)，由 M5.Power.setVibration() 驱动（内部写 PWM1）。
// 马达供电来自 3V3_L3B 电源轨，对应 M5IOE1 G8(PYB_L3B_EN)（零基索引仅用于开电源轨）。
constexpr uint8_t  kScreenBrightness = 120;             // 亮屏亮度（同 setup 里初始值）

// 电源键单击：短按判定区间，避免误触与长按（进入下载模式）冲突
constexpr uint32_t kPwrClickMinMs = 40;    // 触摸去抖后的最小按下时长
constexpr uint32_t kPwrClickMaxMs = 1000;  // 超过则视为长按，不切换屏幕

// ===================== 物理按键（StopWatch：BtnA=GPIO2，BtnB=GPIO1，低电平按下） =====================
constexpr int     kBtnAPin   = 2;
constexpr int     kBtnBPin   = 1;
constexpr uint32_t kBtnDebounceMs = 25;

struct Btn {
  int pin;
  bool raw;
  uint32_t lastChangeMs;
};

static Btn btnA = { kBtnAPin, false, 0 };
static Btn btnB = { kBtnBPin, false, 0 };

static bool     optionHeld  = false;
static bool     btnBHeld    = false;
static uint32_t btnBPressMs = 0;
static bool     btnAHeld    = false;
static uint32_t btnAPressMs = 0;

// ===================== 触摸状态 =====================
static bool     touchActive       = false;
static int32_t  touchSector       = -1;
static uint32_t touchStartMs      = 0;
static bool     singleActionFired = false;
static bool     blueLongPress     = false;
static uint32_t blueRepeatLastMs  = 0;
static int32_t  highlight         = -1;
static uint32_t highlightSetMs    = 0;

// ===================== 电量显示（借鉴 voice-coding-badge） =====================
static uint32_t lastBatteryReadMs           = 0;
static int32_t  currentBatteryLevel         = -1;
static int8_t   currentChargingState        = -1;   // 1=充电 0=放电 -1=未知
static int32_t  lastDrawnBatteryLevel       = -100;
static int8_t   lastDrawnChargingState      = -100;
static int16_t  lastReportedBleBatteryLevel = -1;

// ===================== 震动马达 + 电源键（M5IOE1 / M5PM1） =====================
// 震动方案完全移植自 codex-micro-stopwatch：
//   1) 震动 = M5Unified 内置 M5.Power.setVibration(level)，level 0..255
//      （内部写 M5IOE1 PWM1 -> IO9/G9 马达，已含 EN 位与占空比换算）
//   2) 马达供电来自 3V3_L3B 电源轨，由 M5IOE1 G8(PYB_L3B_EN) 负载开关控制，
//      必须先经 setHighImpedance(推挽) + digitalWrite 置高，否则马达无电。
constexpr uint8_t kL3bEnIndex = 7;   // M5IOE1 G8（零基索引，同参考项目 kIoeSharedL3bEnable）
static bool     gIoeReady = false;

// 震动状态（非阻塞：参考项目 startHaptic/stopHaptic 模式）
static bool     gVibrateActive   = false;
static uint32_t gVibrateUntilMs  = 0;
static uint8_t  gVibrateStrength = 0;

// 电源键（M5PM1）
static M5PM1  gPm1;
static bool   gPm1Ready    = false;
static bool   gScreenOn    = true;
static int32_t gBrightness = kScreenBrightness;

// 打开/关闭 3V3_L3B 电源轨（G8/PYB_L3B_EN），同参考项目 setSharedRail()
static void setL3bRail(bool enabled) {
  auto& ioe1 = M5.getIOExpander(0);
  ioe1.setHighImpedance(kL3bEnIndex, false);   // 推挽输出（关键：否则 EN 浮空）
  ioe1.setDirection(kL3bEnIndex, true);
  ioe1.digitalWrite(kL3bEnIndex, enabled);
}

// ===================== 震动马达（M5.Power.setVibration，非阻塞） =====================
static void vibrateInit() {
  setL3bRail(true);                     // 开 L3B，马达才有供电
  M5.Power.setVibration(0);             // 初始关闭 PWM
  gIoeReady = true;
  Serial.println("[VIB] motor ready (M5.Power.setVibration + L3B rail ON)");
}

// 触发一次震动（参考项目 startHaptic：立即输出，由 updateVibrator() 定时关闭）
static void vibrate(uint16_t durationMs, uint8_t strength) {
  if (!gIoeReady) return;
  gVibrateStrength = strength;
  gVibrateUntilMs  = millis() + durationMs;
  gVibrateActive   = true;
  M5.Power.setVibration(strength);
}

static void updateVibrator() {
  if (!gVibrateActive) return;
  if ((int32_t)(millis() - gVibrateUntilMs) < 0) return;
  gVibrateActive = false;
  M5.Power.setVibration(0);
}

// ===================== 电源键：单击熄屏/亮屏（M5PM1） =====================
static void setScreenOn(bool on) {
  gScreenOn = on;
  M5.Display.setBrightness(on ? (uint8_t)gBrightness : 0);
}

static void powerInit() {
  if (gPm1.begin(&M5.In_I2C, M5PM1_DEFAULT_ADDR) != M5PM1_OK) {
    Serial.println("[PWR] M5PM1 init failed");
    return;
  }
  gPm1Ready = true;
  gPm1.setI2cSleepTime(0);  // 关闭 I2C 自动睡眠，保证电源键轮询稳定
  // 单击电源键原本会硬件复位，这里禁用，改为由固件做熄屏/亮屏切换；
  // 双击关机、长按(≈2s)下载模式为 M5PM1 硬件行为，保持不变。
  gPm1.setSingleResetDisable(true);
  Serial.println("[PWR] single-click -> screen toggle (hardware reset disabled)");
}

static void handlePowerButton() {
  if (!gPm1Ready) return;
  bool pressed = false;
  if (gPm1.btnGetState(&pressed) != M5PM1_OK) return;

  static bool prevPressed = false;
  static uint32_t pressMs = 0;
  uint32_t now = millis();

  if (pressed && !prevPressed) {
    pressMs = now;
  } else if (!pressed && prevPressed) {
    uint32_t held = now - pressMs;
    // 短按 → 切换熄屏/亮屏；长按(≥1s)忽略，留给下载模式
    if (held >= kPwrClickMinMs && held < kPwrClickMaxMs) {
      setScreenOn(!gScreenOn);
      vibrate(20, kHapticIntensity);  // 电源键：短反馈
      Serial.printf("[PWR] screen %s\n", gScreenOn ? "ON" : "OFF");
    }
  }
  prevPressed = pressed;
}

// ===================== HID 发送（BLE 键盘） =====================
static void pressModifiers(uint8_t m) {
  if (m & 0x01) bleKeyboard.press(KEY_LEFT_CTRL);
  if (m & 0x02) bleKeyboard.press(KEY_LEFT_SHIFT);
  if (m & 0x04) bleKeyboard.press(KEY_LEFT_ALT);
  if (m & 0x08) bleKeyboard.press(KEY_LEFT_GUI);
  if (m & 0x10) bleKeyboard.press(KEY_RIGHT_CTRL);
  if (m & 0x20) bleKeyboard.press(KEY_RIGHT_SHIFT);
  if (m & 0x40) bleKeyboard.press(KEY_RIGHT_ALT);
  if (m & 0x80) bleKeyboard.press(KEY_RIGHT_GUI);
}
static void releaseModifiers(uint8_t m) {
  if (m & 0x01) bleKeyboard.release(KEY_LEFT_CTRL);
  if (m & 0x02) bleKeyboard.release(KEY_LEFT_SHIFT);
  if (m & 0x04) bleKeyboard.release(KEY_LEFT_ALT);
  if (m & 0x08) bleKeyboard.release(KEY_LEFT_GUI);
  if (m & 0x10) bleKeyboard.release(KEY_RIGHT_CTRL);
  if (m & 0x20) bleKeyboard.release(KEY_RIGHT_SHIFT);
  if (m & 0x40) bleKeyboard.release(KEY_RIGHT_ALT);
  if (m & 0x80) bleKeyboard.release(KEY_RIGHT_GUI);
}
static void sendHID(uint8_t modifier, uint8_t keycode) {
  if (!bleKeyboard.isConnected()) return;
  pressModifiers(modifier);
  if (keycode != 0) bleKeyboard.press(keycode);
  delay(60);
  if (keycode != 0) bleKeyboard.release(keycode);
  releaseModifiers(modifier);
  bleKeyboard.releaseAll();
}
static void tapBackspace() {
  if (!bleKeyboard.isConnected()) return;
  bleKeyboard.press(KEY_BACKSPACE);
  delay(20);
  bleKeyboard.release(KEY_BACKSPACE);
  bleKeyboard.releaseAll();
}
static void pressOption() {
  if (optionHeld) return;
  if (!bleKeyboard.isConnected()) return;
  bleKeyboard.press(KEY_RIGHT_ALT);
  optionHeld = true;
}
static void releaseOption() {
  if (!optionHeld) return;
  bleKeyboard.release(KEY_RIGHT_ALT);
  bleKeyboard.releaseAll();
  optionHeld = false;
}

// ===================== 饼图绘制 =====================
static void drawSector(float a0, float a1, uint32_t color) {
  M5.Display.fillArc(kCx, kCy, 0, kRadius, a0, a1, color);
}
static void drawSeparatorLine(float screenAngle) {
  float rad = screenAngle * PI / 180.0f;
  int32_t x1 = kCx + (int32_t)lroundf(kRadius * cosf(rad));
  int32_t y1 = kCy + (int32_t)lroundf(kRadius * sinf(rad));
  float dx = (float)(x1 - kCx), dy = (float)(y1 - kCy);
  float len = sqrtf(dx * dx + dy * dy);
  float px = -dy / len, py = dx / len;
  M5.Display.drawLine(kCx, kCy, x1, y1, kColorWhite);
  M5.Display.drawLine(kCx + (int32_t)lroundf(px), kCy + (int32_t)lroundf(py),
                      x1 + (int32_t)lroundf(px), y1 + (int32_t)lroundf(py), kColorWhite);
}
static void drawPie(int32_t highlightSector) {
  // 高亮时对应扇区变为纯白（130ms 后由 clearHighlight 恢复；蓝色长按期间保持）
  drawSector(270, 30,  highlightSector == 0 ? kColorWhite : kColorOrange);
  drawSector(150, 270, highlightSector == 1 ? kColorWhite : kColorBlue);
  drawSector(30, 150,  highlightSector == 2 ? kColorWhite : kColorGray);
  M5.Display.drawCircle(kCx, kCy, kRadius, kColorWhite);
  drawSeparatorLine(270);
  drawSeparatorLine(150);
  drawSeparatorLine(30);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(kColorWhite);
  M5.Display.setTextSize(1);
  M5.Display.drawString("StopWatch", kCx, kCy);
}
static void clearHighlight() {
  if (highlight != -1) { highlight = -1; drawPie(-1); }
}

// ===================== 触摸判定（归一化 → 屏幕坐标） =====================
static bool readTouchScreen(int16_t* outX, int16_t* outY) {
  if (M5.Display.touch() == nullptr) return false;
  // 用 M5.Touch 的缓存结果（由 M5.update() 更新）。
  auto t = M5.Touch.getDetail();
  if (!t.isPressed()) return false;
  // CST820B 触摸坐标本身就是屏幕坐标系（0..466，与 466x466 屏幕一致），直接使用。
  *outX = t.x;
  *outY = t.y;
  return true;
}

static int32_t sectorAt(int16_t tx, int16_t ty) {
  float dx = (float)(tx - kCx), dy = (float)(kCy - ty);
  float r = sqrtf(dx * dx + dy * dy);
  if (r > kRadius) return -1;
  float angle = atan2f(dy, dx) * 180.0f / PI;
  if (angle < 0.0f) angle += 360.0f;
  if (angle >= 330.0f || angle < 90.0f)      return 0;
  else if (angle >= 90.0f && angle < 210.0f) return 1;
  else                                       return 2;
}

static void handleTouch() {
  int16_t tx = 0, ty = 0;
  bool touching = readTouchScreen(&tx, &ty);
  uint32_t now = millis();

  // 熄屏时忽略触摸，避免误触发复制/删除/粘贴（亮屏后自动恢复）
  if (!gScreenOn) {
    if (touchActive) {
      touchActive = false;
      touchSector = -1;
      blueLongPress = false;
      singleActionFired = false;
      clearHighlight();
    }
    return;
  }

  if (touching) {
    if (!touchActive) {
      touchActive = true;
      touchSector = -1;
      touchStartMs = now;
      singleActionFired = false;
      blueLongPress = false;
      blueRepeatLastMs = 0;
      return;
    }
    if (now - touchStartMs < kTouchDebounceMs) return;

    int32_t sector = sectorAt(tx, ty);
    if (sector < 0) {
      touchSector = -1;
      if (!blueLongPress) clearHighlight();
      return;
    }

    // 首次进入该扇区：白闪反馈
    if (touchSector != sector) {
      touchSector = sector;
      highlight = sector;
      highlightSetMs = now;
      drawPie(sector);
    }

    if (sector == 1) {
      // 蓝色：长按(>200ms)才开始连删；单击由抬手时发一次退格
      if (!blueLongPress && (now - touchStartMs) >= kBlueHoldMs) {
        blueLongPress = true;
        blueRepeatLastMs = now;
        highlight = 1;   // 长按期间高亮保持
        drawPie(1);
        vibrate(20, kHapticIntensity);  // 蓝色长按起始
        tapBackspace();
      }
      if (blueLongPress && (now - blueRepeatLastMs) >= kBlueRepeatMs) {
        vibrate(12, kHapticIntensity);  // 连删节拍
        tapBackspace();
        blueRepeatLastMs = now;
      }
    } else {
      // 橙色 / 灰色：单击发一次
      if (!singleActionFired) {
        singleActionFired = true;
        vibrate(25, kHapticIntensity);  // 橙/灰单击反馈
        if (sector == 0) sendHID(0x08, 'c');  // Cmd+C 复制（Mac；若需字面 Ctrl 键改为 0x01）
        else             sendHID(0x08, 'v');  // Cmd+V 粘贴
      }
    }
  } else {
    if (touchActive) {
      uint32_t held = now - touchStartMs;
      // 蓝色单击：抬手且未到长按阈值 → 发一次退格
      if (touchSector == 1 && !blueLongPress && held >= kTouchDebounceMs && held < kBlueHoldMs) {
        vibrate(20, kHapticIntensity);  // 蓝色单击抬手
        tapBackspace();
      }
      touchActive = false;
      touchSector = -1;
      blueLongPress = false;
      singleActionFired = false;
      clearHighlight();
    }
  }

  // 白闪 130ms 后恢复（蓝色长按期间不恢复）
  if (highlight >= 0 && !blueLongPress && (now - highlightSetMs) >= kHighlightFlashMs) {
    clearHighlight();
  }
}

// ===================== 物理按键（直接 GPIO + 去抖 + 长按） =====================
static bool readBtn(Btn& b) {
  bool raw = (digitalRead(b.pin) == LOW);
  if (raw != b.raw) {
    uint32_t now = millis();
    if (now - b.lastChangeMs >= kBtnDebounceMs) {
      b.raw = raw;
      b.lastChangeMs = now;
    }
  }
  return b.raw;
}

static void handleButtons() {
  bool a = readBtn(btnA);
  bool b = readBtn(btnB);
  uint32_t now = millis();
  static bool aPrev = false, bPrev = false;

  // A 键（黄色）：短按=Shift+回车（聊天换行，不发送）；长按≥500ms=语音功能（按住右Option）
  if (a && !aPrev) { btnAPressMs = now; btnAHeld = false; }
  if (a && !btnAHeld && (now - btnAPressMs) >= kBtnAHoldMs) {
    btnAHeld = true;
    pressOption();   // 长按触发语音功能（按住）
  }
  if (!a && aPrev) {
    if (!btnAHeld && (now - btnAPressMs) >= kBtnDebounceMs) {
      sendHID(0x02, KEY_RETURN);   // 左Shift + 回车
    }
    releaseOption();
    btnAHeld = false;
  }
  aPrev = a;

  if (b && !bPrev) { btnBPressMs = now; btnBHeld = false; }
  if (b && !btnBHeld && (now - btnBPressMs) >= kBtnBHoldMs) {
    btnBHeld = true;
    sendHID(0x08, 'z');
  }
  if (!b && bPrev) {
    if (!btnBHeld && (now - btnBPressMs) >= kBtnDebounceMs) {
      sendHID(0x00, KEY_RETURN);
    }
    btnBHeld = false;
  }
  bPrev = b;
}

// ===================== 电量显示（借鉴 voice-coding-badge） =====================
static void updateBatterySnapshot(bool force) {
  uint32_t now = millis();
  if (!force && (now - lastBatteryReadMs) < kBatteryRefreshMs) return;
  lastBatteryReadMs = now;

  currentBatteryLevel = M5.Power.getBatteryLevel();
  const auto charging = M5.Power.isCharging();
  currentChargingState = charging == m5::Power_Class::is_charging_t::is_charging ? 1
                       : charging == m5::Power_Class::is_charging_t::is_discharging ? 0 : -1;

  // 通过 BLE Battery Service (0x2A19) 上报真实电量，让 Mac 蓝牙菜单显示真实百分比
  if (currentBatteryLevel >= 0) {
    uint8_t bleLevel = currentBatteryLevel > 100 ? 100 : (uint8_t)currentBatteryLevel;
    if (bleLevel != lastReportedBleBatteryLevel) {
      bleKeyboard.setBatteryLevel(bleLevel);
      lastReportedBleBatteryLevel = bleLevel;
    }
  }
}

static void drawBatteryIndicator(bool force) {
  updateBatterySnapshot(force);
  if (!force && currentBatteryLevel == lastDrawnBatteryLevel &&
      currentChargingState == lastDrawnChargingState) {
    return;
  }
  lastDrawnBatteryLevel  = currentBatteryLevel;
  lastDrawnChargingState = currentChargingState;

  const int32_t iconW   = 40;
  const int32_t iconH   = 18;
  const int32_t y       = M5.Display.height() - 36;   // 底部黑色留白区（饼图下方）
  const int32_t iconX   = 130;   // 电池靠左，右侧留给 DeepSeek 余额
  const int32_t iconY   = y + 2;
  const int32_t fillMax = iconW - 6;
  const int32_t pctX    = iconX + iconW + 8;

  // 只清电池区域，不碰饼图
  M5.Display.fillRect(iconX - 4, y - 2, 92, 32, kColorBlack);

  // 电池外框 + 正极凸起
  M5.Display.drawRoundRect(iconX, iconY, iconW, iconH, 3, kColorWhite);
  M5.Display.fillRect(iconX + iconW, iconY + 6, 3, 8, kColorWhite);

  uint32_t fg = kColorWhite;
  if (currentChargingState == 1) {
    fg = 0x00FF00;   // 充电绿
  } else if (currentBatteryLevel >= 0 && currentBatteryLevel <= kLowBatteryLevel) {
    fg = 0xFF0000;   // 低电量红
  }

  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(1);
  if (currentBatteryLevel >= 0) {
    int32_t lvl   = currentBatteryLevel > 100 ? 100 : currentBatteryLevel;
    int32_t fillW = (fillMax * lvl) / 100;
    if (fillW > 0) {
      M5.Display.fillRect(iconX + 3, iconY + 3, fillW, iconH - 6, fg);
    }
    M5.Display.setTextColor(fg, kColorBlack);
    M5.Display.setCursor(pctX, y + 6);
    M5.Display.printf("%d%%", (int)currentBatteryLevel);
  } else {
    M5.Display.setTextColor(kColorGray, kColorBlack);
    M5.Display.setCursor(pctX, y + 6);
    M5.Display.print("--");
  }

  // 充电闪电标记
  if (currentChargingState == 1) {
    M5.Display.drawLine(iconX + 22, iconY + 3,  iconX + 15, iconY + 11, 0xFFFF00);
    M5.Display.drawLine(iconX + 15, iconY + 11, iconX + 23, iconY + 11, 0xFFFF00);
    M5.Display.drawLine(iconX + 23, iconY + 11, iconX + 16, iconY + 16, 0xFFFF00);
  }
}

// ===================== DeepSeek 余额显示（Mac 通过 BLE 写入，风格同电量） =====================
static void drawBalanceIndicator() {
  const int32_t y    = M5.Display.height() - 36;
  const int32_t balX = 236;

  // 只清余额区域，不碰电量/饼图
  M5.Display.fillRect(balX - 4, y - 2, 128, 32, kColorBlack);

  char buf[32];
  portENTER_CRITICAL(&gBalanceMux);
  strncpy(buf, gBalance, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  portEXIT_CRITICAL(&gBalanceMux);

  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(1);
  if (buf[0] != '\0') {
    M5.Display.setTextColor(0x00A3FF, kColorBlack);   // 蓝色：DeepSeek 余额
    M5.Display.setCursor(balX, y + 6);
    M5.Display.print("DS ");
    M5.Display.print(buf);
  } else {
    M5.Display.setTextColor(kColorGray, kColorBlack);
    M5.Display.setCursor(balX, y + 6);
    M5.Display.print("DS --");
  }
}

// ===================== 主流程 =====================
void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_spk = false;   // 无音频需求，关闭扬声器/编解码
  M5.begin(cfg);

  M5.Display.setRotation(0);
  M5.Display.setBrightness(kScreenBrightness);
  gBrightness = kScreenBrightness;

  // 震动马达 + 电源键初始化（M5IOE1 / M5PM1，需在 M5.begin 之后，I2C 总线就绪）
  vibrateInit();
  powerInit();

  // 开机自检震动：两次（各 150ms），方便体感确认马达正常
  vibrate(150, kHapticIntensity);
  delay(300);
  vibrate(150, kHapticIntensity);
  delay(150);

  kCx = M5.Display.width() / 2;
  kCy = M5.Display.height() / 2;
  kRadius = (int32_t)((float)min(M5.Display.width(), M5.Display.height()) * 165.0f / 466.0f);
  if (kRadius < 60) kRadius = 165;

  pinMode(kBtnAPin, INPUT_PULLUP);
  pinMode(kBtnBPin, INPUT_PULLUP);

  M5.Display.fillScreen(kColorBlack);
  drawPie(-1);

  // BLE 键盘：开始广播，Mac「蓝牙」里配对 "StopWatchHID"
  bleKeyboard.begin();

  // 初始电量显示（同时上报真实电量到 BLE Battery Service）
  drawBatteryIndicator(true);
  drawBalanceIndicator();

  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(0x00FF00);
  M5.Display.setCursor(2, 2);
  M5.Display.print("BLE KB OK");

  Serial.begin(115200);
  Serial.println("[StopWatchHID] BLE keyboard advertising as StopWatchHID");
}

void loop() {
  M5.update();

  static bool lastConnected = false;
  bool connected = bleKeyboard.isConnected();
  if (connected != lastConnected) {
    lastConnected = connected;
    Serial.println(connected ? "[BLE] connected" : "[BLE] disconnected");
  }

  handleButtons();
  handlePowerButton();
  handleTouch();
  updateVibrator();
  drawBatteryIndicator(false);   // 每 5s 刷新电量显示 + BLE 上报
  if (gBalanceDirty) {
    gBalanceDirty = false;
    drawBalanceIndicator();      // 收到新余额时刷新
  }
  delay(5);
}
