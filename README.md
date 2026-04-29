# KernelESP

![KernelESP](https://img.shields.io/badge/KernelESP-v1.0-00979D?style=for-the-badge&logo=espressif&logoColor=white)
![Python](https://img.shields.io/badge/Python-3.10+-3776AB?style=for-the-badge&logo=python&logoColor=white)
![ESP32](https://img.shields.io/badge/ESP32-WROOM--32-E7352C?style=for-the-badge&logo=espressif&logoColor=white)
![Platform](https://img.shields.io/badge/Platform-Windows_|_macOS_|_Linux-808080?style=for-the-badge)
![License](https://img.shields.io/badge/License-CC0_1.0-lightgrey?style=for-the-badge)

> **A full Linux-like interactive shell for the ESP32 Family — with WiFi, real persistent filesystem, DAC, touch, PWM, scripting, and a clean Python terminal.**

KernelESP turns your ESP32 into a live interactive environment. A Python terminal on your PC talks to the ESP32 over Serial, giving you a shell to control GPIO, read sensors, manage a real SPIFFS filesystem, run scripts, scan WiFi, start a web server — all without recompiling.

---

**Demo**
![Image](https://github.com/Chirrenthen/KernelESP/blob/main/KernelESP.png)

## Installation

### Requirements

- ESP32 development board (any variant — WROOM-32, S3, C3, etc.)
- Python 3.10+
- USB cable (data-capable)
- Arduino IDE 2.x with **ESP32 board package** installed

### Step 1 — Clone

```bash
git clone https://github.com/chirrenthen/KernelESP.git
cd KernelESP
```

### Step 2 — Python environment

**macOS / Linux:**
```bash
python3 -m venv .venv
source .venv/bin/activate
pip install pyserial
```

**Windows:**
```bash
python -m venv .venv
.venv\Scripts\activate.bat
pip install pyserial pyreadline3
```

### Step 3 — Flash the firmware

1. Open `esp32.ino` in Arduino IDE
2. **Board:** `ESP32 Dev Module` (or your variant)
3. **Partition Scheme:** `Default 4MB with spiffs` ← important
4. **Upload Speed:** `115200` or `921600`
5. Select the correct **Port** and click **Upload**

**Alternatively**
1. Visit [FlashESP](https://flashesp.com/explore/GMUXPHHo712qFnD55xsL6)
2. **Login** with an account
2. Click on '**Flash to Device**' upload the firmware directly from the web, no hassle

### Step 4 — Launch the terminal

**macOS / Linux:**
```bash
source .venv/bin/activate
python terminal.py
```

**Windows:**
```bat
.venv\Scripts\activate.bat
python terminal.py
```

The terminal auto-detects your ESP32's port and connects. If multiple ports are found you'll be prompted to choose.

---

## Terminal Commands (prefix `!`)

```
!help                    Show terminal help
!clear                   Clear screen and redraw banner
!exit  !q                Quit

!ports                   List available serial ports
!connect [port]          Connect (auto-detect if no port)
!disconnect              Disconnect
!reconnect               Reconnect

!ls                      List local workspace files
!edit   <file>           Create / edit a script in workspace
!upload <file>           Upload file to ESP32 SPIFFS
!download <file>         Download file from ESP32
!sync                    Upload all workspace files to ESP32

!macro                   List all macros
!macro <name>            Run macro
!macro <name> = <cmd>    Save macro
!macro <name> del        Delete macro

!anim                    Matrix rain animation
```

---

## ESP32 Commands

All commands are sent live over Serial — no recompiling.

### Hardware Control

```bash
# GPIO
pinmode 2 output              # Set pin mode: input / output / pullup / pulldown
write 2 HIGH                  # Digital write: HIGH LOW 1 0 on off
read                          # Read all digital pins
read 5                        # Read specific pin
gpio 2 on                     # Quick GPIO control
gpio 2 toggle                 # Toggle pin state

# ADC (12-bit, 3.3V reference)
aread                         # Read all ADC channels with bar graph
aread A2                      # Read specific ADC pin (A0=GPIO36 … A5=GPIO33)

# PWM (LEDC — any output pin)
pwm 16 128                    # 50% duty on GPIO16, 5kHz default
pwm 16 200 1000               # 78% duty, 1kHz
pwm 16 200 1000 1             # Channel 1 explicitly

# DAC (GPIO25, GPIO26 only)
dac 25 128                    # ~1.65V on GPIO25
dac 26 255                    # 3.3V on GPIO26

# Touch sensing
touch                         # Read all 10 touch pins
touch 4                       # Read GPIO4 (T0)

# Tone
tone 16 440 1000              # 440Hz square wave for 1000ms
notone 16                     # Stop
```

### Sensors & Monitoring

```bash
sensor                        # All ADC channels with bar graph
scope A0 100                  # Oscilloscope — 100 samples from A0 (GPIO36)
scope 32 80 10                # GPIO32, 80 samples, 10ms between
monitor A0 500 10             # Live monitor every 500ms for 10s
```

### WiFi

```bash
wifi                          # Show current WiFi status
wifi scan                     # Scan for nearby networks
wifi connect MySSID mypass    # Connect to WiFi
wifi disconnect               # Disconnect
wifi ap MyAP mypassword       # Create Soft Access Point
wifi ping 192.168.1.1         # TCP connectivity check
wifi http start               # Start HTTP server on port 80
wifi http start 8080          # Start on custom port
wifi http stop                # Stop HTTP server
wifi hostname myesp           # Set mDNS hostname
wifi mac                      # Print MAC addresses
```

### Filesystem (SPIFFS — persistent across reboots)

```bash
ls                            # List current directory
ls /home                      # List specific path
pwd                           # Print working directory
cd home                       # Change directory
cd ..                         # Go up
mkdir scripts                 # Create directory
touch notes.txt               # Create empty file
writefile notes.txt hello     # Write (overwrite) content
append notes.txt world        # Append line to file
cat notes.txt                 # Read file
rm notes.txt                  # Delete file
rm scripts -r                 # Delete directory recursively
mv a.txt b.txt                # Move / rename
cp a.txt backup.txt           # Copy
df                            # Show SPIFFS usage
```

> SPIFFS on a 4MB flash gives you ~1.4 MB of usable storage. Files survive reboots.

### Scripting & Automation

```bash
# Inline multi-command scripts (semicolon-separated)
eval "gpio 2 on; delay 500; gpio 2 off"

# Loop a command N times (up to 1000)
for 10 "gpio 2 toggle; delay 200"

# Write a script to SPIFFS and run it
writefile /home/blink.sh "gpio 2 on;delay 300;gpio 2 off;delay 300"
run /home/blink.sh

# Delay
delay 1000                    # Wait 1000ms
```

### Fun & Extras

```bash
disco 5 30                    # LED light show on all output pins
morse 2 SOS                   # Morse code on GPIO2
wave                          # ASCII wave art
```

### System Commands

```bash
help                          # Full command reference
sysinfo / neofetch            # System info with ASCII art
uptime                        # Time since boot
dmesg                         # Kernel message log (last 12)
free                          # RAM and SPIFFS usage
df                            # Filesystem usage
reboot                        # Restart ESP32
whoami                        # Prints: root
uname                         # OS and platform info
clear                         # Clear screen and show logo
echo <text>                   # Print text
```

---

## Macros

```bash
# Save a macro
!macro blink = eval "for 10 \"gpio 2 toggle; delay 200\""

# Run it
!macro blink

# List all
!macro

# Delete
!macro blink del
```

Macros are saved to `~/.kernelesp_macros.json` and persist across sessions.

---

## Workspace & File Sync

KernelESP creates a local workspace at `~/kernelesp_workspace/`. Write scripts there and sync to ESP32:

```bash
# In the terminal
!edit blink.sh          # Write your script
!upload blink.sh        # Upload to ESP32 SPIFFS

# On the ESP32
run /blink.sh           # Execute it
```

---

## Troubleshooting

**No port found**
Ensure the USB cable supports data. On Linux: `sudo usermod -aG dialout $USER` then re-login.

**Garbled / no output**
Check baud rate is `115200` on both sides. Try `!reconnect`.

**SPIFFS mount failed**
Set Partition Scheme to `Default 4MB with spiffs` in Arduino IDE → Tools → Partition Scheme.

**WiFi won't connect**
ESP32 only supports 2.4GHz networks. Check SSID/password. Use `wifi scan` to verify the network is visible.

**`readline` error on Windows**
```bat
pip install pyreadline3
```

**Touch pins reading wrong**
Do not use `INPUT_PULLUP` on touch pins. Leave them floating. Values below ~30 typically indicate a touch.

---

## Contributing

Pull requests welcome. Open an issue for bugs or feature requests.

---

## Acknowledgements
I sincerely thank @PPPDUD and @Arc1011 — some of the command implementations in this project are based on their work in KernelUNO.

<p align="center">Built for the ESP32 community — KernelESP v1.0</p>
