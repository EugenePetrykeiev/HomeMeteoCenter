#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <math.h>
#include <time.h>

#if __has_include(<esp_idf_version.h>)
#include <esp_idf_version.h>
#endif

// Wi-Fi settings
const char* WIFI_SSID = "Vodafone-56D3";
const char* WIFI_PASSWORD = "nnKP7TnTrYErjMeP";

// Hardware pins
const int LAMP_PIN = 13;
// GPIO34 is an ADC1 input-only pin and remains available while Wi-Fi is active.
const int SOIL_SENSOR_PIN = 34;
const int SOIL_POWER_PIN = 32;
// GPIO15 is an ESP32 boot-strapping pin. If boot becomes unstable or slow,
// check the DHT pull-up wiring or move DHT_PIN to a non-strapping GPIO.
const int DHT_PIN = 15;

// The relay input is active LOW: LOW turns the lamp on, HIGH turns it off.
const bool LAMP_ACTIVE_HIGH = false;

// Magdeburg, Germany
const float LATITUDE = 52.1205;
const float LONGITUDE = 11.6276;
const char* TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";
const char* OPEN_METEO_TIMEZONE = "Europe%2FBerlin";

// Soil sensor calibration. Change these after real calibration.
const int SOIL_DRY_VALUE = 3000;
const int SOIL_WET_VALUE = 1200;

// Periodic tasks
const unsigned long SENSOR_STEP_INTERVAL_MS = 3000;
const unsigned long WEATHER_SUCCESS_INTERVAL_MS = 15UL * 60UL * 1000UL;
const unsigned long WEATHER_RETRY_INTERVAL_MS = 60000;
const unsigned long WEATHER_INITIAL_DELAY_MS = 5000;
const unsigned long SCHEDULE_INTERVAL_MS = 1000;
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 15000;
const unsigned long SOIL_STABILIZE_MS = 600;
const unsigned long SENSOR_LOG_INTERVAL_MS = 60000;
const uint16_t SENSOR_HISTORY_DAYS = 7;
const uint16_t SENSOR_HISTORY_SIZE = SENSOR_HISTORY_DAYS * 24 * 60;
const uint16_t HISTORY_API_MAX_POINTS = 480;
const char* SENSOR_HISTORY_PATH = "/sensor-history.bin";
const uint32_t SENSOR_HISTORY_MAGIC = 0x53484C47;

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
  int httpCode = 0;
  char error[96] = "";
};

struct __attribute__((packed)) SensorLogEntry {
  uint32_t epoch = 0;
  int16_t temperatureC10 = 0;
  uint16_t airHumidity10 = 0;
  uint16_t soilRaw = 0;
  uint8_t soilPercent = 0;
  uint8_t flags = 0;
};

struct __attribute__((packed)) SensorHistoryHeader {
  uint32_t magic = SENSOR_HISTORY_MAGIC;
  uint16_t version = 1;
  uint16_t recordSize = sizeof(SensorLogEntry);
  uint16_t capacity = SENSOR_HISTORY_SIZE;
  uint16_t count = 0;
  uint16_t nextIndex = 0;
  uint16_t reserved = 0;
};

static_assert(sizeof(SensorLogEntry) == 12, "Unexpected sensor history record size");

Preferences preferences;
WebServer server(80);
DHT dht(DHT_PIN, DHTTYPE);
Settings settings;
SensorState sensors;
WeatherState weather;
SensorHistoryHeader sensorHistoryHeader;

bool lampOn = false;
time_t lampTurnedOnEpoch = 0;

unsigned long lastSensorStep = 0;
unsigned long soilPowerStartedAt = 0;
unsigned long lastWeatherFetch = 0;
unsigned long weatherFetchInterval = WEATHER_INITIAL_DELAY_MS;
unsigned long lastScheduleCheck = 0;
unsigned long lastWiFiReconnectAttempt = 0;
unsigned long lastSensorLog = 0;
bool soilMeasurementActive = false;
bool nextSensorStepIsDht = true;
bool sensorHistoryReady = false;

