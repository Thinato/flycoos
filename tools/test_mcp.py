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

    def rpc(method, params=None):
        msg = {"jsonrpc": "2.0", "id": next(next_id), "method": method, "params": params or {}}
        bridge.stdin.write(json.dumps(msg) + "\n")
        bridge.stdin.flush()
        reply = json.loads(bridge.stdout.readline())
        assert reply["id"] == msg["id"], reply
        return reply["result"]

    def tool(name, **args):
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
        names = {t["name"] for t in rpc("tools/list")["tools"]}
        check({"state", "raise", "path", "apply", "undo", "save", "screenshot", "map", "heights", "inspect", "goto"} <= names,
              f"tools/list ({len(names)} tools)")

        for _ in range(3000):  # the game generates its world first (~1 s release, ~1-2 min with sanitizers)
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
                break
            except OSError:
                time.sleep(0.1)
        err, state, _ = tool("state")
        check(not err and state["size"] == [1024, 192, 1024] and state["undo_steps"] == 0, "state")
        if err:
            raise RuntimeError(state)
        x, z = int(state["player"][0]) + 20, int(state["player"][2]) + 20

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
        err, r, _ = tool("goto", position=[x, before + 20, z + 30], look_at=[x, before, z])
        check(not err and r["moved"], "goto")
        err, r, _ = tool("save")
        check(not err and os.path.getsize(opt.world) > 100_000, f"save writes {opt.world}")
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
