#!/usr/bin/env python3
"""MCP server (stdio) that lets Claude shape the flycoos world in the running game.

Dependency-free: speaks the JSON-RPC subset of MCP that tools need (initialize, ping, tools/list,
tools/call) and forwards each tool call as one command to the game's command server on
127.0.0.1 (newline-delimited JSON, see docs/ARCHITECTURE.md). Register it with the repo's .mcp.json.
"""
import base64
import json
import os
import socket
import sys
import tempfile

PROTOCOL = "2025-06-18"
KNOWN_PROTOCOLS = {"2025-06-18", "2025-03-26", "2024-11-05"}
PORT = int(os.environ.get("FLYCOOS_PORT", "47800"))
TIMEOUT = 60  # seconds for one command; big paths and maps take well under a second
IMAGES = os.path.realpath(os.path.join(tempfile.gettempdir(), "flycoos"))  # where the game writes its PNGs

WORLD = ("World: 1024 x 192 x 1024 voxels, y is up, x and z run 0..1023, sea level y=34 (water surface at 34). "
         "Heights are the y of a column's highest solid voxel (trees and rocks count as terrain).")
SOLID = ["Grass", "Dirt", "Stone", "Sand", "Snow", "Wood", "Leaves", "Rock"]
MATERIALS = SOLID + ["Water", "Air"]
EDIT_NOTE = (" One call is one undo step, shared with the user's in-game undo (Ctrl+Z). The edit flashes in the game."
             " A call over the work budget (~50 ms of game time: big brushes, long wide paths, spread-out ops) is refused;"
             " split it.")


def num(desc, lo=None, hi=None, integer=True):
    s = {"type": "integer" if integer else "number", "description": desc}
    if lo is not None:
        s["minimum"] = lo
    if hi is not None:
        s["maximum"] = hi
    return s


def xyz(desc):
    return {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3, "description": desc + " [x, y, z]"}


def obj(props, required=()):
    return {"type": "object", "properties": props, "required": list(required), "additionalProperties": False}


CENTER = {"x": num("Column x of the brush center", 0, 1023), "z": num("Column z of the brush center", 0, 1023),
          "radius": num("Brush radius in blocks (circle), default 6", 0, 32)}
STRENGTH = num("Most blocks moved per column (at the center; falls off toward the rim), default 4", 1, 64)
REGION = {"x0": num("Region corner x"), "z0": num("Region corner z"), "x1": num("Opposite corner x"),
          "z1": num("Opposite corner z")}
OPS = {
    "raise": ("Raise the ground in a circle: adds up to `strength` blocks on each column, most at the center, "
              "keeping the surface material on top (dirt under grass). Pushes ground up through water.",
              obj({**CENTER, "strength": STRENGTH}, ["x", "z"])),
    "lower": ("Lower the ground in a circle by up to `strength` blocks. Under water the removed cells stay water; "
              "dry land becomes a dry pit even below sea level (water is never created).",
              obj({**CENTER, "strength": STRENGTH}, ["x", "z"])),
    "smooth": ("Smooth the ground in a circle toward each column's 5x5 neighbourhood average.",
               obj({**CENTER, "strength": num("Most blocks moved per column, default 64", 1, 64)}, ["x", "z"])),
    "flatten": ("Flatten a circle of columns (a hard-edged disc) to ground height `height`.",
                obj({**CENTER, "height": num("Target ground top y (sea level is 34)", 0, 191),
                     "strength": num("Most blocks moved per column, default 64 (all the way)", 1, 64)},
                    ["x", "z", "height"])),
    "paint": ("Recolor the surface voxel of every column in a circle.",
              obj({**CENTER, "material": {"type": "string", "enum": SOLID}}, ["x", "z", "material"])),
    "fill": ("Set every voxel in a box (inclusive corners, at most 128^3 voxels) to one material; Air clears, "
             "Water makes water.",
             obj({"min": xyz("Corner"), "max": xyz("Opposite corner"), "material": {"type": "string", "enum": MATERIALS}},
                 ["min", "max", "material"])),
}

