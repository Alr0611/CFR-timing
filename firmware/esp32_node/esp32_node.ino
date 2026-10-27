#include <WiFi.h>
#include <WiFiUDP.h>

// ─── CONFIG ───────────────────────────────────────────────
const char* AP_SSID     = "CFR-Timing";
const char* AP_PASSWORD = "cfracing25";

const int   NODE_ID     = 1;       // 1 = start/finish, hosts the WiFi network
                                   // 2 = second node (accel), joins node 1's network

const int   SENSOR_PIN  = 4;       // GPIO pin connected to the receiver signal

// Level the receiver outputs while the beam is reaching it.
//   LDR module (original hardware):        HIGH
//   TSSP4038 / TSOP-style IR receiver:     LOW (pulls low while it sees the carrier)
const int   BEAM_INTACT_LEVEL = HIGH;

// Modulated IR beam. A plain LDR also sees sunlight, so on a sunny day the
// beam never reads as broken. Driving an IR LED with a 38 kHz carrier and
// using a matched receiver (e.g. Vishay TSSP4038) makes the receiver ignore
// sunlight. Set to 1 when the IR LED is wired to IR_TX_PIN (retroreflective
// layout: LED + receiver on this post, corner-cube reflector across the track).
#define USE_IR_CARRIER 0
const int      IR_TX_PIN     = 25;
const uint32_t IR_CARRIER_HZ = 38000;

const int   UDP_PORT    = 5005;    // must match Python server
// Broadcast to everyone on the CFR-Timing network, so it no longer matters
// which IP the laptop gets (a phone joining first used to steal 192.168.4.2).
IPAddress   BROADCAST_IP(192, 168, 4, 255);

// A pass only counts if the beam stays broken this long. Vibration makes the
// beam flicker for a millisecond or two; a car blocks it for ~50-150 ms.
const unsigned long MIN_BREAK_MS = 8;
// ignore re-triggers within this many milliseconds
const unsigned long DEBOUNCE_MS  = 500;

// UDP has no delivery guarantee: resend each trigger until the server ACKs it.
const unsigned long RESEND_MS    = 40;
const int           MAX_SENDS    = 10;

// Heartbeat so the UI can show "offline" or "beam blocked / misaligned".
const unsigned long STATUS_MS    = 1000;
// ──────────────────────────────────────────────────────────

WiFiUDP udp;

bool          breakPending   = false;  // beam broken, not yet long enough to count
bool          triggerLatched = false;  // already triggered for this break
unsigned long breakStartMs   = 0;
unsigned long lastTriggerMs  = 0;
unsigned long lastStatusMs   = 0;
uint32_t      nextSeq        = 1;

struct PendingTrigger {
  bool          active;
  uint32_t      seq;
  unsigned long timestamp;
  unsigned long lastSendMs;
  int           sends;
};
const int QUEUE_LEN = 4;
PendingTrigger queue[QUEUE_LEN];

void setup() {
  Serial.begin(115200);
  pinMode(SENSOR_PIN, INPUT);

#if USE_IR_CARRIER
  // 50% duty square wave at the carrier frequency
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(IR_TX_PIN, IR_CARRIER_HZ, 8);
    ledcWrite(IR_TX_PIN, 128);
  #else
    ledcSetup(0, IR_CARRIER_HZ, 8);
    ledcAttachPin(IR_TX_PIN, 0);
    ledcWrite(0, 128);
  #endif
#endif

  if (NODE_ID == 1) {
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.print("AP started — IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    // Only node 1 hosts the network; the laptop can only join one AP, so
    // node 2 must join node 1's network for its packets to reach the laptop.
    WiFi.mode(WIFI_STA);
    WiFi.begin(AP_SSID, AP_PASSWORD);
    Serial.print("Joining node 1 network");
    while (WiFi.status() != WL_CONNECTED) {
      delay(250);
      Serial.print(".");
    }
    Serial.print("\nJoined — IP: ");
    Serial.println(WiFi.localIP());
  }

  udp.begin(UDP_PORT);
  Serial.println("UDP ready");
  Serial.print("Node ID: ");
  Serial.println(NODE_ID);
}

void loop() {
  unsigned long now = millis();
  bool broken = digitalRead(SENSOR_PIN) != BEAM_INTACT_LEVEL;

  if (broken) {
    if (!breakPending && !triggerLatched) {
      breakPending = true;
      breakStartMs = now;
    }
    // Only count the break once it has lasted MIN_BREAK_MS; timestamp it at
    // the moment the beam first broke, not when the filter confirmed it.
    if (breakPending && now - breakStartMs >= MIN_BREAK_MS) {
      breakPending   = false;
      triggerLatched = true;
      if (breakStartMs - lastTriggerMs > DEBOUNCE_MS) {
        lastTriggerMs = breakStartMs;
        queueTrigger(breakStartMs);
      }
    }
  } else {
    breakPending   = false;
    triggerLatched = false;
  }

  if (NODE_ID != 1 && WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  }

  readAcks();
  resendPending(now);

  if (now - lastStatusMs >= STATUS_MS) {
    lastStatusMs = now;
    sendStatus(now, !broken);
  }

  delay(1); // tight loop, 1ms resolution
}

void queueTrigger(unsigned long timestamp) {
  // Use a free slot, or overwrite the oldest if the server has gone quiet.
  int slot = 0;
  for (int i = 0; i < QUEUE_LEN; i++) {
    if (!queue[i].active) { slot = i; break; }
    if (queue[i].seq < queue[slot].seq) slot = i;
  }
  queue[slot] = { true, nextSeq++, timestamp, 0, 0 };
  sendTrigger(queue[slot]);
}

void sendTrigger(PendingTrigger& t) {
  // Packet format: "TRIGGER,<node_id>,<timestamp_ms>,<seq>"
  char packet[64];
  snprintf(packet, sizeof(packet), "TRIGGER,%d,%lu,%lu", NODE_ID, t.timestamp, (unsigned long)t.seq);
  sendPacket(packet);
  t.lastSendMs = millis();
  t.sends++;

  Serial.print("Sent: ");
  Serial.println(packet);
}

void resendPending(unsigned long now) {
  for (int i = 0; i < QUEUE_LEN; i++) {
    if (!queue[i].active || now - queue[i].lastSendMs < RESEND_MS) continue;
    if (queue[i].sends >= MAX_SENDS) {
      Serial.print("Gave up on seq ");
      Serial.println(queue[i].seq);
      queue[i].active = false;
      continue;
    }
    sendTrigger(queue[i]);
  }
}

void readAcks() {
  // Packet format: "ACK,<node_id>,<seq>"
  while (int len = udp.parsePacket()) {
    char buf[64];
    int n = udp.read(buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = '\0';
    int node;
    unsigned long seq;
    if (sscanf(buf, "ACK,%d,%lu", &node, &seq) == 2 && node == NODE_ID) {
      for (int i = 0; i < QUEUE_LEN; i++) {
        if (queue[i].active && queue[i].seq == seq) queue[i].active = false;
      }
    }
  }
}

void sendStatus(unsigned long now, bool beamIntact) {
  // Packet format: "STATUS,<node_id>,<beam_intact 0|1>,<timestamp_ms>"
  char packet[64];
  snprintf(packet, sizeof(packet), "STATUS,%d,%d,%lu", NODE_ID, beamIntact ? 1 : 0, now);
  sendPacket(packet);
}

void sendPacket(const char* packet) {
  udp.beginPacket(BROADCAST_IP, UDP_PORT);
  udp.print(packet);
  udp.endPacket();
}
