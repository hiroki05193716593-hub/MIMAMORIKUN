#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <esp_sleep.h>

#define PIR_PIN GPIO_NUM_13
#define LED_PIN 2

// ---- OTA 設定 ----
#define FIRMWARE_VERSION  "1.0.4"
const char* OTA_VERSION_URL  = "https://hiroki05193716593-hub.github.io/MIMAMORIKUN/version.txt";
const char* OTA_FIRMWARE_URL = "https://hiroki05193716593-hub.github.io/MIMAMORIKUN/firmware.bin";

// ---- 設定 ----
#define STRICT_SECS 300

// デバッグ時は 1 にするとシリアル出力が有効になる
#define DEBUG 0
#if DEBUG
  #define LOG(...) Serial.printf(__VA_ARGS__)
#else
  #define LOG(...)
#endif

static String wifiSSID, wifiPassword, lineToken, lineUserId;
static int    hourStart = 5, hourEnd = 10;

RTC_DATA_ATTR bool   doneForToday     = false;
RTC_DATA_ATTR bool   scheduledFor5am  = false;
RTC_DATA_ATTR bool   pirWarmedUp      = false;
RTC_DATA_ATTR time_t monitorStartTime = 0;

// ---- NVS ----

bool loadCredentials() {
  Preferences prefs;
  prefs.begin("mimamorikun", true);
  wifiSSID     = prefs.getString("ssid",   "");
  wifiPassword = prefs.getString("pass",   "");
  lineToken    = prefs.getString("token",  "");
  lineUserId   = prefs.getString("userid", "");
  hourStart    = prefs.getInt("hstart", 5);
  hourEnd      = prefs.getInt("hend",  10);
  prefs.end();
  return wifiSSID.length() > 0 && lineToken.length() > 0;
}

void saveCredentials() {
  Preferences prefs;
  prefs.begin("mimamorikun", false);
  prefs.putString("ssid",   wifiSSID);
  prefs.putString("pass",   wifiPassword);
  prefs.putString("token",  lineToken);
  prefs.putString("userid", lineUserId);
  prefs.putInt("hstart", hourStart);
  prefs.putInt("hend",   hourEnd);
  prefs.end();
}

// Serial は設定モードの対話入力に必須なので常に有効
String readSerialLine(const char* prompt) {
  Serial.print(prompt);
  String line = "";
  while (true) {
    while (!Serial.available()) delay(10);
    char c = Serial.read();
    if (c == '\n') break;
    if (c == '\r') continue;
    if (c == 0x08 || c == 0x7F) {
      if (line.length() > 0) {
        line.remove(line.length() - 1);
        Serial.print("\b \b");
      }
      continue;
    }
    line += c;
    Serial.print(c);
  }
  Serial.println();
  return line;
}

void enterSetupMode() {
  Serial.println("\n=== NVS設定モード ===");
  Serial.println("各項目を入力してEnterを押してください\n");

  wifiSSID     = readSerialLine("WiFi SSID     : ");
  wifiPassword = readSerialLine("WiFi Password : ");
  lineToken    = readSerialLine("LINE Token    : ");
  lineUserId   = readSerialLine("LINE User ID  : ");

  while (true) {
    String s = readSerialLine("監視開始時刻（0〜23）  : ");
    int n = s.toInt();
    if (n >= 0 && n <= 23) { hourStart = n; break; }
    Serial.println("0〜23 で入力してください");
  }
  while (true) {
    String s = readSerialLine("監視終了時刻（0〜23）  : ");
    int n = s.toInt();
    if (n > hourStart && n <= 23) { hourEnd = n; break; }
    Serial.printf("開始時刻（%d）より大きい値を入力してください\n", hourStart);
  }

  saveCredentials();
  Serial.println("\nNVSに保存しました。再起動します...");
  delay(1000);
  ESP.restart();
}

// ---- LED ----

static TaskHandle_t ledBlinkHandle = nullptr;
static void ledBlinkTask(void*) {
  for (;;) {
    digitalWrite(LED_PIN, HIGH); vTaskDelay(pdMS_TO_TICKS(500));
    digitalWrite(LED_PIN, LOW);  vTaskDelay(pdMS_TO_TICKS(500));
  }
}
void startLedBlink() {
  if (!ledBlinkHandle)
    xTaskCreate(ledBlinkTask, "led", 1024, nullptr, 1, &ledBlinkHandle);
}
void stopLedBlink() {
  if (ledBlinkHandle) {
    vTaskDelete(ledBlinkHandle);
    ledBlinkHandle = nullptr;
    digitalWrite(LED_PIN, LOW);
  }
}

