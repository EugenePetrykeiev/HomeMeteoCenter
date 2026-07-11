#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <time.h>

#if __has_include(<esp_idf_version.h>)
#include <esp_idf_version.h>
#endif

// Wi-Fi settings
const char* WIFI_SSID = "Vodafone-56D3";
const char* WIFI_PASSWORD = "nnKP7TnTrYErjMeP";

// Hardware pins
const int LAMP_PIN = 13;
const int SOIL_SENSOR_PIN = 34;
const int SOIL_POWER_PIN = 32;
const int DHT_PIN = 15;

const bool LAMP_ACTIVE_HIGH = true;

// Magdeburg, Germany
const float LATITUDE = 52.1205;
const float LONGITUDE = 11.6276;
const char* TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";
const char* OPEN_METEO_TIMEZONE = "Europe%2FBerlin";

// Soil sensor calibration. Change these after real calibration.
const int SOIL_DRY_VALUE = 3000;
const int SOIL_WET_VALUE = 1200;

// Periodic tasks
const unsigned long DHT_INTERVAL_MS = 10000;
const unsigned long SOIL_INTERVAL_MS = 60000;
const unsigned long WEATHER_INTERVAL_MS = 60000;
const unsigned long SCHEDULE_INTERVAL_MS = 1000;
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 15000;
const unsigned long SOIL_STABILIZE_MS = 600;

#define DHTTYPE DHT22

enum OnMode : uint8_t {
  ON_MODE_TIME = 0,
  ON_MODE_SUNSET = 1
};

enum OffMode : uint8_t {
  OFF_MODE_TIME = 0,
  OFF_MODE_TIMER = 1
};

struct Settings {
  bool autoOnEnabled = false;
  uint8_t onMode = ON_MODE_TIME;
  uint8_t onHour = 18;
  uint8_t onMinute = 30;
  bool autoOffEnabled = false;
  uint8_t offMode = OFF_MODE_TIME;
  uint8_t offHour = 23;
  uint8_t offMinute = 0;
  uint16_t offTimerMinutes = 0;
  uint8_t activeDaysMask = 0b01111111;
};

struct SensorState {
  bool soilValid = false;
  int soilRaw = 0;
  int soilPercent = 0;
  bool dhtValid = false;
  float airTemperature = NAN;
  float airHumidity = NAN;
};

struct WeatherState {
  bool valid = false;
  float currentTemp = NAN;
  int currentCode = -1;
  char currentText[64] = "Немає даних";
  float tomorrowMin = NAN;
  float tomorrowMax = NAN;
  int tomorrowCode = -1;
  char tomorrowText[64] = "Немає даних";
  char sunrise[20] = "--:--";
  char sunset[20] = "--:--";
  uint8_t sunsetHour = 0;
  uint8_t sunsetMinute = 0;
  bool sunsetValid = false;
  time_t updatedAt = 0;
};

Preferences preferences;
WebServer server(80);
DHT dht(DHT_PIN, DHTTYPE);
Settings settings;
SensorState sensors;
WeatherState weather;

bool lampOn = false;
time_t lampTurnedOnEpoch = 0;

unsigned long lastDhtRead = 0;
unsigned long lastSoilStart = 0;
unsigned long soilPowerStartedAt = 0;
unsigned long lastWeatherFetch = 0;
unsigned long lastScheduleCheck = 0;
unsigned long lastWiFiReconnectAttempt = 0;
bool soilMeasurementActive = false;

