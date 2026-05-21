# PetScale — Balança IoT Autônoma para Pets (ESP32 + HX711 + OLED)

Projeto do **Challenge FIAP 2026 — CLYVO VET**: uma balança IoT simulada no Wokwi (rodando local no VS Code) com:

- leitura de peso via **HX711**
- exibição no **OLED SSD1306**
- dashboard web (HTML + JS) servido pelo ESP32
- endpoints para **tara** e **calibração**

---

## 1) Hardware (Wokwi)

- ESP32 DevKit V1 (esp32doit-devkit-v1)
- HX711 + célula de carga (50kg)
- OLED SSD1306 128x64 (I2C)

### Pinos (conforme o `sketch.ino`)

**HX711**

- DT → GPIO **4**
- SCK → GPIO **5**
- VCC → 3V3
- GND → GND

**OLED (I2C)**

- SDA → GPIO **21**
- SCL → GPIO **22**
- VCC → 3V3
- GND → GND

> Se você mudar o wiring no simulador, ajuste os `#define HX711_DT` e `#define HX711_SCK` no código.

---

## 2) Bibliotecas usadas

### Instalar (Wokwi `libraries.txt`)

- Adafruit GFX Library
- Adafruit SSD1306
- ArduinoJson
- HX711 (by bogde)

### Já inclusas no core ESP32 (não instala)

- WiFi
- WiFiClient
- WebServer

---

## 3) Wi‑Fi

O sketch conecta na rede do Wokwi:

- SSID: `Wokwi-GUEST`
- Password: _(vazio)_
- Channel: `6` (para acelerar a conexão)

No Serial, você verá o IP:

- `[WiFi] IP: x.x.x.x`

Esse IP também aparece no OLED.

---

## 4) Dashboard e Rotas HTTP

O servidor roda na porta **80**.

### Dashboard (HTML)

- `GET /`

### API (JSON)

- `GET /data`

Exemplo de retorno:

```json
{
  "current": 12.34,
  "alert": false,
  "records": [
    { "weight": 10.0, "timestamp_s": 12 },
    { "weight": 12.34, "timestamp_s": 20 }
  ]
}
```

### Tara (zera offset e histórico)

- `GET /tare`

Efeito:

- executa `scale.tare()`
- **zera o histórico** (records) e contadores

### Calibração (define fator de escala e zera histórico)

- `GET /calibrate?kg=<peso_conhecido>`

Exemplo:

- `GET /calibrate?kg=5.2`

Efeito:

- calcula o `calibrationFactor` a partir do peso conhecido
- aplica `scale.set_scale(calibrationFactor)`
- **zera o histórico** (records) e contadores

> Recomendação de uso: faça `/tare` com 0kg antes de calibrar.

---

## 5) Fluxo recomendado para calibrar no Wokwi

1. Rode a simulação e abra o **Serial Monitor**
2. Garanta que o peso está em **0kg**
3. Acesse: `http://<ip>/tare`
4. Ajuste o simulador para um peso conhecido (ex.: **5.2kg**)
5. Acesse: `http://<ip>/calibrate?kg=5.2`
6. Volte ao dashboard: `http://<ip>/`

No Serial aparecerá algo como:

```
[CAL] known=5.200kg raw=XXXX.XX factor=YYY.YY
```

---

## 6) Regras de registro (histórico)

O sketch só grava um registro quando:

- `weight > WEIGHT_MIN` (padrão: 0.3kg)
- e a diferença para o último salvo é maior que `CHANGE_THRESHOLD` (padrão: 0.3kg)

Alerta (`alert=true`) quando a variação entre os dois últimos registros passa de:

- `ALERT_THRESHOLD` (padrão: 5.0kg)

---

## 7) Troubleshooting rápido

### Dashboard abre, mas não atualiza

- Verifique o texto “Debug:” no topo do dashboard:
  - `OK HH:MM:SS` → está atualizando
  - `Erro: ...` → erro de fetch ou rede

### Peso não bate com o valor do simulador

- Execute o fluxo de `/tare` + `/calibrate?kg=...`
- O fator de calibração não é fixo: ele depende do ambiente/simulação

---

## 8) Arquivos do projeto

- `sketch.ino` — firmware (ESP32)
- `Wokwi.toml` — configuração do Wokwi local (VS Code)
- `README.md` — este documento
