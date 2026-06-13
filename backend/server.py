import socket
import threading
import time
import os
from datetime import datetime
from flask import Flask, render_template, jsonify, request, Response
from flask_socketio import SocketIO

# ─── CONFIG ───────────────────────────────────────────────
UDP_IP     = "0.0.0.0"
UDP_PORT   = 5005
FLASK_PORT = 5000
LOG_DIR    = os.path.join(os.path.dirname(__file__), "..", "logs")
# ──────────────────────────────────────────────────────────

app = Flask(__name__, template_folder="../frontend")
io  = SocketIO(app, cors_allowed_origins="*")

lock  = threading.Lock()
state = {
    "mode":           "lap",      # "lap" | "accel"
    "armed":          False,
    "laps":           [],
    "runs":           [],
    "drivers":        [],         # list of driver names
    "active_driver":  None,       # current driver name or None
    "_lap_start":     None,
    "_run_start":     None,
}

def handle_trigger(node_id: int, esp_ms: int):
    """All timing uses the laptop wall clock at packet arrival."""
    wall_ts = time.time()
    with lock:
        if not state["armed"]:
            return
        if state["mode"] == "lap":
            _handle_lap(wall_ts)
        elif state["mode"] == "accel":
            _handle_accel(node_id, wall_ts)

def _handle_lap(wall_ts: float):
    if state["_lap_start"] is None:
        state["_lap_start"] = wall_ts
        io.emit("event", {"type": "lap_started"})
    else:
        lap_time = wall_ts - state["_lap_start"]
        if lap_time <= 0:
            return
        entry = {
            "lap":      len(state["laps"]) + 1,
            "driver":   state["active_driver"] or "—",
            "time_s":   round(lap_time, 3),
            "time_fmt": fmt_time(lap_time),
            "ts":       round(wall_ts, 3),
        }
        state["laps"].append(entry)
        state["_lap_start"] = wall_ts
        io.emit("event", {"type": "lap", "data": entry})

def _handle_accel(node_id: int, wall_ts: float):
    if node_id == 1:
        state["_run_start"] = wall_ts
        io.emit("event", {"type": "run_started"})
    elif node_id == 2 and state["_run_start"] is not None:
        run_time = wall_ts - state["_run_start"]
        if run_time <= 0:
            return
        entry = {
            "run":      len(state["runs"]) + 1,
            "driver":   state["active_driver"] or "—",
            "time_s":   round(run_time, 3),
            "time_fmt": fmt_time(run_time),
            "ts":       round(wall_ts, 3),
        }
        state["runs"].append(entry)
        state["_run_start"] = None
        io.emit("event", {"type": "run", "data": entry})