int lastScheduleEventDay = -1;
int lastScheduleEventMinute = -1;

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
    button.lamp-toggle { min-width: 190px; }
    em.inline-time {
      color: var(--blue);
      font-style: italic;
      font-weight: 650;
    }
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
    .chart-panel {
      display: grid;
      grid-template-columns: minmax(0, 2fr) minmax(240px, 1fr);
      gap: 14px;
      align-items: stretch;
    }
    canvas {
      width: 100%;
      height: 260px;
      border: 1px solid var(--line);
      border-radius: 8px;
      background: #fbfcfa;
    }
    .legend {
      display: flex;
      gap: 12px;
      flex-wrap: wrap;
      color: var(--muted);
      font-size: 13px;
    }
    .dot {
      width: 10px;
      height: 10px;
      border-radius: 50%;
      display: inline-block;
      margin-right: 5px;
    }
    .analysis {
      display: grid;
      gap: 8px;
      align-content: start;
      color: var(--muted);
      line-height: 1.4;
    }
    @media (max-width: 900px) {
      .span-3, .span-4, .span-5, .span-6, .span-7 { grid-column: span 12; }
      header { align-items: flex-start; flex-direction: column; }
      .time { text-align: left; }
      .weather { grid-template-columns: repeat(2, minmax(0, 1fr)); }
      .chart-panel { grid-template-columns: 1fr; }
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
        <button id="lampToggle" class="primary lamp-toggle" onclick="toggleLamp()">Увімкнути лампу</button>
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
      <label><input name="onMode" value="sunset" type="radio"> За заходом сонця <em class="inline-time" id="sunsetAutoTime">--:--</em></label>
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
          <small>DHT22 · GPIO15</small>
        </div>
      </div>
    </section>

    <section class="span-4">
      <div class="gauge-wrap">
        <div class="gauge" id="humGauge" style="--gauge-color: var(--blue)"><span id="humValue">--%</span></div>
        <div class="metric">
          <strong>Вологість повітря</strong>
          <small>DHT22 · GPIO15</small>
        </div>
      </div>
    </section>

    <section class="span-12 stack">
      <div class="row between">
        <h2>Графіки та аналіз</h2>
        <p class="sub" id="historyInfo">Історія завантажується</p>
      </div>
      <div class="chart-panel">
        <div class="stack">
          <canvas id="historyChart" width="920" height="300"></canvas>
          <div class="legend">
            <span><span class="dot" style="background: var(--amber)"></span>Температура</span>
            <span><span class="dot" style="background: var(--blue)"></span>Вологість повітря</span>
            <span><span class="dot" style="background: var(--green)"></span>Вологість ґрунту</span>
          </div>
        </div>
        <div class="analysis" id="chartAnalysis">Потрібно щонайменше два збережені вимірювання.</div>
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
let currentLampOn = false;
let historyRows = [];

function pad(n) { return String(n).padStart(2, '0'); }
function setText(id, value) { document.getElementById(id).textContent = value; }
function timeValue(h, m) { return `${pad(h)}:${pad(m)}`; }
function minuteLabel(value) {
  return value >= 0 ? `${pad(Math.floor(value / 60))}:${pad(value % 60)}` : '--:--';
}

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

function setSoilGauge(value) {
  const gauge = document.getElementById('soilGauge');
  const percent = Math.max(0, Math.min(100, value));
  const color = percent < 20 ? 'var(--red)' : percent < 60 ? 'var(--amber)' : 'var(--green)';
  gauge.style.setProperty('--gauge-color', color);
  setGauge('soilGauge', percent);
}

function soilGradeText(value) {
  if (value < 20) return 'сухо';
  if (value < 60) return 'середньо';
  return 'добре';
}

async function lamp(action) {
  await fetch(`/api/lamp/${action}`, { method: 'POST' });
  await loadStatus();
}

async function toggleLamp() {
  await lamp(currentLampOn ? 'off' : 'on');
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
  const response = await fetch('/api/settings', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  });
  if (!response.ok) {
    setText('lampHint', 'Не вдалося зберегти розклад. Перевірте вибраний час.');
    return;
  }
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
    const lampToggle = document.getElementById('lampToggle');
    currentLampOn = data.lampOn;
    lampState.textContent = data.lampOn ? 'Увімкнена' : 'Вимкнена';
    lampState.classList.toggle('on', data.lampOn);
    lampToggle.textContent = data.lampOn ? 'Вимкнути лампу' : 'Увімкнути лампу';
    lampToggle.className = data.lampOn ? 'warn lamp-toggle' : 'primary lamp-toggle';
    if (data.sensors.soil.valid) {
      setText('soilValue', `${data.sensors.soil.percent}%`);
      setText('soilRaw', `GPIO34 · raw ${data.sensors.soil.raw} · ${soilGradeText(data.sensors.soil.percent)}`);
      setSoilGauge(data.sensors.soil.percent);
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
      setText('sunsetAutoTime', data.weather.sunset || '--:--');
      setText('weatherUpdated', data.weather.updatedAt || 'Open-Meteo');
    } else {
      setText('weatherNow', '--');
      setText('weatherTomorrow', '--');
      setText('sunrise', '--:--');
      setText('sunset', '--:--');
      setText('sunsetAutoTime', data.weather.sunset || '--:--');
      setText('weatherUpdated', data.weather.error ? `Open-Meteo: ${data.weather.error}` : 'Open-Meteo');
    }
    if (!data.timeValid) {
      setText('lampHint', 'Розклад очікує синхронізацію часу через інтернет.');
    } else if (data.schedule.autoOnEnabled && data.schedule.onMode === 'sunset' && !data.weather.sunsetValid) {
      setText('lampHint', 'Розклад за заходом очікує дані Open-Meteo.');
    } else if (!data.schedule.activeToday) {
      setText('lampHint', 'Сьогодні не вибрано в днях розкладу.');
    } else {
      const onText = data.schedule.autoOnEnabled
        ? `увімкнення ${minuteLabel(data.schedule.onTargetMinute)}`
        : 'автоввімкнення вимкнене';
      const offText = data.schedule.autoOffEnabled
        ? (data.schedule.offMode === 'timer'
            ? `вимкнення через ${data.schedule.offTimerMinutes} хв`
            : `вимкнення ${minuteLabel(data.schedule.offTargetMinute)}`)
        : 'автовимкнення вимкнене';
      setText('lampHint', `Сьогодні: ${onText}; ${offText}.`);
    }
    if (!hydrated) hydrateSettings(data);
  } catch (error) {
    setText('ip', 'немає зв’язку з ESP32');
  }
}

