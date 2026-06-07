#include <WiFi.h>
#include <WiFiUDP.h>

// ─── CONFIG ───────────────────────────────────────────────
const char* AP_SSID     = "CFR-Timing";
const char* AP_PASSWORD = "cfracing25";

const int   LDR_PIN     = 4;       // GPIO pin connected to LDR receiver signal
const int   NODE_ID     = 1;       // 1 = start/finish, 2 = second node (accel)

const int   UDP_PORT    = 5005;    // must match Python server
IPAddress   LAPTOP_IP(192, 168, 4, 2);  // laptop gets this IP when it connects to AP

// debounce: ignore re-triggers within this many milliseconds
const unsigned long DEBOUNCE_MS = 500;
// ──────────────────────────────────────────────────────────

WiFiUDP udp;
bool    lastBeamState  = HIGH;   // HIGH = beam intact
unsigned long lastTriggerMs = 0;

void setup() {
  Serial.begin(115200);
  pinMode(LDR_PIN, INPUT);

  // Start access point
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("AP started — IP: ");
  Serial.println(WiFi.softAPIP());

  udp.begin(UDP_PORT);
  Serial.println("UDP ready");
  Serial.print("Node ID: ");
  Serial.println(NODE_ID);
}

void loop() {
  bool beamState = digitalRead(LDR_PIN);

  // Trigger on falling edge (beam breaks: HIGH → LOW)
  if (lastBeamState == HIGH && beamState == LOW) {
    unsigned long now = millis();

    // Debounce — ignore if triggered too recently
    if (now - lastTriggerMs > DEBOUNCE_MS) {
      lastTriggerMs = now;
      sendTrigger(now);
    }
  }

  lastBeamState = beamState;
  delay(1); // tight loop, 1ms resolution
}

void sendTrigger(unsigned long timestamp) {
  // Packet format: "TRIGGER,<node_id>,<timestamp_ms>"
  char packet[64];
  snprintf(packet, sizeof(packet), "TRIGGER,%d,%lu", NODE_ID, timestamp);

  udp.beginPacket(LAPTOP_IP, UDP_PORT);
  udp.print(packet);
  udp.endPacket();

  Serial.print("Sent: ");
  Serial.println(packet);
}
