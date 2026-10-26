#!/usr/bin/env python3
"""Client for bf3.exe's debug API (development builds; project/src/dev/debug_api.cpp).

The game listens on 127.0.0.1:4790 (BF3_DEBUG_API=<port>); WSL reaches it through mirrored networking. Requests are
one line of words, replies one line of JSON ("ok", then the command's members); "shot" is followed by raw BGRA.

Command line:
  bf3dbg.py <command words...>          send one command, print the JSON reply (e.g. status, help, key W 2000)
  bf3dbg.py wait <phase> [timeout_s]    wait until status.phase is <phase> (boot, main_menu, menu, loading,
                                        spawn_screen, playing, ingame_menu)
  bf3dbg.py shot <out.png>              capture the game window
  bf3dbg.py monitor [interval_s]        print a status line every interval
  bf3dbg.py watch <addr> [len] [hits] [secs] [w|rw]    hardware watch; prints the accesses by guest function
  bf3dbg.py trace <sub_X> [hits] [secs]                 calls of a guest function: registers, caller, return
  bf3dbg.py bp | messages read|stats | crash            breakpoints; message trace with names; last crash report
  bf3dbg.py run [secs] [--metro] [--team us|ru] [--kit n] [--spot name] [--walk ms] [--look yaw pitch]
                [--exe-args ...]
                                        deploy + launch the game (scripts/win_run.sh, in the background), wait for the
                                        API; with --metro, go straight to Metro TDM and deploy (no menu clicks)
  bf3dbg.py tp <spot|x,y,z[,heading]>  teleport (spots: sunny_street, balcony) and set the spot's aim
  bf3dbg.py metro [--team ru] [--kit 0] [--spot name] [--walk ms] [--look yaw pitch]
                                        the Metro TDM recipe on an already running game

As a module: `from bf3dbg import Bf3` -> Bf3().cmd("status"), .wait_phase("playing"), .shot("x.png"), .metro().
"""

import argparse
import json
import re
import os
import socket
import struct
import subprocess
import sys
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PORT = int(os.environ.get("BF3_DEBUG_API", "4790") or 4790)
METRO_TDM = "Levels/MP_Subway/MP_Subway:GameMode=TeamDeathMatch0"

# Metro TDM test spots: soldier origin (x, y, z), heading (status player.heading) and aim (look yaw, pitch).
SPOTS = {
    # Sunny street, pale pavement: best for viewmodel/body checks (looking down at the legs).
    "sunny_street": ((-229.091, 64.437, -543.129), -1.534, (1.534, -1.219)),
    # Balcony over the park.
    "balcony": ((-229.239, 69.573, -537.213), -1.381, (1.381, 0.0)),
}


class Bf3Error(RuntimeError):
    pass