function drawHistoryChart() {
  const canvas = document.getElementById('historyChart');
  const ctx = canvas.getContext('2d');
  const width = canvas.width;
  const height = canvas.height;
  const plot = { left: 58, right: 54, top: 22, bottom: 48 };
  const plotWidth = width - plot.left - plot.right;
  const plotHeight = height - plot.top - plot.bottom;
  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = '#fbfcfa';
  ctx.fillRect(0, 0, width, height);
  ctx.font = '12px system-ui, sans-serif';
  ctx.textBaseline = 'middle';

  // Y axes: humidity on the left, temperature on the right.
  ctx.strokeStyle = '#dce4da';
  ctx.lineWidth = 1;
  for (let i = 0; i <= 5; i++) {
    const ratio = i / 5;
    const y = plot.top + plotHeight * ratio;
    ctx.beginPath();
    ctx.moveTo(plot.left, y);
    ctx.lineTo(width - plot.right, y);
    ctx.stroke();
    ctx.fillStyle = '#647067';
    ctx.textAlign = 'right';
    ctx.fillText(`${Math.round(100 * (1 - ratio))}%`, plot.left - 8, y);
    ctx.textAlign = 'left';
    ctx.fillText(`${Math.round(45 - 55 * ratio)}°`, width - plot.right + 8, y);
  }
  ctx.fillStyle = '#647067';
  ctx.textAlign = 'left';
  ctx.fillText('Вологість, %', plot.left, 10);
  ctx.textAlign = 'right';
  ctx.fillText('Температура, °C', width - plot.right, 10);

  if (historyRows.length < 2) return;

  const validEpochs = historyRows.map(row => row.epoch).filter(epoch => epoch > 0);
  const firstEpoch = validEpochs.length ? validEpochs[0] : 0;
  const lastEpoch = validEpochs.length ? validEpochs[validEpochs.length - 1] : 0;
  const epochSpan = Math.max(1, lastEpoch - firstEpoch);

  // X axis uses the actual sample timestamps.
  ctx.strokeStyle = '#9fac9f';
  ctx.beginPath();
  ctx.moveTo(plot.left, height - plot.bottom);
  ctx.lineTo(width - plot.right, height - plot.bottom);
  ctx.stroke();
  const xTicks = 5;
  for (let i = 0; i < xTicks; i++) {
    const ratio = i / (xTicks - 1);
    const x = plot.left + plotWidth * ratio;
    const epoch = firstEpoch + epochSpan * ratio;
    const date = new Date(epoch * 1000);
    const label = epochSpan >= 24 * 3600
      ? date.toLocaleString('uk-UA', { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' })
      : date.toLocaleTimeString('uk-UA', { hour: '2-digit', minute: '2-digit' });
    ctx.strokeStyle = '#9fac9f';
    ctx.beginPath();
    ctx.moveTo(x, height - plot.bottom);
    ctx.lineTo(x, height - plot.bottom + 5);
    ctx.stroke();
    ctx.fillStyle = '#647067';
    ctx.textAlign = i === 0 ? 'left' : i === xTicks - 1 ? 'right' : 'center';
    ctx.textBaseline = 'top';
    ctx.fillText(label, x, height - plot.bottom + 9);
  }
  ctx.textBaseline = 'middle';

  function point(row, index, value, min, max) {
    const epochRatio = row.epoch > 0
      ? (row.epoch - firstEpoch) / epochSpan
      : index / (historyRows.length - 1);
    const x = plot.left + plotWidth * Math.max(0, Math.min(1, epochRatio));
    const y = plot.top + plotHeight * (1 - Math.max(0, Math.min(1, (value - min) / (max - min))));
    return { x, y };
  }
  function line(key, color, min, max) {
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    let started = false;
    historyRows.forEach((row, index) => {
      if (row[key] === null || row[key] === undefined) return;
      const p = point(row, index, row[key], min, max);
      if (!started) {
        ctx.moveTo(p.x, p.y);
        started = true;
      } else {
        ctx.lineTo(p.x, p.y);
      }
    });
    ctx.stroke();
  }
  line('temperature', '#d99028', -10, 45);
  line('airHumidity', '#3572a5', 0, 100);
  line('soilPercent', '#2f8c57', 0, 100);
}

function summarizeSeries(label, rows, key, unit) {
  const values = rows.map(row => row[key]).filter(value => typeof value === 'number');
  if (values.length < 2) return `${label}: замало даних.`;
  const first = values[0];
  const last = values[values.length - 1];
  const avg = values.reduce((sum, value) => sum + value, 0) / values.length;
  const diff = last - first;
  const trend = Math.abs(diff) < 0.5 ? 'стабільно' : diff > 0 ? 'зростає' : 'знижується';
  return `${label}: ${trend}, зараз ${last.toFixed(1)}${unit}, середнє ${avg.toFixed(1)}${unit}.`;
}

function analyzeHistory() {
  const box = document.getElementById('chartAnalysis');
  if (historyRows.length < 2) {
    box.textContent = 'Потрібно щонайменше два збережені вимірювання.';
    return;
  }
  box.innerHTML = [
    summarizeSeries('Температура', historyRows, 'temperature', '°C'),
    summarizeSeries('Вологість повітря', historyRows, 'airHumidity', '%'),
    summarizeSeries('Вологість ґрунту', historyRows, 'soilPercent', '%')
  ].map(text => `<p>${text}</p>`).join('');
}

async function loadHistory() {
  try {
    const response = await fetch('/api/history');
    const data = await response.json();
    historyRows = data.history || [];
    const sampling = data.sampleMinutes > 1
      ? `графік: ${historyRows.length} точок, крок ≈${data.sampleMinutes} хв`
      : `графік: ${historyRows.length} точок`;
    setText('historyInfo', `${data.count || 0} із ${data.capacity || 10080} записів · ${data.retentionDays || 7} днів · ${sampling}`);
    drawHistoryChart();
    analyzeHistory();
  } catch (error) {
    setText('historyInfo', 'Історія недоступна');
  }
}

renderDays();
updateTimerButtons();
loadStatus();
loadHistory();
setInterval(renderClock, 1000);
setInterval(loadStatus, 10000);
setInterval(loadHistory, 60000);
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
  int outputLevel = on ? lampActiveLevel() : lampInactiveLevel();
  digitalWrite(LAMP_PIN, outputLevel);
  if (on) {
    time(&lampTurnedOnEpoch);
  } else {
    lampTurnedOnEpoch = 0;
  }
  Serial.printf("Lamp: %s, GPIO%d=%s\n",
                on ? "on" : "off", LAMP_PIN, outputLevel == HIGH ? "HIGH" : "LOW");
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

void startSoilMeasurement() {
  if (soilMeasurementActive) {
    return;
  }
  soilMeasurementActive = true;
  soilPowerStartedAt = millis();
  digitalWrite(SOIL_POWER_PIN, HIGH);
}

void finishSoilMeasurementIfReady() {
  unsigned long nowMs = millis();
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

String weatherUrl(const char* scheme) {
  String url = String(scheme) + "://api.open-meteo.com/v1/forecast?latitude=";
  url += String(LATITUDE, 4);
  url += "&longitude=";
  url += String(LONGITUDE, 4);
  url += "&current=temperature_2m,weather_code";
  url += "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset";
  url += "&forecast_days=2&timezone=";
  url += OPEN_METEO_TIMEZONE;
  return url;
}

bool parseWeatherPayload(const String& payload) {
  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    snprintf(weather.error, sizeof(weather.error), "JSON: %s", error.c_str());
    return false;
  }

  if (doc["error"] | false) {
    const char* reason = doc["reason"] | "API error";
    snprintf(weather.error, sizeof(weather.error), "API: %.80s", reason);
    return false;
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
  if (weather.valid) {
    weather.error[0] = '\0';
  } else {
    strlcpy(weather.error, "missing current temperature", sizeof(weather.error));
  }
  return weather.valid;
}

bool fetchWeatherHttps(const String& url) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(12000);
#if defined(ESP_IDF_VERSION_MAJOR)
  client.setHandshakeTimeout(10);
#endif
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    strlcpy(weather.error, "HTTPS begin failed", sizeof(weather.error));
    return false;
  }

  http.addHeader("User-Agent", "ESP32-Lamp/1.0");
  http.addHeader("Connection", "close");
  esp_task_wdt_reset();
  weather.httpCode = http.GET();
  if (weather.httpCode != HTTP_CODE_OK) {
    String detail = HTTPClient::errorToString(weather.httpCode);
    snprintf(weather.error, sizeof(weather.error), "HTTPS %d: %.64s", weather.httpCode, detail.c_str());
    Serial.printf("Open-Meteo request failed: %s\n", weather.error);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();
  esp_task_wdt_reset();
  bool ok = parseWeatherPayload(payload);
  if (!ok) {
    Serial.printf("Open-Meteo response failed: %s, bytes=%u\n", weather.error, payload.length());
  }
  return ok;
}

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    strlcpy(weather.error, "Wi-Fi disconnected", sizeof(weather.error));
    return;
  }

  weather.httpCode = 0;
  fetchWeatherHttps(weatherUrl("https"));
}

