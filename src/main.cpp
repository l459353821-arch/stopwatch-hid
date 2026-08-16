/*
 * StopWatchHID — M5Stack StopWatch 三扇区饼图 USB 键盘 + 原生 USB 麦克风
 *
 * 最终版（键盘改用 USB HID，与麦克风走同一根 USB 线，不再依赖蓝牙配对）：
 *   - 三扇区饼图触摸：复制(Cmd+C) / 删除(Backspace 按住连删) / 粘贴(Cmd+V)
 *   - USB HID 键盘：BtnA 按住=右Option、BtnB 单击=回车、BtnB 长按=撤销(Cmd+Z)
 *   - 原生 USB 麦克风：USBAudioCard(UAC1) + ES8311，Mac 输入里直接显示设备
 *   - USB 复合设备：同一个 USB 口同时提供「麦克风 + 键盘」
 *
 * 物理按键直接读 GPIO（GPIO2=A、GPIO1=B，低电平按下），触摸直接读 M5GFX 并归一化。
 */
#include <Arduino.h>
#include <M5Unified.h>
#include <math.h>
#include <USB.h>
#include <USBAudioCard.h>
#include <USBHIDKeyboard.h>
#include <tusb.h>
#include <atomic>

// ===================== USB 键盘（替代蓝牙键盘） =====================
USBHIDKeyboard usbKeyboard;

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

// ===================== USB 麦克风 =====================
static USBAudioCard usbAudio(48000, UAC_BPS_16, UAC_SPK_NONE, UAC_MIC_MONO);
constexpr uint32_t kMicRate         = 48000;
constexpr size_t   kMicBlockSamples = 480;   // 10ms
constexpr size_t   kMicBlockBytes   = kMicBlockSamples * 2;

static int16_t     micBuf[2][kMicBlockSamples];
static QueueHandle_t micQueue = nullptr;
static TaskHandle_t  micCaptureTask = nullptr;
static TaskHandle_t  micFeedTask = nullptr;
static std::atomic<bool> micStreaming{false};

struct MicBlock { int16_t s[kMicBlockSamples]; };

// ===================== 触摸状态 =====================
static bool     touchActive       = false;
static int32_t  touchSector       = -1;
static uint32_t touchStartMs      = 0;
static bool     singleActionFired = false;
static bool     blueLongPress     = false;
static uint32_t blueRepeatLastMs  = 0;
static int32_t  highlight         = -1;
static uint32_t highlightSetMs    = 0;

