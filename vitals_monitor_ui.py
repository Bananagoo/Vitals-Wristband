import tkinter as tk
from tkinter import ttk, scrolledtext
import serial
import serial.tools.list_ports
import threading
import time
from datetime import datetime

# ── Helpers ────────────────────────────────────────────────────────────────────

def parse_line(line):
    """Parse 'BPM:72,RR:14,SPO2:0,BP:0' → dict or None on failure."""
    try:
        parts = {}
        for item in line.strip().split(","):
            k, v = item.split(":")
            parts[k.strip()] = int(v.strip())
        return {
            "bpm":  parts["BPM"],
            "rr":   parts["RR"],
            "spo2": parts["SPO2"],
            "bp":   parts["BP"],
            "ts":   time.time(),   # wall-clock timestamp for duration calculations
        }
    except Exception:
        return None   # silently discard malformed lines (startup noise, partial reads)


def rising_edges(log, key, trigger_val, normal_val=0):
    """Count transitions from normal_val → trigger_val in a log list.

    Counts discrete events rather than total samples in alarm state —
    e.g. one SpO2 episode = 1 regardless of how long it lasted.
    """
    count = 0
    for i in range(1, len(log)):
        if log[i][key] == trigger_val and log[i - 1][key] == normal_val:
            count += 1
    return count


def systemic_state(entry):
    # Thresholds mirror the Arduino firmware exactly so report classification
    # matches what the hardware displayed during the session.
    bpm, rr = entry["bpm"], entry["rr"]
    hr_lo = bpm < 50    # includes 0 BPM
    hr_hi = bpm > 110
    rr_lo = rr < 12     # includes 0 RPM
    rr_hi = rr > 24
    if hr_lo and rr_lo:
        return "depression"
    if hr_hi and rr_hi:
        return "excitation"
    return "normal"