bool writeSensorHistoryHeader(File& file) {
  if (!file.seek(0, SeekSet)) {
    return false;
  }
  return file.write(reinterpret_cast<const uint8_t*>(&sensorHistoryHeader),
                    sizeof(sensorHistoryHeader)) == sizeof(sensorHistoryHeader);
}

bool appendSensorHistoryEntry(const SensorLogEntry& entry) {
  if (!sensorHistoryReady) {
    return false;
  }

  File file = LittleFS.open(SENSOR_HISTORY_PATH, "r+");
  if (!file) {
    return false;
  }

  size_t offset = sizeof(SensorHistoryHeader) +
                  static_cast<size_t>(sensorHistoryHeader.nextIndex) * sizeof(SensorLogEntry);
  bool written = file.seek(offset, SeekSet) &&
                 file.write(reinterpret_cast<const uint8_t*>(&entry), sizeof(entry)) == sizeof(entry);
  if (written) {
    sensorHistoryHeader.nextIndex = (sensorHistoryHeader.nextIndex + 1) % SENSOR_HISTORY_SIZE;
    if (sensorHistoryHeader.count < SENSOR_HISTORY_SIZE) {
      sensorHistoryHeader.count++;
    }
    written = writeSensorHistoryHeader(file);
  }
  file.flush();
  file.close();
  return written;
}

