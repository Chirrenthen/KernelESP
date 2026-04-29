/*
 * KernelESP v1.0
 * A Linux-like interactive shell for the ESP32
 * Tested on ESP32 - WROOM
 */

#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <driver/dac.h>
#include <driver/touch_pad.h>
#include <string.h>
#include <stdlib.h>

// Limits
#define CMD_LEN       256
#define MAX_ARGS      16
#define ARG_LEN       64
#define PATH_LEN      64
#define DMESG_LINES   12
#define DMESG_LEN     80
#define HOSTNAME      "kernelesp"

// ANSI helpers
#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define DIM     "\033[2m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN    "\033[36m"
#define WHITE   "\033[37m"
#define GRAY    "\033[90m"
#define BGREEN  "\033[92m"
#define BYELLOW "\033[93m"
#define BCYAN   "\033[96m"

// Global State
char  inputBuffer[CMD_LEN];
int   inputLen   = 0;
char  currentPath[PATH_LEN] = "/";
unsigned long bootTime;

// Args storage (avoid heap fragmentation)
char  argStorage[MAX_ARGS][ARG_LEN];
char* args[MAX_ARGS];
uint8_t argCount = 0;

// Kernel log
struct DmesgEntry { unsigned long ts; char msg[DMESG_LEN]; };
DmesgEntry dmesgBuf[DMESG_LINES];
uint8_t dmesgHead = 0;
uint8_t dmesgCount = 0;

// WiFi state
bool   wifiConnected  = false;
bool   apActive       = false;
String staSSID        = "";
String apSSID         = HOSTNAME "_ap";
String apPASS         = "kernelesp";

// Web server (optional)
WebServer* httpServer = nullptr;
bool httpRunning = false;

// Kernel Log
void klog(const char* msg) {
  uint8_t idx = dmesgHead % DMESG_LINES;
  dmesgBuf[idx].ts = (millis() - bootTime) / 1000;
  strncpy(dmesgBuf[idx].msg, msg, DMESG_LEN - 1);
  dmesgBuf[idx].msg[DMESG_LEN - 1] = '\0';
  dmesgHead++;
  if (dmesgCount < DMESG_LINES) dmesgCount++;
}

// Utilities
int safeAtoi(const char* s) {
  if (!s || !*s) return 0;
  return (int)strtol(s, nullptr, 0); // handles 0x hex too
}

float safeAtof(const char* s) {
  if (!s || !*s) return 0.0f;
  return strtof(s, nullptr);
}

void strlowerBuf(char* s) {
  for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32;
}

