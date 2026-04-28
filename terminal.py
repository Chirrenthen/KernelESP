#!/usr/bin/env python3
"""
KernelESP Terminal v1.0.1
Clean, minimal Python terminal for the KernelESP ESP32 shell.
"""

import os
import sys
import json
import time
import serial
import threading
import signal
import serial.tools.list_ports
from datetime import datetime

# ─── Platform ─────────────────────────────────────────────────────────────────
IS_WINDOWS = sys.platform.startswith("win")

if IS_WINDOWS:
    try:
        import ctypes
        ctypes.windll.kernel32.SetConsoleMode(
            ctypes.windll.kernel32.GetStdHandle(-11), 7
        )
    except Exception:
        pass

try:
    import readline as _rl
    HAS_READLINE = True
except ImportError:
    HAS_READLINE = False

# ─── ANSI Colors ──────────────────────────────────────────────────────────────
class C:
    R  = "\033[0m"
    B  = "\033[1m"
    D  = "\033[2m"
    K  = "\033[30m"
    RE = "\033[31m"
    G  = "\033[32m"
    Y  = "\033[33m"
    BL = "\033[34m"
    M  = "\033[35m"
    CY = "\033[36m"
    W  = "\033[37m"
    GR = "\033[90m"
    BG = "\033[92m"
    BY = "\033[93m"
    BB = "\033[94m"
    BM = "\033[95m"
    BC = "\033[96m"
    BW = "\033[97m"

    @staticmethod
    def rgb(r: int, g: int, b: int) -> str:
        return f"\033[38;2;{r};{g};{b}m"

# ─── Constants ────────────────────────────────────────────────────────────────
BAUD         = 115200
HISTORY_FILE = os.path.expanduser("~/.kernelesp_history")
MACROS_FILE  = os.path.expanduser("~/.kernelesp_macros.json")
WORKSPACE    = os.path.expanduser("~/kernelesp_workspace")

ESP32_COMMANDS = [
    # Hardware
    "pinmode","write","read","aread","pwm","dac","gpio","tone","notone",
    "touch","disco","morse","sensor","scope","monitor",
    # Filesystem
    "ls","cd","pwd","mkdir","touch","cat","writefile","append","rm","mv","cp","df","echo",
    # WiFi
    "wifi","wifi scan","wifi connect","wifi disconnect","wifi ap",
    "wifi ping","wifi http","wifi mac","wifi hostname",
    # Scripting
    "eval","exec","run","for","delay","sleep","sh",
    # System
    "help","sysinfo","neofetch","dmesg","free","mem","uptime",
    "whoami","uname","clear","cls","reboot","reset","wave",
    # Terminal-local
    "!help","!exit","!quit","!q","!clear","!ports","!connect",
    "!disconnect","!reconnect","!edit","!upload","!download","!sync",
    "!macro","!ls","!anim",
]

DEFAULT_MACROS = {
    "blink":   'eval "for 5 \\"gpio 2 toggle; delay 300\\""',
    "sensors": "sensor",
    "sysinfo": "sysinfo",
    "wifi":    "wifi",
}