void migrateLegacySensorHistory() {
  struct LegacySensorLogEntry {
    uint32_t epoch;
    int16_t temperatureC10;
    uint16_t airHumidity10;
    uint8_t soilPercent;
    uint16_t soilRaw;
    uint8_t flags;
  };

  constexpr uint16_t LEGACY_CAPACITY = 60;
  LegacySensorLogEntry legacy[LEGACY_CAPACITY] = {};
  preferences.begin("sensorHistory", true);
  uint16_t count = min(preferences.getUShort("count", 0), LEGACY_CAPACITY);
  uint16_t next = preferences.getUShort("next", 0);
  size_t bytes = preferences.getBytesLength("entries");
  if (bytes == sizeof(legacy)) {
    preferences.getBytes("entries", legacy, sizeof(legacy));
  } else {
    count = 0;
  }
  preferences.end();

  int start = count < LEGACY_CAPACITY ? 0 : next % LEGACY_CAPACITY;
  for (uint16_t i = 0; i < count; i++) {
    const LegacySensorLogEntry& oldEntry = legacy[(start + i) % LEGACY_CAPACITY];
    SensorLogEntry entry;
    entry.epoch = oldEntry.epoch;
    entry.temperatureC10 = oldEntry.temperatureC10;
    entry.airHumidity10 = oldEntry.airHumidity10;
    entry.soilRaw = oldEntry.soilRaw;
    entry.soilPercent = oldEntry.soilPercent;
    entry.flags = oldEntry.flags;
    appendSensorHistoryEntry(entry);
  }
  if (count > 0) {
    Serial.printf("History: migrated %u legacy records\n", count);
  }
}

