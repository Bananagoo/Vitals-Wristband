#include <Wire.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

// Pin definitions
#define INT_BUS     2 // All sensor signals share one interrupt line
#define HR_PIN      3
#define RR_PIN      4
#define SPO2_PIN    5
#define BP_LO_PIN   6
#define BP_HI_PIN   7

// Respiratory LED (common cathode RGB — HIGH = on)
#define RESP_R      10
#define RESP_B      9
#define RESP_G      8

// Circulatory LED (common cathode RGB — HIGH = on)
#define CIRC_R      13
#define CIRC_B      12
#define CIRC_G      11

// Timer constants (all in units of Timer1 overflow ticks, ≈16.384 ms each)
#define HR_WIN_OVF       366UL   // 6 s
#define RR_WIN_OVF       732UL   // 12 s
#define DEBOUNCE_OVF       3UL   // 49 MS
#define FLASH_OVF         30UL   // 500 ms
#define LCD_OVF           30UL   // 500 ms
#define LATCH_30S_OVF   1831UL   // 30s

// BPM/RPM scaling factors: counts × scale = per-minute rate
#define BPM_SCALE   10UL
#define RPM_SCALE    5UL

// ISR-shared variables
// the ISR and main loop both touch them, so every access must go to memory hence volatile
volatile uint32_t ovf_count     = 0;   // master tick counter, incremented by Timer1 ISR

volatile uint8_t  hr_count      = 0; 
volatile uint32_t hr_win_start  = 0;   // tick timestamp when current HR window opened
volatile uint32_t hr_last       = 0;   // tick of last accepted HR pulse (for debounce)

volatile uint8_t  rr_count      = 0;
volatile uint32_t rr_win_start  = 0;
volatile uint32_t rr_last       = 0;

// Latched abnormal states set by ISR
volatile uint8_t  spo2_low      = 0;
volatile uint32_t spo2_start    = 0;
volatile uint32_t spo2_last     = 0;

volatile uint8_t  bp_bits       = 0; // 00 normal, 01 low, 10 high
volatile uint32_t bp_start      = 0;
volatile uint32_t bp_last       = 0;

// Main loop variables
uint8_t  bpm        = 0;   // most recent heart rate result
uint8_t  rpm        = 0;   // most recent respiratory rate result

uint8_t  hr_state   = 0;   // 00 normal, 01 low, 10 high
uint8_t  rr_state   = 0;
uint8_t  spo2_state = 0;
uint8_t  bp_state   = 0;

uint8_t  sys_state  = 0;

uint32_t flash_last    = 0;
uint32_t lcd_last      = 0;
uint8_t  flash_on      = 0;   // toggled every FLASH_OVF ticks for blinking

uint32_t hr_beat_last  = 0;
uint8_t  hr_beat_on    = 0;   // heart icon animation state
uint32_t rr_flash_last = 0;
uint8_t  rr_flash_on   = 0;   // circle icon animation state


// Custom LCD characters (5×8 pixel bitmaps stored in LCD CGRAM slots 0–4)
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

// Timer1 Compare Match A ISR
ISR(TIMER1_COMPA_vect) {
  ovf_count++;
}

// INT0 Rising Edge ISR, fires whenever any sensor drives INT_BUS (pin 2) high
ISR(INT0_vect) {
  uint8_t  pins = PIND;        // snapshot Port D pins 0–7 in one atomic read
  uint32_t now  = ovf_count;   // ISR entry has interrupts already off so we can read ovf_count

  // HR pulse count
  if (pins & _BV(3)) {
    if ((now - hr_last) > DEBOUNCE_OVF) {
      hr_count++;
      hr_last = now;
    }
  }

  // RR pulse count
  if (pins & _BV(4)) {
    if ((now - rr_last) > DEBOUNCE_OVF) {
      rr_count++;
      rr_last = now;
    }
  }

  // SpO2 low latch for 30 s
  if (pins & _BV(5)) {
    if ((now - spo2_last) > DEBOUNCE_OVF) {
      spo2_low   = 1;
      spo2_start = now;
      spo2_last  = now;
    }
  }

  // BP low latch for 30 s (ignored if high is already latched)
  if (pins & _BV(6)) {
    if (bp_bits != 0b10 && (now - bp_last) > DEBOUNCE_OVF) {
      bp_bits  = 0b01;
      bp_start = now;
      bp_last  = now;
    }
  }

  // BP high latch for 30 s (ignored if low is already latched)
  if (pins & _BV(7)) {
    if (bp_bits != 0b01 && (now - bp_last) > DEBOUNCE_OVF) {
      bp_bits  = 0b10;
      bp_start = now;
      bp_last  = now;
    }
  }
}

void setup() {
  pinMode(INT_BUS,   INPUT);
  pinMode(HR_PIN,    INPUT);
  pinMode(RR_PIN,    INPUT);
  pinMode(SPO2_PIN,  INPUT);
  pinMode(BP_LO_PIN, INPUT);
  pinMode(BP_HI_PIN, INPUT);

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

  // Load custom characters into CGRAM slots 0–4
  lcd.createChar(0, charSmile);
  lcd.createChar(1, charDown);
  lcd.createChar(2, charUp);
  lcd.createChar(3, charHeart);
  lcd.createChar(4, charCircle);

  TCCR1A = 0;                                         // CTC mode (WGM13:10 = 0100)
  TCCR1B = _BV(WGM12) | _BV(CS12) | _BV(CS10);      // CTC + prescaler /1024
  OCR1A  = 255;                                       // TOP, fires ISR every 256 ticks
  TIMSK1 = _BV(OCIE1A);                              // enable Compare Match A interrupt

  // INT0: rising edge trigger (ISC01=1, ISC00=1)
  EICRA |= (_BV(ISC01) | _BV(ISC00));
  EIMSK |= _BV(INT0);

  sei();   // enable global interrupts so ISRs active from this point

  uint32_t t = ovf_count;
  hr_win_start  = t;
  rr_win_start  = t;
  flash_last    = t;
  lcd_last      = t;
  hr_beat_last  = t;
  rr_flash_last = t;
}