// Trim leading whitespace in-place, returns pointer into s
char* ltrim(char* s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

// Command Parser
// Handles quoted strings properly: eval "gpio 2 on; delay 100"
void parseCommand(char* line, char** argv, uint8_t* argc) {
  *argc = 0;
  char* p = line;
  while (*p && *argc < MAX_ARGS) {
    while (*p == ' ') p++;
    if (!*p) break;

    char* dst = argStorage[*argc];
    int di = 0;

    if (*p == '"' || *p == '\'') {
      char q = *p++;
      while (*p && *p != q && di < ARG_LEN - 1) dst[di++] = *p++;
      if (*p == q) p++;
    } else {
      while (*p && *p != ' ' && di < ARG_LEN - 1) dst[di++] = *p++;
    }
    dst[di] = '\0';
    argv[(*argc)++] = dst;
  }
}

// Pin Resolution
// ESP32 GPIO: 0-39 (input-only: 34-39), ADC1: 32-39, ADC2: 0,2,4,12-15,25-27
// DAC: 25, 26  PWM: any output pin  Touch: 0,2,4,12-15,27,32,33
int resolvePin(const char* name) {
  if (!name) return -1;
  if ((name[0] == 'D' || name[0] == 'd') && name[1]) return safeAtoi(name + 1);
  if (name[0] >= '0' && name[0] <= '9') return safeAtoi(name);
  if ((name[0] == 'A' || name[0] == 'a') && name[1] >= '0' && name[1] <= '9') {
    // Logical Ax -> GPIO mapping (common ESP32 breakout)
    const int amap[] = {36, 39, 34, 35, 32, 33, 25, 26, 27, 14};
    int ch = name[1] - '0';
    return (ch < 10) ? amap[ch] : -1;
  }
  if (strcasecmp(name, "LED") == 0) return 2;   // Built-in LED most boards
  if (strcasecmp(name, "DAC1") == 0) return 25;
  if (strcasecmp(name, "DAC2") == 0) return 26;
  return -1;
}

// SPIFFS Filesystem Helpers
String buildPath(const char* name) {
  if (name[0] == '/') return String(name);
  String p = String(currentPath);
  if (!p.endsWith("/")) p += "/";
  p += name;
  return p;
}

void ensureDir(const String& dirPath) {
  // SPIFFS has no real directories — we track them via sentinel files
  String marker = dirPath;
  if (!marker.endsWith("/")) marker += "/";
  marker += ".dir";
  if (!SPIFFS.exists(marker)) {
    File f = SPIFFS.open(marker, FILE_WRITE);
    if (f) f.close();
  }
}

bool isDirectory(const String& path) {
  String marker = path;
  if (!marker.endsWith("/")) marker += "/";
  marker += ".dir";
  return SPIFFS.exists(marker);
}

void initFilesystem() {
  if (!SPIFFS.begin(true)) {
    Serial.println(RED "SPIFFS mount failed — formatting..." RESET);
    SPIFFS.format();
    SPIFFS.begin(true);
  }
  // Create default directories
  const char* defaults[] = {"/home", "/tmp", "/etc", "/dev"};
  for (auto d : defaults) ensureDir(String(d));
  klog("SPIFFS mounted OK");
}

// ASCII Logo
void showLogo() {
  Serial.print(F("\033[2J\033[H")); // clear screen, home
  Serial.println(F(CYAN
    "  ██╗  ██╗███████╗██████╗ ███╗   ██╗███████╗██╗     ███████╗███████╗██████╗ "  RESET));
  Serial.println(F(CYAN
    "  ██║ ██╔╝██╔════╝██╔══██╗████╗  ██║██╔════╝██║     ██╔════╝██╔════╝██╔══██╗" RESET));
  Serial.println(F(BCYAN
    "  █████╔╝ █████╗  ██████╔╝██╔██╗ ██║█████╗  ██║     █████╗  ███████╗██████╔╝" RESET));
  Serial.println(F(CYAN
    "  ██╔═██╗ ██╔══╝  ██╔══██╗██║╚██╗██║██╔══╝  ██║     ██╔══╝  ╚════██║██╔═══╝ " RESET));
  Serial.println(F(CYAN
    "  ██║  ██╗███████╗██║  ██║██║ ╚████║███████╗███████╗███████╗███████║██║      " RESET));
  Serial.println(F(GRAY
    "  ╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝╚═╝  ╚═══╝╚══════╝╚══════╝╚══════╝╚══════╝╚═╝  v1.0" RESET));
  Serial.println();

  // System info bar
  uint32_t heap = ESP.getFreeHeap();
  uint32_t flash = ESP.getFlashChipSize() / 1024;
  Serial.printf(GRAY "  CPU: " WHITE "Xtensa LX6 @ %dMHz  " GRAY
                "RAM: " WHITE "%u KB free  " GRAY
                "Flash: " WHITE "%u KB" RESET "\n",
                ESP.getCpuFreqMHz(), heap / 1024, flash);

  // WiFi status
  if (wifiConnected) {
    Serial.printf(GRAY "  WiFi: " GREEN "Connected  " GRAY "IP: " WHITE "%s" RESET "\n",
                  WiFi.localIP().toString().c_str());
  } else if (apActive) {
    Serial.printf(GRAY "  WiFi: " YELLOW "AP Mode  " GRAY "IP: " WHITE "%s" RESET "\n",
                  WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println(GRAY "  WiFi: " RED "Offline" RESET);
  }

  Serial.println(GRAY "  ─────────────────────────────────────────────────────────────────────────" RESET);
  Serial.println(GRAY "  Type " YELLOW "'help'" GRAY " for commands.  " YELLOW "'wifi help'" GRAY " for network commands." RESET "\n");
}

// Prompt
void printPrompt() {
  const char* wifiTag = wifiConnected ? GREEN " [" BCYAN "net" GREEN "]" : "";
  Serial.printf(BGREEN "root@" HOSTNAME RESET ":" BLUE "%s" RESET "%s" WHITE "$ " RESET,
                currentPath, wifiTag);
}

// Hardware Commands
void cmdPinMode(char** argv, uint8_t argc) {
  if (argc < 3) { Serial.println(F("Usage: pinmode <pin> <input|output|pullup|pulldown>")); return; }
  int pin = resolvePin(argv[1]);
  if (pin < 0) { Serial.println(RED "Invalid pin" RESET); return; }
  String m = String(argv[2]); m.toLowerCase();
  if (m == "output" || m == "out")           { pinMode(pin, OUTPUT);        Serial.printf("GPIO%d → OUTPUT\n", pin); }
  else if (m == "input")                      { pinMode(pin, INPUT);         Serial.printf("GPIO%d → INPUT\n", pin); }
  else if (m == "pullup"   || m == "input_pullup")   { pinMode(pin, INPUT_PULLUP);  Serial.printf("GPIO%d → INPUT_PULLUP\n", pin); }
  else if (m == "pulldown" || m == "input_pulldown") { pinMode(pin, INPUT_PULLDOWN);Serial.printf("GPIO%d → INPUT_PULLDOWN\n", pin); }
  else { Serial.println(RED "Unknown mode" RESET); return; }
  char buf[48]; snprintf(buf, sizeof(buf), "pinMode GPIO%d %s", pin, argv[2]); klog(buf);
}

void cmdDigitalWrite(char** argv, uint8_t argc) {
  if (argc < 3) { Serial.println(F("Usage: write <pin> <HIGH|LOW|1|0|on|off>")); return; }
  int pin = resolvePin(argv[1]);
  if (pin < 0) { Serial.println(RED "Invalid pin" RESET); return; }
  String v = String(argv[2]); v.toLowerCase();
  int val = (v == "high" || v == "1" || v == "on") ? HIGH : LOW;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, val);
  Serial.printf("GPIO%d = %s\n", pin, val ? "HIGH" : "LOW");
}

void cmdDigitalRead(char** argv, uint8_t argc) {
  if (argc < 2) {
    Serial.println(F("\n  " YELLOW "GPIO State:" RESET "  (output-capable: 0-33, input-only: 34-39)\n"));
    Serial.println(F("  Pin  State   │  Pin  State"));
    Serial.println(F("  ─────────────┼─────────────"));
    const int pins[] = {2,4,5,12,13,14,15,16,17,18,19,21,22,23,25,26,27,32,33,34,35,36,39};
    for (int i = 0; i < 23; i++) {
      int p = pins[i];
      int v = digitalRead(p);
      Serial.printf("  GPIO%-2d  %s%-5s" RESET, p, v ? GREEN : GRAY, v ? "HIGH" : "LOW");
      if (i % 2 == 0) Serial.print("  │");
      else Serial.println();
    }
    Serial.println(F("\n"));
    return;
  }
  int pin = resolvePin(argv[1]);
  if (pin < 0) { Serial.println(RED "Invalid pin" RESET); return; }
  int v = digitalRead(pin);
  Serial.printf("GPIO%d = %s%s" RESET "\n", pin, v ? GREEN : GRAY, v ? "HIGH" : "LOW");
}

void cmdAnalogRead(char** argv, uint8_t argc) {
  // ADC on ESP32: 12-bit (0-4095), 3.3V reference
  if (argc < 2) {
    Serial.println(F("\n  " YELLOW "ADC Channels:" RESET "\n"));
    const int adcPins[] = {36, 39, 34, 35, 32, 33};
    const char* adcNames[] = {"A0/GPIO36","A1/GPIO39","A2/GPIO34","A3/GPIO35","A4/GPIO32","A5/GPIO33"};
    for (int i = 0; i < 6; i++) {
      int raw = analogRead(adcPins[i]);
      float v  = raw * 3.3f / 4095.0f;
      int bar  = map(raw, 0, 4095, 0, 30);
      Serial.printf("  %-12s [", adcNames[i]);
      for (int b = 0; b < 30; b++) Serial.print(b < bar ? GREEN "█" RESET : GRAY "░" RESET);
      Serial.printf("] %4d (%.2fV)\n", raw, v);
    }
    Serial.println();
    return;
  }
  int pin = resolvePin(argv[1]);
  if (pin < 0) { Serial.println(RED "Invalid pin" RESET); return; }
  int raw = analogRead(pin);
  float v  = raw * 3.3f / 4095.0f;
  Serial.printf("GPIO%d ADC = %d (%.3fV / 3.3V)\n", pin, raw, v);
}

void cmdPWM(char** argv, uint8_t argc) {
  // ESP32 LEDC: channel-based PWM
  if (argc < 3) {
    Serial.println(F("Usage: pwm <pin> <duty 0-255> [freq_hz] [channel 0-15]"));
    return;
  }
  int pin  = resolvePin(argv[1]);
  if (pin < 0) { Serial.println(RED "Invalid pin" RESET); return; }
  int duty = constrain(safeAtoi(argv[2]), 0, 255);
  int freq = (argc >= 4) ? safeAtoi(argv[3]) : 5000;
  int ch   = (argc >= 5) ? constrain(safeAtoi(argv[4]), 0, 15) : 0;
  ledcSetup(ch, freq, 8);          // 8-bit resolution
  ledcAttachPin(pin, ch);
  ledcWrite(ch, duty);
  float pct = duty * 100.0f / 255.0f;
  Serial.printf("GPIO%d PWM ch%d: duty=%d (%.0f%%) freq=%dHz\n", pin, ch, duty, pct, freq);
}

void cmdDAC(char** argv, uint8_t argc) {
  if (argc < 3) { Serial.println(F("Usage: dac <25|26> <0-255>")); return; }
  int pin = resolvePin(argv[1]);
  if (pin != 25 && pin != 26) { Serial.println(RED "DAC only on GPIO25 and GPIO26" RESET); return; }
  int val = constrain(safeAtoi(argv[2]), 0, 255);
  dacWrite(pin, val);
  float v = val * 3.3f / 255.0f;
  Serial.printf("DAC GPIO%d = %d (%.3fV)\n", pin, val, v);
}

void cmdTouch(char** argv, uint8_t argc) {
  // Touch-capable pins: 0,2,4,12,13,14,15,27,32,33
  const int touchPins[] = {4, 0, 2, 15, 13, 12, 14, 27, 33, 32};
  if (argc < 2) {
    Serial.println(F("\n  " YELLOW "Touch Sensor Readings:" RESET "\n"));
    const char* tnames[] = {"T0/GPIO4","T1/GPIO0","T2/GPIO2","T3/GPIO15",
                             "T4/GPIO13","T5/GPIO12","T6/GPIO14","T7/GPIO27",
                             "T8/GPIO33","T9/GPIO32"};
    for (int i = 0; i < 10; i++) {
      uint16_t val = touchRead(touchPins[i]);
      int bar = map(constrain(val, 0, 80), 0, 80, 30, 0);
      bool touched = val < 30;
      Serial.printf("  %-12s [", tnames[i]);
      for (int b = 0; b < 30; b++)
        Serial.print(b < bar ? (touched ? RED "█" RESET : CYAN "█" RESET) : GRAY "░" RESET);
      Serial.printf("] %3d  %s\n", val, touched ? RED "<TOUCH>" RESET : "");
    }
    Serial.println();
    return;
  }
  int pin = resolvePin(argv[1]);
  if (pin < 0) { Serial.println(RED "Invalid pin" RESET); return; }
  uint16_t val = touchRead(pin);
  Serial.printf("Touch GPIO%d = %d (%s)\n", pin, val, val < 30 ? RED "TOUCHED" RESET : "not touched");
}

void cmdTone(char** argv, uint8_t argc) {
  if (argc < 3) { Serial.println(F("Usage: tone <pin> <freq_hz> [duration_ms]")); return; }
  int pin  = resolvePin(argv[1]);
  if (pin < 0) { Serial.println(RED "Invalid pin" RESET); return; }
  int freq = safeAtoi(argv[2]);
  if (freq < 1 || freq > 20000) { Serial.println(RED "Frequency: 1-20000 Hz" RESET); return; }
  ledcSetup(14, freq, 8);
  ledcAttachPin(pin, 14);
  ledcWrite(14, 127); // 50% duty = square wave
  if (argc >= 4) {
    int ms = safeAtoi(argv[3]);
    Serial.printf("Tone GPIO%d: %dHz for %dms\n", pin, freq, ms);
    delay(ms);
    ledcWrite(14, 0);
    ledcDetachPin(pin);
  } else {
    Serial.printf("Tone GPIO%d: %dHz continuous. Use 'notone %s' to stop.\n", pin, freq, argv[1]);
  }
}

void cmdNoTone(char** argv, uint8_t argc) {
  int pin = (argc >= 2) ? resolvePin(argv[1]) : -1;
  ledcWrite(14, 0);
  if (pin >= 0) ledcDetachPin(pin);
  Serial.println("Tone stopped");
}

void cmdGPIO(char** argv, uint8_t argc) {
  if (argc < 3) { Serial.println(F("Usage: gpio <pin> <on|off|toggle|read>")); return; }
  int pin = resolvePin(argv[1]);
  if (pin < 0) { Serial.println(RED "Invalid pin" RESET); return; }
  String act = String(argv[2]); act.toLowerCase();
  if (act == "on"  || act == "1" || act == "high") { pinMode(pin, OUTPUT); digitalWrite(pin, HIGH); Serial.printf("GPIO%d → ON\n", pin); }
  else if (act == "off" || act == "0" || act == "low") { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); Serial.printf("GPIO%d → OFF\n", pin); }
  else if (act == "toggle") {
    pinMode(pin, OUTPUT);
    int nv = !digitalRead(pin); digitalWrite(pin, nv);
    Serial.printf("GPIO%d toggled → %s\n", pin, nv ? "HIGH" : "LOW");
  }
  else if (act == "read") {
    Serial.printf("GPIO%d = %s\n", pin, digitalRead(pin) ? "HIGH" : "LOW");
  }
  else { Serial.println(RED "Unknown action. Use: on off toggle read" RESET); }
}

