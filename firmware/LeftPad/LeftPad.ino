#include <Arduino.h>
#include <bluefruit.h>
#include "Adafruit_TinyUSB.h"
#include <RotaryEncoder.h>

//-----------------------------------------
// Rotary Encoder 定義
//-----------------------------------------
#define ENCODER_A 31  // エンコーダ A相（プルアップ入力）
#define ENCODER_B 30  // エンコーダ B相（プルアップ入力）
RotaryEncoder encoder(ENCODER_A, ENCODER_B, RotaryEncoder::LatchMode::FOUR3);

//-----------------------------------------
// デバッグモード
//-----------------------------------------
bool is_debugMode = true;  // trueでシリアル出力有効

//-----------------------------------------
// ボタンとLEDピンの設定
//-----------------------------------------
#define BUTTON_0 0
#define BUTTON_1 1
#define BUTTON_2 2
#define BUTTON_3 3
#define BUTTON_4 4
#define BUTTON_5 5
#define BUTTON_6 6
#define BUTTON_7 7
#define BUTTON_8 8
#define BUTTON_9 9

//-----------------------------------------
// バッテリー関連の設定
//-----------------------------------------
#define PIN_HICHG 22
#define PIN_INVCHG 23

// 平均化設定
#define BAT_AVERAGE_COUNT 16
#define BAT_AVERAGE_MASK 0x0F

//-----------------------------------------
// BLE HIDキーボード関連
//-----------------------------------------
BLEDis bledis;
BLEHidAdafruit blehid;

//-----------------------------------------
// スリープ関連
//-----------------------------------------
unsigned long lastActivityTime = 0;
const unsigned long sleepTimeout = 1800000;  // 30分

//-----------------------------------------
// バッテリー電圧
//-----------------------------------------
double batteryVoltage = 4.0;

//-----------------------------------------
// ボタン状態管理
//-----------------------------------------
static bool buttonState[10] = { false, false, false, false, false, false, false, false, false, false};

//-----------------------------------------
// キーマッピング（例）
// ※ キーコードは用途に合わせて調整してください
//-----------------------------------------
struct KeyMapping {
  uint8_t modifier;
  uint8_t keycode;
};
KeyMapping keymap[12];

enum BleState {
  BLE_DISCONNECTED,
  BLE_ADVERTISING,
  BLE_CONNECTED
};

enum BatteryState {
  BAT_NORMAL,
  BAT_LOW,
  BAT_CHARGING,
  BAT_FULL
};

// 現在の状態
BleState currentBleState = BLE_DISCONNECTED;
BatteryState currentBatteryState = BAT_NORMAL;

//-----------------------------------------
// 定数: ボタンピン配列（ピン定義は変更しない）
//-----------------------------------------
const uint8_t buttonPins[10] = { BUTTON_0, BUTTON_1, BUTTON_2, BUTTON_3, BUTTON_4, BUTTON_5, BUTTON_6, BUTTON_7, BUTTON_8, BUTTON_9 };


//------------------------------------------------------------------------------
// LED制御（LOWで点灯する想定）
//------------------------------------------------------------------------------
void setLED(bool r, bool g, bool b) {
  digitalWrite(LED_RED, !r);
  digitalWrite(LED_GREEN, !g);
  digitalWrite(LED_BLUE, !b);
}

//------------------------------------------------------------------------------
// デバッグ出力ラッパ
//------------------------------------------------------------------------------
void debugPrint(const char* message) {
  if (is_debugMode) {
    Serial.println(message);
  }
}

//------------------------------------------------------------------------------
// バッテリー充電判定（例: PIN_INVCHGがLOWなら充電中）
//------------------------------------------------------------------------------
bool battery_isCharging() {
  return (digitalRead(PIN_INVCHG) == LOW);
}

