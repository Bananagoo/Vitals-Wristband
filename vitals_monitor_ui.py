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
            "ts":   time.time(),
        }
    except Exception:
        return None


def rising_edges(log, key, trigger_val, normal_val=0):
    """Count transitions from normal_val → trigger_val in a log list."""
    count = 0
    for i in range(1, len(log)):
        if log[i][key] == trigger_val and log[i - 1][key] == normal_val:
            count += 1
    return count


def systemic_state(entry):
    bpm, rr = entry["bpm"], entry["rr"]
    hr_lo = 0 < bpm < 50
    hr_hi = bpm > 110
    rr_lo = 0 < rr < 8
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

    bpms = [e["bpm"] for e in log if e["bpm"] > 0]
    rrs  = [e["rr"]  for e in log if e["rr"]  > 0]

    spo2_events  = rising_edges(log, "spo2", 1, 0)
    bp_lo_events = rising_edges(log, "bp",   1, 0)
    bp_hi_events = rising_edges(log, "bp",   2, 0)

    dep_events = exc_events = 0
    prev = "normal"
    for e in log:
        s = systemic_state(e)
        if s == "depression" and prev != "depression":
            dep_events += 1
        if s == "excitation" and prev != "excitation":
            exc_events += 1
        prev = s

    spo2_pct  = sum(1 for e in log if e["spo2"] == 1) / total * 100
    bp_lo_pct = sum(1 for e in log if e["bp"]   == 1) / total * 100
    bp_hi_pct = sum(1 for e in log if e["bp"]   == 2) / total * 100

    start_str = datetime.fromtimestamp(log[0]["ts"]).strftime("%H:%M:%S")
    end_str   = datetime.fromtimestamp(log[-1]["ts"]).strftime("%H:%M:%S")

    lines = [
        "=" * 44,
        "            VITALS REPORT",
        "=" * 44,
        f"  Start:    {start_str}",
        f"  End:      {end_str}",
        f"  Duration: {mins} min {secs} s",
        f"  Samples:  {total}",
        "",
        "── Heart Rate ──────────────────────────",
    ]
    if bpms:
        lines += [
            f"  Current:  {bpms[-1]} BPM",
            f"  Min:      {min(bpms)} BPM",
            f"  Max:      {max(bpms)} BPM",
            f"  Avg:      {sum(bpms) // len(bpms)} BPM",
            f"  Range:    50 – 110 BPM",
        ]
    else:
        lines.append("  No HR data")

    lines += [
        "",
        "── Respiratory Rate ────────────────────",
    ]
    if rrs:
        lines += [
            f"  Current:  {rrs[-1]} RPM",
            f"  Min:      {min(rrs)} RPM",
            f"  Max:      {max(rrs)} RPM",
            f"  Avg:      {sum(rrs) // len(rrs)} RPM",
            f"  Range:    8 – 24 RPM",
        ]
    else:
        lines.append("  No RR data")

    lines += [
        "",
        "── SpO2 ────────────────────────────────",
        f"  Time below 95%:  {spo2_pct:.1f}%",
        f"  Low events:      {spo2_events}",
        "",
        "── Blood Pressure ──────────────────────",
        f"  Time normal:     {100 - bp_lo_pct - bp_hi_pct:.1f}%",
        f"  Time low  (<90): {bp_lo_pct:.1f}%  ({bp_lo_events} events)",
        f"  Time high (>140):{bp_hi_pct:.1f}%  ({bp_hi_events} events)",
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
        self.root.resizable(False, False)

        self.ser        = None
        self.running    = False
        self.recording  = False
        self.log        = []
        self._thread    = None

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

        self.hr_lbl   = ttk.Label(live, text="HR  —",   font=("Courier", 18, "bold"), width=10)
        self.rr_lbl   = ttk.Label(live, text="RR  —",   font=("Courier", 18, "bold"), width=10)
        self.spo2_lbl = ttk.Label(live, text="SpO2 —",  font=("Courier", 14),         width=12)
        self.bp_lbl   = ttk.Label(live, text="BP   —",  font=("Courier", 14),         width=14)

        for w in (self.hr_lbl, self.rr_lbl, self.spo2_lbl, self.bp_lbl):
            w.pack(side="left", padx=16, pady=8)

        # ── Controls row ──
        ctrl = ttk.Frame(self.root)
        ctrl.grid(row=2, column=0, columnspan=2, sticky="w", **PAD)

        self.rec_btn    = ttk.Button(ctrl, text="⏺  Record",          command=self.start_recording, state="disabled")
        self.stop_btn   = ttk.Button(ctrl, text="⏹  Stop",            command=self.stop_recording,  state="disabled")
        self.report_btn = ttk.Button(ctrl, text="📋  Generate Report", command=self.show_report,     state="disabled")

        self.rec_btn.pack(side="left", padx=4)
        self.stop_btn.pack(side="left", padx=4)
        self.report_btn.pack(side="left", padx=4)

        self.rec_lbl = ttk.Label(ctrl, text="")
        self.rec_lbl.pack(side="left", padx=10)

        # ── Report text area ──
        rpt = ttk.LabelFrame(self.root, text="Report")
        rpt.grid(row=3, column=0, columnspan=2, sticky="nsew", **PAD)

        self.report_txt = scrolledtext.ScrolledText(
            rpt, width=52, height=24, font=("Courier", 10), state="disabled"
        )
        self.report_txt.pack(fill="both", expand=True, padx=4, pady=4)

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
            self.ser     = serial.Serial(self.port_var.get(), 9600, timeout=1)
            self.running = True
            self._thread = threading.Thread(target=self._read_loop, daemon=True)
            self._thread.start()
            self.conn_lbl.config(text="● Connected", foreground="green")
            self.conn_btn.config(text="Disconnect")
            self.rec_btn.config(state="normal")
        except Exception as e:
            self.conn_lbl.config(text=f"Error: {e}", foreground="red")

    def _disconnect(self):
        self.running = False
        if self._thread:
            self._thread.join(timeout=2)
        if self.ser:
            self.ser.close()
            self.ser = None
        self.conn_lbl.config(text="● Disconnected", foreground="gray")
        self.conn_btn.config(text="Connect")
        self.rec_btn.config(state="disabled")
        self.stop_btn.config(state="disabled")

    # ── Serial reading thread ──────────────────────────────────────────────────

    def _read_loop(self):
        while self.running:
            try:
                raw = self.ser.readline().decode("utf-8", errors="ignore")
                entry = parse_line(raw)
                if entry:
                    if self.recording:
                        self.log.append(entry)
                    self.root.after(0, self._update_live, entry)
            except Exception:
                break

    # ── Live display update (runs on main thread via after()) ─────────────────

    def _update_live(self, e):
        bpm, rr, spo2, bp = e["bpm"], e["rr"], e["spo2"], e["bp"]

        hr_color = "red" if (bpm > 0 and (bpm < 50 or bpm > 110)) else "black"
        rr_color = "red" if (rr  > 0 and (rr  < 8  or rr  > 24))  else "black"
        sp_color = "red"  if spo2 else "black"
        bp_color = "red"  if bp   else "black"

        sp_str = "LOW (<95%)" if spo2 else "OK (≥95%)"
        bp_map = {0: "Normal", 1: "Low (<90)", 2: "High (>140)"}
        bp_str = bp_map.get(bp, "?")

        self.hr_lbl.config(  text=f"HR   {bpm} BPM", foreground=hr_color)
        self.rr_lbl.config(  text=f"RR   {rr} RPM",  foreground=rr_color)
        self.spo2_lbl.config(text=f"SpO2 {sp_str}",  foreground=sp_color)
        self.bp_lbl.config(  text=f"BP   {bp_str}",  foreground=bp_color)

    # ── Recording controls ─────────────────────────────────────────────────────

    def start_recording(self):
        self.log       = []
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
        self.rec_lbl.config(text=f"Stopped  ({n} samples)", foreground="black")
        if n > 0:
            self.report_btn.config(state="normal")

    # ── Report ─────────────────────────────────────────────────────────────────

    def show_report(self):
        text = generate_report_text(self.log)
        self.report_txt.config(state="normal")
        self.report_txt.delete("1.0", tk.END)
        self.report_txt.insert(tk.END, text)
        self.report_txt.config(state="disabled")


# ── Entry point ────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    root = tk.Tk()
    app  = VitalsApp(root)
    root.mainloop()