void cmdDisco(char** argv, uint8_t argc) {
  int cycles = (argc >= 2) ? constrain(safeAtoi(argv[1]), 1, 30)  : 3;
  int speed  = (argc >= 3) ? constrain(safeAtoi(argv[2]), 5, 500) : 40;
  const int pins[] = {2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23};
  const int nPins  = sizeof(pins) / sizeof(pins[0]);
  for (int p : pins) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  Serial.printf(MAGENTA "  *** DISCO MODE *** " RESET "cycles=%d speed=%dms\n", cycles, speed);
  for (int c = 0; c < cycles; c++) {
    for (int i = 0; i < nPins; i++) { digitalWrite(pins[i], HIGH); delay(speed); digitalWrite(pins[i], LOW); }
    for (int i = nPins - 2; i > 0; i--) { digitalWrite(pins[i], HIGH); delay(speed); digitalWrite(pins[i], LOW); }
    for (int i = 0; i < nPins; i++) { digitalWrite(pins[i], (c + i) % 2); }
    delay(speed * 4);
    for (int p : pins) digitalWrite(p, LOW);
    Serial.printf("\r  Cycle %d/%d", c + 1, cycles);
  }
  for (int p : pins) pinMode(p, INPUT);
  Serial.println(F("\n  " GREEN "Disco complete!" RESET));
}

void cmdMorse(char** argv, uint8_t argc) {
  if (argc < 3) { Serial.println(F("Usage: morse <pin> <MESSAGE>")); return; }
  int pin = resolvePin(argv[1]);
  if (pin < 0) { Serial.println(RED "Invalid pin" RESET); return; }
  pinMode(pin, OUTPUT); digitalWrite(pin, LOW);
  const char* mt[] = {".-","-...","-.-.","-..",".","..-.","--.","....","..",".---","-.-",".-..","--","-.","---",".--.","--.-",".-.","...","-","..-","...-",".--","-..-","-.--","--.."};
  Serial.printf("Morse GPIO%d: ", pin);
  for (int a = 2; a < argc; a++) {
    for (char* c = argv[a]; *c; c++) {
      char ch = toupper(*c);
      if (ch >= 'A' && ch <= 'Z') {
        Serial.print(ch);
        const char* code = mt[ch - 'A'];
        for (const char* m = code; *m; m++) {
          digitalWrite(pin, HIGH); Serial.print(*m == '.' ? '.' : '-');
          delay(*m == '.' ? 100 : 300);
          digitalWrite(pin, LOW); delay(100);
        }
        delay(300);
      } else if (ch == ' ') { Serial.print(' '); delay(700); }
    }
    Serial.print(' ');
  }
  Serial.println("Done");
}

void cmdSensor(char** argv, uint8_t argc) {
  Serial.println(F("\n  " YELLOW "Sensor Monitor (ADC — 3.3V ref, 12-bit)" RESET "\n"));
  const int pins[]  = {36, 39, 34, 35, 32, 33};
  const char* names[] = {"A0/GPIO36","A1/GPIO39","A2/GPIO34","A3/GPIO35","A4/GPIO32","A5/GPIO33"};
  for (int i = 0; i < 6; i++) {
    long sum = 0;
    for (int j = 0; j < 8; j++) { sum += analogRead(pins[i]); delay(1); }
    int avg = sum / 8;
    float v  = avg * 3.3f / 4095.0f;
    int bar  = map(avg, 0, 4095, 0, 32);
    Serial.printf("  %-12s [", names[i]);
    for (int b = 0; b < 32; b++) Serial.print(b < bar ? GREEN "█" RESET : GRAY "░" RESET);
    Serial.printf("] %4d (%.2fV)\n", avg, v);
  }
  Serial.println();
}