int lastAutoOnYday = -1;
int lastAutoOnMinuteOfDay = -1;
int lastAutoOffYday = -1;
int lastAutoOffMinuteOfDay = -1;

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="uk">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Lamp</title>
  <style>
    :root {
      color-scheme: light;
      --bg: #f5f7f2;
      --panel: #ffffff;
      --ink: #18201d;
      --muted: #647067;
      --line: #dce4da;
      --green: #2f8c57;
      --amber: #d99028;
      --blue: #3572a5;
      --red: #b64b3c;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: var(--bg);
      color: var(--ink);
    }
    main {
      width: min(1180px, calc(100% - 28px));
      margin: 0 auto;
      padding: 18px 0 28px;
    }
    header {
      display: flex;
      align-items: flex-end;
      justify-content: space-between;
      gap: 16px;
      padding: 8px 0 18px;
      border-bottom: 1px solid var(--line);
    }
    h1, h2, p { margin: 0; }
    h1 { font-size: clamp(24px, 4vw, 42px); letter-spacing: 0; }
    h2 { font-size: 17px; letter-spacing: 0; }
    .time {
      text-align: right;
      font-size: clamp(26px, 5vw, 48px);
      font-weight: 750;
      font-variant-numeric: tabular-nums;
    }
    .sub { color: var(--muted); font-size: 14px; margin-top: 4px; }
    .grid {
      display: grid;
      grid-template-columns: repeat(12, 1fr);
      gap: 12px;
      padding-top: 16px;
    }
    section {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 14px;
      min-width: 0;
    }
    .span-3 { grid-column: span 3; }
    .span-4 { grid-column: span 4; }
    .span-5 { grid-column: span 5; }
    .span-6 { grid-column: span 6; }
    .span-7 { grid-column: span 7; }
    .span-12 { grid-column: span 12; }
    .row {
      display: flex;
      gap: 8px;
      align-items: center;
      flex-wrap: wrap;
    }
    .between { justify-content: space-between; }
    .stack { display: grid; gap: 10px; }
    button, input, select {
      min-height: 38px;
      border: 1px solid var(--line);
      border-radius: 8px;
      background: #fff;
      color: var(--ink);
      font: inherit;
      padding: 8px 10px;
    }
    button {
      cursor: pointer;
      font-weight: 650;
      transition: transform .12s ease, border-color .12s ease, background .12s ease;
    }
    button:hover { transform: translateY(-1px); border-color: #aebcaf; }
    button.primary { background: var(--green); color: #fff; border-color: var(--green); }
    button.warn { background: var(--amber); color: #fff; border-color: var(--amber); }
    button.ghost.active { background: #e8f2ec; border-color: var(--green); color: var(--green); }
    label {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      color: var(--muted);
      font-size: 14px;
    }
    input[type="checkbox"], input[type="radio"] { min-height: auto; width: 18px; height: 18px; }
    input[type="time"] { width: 120px; }
    .status {
      font-size: 26px;
      font-weight: 800;
      color: var(--red);
    }
    .status.on { color: var(--green); }
    .gauge-wrap {
      display: grid;
      grid-template-columns: 118px 1fr;
      align-items: center;
      gap: 14px;
    }
    .gauge {
      --value: 0;
      --gauge-color: var(--green);
      width: 118px;
      aspect-ratio: 1;
      border-radius: 50%;
      display: grid;
      place-items: center;
      background: conic-gradient(var(--gauge-color) calc(var(--value) * 1%), #e8eee7 0);
      position: relative;
    }
    .gauge::after {
      content: "";
      position: absolute;
      inset: 12px;
      border-radius: 50%;
      background: #fff;
    }
    .gauge span {
      position: relative;
      z-index: 1;
      font-size: 22px;
      font-weight: 850;
      font-variant-numeric: tabular-nums;
    }
    .metric {
      display: grid;
      gap: 4px;
      min-width: 0;
    }
    .metric strong { font-size: 18px; }
    .metric small { color: var(--muted); line-height: 1.35; }
    .days { display: grid; grid-template-columns: repeat(7, minmax(42px, 1fr)); gap: 6px; }
    .quick { display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; }
    .weather {
      display: grid;
      grid-template-columns: repeat(4, minmax(0, 1fr));
      gap: 10px;
    }
    .weather div {
      border-left: 3px solid var(--blue);
      padding-left: 10px;
      min-width: 0;
    }
    .value {
      font-size: 21px;
      font-weight: 760;
      font-variant-numeric: tabular-nums;
    }
    .save-line {
      display: flex;
      justify-content: flex-end;
      padding-top: 2px;
    }
    @media (max-width: 900px) {
      .span-3, .span-4, .span-5, .span-6, .span-7 { grid-column: span 12; }
      header { align-items: flex-start; flex-direction: column; }
      .time { text-align: left; }
      .weather { grid-template-columns: repeat(2, minmax(0, 1fr)); }
    }
    @media (max-width: 560px) {
      main { width: min(100% - 18px, 1180px); }
      .gauge-wrap { grid-template-columns: 1fr; }
      .quick, .weather { grid-template-columns: 1fr; }
      .days { grid-template-columns: repeat(4, minmax(42px, 1fr)); }
      button, input, select { width: 100%; }
      label button, label input, label select { width: auto; }
    }
  </style>
</head>
<body>
<main>
  <header>
    <div>
      <h1>ESP32 Lamp</h1>
      <p class="sub">Магдебург · Europe/Berlin · <span id="ip">IP очікується</span></p>
    </div>
    <div>
      <div class="time" id="clock">--:--:--</div>
      <p class="sub" id="dateLine">Синхронізація часу</p>
    </div>
  </header>

  <div class="grid">
    <section class="span-4 stack">
      <div class="row between">
        <h2>Лампа</h2>
        <div id="lampState" class="status">Вимкнена</div>
      </div>
      <div class="row">
        <button class="primary" onclick="lamp('on')">Увімкнути лампу</button>
        <button class="warn" onclick="lamp('off')">Вимкнути лампу</button>
      </div>
      <p class="sub" id="lampHint">Ручне керування працює незалежно від розкладу.</p>
    </section>

    <section class="span-4 stack">
      <h2>Авто ввімкнення</h2>
      <label><input id="autoOnEnabled" type="checkbox"> Активне</label>
      <div class="row">
        <label><input name="onMode" value="time" type="radio"> За часом</label>
        <input id="onTime" type="time">
      </div>
      <label><input name="onMode" value="sunset" type="radio"> За заходом сонця</label>
    </section>

    <section class="span-4 stack">
      <h2>Авто вимкнення</h2>
      <label><input id="autoOffEnabled" type="checkbox"> Активне</label>
      <div class="row">
        <label><input name="offMode" value="time" type="radio"> За часом</label>
        <input id="offTime" type="time">
      </div>
      <div class="row">
        <label><input name="offMode" value="timer" type="radio"> Таймер після увімкнення</label>
      </div>
      <div class="quick">
        <button class="ghost timer" data-min="60">1 год</button>
        <button class="ghost timer" data-min="120">2 год</button>
        <button class="ghost timer" data-min="180">3 год</button>
        <button class="ghost timer" data-min="300">5 год</button>
      </div>
    </section>

    <section class="span-7 stack">
      <div class="row between">
        <h2>Дні розкладу</h2>
        <button class="primary" onclick="saveSettings()">Зберегти</button>
      </div>
      <div class="days" id="days"></div>
      <div class="quick">
        <button onclick="setDays([0,1,2,3,4])">Тільки будні</button>
        <button onclick="setDays([5,6])">Тільки вихідні</button>
        <button onclick="setDays([0,1,2,3,4,5,6])">Усі дні</button>
        <button onclick="setDays([])">Очистити вибір</button>
      </div>
    </section>

    <section class="span-5 stack">
      <h2>Погода</h2>
      <div class="weather">
        <div><p class="sub">Зараз</p><p class="value" id="weatherNow">--</p></div>
        <div><p class="sub">Завтра</p><p class="value" id="weatherTomorrow">--</p></div>
        <div><p class="sub">Схід</p><p class="value" id="sunrise">--:--</p></div>
        <div><p class="sub">Захід</p><p class="value" id="sunset">--:--</p></div>
      </div>
      <p class="sub" id="weatherUpdated">Open-Meteo</p>
    </section>

    <section class="span-4">
      <div class="gauge-wrap">
        <div class="gauge" id="soilGauge"><span id="soilValue">--%</span></div>
        <div class="metric">
          <strong>Вологість ґрунту</strong>
          <small id="soilRaw">GPIO34 · очікування першого вимірювання</small>
        </div>
      </div>
    </section>

    <section class="span-4">
      <div class="gauge-wrap">
        <div class="gauge" id="tempGauge" style="--gauge-color: var(--amber)"><span id="tempValue">--°C</span></div>
        <div class="metric">
          <strong>Температура повітря</strong>
          <small>DHT22 · GPIO33</small>
        </div>
      </div>
    </section>

    <section class="span-4">
      <div class="gauge-wrap">
        <div class="gauge" id="humGauge" style="--gauge-color: var(--blue)"><span id="humValue">--%</span></div>
        <div class="metric">
          <strong>Вологість повітря</strong>
          <small>DHT22 · GPIO33</small>
        </div>
      </div>
    </section>
  </div>
</main>

<script>
const dayNames = ['Пн', 'Вт', 'Ср', 'Чт', 'Пт', 'Сб', 'Нд'];
let activeDays = new Set([0,1,2,3,4,5,6]);
let hydrated = false;
let baseEpoch = 0;
let baseMillis = 0;
let offTimerMinutes = 0;

function pad(n) { return String(n).padStart(2, '0'); }
function setText(id, value) { document.getElementById(id).textContent = value; }
function timeValue(h, m) { return `${pad(h)}:${pad(m)}`; }

function renderDays() {
  const wrap = document.getElementById('days');
  wrap.innerHTML = '';
  dayNames.forEach((name, index) => {
    const button = document.createElement('button');
    button.textContent = name;
    button.className = activeDays.has(index) ? 'ghost active' : 'ghost';
    button.onclick = () => {
      activeDays.has(index) ? activeDays.delete(index) : activeDays.add(index);
      renderDays();
    };
    wrap.appendChild(button);
  });
}

function setDays(days) {
  activeDays = new Set(days);
  renderDays();
}

function updateTimerButtons() {
  document.querySelectorAll('.timer').forEach(button => {
    button.classList.toggle('active', Number(button.dataset.min) === offTimerMinutes);
    button.onclick = () => {
      offTimerMinutes = Number(button.dataset.min);
      document.querySelector('input[name="offMode"][value="timer"]').checked = true;
      updateTimerButtons();
    };
  });
}

function hydrateSettings(data) {
  document.getElementById('autoOnEnabled').checked = data.schedule.autoOnEnabled;
  document.getElementById('autoOffEnabled').checked = data.schedule.autoOffEnabled;
  document.querySelector(`input[name="onMode"][value="${data.schedule.onMode}"]`).checked = true;
  document.querySelector(`input[name="offMode"][value="${data.schedule.offMode}"]`).checked = true;
  document.getElementById('onTime').value = timeValue(data.schedule.onHour, data.schedule.onMinute);
  document.getElementById('offTime').value = timeValue(data.schedule.offHour, data.schedule.offMinute);
  offTimerMinutes = data.schedule.offTimerMinutes;
  activeDays = new Set();
  for (let i = 0; i < 7; i++) {
    if (data.schedule.activeDaysMask & (1 << i)) activeDays.add(i);
  }
  renderDays();
  updateTimerButtons();
  hydrated = true;
}

function renderClock() {
  if (!baseEpoch) return;
  const now = new Date((baseEpoch + Math.floor((Date.now() - baseMillis) / 1000)) * 1000);
  setText('clock', `${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}`);
  setText('dateLine', now.toLocaleDateString('uk-UA', { weekday: 'long', year: 'numeric', month: 'long', day: 'numeric' }));
}

function setGauge(id, value, min = 0, max = 100) {
  const normalized = Math.max(0, Math.min(100, ((value - min) / (max - min)) * 100));
  document.getElementById(id).style.setProperty('--value', normalized.toFixed(0));
}

async function lamp(action) {
  await fetch(`/api/lamp/${action}`, { method: 'POST' });
  await loadStatus();
}

async function saveSettings() {
  const [onHour, onMinute] = document.getElementById('onTime').value.split(':').map(Number);
  const [offHour, offMinute] = document.getElementById('offTime').value.split(':').map(Number);
  let mask = 0;
  activeDays.forEach(day => mask |= (1 << day));
  const payload = {
    autoOnEnabled: document.getElementById('autoOnEnabled').checked,
    onMode: document.querySelector('input[name="onMode"]:checked').value,
    onHour, onMinute,
    autoOffEnabled: document.getElementById('autoOffEnabled').checked,
    offMode: document.querySelector('input[name="offMode"]:checked').value,
    offHour, offMinute,
    offTimerMinutes,
    activeDaysMask: mask
  };
  await fetch('/api/settings', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  });
  hydrated = false;
  await loadStatus();
}

async function loadStatus() {
  try {
    const response = await fetch('/api/status');
    const data = await response.json();
    setText('ip', data.ip || 'IP недоступний');
    if (data.timeValid) {
      baseEpoch = data.epoch;
      baseMillis = Date.now();
      renderClock();
    }
    const lampState = document.getElementById('lampState');
    lampState.textContent = data.lampOn ? 'Увімкнена' : 'Вимкнена';
    lampState.classList.toggle('on', data.lampOn);
    if (data.sensors.soil.valid) {
      setText('soilValue', `${data.sensors.soil.percent}%`);
      setText('soilRaw', `GPIO34 · raw ${data.sensors.soil.raw}`);
      setGauge('soilGauge', data.sensors.soil.percent);
    }
    if (data.sensors.dht.valid) {
      setText('tempValue', `${data.sensors.dht.temperature.toFixed(1)}°C`);
      setText('humValue', `${data.sensors.dht.humidity.toFixed(0)}%`);
      setGauge('tempGauge', data.sensors.dht.temperature, -10, 45);
      setGauge('humGauge', data.sensors.dht.humidity);
    }
    if (data.weather.valid) {
      setText('weatherNow', `${data.weather.currentTemp.toFixed(1)}°C · ${data.weather.currentText}`);
      setText('weatherTomorrow', `${data.weather.tomorrowMin.toFixed(0)}…${data.weather.tomorrowMax.toFixed(0)}°C · ${data.weather.tomorrowText}`);
      setText('sunrise', data.weather.sunrise || '--:--');
      setText('sunset', data.weather.sunset || '--:--');
      setText('weatherUpdated', data.weather.updatedAt || 'Open-Meteo');
    }
    if (!hydrated) hydrateSettings(data);
  } catch (error) {
    setText('ip', 'немає зв’язку з ESP32');
  }
}

renderDays();
updateTimerButtons();
loadStatus();
setInterval(renderClock, 1000);
setInterval(loadStatus, 10000);
</script>
</body>
</html>
)rawliteral";

int lampActiveLevel() {
  return LAMP_ACTIVE_HIGH ? HIGH : LOW;
}

int lampInactiveLevel() {
  return LAMP_ACTIVE_HIGH ? LOW : HIGH;
}

void setLamp(bool on) {
  lampOn = on;
  digitalWrite(LAMP_PIN, on ? lampActiveLevel() : lampInactiveLevel());
  if (on) {
    time(&lampTurnedOnEpoch);
  } else {
    lampTurnedOnEpoch = 0;
  }
}

bool localTime(tm& out) {
  return getLocalTime(&out, 50);
}

String localTimeString(time_t epoch = 0) {
  tm info;
  if (epoch == 0) {
    time(&epoch);
  }
  localtime_r(&epoch, &info);
  char buffer[24];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &info);
  return String(buffer);
}

const char* weatherCodeText(int code) {
  switch (code) {
    case 0: return "Ясно";
    case 1:
    case 2:
    case 3: return "Мінлива хмарність";
    case 45:
    case 48: return "Туман";
    case 51:
    case 53:
    case 55: return "Мряка";
    case 56:
    case 57: return "Крижана мряка";
    case 61:
    case 63:
    case 65: return "Дощ";
    case 66:
    case 67: return "Крижаний дощ";
    case 71:
    case 73:
    case 75: return "Сніг";
    case 77: return "Снігові зерна";
    case 80:
    case 81:
    case 82: return "Зливи";
    case 85:
    case 86: return "Снігові зливи";
    case 95: return "Гроза";
    case 96:
    case 99: return "Гроза з градом";
    default: return "Невідомо";
  }
}

String hhmmFromIso(const char* isoTime) {
  if (isoTime == nullptr) {
    return "--:--";
  }
  String value(isoTime);
  int marker = value.indexOf('T');
  if (marker < 0 || marker + 5 >= static_cast<int>(value.length())) {
    return "--:--";
  }
  return value.substring(marker + 1, marker + 6);
}

bool parseHourMinute(const char* hhmm, uint8_t& hour, uint8_t& minute) {
  if (hhmm == nullptr || strlen(hhmm) < 5 || hhmm[2] != ':') {
    return false;
  }
  int h = (hhmm[0] - '0') * 10 + (hhmm[1] - '0');
  int m = (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
  if (h < 0 || h > 23 || m < 0 || m > 59) {
    return false;
  }
  hour = static_cast<uint8_t>(h);
  minute = static_cast<uint8_t>(m);
  return true;
}

void loadSettings() {
  preferences.begin("lamp", true);
  settings.autoOnEnabled = preferences.getBool("autoOn", settings.autoOnEnabled);
  settings.onMode = preferences.getUChar("onMode", settings.onMode);
  settings.onHour = preferences.getUChar("onHour", settings.onHour);
  settings.onMinute = preferences.getUChar("onMin", settings.onMinute);
  settings.autoOffEnabled = preferences.getBool("autoOff", settings.autoOffEnabled);
  settings.offMode = preferences.getUChar("offMode", settings.offMode);
  settings.offHour = preferences.getUChar("offHour", settings.offHour);
  settings.offMinute = preferences.getUChar("offMin", settings.offMinute);
  settings.offTimerMinutes = preferences.getUShort("offTimer", settings.offTimerMinutes);
  settings.activeDaysMask = preferences.getUChar("days", settings.activeDaysMask);
  preferences.end();

  if (settings.onHour > 23) settings.onHour = 18;
  if (settings.onMinute > 59) settings.onMinute = 30;
  if (settings.offHour > 23) settings.offHour = 23;
  if (settings.offMinute > 59) settings.offMinute = 0;
  if (settings.onMode > ON_MODE_SUNSET) settings.onMode = ON_MODE_TIME;
  if (settings.offMode > OFF_MODE_TIMER) settings.offMode = OFF_MODE_TIME;
}

void saveSettings() {
  preferences.begin("lamp", false);
  preferences.putBool("autoOn", settings.autoOnEnabled);
  preferences.putUChar("onMode", settings.onMode);
  preferences.putUChar("onHour", settings.onHour);
  preferences.putUChar("onMin", settings.onMinute);
  preferences.putBool("autoOff", settings.autoOffEnabled);
  preferences.putUChar("offMode", settings.offMode);
  preferences.putUChar("offHour", settings.offHour);
  preferences.putUChar("offMin", settings.offMinute);
  preferences.putUShort("offTimer", settings.offTimerMinutes);
  preferences.putUChar("days", settings.activeDaysMask);
  preferences.end();
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 12000) {
    delay(250);
    Serial.print(".");
    esp_task_wdt_reset();
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi connection pending; will retry in loop.");
  }
}

void setupWatchdog() {
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t config = {};
  config.timeout_ms = 20000;
  config.idle_core_mask = (1 << portNUM_PROCESSORS) - 1;
  config.trigger_panic = true;
  esp_task_wdt_init(&config);
#else
  esp_task_wdt_init(20, true);
#endif
  esp_task_wdt_add(NULL);
}

void setupTime() {
  configTzTime(TIMEZONE, "pool.ntp.org", "time.nist.gov", "europe.pool.ntp.org");
}

void readDht() {
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();
  if (!isnan(humidity) && !isnan(temperature) &&
      humidity >= 0.0f && humidity <= 100.0f &&
      temperature >= -40.0f && temperature <= 80.0f) {
    sensors.airHumidity = humidity;
    sensors.airTemperature = temperature;
    sensors.dhtValid = true;
  }
}

void updateSoilSensor() {
  unsigned long nowMs = millis();
  if (!soilMeasurementActive && nowMs - lastSoilStart >= SOIL_INTERVAL_MS) {
    soilMeasurementActive = true;
    soilPowerStartedAt = nowMs;
    lastSoilStart = nowMs;
    digitalWrite(SOIL_POWER_PIN, HIGH);
  }

  if (!soilMeasurementActive || nowMs - soilPowerStartedAt < SOIL_STABILIZE_MS) {
    return;
  }

  long total = 0;
  const int samples = 8;
  for (int i = 0; i < samples; i++) {
    total += analogRead(SOIL_SENSOR_PIN);
  }
  int raw = total / samples;
  digitalWrite(SOIL_POWER_PIN, LOW);
  soilMeasurementActive = false;

  if (raw > 50 && raw < 4095) {
    int percent = map(raw, SOIL_DRY_VALUE, SOIL_WET_VALUE, 0, 100);
    sensors.soilPercent = constrain(percent, 0, 100);
    sensors.soilRaw = raw;
    sensors.soilValid = true;
  }
}

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  String url = "https://api.open-meteo.com/v1/forecast?latitude=";
  url += String(LATITUDE, 4);
  url += "&longitude=";
  url += String(LONGITUDE, 4);
  url += "&current=temperature_2m,weather_code";
  url += "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset";
  url += "&forecast_days=2&timezone=";
  url += OPEN_METEO_TIMEZONE;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(client, url)) {
    return;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return;
  }

  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();
  if (error) {
    return;
  }

  JsonVariant currentTemp = doc["current"]["temperature_2m"];
  weather.currentTemp = currentTemp.isNull() ? NAN : currentTemp.as<float>();
  weather.currentCode = doc["current"]["weather_code"] | -1;
  strlcpy(weather.currentText, weatherCodeText(weather.currentCode), sizeof(weather.currentText));

  JsonObject daily = doc["daily"].as<JsonObject>();
  JsonVariant tomorrowMin = daily["temperature_2m_min"][1];
  JsonVariant tomorrowMax = daily["temperature_2m_max"][1];
  weather.tomorrowMin = tomorrowMin.isNull() ? NAN : tomorrowMin.as<float>();
  weather.tomorrowMax = tomorrowMax.isNull() ? NAN : tomorrowMax.as<float>();
  weather.tomorrowCode = daily["weather_code"][1] | -1;
  strlcpy(weather.tomorrowText, weatherCodeText(weather.tomorrowCode), sizeof(weather.tomorrowText));

  String sunrise = hhmmFromIso(daily["sunrise"][0] | "");
  String sunset = hhmmFromIso(daily["sunset"][0] | "");
  strlcpy(weather.sunrise, sunrise.c_str(), sizeof(weather.sunrise));
  strlcpy(weather.sunset, sunset.c_str(), sizeof(weather.sunset));
  weather.sunsetValid = parseHourMinute(weather.sunset, weather.sunsetHour, weather.sunsetMinute);

  time(&weather.updatedAt);
  weather.valid = !isnan(weather.currentTemp);
}