void blinkFast() {
  stopLedBlink();
  unsigned long start = millis();
  while (millis() - start < 30000) {
    digitalWrite(LED_PIN, HIGH); delay(100);
    digitalWrite(LED_PIN, LOW);  delay(100);
  }
  digitalWrite(LED_PIN, LOW);
}

// ---- WiFi / NTP / LINE ----

// 起動時専用：30秒間隔で最大 maxAttempts 回まで粘る
bool connectWiFiWithRetry(int maxAttempts, int waitSec) {
  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    if (attempt > 0) {
      LOG("WiFi再試行 %d/%d（%d秒後）\n", attempt + 1, maxAttempts, waitSec);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay((long)waitSec * 1000);
    }
    if (connectWiFi()) return true;
  }
  return false;
}

bool connectWiFi() {
  LOG("WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 40) { delay(500); timeout++; }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    delay(1000);
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
    timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 40) { delay(500); timeout++; }
  }
  if (WiFi.status() == WL_CONNECTED) { LOG("OK\n"); return true; }
  LOG("失敗\n"); return false;
}

bool syncTime(struct tm &t) {
  struct timeval tvSaved;
  gettimeofday(&tvSaved, nullptr);
  struct timeval tv_zero = {0, 0};
  settimeofday(&tv_zero, nullptr);
  configTime(9 * 3600, 0, "ntp.nict.go.jp", "pool.ntp.org", "time.google.com");
  LOG("NTP...");
  for (int i = 0; i < 5; i++) {
    if (getLocalTime(&t, 15000) && t.tm_year > 120) {
      LOG("OK %02d:%02d\n", t.tm_hour, t.tm_min);
      return true;
    }
  }
  settimeofday(&tvSaved, nullptr);
  LOG("失敗\n"); return false;
}

bool sendLine(const char* message) {
  if (WiFi.status() != WL_CONNECTED) return false;
  LOG("LINE: 「%s」", message);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://api.line.me/v2/bot/message/push");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + lineToken);
  JsonDocument doc;
  doc["to"] = lineUserId;
  doc["messages"][0]["type"] = "text";
  doc["messages"][0]["text"] = message;
  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  LOG(" %d\n", code);
  http.end();
  return code == 200;
}

// ---- スリープ ----

void sleepUntil5am(struct tm &t) {
  int h = t.tm_hour, m = t.tm_min, s = t.tm_sec;
  long secs = (h < hourStart)
    ? (hourStart - h) * 3600L - m * 60 - s
    : (24 - h + hourStart) * 3600L - m * 60 - s;
  scheduledFor5am = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  LOG("スリープ %ld秒\n", secs);
  esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
  esp_deep_sleep_start();
}

// ---- OTA ----

void checkOTA() {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);

  HTTPClient http;
  http.begin(client, OTA_VERSION_URL);
  http.setTimeout(5000);
  int code = http.GET();
  if (code != 200) {
    LOG("OTA確認スキップ(HTTP%d)\n", code);
    http.end();
    return;
  }
  String serverVer = http.getString();
  serverVer.trim();
  http.end();

  LOG("OTA 現在:%s サーバー:%s\n", FIRMWARE_VERSION, serverVer.c_str());
  if (serverVer == FIRMWARE_VERSION) return;

  LOG("OTA更新開始...\n");
  t_httpUpdate_return ret = httpUpdate.update(client, OTA_FIRMWARE_URL);
  if (ret == HTTP_UPDATE_FAILED) {
    LOG("OTA失敗: %s → 通常動作を続けます\n", httpUpdate.getLastErrorString().c_str());
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
      int timeout = 0;
      while (WiFi.status() != WL_CONNECTED && timeout < 20) { delay(500); timeout++; }
    }
  }
}

// ---- PIR ----

bool confirmPIR(int minCount) {
  int count = 0;
  for (int i = 0; i < 15; i++) {
    if (digitalRead(PIR_PIN) == HIGH) count++;
    delay(200);
  }
  bool result = count >= minCount;
  LOG("PIR %d/15 閾値%d → %s\n", count, minCount, result ? "検知" : "誤反応");
  return result;
}

int pirThreshold() {
  if (monitorStartTime == 0) return 14;
  struct tm tNow;
  if (!getLocalTime(&tNow, 0)) return 14;
  long elapsed = (long)difftime(mktime(&tNow), monitorStartTime);
  int threshold = (elapsed < STRICT_SECS) ? 14 : 10;
  LOG("閾値%d（経過%lds）\n", threshold, elapsed);
  return threshold;
}

// ---- 監視ループ（5〜10時は起動しっぱなし）----

