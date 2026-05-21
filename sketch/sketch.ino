/*
 * PetScale — Balança IoT Autônoma para Pets
 * Challenge FIAP 2026 — CLYVO VET
 *
 * Hardware (Wokwi):
 *   - ESP32 DevKit V1
 *   - HX711 + Célula de carga 50kg
 *   - Display OLED SSD1306 128x64 (I2C)
 *
 * Conexões (ajuste se necessário):
 *   HX711  DT  → GPIO 4
 *   HX711  SCK → GPIO 5
 *   OLED   SDA → GPIO 21
 *   OLED   SCL → GPIO 22
 *
 * Rotas:
 *   GET /            -> dashboard
 *   GET /data        -> JSON
 *   GET /tare        -> faz tara e zera histórico
 *   GET /calibrate?kg=5.2 -> calibra com peso conhecido e zera histórico
 */

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <HX711.h>

// ─── WiFi ─────────────────────────────────────────────────────────────────────
const char* SSID     = "Wokwi-GUEST";
const char* PASSWORD = "";
#define WIFI_CHANNEL 6

// ─── HX711 ────────────────────────────────────────────────────────────────────
#define HX711_DT  4
#define HX711_SCK 5

HX711 scale;

// Começa neutro; depois /calibrate?kg=...
float calibrationFactor = 1.0f;

// ─── OLED ─────────────────────────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ─── Servidor web (sync) ─────────────────────────────────────────────────────
WebServer server(80);

// ─── Histórico de pesagens ─────────────────────────────────────────────────────
#define MAX_RECORDS 50

struct WeightRecord {
  float    weight;
  uint32_t timestamp_s;
};

WeightRecord records[MAX_RECORDS];
int   recordCount   = 0;
float currentWeight = 0.0;
float lastSaved     = -99.0;

// Limites para célula de 50kg
const float WEIGHT_MIN       = 0.3;   // kg — abaixo disso ignora (ruído)
const float WEIGHT_MAX       = 50.0;  // kg — limite da célula
const float CHANGE_THRESHOLD = 0.3;   // kg — salva se mudou mais que isso
const float ALERT_THRESHOLD  = 5.0;   // kg — alerta se variação brusca

SemaphoreHandle_t dataMutex;

// ─────────────────────────────────────────────────────────────────────────────
// Dashboard HTML (sem dependências externas)
// ─────────────────────────────────────────────────────────────────────────────
const char DASHBOARD[] PROGMEM = R"html(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>PetScale — CLYVO VET</title>
  <style>
    :root {
      --bg:      #0d1117;
      --card:    #161b22;
      --accent:  #58a6ff;
      --danger:  #f85149;
      --ok:      #3fb950;
      --text:    #e6edf3;
      --sub:     #8b949e;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: 'Segoe UI', sans-serif;
      background: var(--bg);
      color: var(--text);
      padding: 24px;
    }
    header {
      display: flex;
      align-items: center;
      gap: 12px;
      margin-bottom: 10px;
    }
    header h1 { font-size: 1.4rem; font-weight: 700; }
    header span { font-size: 1.8rem; }
    .grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 16px;
      margin-bottom: 16px;
    }
    @media (max-width: 600px) { .grid { grid-template-columns: 1fr; } }
    .card {
      background: var(--card);
      border: 1px solid #30363d;
      border-radius: 12px;
      padding: 20px;
    }
    .card h2 {
      font-size: 0.8rem;
      color: var(--sub);
      text-transform: uppercase;
      letter-spacing: .08em;
      margin-bottom: 12px;
    }
    .big-number { font-size: 3rem; font-weight: 800; color: var(--accent); line-height: 1; }
    .big-number span { font-size: 1.2rem; color: var(--sub); font-weight: 400; margin-left: 4px; }
    .status {
      display: inline-block;
      margin-top: 8px;
      padding: 4px 10px;
      border-radius: 20px;
      font-size: 0.78rem;
      font-weight: 600;
    }
    .status.ok     { background: rgba(63,185,80,.15); color: var(--ok); }
    .status.danger { background: rgba(248,81,73,.15);  color: var(--danger); }
    .stat-row {
      display: flex;
      justify-content: space-between;
      margin-top: 8px;
      font-size: 0.88rem;
      color: var(--sub);
    }
    .stat-row strong { color: var(--text); }
    .chart-card { grid-column: 1 / -1; }
    .gauge-wrap { display: flex; align-items: center; gap: 12px; margin-top: 12px; }
    .gauge-bar {
      flex: 1;
      height: 10px;
      background: #21262d;
      border-radius: 5px;
      overflow: hidden;
    }
    .gauge-fill {
      height: 100%;
      border-radius: 5px;
      background: linear-gradient(90deg, var(--ok), var(--accent));
      transition: width .5s ease;
    }
    .gauge-label { font-size: 0.8rem; color: var(--sub); white-space: nowrap; }
    table { width: 100%; border-collapse: collapse; font-size: 0.85rem; margin-top: 8px; }
    th {
      text-align: left;
      color: var(--sub);
      font-weight: 500;
      padding: 6px 0;
      border-bottom: 1px solid #30363d;
    }
    td { padding: 8px 0; border-bottom: 1px solid #21262d; }
    td.up   { color: var(--danger); }
    td.down { color: var(--ok); }
    td.eq   { color: var(--sub); }
    .dot {
      width: 8px; height: 8px;
      border-radius: 50%;
      background: var(--ok);
      display: inline-block;
      margin-right: 6px;
      animation: pulse 2s infinite;
    }
    @keyframes pulse { 0%,100%{opacity:1} 50%{opacity:.3} }
    #debug {
      margin: 8px 0 18px 0;
      color: var(--sub);
      font-size: .82rem;
    }
  </style>