void cmdScope(char** argv, uint8_t argc) {
  if (argc < 2) { Serial.println(F("Usage: scope <pin> [samples=80] [delay_ms=5]")); return; }
  int pin     = resolvePin(argv[1]);
  if (pin < 0) { Serial.println(RED "Invalid pin" RESET); return; }
  int samples = (argc >= 3) ? constrain(safeAtoi(argv[2]), 10, 200) : 80;
  int dly     = (argc >= 4) ? constrain(safeAtoi(argv[3]), 1, 500)  : 5;

  int* vals = (int*)malloc(samples * sizeof(int));
  if (!vals) { Serial.println(RED "malloc failed" RESET); return; }

  Serial.printf("\n  " CYAN "Scope GPIO%d" RESET " — %d samples @ %dms\n\n", pin, samples, dly);
  for (int i = 0; i < samples; i++) { vals[i] = analogRead(pin); delay(dly); }

  int vmin = 4095, vmax = 0;
  long vsum = 0;
  for (int i = 0; i < samples; i++) {
    if (vals[i] < vmin) vmin = vals[i];
    if (vals[i] > vmax) vmax = vals[i];
    vsum += vals[i];
  }

  int height = 12;
  int range  = vmax - vmin;
  if (range == 0) range = 1;
  for (int row = height - 1; row >= 0; row--) {
    Serial.print("  ");
    for (int i = 0; i < samples && i < 100; i++) {
      int mapped = map(vals[i], vmin, vmax, 0, height - 1);
      if      (mapped == row) Serial.print(CYAN "▪" RESET);
      else if (mapped >  row) Serial.print(GRAY "│" RESET);
      else                    Serial.print(" ");
    }
    Serial.println();
  }
  Serial.printf("  Min:%d  Max:%d  Avg:%ld  (%.2f–%.2fV)\n\n",
                vmin, vmax, vsum / samples,
                vmin * 3.3f / 4095.0f, vmax * 3.3f / 4095.0f);
  free(vals);
}

void cmdMonitor(char** argv, uint8_t argc) {
  if (argc < 3) { Serial.println(F("Usage: monitor <pin> <interval_ms> [duration_s=10]")); return; }
  int pin      = resolvePin(argv[1]);
  if (pin < 0) { Serial.println(RED "Invalid pin" RESET); return; }
  int interval = max(50, safeAtoi(argv[2]));
  int duration = (argc >= 4) ? constrain(safeAtoi(argv[3]), 1, 300) : 10;
  bool isADC   = (pin == 36||pin==39||pin==34||pin==35||pin==32||pin==33||pin==25||pin==26||pin==27||pin==14);

  Serial.printf("\n  " YELLOW "Monitor GPIO%d" RESET " every %dms for %ds\n\n", pin, interval, duration);
  unsigned long end = millis() + duration * 1000UL;
  while (millis() < end) {
    unsigned long t = (millis() - bootTime) / 1000;
    if (isADC) {
      int raw = analogRead(pin);
      float v = raw * 3.3f / 4095.0f;
      Serial.printf("  [t+%lus] %d (%.3fV)\n", t, raw, v);
    } else {
      Serial.printf("  [t+%lus] %s\n", t, digitalRead(pin) ? GREEN "HIGH" RESET : GRAY "LOW" RESET);
    }
    delay(interval);
  }
  Serial.println(GREEN "  Monitor done" RESET "\n");
}

// Filesystem Commands
void cmdLS(char** argv, uint8_t argc) {
  String target = (argc >= 2) ? buildPath(argv[1]) : String(currentPath);
  if (!target.endsWith("/")) target += "/";

  Serial.println(F("\n  " YELLOW "Name                 Size    Modified" RESET));
  Serial.println(F("  " GRAY "───────────────────────────────────────────" RESET));

  int count = 0;
  File root = SPIFFS.open("/");
  File f    = root.openNextFile();
  while (f) {
    String fp = String(f.name()); // e.g. "/home/file.txt"
    // Show only direct children of target
    if (fp.startsWith(target)) {
      String rest = fp.substring(target.length());
      // Skip if rest contains another slash (grandchild)
      if (rest.length() > 0 && rest.indexOf('/') < 0) {
        bool isDir = rest.endsWith(".dir") || f.isDirectory();
        if (rest == ".dir") { f = root.openNextFile(); continue; } // skip dir markers
        Serial.print("  ");
        if (isDir) {
          Serial.print(BLUE); Serial.printf("%-22s" RESET, (rest + "/").c_str());
          Serial.println(GRAY "  <DIR>" RESET);
        } else {
          Serial.printf(WHITE "%-22s" RESET, rest.c_str());
          Serial.printf("%6d bytes\n", (int)f.size());
        }
        count++;
      }
    }
    f = root.openNextFile();
  }

  if (count == 0) Serial.println(GRAY "  (empty)" RESET);
  // Show total SPIFFS usage
  size_t total = SPIFFS.totalBytes();
  size_t used  = SPIFFS.usedBytes();
  Serial.printf("\n  " GRAY "%d items  │  SPIFFS: %u / %u KB used" RESET "\n\n",
                count, used / 1024, total / 1024);
}

void cmdCD(char** argv, uint8_t argc) {
  if (argc < 2 || strcmp(argv[1], "/") == 0) {
    strncpy(currentPath, "/", PATH_LEN - 1); return;
  }
  if (strcmp(argv[1], "..") == 0) {
    if (strcmp(currentPath, "/") == 0) return;
    String p = String(currentPath);
    if (p.endsWith("/")) p = p.substring(0, p.length() - 1);
    int last = p.lastIndexOf('/');
    strncpy(currentPath, (last <= 0 ? "/" : p.substring(0, last + 1)).c_str(), PATH_LEN - 1);
    return;
  }
  // Absolute or relative
  String target = buildPath(argv[1]);
  if (!target.endsWith("/")) target += "/";
  if (isDirectory(target.substring(0, target.length() - 1)) || target == "/") {
    strncpy(currentPath, target.c_str(), PATH_LEN - 1);
  } else {
    Serial.printf(RED "cd: '%s' not found\n" RESET, argv[1]);
  }
}

void cmdMkdir(char** argv, uint8_t argc) {
  if (argc < 2) { Serial.println(F("Usage: mkdir <name>")); return; }
  String path = buildPath(argv[1]);
  if (isDirectory(path)) { Serial.println(YELLOW "Already exists" RESET); return; }
  ensureDir(path);
  Serial.printf(GREEN "Directory '%s' created\n" RESET, argv[1]);
  klog(("mkdir " + path).c_str());
}

void cmdTouch2(char** argv, uint8_t argc) {
  if (argc < 2) { Serial.println(F("Usage: touch <filename>")); return; }
  String path = buildPath(argv[1]);
  if (!SPIFFS.exists(path)) {
    File f = SPIFFS.open(path, FILE_WRITE);
    if (!f) { Serial.println(RED "Failed to create file" RESET); return; }
    f.close();
  }
  Serial.printf("'%s' OK\n", argv[1]);
}

void cmdCat(char** argv, uint8_t argc) {
  if (argc < 2) { Serial.println(F("Usage: cat <filename>")); return; }
  String path = buildPath(argv[1]);
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) { Serial.printf(RED "File not found: %s\n" RESET, argv[1]); return; }
  if (f.size() == 0) { Serial.println(GRAY "(empty)" RESET); f.close(); return; }
  while (f.available()) {
    char c = f.read();
    Serial.write(c);
  }
  Serial.println();
  f.close();
}