// ===================== HID 发送 =====================
static void pressModifiers(uint8_t m) {
  if (m & 0x01) usbKeyboard.press(KEY_LEFT_CTRL);
  if (m & 0x02) usbKeyboard.press(KEY_LEFT_SHIFT);
  if (m & 0x04) usbKeyboard.press(KEY_LEFT_ALT);
  if (m & 0x08) usbKeyboard.press(KEY_LEFT_GUI);
  if (m & 0x10) usbKeyboard.press(KEY_RIGHT_CTRL);
  if (m & 0x20) usbKeyboard.press(KEY_RIGHT_SHIFT);
  if (m & 0x40) usbKeyboard.press(KEY_RIGHT_ALT);
  if (m & 0x80) usbKeyboard.press(KEY_RIGHT_GUI);
}
static void releaseModifiers(uint8_t m) {
  if (m & 0x01) usbKeyboard.release(KEY_LEFT_CTRL);
  if (m & 0x02) usbKeyboard.release(KEY_LEFT_SHIFT);
  if (m & 0x04) usbKeyboard.release(KEY_LEFT_ALT);
  if (m & 0x08) usbKeyboard.release(KEY_LEFT_GUI);
  if (m & 0x10) usbKeyboard.release(KEY_RIGHT_CTRL);
  if (m & 0x20) usbKeyboard.release(KEY_RIGHT_SHIFT);
  if (m & 0x40) usbKeyboard.release(KEY_RIGHT_ALT);
  if (m & 0x80) usbKeyboard.release(KEY_RIGHT_GUI);
}
static void sendHID(uint8_t modifier, uint8_t keycode) {
  pressModifiers(modifier);
  if (keycode != 0) usbKeyboard.press(keycode);
  delay(60);
  if (keycode != 0) usbKeyboard.release(keycode);
  releaseModifiers(modifier);
  usbKeyboard.releaseAll();
}
static void tapBackspace() {
  usbKeyboard.press(KEY_BACKSPACE);
  delay(20);
  usbKeyboard.release(KEY_BACKSPACE);
  usbKeyboard.releaseAll();
}
static void pressOption() {
  if (optionHeld) return;
  usbKeyboard.press(KEY_RIGHT_ALT);
  optionHeld = true;
}
static void releaseOption() {
  if (!optionHeld) return;
  usbKeyboard.release(KEY_RIGHT_ALT);
  usbKeyboard.releaseAll();
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
  // 不能再直接 M5.Display.getTouch()：M5.update() 已经读过一次触摸数据，
  // CST820B 读一次就会清掉，再读永远拿不到，导致扇形按键失效。
  auto t = M5.Touch.getDetail();
  if (!t.isPressed()) return false;
  // CST820B 触摸坐标本身就是屏幕坐标系（0..466，与 466x466 屏幕一致），
  // 直接使用，不要再做 x_max 归一化缩放，否则会双重缩放导致扇形错位。
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

// ===================== USB 麦克风 =====================
// ES8311 语音增益（24dB PGA / 24dB ADC / +6dB 数字）
static bool configureMicGain() {
  m5gfx::i2c::i2c_temporary_switcher_t bus(1, GPIO_NUM_47, GPIO_NUM_48);
  bool ok = true;
  ok = M5.In_I2C.writeRegister8(0x18, 0x17, 0xCB, 100000) && ok;
  ok = M5.In_I2C.writeRegister8(0x18, 0x16, 0x04, 100000) && ok;
  ok = M5.In_I2C.writeRegister8(0x18, 0x14, 0x18, 100000) && ok;
  ok = M5.In_I2C.writeRegister8(0x18, 0x1C, 0x6A, 100000) && ok;
  bus.restore();
  return ok;
}

static void micCaptureLoop(void*) {
  for (int i = 0; i < 2; ++i) {
    if (!M5.Mic.record(micBuf[i], kMicBlockSamples, kMicRate)) {
      micCaptureTask = nullptr; vTaskDelete(nullptr); return;
    }
  }
  size_t buf = 0;
  for (;;) {
    while (M5.Mic.isRecording() >= 2) vTaskDelay(1);
    MicBlock blk;
    memcpy(blk.s, micBuf[buf], kMicBlockBytes);
    if (xQueueSend(micQueue, &blk, pdMS_TO_TICKS(50)) != pdTRUE) {
      MicBlock drop;
      xQueueReceive(micQueue, &drop, 0);
      xQueueSend(micQueue, &blk, 0);
    }
    if (!M5.Mic.record(micBuf[buf], kMicBlockSamples, kMicRate)) {
      micCaptureTask = nullptr; vTaskDelete(nullptr); return;
    }
    buf ^= 1;
  }
}

static void micFeedLoop(void*) {
  MicBlock blk;
  for (;;) {
    if (xQueueReceive(micQueue, &blk, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
    const uint8_t* p = (const uint8_t*)blk.s;
    size_t rem = kMicBlockBytes;
    while (rem > 0) {
      size_t n = usbAudio.write(p + (kMicBlockBytes - rem), rem);
      if (n == 0) { vTaskDelay(1); continue; }
      rem -= n;
    }
  }
}

static void usbAudioEvent(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base != ARDUINO_USB_AUDIO_CARD_EVENTS ||
      id != ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT || data == nullptr) return;
  auto* d = static_cast<const arduino_usb_audio_card_event_data_t*>(data);
  if (d->interface_enable.interface != UAC_INTERFACE_MIC) return;
  micStreaming.store(d->interface_enable.enable);
  if (d->interface_enable.enable) {
    tu_fifo_t* ff = tud_audio_get_ep_in_ff();
    if (ff != nullptr) {
      uint16_t target = tud_audio_get_ep_in_fifo_threshold();
      uint16_t half = tu_fifo_depth(ff) / 2U;
      if (target == 0 || target > half) target = half;
      const uint8_t silence[256] = {0};
      while (tu_fifo_count(ff) < target) {
        uint16_t req = target - tu_fifo_count(ff);
        if (req > sizeof(silence)) req = sizeof(silence);
        if (usbAudio.write(silence, req) == 0) break;
      }
    }
  }
}

static bool beginUsbMic() {
  M5.Speaker.end();
  auto mc = M5.Mic.config();
  mc.sample_rate = kMicRate;
  mc.input_channel = m5::input_only_right;
  mc.dma_buf_len = 480;
  mc.dma_buf_count = 4;
  mc.over_sampling = 1;
  mc.magnification = 2;
  M5.Mic.config(mc);
  if (!M5.Mic.begin()) return false;

  M5.Mic.record(micBuf[0], kMicBlockSamples, kMicRate);
  uint32_t t = millis();
  while (M5.Mic.isRecording() != 0 && millis() - t < 100) vTaskDelay(1);
  M5.Mic.end();
  if (!M5.Mic.begin()) return false;
  configureMicGain();

  micQueue = xQueueCreate(2, sizeof(MicBlock));
  if (micQueue == nullptr) return false;

  usbAudio.onEvent(ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT, usbAudioEvent);
  if (!usbAudio.begin()) return false;

  USB.VID(0x303A);
  USB.PID(0x0002);
  USB.productName("StopWatch Mic + KB");
  USB.manufacturerName("M5Stack");
  USB.serialNumber("stopwatch-mic");
  if (!USB.begin()) return false;

  xTaskCreate(micCaptureLoop, "mic_cap", 4096, nullptr, 3, &micCaptureTask);
  xTaskCreate(micFeedLoop, "mic_feed", 4096, nullptr, 2, &micFeedTask);
  return true;
}

// ===================== 主流程 =====================
void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_spk = false;   // 麦克风/扬声器共用 ES8311，用麦克风时关扬声器
  cfg.internal_mic = true;
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

  // USB 键盘（必须在 USB.begin() 之前调用 begin 初始化；接口由全局构造函数注册）
  usbKeyboard.begin();

  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(0x00FF00);
  M5.Display.setCursor(2, 2);
  M5.Display.print("USB KB OK");

  if (beginUsbMic()) {
    M5.Display.setTextColor(0x00FF00);
    M5.Display.setCursor(2, 18);
    M5.Display.print("USB MIC OK");
  }
}

void loop() {
  M5.update();
  handleButtons();
  handleTouch();
  delay(5);
}