</head>
<body>
  <header>
    <span>🐾</span>
    <div>
      <h1>PetScale Monitor — 50kg</h1>
      <small style="color:var(--sub)"><span class="dot"></span>Ao vivo</small>
    </div>
  </header>

  <div id="debug">Debug: iniciando...</div>

  <div class="grid">

    <div class="card">
      <h2>Peso Atual</h2>
      <div class="big-number" id="cur">--<span>kg</span></div>
      <div class="gauge-wrap">
        <div class="gauge-bar"><div class="gauge-fill" id="gauge" style="width:0%"></div></div>
        <span class="gauge-label" id="gaugeLabel">0 / 50 kg</span>
      </div>
      <div id="statusBadge" class="status ok" style="margin-top:12px">Aguardando leitura</div>
    </div>

    <div class="card">
      <h2>Estatísticas</h2>
      <div class="stat-row"><span>Máximo</span><strong id="maxW">-- kg</strong></div>
      <div class="stat-row"><span>Mínimo</span><strong id="minW">-- kg</strong></div>
      <div class="stat-row"><span>Média</span><strong id="avgW">-- kg</strong></div>
      <div class="stat-row"><span>Última variação</span><strong id="lastDiff">--</strong></div>
      <div class="stat-row"><span>Total de registros</span><strong id="total">0</strong></div>
    </div>

    <div class="card chart-card">
      <h2>Histórico de Pesagens</h2>
      <table>
        <thead>
          <tr>
            <th>#</th>
            <th>Tempo</th>
            <th>Peso</th>
            <th>Variação</th>
          </tr>
        </thead>
        <tbody id="tbody"></tbody>
      </table>
    </div>

  </div>

  <script>
    function setText(id, txt) { document.getElementById(id).textContent = txt; }

    async function update() {
      const dbg = document.getElementById('debug');
      try {
        const url = new URL('/data', window.location.href);
        url.searchParams.set('ts', Date.now());

        const r = await fetch(url.toString(), { cache: 'no-store' });
        dbg.textContent = 'HTTP ' + r.status + ' ' + r.statusText;

        if (!r.ok) return;

        const d = await r.json();
        dbg.textContent = 'OK ' + new Date().toLocaleTimeString();

        const w = Number(d.current || 0);
        document.getElementById('cur').innerHTML = w.toFixed(2) + '<span>kg</span>';
        const pct = Math.min((w / 50) * 100, 100);
        document.getElementById('gauge').style.width = pct.toFixed(1) + '%';
        setText('gaugeLabel', w.toFixed(1) + ' / 50 kg');

        const badge = document.getElementById('statusBadge');
        if (d.alert) {
          badge.textContent = 'Variação brusca detectada!';
          badge.className = 'status danger';
        } else if (w > 0.3) {
          badge.textContent = 'Pet na balança';
          badge.className = 'status ok';
        } else {
          badge.textContent = 'Aguardando pet...';
          badge.className = 'status ok';
        }

        const recs = Array.isArray(d.records) ? d.records : [];
        setText('total', String(recs.length));

        if (recs.length === 0) {
          document.getElementById('tbody').innerHTML = '';
          setText('maxW', '-- kg');
          setText('minW', '-- kg');
          setText('avgW', '-- kg');
          setText('lastDiff', '--');
          return;
        }

        const weights = recs.map(x => Number(x.weight || 0));
        setText('maxW', Math.max(...weights).toFixed(2) + ' kg');
        setText('minW', Math.min(...weights).toFixed(2) + ' kg');
        setText('avgW', (weights.reduce((a,b)=>a+b,0)/weights.length).toFixed(2) + ' kg');

        if (weights.length > 1) {
          const diff = weights[weights.length-1] - weights[weights.length-2];
          const sign = diff > 0 ? '+' : '';
          const el = document.getElementById('lastDiff');
          el.textContent = sign + diff.toFixed(2) + ' kg';
          el.style.color = diff > 0 ? '#f85149' : diff < 0 ? '#3fb950' : '#8b949e';
        }

        const tbody = document.getElementById('tbody');
        tbody.innerHTML = '';
        for (let i = recs.length - 1; i >= 0; i--) {
          const rec  = recs[i];
          const prev = i > 0 ? Number(recs[i-1].weight || 0) : Number(rec.weight || 0);
          const diff = Number(rec.weight || 0) - prev;
          let cls = 'eq', symbol = '—';
          if (diff >  0.1) { cls = 'up';   symbol = '▲ +' + diff.toFixed(2) + ' kg'; }
          if (diff < -0.1) { cls = 'down'; symbol = '▼ '  + diff.toFixed(2) + ' kg'; }
          tbody.innerHTML += `
            <tr>
              <td>${i + 1}</td>
              <td>${rec.timestamp_s}s</td>
              <td><strong>${Number(rec.weight || 0).toFixed(2)} kg</strong></td>
              <td class="${cls}">${symbol}</td>
            </tr>`;
        }
      } catch (e) {
        dbg.textContent = 'Erro: ' + e.message;
        console.error(e);
      }
    }

    update();
    setInterval(update, 2000);
  </script>
