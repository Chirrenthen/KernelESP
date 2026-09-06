/*
   KernelESP v1.0
   A Linux-like interactive shell for the ESP32
  Tested on ESP32 - WROOM
 */

#include <Arduino.h>
#include <Console.h>
#include "argtable3/argtable3.h"
#include <WiFi.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <driver/dac.h>
#include <driver/touch_pad.h>
#include <string.h>
#include <stdlib.h>

#include <list>

// Boards
#ifdef ARDUINO_ESP32S3_DEV
#define KESP_BOARD_SET
// Avoid the pins used for PSRAM/Flash (26-32, 33-38 octal), UART0 (15-16, 43-44),
// and USB (19-20), which we might be using.
// TODO: be smarter about including/excluding those pins based on further config defines
#define KESP_GPIO_OPI_MASK (BIT33 | BIT34 | BIT35 | BIT36 | BIT37)
#define KESP_GPIO_VALID_GPIO_MASK (SOC_GPIO_VALID_GPIO_MASK & ~(0ULL | BIT15 | BIT16 | BIT19 | BIT20 | BIT26 | BIT27 | BIT28 | BIT29 | BIT30 | BIT31 | BIT32 | KESP_GPIO_OPI_MASK | BIT43 | BIT44))
const int boardAdcPins[] = {A0, A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12, A13, A14, A15, A16, A17};
const int boardTouchPins[] = {T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14};
const int boardDiscoPins[] = {2, 4, 5, 12, 13, 14, 17, 18, 21};
const int boardDacPins[] = {};
#endif

// Fallback (original "ESP32 WROOM" values)
#ifndef KESP_BOARD_SET
#define KESP_GPIO_VALID_GPIO_MASK (SOC_GPIO_VALID_GPIO_MASK & ~(0ULL | BIT0 | BIT1 | BIT3 | BIT6 | BIT7 | BIT8 | BIT9 | BIT10 | BIT11 | BIT20 | BIT37 | BIT38))
const int boardAdcPins[] = {36, 39, 34, 35, 32, 33, 25, 26, 27, 14};
const int boardTouchPins[] = {4, 0, 2, 15, 13, 12, 14, 27, 33, 32};
const int boardDiscoPins[] = {2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23};
const int boardDacPins[] = {DAC1, DAC2};
#endif

#define BOARD_ADC_COUNT (sizeof(boardAdcPins)/sizeof(int))
#define BOARD_TOUCH_COUNT (sizeof(boardTouchPins)/sizeof(int))
#define BOARD_DISCO_COUNT (sizeof(boardDiscoPins)/sizeof(int))
#define BOARD_DAC_COUNT (sizeof(boardDacPins)/sizeof(int))

#define BOARD_ADC_END (boardAdcPins+BOARD_ADC_COUNT)
#define BOARD_TOUCH_END (boardTouchPins+BOARD_TOUCH_COUNT)
#define BOARD_DISCO_END (boardDiscoPins+BOARD_DISCO_COUNT)
#define BOARD_DAC_END (boardDacPins+BOARD_DAC_COUNT)

// Take a default GPIO mask if the board didn't define one.
#ifndef KESP_GPIO_VALID_GPIO_MASK
#define KESP_GPIO_VALID_GPIO_MASK SOC_GPIO_VALID_GPIO_MASK
#endif

// Determine if a pin should be avoided/excluded for this board.
#define GPIO_AVOID(p) (p >= SOC_GPIO_PIN_COUNT || p < 0 || ((KESP_GPIO_VALID_GPIO_MASK & (1ULL << p)) == 0))

// Limits
#define CMD_LEN       256
#define MAX_ARGS      16
#define ARG_LEN       64
#define PATH_LEN      64
#define DMESG_LINES   12
#define DMESG_LEN     80

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

// Pin management
typedef uint8_t pinCap_t;
#define PC_DIN    (1 << 0)
#define PC_DOUT   (1 << 1)
#define PC_LEDPWM (1 << 2)
#define PC_TOUCH  (1 << 3)
#define PC_ANALOG (1 << 4)
#define PC_DAC    (1 << 5)

// Global State
char  currentPath[PATH_LEN] = "/";
unsigned long bootTime;
pinCap_t  pinCapabilities[SOC_GPIO_PIN_COUNT];
pinCap_t  pinAssignment[SOC_GPIO_PIN_COUNT];
String prompt;

// Kernel log
struct DmesgEntry { unsigned long ts; char msg[DMESG_LEN]; };
DmesgEntry dmesgBuf[DMESG_LINES];
uint8_t dmesgHead = 0;
uint8_t dmesgCount = 0;

// WiFi state
String Hostname       = "kernelesp";
bool   wifiConnected  = false;
bool   apActive       = false;
String staSSID        = "";
String apSSID         = Hostname + "_ap";
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