//------------------------------------------------------------------------------
// バッテリー電圧測定
//------------------------------------------------------------------------------
void measureBattery() {
  static uint32_t lastMeasure = 0;
  static bool lastIsCharging = false;
  static uint16_t rawvalues[BAT_AVERAGE_COUNT] = { 0 };
  static uint8_t index = 0;
  static uint8_t count = 0;

  uint32_t ms = millis();
  bool isCharging = battery_isCharging();

  if (ms - lastMeasure > 3000) {  // 3秒毎に測定
    lastMeasure = ms;
    if (lastIsCharging != isCharging) {
      index = 0;
      count = 0;
    }
    lastIsCharging = isCharging;

    // バッテリ計測回路を有効化 (VBAT_ENABLEをLOWに) → analogRead → 無効化
    pinMode(VBAT_ENABLE, OUTPUT);
    digitalWrite(VBAT_ENABLE, LOW);
    delay(2);

    rawvalues[index] = (uint16_t)analogRead(PIN_VBAT);
    pinMode(VBAT_ENABLE, INPUT);  // High-Z戻し

    index = (index + 1) & BAT_AVERAGE_MASK;
    count = min(count + 1, (uint8_t)BAT_AVERAGE_COUNT);

    // 平均値をとる
    uint32_t rawtotal = 0;
    for (uint8_t i = 0; i < count; i++) {
      rawtotal += rawvalues[i];
    }
    float rawavg = (float)rawtotal / (float)count;

    // 10bit ADC (0-1023), Vref=3.6V, 分圧比 ≈2.96 (1510/510) の一例
    batteryVoltage = rawavg / 1024.0 * 3.6 * (1510.0 / 510.0);

    if (is_debugMode) {
      Serial.print("Battery Voltage: ");
      Serial.print(batteryVoltage, 2);
      Serial.println(" V");
    }
  }
}

//------------------------------------------------------------------------------
// バッテリー状態更新
//------------------------------------------------------------------------------
void updateBatteryState() {
  if (battery_isCharging()) {
    currentBatteryState = BAT_CHARGING;
  } else {
    if (batteryVoltage < 3.7) {
      currentBatteryState = BAT_LOW;
    } else if (batteryVoltage > 4.15) {
      currentBatteryState = BAT_FULL;
    } else {
      currentBatteryState = BAT_NORMAL;
    }
  }
}

//------------------------------------------------------------------------------
// LED更新（バッテリー & BLE状態に基づく）
//------------------------------------------------------------------------------
void updateLED() {
  static uint32_t lastBlinkTime = 0;
  static bool blinkState = false;
  uint32_t currentTime = millis();

  // 1. バッテリー電圧低下が最優先（赤点滅）
  if (currentBatteryState == BAT_LOW) {
    if (currentTime - lastBlinkTime >= 500) {
      lastBlinkTime = currentTime;
      blinkState = !blinkState;
    }
    setLED(blinkState, false, false);
    return;
  }

  // 2. BLE未接続（アドバタイズ or 非アクティブ）は青点滅
  if (currentBleState == BLE_ADVERTISING || currentBleState == BLE_DISCONNECTED) {
    if (currentTime - lastBlinkTime >= 500) {
      lastBlinkTime = currentTime;
      blinkState = !blinkState;
    }
    setLED(false, false, blinkState);
    return;
  }

  // 3. BLE接続中は緑点灯
  if (currentBleState == BLE_CONNECTED) {
    setLED(false, true, false);
    return;
  }

  // 4. その他は消灯
  setLED(false, false, false);
}

//------------------------------------------------------------------------------
// ボタンチェックとキー送信
//------------------------------------------------------------------------------
void checkButton(uint8_t index, uint8_t pin) {
  if (digitalRead(pin) == HIGH) {
    delay(10);  // チャタリング対策
    if (digitalRead(pin) == HIGH && !buttonState[index]) {
      buttonState[index] = true;
      if (is_debugMode) {
        Serial.print("BUTTON_");
        Serial.print(index);
        Serial.print(" pressed. Keycode: 0x");
        Serial.println(keymap[index].keycode, HEX);
      }
      // HIDキーボードレポート送信
      uint8_t keycodes[6] = { keymap[index].keycode, 0, 0, 0, 0, 0 };
      blehid.keyboardReport(0, keymap[index].modifier, keycodes);
      delay(10);
      blehid.keyRelease();
    }
  } else {
    buttonState[index] = false;
  }
}