void cmdWrite(char** argv, uint8_t argc) {
  // writefile <name> <content...>  or  append <name> <content>
  bool appendMode = (argc >= 1 && strcasecmp(argv[0], "append") == 0);
  if (argc < 3) {
    Serial.println(F("Usage: writefile <filename> <content>"));
    Serial.println(F("       append    <filename> <content>"));
    return;
  }
  String path = buildPath(argv[1]);
  File f = SPIFFS.open(path, appendMode ? FILE_APPEND : FILE_WRITE);
  if (!f) { Serial.println(RED "Cannot open file" RESET); return; }
  for (uint8_t i = 2; i < argc; i++) {
    f.print(argv[i]);
    if (i < argc - 1) f.print(' ');
  }
  f.println();
  f.close();
  Serial.printf("Written to '%s'\n", argv[1]);
}

void cmdRM(char** argv, uint8_t argc) {
  if (argc < 2) { Serial.println(F("Usage: rm <name> [-r]")); return; }
  String path = buildPath(argv[1]);
  bool recursive = (argc >= 3 && strcmp(argv[2], "-r") == 0);

  if (isDirectory(path)) {
    if (!recursive) { Serial.println(YELLOW "Use 'rm <dir> -r' to remove directory" RESET); return; }
    // Remove all files under this path
    String prefix = path; if (!prefix.endsWith("/")) prefix += "/";
    File root = SPIFFS.open("/");
    File fi = root.openNextFile();
    while (fi) {
      if (String(fi.name()).startsWith(prefix)) SPIFFS.remove(fi.name());
      fi = root.openNextFile();
    }
    String marker = path + "/.dir";
    SPIFFS.remove(marker);
    Serial.printf(GREEN "Removed directory '%s'\n" RESET, argv[1]);
  } else if (SPIFFS.exists(path)) {
    SPIFFS.remove(path);
    Serial.printf(GREEN "Removed '%s'\n" RESET, argv[1]);
  } else {
    Serial.printf(RED "Not found: %s\n" RESET, argv[1]);
  }
}

void cmdMv(char** argv, uint8_t argc) {
  if (argc < 3) { Serial.println(F("Usage: mv <src> <dst>")); return; }
  String src = buildPath(argv[1]);
  String dst = buildPath(argv[2]);
  if (!SPIFFS.exists(src)) { Serial.println(RED "Source not found" RESET); return; }
  // Copy then delete
  File fs_ = SPIFFS.open(src, FILE_READ);
  File fd  = SPIFFS.open(dst, FILE_WRITE);
  if (!fs_ || !fd) { Serial.println(RED "Move failed" RESET); return; }
  while (fs_.available()) fd.write(fs_.read());
  fs_.close(); fd.close();
  SPIFFS.remove(src);
  Serial.printf("Moved '%s' → '%s'\n", argv[1], argv[2]);
}

void cmdCp(char** argv, uint8_t argc) {
  if (argc < 3) { Serial.println(F("Usage: cp <src> <dst>")); return; }
  String src = buildPath(argv[1]);
  String dst = buildPath(argv[2]);
  if (!SPIFFS.exists(src)) { Serial.println(RED "Source not found" RESET); return; }
  File fs_ = SPIFFS.open(src, FILE_READ);
  File fd  = SPIFFS.open(dst, FILE_WRITE);
  if (!fs_ || !fd) { Serial.println(RED "Copy failed" RESET); return; }
  size_t bytes = 0;
  while (fs_.available()) { fd.write(fs_.read()); bytes++; }
  fs_.close(); fd.close();
  Serial.printf("Copied %u bytes → '%s'\n", bytes, argv[2]);
}

void cmdDf(char** argv, uint8_t argc) {
  size_t total = SPIFFS.totalBytes();
  size_t used  = SPIFFS.usedBytes();
  size_t free_ = total - used;
  int pct = (used * 100) / total;
  int bar = (used * 30) / total;
  Serial.println(F("\n  " YELLOW "Filesystem (SPIFFS):" RESET));
  Serial.print  (F("  ["));
  for (int i = 0; i < 30; i++) Serial.print(i < bar ? GREEN "█" RESET : GRAY "░" RESET);
  Serial.printf ("] %d%%\n", pct);
  Serial.printf ("  Total: %u KB  Used: %u KB  Free: %u KB\n\n",
                 total/1024, used/1024, free_/1024);
}