// Trim leading whitespace in-place, returns pointer into s
char* ltrim(char* s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

void setPrompt() {
  prompt = BGREEN "root@";
  prompt += Hostname;
  prompt += RESET ":" BLUE;
  prompt += currentPath;
  prompt += RESET;
  prompt += (wifiConnected ? GREEN " [" BCYAN "net" GREEN "]" : "");
  prompt += WHITE "$ " RESET;

  Console.setPrompt(prompt);
  return;
}

// Console class command adapter/convenience wrapper
template <typename T> class Command {
  private:
    // Wrap the core implementation of a command in boiler plate that
    // does the common parsing and error handling in a re-entrant safe
    // way, so that the implementation itself only has to focus on the
    // details of the command itself.
    static int execute(int argc, char** argv, void(*setArgs)(T&), int(*implementation)(int argc, char** argv, T& args)) {
      T args;
      int retval = 0;
      setArgs(args);
      if (arg_parse(argc, argv, (void**)&args) != 0) {
        arg_print_errors(stderr, args.end, argv[0]);
        retval = -1;
      } else {
        retval = implementation(argc, argv, args);
      }

      arg_freetable((void**)&args, sizeof(T)/sizeof(void*));
      return retval;
    }
  public:
    // Wrap Console.addCmd so that aliases can be registered without
    // room for copy/paste or incomplete maintenance errors.
    static void addCmd(std::list<const char*> names, const char* description, T &help) {
      T::setArgs(help);
      for (const char* name : names) {
        Console.addCmd(name, description, (void*)&help, Command<T>::wrapper);
      }
    }

    // Adapt the internal execute command, to the external Console callback API.
    static int wrapper(int argc, char** argv) {
      return Command<T>::execute(argc, argv, T::setArgs, T::implementation);
    }
};

// Basic command implementation template
/*
struct cmdNoop_args {
  // Add all command arguments here (in implicit order)
  arg_end_t *end;
  static void setArgs(struct cmdNoop_args &args) {
    // Set all arg_xxx_t* that were defined
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdNoop_args &args) {
    // Real command implementation goes here.
    return 0;
  }
};
static struct cmdNoop_args cmdNoopHelp;
*/


// Pin Resolution
void initPins() {
  for (int i = 0; i < SOC_GPIO_PIN_COUNT; i++) {
    pinAssignment[i] = 0;
    pinCapabilities[i] = 0;
    // Avoid pins get no assignment, and no capabilities.
    if (GPIO_AVOID(i)) { continue; }

    pinAssignment[i] = PC_DIN;   // All pins start as digital inputs.
    pinCapabilities[i] = PC_DIN; // All pins are valid digital inputs.
    // Mark pins capable of digital output.
    if (GPIO_IS_VALID_OUTPUT_GPIO(i)) {
      // All pins capable of digital output are also capable of LED PWM
      pinCapabilities[i] |= PC_DOUT;
      pinCapabilities[i] |= PC_LEDPWM;
    }
  }

  for (int i = 0; i < BOARD_ADC_COUNT; i++) {
      // Only add PC_ANALOG to un-avoided pins
      pinCapabilities[i] |= pinCapabilities[i] ? PC_ANALOG : 0;
  }

  for (int i = 0; i < BOARD_TOUCH_COUNT; i++) {
      // Only add PC_TOUCH to un-avoided pins
      pinCapabilities[i] |= pinCapabilities[i] ? PC_TOUCH : 0;
  }

  for (int i = 0; i < BOARD_DAC_COUNT; i++) {
    // Only add PC_DAC to un-avoided pins
    pinCapabilities[i] |= pinCapabilities[i] ? PC_DAC : 0;
  }
}

// Handle converting names to pin numbers (An, Tn, DACn), and checking
// if a given pin has any of the requested capabilities (output, ADC, 
// PWM, touch)
int resolvePin(const char* name, uint32_t capability_mask = 0xFFFFFFFF) {
  if (!name) return -1;
  int ch = -2;
  // No prefix -> plain pin number
  if (name[0] >= '0' && name[0] <= '9') { ch = safeAtoi(name); }
  // A prefix -> translate to a pin number (analog input)
  else if ((name[0] == 'A' || name[0] == 'a') && name[1]) {
    ch = safeAtoi(name + 1);
    ch = (ch < BOARD_ADC_COUNT) ? boardAdcPins[ch] : -1;
  }
  // T prefix -> translate to a pin number (touch input)
  else if ((name[0] == 'T' || name[0] == 't') && name[1]) {
    ch = safeAtoi(name + 1);
    ch = (ch < BOARD_TOUCH_COUNT) ? boardTouchPins[ch] : -1;
  }
  else if (
               (name [0] == 'D' || name[0] == 'd') &&
    name[1] && (name[1] == 'A' || name[1] == 'a') &&
    name[2] && (name[2] == 'C' || name[2] == 'c') &&
    name[3]) {
      ch = safeAtoi(name + 3);
      ch = (ch < BOARD_DAC_COUNT) ? boardDacPins[ch] : -1;
  }

  // Bail out if we know we have an invalid pin
  if (ch == -1) { return -1; }


  // We completed a translation; mask out any forbidden pins.
  if (ch >= 0) {
    return ((pinAssignment[ch] & capability_mask) == 0) ? -1 : ch;
  }

  // We did not complete a translation, keep parsing...
  // Note this is NOT typed or managed the way other outputs are!
#ifdef LED_BUILTIN
  if (strcasecmp(name, "LED") == 0) return LED_BUILTIN;   // Built-in LED most boards
#endif
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
    printf(RED "SPIFFS mount failed — formatting..." RESET"\n");
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
  printf("\033[2J\033[H"); // clear screen, home
  printf(CYAN
    "  ██╗  ██╗███████╗██████╗ ███╗   ██╗███████╗██╗     ███████╗███████╗██████╗ "  RESET "\n");
  printf(CYAN
    "  ██║ ██╔╝██╔════╝██╔══██╗████╗  ██║██╔════╝██║     ██╔════╝██╔════╝██╔══██╗" RESET "\n");
  printf(BCYAN
    "  █████╔╝ █████╗  ██████╔╝██╔██╗ ██║█████╗  ██║     █████╗  ███████╗██████╔╝" RESET "\n");
  printf(CYAN
    "  ██╔═██╗ ██╔══╝  ██╔══██╗██║╚██╗██║██╔══╝  ██║     ██╔══╝  ╚════██║██╔═══╝ " RESET "\n");
  printf(CYAN
    "  ██║  ██╗███████╗██║  ██║██║ ╚████║███████╗███████╗███████╗███████║██║      " RESET "\n");
  printf(GRAY
    "  ╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝╚═╝  ╚═══╝╚══════╝╚══════╝╚══════╝╚══════╝╚═╝  v1.0" RESET "\n");
  printf("\n");

  // System info bar
  uint32_t heap = ESP.getFreeHeap();
  uint32_t flash = ESP.getFlashChipSize() / 1024;
  printf(GRAY "  Board: " WHITE ARDUINO_BOARD" @ %dMHz  " GRAY
                "RAM: " WHITE "%u KB free  " GRAY
                "Flash: " WHITE "%u KB" RESET "\n",
                ESP.getCpuFreqMHz(), heap / 1024, flash);

  // WiFi status
  if (wifiConnected) {
    printf(GRAY "  WiFi: " GREEN "Connected  " GRAY "IP: " WHITE "%s" RESET "\n",
                  WiFi.localIP().toString().c_str());
  } else if (apActive) {
    printf(GRAY "  WiFi: " YELLOW "AP Mode  " GRAY "IP: " WHITE "%s" RESET "\n",
                  WiFi.softAPIP().toString().c_str());
  } else {
    printf(GRAY "  WiFi: " RED "Offline" RESET "\n");
  }

  printf(GRAY "  ─────────────────────────────────────────────────────────────────────────" RESET "\n");
  printf(GRAY "  Type " YELLOW "'help'" GRAY " for commands.  " YELLOW "'wifi help'" GRAY " for network commands." RESET "\n\n");
}

// Hardware Commands
struct cmdPinMode_args {
  arg_str_t *pin;
  arg_str_t *mode;
  arg_end_t *end;
  static void setArgs(struct cmdPinMode_args &args) {
    args.pin = arg_str1(NULL, NULL, "<pin>", "The pin to set the mode on");
    args.mode = arg_str1(NULL, NULL, "<input|output|pullup|pulldown|analog|touch|ledpwm|dac>", "The mode to assign to the pin");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdPinMode_args &args) {
    int pin = resolvePin(args.pin->sval[0]);
    if (pin < 0) { printf(RED "Invalid pin" RESET"\n"); return -1; }
    String m = String(args.mode->sval[0]); m.toLowerCase();
    if (m == "output" || m == "out") {
      if ((pinCapabilities[pin] & PC_DOUT) == 0) { printf(RED "Invalid pin" RESET"\n"); return -1; }
      pinAssignment[pin] = PC_DOUT;
      pinMode(pin, OUTPUT);
      printf("GPIO%d → OUTPUT\n", pin);
    }
    else if (m == "input") {
      if ((pinCapabilities[pin] & PC_DIN) == 0) { printf(RED "Invalid pin" RESET"\n"); return -1; }
      pinAssignment[pin] = PC_DIN;
      pinMode(pin, INPUT);
      printf("GPIO%d → INPUT\n", pin);
    }
    else if (m == "pullup"   || m == "input_pullup") {
      if ((pinCapabilities[pin] & PC_DIN) == 0) { printf(RED "Invalid pin" RESET"\n"); return -1; }
      pinAssignment[pin] = PC_DIN;
      pinMode(pin, INPUT_PULLUP);
      printf("GPIO%d → INPUT_PULLUP\n", pin);
    }
    else if (m == "pulldown" || m == "input_pulldown") {
      if ((pinCapabilities[pin] & PC_DIN) == 0) { printf(RED "Invalid pin" RESET"\n"); return -1; }
      pinAssignment[pin] = PC_DIN;
      pinMode(pin, INPUT_PULLDOWN);
      printf("GPIO%d → INPUT_PULLDOWN\n", pin);
    }
    else if (m == "analog") {
      if ((pinCapabilities[pin] & PC_ANALOG) == 0) { printf(RED "Invalid pin" RESET"\n"); return -1; }
      pinAssignment[pin] = PC_ANALOG;
      analogRead(pin);
      printf("GPIO%d → ANALOG INPUT\n", pin);
    }
    else if (m == "touch") {
      if ((pinCapabilities[pin] & PC_TOUCH) == 0) { printf(RED "Invalid pin" RESET"\n"); return -1; }
      pinAssignment[pin] = PC_TOUCH;
      touchRead(pin);
      printf("GPIO%d → TOUCH INPUT\n", pin);
    }
    else if (m == "ledpwm") {
      if ((pinCapabilities[pin] & PC_LEDPWM) == 0) { printf(RED "Invalid pin" RESET"\n"); return -1; }
      pinAssignment[pin] = PC_LEDPWM;
      pinMode(pin, OUTPUT);
      printf("GPIO%d → LED PWM OUTPUT\n", pin);
    }
    else if (m == "dac") {
      if ((pinCapabilities[pin] & PC_DAC) == 0) { printf(RED "Invalid pin" RESET"\n"); return -1; }
      pinAssignment[pin] = PC_DAC;
      pinMode(pin, OUTPUT);
      printf("GPIO%d → ANALOG OUTPUT\n", pin);
    }
    else { printf(RED "Unknown mode" RESET"\n"); return -1; }
    char buf[48]; snprintf(buf, sizeof(buf), "pinMode GPIO%d %s", pin, argv[2]); klog(buf);
    return 0;
  }
};
static struct cmdPinMode_args cmdPinModeHelp;

struct cmdDigitalWrite_args {
  arg_str_t *pin;
  arg_str_t *value;
  arg_end_t *end;
  static void setArgs(struct cmdDigitalWrite_args &args) {
    args.pin = arg_str1(NULL, NULL, "<pin>", "Pin to write to");
    args.value = arg_str1(NULL, NULL, "<HIGH|LOW|1|0|on|off>", "Value to write");
    args.end = arg_end(2);
  }
    static int implementation(int argc, char** argv, struct cmdDigitalWrite_args &args) {
    int pin = resolvePin(args.pin->sval[0], PC_DOUT);
    if (pin < 0) { printf(RED "Invalid pin" RESET"\n"); return -1; }
    String v = String(args.value->sval[0]); v.toLowerCase();
    int val = (v == "high" || v == "1" || v == "on") ? HIGH : LOW;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, val);
    printf("GPIO%d = %s\n", pin, val ? "HIGH" : "LOW");
    return 0;
  }
};
static struct cmdDigitalWrite_args cmdDigitalWriteHelp;

struct cmdDigitalRead_args {
  arg_str_t *pin;
  arg_end_t *end;
  static void setArgs(struct cmdDigitalRead_args &args) {
    args.pin = arg_str0(NULL, NULL, "<pin>", "Pin to read");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdDigitalRead_args &args) {
    if (!(args.pin->count)) {
      printf("\n  " YELLOW "GPIO State:" RESET "\n\n");
      printf("  Pin     State  │  Pin  State\n");
      printf("  ───────────────┼─────────────\n");
      int printcount = 0;
      for (int i = 0; i < SOC_GPIO_PIN_COUNT; i++) {
        // Only display pins configured for digital input or output
        if ((pinAssignment[i] & (PC_DIN | PC_DOUT)) == 0) { continue; }
        int v = digitalRead(i);
        printf("  GPIO%-2d  %s%-5s" RESET, i, v ? ((pinAssignment[i] == PC_DIN) ? GREEN : RED) : ((pinAssignment[i] == PC_DIN) ? GRAY : WHITE), v ? "HIGH" : "LOW");
        if (printcount % 2 == 0) printf("  │");
        else printf("\n");
        printcount++;
      }
      printf("\n\n");
    } else {
      int pin = resolvePin(args.pin->sval[0], PC_DIN | PC_DOUT);
      if (pin < 0) { printf(RED "Invalid pin" RESET"\n"); return -1; }
      int v = digitalRead(pin);
      printf("GPIO%d = %s%s" RESET "\n", pin, v ? ((pinAssignment[pin] == PC_DIN) ? GREEN : RED) : ((pinAssignment[pin] == PC_DIN) ? GRAY : WHITE), v ? "HIGH" : "LOW");
    }
    return 0;
  }
};
static struct cmdDigitalRead_args cmdDigitalReadHelp;

