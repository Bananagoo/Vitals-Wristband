# Vitals Monitor — BME 393L Project

A wearable wristband vitals monitor built on Arduino with a Python companion app for session logging and reporting.

---

## Files

| File | Purpose |
|---|---|
| `vitals_wristband.ino` | Arduino firmware |
| `vitals_monitor_ui.py` | Python desktop companion app |

---

## Dependencies

**Arduino libraries** (install via Arduino IDE Library Manager):
- `Wire` (built-in)
- `LiquidCrystal_I2C`

**Python** (Python 3.x):
```bash
pip install pyserial
```
`tkinter` is included with the standard Python installer.

---

## Arduino Firmware

### Hardware

| Component | Detail |
|---|---|
| Microcontroller | Arduino Uno/Nano (ATmega328P, 16 MHz) |
| Display | 16×2 I2C LCD at address `0x27` |
| Respiratory RGB LED | Common cathode, pins 8–10 |
| Circulatory RGB LED | Common cathode, pins 11–13 |
| Sensor inputs | 5 digital pins (2–7), all sharing interrupt line INT0 (pin 2) |

### Pin Map

| Pin | Signal |
|---|---|
| 2 | `INT_BUS` — shared interrupt line |
| 3 | `HR_PIN` — heart rate pulse |
| 4 | `RR_PIN` — respiratory rate pulse |
| 5 | `SPO2_PIN` — SpO2 low alert |
| 6 | `BP_LO_PIN` — blood pressure low alert |
| 7 | `BP_HI_PIN` — blood pressure high alert |

---

### Timer Design

The firmware uses **Timer1** in CTC (Clear Timer on Compare) mode — not Timer0, which the Arduino framework reserves for `millis()` and `delay()`.

**Configuration:**
```
Prescaler:  /1024  →  tick rate = 16 MHz / 1024 = 15,625 Hz
OCR1A:      255    →  compare match every 256 ticks
Period:     256 / 15,625 = 16.384 ms per overflow
```

A single ISR increments a `uint32_t` software counter (`ovf_count`) on every compare match. All timing in the system is derived from this one counter by comparing differences — this makes rollover safe because unsigned subtraction wraps correctly.

**Derived time constants:**

| Constant | Overflows | Real time | Used for |
|---|---|---|---|
| `DEBOUNCE_OVF = 3` | 3 | ~49 ms | Input debounce window |
| `FLASH_OVF = 30` | 30 | ~491 ms | LED flash / LCD refresh |
| `HR_WIN_OVF = 366` | 366 | ~6 s | BPM measurement window |
| `RR_WIN_OVF = 732` | 732 | ~12 s | RPM measurement window |
| `LATCH_30S_OVF = 1831` | 1831 | ~30 s | SpO2 / BP alert hold duration |

---

### Interrupt Service Routines

#### Timer1 Compare Match ISR
```cpp
ISR(TIMER1_COMPA_vect) { ovf_count++; }
```
Increments the global clock counter. Kept minimal — no branching, no calls.

#### INT0 Rising-Edge ISR

All five sensor signals share a single external interrupt on pin 2 (INT0). When triggered, the ISR reads the full Port D register in one atomic instruction (`PIND`), snapshots `ovf_count`, then checks each bit individually:

| Bit | Signal | Action |
|---|---|---|
| 3 | HR | Increment `hr_count` (debounced) |
| 4 | RR | Increment `rr_count` (debounced) |
| 5 | SpO2 | Latch `spo2_low = 1` for 30 s |
| 6 | BP Lo | Latch `bp_bits = 01` for 30 s |
| 7 | BP Hi | Latch `bp_bits = 10` for 30 s |

Debounce: a pulse is only accepted if at least `DEBOUNCE_OVF` (~49 ms) has elapsed since the last accepted pulse on that signal. SpO2 and BP use latching — once triggered, they stay active for 30 seconds even if the signal drops, then auto-clear.

---

### Vital Sign Calculation

**Heart rate (BPM):**
Every 6 seconds, the main loop atomically reads and clears `hr_count`, then scales:
```
BPM = hr_count × 10
```
Ten beats counted in 6 seconds = 60/6 × 10 = 100 BPM. The window then resets.

**Respiratory rate (RPM):**
Every 12 seconds, same approach:
```
RPM = rr_count × 5
```
Three breaths in 12 seconds = 60/12 × 5 = 15 RPM.

**Normal ranges:**

| Vital | Low threshold | Normal | High threshold |
|---|---|---|---|
| HR | < 50 BPM | 50–110 | > 110 BPM |
| RR | < 8 RPM | 8–24 | > 24 RPM |
| SpO2 | < 95% (latch) | ≥ 95% | — |
| BP | < 90 mmHg (latch) | 90–140 | > 140 mmHg (latch) |

---

### System State Logic

A `sys_state` value is computed each loop iteration from the current vital states:

| `sys_state` | Condition | Meaning |
|---|---|---|
| 0 | All normal | No alarm |
| 1 | SpO2 low + RR low | Respiratory depression (partial) |
| 2 | RR high only | Respiratory excitation |
| 3 | HR low + BP low | Circulatory depression (partial) |
| 4 | HR high only | Circulatory excitation |
| 5 | HR low AND RR low | **Systemic depression** |
| 6 | HR high AND RR high | **Systemic excitation** |

