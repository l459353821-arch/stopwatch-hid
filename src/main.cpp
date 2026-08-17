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
 *
 * 饼图触摸：复制(Cmd+C) / 删除(Backspace 按住连删) / 粘贴(Cmd+V)
 * 物理按键：BtnA 按住=右Option、BtnB 单击=回车、BtnB 长按=撤销(Cmd+Z)
 */
#include <Arduino.h>
#include <M5Unified.h>
#include <math.h>
#include <BleKeyboard.h>

// ===================== BLE 键盘（替代 USB 键盘，蓝牙配对后使用） =====================
BleKeyboard bleKeyboard("StopWatchHID", "M5Stack", 100);

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
constexpr uint32_t kBtnBHoldMs       = 700;  // B 键长按阈值

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

// ===================== 触摸状态 =====================
static bool     touchActive       = false;
static int32_t  touchSector       = -1;
static uint32_t touchStartMs      = 0;
static bool     singleActionFired = false;
static bool     blueLongPress     = false;
static uint32_t blueRepeatLastMs  = 0;
static int32_t  highlight         = -1;
static uint32_t highlightSetMs    = 0;

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
        tapBackspace();
      }
      if (blueLongPress && (now - blueRepeatLastMs) >= kBlueRepeatMs) {
        tapBackspace();
        blueRepeatLastMs = now;
      }
    } else {
      // 橙色 / 灰色：单击发一次
      if (!singleActionFired) {
        singleActionFired = true;
        if (sector == 0) sendHID(0x08, 'c');  // Cmd+C 复制（Mac；若需字面 Ctrl 键改为 0x01）
        else             sendHID(0x08, 'v');  // Cmd+V 粘贴
      }
    }
  } else {
    if (touchActive) {
      uint32_t held = now - touchStartMs;
      // 蓝色单击：抬手且未到长按阈值 → 发一次退格
      if (touchSector == 1 && !blueLongPress && held >= kTouchDebounceMs && held < kBlueHoldMs) {
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

  if (a && !aPrev) pressOption();
  if (!a && aPrev) releaseOption();
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

// ===================== 主流程 =====================
void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_spk = false;   // 无音频需求，关闭扬声器/编解码
  M5.begin(cfg);

  M5.Display.setRotation(0);
  M5.Display.setBrightness(120);

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
  handleTouch();
  delay(5);
}