// WiFi Commands
void cmdWifi(char** argv, uint8_t argc) {
  if (argc < 2) {
    // Show status
    Serial.println(F("\n  " CYAN "WiFi Status:" RESET));
    Serial.printf("  Mode : %s\n",
      WiFi.getMode() == WIFI_STA ? "Station" :
      WiFi.getMode() == WIFI_AP  ? "Access Point" :
      WiFi.getMode() == WIFI_AP_STA ? "AP+Station" : "Off");
    if (wifiConnected) {
      Serial.printf("  SSID : %s\n", WiFi.SSID().c_str());
      Serial.printf("  IP   : %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("  RSSI : %d dBm\n", WiFi.RSSI());
      Serial.printf("  MAC  : %s\n", WiFi.macAddress().c_str());
      Serial.printf("  GW   : %s\n", WiFi.gatewayIP().toString().c_str());
      Serial.printf("  DNS  : %s\n", WiFi.dnsIP().toString().c_str());
    }
    if (apActive) {
      Serial.printf("  AP   : %s  IP: %s  Clients: %d\n",
                    apSSID.c_str(), WiFi.softAPIP().toString().c_str(),
                    WiFi.softAPgetStationNum());
    }
    Serial.println();
    return;
  }

  String sub = String(argv[1]); sub.toLowerCase();

  // wifi connect <ssid> <pass>
  if (sub == "connect" || sub == "up") {
    if (argc < 4) { Serial.println(F("Usage: wifi connect <SSID> <PASSWORD>")); return; }
    Serial.printf("  Connecting to '%s'", argv[2]);
    WiFi.mode(WIFI_STA);
    WiFi.begin(argv[2], argv[3]);
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 30) {
      delay(500); Serial.print('.'); retries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      staSSID = String(argv[2]);
      Serial.printf("\n  " GREEN "Connected! IP: %s" RESET "\n\n", WiFi.localIP().toString().c_str());
      klog(("WiFi connected: " + staSSID).c_str());
    } else {
      Serial.println(F("\n  " RED "Connection failed" RESET));
      WiFi.disconnect();
    }
    return;
  }

  // wifi disconnect
  if (sub == "disconnect" || sub == "down") {
    WiFi.disconnect(true);
    wifiConnected = false;
    Serial.println("  WiFi disconnected");
    return;
  }

  // wifi ap <ssid> [pass]  — create soft AP
  if (sub == "ap") {
    if (argc < 3) { Serial.println(F("Usage: wifi ap <SSID> [PASSWORD]")); return; }
    apSSID = String(argv[2]);
    apPASS = (argc >= 4) ? String(argv[3]) : "";
    WiFi.mode(wifiConnected ? WIFI_AP_STA : WIFI_AP);
    bool ok = apPASS.length() > 0
                ? WiFi.softAP(apSSID.c_str(), apPASS.c_str())
                : WiFi.softAP(apSSID.c_str());
    apActive = ok;
    if (ok) Serial.printf(GREEN "  AP '%s' started  IP: %s\n" RESET,
                           apSSID.c_str(), WiFi.softAPIP().toString().c_str());
    else     Serial.println(RED "  AP failed" RESET);
    return;
  }

  // wifi scan
  if (sub == "scan") {
    Serial.println(F("  Scanning..."));
    int n = WiFi.scanNetworks();
    if (n == 0) { Serial.println(F("  No networks found")); return; }
    Serial.println(F("\n  " YELLOW "#  SSID                           RSSI  ENC" RESET));
    Serial.println(F("  " GRAY "───────────────────────────────────────────────" RESET));
    for (int i = 0; i < n; i++) {
      const char* enc = WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? GREEN "Open" RESET : CYAN "WPA" RESET;
      Serial.printf("  %-2d %-32s %4d  %s\n", i + 1,
                    WiFi.SSID(i).c_str(), WiFi.RSSI(i), enc);
    }
    WiFi.scanDelete();
    Serial.println();
    return;
  }

  // wifi ping <ip_or_host>
  if (sub == "ping") {
    if (argc < 3) { Serial.println(F("Usage: wifi ping <IP|host>")); return; }
    if (!wifiConnected) { Serial.println(YELLOW "Not connected to WiFi" RESET); return; }
    IPAddress ip;
    if (!ip.fromString(argv[2])) { Serial.println(RED "Invalid IP. DNS not supported in minimal mode." RESET); return; }
    Serial.printf("  Pinging %s:\n", argv[2]);
    for (int i = 0; i < 4; i++) {
      // ESP32 Arduino core does not include ping by default; send TCP probe instead
      WiFiClient client;
      unsigned long t = millis();
      bool ok = client.connect(ip, 80);
      unsigned long rtt = millis() - t;
      client.stop();
      if (ok) Serial.printf("  seq=%d time=%lums " GREEN "reachable" RESET "\n", i, rtt);
      else    Serial.printf("  seq=%d " RED "no response" RESET "\n", i);
      delay(500);
    }
    return;
  }

  // wifi hostname [name]
  if (sub == "hostname") {
    if (argc >= 3) {
      WiFi.setHostname(argv[2]);
      Serial.printf("  Hostname set to '%s'\n", argv[2]);
    } else {
      Serial.printf("  Hostname: %s\n", WiFi.getHostname());
    }
    return;
  }

  // wifi http start [port]  — minimal web server
  if (sub == "http") {
    if (argc < 3) { Serial.println(F("Usage: wifi http <start|stop> [port]")); return; }
    String action = String(argv[2]); action.toLowerCase();
    if (action == "start") {
      if (!wifiConnected && !apActive) { Serial.println(YELLOW "Connect WiFi first" RESET); return; }
      int port = (argc >= 4) ? safeAtoi(argv[3]) : 80;
      if (httpServer) { delete httpServer; httpServer = nullptr; }
      httpServer = new WebServer(port);
      httpServer->on("/", []() {
        String html = F("<!DOCTYPE html><html><head><title>KernelESP</title>"
          "<style>body{font-family:monospace;background:#0d0d0d;color:#0f0;padding:2em}"
          "h1{color:#0ff}pre{color:#fff;border:1px solid #0f0;padding:1em;}</style></head>"
          "<body><h1>KernelESP v1.0</h1><pre>Status: Online\nHost: kernelesp\n"
          "Uptime: ");
        html += (millis() - bootTime) / 1000;
        html += F("s\nFree RAM: ");
        html += ESP.getFreeHeap() / 1024;
        html += F(" KB\n</pre>"
          "<p>Control via Serial terminal.</p></body></html>");
        if (httpServer) httpServer->send(200, "text/html", html);
      });
      httpServer->begin();
      httpRunning = true;
      Serial.printf(GREEN "  HTTP server started on port %d\n  URL: http://%s/\n" RESET,
                    port, wifiConnected ? WiFi.localIP().toString().c_str()
                                        : WiFi.softAPIP().toString().c_str());
    } else if (action == "stop") {
      if (httpServer) { httpServer->stop(); delete httpServer; httpServer = nullptr; httpRunning = false; }
      Serial.println("  HTTP server stopped");
    }
    return;
  }

  // wifi mac
  if (sub == "mac") {
    Serial.printf("  STA MAC: %s\n", WiFi.macAddress().c_str());
    Serial.printf("  AP  MAC: %s\n", WiFi.softAPmacAddress().c_str());
    return;
  }

  // wifi help
  Serial.println(F("\n  " CYAN "WiFi Commands:" RESET));
  Serial.println(F("  wifi                          Show status"));
  Serial.println(F("  wifi scan                     Scan networks"));
  Serial.println(F("  wifi connect <SSID> <PASS>    Connect to AP"));
  Serial.println(F("  wifi disconnect               Disconnect"));
  Serial.println(F("  wifi ap <SSID> [PASS]         Start Soft-AP"));
  Serial.println(F("  wifi ping <IP>                TCP connectivity check"));
  Serial.println(F("  wifi http start [port]        Start HTTP server"));
  Serial.println(F("  wifi http stop                Stop HTTP server"));
  Serial.println(F("  wifi hostname [name]          Get/set hostname"));
  Serial.println(F("  wifi mac                      Show MAC addresses\n"));
}

// Scripting
void executeCommand(char* line);  // forward declaration

void runScript(const char* text) {
  char* buf = (char*)malloc(strlen(text) + 2);
  if (!buf) { Serial.println(RED "malloc error" RESET); return; }
  strcpy(buf, text);

  char* cmd = strtok(buf, ";");
  int n = 0;
  while (cmd) {
    cmd = ltrim(cmd);
    // strip trailing whitespace
    int len = strlen(cmd);
    while (len > 0 && (cmd[len-1] == ' ' || cmd[len-1] == '\r' || cmd[len-1] == '\n')) cmd[--len] = '\0';
    if (len > 0 && cmd[0] != '#') {
      Serial.printf(GRAY "  [%d]" RESET " $ %s\n", ++n, cmd);
      executeCommand(cmd);
      delay(20);
    }
    cmd = strtok(nullptr, ";");
  }
  free(buf);
}

void cmdEval(char** argv, uint8_t argc) {
  if (argc < 2) { Serial.println(F("Usage: eval \"cmd1; cmd2; ...\"")); return; }
  String code = "";
  for (int i = 1; i < argc; i++) { if (i > 1) code += " "; code += argv[i]; }
  Serial.println(F(CYAN ">>> eval" RESET));
  runScript(code.c_str());
  Serial.println(F(CYAN ">>> done" RESET));
}

void cmdRun(char** argv, uint8_t argc) {
  if (argc < 2) { Serial.println(F("Usage: run <script_file>")); return; }
  String path = buildPath(argv[1]);
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) { Serial.printf(RED "Script not found: %s\n" RESET, argv[1]); return; }
  String content = f.readString();
  f.close();
  Serial.printf(CYAN ">>> run %s" RESET "\n", argv[1]);
  runScript(content.c_str());
  Serial.printf(CYAN ">>> done (%u bytes)" RESET "\n", content.length());
  klog(("run " + path).c_str());
}

void cmdFor(char** argv, uint8_t argc) {
  if (argc < 3) { Serial.println(F("Usage: for <count> \"<cmd>\"")); return; }
  int count = constrain(safeAtoi(argv[1]), 1, 1000);
  String cmd = "";
  for (int i = 2; i < argc; i++) { if (i > 2) cmd += " "; cmd += argv[i]; }
  for (int i = 0; i < count; i++) {
    Serial.printf(GRAY "\r  [%d/%d]" RESET, i + 1, count);
    char* buf = strdup(cmd.c_str());
    executeCommand(buf);
    free(buf);
    delay(5);
  }
  Serial.println();
}

void cmdDelay(char** argv, uint8_t argc) {
  if (argc < 2) { Serial.println(F("Usage: delay <ms>")); return; }
  int ms = constrain(safeAtoi(argv[1]), 0, 60000);
  delay(ms);
}