def fmt_time(seconds: float) -> str:
    m  = int(seconds // 60)
    s  = int(seconds % 60)
    ms = int((seconds - int(seconds)) * 1000)
    return f"{m}:{s:02d}.{ms:03d}"

# ─── UDP LISTENER ─────────────────────────────────────────
def udp_listener():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((UDP_IP, UDP_PORT))
    print(f"[UDP] Listening on port {UDP_PORT}")
    while True:
        data, addr = sock.recvfrom(128)
        raw = data.decode("utf-8").strip()
        print(f"[UDP] {addr} → {raw}")
        try:
            parts = raw.split(",")
            if parts[0] == "TRIGGER" and len(parts) == 3:
                handle_trigger(int(parts[1]), int(parts[2]))
        except Exception as e:
            print(f"[UDP] Parse error: {e}")

# ─── ROUTES ───────────────────────────────────────────────
@app.route("/")
def index():
    return render_template("index.html")

@app.route("/api/state")
def api_state():
    with lock:
        return jsonify({
            "mode":          state["mode"],
            "armed":         state["armed"],
            "laps":          state["laps"],
            "runs":          state["runs"],
            "drivers":       state["drivers"],
            "active_driver": state["active_driver"],
        })

@app.route("/api/arm", methods=["POST"])
def api_arm():
    with lock:
        state["armed"] = True
    io.emit("event", {"type": "armed"})
    return jsonify({"armed": True})

@app.route("/api/disarm", methods=["POST"])
def api_disarm():
    with lock:
        state["armed"]      = False
        state["_lap_start"] = None   # next arm starts a fresh lap
        state["_run_start"] = None
    io.emit("event", {"type": "disarmed"})
    return jsonify({"armed": False})

@app.route("/api/mode", methods=["POST"])
def api_mode():
    mode = request.json.get("mode")
    if mode not in ("lap", "accel"):
        return jsonify({"error": "invalid mode"}), 400
    with lock:
        state["mode"]       = mode
        state["armed"]      = False
        state["_lap_start"] = None
        state["_run_start"] = None
    io.emit("event", {"type": "mode_changed", "mode": mode})
    return jsonify({"mode": mode})

@app.route("/api/driver/add", methods=["POST"])
def api_driver_add():
    name = (request.json.get("name") or "").strip()
    if not name:
        return jsonify({"error": "empty name"}), 400
    with lock:
        if name not in state["drivers"]:
            state["drivers"].append(name)
        # auto-select if first driver
        if state["active_driver"] is None:
            state["active_driver"] = name
    io.emit("event", {"type": "drivers_changed",
                      "drivers": state["drivers"],
                      "active_driver": state["active_driver"]})
    return jsonify({"drivers": state["drivers"], "active_driver": state["active_driver"]})

@app.route("/api/driver/select", methods=["POST"])
def api_driver_select():
    name = request.json.get("name")
    with lock:
        if name not in state["drivers"]:
            return jsonify({"error": "unknown driver"}), 400
        state["active_driver"] = name
    io.emit("event", {"type": "drivers_changed",
                      "drivers": state["drivers"],
                      "active_driver": state["active_driver"]})
    return jsonify({"active_driver": name})

@app.route("/api/driver/remove", methods=["POST"])
def api_driver_remove():
    name = request.json.get("name")
    with lock:
        if name in state["drivers"]:
            state["drivers"].remove(name)
        if state["active_driver"] == name:
            state["active_driver"] = state["drivers"][0] if state["drivers"] else None
    io.emit("event", {"type": "drivers_changed",
                      "drivers": state["drivers"],
                      "active_driver": state["active_driver"]})
    return jsonify({"drivers": state["drivers"], "active_driver": state["active_driver"]})

@app.route("/api/reset", methods=["POST"])
def api_reset():
    with lock:
        state["laps"]       = []
        state["runs"]       = []
        state["_lap_start"] = None
        state["_run_start"] = None
    io.emit("event", {"type": "reset"})
    return jsonify({"ok": True})

@app.route("/api/export")
def api_export():
    os.makedirs(LOG_DIR, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    with lock:
        mode = state["mode"]
        rows = list(state["laps"] if mode == "lap" else state["runs"])
    # also save a copy server-side
    filepath = os.path.join(LOG_DIR, f"session_{mode}_{ts}.csv")
    try:
        with open(filepath, "w") as f:
            if rows:
                f.write(",".join(rows[0].keys()) + "\n")
                for row in rows:
                    f.write(",".join(str(v) for v in row.values()) + "\n")
    except Exception as e:
        print(f"[LOG] save failed: {e}")
    def generate():
        if not rows:
            yield "no data\n"
            return
        yield ",".join(rows[0].keys()) + "\n"
        for row in rows:
            yield ",".join(str(v) for v in row.values()) + "\n"
    return Response(
        generate(),
        mimetype="text/csv",
        headers={"Content-Disposition": f"attachment; filename=session_{mode}_{ts}.csv"}
    )

# ─── MAIN ─────────────────────────────────────────────────
if __name__ == "__main__":
    threading.Thread(target=udp_listener, daemon=True).start()
    print(f"[WEB] Open http://localhost:{FLASK_PORT}")
    io.run(app, host="0.0.0.0", port=FLASK_PORT, debug=False)