def generate_report_text(log):
    if not log:
        return "No data recorded."

    duration = log[-1]["ts"] - log[0]["ts"]
    mins, secs = int(duration // 60), int(duration % 60)
    total = len(log)

    # Include 0 values — a 0 BPM/RPM reading is a valid measurement, not missing data
    bpms = [e["bpm"] for e in log]
    rrs  = [e["rr"]  for e in log]
    final_bpm = log[-1]["bpm"]
    final_rr  = log[-1]["rr"]

    spo2_events  = rising_edges(log, "spo2", 1, 0)
    bp_lo_events = rising_edges(log, "bp",   1, 0)
    bp_hi_events = rising_edges(log, "bp",   2, 0)

    # Count transitions into each systemic state using a prev-state tracker
    dep_events = exc_events = 0
    prev = "normal"
    for e in log:
        s = systemic_state(e)
        if s == "depression" and prev != "depression":
            dep_events += 1
        if s == "excitation" and prev != "excitation":
            exc_events += 1
        prev = s

    # Time-in-state: fraction of samples in state × total duration → minutes
    # This assumes uniform sampling rate, which holds since the Arduino sends
    # one line every ~500 ms regardless of vital sign values.
    spo2_mins  = sum(1 for e in log if e["spo2"] == 1) / total * duration / 60
    bp_lo_mins = sum(1 for e in log if e["bp"]   == 1) / total * duration / 60
    bp_hi_mins = sum(1 for e in log if e["bp"]   == 2) / total * duration / 60
    bp_ok_mins = duration / 60 - bp_lo_mins - bp_hi_mins

    start_str = datetime.fromtimestamp(log[0]["ts"]).strftime("%H:%M:%S")
    end_str   = datetime.fromtimestamp(log[-1]["ts"]).strftime("%H:%M:%S")

    lines = [
        "=" * 44,
        "            VITALS REPORT",
        "=" * 44,
        f"  Start:    {start_str}",
        f"  End:      {end_str}",
        f"  Duration: {mins} min {secs} s",
        "",
        "── Heart Rate ──────────────────────────",
    ]
    lines += [
        f"  Current:  {final_bpm} BPM",
        f"  Min:      {min(bpms)} BPM",
        f"  Max:      {max(bpms)} BPM",
        f"  Avg:      {sum(bpms) // len(bpms)} BPM",
    ]

    lines += [
        "",
        "── Respiratory Rate ────────────────────",
        f"  Current:  {final_rr} RPM",
        f"  Min:      {min(rrs)} RPM",
        f"  Max:      {max(rrs)} RPM",
        f"  Avg:      {sum(rrs) // len(rrs)} RPM",
    ]

    lines += [
        "",
        "── SpO2 ────────────────────────────────",
        f"  Time below 95%:  {spo2_mins:.1f} min",
        f"  Low events:      {spo2_events}",
        "",
        "── Blood Pressure ──────────────────────",
        f"  Time normal:     {bp_ok_mins:.1f} min",
        f"  Time low  (<90): {bp_lo_mins:.1f} min  ({bp_lo_events} events)",
        f"  Time high (>140):{bp_hi_mins:.1f} min  ({bp_hi_events} events)",
        "",
        "── Systemic Alarms ─────────────────────",
        f"  Depression episodes: {dep_events}",
        f"  Excitation episodes: {exc_events}",
        "",
        "=" * 44,
    ]
    return "\n".join(lines)


# ── Main UI ────────────────────────────────────────────────────────────────────

class VitalsApp:
    def __init__(self, root):
        self.root = root
        self.root.title("Vitals Monitor")
        self.root.resizable(True, True)
        self.root.geometry("800x600")
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(3, weight=1)   # report area expands with window

        self.ser        = None    # pyserial Serial object, None when disconnected
        self.running    = False   # flag that drives the serial read loop
        self.recording  = False   # when True, entries are appended to self.log
        self.log        = []      # list of sample dicts for the current session
        self._thread    = None    # daemon thread running _read_loop

        self._build_ui()
        self.refresh_ports()

    # ── UI construction ────────────────────────────────────────────────────────

    def _build_ui(self):
        PAD = {"padx": 8, "pady": 4}

        # ── Connection row ──
        conn = ttk.LabelFrame(self.root, text="Connection")
        conn.grid(row=0, column=0, columnspan=2, sticky="ew", **PAD)

        ttk.Label(conn, text="Port:").pack(side="left", padx=4)
        self.port_var = tk.StringVar()
        self.port_cb  = ttk.Combobox(conn, textvariable=self.port_var, width=14, state="readonly")
        self.port_cb.pack(side="left", padx=4)

        ttk.Button(conn, text="Refresh",    command=self.refresh_ports).pack(side="left", padx=2)
        self.conn_btn = ttk.Button(conn, text="Connect", command=self.toggle_connect)
        self.conn_btn.pack(side="left", padx=2)

        self.conn_lbl = ttk.Label(conn, text="● Disconnected", foreground="gray")
        self.conn_lbl.pack(side="left", padx=8)

        # ── Live vitals row ──
        live = ttk.LabelFrame(self.root, text="Live Vitals")
        live.grid(row=1, column=0, columnspan=2, sticky="ew", **PAD)

        self.hr_lbl   = ttk.Label(live, text="HR  —",   font=("Courier", 16, "bold"), width=18)
        self.rr_lbl   = ttk.Label(live, text="RR  —",   font=("Courier", 16, "bold"), width=18)
        self.spo2_lbl = ttk.Label(live, text="SpO2 —",  font=("Courier", 16, "bold"), width=18)
        self.bp_lbl   = ttk.Label(live, text="BP   —",  font=("Courier", 16, "bold"), width=25)

        for w in (self.hr_lbl, self.rr_lbl, self.spo2_lbl, self.bp_lbl):
            w.pack(side="left", padx=16, pady=8)

        # ── Controls row ──
        ctrl = ttk.Frame(self.root)
        ctrl.grid(row=2, column=0, columnspan=2, sticky="ew", **PAD)

        self.rec_btn    = ttk.Button(ctrl, text="⏺  Record",          command=self.start_recording, state="disabled")
        self.stop_btn   = ttk.Button(ctrl, text="⏹  Stop",            command=self.stop_recording,  state="disabled")
        self.report_btn = ttk.Button(ctrl, text="Generate Report", command=self.show_report,     state="disabled")

        self.rec_btn.pack(side="left", padx=4)
        self.stop_btn.pack(side="left", padx=4)
        self.report_btn.pack(side="left", padx=4)

        self.rec_lbl = ttk.Label(ctrl, text="")
        self.rec_lbl.pack(side="left", padx=10)

        # ── Report text area ──
        rpt = ttk.LabelFrame(self.root, text="Report")
        rpt.grid(row=3, column=0, columnspan=2, sticky="nsew", **PAD)

        self.report_txt = scrolledtext.ScrolledText(
            rpt, font=("Courier", 15), state="disabled"
        )
        self.report_txt.pack(fill="both", expand=True, padx=4, pady=4)

        rpt.bind("<Configure>", self.resize_text)

    def resize_text(self, event):
        # scale font based on window width
        new_size = max(10, int(event.width / 40))
        self.report_txt.configure(font=("Courier", new_size))

    # ── Port management ────────────────────────────────────────────────────────

    def refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_cb["values"] = ports
        if ports:
            self.port_var.set(ports[0])

    def toggle_connect(self):
        if self.ser and self.ser.is_open:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        try:
            # timeout=1 prevents readline() from blocking forever if the Arduino
            # stops sending — the read loop will retry on the next iteration
            self.ser     = serial.Serial(self.port_var.get(), 9600, timeout=1)
            self.running = True
            # daemon=True: thread is killed automatically when the app exits,
            # so the process never hangs waiting for a final readline()
            self._thread = threading.Thread(target=self._read_loop, daemon=True)
            self._thread.start()
            self.conn_lbl.config(text="● Connected", foreground="green")
            self.conn_btn.config(text="Disconnect")
            self.rec_btn.config(state="normal")
        except Exception as e:
            self.conn_lbl.config(text=f"Error: {e}", foreground="red")

    def _disconnect(self):
        self.running = False          # signals the read loop to exit
        if self._thread:
            self._thread.join(timeout=2)   # wait up to one readline timeout + margin
        if self.ser:
            self.ser.close()
            self.ser = None
        self.conn_lbl.config(text="● Disconnected", foreground="gray")
        self.conn_btn.config(text="Connect")
        self.rec_btn.config(state="disabled")
        self.stop_btn.config(state="disabled")

    # ── Serial reading thread ──────────────────────────────────────────────────

    def _read_loop(self):
        # Runs on the daemon thread. readline() blocks until \n arrives or timeout.
        # list.append is GIL-atomic in CPython, so no explicit lock is needed.
        while self.running:
            try:
                raw = self.ser.readline().decode("utf-8", errors="ignore")
                entry = parse_line(raw)
                if entry:
                    if self.recording:
                        self.log.append(entry)
                    # root.after(0, ...) posts the UI update to tkinter's event queue
                    # so it executes on the main thread — tkinter is not thread-safe
                    self.root.after(0, self._update_live, entry)
            except Exception:
                break

    # ── Live display update (runs on main thread via after()) ─────────────────

    def _update_live(self, e):
        bpm, rr, spo2, bp = e["bpm"], e["rr"], e["spo2"], e["bp"]

        # Colour coding: red = high/above range, blue = low/below range (incl. 0), black = normal
        hr_color = "red" if (bpm > 110) else "blue" if (bpm < 50) else "black"
        rr_color = "red" if (rr > 24) else "blue" if (rr < 12) else "black"
        sp_color = "blue" if spo2 else "black"
        bp_color = "blue" if bp == 1 else "red" if bp == 2 else "black"

        sp_str = "Low (<95%)" if spo2 else "Normal (≥95%)"
        bp_map = {0: "Normal", 1: "Low (<90 mmHg)", 2: "High (>140 mmHg)"}
        bp_str = bp_map.get(bp, "?")

        self.hr_lbl.config(  text=f"HR   {bpm} BPM", foreground=hr_color)
        self.rr_lbl.config(  text=f"RR   {rr} RPM",  foreground=rr_color)
        self.spo2_lbl.config(text=f"SpO2 {sp_str}",  foreground=sp_color)
        self.bp_lbl.config(  text=f"BP   {bp_str}",  foreground=bp_color)

    # ── Recording controls ─────────────────────────────────────────────────────

    def start_recording(self):
        self.log       = []     # clear any previous session
        self.recording = True
        self.rec_btn.config(state="disabled")
        self.stop_btn.config(state="normal")
        self.report_btn.config(state="disabled")
        self.rec_lbl.config(text="⏺ Recording…", foreground="red")

    def stop_recording(self):
        self.recording = False
        n = len(self.log)
        self.stop_btn.config(state="disabled")
        self.rec_btn.config(state="normal")
        self.rec_lbl.config(text="Stopped", foreground="black")
        if n > 0:
            self.report_btn.config(state="normal")

    # ── Report ─────────────────────────────────────────────────────────────────

    def show_report(self):
        text = generate_report_text(self.log)
        self.report_txt.config(state="normal")
        self.report_txt.delete("1.0", tk.END)
        self.report_txt.insert(tk.END, text)
        self.report_txt.config(state="disabled")   # prevent user editing


# ── Entry point ────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    root = tk.Tk()
    app  = VitalsApp(root)
    root.mainloop()