</body>
</html>
)html";

// ─────────────────────────────────────────────────────────────────────────────
// Lê peso via HX711 com média de 5 amostras
// ─────────────────────────────────────────────────────────────────────────────
float readWeight() {
  if (!scale.is_ready()) return currentWeight;
  float w = scale.get_units(5);
  if (w < 0) w = 0;
  if (w > WEIGHT_MAX) w = WEIGHT_MAX;
  return w;
}

// ─────────────────────────────────────────────────────────────────────────────
// Histórico circular
// ─────────────────────────────────────────────────────────────────────────────
void addRecord(float weight) {
  if (recordCount >= MAX_RECORDS) {
    for (int i = 0; i < MAX_RECORDS - 1; i++) records[i] = records[i + 1];
    recordCount = MAX_RECORDS - 1;
  }
  records[recordCount].weight      = weight;
  records[recordCount].timestamp_s = millis() / 1000;
  recordCount++;
}

void resetRecordsLocked() {
  recordCount = 0;
  lastSaved = -99.0f;
  currentWeight = 0.0f;
  for (int i = 0; i < MAX_RECORDS; i++) {
    records[i].weight = 0;
    records[i].timestamp_s = 0;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Atualiza display OLED
// ─────────────────────────────────────────────────────────────────────────────
void updateOLED(float weight, bool alert) {
  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("PetScale - CLYVO VET");
  display.drawLine(0, 10, 128, 10, WHITE);

  display.setTextSize(2);
  display.setCursor(10, 16);
  display.print(weight, 1);
  display.println(" kg");

  display.setTextSize(1);
  display.setCursor(0, 40);
  display.print("Cap: 50kg [");
  int barWidth = (int)((weight / WEIGHT_MAX) * 40);
  if (barWidth < 0) barWidth = 0;
  if (barWidth > 40) barWidth = 40;
  for (int i = 0; i < 40; i++) display.print(i < barWidth ? "|" : " ");
  display.println("]");

  display.setCursor(0, 55);
  if (alert) {
    display.println("!! VARIACAO BRUSCA !!");
  } else if (weight > WEIGHT_MIN) {
    display.print("Registros: ");
    display.println(recordCount);
  } else {
    display.println("Aguardando pet...");
  }

  display.display();
}

// ─────────────────────────────────────────────────────────────────────────────
// Task do sensor — núcleo 0
// ─────────────────────────────────────────────────────────────────────────────
void sensorTask(void* param) {
  while (true) {
    float w = readWeight();

    xSemaphoreTake(dataMutex, portMAX_DELAY);

    currentWeight = w;
    bool alert = false;

    if (w > WEIGHT_MIN && fabs(w - lastSaved) > CHANGE_THRESHOLD) {
      if (recordCount > 0 && fabs(w - records[recordCount - 1].weight) > ALERT_THRESHOLD) {
        alert = true;
        Serial.printf("[ALERTA] Variacao brusca: %.2f kg\n", w);
      }
      addRecord(w);
      lastSaved = w;
      Serial.printf("[Registro] %.2f kg — %lu s\n", w, millis() / 1000);
    }

    xSemaphoreGive(dataMutex);

    updateOLED(w, alert);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Rotas do servidor web
// ─────────────────────────────────────────────────────────────────────────────
void setupServer() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", DASHBOARD);
  });

  server.on("/data", HTTP_GET, []() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);

    DynamicJsonDocument doc(8192);
    doc["current"] = currentWeight;

    bool alert = false;
    if (recordCount > 1) {
      float diff = records[recordCount - 1].weight - records[recordCount - 2].weight;
      if (fabs(diff) > ALERT_THRESHOLD) alert = true;
    }
    doc["alert"] = alert;

    JsonArray arr = doc.createNestedArray("records");
    for (int i = 0; i < recordCount; i++) {
      JsonObject o = arr.createNestedObject();
      o["weight"]      = records[i].weight;
      o["timestamp_s"] = records[i].timestamp_s;
    }

    xSemaphoreGive(dataMutex);

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  // Tara (zera histórico)
  server.on("/tare", HTTP_GET, []() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);

    scale.tare();
    resetRecordsLocked();

    xSemaphoreGive(dataMutex);

    server.send(200, "text/plain", "Tara realizada e historico zerado");
    Serial.println("[HX711] Tara via web (historico zerado)");
  });

  // Calibração: /calibrate?kg=5.2 (zera histórico)
  server.on("/calibrate", HTTP_GET, []() {
    if (!server.hasArg("kg")) {
      server.send(400, "text/plain", "Use /calibrate?kg=<peso_conhecido>");
      return;
    }

    float knownKg = server.arg("kg").toFloat();
    if (knownKg <= 0.0f) {
      server.send(400, "text/plain", "Parametro kg invalido");
      return;
    }

    xSemaphoreTake(dataMutex, portMAX_DELAY);

    // IMPORTANTE: /tare deve ser feito antes para offset correto.
    // Força escala 1 para obter raw (value) e calcular o fator.
    scale.set_scale(1.0f);
    float raw = scale.get_value(15);
    float factor = raw / knownKg;

    calibrationFactor = factor;
    scale.set_scale(calibrationFactor);

    resetRecordsLocked(); // novo: histórico zerado após calibrar

    xSemaphoreGive(dataMutex);

    Serial.printf("[CAL] known=%.3fkg raw=%.2f factor=%.2f\n", knownKg, raw, factor);

    String resp = "OK. Fator calculado: ";
    resp += String(factor, 2);
    resp += " (aplicado) e historico zerado.";
    server.send(200, "text/plain", resp);
  });

  server.begin();
  Serial.println("[Server] HTTP started");
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  dataMutex = xSemaphoreCreateMutex();

  // HX711
  scale.begin(HX711_DT, HX711_SCK);
  scale.set_scale(calibrationFactor);
  scale.tare();
  Serial.println("[HX711] Iniciado — tara realizada (scale=1.0; calibre via /calibrate?kg=...)");

  // OLED
  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[OLED] Falha ao iniciar");
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("PetScale iniciando...");
  display.display();

  // WiFi
  WiFi.begin(SSID, PASSWORD, WIFI_CHANNEL);
  Serial.print("[WiFi] Conectando");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] IP: " + WiFi.localIP().toString());
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WiFi OK!");
    display.println(WiFi.localIP().toString());
    display.println("");
    display.println("/  /tare  /data");
    display.display();
    delay(2000);
  } else {
    Serial.println("\n[WiFi] Falhou — modo offline");
  }

  setupServer();

  // Task do sensor no núcleo 0
  xTaskCreatePinnedToCore(
    sensorTask,
    "SensorTask",
    8192,
    NULL,
    1,
    NULL,
    0
  );

  Serial.println("[Sistema] Pronto!");
}

// Loop: WebServer precisa handleClient
void loop() {
  server.handleClient();
  delay(2);
}