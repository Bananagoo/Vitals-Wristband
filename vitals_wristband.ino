#include <Wire.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

// Pin definitions
// INT_BUS is the shared interrupt line that triggers INT0 on Arduino pin 2.
// The ISR then reads Port D to see which individual sensor line is high.
#define INT_BUS     2
#define HR_PIN      3
#define RR_PIN      4
#define SPO2_PIN    5
#define BP_LO_PIN   6
#define BP_HI_PIN   7

// Respiratory LED (common cathode RGB: writing HIGH turns that color on)
#define RESP_R      10
#define RESP_B      9
#define RESP_G      8

// Circulatory LED (common cathode RGB: writing HIGH turns that color on)
#define CIRC_R      13
#define CIRC_B      12
#define CIRC_G      11

// Timer constants
// All times below are expressed in software "ticks" of Timer1 Compare Match A.
// One tick = one TIMER1_COMPA interrupt = about 16.384 ms.
#define HR_WIN_OVF       366UL   // ~6 s heart-rate measurement window
#define RR_WIN_OVF       732UL   // ~12 s respiratory-rate measurement window
#define DEBOUNCE_OVF       3UL   // ~49 ms debounce window
#define FLASH_OVF         30UL   // ~500 ms LED blink period
#define LCD_OVF           30UL   // ~500 ms LCD refresh period
#define LATCH_30S_OVF   1831UL   // ~30 s latch duration

// Counts gathered over a fixed window are scaled to a per-minute rate.
#define BPM_SCALE   10UL         // 6 s window -> multiply by 10 for BPM
#define RPM_SCALE    5UL         // 12 s window -> multiply by 5 for RPM

// ISR-shared variables
// volatile tells the compiler these values can change outside normal code flow 
// --> always read from memory rather than caching
volatile uint32_t ovf_count     = 0;   // software timebase; increments every Timer1 compare match (~16.384 ms)

volatile uint8_t  hr_count      = 0;   // accepted HR pulses in current 6 s window
volatile uint32_t hr_win_start  = 0;   // tick when current HR window began
volatile uint32_t hr_last       = 0;   // tick of last accepted HR pulse (for debounce)

volatile uint8_t  rr_count      = 0;   // accepted RR pulses in current 12 s window
volatile uint32_t rr_win_start  = 0;   // tick when current RR window began
volatile uint32_t rr_last       = 0;   // tick of last accepted RR pulse (for debounce)

// Abnormal states latched by the interrupt routine
volatile uint8_t  spo2_low      = 0;   // 1 = low SpO2 currently latched
volatile uint32_t spo2_start    = 0;   // tick when SpO2 low was latched
volatile uint32_t spo2_last     = 0;   // last accepted SpO2 event (for debounce)

volatile uint8_t  bp_bits       = 0;   // 00 normal, 01 low, 10 high
volatile uint32_t bp_start      = 0;   // tick when BP state was latched
volatile uint32_t bp_last       = 0;   // last accepted BP event (for debounce)

// Main-loop variables
uint8_t  bpm        = 0;   // most recent heart-rate result
uint8_t  rpm        = 0;   // most recent respiratory-rate result

uint8_t  hr_state   = 0;   // 00 normal, 01 low, 10 high
uint8_t  rr_state   = 0;
uint8_t  spo2_state = 0;
uint8_t  bp_state   = 0;

uint8_t  sys_state  = 0;   // 0 normal, 1 systemic depression, 2 systemic excitation

uint32_t flash_last    = 0;   // last time the system flash state toggled
uint32_t lcd_last      = 0;   // last LCD refresh tick
uint8_t  flash_on      = 0;   // toggled every FLASH_OVF ticks

uint32_t hr_beat_last  = 0;   // last time heart icon toggled
uint8_t  hr_beat_on    = 0;   // heart icon animation state
uint32_t rr_flash_last = 0;   // last time circle icon toggled
uint8_t  rr_flash_on   = 0;   // circle icon animation state

// Custom LCD characters
// Stored in the HD44780 CGRAM
byte charSmile[8] = {
  B00000,
  B01010,
  B01010,
  B00000,
  B00000,
  B10001,
  B01110,
  B00000
};