void loadSensorHistory() {
  if (!LittleFS.begin(true)) {
    Serial.println("History: LittleFS mount failed");
    return;
  }

  bool createNew = !LittleFS.exists(SENSOR_HISTORY_PATH);
  if (!createNew) {
    File file = LittleFS.open(SENSOR_HISTORY_PATH, "r");
    SensorHistoryHeader stored;
    bool valid = file &&
                 file.read(reinterpret_cast<uint8_t*>(&stored), sizeof(stored)) == sizeof(stored) &&
                 stored.magic == SENSOR_HISTORY_MAGIC && stored.version == 1 &&
                 stored.recordSize == sizeof(SensorLogEntry) &&
                 stored.capacity == SENSOR_HISTORY_SIZE &&
                 stored.count <= SENSOR_HISTORY_SIZE && stored.nextIndex < SENSOR_HISTORY_SIZE;
    size_t expectedRecords = valid && stored.count == SENSOR_HISTORY_SIZE
                               ? SENSOR_HISTORY_SIZE : (valid ? stored.count : 0);
    valid = valid && file.size() >= sizeof(SensorHistoryHeader) +
                              expectedRecords * sizeof(SensorLogEntry);
    if (file) file.close();
    if (valid) {
      sensorHistoryHeader = stored;
    } else {
      createNew = true;
      LittleFS.remove(SENSOR_HISTORY_PATH);
    }
  }

  if (createNew) {
    sensorHistoryHeader = SensorHistoryHeader();
    File file = LittleFS.open(SENSOR_HISTORY_PATH, "w");
    if (!file || !writeSensorHistoryHeader(file)) {
      Serial.println("History: cannot create storage file");
      if (file) file.close();
      return;
    }
    file.close();
  }

  sensorHistoryReady = true;
  if (createNew) {
    migrateLegacySensorHistory();
  }
  Serial.printf("History: %u/%u records ready\n",
                sensorHistoryHeader.count, SENSOR_HISTORY_SIZE);
}

void logSensorSnapshot() {
  if (!sensors.dhtValid && !sensors.soilValid) {
    return;
  }

  SensorLogEntry entry;
  time_t epoch;
  time(&epoch);
  entry.epoch = static_cast<uint32_t>(epoch);
  if (sensors.dhtValid) {
    entry.temperatureC10 = static_cast<int16_t>(lroundf(sensors.airTemperature * 10.0f));
    entry.airHumidity10 = static_cast<uint16_t>(lroundf(sensors.airHumidity * 10.0f));
    entry.flags |= 0x01;
  }
  if (sensors.soilValid) {
    entry.soilPercent = static_cast<uint8_t>(sensors.soilPercent);
    entry.soilRaw = static_cast<uint16_t>(sensors.soilRaw);
    entry.flags |= 0x02;
  }

  if (!appendSensorHistoryEntry(entry)) {
    Serial.println("History: record write failed");
  }
}

void updateSensorHistory() {
  unsigned long nowMs = millis();
  if (nowMs - lastSensorLog >= SENSOR_LOG_INTERVAL_MS) {
    lastSensorLog = nowMs;
    logSensorSnapshot();
  }
}

void updateSensors() {
  finishSoilMeasurementIfReady();
  unsigned long nowMs = millis();
  if (nowMs - lastSensorStep < SENSOR_STEP_INTERVAL_MS || soilMeasurementActive) {
    return;
  }
  lastSensorStep = nowMs;
  if (nextSensorStepIsDht) {
    readDht();
  } else {
    startSoilMeasurement();
  }
  nextSensorStepIsDht = !nextSensorStepIsDht;
}

bool isScheduleDayActive(const tm& now) {
  int mondayBasedIndex = (now.tm_wday + 6) % 7;
  return settings.activeDaysMask & (1 << mondayBasedIndex);
}

