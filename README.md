# CFR Timing

Lap and acceleration timing system.

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

Both modes tag every result with the active driver and rank a per-driver leaderboard.

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

### Backend (manual run)

```bash
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python backend/server.py
```

Open `http://localhost:5000` in your browser.

---

## Run as an always-on service (Linux / systemd)

So the server starts on boot and you never have to launch it by hand:

```bash
sudo cp deploy/cfr-timing.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable cfr-timing
sudo systemctl start cfr-timing
```

Day-to-day commands:

```bash
systemctl status cfr-timing       # check if running
sudo systemctl restart cfr-timing # reload after editing code
sudo systemctl stop cfr-timing    # turn off
sudo systemctl start cfr-timing   # turn on
```

**Note:** after editing `server.py` or `index.html`, run `sudo systemctl restart cfr-timing` so the changes take effect.

The service file assumes the repo lives at `/home/aboud/cfr-timing` and uses the venv at `venv/`. Edit `deploy/cfr-timing.service` if your paths differ.

---

## Accessing from other devices

The server binds to `0.0.0.0:5000`, so anyone on the same network can open it:

```
http://<your-laptop-ip>:5000
```

Find your IP with:

```bash
hostname -I | awk '{print $1}'
```

At the track, teammates connect to the ESP32's WiFi network and open the same address.

---

## On test day

1. Power up ESP32 node(s)
2. Connect laptop WiFi to `CFR-Timing` (password: `cfracing25`)
3. Server is already running via systemd — just open `http://localhost:5000`
4. Add drivers, pick the active one, select mode, hit **Arm**, send the car

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
├── deploy/
│   └── cfr-timing.service
├── logs/              # auto-saved CSV exports (gitignored)
├── requirements.txt
└── README.md
```

---

---

## Track reliability (2026-10 fixes)

First track tests failed for two reasons: sunlight and vibration. What changed:

- **Vibration filter.** A pass only counts if the beam stays broken for `MIN_BREAK_MS` (8 ms). Mount vibration makes the beam flicker for a millisecond or two; a car blocks it for ~50–150 ms.
- **Sunlight.** The LDR receiver also sees the sun, so on a sunny day the beam never reads as broken. The firmware can now drive a 38 kHz modulated IR beam (`USE_IR_CARRIER 1`, IR LED on `IR_TX_PIN`); pair it with a light-barrier receiver such as the Vishay TSSP4038 (set `BEAM_INTACT_LEVEL = LOW`). The receiver only responds to the carrier, so sunlight is ignored.
- **Retroreflective layout (recommended).** Put the IR LED and receiver on the same post and a corner-cube reflector across the track. A corner cube returns the beam to its source even when tilted, so vibration barely affects alignment, and one ESP32 drives both the LED and the receiver.
- **Broadcast instead of a fixed laptop IP.** Nodes broadcast to `192.168.4.255`, so it no longer matters which device joined the network first.
- **Accel mode networking.** Only node 1 hosts `CFR-Timing`; node 2 joins it. Before, both nodes hosted their own network and the laptop could only hear one.
- **Node clock timing.** The server times passes with each node's own `millis()` timestamp, aligned to the laptop clock using the smallest observed delay, instead of packet arrival time (WiFi jitter is a few ms to 100+ ms).
- **No lost passes.** Triggers carry a sequence number and are resent until the server ACKs them; the server ignores duplicates.
- **Node health in the UI.** Each node sends a heartbeat every second. The UI shows each node as `beam ok`, `beam blocked` (broken for over 2 s, likely misaligned) or `offline`.

Packet formats (UDP port 5005):

```
TRIGGER,<node_id>,<timestamp_ms>,<seq>   node → server
ACK,<node_id>,<seq>                       server → node
STATUS,<node_id>,<beam_intact 0|1>,<timestamp_ms>   node → server, every 1 s
```

The server still accepts the old 3-field `TRIGGER` packet from unflashed nodes.
