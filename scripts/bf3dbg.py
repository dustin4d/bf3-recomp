#!/usr/bin/env python3
"""Client for bf3.exe's debug API (development builds; project/src/dev/debug_api.cpp).

The game listens on 127.0.0.1:4790 (BF3_DEBUG_API=<port>); WSL reaches it through mirrored networking. Requests are
one line of words, replies one line of JSON ("ok", then the command's members); "shot" is followed by raw BGRA.

Command line:
  bf3dbg.py <command words...>          send one command, print the JSON reply (e.g. status, help, key W 2000)
  bf3dbg.py wait <phase> [timeout_s]    wait until status.phase is <phase> (boot, main_menu, menu, loading,
                                        spawn_screen, playing, ingame_menu)
  bf3dbg.py shot <out.png>              capture the game window
  bf3dbg.py watch [interval_s]          print a status line every interval
  bf3dbg.py run [secs] [--metro] [--team us|ru] [--kit n] [--walk ms] [--look yaw pitch] [--exe-args ...]
                                        deploy + launch the game (scripts/win_run.sh, in the background), wait for the
                                        API; with --metro, go straight to Metro TDM and deploy (no menu clicks)
  bf3dbg.py metro [--team ru] [--kit 0] [--walk ms] [--look yaw pitch]
                                        the Metro TDM recipe on an already running game

As a module: `from bf3dbg import Bf3` -> Bf3().cmd("status"), .wait_phase("playing"), .shot("x.png"), .metro().
"""

import argparse
import json
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

    def wait_movable(self, timeout=30.0):
        """After a deploy the round start holds the soldier for a few seconds (pre-round countdown; no round state in
        the API yet): taps W until the position changes. Returns the seconds waited."""
        t0 = time.time()
        start = self.status()["player"].get("pos")
        while time.time() - t0 < timeout:
            self.cmd("key W 250")
            time.sleep(0.6)
            pos = self.status()["player"].get("pos")
            if start and pos and sum((a - b) ** 2 for a, b in zip(pos, start)) > 0.01:
                return time.time() - t0
        raise Bf3Error(f"the soldier did not move within {timeout:.0f} s")

    def metro(self, team="ru", kit=0, walk_ms=0, look=None, log=print):
        """Main menu -> Metro TDM on the local server -> deploy, without menu input. Returns the playing status."""
        s = self.wait(lambda s: s["phase"] == "main_menu" or s["phase"] in ("spawn_screen", "playing"), 180,
                      what="the main menu")
        if s["phase"] == "main_menu":
            log(f"main menu at {s.get('uptime_s', 0):.1f} s; team {team}, loading Metro TDM")
            self.cmd(f"team {team}")
            self.cmd(f"level {METRO_TDM}")
            t0 = time.time()
            self.wait(lambda s: s["phase"] == "spawn_screen" and "MP_Subway" in s["level"]["name"], 240,
                      what="the Metro spawn screen")
            log(f"spawn screen after {time.time() - t0:.1f} s")
        if self.status()["phase"] != "playing":
            # The spawn screen needs a moment before its spawn system takes a deploy; retry until the soldier exists.
            deadline = time.time() + 60
            while True:
                self.cmd(f"deploy {kit}")
                try:
                    self.wait(lambda s: s["phase"] == "playing", 8, what="the soldier")
                    break
                except Bf3Error:
                    if time.time() >= deadline:
                        raise
        s = self.status()
        log(f"playing: pos {fmt_vec(s['player'].get('pos'))} team {s['player']['team']}")
        if walk_ms or look:
            log(f"movable after {self.wait_movable():.1f} s")
            self.cmd(f"key W {walk_ms}")
            time.sleep(walk_ms / 1000 + 0.3)
        if look:
            self.cmd(f"look {look[0]} {look[1]}")
            time.sleep(0.2)
        return self.status()


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
        ap.add_argument("--shot", help="capture the game window to this PNG at the end")
        a = ap.parse_args(argv[1:])
        proc = None
        if verb == "run":
            close_running()
            proc = launch(a.secs, a.exe_args)
            print(f"launched (pid {proc.pid}, {a.secs} s); waiting for the debug API")
        bf = Bf3(connect_timeout=90 if proc else 0)
        if verb == "metro" or a.metro:
            s = bf.metro(a.team, a.kit, a.walk, a.look)
            print(status_line(s))
        if a.shot:
            print("shot:", bf.shot(a.shot))
        return 0

    try:
        bf = Bf3()
    except OSError:
        print(f"bf3dbg: no game answering on 127.0.0.1:{DEFAULT_PORT} (development build running?)", file=sys.stderr)
        return 1
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
    if verb == "watch":
        interval = float(argv[1]) if len(argv) > 1 else 1.0
        while True:
            print(status_line(bf.status()), flush=True)
            time.sleep(interval)
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
