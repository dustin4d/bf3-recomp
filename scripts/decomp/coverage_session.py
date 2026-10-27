#!/usr/bin/env python3
"""Records which guest functions a full Metro TDM session runs (coverage build, see docs/DECOMP.md).

Launches the game (development build with coverage), then drives it through the debug API and dumps the hit set at
each phase boundary, clearing it as it reads (bf3dbg `coverage dump reset`), so every phase file holds the functions
first or again run in that phase:
  boot (launch -> main menu), load (Metro TDM until the spawn screen), then per kit and team: deploy, movement, aim,
  fire, reload, weapon switch, gadgets, grenade, knife, fall damage, death, respawn; round end; leave to the menu.
Output: logs/coverage/<run>/<NN>-<phase>.tsv (module<TAB>0xADDR) and phases.json (timing, per-phase counts, notes).

Usage: coverage_session.py <run name> [--teams ru,us] [--kits 0,1,2,3] [--secs 900]
"""
import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from bf3dbg import Bf3, Bf3Error, ROOT, SPOTS, close_running, launch  # noqa: E402

KITS = ["assault", "engineer", "support", "recon"]


class Session:
    def __init__(self, bf, out_dir):
        self.bf, self.out, self.n = bf, out_dir, 0
        self.phases = []
        self.t0 = time.time()

    def log(self, msg):
        print(f"[{time.time() - self.t0:7.1f}s] {msg}", flush=True)

    def dump(self, phase, note=""):
        self.n += 1
        path = os.path.join(self.out, f"{self.n:02d}-{phase}.tsv")
        reply = self.bf.coverage_dump(path, reset=True)
        counts = {m["name"]: m["hits"] for m in reply["modules"]}
        self.phases.append({"phase": phase, "file": os.path.basename(path), "t": round(time.time() - self.t0, 1),
                            "hits": counts, "note": note})
        self.log(f"{phase}: {counts}" + (f"  ({note})" if note else ""))
        # A playing phase runs thousands of functions; a few hundred means the game stalled (2026-10-27 run2: the
        # GPU stopped completing fences, main thread in the GPU wait sub_82FCC8D8). Stop instead of recording that.
        if phase.split("-")[0] in ("move", "weapons", "gadgets") and counts.get("BF.Main", 0) < 1000:
            self.save()
            raise Bf3Error(f"game stalled ({phase}: {counts.get('BF.Main', 0)} functions)")

    def step(self, what, fn):
        """Runs one scripted action; a failure is noted, not fatal (the next phases still record)."""
        try:
            fn()
            return True
        except (Bf3Error, OSError, KeyError) as e:
            self.log(f"  {what} failed: {e}")
            self.phases.append({"phase": "error", "what": what, "error": str(e)})
            return False

    def key(self, keys, ms, wait=None):
        self.bf.cmd(f"key {keys} {ms}")
        time.sleep((wait if wait is not None else ms) / 1000 + 0.25)

    def pad(self, buttons, ms=150, wait=0.4):
        self.bf.cmd(f"pad {buttons} {ms}")
        time.sleep(ms / 1000 + wait)

    def save(self):
        with open(os.path.join(self.out, "phases.json"), "w") as f:
            json.dump(self.phases, f, indent=1)