class Bf3:
    def __init__(self, host="127.0.0.1", port=DEFAULT_PORT, connect_timeout=0.0):
        self.host, self.port = host, port
        self.sock = None
        self.buf = b""
        self.connect(connect_timeout)

    def connect(self, timeout=0.0):
        """Connects, retrying for up to `timeout` seconds (the game takes a few seconds to start listening)."""
        deadline = time.time() + timeout
        while True:
            try:
                self.sock = socket.create_connection((self.host, self.port), timeout=5)
                self.sock.settimeout(60)
                self.buf = b""
                return
            except OSError:
                if time.time() >= deadline:
                    raise
                time.sleep(0.5)

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def _read_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(max(65536, n - len(self.buf)))
            if not chunk:
                raise Bf3Error("connection closed")
            self.buf += chunk
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def _read_line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise Bf3Error("connection closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line

    def request(self, line):
        """Sends one command; returns (reply dict, binary bytes or None). Never raises on ok=false."""
        self.sock.sendall(line.encode() + b"\n")
        reply = json.loads(self._read_line())
        binary = self._read_exact(reply["bytes"]) if reply.get("bytes") else None
        return reply, binary

    def cmd(self, line, check=True):
        reply, _ = self.request(line)
        if check and not reply.get("ok"):
            raise Bf3Error(f"{line}: {reply.get('error')}")
        return reply

    def status(self):
        return self.cmd("status")

    def wait(self, predicate, timeout=60.0, poll=0.25, what="condition"):
        """Polls status until predicate(status) is true; returns that status."""
        deadline = time.time() + timeout
        while True:
            s = self.status()
            if predicate(s):
                return s
            if time.time() >= deadline:
                raise Bf3Error(f"timed out after {timeout:.0f} s waiting for {what} (phase {s.get('phase')}, "
                               f"top {s.get('ui', {}).get('top')})")
            time.sleep(poll)

    def wait_phase(self, phase, timeout=60.0):
        return self.wait(lambda s: s.get("phase") == phase, timeout, what=f"phase {phase}")

    def shot(self, path):
        reply, data = self.request("shot")
        if not reply.get("ok"):
            raise Bf3Error(f"shot: {reply.get('error')}")
        write_png(path, reply["width"], reply["height"], data)
        return path

    def deploy(self, kit=0, timeout=60):
        """Deploys from the spawn screen and waits for the soldier. The spawn screen needs a moment before its spawn
        system takes a deploy, so the request is repeated until the soldier exists."""
        self.wait(lambda s: s["phase"] in ("spawn_screen", "playing"), timeout, what="the spawn screen")
        deadline = time.time() + timeout
        while self.status()["phase"] != "playing":
            self.cmd(f"deploy {kit}")
            try:
                self.wait(lambda s: s["phase"] == "playing", 8, what="the soldier")
            except Bf3Error:
                if time.time() >= deadline:
                    raise
        return self.status()

    def tp(self, spot):
        """Teleports to a named spot (SPOTS) or "x,y,z[,heading]", waits for the client to follow, then sets the aim."""
        if spot in SPOTS:
            (x, y, z), heading, aim = SPOTS[spot]
        else:
            v = [float(t) for t in spot.split(",")]
            (x, y, z), heading, aim = v[:3], (v[3] if len(v) > 3 else None), None
        self.cmd(f"teleport {x} {y} {z}" + (f" {heading}" if heading is not None else ""))
        self.wait(lambda s: s["player"].get("pos") and sum((a - b) ** 2 for a, b in zip(s["player"]["pos"], (x, y, z)))
                  < 0.25, 5, what=f"the soldier at {spot}")
        if aim:
            self.cmd(f"look {aim[0]} {aim[1]}")
            time.sleep(0.2)
        return self.status()

    def metro(self, team="ru", kit=0, walk_ms=0, look=None, log=print, spot=None):
        """Main menu -> Metro TDM on the local server -> deploy, without menu input. Returns the playing status."""
        s = self.wait(lambda s: s["phase"] == "main_menu" or s["phase"] in ("spawn_screen", "playing"), 180,
                      what="the main menu")
        if s["phase"] == "main_menu":
            log(f"main menu at {s.get('uptime_s', 0):.1f} s; team {team}, loading Metro TDM")
            self.cmd(f"team {team}")
            self.cmd(f"level {METRO_TDM}")
            t0 = time.time()
            self.wait(lambda s: s["phase"] == "spawn_screen" and "MP_Subway" in s["level"]["name"], 120,
                      what="the Metro spawn screen")
            log(f"spawn screen after {time.time() - t0:.1f} s")
        if self.status()["phase"] != "playing":
            self.deploy(kit)
        s = self.status()
        log(f"playing: pos {fmt_vec(s['player'].get('pos'))} team {s['player']['team']}")
        if walk_ms or look or spot:
            # The round start holds the soldier for its countdown (status round.state).
            t0 = time.time()
            self.wait(lambda s: s.get("round", {}).get("state") == "running", 30, what="the round start")
            log(f"round running after {time.time() - t0:.1f} s")
        if spot:
            s = self.tp(spot)
            log(f"at {spot}: pos {fmt_vec(s['player'].get('pos'))}")
        if walk_ms:
            self.cmd(f"key W {walk_ms}")
            time.sleep(walk_ms / 1000 + 0.3)
        if look:
            self.cmd(f"look {look[0]} {look[1]}")
            time.sleep(0.2)
        return self.status()


_maps = {}
_msg_names = None


def message_names():
    """Frostbite message hashes (djb2-xor of the class name, see scripts/msg_names.py) -> names, from the BF.Main image;
    cached in logs/dbg/msg_names.json."""
    global _msg_names
    if _msg_names is not None:
        return _msg_names
    cache = os.path.join(ROOT, "logs", "dbg", "msg_names.json")
    if os.path.exists(cache):
        _msg_names = {int(k): v for k, v in json.load(open(cache)).items()}
        return _msg_names
    image_path = os.path.join(ROOT, "dump", "Disc 1", "BF.Main.image.bin")
    _msg_names = {}
    if os.path.exists(image_path):
        image = open(image_path, "rb").read()
        for m in set(re.findall(rb"[A-Za-z_][A-Za-z0-9_]{2,80}", image)):
            h = 5381
            for c in m:
                h = ((h * 33) ^ c) & 0xFFFFFFFF
            _msg_names.setdefault(h, m.decode())
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        json.dump({str(k): v for k, v in _msg_names.items()}, open(cache, "w"))
    return _msg_names


def print_breakpoints(bps):
    for b in bps:
        print(f"DR{b['slot']} {b['kind']} {b['addr']}: {b['hits']} hit(s){' (active)' if b['active'] else ''}")
        for h in b["records"]:
            if b["kind"] == "trace":
                regs = " ".join(f"r{i + 3}={v}" for i, v in enumerate(h["r3_r10"][:5]))
                ret = f" -> r3={h['ret_r3']} f1={h['ret_f1']:.6g} ({h['us']:.1f} us)" if h["returned"] else " (no return seen)"
                print(f"  {h['t']:8.3f}s t{h['tid']} from {h['lr']}  {regs} f1={h['f1_f4'][0]:.6g}{ret}")
            else:
                print(f"  {h['t']:8.3f}s t{h['tid']} value {h['value']}")
            for f in h["frames"][:8]:
                print("      " + resolve_frame(f))


def print_crash_report(log_path=os.path.join(ROOT, "logs", "win-run.log")):
    """Prints the last crash report (and the crash stack) from the game log."""
    lines = open(log_path, errors="replace").read().splitlines()
    begins = [i for i, line in enumerate(lines) if "[crash_report] begin" in line]
    if not begins:
        print("no crash report in", log_path)
        return 1
    start = begins[-1]
    for line in lines[max(0, start - 40):start]:  # the crash_trace stack just before it
        if "[crash_trace]" in line or "Unhandled guest" in line:
            frame = re.search(r"(bf3\S*\.(?:dll|DLL|exe)\+0x[0-9A-Fa-f]+)", line)
            print(line[line.find("]", 30) + 2:] + (f"   {resolve_frame(frame.group(1))}" if frame else ""))
    for line in lines[start:]:
        if "[crash_report]" in line:
            print(line[line.find("[crash_report]"):])
        if "[crash_report] end" in line:
            break
    return 0


def resolve_frame(frame, build_dir=os.path.join(ROOT, "project", "out", "build", "win-cross-release")):
    """"bf3_BF_Main.dll+0x1234" -> "bf3_BF_Main.dll!sub_83264488+0x1a" from the linker map (recompiled functions are
    sub_<guest address>)."""
    import bisect
    module, _, rva = frame.partition("+0x")
    if not rva:
        return frame
    if module not in _maps:
        path = os.path.join(build_dir, os.path.splitext(module)[0] + ".map")
        if os.path.exists(path):
            sys.path.insert(0, os.path.join(ROOT, "scripts"))
            from resolve_rva import load_map
            _maps[module] = load_map(path)
        else:
            _maps[module] = None
    table = _maps[module]
    if not table:
        return frame
    addrs, names = table
    i = bisect.bisect_right(addrs, int(rva, 16)) - 1
    return f"{module}!{names[i]}+{int(rva, 16) - addrs[i]:#x}" if i >= 0 else frame


def write_png(path, width, height, bgra):
    """Top-down BGRA -> RGB PNG (stdlib only)."""
    stride = width * 4
    raw = bytearray()
    for y in range(height):
        row = bgra[y * stride:(y + 1) * stride]
        rgb = bytearray(width * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
        raw += b"\x00" + rgb

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)


def fmt_vec(v):
    return "(" + ", ".join(f"{x:.2f}" for x in v) + ")" if v else "-"


def status_line(s):
    p, perf = s.get("player", {}), s.get("perf", {})
    parts = [f"{s.get('uptime_s', 0):7.1f}s", f"{s['phase']:<12}", f"{s.get('level', {}).get('name', '')[-24:]:<24}"]
    if perf.get("frames"):
        parts.append(f"{perf['fps']:5.1f} fps p99 {perf['p99_ms']:5.1f} ms")
    if p.get("pos"):
        parts.append(f"pos {fmt_vec(p['pos'])} aim {p['aim_yaw']:+.3f}/{p['aim_pitch']:+.3f} {p['speed']:.1f} m/s")
    parts.append(f"top {s.get('ui', {}).get('top', '')}")
    return "  ".join(parts)


def launch(secs, exe_args, env_extra=None):
    """Runs scripts/win_run.sh in the background (deploys first); output in logs/bf3dbg-run.log."""
    env = dict(os.environ, **(env_extra or {}))
    log = open(os.path.join(ROOT, "logs", "bf3dbg-run.log"), "w")
    return subprocess.Popen([os.path.join(ROOT, "scripts", "win_run.sh"), str(secs), "info", *exe_args],
                            stdout=log, stderr=subprocess.STDOUT, env=env, cwd=ROOT)


def close_running():
    """Quits a game that still answers on the port (its exe would block the deploy and the port)."""
    try:
        old = Bf3()
    except OSError:
        return
    pid = old.cmd("ping").get("pid")
    try:
        old.request("quit")
    except (Bf3Error, OSError):
        pass
    old.close()
    print(f"closed the running game (pid {pid})")
    for _ in range(40):
        try:
            Bf3().close()
            time.sleep(0.25)
        except OSError:
            return


def main():
    argv = sys.argv[1:]
    if not argv:
        print(__doc__)
        return 2
    verb = argv[0]

    if verb in ("run", "metro"):
        ap = argparse.ArgumentParser(prog=f"bf3dbg.py {verb}")
        if verb == "run":
            ap.add_argument("secs", nargs="?", type=int, default=300)
            ap.add_argument("--metro", action="store_true")
            ap.add_argument("--exe-args", nargs=argparse.REMAINDER, default=[])
        ap.add_argument("--team", default="ru", choices=["us", "ru"])
        ap.add_argument("--kit", type=int, default=0)
        ap.add_argument("--walk", type=int, default=0, help="hold W this many ms after deploying")
        ap.add_argument("--look", nargs=2, type=float, metavar=("YAW", "PITCH"))
        ap.add_argument("--spot", help="teleport to a test spot: " + ", ".join(SPOTS) + " or x,y,z[,heading]")
        ap.add_argument("--shot", help="capture the game window to this PNG at the end")
        a = ap.parse_args(argv[1:])
        proc = None
        if verb == "run":
            close_running()
            proc = launch(a.secs, a.exe_args)
            print(f"launched (pid {proc.pid}, {a.secs} s); waiting for the debug API")
        bf = Bf3(connect_timeout=90 if proc else 0)
        if verb == "metro" or a.metro:
            s = bf.metro(a.team, a.kit, a.walk, a.look, spot=a.spot)
            print(status_line(s))
        if a.shot:
            print("shot:", bf.shot(a.shot))
        return 0

    if verb == "crash":  # reads the log; the game may be gone
        return print_crash_report()
    try:
        bf = Bf3()
    except OSError:
        print(f"bf3dbg: no game answering on 127.0.0.1:{DEFAULT_PORT} (development build running?)", file=sys.stderr)
        return 1
    if verb == "tp":
        print(status_line(bf.tp(argv[1])))
        return 0
    if verb == "quit":
        try:
            bf.request("quit")
        except (Bf3Error, OSError):
            pass  # the process ends before it can answer
        print("quit")
        return 0
    if verb == "wait":
        phase = argv[1]
        timeout = float(argv[2]) if len(argv) > 2 else 120
        print(status_line(bf.wait_phase(phase, timeout)))
        return 0
    if verb == "shot":
        print(bf.shot(argv[1] if len(argv) > 1 else os.path.join(ROOT, "logs", "dbg", "shot.png")))
        return 0
    if verb == "monitor":
        interval = float(argv[1]) if len(argv) > 1 else 1.0
        while True:
            print(status_line(bf.status()), flush=True)
            time.sleep(interval)
    if verb in ("watch", "trace") and len(argv) > 1 and argv[1] != "off":
        # Arm, wait for the hits (or the time), then print the records by guest function.
        reply = bf.cmd(" ".join(argv))
        addr = argv[1][4:] if argv[1].startswith("sub_") else argv[1]
        target = int(addr, 16)
        while True:
            mine = [b for b in reply["breakpoints"] if int(b["addr"], 16) == target and
                    (b["kind"] == "trace") == (verb == "trace")]
            if not mine or not mine[-1]["active"]:
                break
            time.sleep(0.25)
            reply = bf.cmd("bp")
        print_breakpoints(mine)
        return 0
    if verb == "bp":
        print_breakpoints(bf.cmd("bp")["breakpoints"])
        return 0
    if verb == "messages" and (len(argv) == 1 or argv[1] in ("read", "stats")):
        reply = bf.cmd(" ".join(argv))
        names = message_names()
        for e in reply["items"]:
            cat, typ = int(e["category"], 16), int(e["type"], 16)
            head = f"{e['t']:9.3f}s seq {e['seq']:<6} " if "seq" in e else f"{e['count']:>7} x  "
            print(f"{head}{names.get(typ, e['type'])}  [{names.get(cat, e['category'])}]"
                  + (f"  from {e['lr']}" if "lr" in e else ""))
        return 0
    reply, _ = bf.request(" ".join(argv))
    print(json.dumps(reply, indent=2))
    return 0 if reply.get("ok") else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Bf3Error, OSError) as e:
        print(f"bf3dbg: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)