// System Commands
void cmdFree(char** argv, uint8_t argc) {
  uint32_t heap    = ESP.getFreeHeap();
  uint32_t minHeap = ESP.getMinFreeHeap();
  uint32_t maxAlloc= ESP.getMaxAllocHeap();
  Serial.println(F("\n  " YELLOW "Memory:" RESET));
  Serial.printf("  Free heap      : %u bytes (%u KB)\n", heap, heap / 1024);
  Serial.printf("  Min free heap  : %u bytes\n", minHeap);
  Serial.printf("  Max alloc block: %u bytes\n", maxAlloc);
  Serial.printf("  PSRAM          : %u bytes\n", ESP.getFreePsram());
  cmdDf(argv, 0);
}

void cmdSysInfo(char** argv, uint8_t argc) {
  unsigned long up  = (millis() - bootTime) / 1000;
  uint8_t h = up / 3600, m = (up % 3600) / 60, s = up % 60;

  Serial.println(F("\n"
    CYAN  "  ██████╗ ███████╗██████╗ ██████╗ ██████╗ \n"
    BCYAN "  ██╔════╝██╔════╝██╔══██╗╚════██╗╚════██╗\n"
    CYAN  "  █████╗  ███████╗██████╔╝ █████╔╝ █████╔╝\n"
    CYAN  "  ██╔══╝  ╚════██║██╔═══╝  ╚═══██╗ ██╔═══╝\n"
    BCYAN "  ███████╗███████║██║     ███████╗███████║ \n"
    GRAY  "  ╚══════╝╚══════╝╚═╝     ╚══════╝╚══════╝" RESET "\n"
  ));

  Serial.printf("  " YELLOW "OS     " RESET ": KernelESP v1.0\n");
  Serial.printf("  " YELLOW "Host   " RESET ": %s\n", HOSTNAME);
  Serial.printf("  " YELLOW "CPU    " RESET ": Xtensa LX6 Dual-Core @ %d MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("  " YELLOW "Chip   " RESET ": ESP32  Rev%d  Cores:%d\n", ESP.getChipRevision(), ESP.getChipCores());
  Serial.printf("  " YELLOW "Flash  " RESET ": %u KB  (mode:%d  speed:%d MHz)\n",
                ESP.getFlashChipSize()/1024, ESP.getFlashChipMode(), ESP.getFlashChipSpeed()/1000000);
  Serial.printf("  " YELLOW "RAM    " RESET ": %u KB free / PSRAM: %u KB\n",
                ESP.getFreeHeap()/1024, ESP.getFreePsram()/1024);
  Serial.printf("  " YELLOW "SPIFFS " RESET ": %u / %u KB\n",
                SPIFFS.usedBytes()/1024, SPIFFS.totalBytes()/1024);
  Serial.printf("  " YELLOW "Uptime " RESET ": %dh %dm %ds\n", h, m, s);
  Serial.printf("  " YELLOW "WiFi   " RESET ": %s\n",
                wifiConnected ? (GREEN + WiFi.SSID() + "  " + WiFi.localIP().toString() + RESET).c_str()
                              : RED "Offline" RESET);
  Serial.println();
}

void cmdDmesg(char** argv, uint8_t argc) {
  Serial.println(F("\n  " YELLOW "Kernel Log:" RESET "\n"));
  for (uint8_t i = 0; i < dmesgCount; i++) {
    uint8_t idx = (dmesgHead - dmesgCount + i + DMESG_LINES) % DMESG_LINES;
    Serial.printf("  " GRAY "[%4lus]" RESET " %s\n", dmesgBuf[idx].ts, dmesgBuf[idx].msg);
  }
  Serial.println();
}

void cmdReboot(char** argv, uint8_t argc) {
  Serial.println(F("\n  Rebooting...\n"));
  delay(300);
  ESP.restart();
}

void cmdWhoami(char** argv, uint8_t argc) { Serial.println("root"); }
void cmdUname(char** argv, uint8_t argc)  { Serial.println("KernelESP v1.0 ESP32 xtensa"); }
void cmdUptime(char** argv, uint8_t argc) {
  unsigned long t = (millis() - bootTime) / 1000;
  Serial.printf("up %dh %dm %ds\n", (int)(t/3600), (int)((t%3600)/60), (int)(t%60));
}
void cmdPwd(char** argv, uint8_t argc) { Serial.println(currentPath); }
void cmdEcho(char** argv, uint8_t argc) {
  for (int i = 1; i < argc; i++) { if (i > 1) Serial.print(' '); Serial.print(argv[i]); }
  Serial.println();
}
void cmdClear(char** argv, uint8_t argc) { showLogo(); }

void cmdWave(char** argv, uint8_t argc) {
  Serial.println(F("\n"
    CYAN "  ╭╮              ╭╮\n"
    CYAN "  ╭╯╰╮          ╭╯╰╮\n"
    CYAN " ╭╯  ╰╮        ╭╯  ╰╮\n"
    CYAN "╭╯    ╰╮──────╭╯    ╰╮\n"
    CYAN "╯      ╰╮    ╭╯      ╰\n" RESET
  ));
}

void cmdHelp(char** argv, uint8_t argc) {
  Serial.println(F("\n  " CYAN "KernelESP v1.0 Command Reference" RESET "\n"));

  Serial.println(F("  " GREEN "Hardware:" RESET));
  Serial.println(F("    pinmode <pin> <mode>         Set GPIO mode (input/output/pullup/pulldown)"));
  Serial.println(F("    write   <pin> <HIGH|LOW>      Digital write"));
  Serial.println(F("    read    [pin]                 Digital read (all if no pin)"));
  Serial.println(F("    aread   [pin]                 ADC read (0-4095, 3.3V)"));
  Serial.println(F("    pwm     <pin> <0-255> [freq]  LEDC PWM output"));
  Serial.println(F("    dac     <25|26> <0-255>       DAC voltage output"));
  Serial.println(F("    gpio    <pin> <on|off|toggle> Quick GPIO"));
  Serial.println(F("    tone    <pin> <hz> [ms]       Square wave tone"));
  Serial.println(F("    notone  [pin]                 Stop tone"));
  Serial.println(F("    tsense  [pin]                 Capacitive touch read"));
  Serial.println(F("    disco   [cycles] [speed]      LED show"));
  Serial.println(F("    morse   <pin> <MSG>            Morse code"));

  Serial.println(F("\n  " GREEN "Sensors:" RESET));
  Serial.println(F("    sensor                        All ADC channels with bar"));
  Serial.println(F("    scope   <pin> [n] [ms]        Oscilloscope plot"));
  Serial.println(F("    monitor <pin> <ms> [s]        Live pin monitor"));

  Serial.println(F("\n  " GREEN "Filesystem (SPIFFS — persistent):" RESET));
  Serial.println(F("    ls [dir]   cd <dir>   pwd   mkdir <name>   touch <name>"));
  Serial.println(F("    cat <f>    writefile <f> <text>   append <f> <text>"));
  Serial.println(F("    rm <name> [-r]   mv <src> <dst>   cp <src> <dst>   df"));

  Serial.println(F("\n  " GREEN "WiFi:" RESET));
  Serial.println(F("    wifi                          Status"));
  Serial.println(F("    wifi scan                     Scan networks"));
  Serial.println(F("    wifi connect <SSID> <PASS>    Connect"));
  Serial.println(F("    wifi ap <SSID> [PASS]         Soft Access Point"));
  Serial.println(F("    wifi ping <IP>                Connectivity check"));
  Serial.println(F("    wifi http start [port]        Web server"));
  Serial.println(F("    wifi mac / hostname           Info / rename"));

  Serial.println(F("\n  " GREEN "Scripting:" RESET));
  Serial.println(F("    eval \"cmd1; cmd2\"             Execute inline script"));
  Serial.println(F("    run  <script.sh>              Execute file script"));
  Serial.println(F("    for  <n> \"cmd\"                Loop n times"));
  Serial.println(F("    delay <ms>                    Wait"));

  Serial.println(F("\n  " GREEN "System:" RESET));
  Serial.println(F("    sysinfo / neofetch   uptime   free   df   dmesg"));
  Serial.println(F("    whoami   uname   echo <text>   clear   wave   reboot\n"));

  Serial.println(F("  " GRAY "Pins: GPIO0-39 (output: 0-33, input-only: 34-39)"));
  Serial.println(F("  " GRAY "ADC:  GPIO32-39 (ADC1)  DAC: GPIO25-26" RESET "\n"));
}

// Main Command Router
void executeCommand(char* line) {
  line = ltrim(line);
  if (!line || strlen(line) == 0 || line[0] == '#') return;

  char lineCopy[CMD_LEN];
  strncpy(lineCopy, line, CMD_LEN - 1);
  lineCopy[CMD_LEN - 1] = '\0';

  uint8_t argc = 0;
  parseCommand(lineCopy, args, &argc);
  if (argc == 0) return;

  strlowerBuf(args[0]);
  const char* cmd = args[0];

  // Hardware
  if      (!strcmp(cmd,"pinmode"))                          cmdPinMode(args, argc);
  else if (!strcmp(cmd,"write")  || !strcmp(cmd,"digitalwrite")) cmdDigitalWrite(args, argc);
  else if (!strcmp(cmd,"read")   || !strcmp(cmd,"digitalread"))  cmdDigitalRead(args, argc);
  else if (!strcmp(cmd,"aread")  || !strcmp(cmd,"analogread"))   cmdAnalogRead(args, argc);
  else if (!strcmp(cmd,"pwm"))                              cmdPWM(args, argc);
  else if (!strcmp(cmd,"dac"))                              cmdDAC(args, argc);
  else if (!strcmp(cmd,"gpio"))                             cmdGPIO(args, argc);
  else if (!strcmp(cmd,"tone"))                             cmdTone(args, argc);
  else if (!strcmp(cmd,"notone"))                           cmdNoTone(args, argc);
  else if (!strcmp(cmd,"tsense"))                           cmdTouch(args, argc);
  else if (!strcmp(cmd,"disco"))                            cmdDisco(args, argc);
  else if (!strcmp(cmd,"morse"))                            cmdMorse(args, argc);
  else if (!strcmp(cmd,"sensor"))                           cmdSensor(args, argc);
  else if (!strcmp(cmd,"scope"))                            cmdScope(args, argc);
  else if (!strcmp(cmd,"monitor"))                          cmdMonitor(args, argc);

  // Filesystem
  else if (!strcmp(cmd,"ls")    || !strcmp(cmd,"dir"))      cmdLS(args, argc);
  else if (!strcmp(cmd,"cd"))                               cmdCD(args, argc);
  else if (!strcmp(cmd,"pwd"))                              cmdPwd(args, argc);
  else if (!strcmp(cmd,"mkdir"))                            cmdMkdir(args, argc);
  else if (!strcmp(cmd,"touch"))                            cmdTouch2(args, argc);
  else if (!strcmp(cmd,"cat")   || !strcmp(cmd,"type"))    cmdCat(args, argc);
  else if (!strcmp(cmd,"writefile") || !strcmp(cmd,"write>")) cmdWrite(args, argc);
  else if (!strcmp(cmd,"append"))                           cmdWrite(args, argc);
  else if (!strcmp(cmd,"rm")    || !strcmp(cmd,"del"))      cmdRM(args, argc);
  else if (!strcmp(cmd,"mv"))                               cmdMv(args, argc);
  else if (!strcmp(cmd,"cp"))                               cmdCp(args, argc);
  else if (!strcmp(cmd,"df"))                               cmdDf(args, argc);
  else if (!strcmp(cmd,"echo"))                             cmdEcho(args, argc);

  // WiFi
  else if (!strcmp(cmd,"wifi"))                             cmdWifi(args, argc);

  // Scripting
  else if (!strcmp(cmd,"eval")  || !strcmp(cmd,"exec"))    cmdEval(args, argc);
  else if (!strcmp(cmd,"run")   || !strcmp(cmd,"sh"))      cmdRun(args, argc);
  else if (!strcmp(cmd,"for")   || !strcmp(cmd,"loop"))    cmdFor(args, argc);
  else if (!strcmp(cmd,"delay") || !strcmp(cmd,"sleep"))   cmdDelay(args, argc);

  // System
  else if (!strcmp(cmd,"help")  || !strcmp(cmd,"?"))       cmdHelp(args, argc);
  else if (!strcmp(cmd,"sysinfo")|| !strcmp(cmd,"neofetch")) cmdSysInfo(args, argc);
  else if (!strcmp(cmd,"dmesg") || !strcmp(cmd,"log"))     cmdDmesg(args, argc);
  else if (!strcmp(cmd,"free")  || !strcmp(cmd,"mem"))     cmdFree(args, argc);
  else if (!strcmp(cmd,"uptime"))                           cmdUptime(args, argc);
  else if (!strcmp(cmd,"whoami"))                           cmdWhoami(args, argc);
  else if (!strcmp(cmd,"uname"))                            cmdUname(args, argc);
  else if (!strcmp(cmd,"clear") || !strcmp(cmd,"cls"))     cmdClear(args, argc);
  else if (!strcmp(cmd,"reboot")|| !strcmp(cmd,"reset"))   cmdReboot(args, argc);
  else if (!strcmp(cmd,"wave"))                             cmdWave(args, argc);

  else {
    Serial.printf(RED "'%s'" RESET " not found. Type " YELLOW "'help'" RESET "\n", cmd);
  }
}

// Setup
void setup() {
  Serial.begin(115200);
  bootTime = millis();

  // Brief boot flash on GPIO2 (built-in LED most boards)
  pinMode(2, OUTPUT);
  for (int i = 0; i < 6; i++) { digitalWrite(2, !digitalRead(2)); delay(60); }
  digitalWrite(2, LOW);

  initFilesystem();

  showLogo();

  klog("KernelESP v1.0 booted");
  klog("SPIFFS OK");
  klog("Serial @ 115200");

  printPrompt();
}

// Loop
void loop() {
  // Handle HTTP server if active
  if (httpRunning && httpServer) httpServer->handleClient();

  // Serial input
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\r') continue; // ignore CR, handle LF

    if (c == '\n') {
      inputBuffer[inputLen] = '\0';
      if (inputLen > 0) {
        Serial.println();
        executeCommand(inputBuffer);
      }
      inputLen = 0;
      memset(inputBuffer, 0, CMD_LEN);
      printPrompt();

    } else if (c == 8 || c == 127) { // Backspace / DEL
      if (inputLen > 0) {
        inputLen--;
        Serial.print(F("\b \b"));
      }

    } else if (c == '\t') {
      // Tab — visual hint only (completion is handled on Python side)
      Serial.print(F(GRAY "..." RESET));

    } else if (inputLen < CMD_LEN - 1 && c >= 32 && c < 127) {
      inputBuffer[inputLen++] = c;
      Serial.print(c); // local echo
    }
  }

  // Small yield so WiFi stack can breathe
  delay(1);
}