struct cmdAnalogRead_args {
  arg_str_t *pin;
  arg_end_t *end;
  static void setArgs(struct cmdAnalogRead_args &args) {
    args.pin = arg_str0(NULL, NULL, "<pin>", "Pin to read");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdAnalogRead_args &args) {
    // ADC on ESP32: 12-bit (0-4095), 3.3V reference
    if (!(args.pin->count)) {
      printf("\n  " YELLOW "ADC Channels:" RESET "\n\n");
      for (int i = 0; i < BOARD_ADC_COUNT; i++) {
        // Skip any pins we shouldn't use.
        if ((pinAssignment[boardAdcPins[i]] & PC_ANALOG) == 0) { continue; }
        int raw = analogRead(boardAdcPins[i]);
        float v  = raw * 3.3f / 4095.0f;
        int bar  = map(raw, 0, 4095, 0, 30);
        printf("  A%-2d/GPIO%-2d [", i, boardAdcPins[i]);
        for (int b = 0; b < 30; b++) printf(b < bar ? GREEN "█" RESET : GRAY "░" RESET);
        printf("] %4d (%.2fV)\n", raw, v);
      }
      printf("\n");
    } else {
      int pin = resolvePin(args.pin->sval[0], PC_ANALOG);
      if (pin < 0) { printf(RED "Invalid pin" RESET"\n"); return -1; }
      int raw = analogRead(pin);
      float v  = raw * 3.3f / 4095.0f;
      printf("GPIO%d ADC = %d (%.3fV / 3.3V)\n", pin, raw, v);
    }
    return 0;
  }
};
static struct cmdAnalogRead_args cmdAnalogReadHelp;

struct cmdPWM_args {
  arg_str_t *pin;
  arg_int_t *duty;
  arg_int_t *freq;
  arg_int_t *channel;
  arg_end_t *end;
  static void setArgs(struct cmdPWM_args &args) {
    args.pin = arg_str1(NULL, NULL, "<pin>", "Output pin");
    args.duty = arg_int1(NULL, NULL, "<duty>", "Duty cycle 0-255");
    args.freq = arg_int0(NULL, NULL, "[freq_hz]", "Frequency");
    args.channel = arg_int0(NULL, NULL, "[channel]", "PWM channel 0-15");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdPWM_args &args) {
    // ESP32 LEDC: channel-based PWM
    int pin  = resolvePin(args.pin->sval[0], PC_LEDPWM);
    if (pin < 0) { printf(RED "Invalid pin" RESET"\n"); return -1; }
    int duty = constrain(args.duty->ival[0], 0, 255);
    int freq = (args.freq->count) ? args.freq->ival[0] : 5000;
    int ch   = (args.channel->count) ? constrain(args.channel->ival[0], 0, 15) : 0;
    ledcAttachChannel(pin, freq, 8, ch);
    ledcWrite(pin, duty);
    float pct = duty * 100.0f / 255.0f;
    printf("GPIO%d PWM ch%d: duty=%d (%.0f%%) freq=%dHz\n", pin, ch, duty, pct, freq);
    return 0;
  }
};
static struct cmdPWM_args cmdPWMHelp;

#ifdef SOC_DAC_SUPPORTED
struct cmdDac_args {
  arg_str_t *pin;
  arg_int_t *value;
  arg_end_t *end;
  static void setArgs(struct cmdDac_args &args) {
    args.pin = arg_str1(NULL, NULL, "<pin>", "The DAC pin to output to");
    args.value = arg_int1(NULL, NULL, "<value>", "The analog value to output");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdDac_args &args) {
    int pin = resolvePin(args.pin->sval[0], PC_DAC);
    if (pin < 0) { printf(RED "Invalid pin" RESET "\n"); return -1; }
    int val = constrain(args.value->ival[0], 0, 255);
    dacWrite(pin, val);
    float v = val * 3.3f / 255.0f;
    printf("DAC GPIO%d = %d (%.3fV)\n", pin, val, v);
    return 0;
  }
};
static struct cmdDac_args cmdDacHelp;
#endif

struct cmdTouch_args {
  arg_str_t *pin;
  arg_end_t *end;
  static void setArgs(struct cmdTouch_args &args) {
    args.pin = arg_str0(NULL, NULL, "<pin>", "The pin to check status on.");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdTouch_args &args) {
    if (args.pin->count <= 0) {
      printf("\n  " YELLOW "Touch Sensor Readings:" RESET "\n\n");
      for (int i = 0; i < BOARD_TOUCH_COUNT; i++) {
        // Only read out pins configured for touch input
        if (pinAssignment[boardTouchPins[i]] != PC_TOUCH) { continue; }
        uint16_t val = touchRead(boardTouchPins[i]);
        int bar = map(constrain(val, 0, 80), 0, 80, 30, 0);
        bool touched = val < 30;
        printf("  T%-2d/GPIO%-2d [", i, boardTouchPins[i]);
        for (int b = 0; b < 30; b++)
          printf(b < bar ? (touched ? RED "█" RESET : CYAN "█" RESET) : GRAY "░" RESET);
        printf("] %3d  %s\n", val, touched ? RED "<TOUCH>" RESET : "");
      }
      printf("\n");
    } else {
      int pin = resolvePin(args.pin->sval[0], PC_TOUCH);
      if (pin < 0) { printf(RED "Invalid pin" RESET "\n"); return -1; }
      uint16_t val = touchRead(pin);
      printf("Touch GPIO%d = %d (%s)\n", pin, val, val < 30 ? RED "TOUCHED" RESET : "not touched");
    }
    return 0;
  }
};
static struct cmdTouch_args cmdTouchHelp;


struct cmdTone_args {
  arg_str_t *pin;
  arg_int_t *freq;
  arg_int_t *duration;
  arg_end_t *end;
  static void setArgs(struct cmdTone_args &args) {
    args.pin = arg_str1(NULL, NULL, "<pin>", "Output pin");
    args.freq = arg_int1(NULL, NULL, "<freq_hz>", "Tone frequency in Hz");
    args.duration = arg_int0(NULL, NULL, "duration_ms", "Duration in milliseconds");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdTone_args &args) {
    int pin  = resolvePin(args.pin->sval[0], PC_LEDPWM);
    if (pin < 0) { printf(RED "Invalid pin" RESET"\n"); return -1; }
    int freq = args.freq->ival[0];
    if (freq < 1 || freq > 20000) { printf(RED "Frequency: 1-20000 Hz" RESET"\n"); return -1; }
    ledcAttachChannel(pin, freq, 8, 14);
    ledcWrite(pin, 127); // 50% duty = square wave
    if (args.duration->count > 0) {
      int ms = args.duration->ival[0];
      printf("Tone GPIO%d: %dHz for %dms\n", pin, freq, ms);
      delay(ms);
      ledcWrite(pin, 0);
      ledcDetach(pin);
    } else {
      printf("Tone GPIO%d: %dHz continuous. Use 'notone %s' to stop.\n", pin, freq, argv[1]);
    }
    return 0;
  }
};
static struct cmdTone_args cmdToneHelp;

struct cmdNoTone_args {
  arg_str_t *pin;
  arg_end_t *end;
  static void setArgs(struct cmdNoTone_args &args) {
    args.pin = arg_str0(NULL, NULL, "<pin>", "The pin to disable tones on");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdNoTone_args &args) {
    int pin = (args.pin->count > 0) ? resolvePin(args.pin->sval[0], PC_LEDPWM) : -1;
    ledcWrite(pin, 0);
    if (pin >= 0) ledcDetach(pin);
    printf("Tone stopped\n");
    return 0;
  }
};
static struct cmdNoTone_args cmdNoToneHelp;

struct cmdGPIO_args {
  arg_str_t *pin;
  arg_str_t *cmd;
  arg_end_t *end;
  static void setArgs(struct cmdGPIO_args &args) {
    args.pin = arg_str1(NULL, NULL, "<pin>", "Pin to act on");
    args.cmd = arg_str1(NULL, NULL, "<on|off|toggle|read>", "Action to perform");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdGPIO_args &args) {
    int pin = resolvePin(args.pin->sval[0], PC_DOUT);
    if (pin < 0) { printf(RED "Invalid pin" RESET"\n"); return -1; }
    String act = String(args.cmd->sval[0]); act.toLowerCase();
    if (act == "on"  || act == "1" || act == "high") { pinMode(pin, OUTPUT); digitalWrite(pin, HIGH); printf("GPIO%d → ON\n", pin); }
    else if (act == "off" || act == "0" || act == "low") { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); printf("GPIO%d → OFF\n", pin); }
    else if (act == "toggle") {
      pinMode(pin, OUTPUT);
      int nv = !digitalRead(pin); digitalWrite(pin, nv);
      printf("GPIO%d toggled → %s\n", pin, nv ? "HIGH" : "LOW");
    }
    else if (act == "read") {
      printf("GPIO%d = %s\n", pin, digitalRead(pin) ? "HIGH" : "LOW");
    }
    else { printf(RED "Unknown action. Use: on off toggle read" RESET"\n"); }
    return 0;
  }
};
static struct cmdGPIO_args cmdGPIOHelp;