# ─── Terminal ──────────────────────────────────────────────────────────────────
class KernelESP:
    def __init__(self) -> None:
        self.ser: serial.Serial | None = None
        self.port:    str  = ""
        self.running: bool = False
        self.macros:  dict = {}
        self._read_lock = threading.Lock()
        self._print_lock = threading.Lock()

        os.makedirs(WORKSPACE, exist_ok=True)
        self._setup_readline()
        self._load_macros()

    # ── Readline ───────────────────────────────────────────────────────────────
    def _setup_readline(self) -> None:
        if not HAS_READLINE:
            return
        try:
            _rl.read_history_file(HISTORY_FILE)
        except FileNotFoundError:
            pass
        _rl.set_history_length(2000)

        def completer(text: str, state: int):
            opts = [c for c in ESP32_COMMANDS if c.startswith(text)]
            return opts[state] if state < len(opts) else None

        _rl.set_completer(completer)
        _rl.parse_and_bind("tab: complete")

    def _save_history(self) -> None:
        if HAS_READLINE:
            try:
                _rl.write_history_file(HISTORY_FILE)
            except Exception:
                pass

    # ── Macros ─────────────────────────────────────────────────────────────────
    def _load_macros(self) -> None:
        try:
            with open(MACROS_FILE) as f:
                self.macros = json.load(f)
        except Exception:
            self.macros = dict(DEFAULT_MACROS)

    def _save_macros(self) -> None:
        with open(MACROS_FILE, "w") as f:
            json.dump(self.macros, f, indent=2)

    # ── Output ─────────────────────────────────────────────────────────────────
    def _print(self, text: str) -> None:
        with self._print_lock:
            sys.stdout.write(text)
            sys.stdout.flush()

    def _println(self, text: str = "") -> None:
        self._print(text + "\n")

    @staticmethod
    def _clear() -> None:
        os.system("cls" if IS_WINDOWS else "clear")

    # ── Banner ─────────────────────────────────────────────────────────────────
    def show_banner(self) -> None:
        self._clear()
        t = C.CY
        r = C.R
        g = C.GR
        print(f"""
{t}  ██╗  ██╗███████╗██████╗ ███╗   ██╗███████╗██╗     ███████╗███████╗██████╗ {r}
{t}  ██║ ██╔╝██╔════╝██╔══██╗████╗  ██║██╔════╝██║     ██╔════╝██╔════╝██╔══██╗{r}
{C.BC}  █████╔╝█████╗  ██████╔╝██╔██╗ ██║██████  █║      █████╗  ███████╗██████╔╝{r}
{t}  ██╔═██╗ ██╔══╝  ██╔══██╗██║╚██╗██║██╔══╝  ██║     ██╔══╝  ╚════██║██╔═══╝ {r}
{t}  ██║  ██╗███████╗██║  ██║██║ ╚████║███████╗███████╗███████╗███████║██║      {r}
{g}  ╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝╚═╝  ╚═══╝╚══════╝╚══════╝╚══════╝╚══════╝╚═╝  v1.0.1{r}

  {g}Python {sys.version.split()[0]}  │  Platform: {sys.platform}  │  Workspace: {WORKSPACE}{r}
  {g}{'─' * 70}{r}
  {g}Arduino commands → sent directly    Terminal commands → prefix with {C.Y}!{g}{r}
  {g}Type {C.Y}!help{g} for terminal help   Type {C.Y}help{g} for ESP32 commands{r}
""")

    # ── Port Detection ─────────────────────────────────────────────────────────
    def list_ports(self) -> list:
        ports = list(serial.tools.list_ports.comports())
        if not ports:
            self._println(f"\n{C.RE}  No serial ports found.{C.R}")
            return []
        self._println(f"\n{C.CY}{C.B}  Available Ports:{C.R}\n")
        for i, p in enumerate(ports):
            dot = f"{C.G}●{C.R}" if any(k in p.description for k in ("USB","Arduino","CP210","CH340","FTDI")) else f"{C.GR}○{C.R}"
            self._println(f"  {dot} [{C.W}{i}{C.R}] {C.G}{p.device}{C.R}  {C.GR}{p.description}{C.R}")
        self._println()
        return ports

    def connect(self, port: str | None = None) -> bool:
        if port is None:
            ports = self.list_ports()
            if not ports:
                return False
            # Auto-pick ESP32/USB port
            candidates = [p for p in ports if any(k in p.description for k in
                          ("USB","Arduino","CP210","CH340","Silicon","FTDI"))]
            if candidates:
                port = candidates[0].device
                self._println(f"  {C.GR}Auto-selected:{C.R} {C.G}{port}{C.R}")
            else:
                try:
                    raw = input(f"  {C.Y}Select port [0]: {C.R}").strip()
                    idx = int(raw) if raw else 0
                    port = ports[idx].device
                except Exception:
                    self._println(f"{C.RE}Invalid selection{C.R}")
                    return False

        try:
            self._print(f"  Connecting to {C.CY}{port}{C.R} @ {BAUD}baud ")
            self.ser = serial.Serial(port, BAUD, timeout=1)
            for _ in range(8):
                self._print(".")
                sys.stdout.flush()
                time.sleep(0.25)
            self.ser.reset_input_buffer()
            self.port = port
            self._println(f"\n\n  {C.BG}✓ Connected to {port}{C.R}\n")
            return True
        except serial.SerialException as e:
            self._println(f"\n  {C.RE}✗ Failed: {e}{C.R}\n")
            self.ser = None
            return False

    def disconnect(self) -> None:
        if self.ser and self.ser.is_open:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self._println(f"  {C.Y}Disconnected{C.R}")

    def reconnect(self) -> None:
        self.disconnect()
        time.sleep(1.0)
        self.connect(self.port if self.port else None)

    # ── Serial I/O ─────────────────────────────────────────────────────────────
    def send(self, cmd: str) -> None:
        if not self.ser or not self.ser.is_open:
            self._println(f"{C.RE}Not connected. Use !connect or !reconnect.{C.R}")
            return
        try:
            self.ser.write(f"{cmd}\r\n".encode("utf-8", errors="replace"))
        except serial.SerialException as e:
            self._println(f"\n{C.RE}Write error: {e}{C.R}")
            self.disconnect()

    def _reader_thread(self) -> None:
        buf = ""
        while self.running:
            try:
                if self.ser and self.ser.is_open and self.ser.in_waiting:
                    chunk = self.ser.read(self.ser.in_waiting).decode("utf-8", errors="replace")
                    buf += chunk
                    while "\n" in buf:
                        line, buf = buf.split("\n", 1)
                        line = line.rstrip("\r")
                        if line:
                            with self._print_lock:
                                sys.stdout.write(f"\r{line}\n")
                                sys.stdout.flush()
                else:
                    time.sleep(0.01)
            except Exception:
                if self.running:
                    time.sleep(0.1)

    # ── File Operations ────────────────────────────────────────────────────────
    def upload_file(self, filename: str) -> bool:
        filepath = os.path.join(WORKSPACE, filename)
        if not os.path.exists(filepath):
            self._println(f"{C.RE}  File not found in workspace: {filename}{C.R}")
            return False
        with open(filepath, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()

        self._println(f"  {C.CY}Uploading {filename} ({len(lines)} lines)...{C.R}")
        # Create file first
        self.send(f"touch {filename}")
        time.sleep(0.3)
        # Write content line-by-line using append
        for i, line in enumerate(lines):
            content = line.rstrip("\n\r").replace('"', '\\"')
            self.send(f'append {filename} "{content}"')
            time.sleep(0.15)
            pct = int((i + 1) * 100 / len(lines))
            bar = int(pct * 20 / 100)
            filled = C.G + "█" * bar + C.GR + "░" * (20 - bar) + C.R
            sys.stdout.write(f"\r  [{filled}] {pct}%")
            sys.stdout.flush()
        self._println(f"\n  {C.BG}✓ Uploaded {filename}{C.R}")
        return True

    def download_file(self, filename: str) -> str | None:
        if not self.ser or not self.ser.is_open:
            self._println(f"{C.RE}Not connected{C.R}")
            return None
        self.ser.reset_input_buffer()
        self.send(f"cat {filename}")
        time.sleep(0.8)
        content = ""
        deadline = time.time() + 3.0
        while time.time() < deadline:
            if self.ser.in_waiting:
                content += self.ser.read(self.ser.in_waiting).decode("utf-8", errors="replace")
                deadline = time.time() + 0.5
            else:
                time.sleep(0.05)
        return content.strip() or None

    def sync_workspace(self) -> None:
        if not self.ser or not self.ser.is_open:
            self._println(f"{C.RE}Not connected{C.R}")
            return
        files = [f for f in os.listdir(WORKSPACE)
                 if not f.startswith(".") and os.path.isfile(os.path.join(WORKSPACE, f))]
        if not files:
            self._println(f"{C.Y}  Workspace is empty.{C.R}")
            return
        self._println(f"\n  {C.CY}Syncing {len(files)} file(s)...{C.R}\n")
        for fn in files:
            ok = self.upload_file(fn)
            self._println(f"  {'✓' if ok else '✗'} {fn}")
        self._println(f"\n  {C.BG}Sync complete{C.R}\n")

    def edit_file(self, filename: str) -> None:
        filepath = os.path.join(WORKSPACE, filename)
        # Load existing if present
        existing: list[str] = []
        if os.path.exists(filepath):
            with open(filepath) as f:
                existing = f.readlines()
            self._println(f"\n  {C.GR}Loaded {len(existing)} lines from {filename}{C.R}")

        self._println(f"\n  {C.CY}{C.B}Edit: {filename}{C.R}")
        self._println(f"  {C.GR}Enter code. Lines separated by Enter. Ctrl+D / Ctrl+Z to save.{C.R}\n")

        lines: list[str] = list(existing)
        line_n = len(lines) + 1
        try:
            while True:
                raw = input(f"  {C.Y}{line_n:03d}{C.R} │ ")
                lines.append(raw + "\n")
                line_n += 1
        except (EOFError, KeyboardInterrupt):
            pass

        with open(filepath, "w", encoding="utf-8") as f:
            f.writelines(lines)
        self._println(f"\n  {C.BG}Saved {len(lines)} lines → {filepath}{C.R}")

        if self.ser and self.ser.is_open:
            try:
                ans = input(f"  {C.Y}Upload to ESP32? [Y/n]: {C.R}").strip().lower()
            except (EOFError, KeyboardInterrupt):
                ans = "n"
            if ans != "n":
                self.upload_file(filename)

    # ── Local Commands ─────────────────────────────────────────────────────────
    def handle_local(self, cmd: str) -> bool:
        parts = cmd.split()
        if not parts:
            return False
        c = parts[0].lower()

        # Exit
        if c in ("!exit", "!quit", "!q", "exit", "quit"):
            self.running = False
            return True

        # Help
        if c in ("!help", "!h", "!?"):
            self._show_help()
            return True

        # Clear
        if c in ("!clear", "!cls"):
            self.show_banner()
            return True

        # Ports
        if c == "!ports":
            self.list_ports()
            return True

        # Connect
        if c == "!connect":
            port = parts[1] if len(parts) > 1 else None
            self.connect(port)
            return True

        # Disconnect
        if c == "!disconnect":
            self.disconnect()
            return True

        # Reconnect
        if c == "!reconnect":
            self.reconnect()
            return True

        # Upload
        if c == "!upload":
            if len(parts) < 2:
                self._println(f"{C.Y}Usage: !upload <filename>{C.R}")
            else:
                self.upload_file(parts[1])
            return True

        # Download
        if c == "!download":
            if len(parts) < 2:
                self._println(f"{C.Y}Usage: !download <filename>{C.R}")
            else:
                content = self.download_file(parts[1])
                if content:
                    fp = os.path.join(WORKSPACE, parts[1])
                    with open(fp, "w") as f:
                        f.write(content)
                    self._println(f"{C.BG}Downloaded → {fp}{C.R}")
                else:
                    self._println(f"{C.RE}No content received{C.R}")
            return True

        # Sync
        if c == "!sync":
            self.sync_workspace()
            return True

        # Edit
        if c == "!edit":
            if len(parts) < 2:
                self._println(f"{C.Y}Usage: !edit <filename>{C.R}")
            else:
                self.edit_file(parts[1])
            return True

        # List workspace
        if c == "!ls":
            files = sorted(os.listdir(WORKSPACE))
            self._println(f"\n  {C.CY}Workspace: {WORKSPACE}{C.R}\n")
            if not files:
                self._println(f"  {C.GR}(empty){C.R}")
            for fn in files:
                fp = os.path.join(WORKSPACE, fn)
                size = os.path.getsize(fp)
                mtime = datetime.fromtimestamp(os.path.getmtime(fp)).strftime("%Y-%m-%d %H:%M")
                self._println(f"  {C.G}{fn:<30}{C.R} {C.GR}{size:>8} B  {mtime}{C.R}")
            self._println()
            return True

        # Macro
        if c == "!macro":
            if len(parts) == 1:
                self._println(f"\n  {C.CY}Macros:{C.R}")
                if not self.macros:
                    self._println(f"  {C.GR}(none){C.R}")
                for name, code in self.macros.items():
                    self._println(f"  {C.G}{name:<20}{C.R} {C.GR}{code}{C.R}")
                self._println()
            elif len(parts) == 2:
                name = parts[1]
                if name in self.macros:
                    self._println(f"  {C.CY}Running macro: {name}{C.R}")
                    self.send(self.macros[name])
                else:
                    self._println(f"  {C.RE}Macro '{name}' not found{C.R}")
            elif len(parts) >= 4 and parts[2] == "=":
                name = parts[1]
                code = " ".join(parts[3:])
                self.macros[name] = code
                self._save_macros()
                self._println(f"  {C.BG}Macro '{name}' saved{C.R}")
            elif len(parts) == 3 and parts[2] == "del":
                name = parts[1]
                if name in self.macros:
                    del self.macros[name]
                    self._save_macros()
                    self._println(f"  {C.Y}Macro '{name}' deleted{C.R}")
                else:
                    self._println(f"  {C.RE}Not found{C.R}")
            else:
                self._println(f"  {C.Y}Usage: !macro | !macro <name> | !macro <name> = <cmd> | !macro <name> del{C.R}")
            return True

        # Matrix rain animation
        if c == "!anim":
            self._matrix_rain()
            self.show_banner()
            return True

        return False

    # ── Help ──────────────────────────────────────────────────────────────────
    def _show_help(self) -> None:
        print(f"""
{C.CY}{C.B}  KernelESP Terminal v1.0.1{C.R}
{C.GR}  ─────────────────────────────────────────────────────────{C.R}

{C.G}  Session:{C.R}
    !help              Show this help
    !clear             Clear screen
    !exit  !q          Quit

{C.G}  Connection:{C.R}
    !ports             List available serial ports
    !connect [port]    Connect (auto-detect if no port given)
    !disconnect        Disconnect
    !reconnect         Reconnect to same port

{C.G}  File Sync:{C.R}
    !ls                List local workspace files
    !edit   <file>     Create / edit a file in workspace
    !upload <file>     Upload workspace file to ESP32 SPIFFS
    !download <file>   Download file from ESP32 to workspace
    !sync              Upload all workspace files

{C.G}  Macros:{C.R}
    !macro                   List all macros
    !macro <name>            Run macro
    !macro <name> = <cmd>    Save macro
    !macro <name> del        Delete macro

{C.G}  Fun:{C.R}
    !anim              Matrix rain animation

{C.GR}  Tip: All other input is forwarded to the ESP32 directly.
  Tip: Press Tab for command completion.
  Tip: Use ↑ / ↓ for command history.
  Workspace: {WORKSPACE}{C.R}
""")

    # ── Matrix Rain ───────────────────────────────────────────────────────────
    def _matrix_rain(self, duration: float = 2.0) -> None:
        import random
        chars = "ﾊﾐﾋｰｳｼﾅﾓﾆｻﾜﾂｵﾘｱﾎﾃﾏｹﾒｴｶｷﾑﾕﾗｾﾈｽﾀﾇﾍ0123789:・.\"=*+-<>|"
        end = time.time() + duration
        while time.time() < end:
            line = "".join(random.choice(chars) for _ in range(80))
            shade = random.randint(80, 255)
            sys.stdout.write(f"\033[38;2;0;{shade};0m{line}{C.R}\n")
            sys.stdout.flush()
            time.sleep(0.04)

    # ── Main Loop ─────────────────────────────────────────────────────────────
    def run(self) -> None:
        self.show_banner()

        # Auto-connect
        ports = list(serial.tools.list_ports.comports())
        if ports:
            self.connect()
        else:
            self._println(f"  {C.RE}No serial port found.{C.R}  Connect ESP32 and run {C.Y}!connect{C.R}.\n")

        self.running = True

        # Background reader thread
        reader = threading.Thread(target=self._reader_thread, daemon=True)
        reader.start()

        # Graceful Ctrl+C
        def _sigint(sig, frame):
            self._println(f"\n  {C.Y}Interrupt. Type !exit to quit.{C.R}")
        signal.signal(signal.SIGINT, _sigint)

        try:
            while self.running:
                # Prompt
                conn_status = (f"{C.BG}●{C.R}" if self.ser and self.ser.is_open
                               else f"{C.RE}○{C.R}")
                prompt = f"  {conn_status} {C.GR}kernelesp{C.R} {C.GR}>{C.R} "

                try:
                    raw = input(prompt).strip()
                except (EOFError, KeyboardInterrupt):
                    self._println()
                    break

                if not raw:
                    continue

                # Local or remote?
                if raw.startswith("!") or raw.lower() in ("exit", "quit"):
                    self.handle_local(raw)
                else:
                    self.send(raw)
                    # Small read delay so response lands before next prompt
                    time.sleep(0.15)

        finally:
            self._save_history()
            self.running = False
            self.disconnect()
            print(f"\n{C.CY}  KernelESP Terminal closed.{C.R}")
            print(f"{C.GR}  Workspace: {WORKSPACE}{C.R}\n")


# ─── Entry Point ──────────────────────────────────────────────────────────────
def main() -> None:
    terminal = KernelESP()
    terminal.run()


if __name__ == "__main__":
    main()