void monitoringLoop() {
  struct tm tStart;
  if (getLocalTime(&tStart, 0) && tStart.tm_year > 120)
    monitorStartTime = mktime(&tStart);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  LOG("監視ループ開始\n");

  while (true) {
    struct tm tNow;
    if (!getLocalTime(&tNow, 0) || tNow.tm_year <= 120) { delay(1000); continue; }

    if (tNow.tm_hour >= hourEnd) {
      if (!connectWiFi()) { delay(60000); continue; }
      struct tm t;
      if (!syncTime(t)) { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); delay(60000); continue; }
      if (t.tm_hour < hourEnd) { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); delay(60000); continue; }
      if (!doneForToday) {
        char msg[80];
        snprintf(msg, sizeof(msg), "%02d:%02d 時点で起床が確認できていません！要対応！", t.tm_hour, t.tm_min);
        if (!sendLine(msg)) {
          WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
          delay(30000); continue;
        }
        doneForToday = true;
      }
      sleepUntil5am(t);
    }

    if (digitalRead(PIR_PIN) == HIGH) {
      if (confirmPIR(pirThreshold())) {
        if (!connectWiFiWithRetry(5, 30)) { delay(30000); continue; }
        struct tm t;
        if (!syncTime(t)) { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); delay(30000); continue; }
        char msg[64];
        snprintf(msg, sizeof(msg), "%02d:%02d に起床を確認しました", t.tm_hour, t.tm_min);
        if (!sendLine(msg)) {
          WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
          delay(30000); continue;
        }
        doneForToday = true;
        sleepUntil5am(t);
      } else {
        delay(30000);
      }
    }

    delay(200);
  }
}

// ---- メイン ----

void setup() {
  Serial.begin(115200);  // 設定モードの対話入力に必須
  pinMode(LED_PIN, OUTPUT);
  pinMode(PIR_PIN, INPUT);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool isFreshBoot = (cause != ESP_SLEEP_WAKEUP_EXT0 && cause != ESP_SLEEP_WAKEUP_TIMER);

  // 初回起動のみ：任意のキーで設定モードに入れる（30秒待機・LED点滅）
  if (isFreshBoot) {
    Serial.println("\n何かキーを押すと設定モード（30秒）...");
    startLedBlink();
    unsigned long waitStart = millis();
    bool enterPressed = false;
    while (millis() - waitStart < 30000) {
      if (Serial.available()) {
        Serial.read();
        enterPressed = true;
        break;
      }
    }
    while (Serial.available()) Serial.read();
    stopLedBlink();

    if (enterPressed) {
      enterSetupMode();
      return;
    }
  }

  if (!loadCredentials()) {
    Serial.println("NVS未設定！設定モードに入ります...");
    enterSetupMode();
    return;
  }

  if (isFreshBoot && !pirWarmedUp) startLedBlink();

  LOG("\n=== 起動:%s done=%d ===\n",
      (cause == ESP_SLEEP_WAKEUP_TIMER) ? "タイマー" : "初回起動", doneForToday);

  if (cause == ESP_SLEEP_WAKEUP_TIMER && scheduledFor5am) {
    doneForToday     = false;
    scheduledFor5am  = false;
    monitorStartTime = 0;
    LOG("AM5リセット\n");
  }

  if (!connectWiFiWithRetry(5, 30)) {  // 30秒間隔×最大5回（約3分）粘る
    blinkFast();
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    esp_sleep_enable_timer_wakeup(3600ULL * 1000000ULL);
    esp_deep_sleep_start();
  }
  struct tm t;
  if (!syncTime(t)) {
    blinkFast();
    if (isFreshBoot) sendLine("【起動】デバイスが起動しました（時刻同期失敗）");
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    esp_sleep_enable_timer_wakeup(3600ULL * 1000000ULL);
    esp_deep_sleep_start();
  }

  LOG("時刻 %02d:%02d 監視窓 %d〜%d時\n", t.tm_hour, t.tm_min, hourStart, hourEnd);

  if (isFreshBoot) {
    char msg[80];
    snprintf(msg, sizeof(msg), "【起動】%02d:%02d にデバイスが起動しました (v%s)", t.tm_hour, t.tm_min, FIRMWARE_VERSION);
    sendLine(msg);
  }

  checkOTA();

  if (t.tm_hour < hourStart) { sleepUntil5am(t); }

  if (t.tm_hour >= hourEnd) {
    if (!doneForToday && cause == ESP_SLEEP_WAKEUP_TIMER) {
      char msg[80];
      snprintf(msg, sizeof(msg), "%02d:%02d 時点で起床が確認できていません！要対応！", t.tm_hour, t.tm_min);
      for (int retry = 0; retry < 3 && !sendLine(msg); retry++) delay(10000);
      doneForToday = true;
    }
    sleepUntil5am(t);
  }

  if (doneForToday) { sleepUntil5am(t); }

  if (!pirWarmedUp) {
    delay(60000);
    pirWarmedUp = true;
  }

  stopLedBlink();
  monitoringLoop();
}

void loop() {}