byte charDown[8] = {
  B00100,
  B00100,
  B00100,
  B00100,
  B10101,
  B01110,
  B00100,
  B00000
};

byte charUp[8] = {
  B00100,
  B01110,
  B10101,
  B00100,
  B00100,
  B00100,
  B00100,
  B00000
};

byte charHeart[8] = {
  B00000,
  B01010,
  B11111,
  B11111,
  B01110,
  B00100,
  B00000,
  B00000
};

byte charCircle[8] = {
  B00000,
  B01110,
  B10001,
  B10001,
  B10001,
  B01110,
  B00000,
  B00000
};

// Timer1 Compare Match ISR
// Runs once every Timer1 compare match.
// With 16 MHz clock, prescaler 1024, and OCR1A = 255:
// timer clock = 16,000,000 / 1024 = 15,625 Hz
// ISR period  = 256 / 15,625 = 16.384 ms
ISR(TIMER1_COMPA_vect) {
  ovf_count++;
}

// External interrupt ISR
// INT0 is tied to Arduino pin 2.
// Any sensor pulse raises the shared INT_BUS line, which triggers this ISR.
// We then read all of Port D once to see which sensor line(s) are active.
ISR(INT0_vect) {
  uint8_t  pins = PIND;      // snapshot PD0-PD7 in one direct register read
  uint32_t now  = ovf_count;

  // Heart-rate pulse on PD3 / Arduino pin 3
  if (pins & _BV(3)) {
    if ((now - hr_last) > DEBOUNCE_OVF) {
      hr_count++;
      hr_last = now;
    }
  }

  // Respiratory-rate pulse on PD4 / Arduino pin 4
  if (pins & _BV(4)) {
    if ((now - rr_last) > DEBOUNCE_OVF) {
      rr_count++;
      rr_last = now;
    }
  }

  // SpO2 low event on PD5 / Arduino pin 5
  // Latch low condition for 30 s; main loop clears it later.
  if (pins & _BV(5)) {
    if ((now - spo2_last) > DEBOUNCE_OVF) {
      spo2_low   = 1;
      spo2_start = now;
      spo2_last  = now;
    }
  }

  // BP low event on PD6 / Arduino pin 6
  // Ignore if BP high is already latched.
  if (pins & _BV(6)) {
    if (bp_bits != 0b10 && (now - bp_last) > DEBOUNCE_OVF) {
      bp_bits  = 0b01;
      bp_start = now;
      bp_last  = now;
    }
  }

  // BP high event on PD7 / Arduino pin 7
  // Ignore if BP low is already latched.
  if (pins & _BV(7)) {
    if (bp_bits != 0b01 && (now - bp_last) > DEBOUNCE_OVF) {
      bp_bits  = 0b10;
      bp_start = now;
      bp_last  = now;
    }
  }
}

void setup() {
  // Sensor inputs
  // configures the hardware direction of pin as output (set data direction register bit)
  pinMode(INT_BUS,   INPUT);
  pinMode(HR_PIN,    INPUT);
  pinMode(RR_PIN,    INPUT);
  pinMode(SPO2_PIN,  INPUT);
  pinMode(BP_LO_PIN, INPUT);
  pinMode(BP_HI_PIN, INPUT);

  // LED outputs
  // pin’s DDR bit is set to 1, makes it an output
  pinMode(RESP_R, OUTPUT);
  pinMode(RESP_B, OUTPUT);
  pinMode(RESP_G, OUTPUT);

  pinMode(CIRC_R, OUTPUT);
  pinMode(CIRC_B, OUTPUT);
  pinMode(CIRC_G, OUTPUT);

  ledsOff();

  Serial.begin(9600);

  lcd.init();
  lcd.backlight();

  // Load custom icons into LCD character memory
  lcd.createChar(0, charSmile);
  lcd.createChar(1, charDown);
  lcd.createChar(2, charUp);
  lcd.createChar(3, charHeart);
  lcd.createChar(4, charCircle);

  // Timer1 setup
  // TCCR1A = 0 clears compare output bits and lower WGM bits
  // WGM12 = 1 in TCCR1B selects CTC mode (clear timer on compare match)
  // CS12 and CS10 = 1 select prescaler 1024
  // OCR1A = 255 means compare match occurs when TCNT1 reaches 255
  // OCIE1A enables the Timer1 Compare Match A interrupt
  TCCR1A = 0;
  TCCR1B = _BV(WGM12) | _BV(CS12) | _BV(CS10);
  OCR1A  = 255;
  TIMSK1 = _BV(OCIE1A);

  // External interrupt setup
  // ISC01 and ISC00 are the interrupt sense control bits for INT0
  // When pin 2 rises, INT0_vect runs 
  EICRA |= (_BV(ISC01) | _BV(ISC00));
  EIMSK |= _BV(INT0); // set enable bit in the interrupt mask register

  sei();   // enable global interrupts from this point onward

  // Initialize all time references to the same startup tick
  uint32_t t = ovf_count;
  hr_win_start  = t;
  rr_win_start  = t;
  flash_last    = t;
  lcd_last      = t;
  hr_beat_last  = t;
  rr_flash_last = t;
}