struct cmdDisco_args {
  arg_int_t *cycles;
  arg_int_t *speed;
  arg_end_t *end;
  static void setArgs(struct cmdDisco_args &args) {
    args.cycles = arg_int0(NULL, NULL, "<cycles>", "The number of disco cycles to perform");
    args.speed = arg_int0(NULL, NULL, "<speed>", "Delay in ms between pattern changes");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdDisco_args &args) {
    int cycles = (args.cycles->count > 0) ? constrain(args.cycles->ival[0], 1, 30)  : 3;
    int speed  = (args.speed->count > 0) ? constrain(args.speed->ival[0], 5, 500) : 40;
    const int nPins  = BOARD_DISCO_COUNT;
    for (int p : boardDiscoPins) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
    printf(MAGENTA "  *** DISCO MODE *** " RESET "cycles=%d speed=%dms\n", cycles, speed);
    for (int c = 0; c < cycles; c++) {
      for (int i = 0; i < nPins; i++) { digitalWrite(boardDiscoPins[i], HIGH); delay(speed); digitalWrite(boardDiscoPins[i], LOW); }
      for (int i = nPins - 2; i > 0; i--) { digitalWrite(boardDiscoPins[i], HIGH); delay(speed); digitalWrite(boardDiscoPins[i], LOW); }
      for (int i = 0; i < nPins; i++) { digitalWrite(boardDiscoPins[i], (c + i) % 2); }
      delay(speed * 4);
      for (int p : boardDiscoPins) digitalWrite(p, LOW);
      printf("\r  Cycle %d/%d", c + 1, cycles);
    }
    for (int p : boardDiscoPins) pinMode(p, INPUT);
    printf("\n  " GREEN "Disco complete!" RESET"\n");
    return 0;
  }
};
static struct cmdDisco_args cmdDiscoHelp;

struct cmdMorse_args {
  arg_str_t *pin;
  arg_str_t *message;
  arg_end_t *end;
  static void setArgs(struct cmdMorse_args &args) {
    args.pin = arg_str1(NULL, NULL, "<pin>", "Pin to write to");
    args.message = arg_strn(NULL, NULL, "<message>", 1, MAX_ARGS - 1, "The message to output");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdMorse_args &args) {
    int pin = resolvePin(args.pin->sval[0], PC_DOUT);
    if (pin < 0) { printf(RED "Invalid pin" RESET "\n"); return -1; }
    pinMode(pin, OUTPUT); digitalWrite(pin, LOW);
    const char* mt[] = {".-","-...","-.-.","-..",".","..-.","--.","....","..",".---","-.-",".-..","--","-.","---",".--.","--.-",".-.","...","-","..-","...-",".--","-..-","-.--","--.."};
    printf("Morse GPIO%d: ", pin);
    for (int a = 0; a < args.message->count; a++) {
      for (const char* c = args.message->sval[a]; *c; c++) {
        char ch = toupper(*c);
        if (ch >= 'A' && ch <= 'Z') {
          printf("%c", ch);
          const char* code = mt[ch - 'A'];
          for (const char* m = code; *m; m++) {
            digitalWrite(pin, HIGH); printf(*m == '.' ? "." : "-");
            delay(*m == '.' ? 100 : 300);
            digitalWrite(pin, LOW); delay(100);
          }
          delay(300);
        } else if (ch == ' ') { printf(" "); delay(700); }
      }
      printf(" ");
    }
    printf("Done\n");
    return 0;
  }
};
static struct cmdMorse_args cmdMorseHelp;

struct cmdSensor_args {
  arg_end_t *end;
  static void setArgs(struct cmdSensor_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdSensor_args &args) {
    printf("\n  " YELLOW "Sensor Monitor (ADC — 3.3V ref, 12-bit)" RESET "\n\n");
    for (int i = 0; i < BOARD_ADC_COUNT; i++) {
      // Only read out pins configured for analog input
      if (pinAssignment[boardAdcPins[i]] != PC_ANALOG) { continue; }
      long sum = 0;
      for (int j = 0; j < 8; j++) { sum += analogRead(boardAdcPins[i]); delay(1); }
      int avg = sum / 8;
      float v  = avg * 3.3f / 4095.0f;
      int bar  = map(avg, 0, 4095, 0, 32);
      printf("  A%-2d/GPIO%-2d [", i, boardAdcPins[i]);
      for (int b = 0; b < 32; b++) printf(b < bar ? GREEN "█" RESET : GRAY "░" RESET);
      printf("] %4d (%.2fV)\n", avg, v);
    }
    printf("\n");
    return 0;
  }
};
static struct cmdSensor_args cmdSensorHelp;

struct cmdScope_args {
  arg_str_t *pin;
  arg_int_t *samples;
  arg_int_t *adelay;
  arg_end_t *end;
  static void setArgs(struct cmdScope_args &args) {
    args.pin = arg_str1(NULL, NULL, "<pin>", "The pin to monitor");
    args.samples = arg_int0(NULL, NULL, "samples", "The numble of samples to collect");
    args.adelay = arg_int0(NULL, NULL, "delay_ms", "Delay between samples in ms");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdScope_args &args) {
    int pin     = resolvePin(args.pin->sval[0], PC_ANALOG);
    if (pin < 0) { printf(RED "Invalid pin" RESET "\n"); return -1; }
    int samples = (args.samples->count > 0) ? constrain(args.samples->ival[0], 10, 200) : 80;
    int dly     = (args.adelay->count > 0) ? constrain(args.adelay->ival[0], 1, 500)  : 5;

    int* vals = (int*)malloc(samples * sizeof(int));
    if (!vals) { printf(RED "malloc failed" RESET "\n"); return -1; }

    printf("\n  " CYAN "Scope GPIO%d" RESET " — %d samples @ %dms\n\n", pin, samples, dly);
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
      printf("  ");
      for (int i = 0; i < samples && i < 100; i++) {
        int mapped = map(vals[i], vmin, vmax, 0, height - 1);
        if      (mapped == row) printf(CYAN "▪" RESET);
        else if (mapped >  row) printf(GRAY "│" RESET);
        else                    printf(" ");
      }
      printf("\n");
    }
    printf("  Min:%d  Max:%d  Avg:%ld  (%.2f–%.2fV)\n\n",
                  vmin, vmax, vsum / samples,
                  vmin * 3.3f / 4095.0f, vmax * 3.3f / 4095.0f);
    free(vals);
    return 0;
  }
};
static struct cmdScope_args cmdScopeHelp;

struct cmdMonitor_args {
  arg_str_t *pin;
  arg_int_t *interval;
  arg_int_t *duration;
  arg_end_t *end;
  static void setArgs(struct cmdMonitor_args &args) {
    args.pin = arg_str1(NULL, NULL, "<pin>", "Pin to monitor");
    args.interval = arg_int1(NULL, NULL, "<interval_ms>", "The interval between samples in ms");
    args.duration = arg_int0(NULL, NULL, "duration_s", "The total time to monitor in s");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdMonitor_args &args) {
    int pin      = resolvePin(args.pin->sval[0], PC_ANALOG | PC_DIN);
    if (pin < 0) { printf(RED "Invalid pin" RESET "\n"); return -1; }
    int interval = max(50, args.interval->ival[0]);
    int duration = (args.duration->count) ? constrain(args.duration->ival[0], 1, 300) : 10;

    printf("\n  " YELLOW "Monitor GPIO%d" RESET " every %dms for %ds\n\n", pin, interval, duration);
    unsigned long end = millis() + duration * 1000UL;
    while (millis() < end) {
      unsigned long t = (millis() - bootTime) / 1000;
      if (pinAssignment[pin] == PC_ANALOG) {
        int raw = analogRead(pin);
        float v = raw * 3.3f / 4095.0f;
        printf("  [t+%lus] %d (%.3fV)\n", t, raw, v);
      } else {
        printf("  [t+%lus] %s\n", t, digitalRead(pin) ? GREEN "HIGH" RESET : GRAY "LOW" RESET);
      }
      delay(interval);
    }
    printf(GREEN "  Monitor done" RESET "\n\n");
    return 0;
  }
};
static struct cmdMonitor_args cmdMonitorHelp;

struct cmdPins_args {
  arg_end_t *end;
  static void setArgs(struct cmdPins_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdPins_args &args) {
    printf("\n  " YELLOW "Pin configuration:\n" RESET);
    printf("  Pin    │ Capabilities │ Mode\n");
    printf("  ───────┼──────────────┼───────────────\n");
    for (int i = 0; i < SOC_GPIO_PIN_COUNT; i++) {
      // Skip disabled/unavailable pins
      if (pinAssignment[i] == 0) { continue; }
      const char* mode;
      switch(pinAssignment[i]) {
        case PC_DIN: mode = "Digital input"; break;
        case PC_DOUT: mode = "Digital output"; break;
        case PC_LEDPWM: mode = "LED PWM output"; break;
        case PC_TOUCH: mode = "Touch input"; break;
        case PC_ANALOG: mode = "Analog input"; break;
        default: mode = RED "ERROR" RESET; break;
      }
      printf("  GPIO%-2d │ %si%so%sl%st%sa" RESET "        │ %s\n",
        i,
        (pinCapabilities[i] & PC_DIN) ? GREEN : GRAY,
        (pinCapabilities[i] & PC_DOUT) ? GREEN : GRAY,
        (pinCapabilities[i] & PC_LEDPWM) ? GREEN : GRAY,
        (pinCapabilities[i] & PC_TOUCH) ? GREEN : GRAY,
        (pinCapabilities[i] & PC_ANALOG) ? GREEN : GRAY,
        mode);
    }
    return 0;
  }
};
static struct cmdPins_args cmdPinsHelp;