bool isScheduleDayActive(const tm& now) {
  int mondayBasedIndex = (now.tm_wday + 6) % 7;
  return settings.activeDaysMask & (1 << mondayBasedIndex);
}

void checkSchedule() {
  tm now;
  if (!localTime(now) || !isScheduleDayActive(now)) {
    return;
  }

  int minuteOfDay = now.tm_hour * 60 + now.tm_min;

  if (settings.autoOnEnabled) {
    int targetMinute = -1;
    if (settings.onMode == ON_MODE_TIME) {
      targetMinute = settings.onHour * 60 + settings.onMinute;
    } else if (weather.sunsetValid) {
      targetMinute = weather.sunsetHour * 60 + weather.sunsetMinute;
    }

    if (targetMinute == minuteOfDay &&
        (lastAutoOnYday != now.tm_yday || lastAutoOnMinuteOfDay != minuteOfDay)) {
      setLamp(true);
      lastAutoOnYday = now.tm_yday;
      lastAutoOnMinuteOfDay = minuteOfDay;
    }
  }

  if (!settings.autoOffEnabled) {
    return;
  }

  if (settings.offMode == OFF_MODE_TIME) {
    int targetMinute = settings.offHour * 60 + settings.offMinute;
    if (targetMinute == minuteOfDay &&
        (lastAutoOffYday != now.tm_yday || lastAutoOffMinuteOfDay != minuteOfDay)) {
      setLamp(false);
      lastAutoOffYday = now.tm_yday;
      lastAutoOffMinuteOfDay = minuteOfDay;
    }
    return;
  }

  if (settings.offMode == OFF_MODE_TIMER && lampOn && settings.offTimerMinutes > 0 && lampTurnedOnEpoch > 0) {
    time_t nowEpoch;
    time(&nowEpoch);
    if (nowEpoch - lampTurnedOnEpoch >= static_cast<time_t>(settings.offTimerMinutes) * 60) {
      setLamp(false);
    }
  }
}