//------------------------------------------------------------------------------
// BLE接続コールバック
//------------------------------------------------------------------------------
void connect_callback(uint16_t conn_handle) {
  debugPrint("[BLE] Connected!");
  currentBleState = BLE_CONNECTED;
}

//------------------------------------------------------------------------------
// BLE切断コールバック
//------------------------------------------------------------------------------
void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  if (is_debugMode) {
    Serial.print("[BLE] Disconnected! Reason: ");
    Serial.println(reason);
  }
  currentBleState = BLE_ADVERTISING;
}

//------------------------------------------------------------------------------
// BLEアドバタイズ開始
//------------------------------------------------------------------------------
void startAdv() {
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addAppearance(BLE_APPEARANCE_HID_KEYBOARD);
  Bluefruit.Advertising.addService(blehid);
  Bluefruit.Advertising.addName();

  // 切断されたら自動再開
  Bluefruit.Advertising.restartOnDisconnect(true);

  // アドバタイズ間隔
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.start(0);
  currentBleState = BLE_ADVERTISING;
}

//------------------------------------------------------------------------------
// キーマッピング初期化
//------------------------------------------------------------------------------
void initKeymap() {
  keymap[0].modifier = 0x00;
  keymap[0].keycode  = 0x13;  // 'P'

  keymap[1].modifier = 0x00;
  keymap[1].keycode  = 0x10;  // 'M'

  keymap[2].modifier = 0x00;
  keymap[2].keycode  = 0x08;  // 'E'

  keymap[3].modifier = 0x08;  // LEFT GUI (Command)
  keymap[3].keycode  = 0x1D;   // 'Z'

  keymap[4].modifier = 0x08;  // LEFT GUI (Command)
  keymap[4].keycode  = 0x17;   // 'T'

  keymap[5].modifier = 0x08;  // LEFT GUI (Command)
  keymap[5].keycode  = 0x07;   // 'D'

  keymap[6].modifier = 0x00;
  keymap[6].keycode  = 0x52;  // '↑'

  keymap[7].modifier = 0x00;
  keymap[7].keycode  = 0x50;  // '←'

  keymap[8].modifier = 0x00;
  keymap[8].keycode  = 0x4F;  // '→'

  keymap[9].modifier = 0x00;
  keymap[9].keycode  = 0x51;  // '↓'

  keymap[10].modifier = 0x00;
  keymap[10].keycode  = 0x2F;  // '['  ロータリーエンコーダ反時計回り

  keymap[11].modifier = 0x00;
  keymap[11].keycode  = 0x30;  // ']'  ロータリーエンコーダ時計回り

  debugPrint("Keymap initialized (hard-coded).");
}

//------------------------------------------------------------------------------
// ディープスリープ処理
//------------------------------------------------------------------------------
void deepSleep() {
  setLED(false, false, false);

  // LEDピンをHigh-Zに（消灯状態を確実に保つ）
  digitalWrite(LED_RED, HIGH);
  digitalWrite(LED_GREEN, HIGH);
  digitalWrite(LED_BLUE, HIGH);
  pinMode(LED_RED, INPUT);
  pinMode(LED_GREEN, INPUT);
  pinMode(LED_BLUE, INPUT);

  Bluefruit.Advertising.stop();
  delay(50);

  // システムOFF解除用に一部ボタンピンを設定
  const uint8_t sleepButtonPins[] = { D0, D1, D2, D3, D4, D5, D6, D7, D8, D9};
  for (uint8_t i = 0; i < sizeof(sleepButtonPins) / sizeof(sleepButtonPins[0]); i++) {
    pinMode(sleepButtonPins[i], INPUT_PULLDOWN);
    nrf_gpio_cfg_sense_input(digitalPinToPinName(sleepButtonPins[i]),
                             NRF_GPIO_PIN_PULLDOWN, NRF_GPIO_PIN_SENSE_HIGH);
  }

  NRF_POWER->SYSTEMOFF = 1;
  while (1) {
    __WFE();
  }
}