// Filesystem Commands
struct cmdLs_args {
  arg_str_t *dst;
  arg_end_t *end;
  static void setArgs(struct cmdLs_args &args) {
    args.dst = arg_str0(NULL, NULL, "[dst]", "Directory to list the contents of");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdLs_args &args) {
    String target = (args.dst->count) ? buildPath(args.dst->sval[0]) : String(currentPath);
    if (!target.endsWith("/")) target += "/";

    printf("\n  " YELLOW "Name                 Size    Modified" RESET"\n");
    printf("  " GRAY "───────────────────────────────────────────" RESET"\n");

    int count = 0;

    File root = SPIFFS.open("/");
    File f    = root.openNextFile();
    while (f) {
      String fp = String(f.path()); // e.g. "/home/file.txt"
      // Show only direct children of target
      if (fp.startsWith(target)) {
        String rest = fp.substring(target.length());
        bool isDir = rest.endsWith(".dir") || f.isDirectory();
        if(isDir) rest = rest.substring(0, rest.length() - 5);
        // Skip if rest contains another slash (grandchild)
        if (rest.length() > 0 && rest.indexOf('/') < 0) {
          if (rest == ".dir") { f = root.openNextFile(); continue; } // skip dir markers
          printf("  ");
          if (isDir) {
            printf(BLUE); printf("%-22s" RESET, (rest + "/").c_str());
            printf(GRAY "  <DIR>" RESET"\n");
          } else {
            printf(WHITE "%-22s" RESET, rest.c_str());
            printf("%6d bytes\n", (int)f.size());
          }
          count++;
        }
      }
      f = root.openNextFile();
    }

    if (count == 0) printf(GRAY "  (empty)" RESET"\n");
    // Show total SPIFFS usage
    size_t total = SPIFFS.totalBytes();
    size_t used  = SPIFFS.usedBytes();
    printf("\n  " GRAY "%d items  │  SPIFFS: %u / %u KB used" RESET "\n\n",
                  count, used / 1024, total / 1024);
    return 0;
  }
};
static struct cmdLs_args cmdLsHelp;

struct cmdCd_args {
  arg_str_t *dst;
  arg_end_t *end;
  static void setArgs(struct cmdCd_args &args) {
    args.dst = arg_str0(NULL, NULL, "[dst]", "The directory to change to.");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdCd_args &args) {
    if (!(args.dst->count) || strcmp(args.dst->sval[0], "/") == 0) {
      strncpy(currentPath, "/", PATH_LEN - 1);
      setPrompt();
      return 0;
    }
    if (strcmp(args.dst->sval[0], "..") == 0) {
      if (strcmp(currentPath, "/") == 0) return 0;
      String p = String(currentPath);
      if (p.endsWith("/")) p = p.substring(0, p.length() - 1);
      int last = p.lastIndexOf('/');
      strncpy(currentPath, (last <= 0 ? "/" : p.substring(0, last + 1)).c_str(), PATH_LEN - 1);
      setPrompt();
      return 0;
    }
    // Absolute or relative
    String target = buildPath(args.dst->sval[0]);
    if (!target.endsWith("/")) target += "/";
    if (isDirectory(target.substring(0, target.length() - 1)) || target == "/") {
      strncpy(currentPath, target.c_str(), PATH_LEN - 1);
      setPrompt();
    } else {
      printf(RED "cd: '%s' not found\n" RESET, args.dst->sval[0]);
    }
    return 0;
  }
};
static struct cmdCd_args cmdCdHelp;

struct cmdMkdir_args {
  arg_str_t *dst;
  arg_end_t *end;
  static void setArgs(struct cmdMkdir_args &args) {
    args.dst = arg_str1(NULL, NULL, "<dst>", "Target directory");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdMkdir_args &args) {
    String path = buildPath(args.dst->sval[0]);
    if (isDirectory(path)) { printf(YELLOW "Already exists" RESET"\n"); return -1; }
    ensureDir(path);
    printf(GREEN "Directory '%s' created\n" RESET, args.dst->sval[0]);
    klog(("mkdir " + path).c_str());
    return 0;
  }
};
static struct cmdMkdir_args cmdMkdirHelp;

struct cmdTouch2_args {
  arg_str_t *dst;
  arg_end_t *end;
  static void setArgs(struct cmdTouch2_args &args) {
    args.dst = arg_str1(NULL, NULL, "<dst>", "Target file");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdTouch2_args &args) {
    String path = buildPath(args.dst->sval[0]);
    if (!SPIFFS.exists(path)) {
      File f = SPIFFS.open(path, FILE_WRITE);
      if (!f) { printf(RED "Failed to create file" RESET"\n"); return -1; }
      f.close();
    }
    printf("'%s' OK\n", args.dst->sval[0]);
    return 0;
  }
};
static struct cmdTouch2_args cmdTouch2Help;

struct cmdCat_args {
  arg_str_t *src;
  arg_end_t *end;
  static void setArgs(struct cmdCat_args &args) {
    args.src = arg_str1(NULL, NULL, "<src>", "Source file");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdCat_args &args) {
    String path = buildPath(args.src->sval[0]);
    File f = SPIFFS.open(path, FILE_READ);
    if (!f) { printf(RED "File not found: %s\n" RESET, args.src->sval[0]); return -1; }
    if (f.size() == 0) { printf(GRAY "(empty)" RESET"\n"); f.close(); return -1; }
    while (f.available()) {
      char c = f.read();
      printf("%c", c);
    }
    printf("\n");
    f.close();
    return 0;
  }
};
static struct cmdCat_args cmdCatHelp;

struct cmdWrite_args {
  arg_lit_t *append;
  arg_str_t *dst;
  arg_str_t *content;
  arg_end_t *end;
  static void setArgs(struct cmdWrite_args &args) {
    args.append = arg_lit0("a", "append", "Append to the file instead of overwriting");
    args.dst = arg_str1(NULL, NULL, "<file>", "The file to write to");
    args.content = arg_strn(NULL, NULL, "<content> [content...]", 1, MAX_ARGS - 2, "The data to write");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdWrite_args &args) {
    bool appendMode = (args.append->count > 0);
    String path = buildPath(args.dst->sval[0]);
    File f = SPIFFS.open(path, appendMode ? FILE_APPEND : FILE_WRITE);
    if (!f) { printf(RED "Cannot open file" RESET "\n"); return -1; }
    for (uint8_t i = 0; i < args.content->count; i++) {
      f.print(args.content->sval[i]);
      if (i < args.content->count - 1) f.print(' ');
    }
    f.println();
    f.close();
    printf("Written to '%s'\n", args.dst->sval[0]);
    return 0;
  }
};
static struct cmdWrite_args cmdWriteHelp;

struct cmdAppend_args {
  arg_str_t *dst;
  arg_str_t *content;
  arg_end_t *end;
  static void setArgs(struct cmdAppend_args &args) {
    args.dst = arg_str1(NULL, NULL, "<file>", "The file to write to");
    args.content = arg_strn(NULL, NULL, "<content> [content...]", 1, MAX_ARGS - 2, "The data to write");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdAppend_args &args) {
    String cmd = "writefile -a ";
    cmd += args.dst->sval[0];
    for(int i = 0; i < args.content->count; i++) {
      cmd += " ";
      cmd += args.content->sval[i];
    }
    Console.run(cmd.c_str());
    return 0;
  }
};
static struct cmdAppend_args cmdAppendHelp;

struct cmdRm_args {
  arg_lit_t *recursive;
  arg_str_t *dst;
  arg_end_t *end;
  static void setArgs(struct cmdRm_args &args) {
    args.recursive = arg_lit0("r", "recursive", "Remove a directory recursively");
    args.dst = arg_str1(NULL, NULL, "<path>", "The file or directory to remove");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdRm_args &args) {
    String path = buildPath(args.dst->sval[0]);
    bool recursive = (args.recursive->count > 0);
    if (isDirectory(path)) {
      if (!recursive) { printf(YELLOW "Use 'rm <dir> -r' to remove directory" RESET"\n"); return -1; }
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
      printf(GREEN "Removed directory '%s'\n" RESET, args.dst->sval[0]);
    } else if (SPIFFS.exists(path)) {
      SPIFFS.remove(path);
      printf(GREEN "Removed '%s'\n" RESET, args.dst->sval[0]);
    } else {
      printf(RED "Not found: %s\n" RESET, args.dst->sval[0]);
      return -1;
    }
    return 0;
  }
};
static struct cmdRm_args cmdRmHelp;