def play_kit(s, kit):
    bf = s.bf
    st = bf.status()
    if st["phase"] != "playing":
        s.step("deploy", lambda: bf.deploy(kit))
    s.step("round start", lambda: bf.wait(lambda x: x.get("round", {}).get("state") == "running", 30,
                                          what="round start"))
    s.step("to spot", lambda: bf.tp("sunny_street"))
    s.dump(f"deploy-{KITS[kit]}")
    # movement
    s.step("walk", lambda: s.key("W", 2000))
    s.step("strafe", lambda: (s.key("A", 800), s.key("D", 800), s.key("S", 800)))
    s.step("sprint", lambda: s.key("W+Shift", 2500))
    s.step("jump", lambda: (s.key("Space", 120, 900), s.key("W+Space", 400, 1200)))
    s.step("crouch", lambda: (s.key("C", 1200), s.key("X", 120, 800), s.key("W", 800), s.key("X", 120, 800)))
    s.step("prone", lambda: (s.key("Z", 120, 1500), s.key("W", 1000), s.key("Z", 120, 1500)))
    s.step("look", lambda: (bf.cmd("mouse look 600 0"), time.sleep(0.6), bf.cmd("mouse look -300 -200"),
                            time.sleep(0.6)))
    s.dump(f"move-{KITS[kit]}")
    # weapons
    s.step("fire", lambda: s.key("LMB", 1500))
    s.step("aim fire", lambda: (s.key("RMB", 2000, 400), s.key("LMB", 800, 1800)))
    s.step("fire mode", lambda: s.key("V", 120, 500))
    s.step("light", lambda: (s.key("T", 120, 500), s.key("T", 120, 500)))
    s.step("reload", lambda: s.key("R", 120, 3500))
    s.step("empty mag", lambda: s.key("LMB", 6000))
    s.step("reload empty", lambda: s.key("R", 120, 4000))
    s.step("pistol", lambda: (s.key("2", 120, 1500), s.key("LMB", 1200), s.key("R", 120, 2500)))
    s.step("primary", lambda: s.key("1", 120, 1500))
    s.dump(f"weapons-{KITS[kit]}")
    # gadgets: 3/4 by key, and the D-pad (PC keys 3/4 don't select gadgets in this build: open issue)
    for g, k, d in (("gadget1", "3", "LEFT"), ("gadget2", "4", "RIGHT")):
        s.step(g, lambda k=k, d=d: (s.key(k, 120, 1500), s.pad(d, 150, 1.5), s.key("LMB", 400, 2500),
                                    s.key("RMB", 400, 1500), s.key("LMB", 400, 2500)))
    s.step("primary", lambda: s.key("1", 120, 1500))
    s.step("grenade", lambda: s.key("G", 150, 3000))
    s.step("knife", lambda: (s.key("F", 120, 1500), s.key("F", 120, 1500)))
    s.step("spot", lambda: s.key("Q", 120, 800))
    s.step("scoreboard", lambda: s.key("Tab", 1500))
    s.dump(f"gadgets-{KITS[kit]}")
    # fall damage, then death and respawn on the next kit
    (x, y, z), heading, _ = SPOTS["sunny_street"]
    s.step("fall", lambda: (bf.cmd(f"teleport {x} {y + 14} {z} {heading}"), time.sleep(4)))
    st = bf.status()
    s.dump(f"damage-{KITS[kit]}", f"health {st.get('player', {}).get('health')}")
    s.step("kill", lambda: (bf.cmd("kill"), bf.wait(lambda x: x["phase"] in ("dead", "spawn_screen"), 15,
                                                    what="death"), time.sleep(6)))
    s.dump(f"death-{KITS[kit]}", bf.status()["phase"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--teams", default="ru,us")
    ap.add_argument("--kits", default="0,1,2,3")
    ap.add_argument("--secs", type=int, default=1200)
    a = ap.parse_args()
    out = os.path.join(ROOT, "logs", "coverage", a.run)
    os.makedirs(out, exist_ok=True)
    close_running()
    proc = launch(a.secs, [])
    bf = Bf3(connect_timeout=90)
    s = Session(bf, out)
    try:
        s.log(f"launched pid {proc.pid}")
        bf.wait(lambda x: x["phase"] == "main_menu", 180, what="the main menu")
        time.sleep(3)  # let the boot's profile load/save finish before the level switch (see `level` in the API)
        s.dump("boot")
        teams = a.teams.split(",")
        kits = [int(k) for k in a.kits.split(",")]
        for ti, team in enumerate(teams):
            if ti == 0:
                bf.cmd(f"team {team}")
                bf.cmd("level")
                bf.wait(lambda x: x["phase"] == "spawn_screen" and "MP_Subway" in x["level"]["name"], 150,
                        what="the Metro spawn screen")
                s.dump("load")
                s.step("typeinfo", lambda: save_memory(bf, 0x84155000, 0x43858, os.path.join(out, "typeinfo.bin")))
            else:
                s.step("team switch", lambda: (bf.cmd(f"team {team}"), time.sleep(5)))
                s.dump(f"team-{team}", bf.status()["phase"])
            for kit in kits:
                s.step("spawn screen", lambda: bf.wait(lambda x: x["phase"] in ("spawn_screen", "playing", "dead"),
                                                       30, what="spawn screen"))
                if bf.status()["phase"] == "dead":
                    time.sleep(5)
                play_kit(s, kit)
                s.save()
        # round end, then leave to the main menu through the in-game menu
        s.step("round end", lambda: round_end(s))
        s.dump("round-end", bf.status()["phase"])
        s.step("leave", lambda: leave_to_menu(s))
        s.dump("leave", bf.status()["phase"])
    finally:
        s.save()
        try:
            bf.request("quit")
        except (Bf3Error, OSError):
            pass
        proc.wait(timeout=60)
    print(f"phases in {out}")


def save_memory(bf, addr, size, path):
    """Guest memory as the running game has it (e.g. the typeinfo section after static init) -> file."""
    data = bytearray()
    for a in range(addr, addr + size, 4096):
        n = min(4096, addr + size - a)
        data += bytes.fromhex(bf.cmd(f"peek {a:X} {n}")["hex"].replace(" ", ""))
    with open(path, "wb") as f:
        f.write(data)


def round_end(s):
    """TDM ends at the kill goal (ServerKillCounterEntity +0xA8, 100); solo there are no enemy kills, so the goal is
    lowered and the score raised through the entity (see `entities` in the API)."""
    bf = s.bf
    reply = bf.cmd("entities ServerKillCounterEntity")
    ents = [int(a, 16) for a in re.findall(r"0x([0-9A-F]{8})/vt", json.dumps(reply))]
    s.log(f"  kill counters: {[hex(e) for e in ents]}")
    for addr in ents:
        bf.cmd(f"poke {addr + 0xA8:X} u32 1")
    bf.cmd("respawn 0")
    time.sleep(8)
    bf.cmd("kill")
    time.sleep(20)


def leave_to_menu(s):
    bf = s.bf
    if bf.status()["phase"] != "main_menu":
        bf.cmd("level Levels/FrontEnd/FrontEnd")
    bf.wait(lambda x: x["phase"] == "main_menu", 120, what="the main menu")
    time.sleep(5)


if __name__ == "__main__":
    main()