//------------------------------------------------------------------------------
// 割り込みハンドラ (ロータリーエンコーダ)
//------------------------------------------------------------------------------
void isr() {
  encoder.tick();
}

//------------------------------------------------------------------------------
// セットアップ
//------------------------------------------------------------------------------
void setup() {
  // LEDピン設定
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  setLED(true, true, true);  // 白点灯

  Serial.begin(115200);
  if (is_debugMode) {
    unsigned long startTime = millis();
    while (!Serial && (millis() - startTime < 5000)) {
      delay(10);
    }
  }

  debugPrint("Booting...");

  // ボタンピンの設定（forループで共通化）
  for (uint8_t i = 0; i < 10; i++) {
    pinMode(buttonPins[i], INPUT_PULLDOWN);
  }

  // バッテリー関連ピン設定
  pinMode(PIN_HICHG, INPUT);
  pinMode(PIN_INVCHG, INPUT);

  // Rotary Encoder 用ピン設定
  pinMode(ENCODER_A, INPUT_PULLUP);
  pinMode(ENCODER_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENCODER_A), isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_B), isr, CHANGE);

  // BLE初期化
  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);
  Bluefruit.begin();
  Bluefruit.setTxPower(4);
  Bluefruit.setName("XIAO LeftPad");

  // デバイス情報
  bledis.setManufacturer("Morita.Tomoki");
  bledis.setModel("BLE Keyboard");
  bledis.begin();

  // HIDキーボード開始
  blehid.begin();

  // キーマッピング初期化
  initKeymap();

  // アドバタイズ開始
  startAdv();

  currentBleState = BLE_ADVERTISING;
  currentBatteryState = BAT_NORMAL;
  lastActivityTime = millis();

  debugPrint("Setup complete. Enter normal mode.");
}

//------------------------------------------------------------------------------
// メインループ
//------------------------------------------------------------------------------
void loop() {
  measureBattery();
  updateBatteryState();

  // ボタンチェックとキー送信処理（forループで共通化）
  bool active = false;
  for (uint8_t i = 0; i < 10; i++) {
    checkButton(i, buttonPins[i]);
    if (buttonState[i]) active = true;
  }

  // ロータリーエンコーダの状態チェック（1ノッチごとにキー入力送信）
  static int lastEncoderPos = encoder.getPosition();
  int newPos = encoder.getPosition();
  if (newPos != lastEncoderPos) {
    if (newPos > lastEncoderPos) {
      // 反時計回り
      uint8_t keycodes[6] = { keymap[10].keycode, 0, 0, 0, 0, 0 };
      blehid.keyboardReport(0, keymap[10].modifier, keycodes);
      delay(10);
      blehid.keyRelease();
      debugPrint("Encoder: -");
    } else {
      // 時計回り
      uint8_t keycodes[6] = { keymap[11].keycode, 0, 0, 0, 0, 0 };
      blehid.keyboardReport(0, keymap[11].modifier, keycodes);
      delay(10);
      blehid.keyRelease();
      debugPrint("Encoder: +");
    }
    lastEncoderPos = newPos;
    active = true;
  }

  if (active) {
    lastActivityTime = millis();
  }

  if ((millis() - lastActivityTime) > sleepTimeout) {
    debugPrint("Entering deep sleep...");
    delay(50);
    deepSleep();
  }

  updateLED();
}