void loop() {
  // cli()/sei() prevents the ISR from modifying a multi-byte variable mid-read
  cli();
  uint32_t now = ovf_count;
  uint8_t  sp  = spo2_low;
  uint8_t  bp  = bp_bits;
  uint32_t sp_start_local = spo2_start;
  uint32_t bp_start_local = bp_start;
  sei();

  // Expire SpO2 latch after 30 s
  if (sp && ((now - sp_start_local) >= LATCH_30S_OVF)) {
    cli();
    if ((ovf_count - spo2_start) >= LATCH_30S_OVF) {
      spo2_low = 0;
    }
    sei();
  }

  // Expire BP latch after 30 s
  if (bp && ((now - bp_start_local) >= LATCH_30S_OVF)) {
    cli();
    if ((ovf_count - bp_start) >= LATCH_30S_OVF) {
      bp_bits = 0b00;
    }
    sei();
  }

  // BPM every 6 s --> snapshot and clear hr_count under cli() so the ISR cannot increment it between the read and the reset
  if ((now - hr_win_start) >= HR_WIN_OVF) {
    cli();
    uint8_t beats = hr_count;
    hr_count = 0;
    sei();

    bpm = (uint8_t)((uint32_t)beats * BPM_SCALE);
    hr_win_start = now;
  }

  // RPM every 12 s
  if ((now - rr_win_start) >= RR_WIN_OVF) {
    cli();
    uint8_t breaths = rr_count;
    rr_count = 0;
    sei();

    rpm = (uint8_t)((uint32_t)breaths * RPM_SCALE);
    rr_win_start = now;
  }

  // Re-read latched states after rate calculation in case they changed mid-loop
  cli();
  sp = spo2_low;
  bp = bp_bits;
  sei();

  // Classify each vital into low (01), high (10), or normal (00)
  hr_state   = (bpm < 50) ? 0b01 : (bpm > 110) ? 0b10 : 0b00;
  rr_state   = (rpm < 12) ? 0b01 : (rpm > 24)  ? 0b10 : 0b00;
  spo2_state = sp;
  bp_state   = bp;

  // Convenience booleans for the state machine below
  uint8_t hrLo = (hr_state == 0b01);
  uint8_t hrHi = (hr_state == 0b10);
  uint8_t rrLo = (rr_state == 0b01);
  uint8_t rrHi = (rr_state == 0b10);

  if      (hrLo && rrLo)           sys_state = 1;   // systemic depression
  else if (hrHi && rrHi)           sys_state = 2;   // systemic excitation
  else                             sys_state = 0;   // all normal

  // flash toggle every 500 ms
  if ((now - flash_last) >= FLASH_OVF) {
    flash_last = now;
    flash_on   = !flash_on;
  }

  // Heart icon toggles at BPM rate
  if (bpm > 0) {
    uint32_t half_period = 1831UL / bpm;
    if ((now - hr_beat_last) >= half_period) {
      hr_beat_last = now;
      hr_beat_on   = !hr_beat_on;
    }
  } else {
    hr_beat_on = 0;
  }

  // Circle icon toggles at RPM rate
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

  // LCD update every 500 ms
  if ((now - lcd_last) >= LCD_OVF) {
    lcd_last = now;
    updateDisplay();
  }
}

// Common cathode LED control
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
    // Systemic Depression: both flash blue
    if (flash_on) {
      setResp(0, 0, 1);
      setCirc(0, 0, 1);
    } else {
      ledsOff();
    }

  } else if (sys_state == 2) {
    // Systemic Excitation: both flash red
    if (flash_on) {
      setResp(1, 0, 0);
      setCirc(1, 0, 0);
    } else {
      ledsOff();
    }

  } else {
    // Independent per-system display for all other states
    // Respiratory LED tracks RR
    if      (rr_state == 0b10) setResp(1, 0, 0);  // high = red
    else if (rr_state == 0b01) setResp(0, 0, 1);  // low  = blue
    else                       setResp(0, 1, 0);  // normal = green

    // Circulatory LED tracks HR
    if      (hr_state == 0b10) setCirc(1, 0, 0);  // high = red
    else if (hr_state == 0b01) setCirc(0, 0, 1);  // low  = blue
    else                       setCirc(0, 1, 0);  // normal = green
  }
}

void updateDisplay() {
  Serial.print("BPM:"); Serial.print(bpm);
  Serial.print(",RR:");  Serial.print(rpm);
  Serial.print(",SPO2:"); Serial.print(spo2_state);
  Serial.print(",BP:");  Serial.println(bp_state);

  // Row 1: RR:<rpm><circle> HR:<bpm><heart>
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

  // Row 2: O2<icon> BP<icon>
  lcd.setCursor(0, 1);
  lcd.print("O2");
  if (spo2_state == 0) lcd.write(byte(0));   // smile = normal
  else                 lcd.write(byte(1));   // down arrow = low

  lcd.print(" BP");
  if      (bp_state == 0b00) lcd.write(byte(0));   // smile  = normal
  else if (bp_state == 0b01) lcd.write(byte(1));   // down   = low
  else if (bp_state == 0b10) lcd.write(byte(2));   // up     = high
  else                       lcd.print('?');

  lcd.print("         ");   // overwrite any leftover characters from previous frame
}