void loop() {
  // Take one consistent snapshot of ISR-updated state.
  // cli()/sei() is needed because some values are 32-bit on an 8-bit MCU.
  cli();
  uint32_t now = ovf_count;
  uint8_t  sp  = spo2_low;
  uint8_t  bp  = bp_bits;
  uint32_t sp_start_local = spo2_start;
  uint32_t bp_start_local = bp_start;
  sei();

  // Clear latched faults after 30 s
  if (sp && ((now - sp_start_local) >= LATCH_30S_OVF)) {
    // Re-check while interrupts are off so the ISR cannot update the latch mid-clear.
    cli();
    if ((ovf_count - spo2_start) >= LATCH_30S_OVF) {
      spo2_low = 0;
    }
    sei();
  }

  if (bp && ((now - bp_start_local) >= LATCH_30S_OVF)) {
    cli();
    if ((ovf_count - bp_start) >= LATCH_30S_OVF) {
      bp_bits = 0b00;
    }
    sei();
  }

  // Rate calculations
  // Every 6 s: snapshot HR pulse count, clear it atomically, convert to BPM.
  if ((now - hr_win_start) >= HR_WIN_OVF) {
    cli();
    uint8_t beats = hr_count;
    hr_count = 0;
    sei();

    bpm = beats * BPM_SCALE;
    hr_win_start = now;
  }

  // Every 12 s: snapshot RR pulse count, clear it atomically, convert to RPM.
  if ((now - rr_win_start) >= RR_WIN_OVF) {
    cli();
    uint8_t breaths = rr_count;
    rr_count = 0;
    sei();

    rpm = breaths * RPM_SCALE;
    rr_win_start = now;
  }

  // Re-read latched states in case they changed during the rate calculations.
  cli();
  sp = spo2_low;
  bp = bp_bits;
  sei();

  // Classify vital signs
  hr_state   = (bpm < 50) ? 0b01 : (bpm > 110) ? 0b10 : 0b00;
  rr_state   = (rpm < 12) ? 0b01 : (rpm > 24)  ? 0b10 : 0b00;
  spo2_state = sp;
  bp_state   = bp;

  // System-level state only depends on HR and RR together.
  if      (hr_state == 0b01 && rr_state == 0b01) sys_state = 1;   // systemic depression
  else if (hr_state == 0b10 && rr_state == 0b10) sys_state = 2;   // systemic excitation
  else                                           sys_state = 0;   // otherwise normal

  // Timed behaviour
  // Toggle shared flash flag every ~500 ms.
  if ((now - flash_last) >= FLASH_OVF) {
    flash_last = now;
    flash_on   = !flash_on;
  }

  // Heart icon blinks at the measured BPM rate.
  if (bpm > 0) {
    uint32_t half_period = 1831UL / bpm;   // 1831 ticks ≈ 30 s, so 30 s / BPM = half-cycle
    if ((now - hr_beat_last) >= half_period) {
      hr_beat_last = now;
      hr_beat_on   = !hr_beat_on;
    }
  } else {
    hr_beat_on = 0;
  }

  // Circle icon blinks at the measured RPM rate.
  if (rpm > 0) {
    uint32_t half_period = 1831UL / rpm;
    if ((now - rr_flash_last) >= half_period) {
      rr_flash_last = now;
      rr_flash_on   = !rr_flash_on;
    }
  } else {
    rr_flash_on = 0;
  }

  updateLEDs();

  // Refresh LCD at ~2 Hz.
  if ((now - lcd_last) >= LCD_OVF) {
    lcd_last = now;
    updateDisplay();
  }
}