void addStatusJson(JsonDocument& doc) {
  tm now;
  time_t epoch;
  time(&epoch);
  bool hasTime = localTime(now);

  doc["timeValid"] = hasTime;
  doc["epoch"] = hasTime ? static_cast<uint32_t>(epoch) : 0;
  doc["localTime"] = hasTime ? localTimeString(epoch) : "";
  doc["lampOn"] = lampOn;
  doc["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;

  JsonObject sensorRoot = doc.createNestedObject("sensors");
  JsonObject soil = sensorRoot.createNestedObject("soil");
  soil["valid"] = sensors.soilValid;
  soil["raw"] = sensors.soilRaw;
  soil["percent"] = sensors.soilPercent;

  JsonObject dhtRoot = sensorRoot.createNestedObject("dht");
  dhtRoot["valid"] = sensors.dhtValid;
  dhtRoot["temperature"] = sensors.dhtValid ? sensors.airTemperature : 0;
  dhtRoot["humidity"] = sensors.dhtValid ? sensors.airHumidity : 0;

  JsonObject weatherRoot = doc.createNestedObject("weather");
  weatherRoot["valid"] = weather.valid;
  weatherRoot["currentTemp"] = weather.valid ? weather.currentTemp : 0;
  weatherRoot["currentCode"] = weather.currentCode;
  weatherRoot["currentText"] = weather.currentText;
  weatherRoot["tomorrowMin"] = weather.valid ? weather.tomorrowMin : 0;
  weatherRoot["tomorrowMax"] = weather.valid ? weather.tomorrowMax : 0;
  weatherRoot["tomorrowCode"] = weather.tomorrowCode;
  weatherRoot["tomorrowText"] = weather.tomorrowText;
  weatherRoot["sunrise"] = weather.sunrise;
  weatherRoot["sunset"] = weather.sunset;
  weatherRoot["updatedAt"] = weather.updatedAt > 0 ? localTimeString(weather.updatedAt) : "";

  JsonObject schedule = doc.createNestedObject("schedule");
  schedule["autoOnEnabled"] = settings.autoOnEnabled;
  schedule["onMode"] = settings.onMode == ON_MODE_SUNSET ? "sunset" : "time";
  schedule["onHour"] = settings.onHour;
  schedule["onMinute"] = settings.onMinute;
  schedule["autoOffEnabled"] = settings.autoOffEnabled;
  schedule["offMode"] = settings.offMode == OFF_MODE_TIMER ? "timer" : "time";
  schedule["offHour"] = settings.offHour;
  schedule["offMinute"] = settings.offMinute;
  schedule["offTimerMinutes"] = settings.offTimerMinutes;
  schedule["activeDaysMask"] = settings.activeDaysMask;
}

void sendJsonStatus() {
  DynamicJsonDocument doc(6144);
  addStatusJson(doc);
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

bool updateSettingsFromJson(const JsonDocument& doc) {
  if (doc.containsKey("autoOnEnabled")) settings.autoOnEnabled = doc["autoOnEnabled"].as<bool>();
  if (doc.containsKey("autoOffEnabled")) settings.autoOffEnabled = doc["autoOffEnabled"].as<bool>();

  const char* onMode = doc["onMode"].is<const char*>() ? doc["onMode"].as<const char*>() : nullptr;
  if (onMode != nullptr) {
    settings.onMode = strcmp(onMode, "sunset") == 0 ? ON_MODE_SUNSET : ON_MODE_TIME;
  }

  const char* offMode = doc["offMode"].is<const char*>() ? doc["offMode"].as<const char*>() : nullptr;
  if (offMode != nullptr) {
    settings.offMode = strcmp(offMode, "timer") == 0 ? OFF_MODE_TIMER : OFF_MODE_TIME;
  }

  int onHour = doc["onHour"] | settings.onHour;
  int onMinute = doc["onMinute"] | settings.onMinute;
  int offHour = doc["offHour"] | settings.offHour;
  int offMinute = doc["offMinute"] | settings.offMinute;
  int offTimer = doc["offTimerMinutes"] | settings.offTimerMinutes;
  int days = doc["activeDaysMask"] | settings.activeDaysMask;

  if (onHour < 0 || onHour > 23 || onMinute < 0 || onMinute > 59 ||
      offHour < 0 || offHour > 23 || offMinute < 0 || offMinute > 59 ||
      offTimer < 0 || offTimer > 1440 || days < 0 || days > 127) {
    return false;
  }

  settings.onHour = onHour;
  settings.onMinute = onMinute;
  settings.offHour = offHour;
  settings.offMinute = offMinute;
  settings.offTimerMinutes = offTimer;
  settings.activeDaysMask = days;
  return true;
}

void handleSettings() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  if (!updateSettingsFromJson(doc)) {
    server.send(422, "application/json", "{\"error\":\"invalid settings\"}");
    return;
  }

  saveSettings();
  sendJsonStatus();
}

void setupRoutes() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
  });
  server.on("/api/status", HTTP_GET, sendJsonStatus);
  server.on("/api/lamp/on", HTTP_POST, []() {
    setLamp(true);
    sendJsonStatus();
  });
  server.on("/api/lamp/off", HTTP_POST, []() {
    setLamp(false);
    sendJsonStatus();
  });
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.onNotFound([]() {
    server.send(404, "application/json", "{\"error\":\"not found\"}");
  });
}