TOOLS = {
    "state": ("Game and world state: world file, size, seed, unsaved flag, undo/redo depth, player and camera "
              "position, editor mode, Follow Claude, time of day. Start here. " + WORLD, obj({})),
    **{k: (d + EDIT_NOTE, s) for k, (d, s) in OPS.items()},
    "apply": ("Apply a list of edits (up to 256) as one undo step. Each op is an object with `tool` (raise, lower, "
              "smooth, flatten, paint, fill) and that tool's arguments. All ops are checked first; if one is invalid "
              "nothing changes." + EDIT_NOTE,
              obj({"ops": {"type": "array", "minItems": 1, "maxItems": 256,
                           "items": {"type": "object", "required": ["tool"],
                                     "properties": {"tool": {"type": "string", "enum": list(OPS)}}}}}, ["ops"])),
    "path": ("Build a road or river bed along a polyline of [x, z] points: flattens discs of `width` along it to a "
             "height that follows the terrain (averaged along the path so it climbs gently) or to a fixed `height`, "
             "`depth` blocks lower (for a channel; water is not added — fill it with the fill tool), then optionally "
             "paints the surface." + EDIT_NOTE,
             obj({"points": {"type": "array", "minItems": 2,
                             "items": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2},
                             "description": "Polyline [[x, z], ...] inside the world"},
                  "width": num("Width in blocks, default 5", 1, 65),
                  "follow_terrain": {"type": "boolean", "description": "Default true; false flattens to `height`"},
                  "height": num("Ground y when follow_terrain is false", 0, 191),
                  "depth": num("Blocks below the graded height (river bed), default 0", 0, 64),
                  "material": {"type": "string", "enum": SOLID, "description": "Surface to paint, e.g. Dirt or Stone"}},
                 ["points"])),
    "undo": ("Undo the last edit step (the user's or Claude's: one shared history).", obj({})),
    "redo": ("Redo the last undone step.", obj({})),
    "save": ("Save the world to its .flyw file. With `path`, write a copy there instead (the world file and the "
             "unsaved flag stay as they are).", obj({"path": {"type": "string"}})),
    "screenshot": ("Render the world from any camera offscreen (the user's view is not disturbed; no HUD) and return "
                   "the PNG. Give `position` and either `look_at` or `yaw`/`pitch`.",
                   obj({"position": xyz("Camera position"), "look_at": xyz("Point to look at"),
                        "yaw": num("Degrees; 0 looks toward -z, 90 toward -x, 180 toward +z, -90 toward +x", -360, 360, False),
                        "pitch": num("Degrees, positive looks down, default 20", -89, 89, False),
                        "fov": num("Vertical field of view in degrees, default the game's (70)", 20, 120, False),
                        "width": num("Pixels, default 1280", 64, 1600), "height": num("Pixels, default 720", 64, 900),
                        "time": num("Time of day 0..1 (0 midnight, 0.25 sunrise, 0.5 noon, 0.75 sunset), default the game's",
                                    0, 1, False)}, ["position"])),
    "map": ("Top-down map of a region as a PNG: surface colors, hillshade (lit from the north-west, i.e. -x/-z), "
            "brighter = higher, water tinted darker by depth, grid lines labelled with x (top) and z (left) "
            "coordinates. Image x runs along +x, image down along +z. Returns min/max ground height. " + WORLD,
            obj({**REGION, "max_size": num("Longest image side in pixels, default 1024", 64, 1024),
                 "grid": num("Grid line spacing in blocks; default picks ~8 lines, 0 = none", 0, 1024)},
                ["x0", "z0", "x1", "z1"])),
    "heights": ("Ground heights of a region as numbers, downsampled to at most 4096 columns (`step` blocks apart; "
                "heights[row][col] is at x = x0 + col*step, z = z0 + row*step), plus a histogram of the top blocks "
                "(Water where a column is under water). " + WORLD, obj(REGION, ["x0", "z0", "x1", "z1"])),
    "inspect": ("Count the blocks of each kind in a box (inclusive corners, at most 128^3 voxels).",
                obj({"min": xyz("Corner"), "max": xyz("Opposite corner")}, ["min", "max"])),
    "goto": ("Fly the user's camera (editor mode) to `position`, facing `look_at`, to show them what you mean. "
             "Does nothing if the user turned off Follow Claude in the Editor panel.",
             obj({"position": xyz("Where the user's character flies to (the camera sits behind it)"),
                  "look_at": xyz("Point to face")}, ["position"])),
}