struct cmdMv_args {
  arg_str_t *src;
  arg_str_t *dst;
  arg_end_t *end;
  static void setArgs(struct cmdMv_args &args) {
    args.src = arg_str1(NULL, NULL, "<src>", "Source file");
    args.dst = arg_str1(NULL, NULL, "<dst>", "Destination file");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdMv_args &args) {
    String src = buildPath(args.src->sval[0]);
    String dst = buildPath(args.dst->sval[0]);
    if (!SPIFFS.exists(src)) { printf(RED "Source not found" RESET"\n"); return -1; }
    // Copy then delete
    File fs_ = SPIFFS.open(src, FILE_READ);
    File fd  = SPIFFS.open(dst, FILE_WRITE);
    if (!fs_ || !fd) { printf(RED "Move failed" RESET"\n"); return -1; }
    while (fs_.available()) fd.write(fs_.read());
    fs_.close(); fd.close();
    SPIFFS.remove(src);
    printf("Moved '%s' → '%s'\n", args.src->sval[0], args.dst->sval[0]);
    return 0;
  }
};
static struct cmdMv_args cmdMvHelp;

struct cmdCp_args {
  arg_str_t *src;
  arg_str_t *dst;
  arg_end_t *end;
  static void setArgs(struct cmdCp_args &args) {
    args.src = arg_str1(NULL, NULL, "<src>", "Source file");
    args.dst = arg_str1(NULL, NULL, "<dst>", "Destination file");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdCp_args &args) {
    String src = buildPath(args.src->sval[0]);
    String dst = buildPath(args.dst->sval[0]);
    if (!SPIFFS.exists(src)) { printf(RED "Source not found" RESET"\n"); return -1; }
    File fs_ = SPIFFS.open(src, FILE_READ);
    File fd  = SPIFFS.open(dst, FILE_WRITE);
    if (!fs_ || !fd) { printf(RED "Copy failed" RESET"\n"); return -1; }
    size_t bytes = 0;
    while (fs_.available()) { fd.write(fs_.read()); bytes++; }
    fs_.close(); fd.close();
    printf("Copied %u bytes → '%s'\n", bytes, args.dst->sval[0]);
    return 0;
  }
};
static struct cmdCp_args cmdCpHelp;

struct cmdDf_args {
  arg_end_t *end;
  static void setArgs(struct cmdDf_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdDf_args &args) {
    size_t total = SPIFFS.totalBytes();
    size_t used  = SPIFFS.usedBytes();
    size_t free_ = total - used;
    int pct = (used * 100) / total;
    int bar = (used * 30) / total;
    printf("\n  " YELLOW "Filesystem (SPIFFS):" RESET"\n");
    printf  ("  [");
    for (int i = 0; i < 30; i++) printf(i < bar ? GREEN "█" RESET : GRAY "░" RESET);
    printf ("] %d%%\n", pct);
    printf ("  Total: %u KB  Used: %u KB  Free: %u KB\n\n",
                  total/1024, used/1024, free_/1024);
    return 0;
  }
};
static struct cmdDf_args cmdDfHelp;

// WiFi Commands
struct cmdWifi_args {
  arg_end_t *end;
  static void setArgs(struct cmdWifi_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdWifi_args &args) {
    // Show status
    printf("\n  " CYAN "WiFi Status:" RESET);
    printf("  Mode : %s\n",
      WiFi.getMode() == WIFI_STA ? "Station" :
      WiFi.getMode() == WIFI_AP  ? "Access Point" :
      WiFi.getMode() == WIFI_AP_STA ? "AP+Station" : "Off");
    if (wifiConnected) {
      printf("  SSID : %s\n", WiFi.SSID().c_str());
      printf("  IP   : %s\n", WiFi.localIP().toString().c_str());
      printf("  RSSI : %d dBm\n", WiFi.RSSI());
      printf("  MAC  : %s\n", WiFi.macAddress().c_str());
      printf("  GW   : %s\n", WiFi.gatewayIP().toString().c_str());
      printf("  DNS  : %s\n", WiFi.dnsIP().toString().c_str());
    }
    if (apActive) {
      printf("  AP   : %s  IP: %s  Clients: %d\n",
                    apSSID.c_str(), WiFi.softAPIP().toString().c_str(),
                    WiFi.softAPgetStationNum());
    }
    printf("\n");
    return 0;
  }
};
static struct cmdWifi_args cmdWifiHelp;

struct cmdWifiConnect_args {
  arg_str_t *ssid;
  arg_str_t *pass;
  arg_end_t *end;
  static void setArgs(struct cmdWifiConnect_args &args) {
    args.ssid = arg_str1(NULL, NULL, "<ssid>", "The wireless network to connect to");
    args.pass = arg_str1(NULL, NULL, "<pass>", "The password for the network");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdWifiConnect_args &args) {
    printf("  Connecting to '%s'", args.ssid->sval[0]);
    WiFi.mode(WIFI_STA);
    WiFi.begin(args.ssid->sval[0], args.pass->sval[0]);
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 30) {
      delay(500); printf("."); retries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      staSSID = String(argv[2]);
      printf("\n  " GREEN "Connected! IP: %s" RESET "\n\n", WiFi.localIP().toString().c_str());
      klog(("WiFi connected: " + staSSID).c_str());
    } else {
      printf("\n  " RED "Connection failed" RESET"\n");
      WiFi.disconnect();
    }
    setPrompt();
    return 0;
  }
};
static struct cmdWifiConnect_args cmdWifiConnectHelp;

struct cmdWifiDisconnect_args {
  arg_end_t *end;
  static void setArgs(struct cmdWifiDisconnect_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdWifiDisconnect_args &args) {
    WiFi.disconnect(true);
    wifiConnected = false;
    printf("  WiFi disconnected\n");
    setPrompt();
    return 0;
  }
};
static struct cmdWifiDisconnect_args cmdWifiDisconnectHelp;

struct cmdWifiAp_args {
  arg_str_t *ssid;
  arg_str_t *pass;
  arg_end_t *end;
  static void setArgs(struct cmdWifiAp_args &args) {
    args.ssid = arg_str1(NULL, NULL, "<SSID>", "The name of the network to create");
    args.pass = arg_str0(NULL, NULL, "password", "The password for the network");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdWifiAp_args &args) {
    apSSID = String(args.ssid->sval[0]);
    apPASS = (args.pass->count) ? String(args.pass->sval[0]) : "";
    WiFi.mode(wifiConnected ? WIFI_AP_STA : WIFI_AP);
    bool ok = apPASS.length() > 0
                ? WiFi.softAP(apSSID.c_str(), apPASS.c_str())
                : WiFi.softAP(apSSID.c_str());
    apActive = ok;
    if (ok) printf(GREEN "  AP '%s' started  IP: %s\n" RESET,
                           apSSID.c_str(), WiFi.softAPIP().toString().c_str());
    else     printf(RED "  AP failed" RESET"\n");
    return 0;
  }
};
static struct cmdWifiAp_args cmdWifiApHelp;

struct cmdWifiScan_args {
  arg_end_t *end;
  static void setArgs(struct cmdWifiScan_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdWifiScan_args &args) {
    printf("  Scanning...\n");
    int n = WiFi.scanNetworks();
    if (n == 0) { printf("  No networks found\n"); return 0; }
    printf("\n  " YELLOW "#  SSID                           RSSI  ENC" RESET"\n");
    printf("  " GRAY "───────────────────────────────────────────────" RESET"\n");
    for (int i = 0; i < n; i++) {
      const char* enc = WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? GREEN "Open" RESET : CYAN "WPA" RESET;
      printf("  %-2d %-32s %4d  %s\n", i + 1,
                    WiFi.SSID(i).c_str(), WiFi.RSSI(i), enc);
    }
    WiFi.scanDelete();
    printf("\n");
    return 0;
  }
};
static struct cmdWifiScan_args cmdWifiScanHelp;

struct cmdWifiPing_args {
  arg_rex_t *ip;
  arg_end_t *end;
  static void setArgs(struct cmdWifiPing_args &args) {
    args.ip = arg_rex1(NULL, NULL, "[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+", "<IP>", 0, "Address to ping");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdWifiPing_args &args) {
    if (!wifiConnected) { printf(YELLOW "Not connected to WiFi" RESET"\n"); return -1; }
    IPAddress ip;
    if (!ip.fromString(args.ip->sval[0])) { printf(RED "Invalid IP. DNS not supported in minimal mode." RESET"\n"); return -1; }
    printf("  Pinging %s:\n", args.ip->sval[0]);
    for (int i = 0; i < 4; i++) {
      // ESP32 Arduino core does not include ping by default; send TCP probe instead
      WiFiClient client;
      unsigned long t = millis();
      bool ok = client.connect(ip, 80);
      unsigned long rtt = millis() - t;
      client.stop();
      if (ok) printf("  seq=%d time=%lums " GREEN "reachable" RESET "\n", i, rtt);
      else    printf("  seq=%d " RED "no response" RESET "\n", i);
      delay(500);
    }
    return 0;
  }
};
static struct cmdWifiPing_args cmdWifiPingHelp;

struct cmdWifiHostname_args {
  arg_str_t *name;
  arg_end_t *end;
  static void setArgs(struct cmdWifiHostname_args &args) {
    args.name = arg_str0(NULL, NULL, "hostname", "Set the system hostname");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdWifiHostname_args &args) {
    if (args.name->count) {
      WiFi.setHostname(args.name->sval[0]);
      Hostname = args.name->sval[0];
      apSSID = args.name->sval[0];
      apSSID += "_ap";
      setPrompt();
      printf("  Hostname set to '%s'\n", args.name->sval[0]);
    } else {
      printf("  Hostname: %s\n", WiFi.getHostname());
    }
    return 0;
  }
};
static struct cmdWifiHostname_args cmdWifiHostnameHelp;