// LED helpers
// Common cathode means HIGH turns a channel on, LOW turns it off.
void ledsOff() {
  digitalWrite(RESP_R, LOW);
  digitalWrite(RESP_B, LOW);
  digitalWrite(RESP_G, LOW);

  digitalWrite(CIRC_R, LOW);
  digitalWrite(CIRC_B, LOW);
  digitalWrite(CIRC_G, LOW);
}

void setResp(uint8_t r, uint8_t g, uint8_t b) {
  digitalWrite(RESP_R, r ? HIGH : LOW);
  digitalWrite(RESP_B, b ? HIGH : LOW);
  digitalWrite(RESP_G, g ? HIGH : LOW);
}

void setCirc(uint8_t r, uint8_t g, uint8_t b) {
  digitalWrite(CIRC_R, r ? HIGH : LOW);
  digitalWrite(CIRC_B, b ? HIGH : LOW);
  digitalWrite(CIRC_G, g ? HIGH : LOW);
}

void updateLEDs() {
  if (sys_state == 1) {
    // Both low -> both systems flash blue
    if (flash_on) {
      setResp(0, 0, 1);
      setCirc(0, 0, 1);
    } else {
      ledsOff();
    }

  } else if (sys_state == 2) {
    // Both high -> both systems flash red
    if (flash_on) {
      setResp(1, 0, 0);
      setCirc(1, 0, 0);
    } else {
      ledsOff();
    }

  } else {
    // Otherwise show each subsystem independently
    if      (rr_state == 0b10) setResp(1, 0, 0);  // RR high = red
    else if (rr_state == 0b01) setResp(0, 0, 1);  // RR low  = blue
    else                       setResp(0, 1, 0);  // RR normal = green

    if      (hr_state == 0b10) setCirc(1, 0, 0);  // HR high = red
    else if (hr_state == 0b01) setCirc(0, 0, 1);  // HR low  = blue
    else                       setCirc(0, 1, 0);  // HR normal = green
  }
}

// LCD / serial output
void updateDisplay() {
  Serial.print("BPM:");   Serial.print(bpm);
  Serial.print(",RR:");   Serial.print(rpm);
  Serial.print(",SPO2:"); Serial.print(spo2_state);
  Serial.print(",BP:");   Serial.println(bp_state);

  // Row 1: RR value + breathing icon, HR value + heart icon
  lcd.setCursor(0, 0);
  lcd.print("RR:");
  if (rpm < 10) lcd.print(' ');
  lcd.print(rpm);
  if (rr_flash_on) lcd.write(byte(4)); else lcd.print(' ');

  lcd.print(" HR:");
  if (bpm < 100) lcd.print(' ');
  if (bpm < 10)  lcd.print(' ');
  lcd.print(bpm);
  if (hr_beat_on) lcd.write(byte(3)); else lcd.print(' ');

  // Row 2: O2 icon and BP icon
  lcd.setCursor(0, 1);
  lcd.print("O2");
  if (spo2_state == 0) lcd.write(byte(0));   // smile = normal
  else                 lcd.write(byte(1));   // down arrow = low

  lcd.print(" BP");
  if      (bp_state == 0b00) lcd.write(byte(0));   // smile = normal
  else if (bp_state == 0b01) lcd.write(byte(1));   // down = low
  else if (bp_state == 0b10) lcd.write(byte(2));   // up = high
  else                       lcd.print('?');

  lcd.print("         ");   // clear leftover characters from the previous frame
}