NOT_RUNNING = ("The flycoos game isn't running (nothing listens on 127.0.0.1:{port}). Ask the user to start it from "
               "the repo root, e.g. `./build/release/flycoos --world worlds/<name>.flyw`, then retry.")


def game(cmd, args):
    """Sends one command to the game; returns (ok, result or error message)."""
    try:
        with socket.create_connection(("127.0.0.1", PORT), timeout=5) as s:
            s.settimeout(TIMEOUT)
            s.sendall((json.dumps({"id": 1, "cmd": cmd, "args": args}) + "\n").encode())
            data = b""
            while not data.endswith(b"\n"):
                chunk = s.recv(1 << 16)
                if not chunk:
                    return False, "the game closed the connection"
                data += chunk
    except ConnectionRefusedError:
        return False, NOT_RUNNING.format(port=PORT)
    except TimeoutError:
        return False, f"the game didn't answer within {TIMEOUT} s (is it busy, minimized, or serving another client?)"
    except OSError as e:
        return False, f"cannot reach the game: {e}"
    reply = json.loads(data)
    return (True, reply["result"]) if reply.get("ok") else (False, reply.get("error", "unknown error"))


def call(name, args):
    if name not in TOOLS:
        return {"content": [{"type": "text", "text": f"unknown tool {name}"}], "isError": True}
    ok, result = game(name, args)
    if not ok:
        return {"content": [{"type": "text", "text": result}], "isError": True}
    content = []
    if name in ("screenshot", "map"):
        path = os.path.realpath(result["path"])
        if os.path.dirname(path) != IMAGES:
            return {"content": [{"type": "text", "text": f"refusing to read an image outside {IMAGES}"}], "isError": True}
        with open(path, "rb") as f:
            content.append({"type": "image", "data": base64.b64encode(f.read()).decode(), "mimeType": "image/png"})
    content.append({"type": "text", "text": json.dumps(result)})
    return {"content": content}


def handle(msg):
    method, params = msg.get("method"), msg.get("params") or {}
    if method == "initialize":
        asked = params.get("protocolVersion")
        return {"protocolVersion": asked if asked in KNOWN_PROTOCOLS else PROTOCOL,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "flycoos", "version": "0.1.0"},
                "instructions": "Shape the flycoos story world in the running game together with the user. " + WORLD +
                                " Look before and after editing (map, heights, screenshot); every edit is undoable."}
    if method == "ping":
        return {}
    if method == "tools/list":
        return {"tools": [{"name": n, "description": d, "inputSchema": s} for n, (d, s) in TOOLS.items()]}
    if method == "tools/call":
        return call(params.get("name"), params.get("arguments") or {})
    raise LookupError(method)


def main():
    for line in sys.stdin.buffer:  # bytes: a line that isn't UTF-8 is a parse error, not the end of the server
        if not line.strip():
            continue
        try:
            msg = json.loads(line)
        except (ValueError, RecursionError) as e:  # JSONDecodeError and UnicodeDecodeError are ValueErrors
            reply = {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": f"parse error: {e}"}}
        else:
            if isinstance(msg, dict) and "id" not in msg:  # a notification (notifications/initialized, cancelled): nothing to answer
                continue
            if not isinstance(msg, dict) or not isinstance(msg.get("method"), str):
                reply = {"jsonrpc": "2.0", "id": msg["id"] if isinstance(msg, dict) else None,
                         "error": {"code": -32600, "message": "invalid request"}}
            else:
                reply = {"jsonrpc": "2.0", "id": msg["id"]}
                try:
                    reply["result"] = handle(msg)
                except LookupError as e:
                    reply["error"] = {"code": -32601, "message": f"method not found: {e}"}
                except Exception as e:  # keep serving after a bug in one call
                    reply["error"] = {"code": -32603, "message": f"internal error: {e}"}
        sys.stdout.write(json.dumps(reply) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