void setup() {
  Serial.begin(115200);
  setupWatchdog();

  pinMode(LAMP_PIN, OUTPUT);
  pinMode(SOIL_POWER_PIN, OUTPUT);
  pinMode(SOIL_SENSOR_PIN, INPUT);
  digitalWrite(SOIL_POWER_PIN, LOW);
  setLamp(false);

  analogReadResolution(12);
  dht.begin();
  loadSettings();
  connectWiFi();
  setupTime();
  setupRoutes();
  server.begin();

  lastDhtRead = millis() - DHT_INTERVAL_MS;
  lastSoilStart = millis() - SOIL_INTERVAL_MS;
  lastWeatherFetch = millis() - WEATHER_INTERVAL_MS;
}

void loop() {
  esp_task_wdt_reset();
  server.handleClient();

  unsigned long nowMs = millis();
  if (WiFi.status() != WL_CONNECTED && nowMs - lastWiFiReconnectAttempt >= WIFI_RECONNECT_INTERVAL_MS) {
    lastWiFiReconnectAttempt = nowMs;
    WiFi.reconnect();
  }

  if (nowMs - lastDhtRead >= DHT_INTERVAL_MS) {
    lastDhtRead = nowMs;
    readDht();
  }

  updateSoilSensor();

  if (nowMs - lastWeatherFetch >= WEATHER_INTERVAL_MS) {
    lastWeatherFetch = nowMs;
    fetchWeather();
  }

  if (nowMs - lastScheduleCheck >= SCHEDULE_INTERVAL_MS) {
    lastScheduleCheck = nowMs;
    checkSchedule();
  }
}
