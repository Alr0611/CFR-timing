# CFR Timing

Lap and acceleration timing system for Concordia Formula Racing (CFR).

Built with an ESP32, laser tripwire sensors, and a Python/Flask backend with a browser-based UI. No commercial transponders, no subscriptions, no external dependencies.

---

## How it works

A laser beam is stretched across the track gate. When the car breaks the beam, the ESP32 fires a UDP packet to a laptop running the Python server. The server handles all timing logic and streams results live to a web UI.

```
[Laser TX]  ·····beam·····  [Laser RX]
                                │
                            [ESP32]
                                │ WiFi (AP mode)
                            [Laptop]
                                │
                         http://localhost:5000
```

---

## Modes

**Lap** — single node at start/finish. First pass starts the timer, every subsequent pass records a lap. Used for autocross and endurance.

**Accel** — two nodes, node 1 at start, node 2 at finish. Used for acceleration and skidpad events.

---

## Hardware

| Item | Qty |
|---|---|
| ESP32 WROOM-32 DevKit V1 | 2 |
| KY-008 laser transmitter module | 2 |
| LDR laser receiver module | 2 |
| Perfboard | 4 |
| 3×AA battery holder with switch | 2 |
| USB power bank | 1 |

Laser TX side is powered by 3×AA batteries, standalone.
ESP32 + RX side is powered by a USB power bank.

---

## Wiring

**ESP32 side (per node)**

| LDR module pin | ESP32 pin |
|---|---|
| VCC | 3.3V |
| GND | GND |
| Signal | GPIO 4 |

**Laser TX side**

| KY-008 pin | Connection |
|---|---|
| VCC | Battery positive |
| GND | Battery negative |
| Signal | Not connected |

---

## Setup

### Firmware

1. Open `firmware/esp32_node/esp32_node.ino` in Arduino IDE
2. Install ESP32 board package — search `esp32 by Espressif` in board manager
3. Set board to `ESP32 Dev Module`
4. For node 1: `NODE_ID = 1` (default)
5. For node 2: change `NODE_ID = 2`
6. Flash to each ESP32

### Backend

```bash
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python backend/server.py
```

Open `http://localhost:5000` in your browser.

### On test day

1. Power up ESP32 node(s)
2. Connect laptop WiFi to `CFR-Timing` (password: `cfracing25`)
3. Run `source venv/bin/activate && python backend/server.py`
4. Open `http://localhost:5000`
5. Select mode, click **Arm Sensor**, send the car

---

## Repo structure

```
cfr-timing/
├── firmware/
│   └── esp32_node/
│       └── esp32_node.ino
├── backend/
│   └── server.py
├── frontend/
│   └── index.html
├── logs/              # auto-saved CSV exports (gitignored)
├── requirements.txt
└── README.md
```

---

## Built by

Abdulkarim Alrefae — DAQ, Concordia Formula Racing