struct cmdWifiHttpStart_args {
  arg_int_t *port;
  arg_end_t *end;
  static void setArgs(struct cmdWifiHttpStart_args &args) {
    args.port = arg_int0(NULL, NULL, "port", "The port to listen on");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdWifiHttpStart_args &args) {
    if (!wifiConnected && !apActive) { printf(YELLOW "Connect WiFi first" RESET"\n"); return -1; }
    int port = (args.port->count) ? args.port->ival[0] : 80;
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
    printf(GREEN "  HTTP server started on port %d\n  URL: http://%s/\n" RESET,
                  port, wifiConnected ? WiFi.localIP().toString().c_str()
                                      : WiFi.softAPIP().toString().c_str());
    return 0;
  }
};
static struct cmdWifiHttpStart_args cmdWifiHttpStartHelp;

struct cmdWifiHttpStop_args {
  arg_end_t *end;
  static void setArgs(struct cmdWifiHttpStop_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdWifiHttpStop_args &args) {
    if (httpServer) { httpServer->stop(); delete httpServer; httpServer = nullptr; httpRunning = false; }
    printf("  HTTP server stopped\n");
    return 0;
  }
};
static struct cmdWifiHttpStop_args cmdWifiHttpStopHelp;

struct cmdWifiMac_args {
  arg_end_t *end;
  static void setArgs(struct cmdWifiMac_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdWifiMac_args &args) {
    printf("  STA MAC: %s\n", WiFi.macAddress().c_str());
    printf("  AP  MAC: %s\n", WiFi.softAPmacAddress().c_str());
    return 0;
  }
};
static struct cmdWifiMac_args cmdWifiMacHelp;


// Scripting

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
      Console.run(cmd);
      delay(20);
    }
    cmd = strtok(nullptr, ";");
  }
  free(buf);
}

struct cmdEval_args {
  struct arg_str *cmd;
  struct arg_end *end;
  static void setArgs(struct cmdEval_args &args) {
      args.cmd = arg_strn(NULL, NULL, "<cmd>", 1, MAX_ARGS, "The command to execute");
      args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdEval_args &args) {
    String code = "";

    for (int i = 1; i < argc; i++) { if (i > 1) code += " "; code += argv[i]; }
    printf(CYAN ">>> eval" RESET "\n");
    runScript(code.c_str());
    printf(CYAN ">>> done" RESET "\n");

    return 0;
  }
};
static struct cmdEval_args cmdEvalHelp;

struct cmdRun_args {
  arg_str_t *file;
  arg_end_t *end;
  static void setArgs(struct cmdRun_args &args) {
    args.file = arg_str1(NULL, NULL, "<file>", "The script file to run");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdRun_args &args) {
    String path = buildPath(args.file->sval[0]);
    File f = SPIFFS.open(path, FILE_READ);
    if (!f) { printf(RED "Script not found: %s\n" RESET, args.file->sval[0]); return -1; }
    String content = f.readString();
    f.close();
    printf(CYAN ">>> run %s" RESET "\n", argv[1]);
    runScript(content.c_str());
    printf(CYAN ">>> done (%u bytes)" RESET "\n", content.length());
    klog(("run " + path).c_str());
    return 0;
  }
};
static struct cmdRun_args cmdRunHelp;

struct cmdFor_args {
  arg_int_t *count;
  arg_str_t *cmd;
  arg_end_t *end;
  static void setArgs(struct cmdFor_args &args) {
    args.count = arg_int1(NULL, NULL, "<count>", "The number of times to execute the command");
    args.cmd = arg_strn(NULL, NULL, "<cmd>", 1, MAX_ARGS, "The command to execute");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdFor_args &args) {
    int count;
    String cmd = "";

    count = args.count->ival[0];

    for (int i = 2; i < argc; i++) { if (i > 2) cmd += " "; cmd += argv[i]; }
    for (int i = 0; i < count; i++) {
      printf(GRAY "\r  [%d/%d]" RESET, i + 1, count);
      Console.run(cmd);
      delay(5);
    }
    printf("\n");

    return 0;
  }
};
static struct cmdFor_args cmdForHelp;

struct cmdDelay_args {
  arg_int_t *adelay;
  arg_end_t *end;
  static void setArgs(struct cmdDelay_args &args) {
    args.adelay = arg_int1(NULL, NULL, "<delay>", "Delay time in ms");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdDelay_args &args) {
    int ms = constrain(args.adelay->ival[0], 0, 60000);
    delay(ms);
    return 0;
  }
};
static struct cmdDelay_args cmdDelayHelp;

// System Commands
struct cmdFree_args {
  arg_end_t *end;
  static void setArgs(struct cmdFree_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdFree_args &args) {
    uint32_t heap    = ESP.getFreeHeap();
    uint32_t minHeap = ESP.getMinFreeHeap();
    uint32_t maxAlloc= ESP.getMaxAllocHeap();
    printf("\n  " YELLOW "Memory:" RESET "\n");
    printf("  Free heap      : %u bytes (%u KB)\n", heap, heap / 1024);
    printf("  Min free heap  : %u bytes\n", minHeap);
    printf("  Max alloc block: %u bytes\n", maxAlloc);
    printf("  PSRAM          : %u bytes\n", ESP.getFreePsram());
    Console.run("df");
    return 0;
  }
};
static struct cmdFree_args cmdFreeHelp;