---

### LED Behaviour

Both LEDs are common cathode: `HIGH` = on, `LOW` = off.

**Normal operation (states 0–4):**
Each LED independently shows the status of its system:

| LED | Tracks | Red | Blue | Green |
|---|---|---|---|---|
| Respiratory (RESP) | RR | RR high | RR low | RR normal |
| Circulatory (CIRC) | HR | HR high | HR low | HR normal |

**Systemic alarms (states 5 and 6):**
Both LEDs override to a synchronised flash at ~500 ms:

| State | Both LEDs |
|---|---|
| Systemic Depression (5) | Flash blue |
| Systemic Excitation (6) | Flash red |

---

### LCD Display

Updated every ~500 ms. No system state number is shown — the display is reserved for raw vital values only.

```
Row 1:  HR: 72♥  RR: 14○
Row 2:  BP☺ O2☺
```

**Row 1 icons:**
- `♥` (heart) — animates at the recorded BPM rate. The half-period in overflows is `1831 / BPM`, so at 60 BPM it toggles every ~500 ms.
- `○` (circle) — animates at the recorded RPM rate using the same formula.

**Row 2 icons** (custom characters):
- `☺` smile = normal
- `↓` down arrow = low
- `↑` up arrow = high

---

### Power-Up Sequence

On power-on, the system enters a **12-second boot hold** before normal operation begins:

1. Timer1 and INT0 are configured and enabled.
2. Both LEDs are set solid green.
3. LCD shows `"Vitals Monitor"` / `"Monitoring..."`.
4. A blocking loop waits for `RR_WIN_OVF` (732) overflows ≈ 12 seconds.
5. All timing references are initialised from the current `ovf_count` — measurement windows start fresh from this point.
6. LCD clears and normal operation begins.

`RR_WIN_OVF` is reused deliberately so the boot duration matches the first RR measurement window.

---

### Serial Output

Every 500 ms, one line is written to the serial port at 9600 baud:
```
BPM:72,RR:14,SPO2:0,BP:0
```

| Field | Values |
|---|---|
| `BPM` | 0–255 |
| `RR` | 0–255 |
| `SPO2` | 0 = OK (≥95%), 1 = LOW (<95%) |
| `BP` | 0 = normal, 1 = low (<90), 2 = high (>140) |

This format is consumed by the Python companion app.

---

## Python Companion App (`vitals_monitor_ui.py`)

### Architecture

The app is a **single-file application** that handles both the UI (frontend) and all data logic (backend) in one process. There is no separate server or database — it is intentionally simple.

**Responsibilities by layer:**

| Layer | What handles it |
|---|---|
| UI / frontend | `tkinter` widgets in `VitalsApp` class |
| Serial communication | Background `threading.Thread` running `_read_loop()` |
| Data parsing | `parse_line()` — converts raw serial string to a dict |
| In-memory logging | `self.log` — a list of sample dicts, only populated while recording |
| Analysis / report | Pure functions: `generate_report_text()`, `rising_edges()`, `systemic_state()` |

Because logging, analysis, and display all live in the same process, no network calls or file I/O are needed during a session. The tradeoff is that the log is lost when the app is closed — it is a session tool, not a long-term database.

---

### How to Run

```bash
python vitals_monitor_ui.py
```

---

### Workflow

```
1. Select port  →  Connect
2. Live vitals appear (always streaming, not yet recorded)
3. Click  ⏺ Record  to begin a session
4. Click  ⏹ Stop    to end the session
5. Click  📋 Generate Report  to see the analysis
```

Recording can be started and stopped multiple times per connection. Each time **Record** is pressed, the previous log is cleared and a new session begins.

---

### Live Display

Values update every ~500 ms (matching the Arduino serial rate). Any value outside its normal range is shown in **red**:

| Field | Red when |
|---|---|
| HR | < 50 or > 110 BPM |
| RR | < 8 or > 24 RPM |
| SpO2 | LOW flag set |
| BP | Low or High flag set |

---

### Report Contents

Generated from all samples collected between Record and Stop:

| Section | Metrics |
|---|---|
| Heart Rate | Current, min, max, average BPM |
| Respiratory Rate | Current, min, max, average RPM |
| SpO2 | % time below 95%, number of low events |
| Blood Pressure | % time normal / low / high, event counts per threshold |
| Systemic Alarms | Number of depression episodes, number of excitation episodes |
| Session | Start time, end time, total duration, sample count |

**Event counting** uses rising-edge detection — one SpO2 episode = one count, regardless of how long it persisted. Systemic episodes are counted as transitions into that state, not the number of samples in that state.

---

### Threading Model

The serial read loop runs in a **daemon thread** so it does not block the UI and exits automatically when the app closes. It calls `root.after(0, ...)` to push UI updates back onto the main tkinter thread — this is required because tkinter is not thread-safe and all widget updates must happen on the main thread.

```
Main thread:   tkinter event loop  →  widget updates, button callbacks
Daemon thread: serial readline()   →  parse → append to log → root.after(update UI)
```