int configuredOnTargetMinute() {
  if (!settings.autoOnEnabled) {
    return -1;
  }
  if (settings.onMode == ON_MODE_TIME) {
    return settings.onHour * 60 + settings.onMinute;
  }
  return weather.sunsetValid ? weather.sunsetHour * 60 + weather.sunsetMinute : -1;
}

int configuredOffTargetMinute() {
  if (!settings.autoOffEnabled || settings.offMode != OFF_MODE_TIME) {
    return -1;
  }
  return settings.offHour * 60 + settings.offMinute;
}

void runDueScheduleEvent(bool turnOn, int targetMinute, int minuteOfDay, int dayKey) {
  if (targetMinute < 0 || targetMinute > minuteOfDay) {
    return;
  }
  if (lastScheduleEventDay == dayKey && targetMinute <= lastScheduleEventMinute) {
    return;
  }

  setLamp(turnOn);
  lastScheduleEventDay = dayKey;
  lastScheduleEventMinute = targetMinute;
  Serial.printf("Schedule: lamp %s at %02d:%02d\n",
                turnOn ? "on" : "off", targetMinute / 60, targetMinute % 60);
}

void checkSchedule() {
  tm now;
  if (!localTime(now) || !isScheduleDayActive(now)) {
    return;
  }

  int minuteOfDay = now.tm_hour * 60 + now.tm_min;
  int dayKey = (now.tm_year + 1900) * 400 + now.tm_yday;
  int onTargetMinute = configuredOnTargetMinute();
  int offTargetMinute = configuredOffTargetMinute();

  // Catch up missed events in chronological order. This survives slow network
  // requests, brief Wi-Fi outages, and restarts after the exact target minute.
  if (onTargetMinute >= 0 && offTargetMinute >= 0) {
    if (onTargetMinute <= offTargetMinute) {
      runDueScheduleEvent(true, onTargetMinute, minuteOfDay, dayKey);
      runDueScheduleEvent(false, offTargetMinute, minuteOfDay, dayKey);
    } else {
      runDueScheduleEvent(false, offTargetMinute, minuteOfDay, dayKey);
      runDueScheduleEvent(true, onTargetMinute, minuteOfDay, dayKey);
    }
  } else {
    runDueScheduleEvent(true, onTargetMinute, minuteOfDay, dayKey);
    runDueScheduleEvent(false, offTargetMinute, minuteOfDay, dayKey);
  }

  if (settings.autoOffEnabled && settings.offMode == OFF_MODE_TIMER &&
      lampOn && settings.offTimerMinutes > 0 && lampTurnedOnEpoch > 0) {
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
  weatherRoot["sunsetValid"] = weather.sunsetValid;
  weatherRoot["updatedAt"] = weather.updatedAt > 0 ? localTimeString(weather.updatedAt) : "";
  weatherRoot["httpCode"] = weather.httpCode;
  weatherRoot["error"] = weather.error;

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
  schedule["activeToday"] = hasTime && isScheduleDayActive(now);
  schedule["onTargetMinute"] = configuredOnTargetMinute();
  schedule["offTargetMinute"] = configuredOffTargetMinute();
}

void sendJsonStatus() {
  DynamicJsonDocument doc(6144);
  addStatusJson(doc);
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

bool readSensorHistoryEntry(File& file, uint16_t logicalIndex, SensorLogEntry& entry) {
  if (logicalIndex >= sensorHistoryHeader.count) {
    return false;
  }
  uint16_t start = sensorHistoryHeader.count < SENSOR_HISTORY_SIZE
                     ? 0 : sensorHistoryHeader.nextIndex;
  uint16_t physicalIndex = (start + logicalIndex) % SENSOR_HISTORY_SIZE;
  size_t offset = sizeof(SensorHistoryHeader) +
                  static_cast<size_t>(physicalIndex) * sizeof(SensorLogEntry);
  return file.seek(offset, SeekSet) &&
         file.read(reinterpret_cast<uint8_t*>(&entry), sizeof(entry)) == sizeof(entry);
}

void sendJsonHistory() {
  if (!sensorHistoryReady) {
    server.send(503, "application/json", "{\"error\":\"history storage unavailable\"}");
    return;
  }

  File file = LittleFS.open(SENSOR_HISTORY_PATH, "r");
  if (!file) {
    server.send(503, "application/json", "{\"error\":\"history file unavailable\"}");
    return;
  }

  uint16_t stride = (sensorHistoryHeader.count + HISTORY_API_MAX_POINTS - 1) /
                    HISTORY_API_MAX_POINTS;
  if (stride == 0) stride = 1;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  String chunk = "{\"count\":" + String(sensorHistoryHeader.count) +
                 ",\"capacity\":" + String(SENSOR_HISTORY_SIZE) +
                 ",\"retentionDays\":" + String(SENSOR_HISTORY_DAYS) +
                 ",\"sampleMinutes\":" + String(stride) + ",\"history\":[";
  server.sendContent(chunk);
  chunk = "";
  chunk.reserve(1400);

  bool first = true;
  for (uint16_t groupStart = 0; groupStart < sensorHistoryHeader.count; groupStart += stride) {
    uint16_t groupEnd = min(sensorHistoryHeader.count,
                            static_cast<uint16_t>(groupStart + stride));
    uint32_t epoch = 0;
    int32_t temperatureSum = 0;
    uint32_t humiditySum = 0;
    uint32_t soilPercentSum = 0;
    uint32_t soilRawSum = 0;
    uint16_t dhtCount = 0;
    uint16_t soilCount = 0;

    for (uint16_t i = groupStart; i < groupEnd; i++) {
      SensorLogEntry entry;
      if (!readSensorHistoryEntry(file, i, entry)) continue;
      if (entry.epoch > 0) epoch = entry.epoch;
      if (entry.flags & 0x01) {
        temperatureSum += entry.temperatureC10;
        humiditySum += entry.airHumidity10;
        dhtCount++;
      }
      if (entry.flags & 0x02) {
        soilPercentSum += entry.soilPercent;
        soilRawSum += entry.soilRaw;
        soilCount++;
      }
    }

    if (!first) chunk += ',';
    first = false;
    chunk += "{\"epoch\":" + String(epoch) + ",\"temperature\":";
    chunk += dhtCount ? String(temperatureSum / (10.0f * dhtCount), 1) : "null";
    chunk += ",\"airHumidity\":";
    chunk += dhtCount ? String(humiditySum / (10.0f * dhtCount), 1) : "null";
    chunk += ",\"soilPercent\":";
    chunk += soilCount ? String(soilPercentSum / static_cast<float>(soilCount), 1) : "null";
    chunk += ",\"soilRaw\":";
    chunk += soilCount ? String(soilRawSum / soilCount) : "null";
    chunk += '}';

    if (chunk.length() >= 1200) {
      server.sendContent(chunk);
      chunk = "";
      esp_task_wdt_reset();
    }
  }
  if (chunk.length() > 0) server.sendContent(chunk);
  server.sendContent("]}");
  server.sendContent("");
  file.close();
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
  lastScheduleEventDay = -1;
  lastScheduleEventMinute = -1;
  checkSchedule();
  sendJsonStatus();
}

void setupRoutes() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
  });
  server.on("/api/status", HTTP_GET, sendJsonStatus);
  server.on("/api/history", HTTP_GET, sendJsonHistory);
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
  analogSetPinAttenuation(SOIL_SENSOR_PIN, ADC_11db);
  dht.begin();
  loadSettings();
  loadSensorHistory();
  connectWiFi();
  setupTime();
  setupRoutes();
  server.begin();

  lastSensorStep = millis() - SENSOR_STEP_INTERVAL_MS;
  lastSensorLog = millis();
  lastWeatherFetch = millis();
  weatherFetchInterval = WEATHER_INITIAL_DELAY_MS;
}

void loop() {
  esp_task_wdt_reset();
  server.handleClient();

  unsigned long nowMs = millis();
  if (WiFi.status() != WL_CONNECTED && nowMs - lastWiFiReconnectAttempt >= WIFI_RECONNECT_INTERVAL_MS) {
    lastWiFiReconnectAttempt = nowMs;
    WiFi.reconnect();
  }

  updateSensors();
  updateSensorHistory();

  if (nowMs - lastWeatherFetch >= weatherFetchInterval) {
    lastWeatherFetch = nowMs;
    fetchWeather();
    weatherFetchInterval = weather.valid ? WEATHER_SUCCESS_INTERVAL_MS : WEATHER_RETRY_INTERVAL_MS;
  }

  if (nowMs - lastScheduleCheck >= SCHEDULE_INTERVAL_MS) {
    lastScheduleCheck = nowMs;
    checkSchedule();
  }
}