struct cmdSysInfo_args {
  arg_end_t *end;
  static void setArgs(struct cmdSysInfo_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdSysInfo_args &args) {
    unsigned long up  = (millis() - bootTime) / 1000;
    uint8_t h = up / 3600, m = (up % 3600) / 60, s = up % 60;

    printf("\n"
      CYAN  "  ██████╗ ███████╗██████╗ ██████╗ ██████╗ \n"
      BCYAN "  ██╔════╝██╔════╝██╔══██╗╚════██╗╚════██╗\n"
      CYAN  "  █████╗  ███████╗██████╔╝ █████╔╝ █████╔╝\n"
      CYAN  "  ██╔══╝  ╚════██║██╔═══╝  ╚═══██╗ ██╔═══╝\n"
      BCYAN "  ███████╗███████║██║     ███████╗███████║ \n"
      GRAY  "  ╚══════╝╚══════╝╚═╝     ╚══════╝╚══════╝" RESET "\n\n"
    );

    printf("  " YELLOW "OS     " RESET ": KernelESP v1.0\n");
    printf("  " YELLOW "Host   " RESET ": %s\n", Hostname);
    printf("  " YELLOW "Board  " RESET ": "ARDUINO_BOARD" @ %d MHz\n", ESP.getCpuFreqMHz());
    printf("  " YELLOW "Chip   " RESET ": %s  Rev%d  Cores:%d\n", ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
    printf("  " YELLOW "Flash  " RESET ": %u KB  (mode:%d  speed:%d MHz)\n",
                  ESP.getFlashChipSize()/1024, ESP.getFlashChipMode(), ESP.getFlashFrequencyMHz());
    printf("  " YELLOW "RAM    " RESET ": %u KB free / PSRAM: %u KB\n",
                  ESP.getFreeHeap()/1024, ESP.getFreePsram()/1024);
    printf("  " YELLOW "SPIFFS " RESET ": %u / %u KB\n",
                  SPIFFS.usedBytes()/1024, SPIFFS.totalBytes()/1024);
    printf("  " YELLOW "Uptime " RESET ": %dh %dm %ds\n", h, m, s);
    printf("  " YELLOW "WiFi   " RESET ": %s\n",
                  wifiConnected ? (GREEN + WiFi.SSID() + "  " + WiFi.localIP().toString() + RESET).c_str()
                                : RED "Offline" RESET);
    printf("\n");
    return 0;
  }
};
static struct cmdSysInfo_args cmdSysInfoHelp;

struct cmdDmesg_args {
  arg_end_t *end;
  static void setArgs(struct cmdDmesg_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdDmesg_args &args) {
    printf("\n  " YELLOW "Kernel Log:" RESET "\n\n");
    for (uint8_t i = 0; i < dmesgCount; i++) {
      uint8_t idx = (dmesgHead - dmesgCount + i + DMESG_LINES) % DMESG_LINES;
      printf("  " GRAY "[%4lus]" RESET " %s\n", dmesgBuf[idx].ts, dmesgBuf[idx].msg);
    }
    printf("\n");
    return 0;
  }
};
static struct cmdDmesg_args cmdDmesgHelp;

struct cmdReboot_args {
  arg_end_t *end;
  static void setArgs(struct cmdReboot_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdReboot_args &args) {

    printf("\n  Rebooting...\n");
    delay(300);
    ESP.restart();

    return 0;
  }
};
static struct cmdReboot_args cmdRebootHelp;

struct cmdWhoami_args {
  arg_end_t *end;
  static void setArgs(struct cmdWhoami_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdWhoami_args &args) {
    printf("root\n");
    return 0;
  }
};
static struct cmdWhoami_args cmdWhoamiHelp;

struct cmdUname_args {
  arg_end_t *end;
  static void setArgs(struct cmdUname_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdUname_args &args) {
    printf("KernelESP v1.0 ESP32 xtensa\n");
    return 0;
  }
};
static struct cmdUname_args cmdUnameHelp;

struct cmdUptime_args {
  arg_end_t *end;
  static void setArgs(struct cmdUptime_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdUptime_args &args) {
    unsigned long t = (millis() - bootTime) / 1000;
    printf("up %dh %dm %ds\n", (int)(t/3600), (int)((t%3600)/60), (int)(t%60));
    return 0;
  }
};
static struct cmdUptime_args cmdUptimeHelp;

struct cmdPwd_args {
  arg_end_t *end;
  static void setArgs(struct cmdPwd_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdPwd_args &args) {
    printf("%s\n", currentPath);
    return 0;
  }
};
static struct cmdPwd_args cmdPwdHelp;

struct cmdEcho_args {
  arg_str_t *content;
  arg_end_t *end;
  static void setArgs(struct cmdEcho_args &args) {
    args.content = arg_strn(NULL, NULL, "content", 1, MAX_ARGS, "The values to print");
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdEcho_args &args) {
    for (int i = 0; i < (args.content->count); i++) { if (i > 0) printf(" "); printf(args.content->sval[i]); }
    printf("\n");
    return 0;
  }
};
static struct cmdEcho_args cmdEchoHelp;

struct cmdClear_args {
  arg_end_t *end;
  static void setArgs(struct cmdClear_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdClear_args &args) {
    showLogo();
    return 0;
  }
};
static struct cmdClear_args cmdClearHelp;

struct cmdWave_args {
  arg_end_t *end;
  static void setArgs(struct cmdWave_args &args) {
    args.end = arg_end(2);
  }
  static int implementation(int argc, char** argv, struct cmdWave_args &args) {
    printf("\n"
      CYAN "  ╭╮              ╭╮\n"
      CYAN "  ╭╯╰╮          ╭╯╰╮\n"
      CYAN " ╭╯  ╰╮        ╭╯  ╰╮\n"
      CYAN "╭╯    ╰╮──────╭╯    ╰╮\n"
      CYAN "╯      ╰╮    ╭╯      ╰\n" RESET
    );
    return 0;
  }
};
static struct cmdWave_args cmdWaveHelp;

// Setup
void setup() {
  Serial.begin(115200);
  bootTime = millis();

  // Brief boot flash on GPIO2 (built-in LED most boards)
  #ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
  for (int i = 0; i < 6; i++) { digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN)); delay(60); }
  digitalWrite(LED_BUILTIN, LOW);
  #endif

  initPins();

  initFilesystem();

  showLogo();

  klog("KernelESP v1.0 booted");
  klog("SPIFFS OK");
  klog("Serial @ 115200");

  // We can't do filesystem operations if we use PSRAM
  Console.usePsram(false);
  setPrompt();
  Console.begin();
  Console.setMaxHistory(16);
  // Hardware
  Command<struct cmdPinMode_args>::addCmd({"pinmode"}, "Set pin mode", cmdPinModeHelp);
  Command<struct cmdDigitalWrite_args>::addCmd({"write", "digitalwrite"}, "Digital write", cmdDigitalWriteHelp);
  Command<struct cmdDigitalRead_args>::addCmd({"read", "digitalread"}, "Digital read (all if no pin)", cmdDigitalReadHelp);
  Command<struct cmdAnalogRead_args>::addCmd({"aread", "analogread"}, "ADC read (0-4095, 3.3V)", cmdAnalogReadHelp);
  Command<struct cmdPWM_args>::addCmd({"pwm"}, "LEDC PWM output", cmdPWMHelp);
  #ifdef SOC_DAC_SUPPORTED
  Command<struct cmdDac_args>::addCmd({"dac"}, "DAC voltage output", cmdDacHelp);
  #endif
  Command<struct cmdGPIO_args>::addCmd({"gpio"}, "Quick GPIO", cmdGPIOHelp);
  Command<struct cmdTone_args>::addCmd({"tone"}, "Square wave tone", cmdToneHelp);
  Command<struct cmdNoTone_args>::addCmd({"notone"}, "Stop tone", cmdNoToneHelp);
  Command<struct cmdTouch_args>::addCmd({"tsense"}, "Capacitive touch read", cmdTouchHelp);
  Command<struct cmdDisco_args>::addCmd({"disco"}, "LED show", cmdDiscoHelp);
  Command<struct cmdMorse_args>::addCmd({"morse"}, "Morse code", cmdMorseHelp);
  Command<struct cmdSensor_args>::addCmd({"sensor"}, "All ADC channels with bar", cmdSensorHelp);
  Command<struct cmdScope_args>::addCmd({"scope"}, "Oscilloscope plot", cmdScopeHelp);
  Command<struct cmdMonitor_args>::addCmd({"monitor"}, "Live pin monitor", cmdMonitorHelp);
  Command<struct cmdPins_args>::addCmd({"pins"}, "Show current pin configuration", cmdPinsHelp);

  // Filesystem
  Command<struct cmdLs_args>::addCmd({"ls", "dir"}, "List files in a path", cmdLsHelp);
  Command<struct cmdCd_args>::addCmd({"cd"}, "Change working directory", cmdCdHelp);
  Command<struct cmdPwd_args>::addCmd({"pwd"}, "Print working directory", cmdPwdHelp);
  Command<struct cmdMkdir_args>::addCmd({"mkdir"}, "Create a directory", cmdMkdirHelp);
  Command<struct cmdTouch2_args>::addCmd({"touch"}, "Create empty file", cmdTouch2Help);
  Command<struct cmdCat_args>::addCmd({"cat", "type"}, "Print the contents of a file", cmdCatHelp);
  Command<struct cmdWrite_args>::addCmd({"writefile", "write>"}, "Write or append values to a file", cmdWriteHelp);
  Command<struct cmdAppend_args>::addCmd({"append"}, "Append values to a file", cmdAppendHelp);
  Command<struct cmdRm_args>::addCmd({"rm", "del"}, "Delete a file or folder", cmdRmHelp);
  Command<struct cmdMv_args>::addCmd({"mv"}, "Move or rename a file", cmdMvHelp);
  Command<struct cmdCp_args>::addCmd({"cp"}, "Copy a file", cmdCpHelp);
  Command<struct cmdDf_args>::addCmd({"df"}, "Show how full the disk is", cmdDfHelp);
  Command<struct cmdEcho_args>::addCmd({"echo"}, "Write values to standard output", cmdEchoHelp);

  // WiFi
  Command<struct cmdWifi_args>::addCmd({"wifi"}, "Show wifi status", cmdWifiHelp);
  Command<struct cmdWifiConnect_args>::addCmd({"wifi-connect", "wifi-up"}, "Connect to AP", cmdWifiConnectHelp);
  Command<struct cmdWifiDisconnect_args>::addCmd({"wifi-disconnect", "wifi-down"}, "Disconnect from AP", cmdWifiDisconnectHelp);
  Command<struct cmdWifiAp_args>::addCmd({"wifi-ap"}, "Create an AP", cmdWifiApHelp);
  Command<struct cmdWifiScan_args>::addCmd({"wifi-scan"}, "Scan for networks", cmdWifiScanHelp);
  Command<struct cmdWifiPing_args>::addCmd({"wifi-ping"}, "TCP connectivity check", cmdWifiPingHelp);
  Command<struct cmdWifiHostname_args>::addCmd({"wifi-hostname"}, "Get/set hostname", cmdWifiHostnameHelp);
  Command<struct cmdWifiHttpStart_args>::addCmd({"wifi-http-start"}, "Start HTTP server", cmdWifiHttpStartHelp);
  Command<struct cmdWifiHttpStop_args>::addCmd({"wifi-http-stop"}, "Stop HTTP server", cmdWifiHttpStopHelp);
  Command<struct cmdWifiMac_args>::addCmd({"wifi-mac"}, "Show MAC addresses", cmdWifiMacHelp);

  // Scripting
  Command<struct cmdEval_args>::addCmd({"eval", "exec"}, "Run commands as though read from a script", cmdEvalHelp);
  Command<struct cmdRun_args>::addCmd({"run", "sh"}, "Execute file script", cmdRunHelp);
  Command<struct cmdFor_args>::addCmd({"for", "loop"}, "Run a command multiple times", cmdForHelp);
  Command<struct cmdDelay_args>::addCmd({"delay", "sleep"}, "Wait", cmdDelayHelp);

  // System
  Command<struct cmdSysInfo_args>::addCmd({"sysinfo", "neofetch"}, "Show system information", cmdSysInfoHelp);
  Command<struct cmdDmesg_args>::addCmd({"dmesg", "log"}, "Show system event logs", cmdDmesgHelp);
  Command<struct cmdFree_args>::addCmd({"free", "mem"}, "Show memory usage", cmdFreeHelp);
  Command<struct cmdUptime_args>::addCmd({"uptime"}, "Show how long system has been running", cmdUptimeHelp);
  Command<struct cmdWhoami_args>::addCmd({"whoami"}, "Show current user name", cmdWhoamiHelp);
  Command<struct cmdUname_args>::addCmd({"uname"}, "Show current kernel name", cmdUnameHelp);
  Command<struct cmdClear_args>::addCmd({"clear", "cls"}, "Clear the screen", cmdClearHelp);
  Command<struct cmdReboot_args>::addCmd({"reboot", "reset"}, "Reboot the processor", cmdRebootHelp);
  Command<struct cmdWave_args>::addCmd({"wave"}, "Display a wave in ASCII art", cmdWaveHelp);
  Console.addHelpCmd();

  Console.attachToSerial(true);

}

// Loop
void loop() {
  // Handle HTTP server if active
  if (httpRunning && httpServer) httpServer->handleClient();

  // Small yield so WiFi stack can breathe
  delay(1);
}
