#!/usr/bin/env python3
"""End-to-end check: starts the game on a scratch world (on a free port), talks MCP to
tools/flycoos_mcp.py over stdio, edits, looks, undoes and saves, sends hostile input straight to the
game's port, then quits the game with SIGTERM.

    python3 tools/test_mcp.py [--world /tmp/e2e.flyw] [--binary build/dev/flycoos] [--keep DIR]

Needs a display (the game opens its window). --keep copies the screenshot and map PNGs to DIR.
"""
import argparse
import base64
import json
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--world")
    ap.add_argument("--binary", default=os.path.join(ROOT, "build/release/flycoos"))
    ap.add_argument("--keep")
    opt = ap.parse_args()
    scratch = tempfile.mkdtemp(prefix="flycoos-e2e-")
    opt.world = opt.world or os.path.join(scratch, "e2e.flyw")
    if os.path.exists(opt.world):
        os.remove(opt.world)
    with socket.socket() as s:  # a free port, so a game the user is running isn't touched
        s.bind(("127.0.0.1", 0))
        PORT = s.getsockname()[1]

    game = subprocess.Popen([opt.binary, "--world", opt.world, "--port", str(PORT)], cwd=ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    bridge = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools/flycoos_mcp.py")], cwd=ROOT,
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                              env={**os.environ, "FLYCOOS_PORT": str(PORT)})
    failures = []

    def check(cond, what):
        print(("ok   " if cond else "FAIL ") + what)
        if not cond:
            failures.append(what)

    next_id = iter(range(1, 1000))
    notices = []  # notifications the bridge sent

    def rpc(method, params=None, server=None):
        server = server or bridge
        msg = {"jsonrpc": "2.0", "id": next(next_id), "method": method, "params": params or {}}
        server.stdin.write(json.dumps(msg) + "\n")
        server.stdin.flush()
        while "id" not in (reply := json.loads(server.stdout.readline())):
            notices.append(reply["method"])
        assert reply["id"] == msg["id"], reply
        return reply["result"]

    def schemas(server=None):
        return {t["name"]: t["inputSchema"]["properties"] for t in rpc("tools/list", server=server)["tools"]}

    def tool(name, /, **args):  # positional-only: markers take a `name` argument
        r = rpc("tools/call", {"name": name, "arguments": args})
        text = next(c["text"] for c in r["content"] if c["type"] == "text")
        images = [base64.b64decode(c["data"]) for c in r["content"] if c["type"] == "image"]
        return r.get("isError", False), (text if r.get("isError") else json.loads(text)), images

    def raw(data):
        """Sends bytes straight to the game's port; returns everything it answers until it closes."""
        out = b""
        with socket.create_connection(("127.0.0.1", PORT), timeout=10) as s:
            s.sendall(data)
            while chunk := s.recv(1 << 16):
                out += chunk
        return out

    def png_size(data):
        return struct.unpack(">II", data[16:24]) if data[:8] == b"\x89PNG\r\n\x1a\n" else None

    try:
        init = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "e2e", "version": "1"}})
        check(init["protocolVersion"] == "2025-06-18" and "tools" in init["capabilities"], "initialize")
        bridge.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        first = schemas()
        names = set(first)
        check({"state", "raise", "path", "apply", "undo", "save", "screenshot", "map", "heights", "inspect", "goto",
               "prefabs", "place", "remove", "move", "instances", "markers", "mark", "edit_marker", "unmark"} <= names,
              f"tools/list ({len(names)} tools)")

        for _ in range(3000):  # the game generates its world first (~1 s release, ~1-2 min with sanitizers)
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
                break
            except OSError:
                time.sleep(0.1)
        err, state, _ = tool("state")
        check(not err and state["size"] == [1024, 192, 1024] and state["undo_steps"] == 0 and state["markers"] == 4,
              "state (a new world has the 4 default spawn markers)")
        if err:
            raise RuntimeError(state)
        x, z = int(state["player"][0]) + 20, int(state["player"][2]) + 20

        # the bridge lists the game's NPC kinds and materials (its catalog command), not its own copies
        unfilled = "enum" not in first["mark"]["entity"]
        check(unfilled == ("notifications/tools/list_changed" in notices),
              f"a tool list made before the game answered is followed by list_changed once it does (first list {'un' if unfilled else ''}filled)")
        listed = schemas()
        with socket.create_connection(("127.0.0.1", PORT), timeout=10) as s:
            s.sendall(b'{"id": 1, "cmd": "catalog"}\n')
            cat = json.loads(s.makefile().readline())["result"]
        kinds = [e["id"] for e in cat["entities"]]
        check(listed["mark"]["entity"]["enum"] == kinds == listed["edit_marker"]["entity"]["enum"] and
              kinds[:8] == ["dummy", "villager", "boar", "bandit", "wolf", "guard", "bandit_chief", "ghoul"] and
              "bandit_chief (Lv 3" in listed["mark"]["entity"]["description"], f"mark lists the game's {len(kinds)} NPC kinds")
        check(listed["paint"]["material"]["enum"] == cat["solid_materials"] and "Grass" in cat["solid_materials"] and
              "Air" not in cat["solid_materials"] and listed["fill"]["material"]["enum"] == cat["materials"] and
              listed["path"]["material"]["enum"] == cat["solid_materials"], "paint, path and fill list the game's materials")
        check({"Ember", "Planks", "Cobble", "Thatch", "Lantern", "Slate", "Air", "Water"} <= set(cat["materials"]) and
              {"Ember", "Planks", "Lantern"} <= set(cat["solid_materials"]) and "Water" not in cat["solid_materials"],
              "the materials are every block (building blocks too; paint takes the solid ones)")
        offline = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools/flycoos_mcp.py")], cwd=ROOT,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                   env={**os.environ, "FLYCOOS_PORT": "1"})
        try:
            alone = schemas(offline)
        finally:
            offline.stdin.close()
            offline.wait(5)
        check("enum" not in alone["mark"]["entity"] and "couldn't get the names from the game" in alone["mark"]["entity"]["description"] and
              "enum" not in alone["paint"]["material"], "without the game the tool list says the names come from it")

        def height():
            return tool("heights", x0=x, z0=z, x1=x, z1=z)[1]["heights"][0][0]

        before = height()
        err, r, _ = tool("raise", x=x, z=z, radius=6, strength=5)
        check(not err and r["changed"] and r["undo_steps"] == 1, "raise")
        check(height() == before + 5, f"heights shows the raise ({before} -> {before + 5})")
        err, r, _ = tool("path", points=[[x - 30, z], [x + 30, z + 10]], width=5, material="Dirt")
        check(not err and r["changed"] and r["undo_steps"] == 2, f"path ({r['ops'] if not err else r} ops, one undo step)")
        err, msg, _ = tool("raise", x=x, z=z, radius=99)
        check(err and "radius" in msg, f"bad args are a tool error ({msg})")

        t = time.time()
        err, r, images = tool("screenshot", position=[x - 40, before + 30, z + 40], look_at=[x, before, z], width=960, height=540)
        check(not err and images and png_size(images[0]) == (960, 540), f"screenshot PNG 960x540 (render {r.get('render_ms', 0):.1f} ms, round trip {1000 * (time.time() - t):.0f} ms)")
        shot = r.get("path")
        err, r, images = tool("map", x0=x - 128, z0=z - 128, x1=x + 127, z1=z + 127)
        check(not err and images and png_size(images[0]) == (1024, 1024), f"map PNG 1024x1024 (heights {r.get('min_height')}..{r.get('max_height')})")
        if opt.keep:
            os.makedirs(opt.keep, exist_ok=True)
            shutil.copy(shot, os.path.join(opt.keep, "e2e_screenshot.png"))
            shutil.copy(r["path"], os.path.join(opt.keep, "e2e_map.png"))
        for png in (shot, r.get("path")):
            if png:
                os.remove(png)

        # hostile input straight to the port: the game survives and runs nothing
        steps = tool("state")[1]["undo_steps"]
        reply = raw(b'{"id": 1, "cmd": "state\xc3"}\n')  # invalid UTF-8; the game drops the idle connection after ~5 s
        check(json.loads(reply)["ok"] is False and not tool("state")[0], "invalid UTF-8 is an error reply, the game keeps running")
        check(raw(b"\xc3\n") == b"", "a line that isn't a JSON object closes the connection")
        body = json.dumps({"id": 1, "cmd": "raise", "args": {"x": x, "z": z, "radius": 6, "strength": 5}}) + "\n"
        reply = raw(f"POST / HTTP/1.1\r\nHost: 127.0.0.1:{PORT}\r\nContent-Type: text/plain\r\n"
                    f"Content-Length: {len(body)}\r\n\r\n{body}".encode())
        check(reply == b"" and tool("state")[1]["undo_steps"] == steps, "an HTTP POST is cut off at its request line, its body never runs")
        outside = os.path.join(tempfile.gettempdir(), "flycoos-e2e-outside.flyw")
        err, msg, _ = tool("save", path=outside)
        check(err and not os.path.exists(outside), f"save outside worlds/ and the world's folder is refused ({msg})")

        err, r, _ = tool("undo")
        err2, r2, _ = tool("undo")
        check(not err and not err2 and r2["undo_steps"] == 0 and height() == before, "undo restores the heights")
        spot = [x, before + 12, z]
        err, r, _ = tool("fill", min=spot, max=spot, material="Ember")
        err2, r2, _ = tool("inspect", min=spot, max=spot)
        check(not err and not err2 and r2["blocks"] == {"Ember": 1}, "fill places a building block (Ember)")
        tool("undo")

        # prefabs: a file written now is usable after a rescan; place, move, remove, undo
        hut = os.path.join(ROOT, "prefabs", "e2e-hut.txt")
        with open(hut, "w") as f:
            f.write("description: e2e test hut\nfoundation: Cobble\nC = Cobble\nP = Planks\nlayer\nCCC\nCCC\nlayer\nPPP\nP.P\nP P\n")
        with open(os.path.join(ROOT, "prefabs", "e2e-broken.txt"), "w") as f:
            f.write("C = Cobble\nlayer\nCX\n")
        try:
            err, r, _ = tool("prefabs")
            names = [p["name"] for p in r["prefabs"]] if not err else []
            check(not err and {"tree", "pine", "bush", "boulder", "spire", "cottage", "camp", "e2e-hut"} <= set(names) and
                  r["errors"] == ["e2e-broken.txt: line 3: 'X' is not in the legend"] and "Cobble" in r["blocks"],
                  f"prefabs lists {len(names)} prefabs and the broken file's error")
        finally:
            for name in ("e2e-hut.txt", "e2e-broken.txt"):
                os.remove(os.path.join(ROOT, "prefabs", name))
        err, r, _ = tool("place", prefab="e2e-hut", x=x, z=z, rotation=90)
        hut_id = r["instance"]["id"] if not err else None
        check(not err and r["instance"]["rotation"] == 90 and r["instance"]["box"]["min"][1] == before + 1, "place a prefab written just now")
        err, r, _ = tool("place", prefab="cottage", x=x + 12, z=z, seed=7)
        check(not err and r["instance"]["id"] == hut_id + 1, "place the cottage")
        err, r, _ = tool("place", prefab="tree", x=x - 12, z=z, params={"trunk": 12, "radius": 5})
        check(not err and r["instance"]["params"] == {"trunk": 12, "radius": 5}, "place a tree with params")
        err, msg, _ = tool("place", prefab="castle", x=x, z=z)
        err2, msg2, _ = tool("place", prefab="tree", x=x, z=z, params={"trunk": 99})
        check(err and "castle" in msg and err2 and "trunk" in msg2, f"bad prefab and params are tool errors ({msg2})")
        err, r, _ = tool("move", id=hut_id, x=x, z=z + 20)
        check(not err and r["instance"]["id"] == hut_id and r["instance"]["position"][2] == z + 20, "move keeps the id")
        err, r, _ = tool("instances", x0=x - 2, z0=z + 18, x1=x + 2, z1=z + 22)
        check(not err and r["count"] == 1 and r["instances"][0]["prefab"] == "e2e-hut", "instances in a region")
        steps = tool("state")[1]["undo_steps"]
        err, r, _ = tool("remove", id=hut_id)
        check(not err and tool("instances")[1]["count"] == 2 and r["undo_steps"] == steps + 1, "remove")
        tool("undo")
        check(tool("instances")[1]["count"] == 3, "undo brings the instance back")
        err, msg, _ = tool("remove", id=999)
        check(err and "999" in msg, "remove an unknown id is an error")
        # a camp brings its own bandits: the instance's markers, removed with it
        err, r, _ = tool("place", prefab="camp", x=x + 40, z=z)
        camp_id = r["instance"]["id"] if not err else None
        made = r.get("markers", []) if not err else []
        check(len(made) == 1 and made[0]["entity"] == "bandit" and made[0]["count"] == 2 and made[0]["instance"] == camp_id,
              "a placed camp brings its bandits' spawn marker")
        err, r, _ = tool("remove", id=camp_id)
        check(not err and r["markers_removed"] == 1 and tool("markers", kind="spawn")[1]["count"] == 4,
              "removing the camp takes its marker along")
        # story markers: add each kind, edit, list, map, undo; hostile arguments change nothing
        err, r, _ = tool("markers", kind="spawn")
        check(not err and r["count"] == 4 and {m["entity"] for m in r["markers"]} == {"dummy", "villager", "boar", "bandit"},
              "markers lists the default spawns")
        steps = tool("state")[1]["undo_steps"]
        err, well, _ = tool("mark", kind="poi", name="Old Well", x=x, z=z - 10)
        err2, vale, _ = tool("mark", kind="region", name="Quiet Vale", x=x, z=z, radius=60)
        err3, den, _ = tool("mark", kind="spawn", name="Boar Den", x=x + 20, z=z, entity="boar", count=3, radius=8)
        err4, start, _ = tool("mark", kind="player_spawn", x=x - 5, z=z - 5)
        check(not (err or err2 or err3 or err4) and well["marker"]["radius"] == 16 and den["marker"]["count"] == 3 and
              tool("state")[1]["undo_steps"] == steps + 4, "mark each kind (one undo step each)")
        if err or err2 or err3 or err4:
            raise RuntimeError((well, vale, den, start))
        den_id = den["marker"]["id"]
        err, r, _ = tool("edit_marker", id=den_id, x=x + 30, count=5, name="Boar Wallow")
        check(not err and r["marker"]["x"] == x + 30 and r["marker"]["count"] == 5 and r["marker"]["name"] == "Boar Wallow"
              and r["marker"]["entity"] == "boar", "edit_marker moves and renames, keeps the rest")
        err, r, _ = tool("markers", x0=x - 3, z0=z - 3, x1=x + 3, z1=z + 3)
        check(not err and {m["name"] for m in r["markers"]} == {"Quiet Vale"}, "markers in a region (the region's circle reaches in)")
        err, r, images = tool("map", x0=x - 64, z0=z - 64, x1=x + 63, z1=z + 63)
        ids = {m["id"] for m in r.get("markers", [])}
        check(not err and images and {well["marker"]["id"], vale["marker"]["id"], den_id, start["marker"]["id"]} <= ids,
              "the map draws the markers and lists them")
        if opt.keep and not err:
            shutil.copy(r["path"], os.path.join(opt.keep, "e2e_map_markers.png"))
        steps = tool("state")[1]["undo_steps"]
        for args in (dict(kind="castle", name="x", x=x, z=z), dict(kind="spawn", x=x, z=z),
                     dict(kind="spawn", x=x, z=z, entity="dragon"), dict(kind="spawn", x=x, z=z, entity="boar", count=99),
                     dict(kind="poi", x=x, z=z), dict(kind="poi", name="x" * 49, x=x, z=z),
                     dict(kind="poi", name="x", x=5000, z=z), dict(kind="poi", name="x", x=float("nan"), z=z),
                     dict(kind="poi", name="x", x=x, z=z, radius=1e9), dict(kind="poi", name="a\u0000b", x=x, z=z),
                     dict(kind="poi", name="x", x=x, z=z, entity="boar"), dict(kind="player_spawn", x=x, z=z)):
            err, msg, _ = tool("mark", **args)
            check(err, f"mark refuses {str(args)[:60]} ({msg})")
        # dialogue lines on spawn markers: set, listed, cleared; hostile ones change nothing
        err, elder, _ = tool("mark", kind="spawn", name="Elder", x=x + 8, z=z + 8, entity="villager",
                             lines=["Welcome, traveller.", "Gr\u00fc\u00df dich."])
        check(not err and elder["marker"]["lines"] == ["Welcome, traveller.", "Gr\u00fc\u00df dich."], "mark a villager with lines")
        err2, chief, _ = tool("mark", kind="spawn", name="Chief", x=x + 12, z=z + 8, entity="bandit_chief", lines=["Begone."])
        err3, pack, _ = tool("mark", kind="spawn", x=x + 16, z=z + 8, entity="wolf", count=2)
        check(not (err2 or err3) and "lines" not in pack["marker"], "new kinds take spawn markers (a chief with taunt lines)")
        steps = tool("state")[1]["undo_steps"]
        elder_id = elder["marker"]["id"] if not err else 0
        for args in (dict(lines="Hello"), dict(lines=[1]), dict(lines=[""]), dict(lines=["x"] * 9), dict(lines=["x" * 161]),
                     dict(lines=["a\u202eb"]), dict(lines=["a\u200bb"]), dict(lines=["a\nb"]), dict(entity="dummy")):
            err, msg, _ = tool("edit_marker", id=elder_id, **args)
            check(err, f"edit_marker refuses {str(args)[:50]} ({msg})")
        for args in (dict(kind="poi", name="Sign", x=x, z=z, lines=["Hi"]), dict(kind="spawn", x=x, z=z, entity="dummy", lines=["Hi"])):
            err, msg, _ = tool("mark", **args)
            check(err, f"mark refuses {str(args)[:60]} ({msg})")
        check(tool("state")[1]["undo_steps"] == steps, "refused lines change nothing")
        err, r, _ = tool("edit_marker", id=elder_id, lines=[])
        listed = [m for m in tool("markers", kind="spawn")[1]["markers"] if m["id"] == elder_id]
        check(not err and listed and "lines" not in listed[0], "lines: [] clears them")
        tool("undo")
        listed = [m for m in tool("markers", kind="spawn")[1]["markers"] if m["id"] == elder_id]
        check(listed and len(listed[0].get("lines", [])) == 2, "undo brings the lines back")
        for marker in (elder, chief, pack):
            tool("unmark", id=marker["marker"]["id"])
        steps = tool("state")[1]["undo_steps"]
        err, msg, _ = tool("edit_marker", id=999999, name="x")
        err2, msg2, _ = tool("unmark", id=0)
        check(err and err2 and tool("state")[1]["undo_steps"] == steps, "bad ids are errors, and nothing changed")
        err, r, _ = tool("unmark", id=den_id)
        check(not err and tool("markers", kind="spawn")[1]["count"] == 4, "unmark")
        tool("undo")
        check(tool("markers", kind="spawn")[1]["count"] == 5, "undo brings the marker back")
        err, r, _ = tool("goto", position=[x, before + 20, z + 30], look_at=[x, before, z])
        check(not err and r["moved"], "goto")
        err, r, _ = tool("save")
        check(not err and os.path.getsize(opt.world) > 100_000, f"save writes {opt.world}")
        with open(opt.world, "rb") as f:
            data = f.read()
            check(b"INST" in data and b"MARK" in data, "the saved world holds the prefab instances and the markers")
        copy = os.path.join(os.path.dirname(opt.world), "e2e-copy.flyw")
        err, r, _ = tool("save", path=copy)
        check(not err and os.path.exists(copy) and os.path.getsize(copy) > 100_000, "save a copy next to the world file")
        if os.path.exists(copy):
            os.remove(copy)
    finally:
        bridge.stdin.close()
        bridge.wait(5)
        game.send_signal(signal.SIGTERM)
        try:
            code = game.wait(60)
        except subprocess.TimeoutExpired:
            game.kill()
            code = "hung"
        log = game.stdout.read()
        check(code == 0, f"game quits cleanly on SIGTERM (exit {code})")
        noisy = [line for line in log.splitlines() if re.search(r"error|sanitizer|fail", line, re.I)]
        check(not noisy, "no errors in the game's log" + "".join("\n     " + line for line in noisy[:10]))
        if failures:
            print(log)
        shutil.rmtree(scratch, ignore_errors=True)
    print(f"{len(failures)} failure(s)" if failures else "all e2e checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
