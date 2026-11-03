#!/usr/bin/env python3
"""MCP server (stdio) that lets Claude shape the flycoos world in the running game.

Dependency-free: speaks the JSON-RPC subset of MCP that tools need (initialize, ping, tools/list,
tools/call) and forwards each tool call as one command to the game's command server on
127.0.0.1 (newline-delimited JSON, see docs/ARCHITECTURE.md). The NPC kinds and materials in the
tool schemas come from the game (its `catalog` command). Register it with the repo's .mcp.json.
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
# Schemas that list what the game defines (materials, NPC kinds): fill() sets them from the game's
# `catalog` command on every tools/list. Without it they say where the names come from.
NOT_LISTED = " (couldn't get the names from the game when this tool list was made; a wrong name's error lists them)"
UNFILLED = {"solid": "A solid block name, e.g. Grass", "materials": "A block name, e.g. Stone, Water or Air",
            "entity": "Spawn only: the NPC kind"}
SOLID = {"type": "string", "description": UNFILLED["solid"] + NOT_LISTED}
MATERIALS = {"type": "string", "description": UNFILLED["materials"] + NOT_LISTED}
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
PREFAB_FORMAT = (
    " Structure files are prefabs/<name>.txt (name: letters, digits, - or _), plain text: header lines "
    "`description: ...`, optional `foundation: <Block>` (fills down from the bottom layer's solid cells to the ground, "
    "so it sits on slopes), optional `y: N` (the bottom layer sits at ground + 1 + N; -1 sinks it one block), and legend "
    "lines `X = Block` (one character; the prefabs result lists the block names). Then the layers, bottom first, each starting with a line "
    "`layer`. Each line of a layer is one row of cells along +x; rows go along +z, so a layer reads like the top-down "
    "map (x right, z down). `.` = Air (clears the world's voxel), space = keep the world's voxel. A blank line right "
    "after `layer` is a row of keep cells; blank lines before the next `layer` are ignored. Lines starting with # are "
    "comments. At most 64 cells per axis. Convention: the front (door) is the last row, +z, at rotation 0. The "
    "footprint is centered on the placed column.")
MARKER_KINDS = ["player_spawn", "spawn", "poi", "region"]
MARKER_NOTE = (" Markers are authored story data: they stand at the center of column x, z on ground voxel y. Kinds: "
               "`player_spawn` (where the player starts and gets back up after being knocked out; at most one), `spawn` "
               "(`count` NPCs of kind `entity` that wander within `radius` of it and respawn there; editing its place, "
               "radius, entity or count respawns them, a new name or new lines don't), `poi` (a named place; its name shows to a player within `radius`), `region` (a named circle; "
               "walking into it shows its name, the smallest wins when they nest). One call is one undo step, shared "
               "with the user's undo.")
MARKER_FIELDS = {
    "name": {"type": "string", "description": "Up to 48 bytes of UTF-8, no control or invisible characters; required for poi and region"},
    "x": num("Column x", 0, 1023), "z": num("Column z", 0, 1023),
    "y": num("Ground voxel it stands on (default: the column's ground, under any tree)", -1, 191),
    "radius": num("Blocks, 0..512. Spawn: wander area (default the kind's, see entity); poi: label range (default 16); region: "
                  "the circle (default 32)", 0, 512, False),
    "entity": {"type": "string", "description": UNFILLED["entity"] + NOT_LISTED},
    "count": num("Spawn only: how many NPCs, default 1 (all spawn markers together make at most 256)", 1, 8),
    "lines": {"type": "array", "maxItems": 8, "items": {"type": "string", "minLength": 1, "maxLength": 160},
              "description": "Spawn only, not for the dummy: what its NPCs say, in order and round again. Talkers "
                             "(villager, guard) say the next line on each E instead of random phrases; anyone that fights "
                             "(guards too) shouts the next one when it turns on the player, at most every 8 s. Up to 8 lines of 1..160 bytes, checked like names. With "
                             "lines and count 1 the NPC is called by the marker's name (e.g. a named story character). "
                             "[] clears them"},
}
INSTANCE_NOTE = (" One call is one undo step (voxels and the instance list together), shared with the user's undo. The "
                 "change flashes in the game.")
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
              obj({**CENTER, "material": SOLID}, ["x", "z", "material"])),
    "fill": ("Set every voxel in a box (inclusive corners, at most 128^3 voxels) to one material; Air clears, "
             "Water makes water.",
             obj({"min": xyz("Corner"), "max": xyz("Opposite corner"), "material": MATERIALS},
                 ["min", "max", "material"])),
}

TOOLS = {
    "state": ("Game and world state: world file, size, seed, unsaved flag, undo/redo depth, player and camera "
              "position, editor mode, Follow Claude, time of day, prefab instance and marker counts. Start here. " + WORLD,
              obj({})),
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
                  "material": SOLID},
                 ["points"])),
    "prefabs": ("List the prefabs `place` can put into the world: generated ones (tree, pine, bush, boulder, spire) with "
                "their params and ranges, and structures from text files in prefabs/ with their size. Rescans the folder "
                "first, so a file you just wrote is usable right away; files that don't parse are listed in `errors` "
                "with their line number." + PREFAB_FORMAT, obj({})),
    "place": ("Place a prefab as an instance that can be moved or removed later. It stands on column `x`, `z`: on the "
              "ground voxel `y` (default: the column's highest solid voxel; trees count, so aim at open ground). "
              "Generated prefabs grow like the world's own trees and rocks (trunks reach down to the ground on slopes, "
              "canopies only fill air); structures overwrite their cells, `.` cells clear. Returns the instance: id, "
              "box, cells changed. Fails if it would change nothing." + INSTANCE_NOTE,
              obj({"prefab": {"type": "string", "description": "Name from the prefabs tool"},
                   "x": num("Column x of the footprint center", 0, 1023), "z": num("Column z", 0, 1023),
                   "y": num("Ground voxel it stands on (structures: bottom layer at y + 1)", 0, 191),
                   "rotation": {"type": "integer", "enum": [0, 90, 180, 270],
                                "description": "Degrees counterclockwise seen from above (90 turns +x toward -z, so a "
                                               "+z front faces +x); structures only, default 0"},
                   "seed": num("Variation of generated prefabs' ragged shape, default from the position", 0, 4294967295),
                   "params": {"type": "object", "description": "Generated prefabs: e.g. {\"trunk\": 12, \"radius\": 5}; "
                                                               "missing ones take their default"}},
                  ["prefab", "x", "z"])),
    "remove": ("Remove a placed instance: every voxel it wrote that still holds what it wrote gets back what was there "
               "before (anything sculpted over it since stays)." + INSTANCE_NOTE,
               obj({"id": num("Instance id (instances lists them)", 1)}, ["id"])),
    "move": ("Move an instance to another column (and optionally turn it), keeping its id, seed and params: a remove "
             "and a place in one undo step. Far moves can be over the work budget: then remove and place." + INSTANCE_NOTE,
             obj({"id": num("Instance id", 1), "x": num("New column x", 0, 1023), "z": num("New column z", 0, 1023),
                  "y": num("Ground voxel at the new spot (default: found after removing it)", 0, 191),
                  "rotation": {"type": "integer", "enum": [0, 90, 180, 270], "description": "Default: unchanged"}},
                 ["id", "x", "z"])),
    "instances": ("List placed prefab instances (id, prefab, position, rotation, seed, params, box, cells), all of them "
                  "or those whose box touches a region; at most 500 listed, `count` has the total.",
                  obj(REGION)),
    "markers": ("List the story markers (id, kind, name, x, y, z, radius; spawns also entity, count and any lines), all of them "
                "or those in a region (a region marker's circle counts), optionally of one kind; at most 500 listed, "
                "`count` has the total." + MARKER_NOTE,
                obj({**REGION, "kind": {"type": "string", "enum": MARKER_KINDS}})),
    "mark": ("Add a story marker; returns it with its id." + MARKER_NOTE,
             obj({"kind": {"type": "string", "enum": MARKER_KINDS}, **MARKER_FIELDS}, ["kind", "x", "z"])),
    "edit_marker": ("Change a marker's name, place (x, z, y; a new column without y stands on its ground), radius, or "
                    "a spawn's entity, count and lines. Fields you leave out stay; the kind can't change (remove it and mark "
                    "again)." + MARKER_NOTE,
                    obj({"id": num("Marker id (markers lists them)", 1), **MARKER_FIELDS}, ["id"])),
    "unmark": ("Remove a story marker (a spawn's NPCs go with it)." + MARKER_NOTE,
               obj({"id": num("Marker id", 1)}, ["id"])),
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
            "coordinates. Image x runs along +x, image down along +z. Story markers are dots labelled with their id "
            "(white player_spawn, red spawn, yellow poi, cyan region with its circle); `markers` in the result has each "
            "drawn one's id, kind, name and pixel. Returns min/max ground height. " + WORLD,
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


def game(cmd, args, timeout=TIMEOUT):
    """Sends one command to the game; returns (ok, result or error message, whether the game answered)."""
    try:
        with socket.create_connection(("127.0.0.1", PORT), timeout=min(5, timeout)) as s:
            s.settimeout(timeout)
            s.sendall((json.dumps({"id": 1, "cmd": cmd, "args": args}) + "\n").encode())
            data = b""
            while not data.endswith(b"\n"):
                chunk = s.recv(1 << 16)
                if not chunk:
                    return False, "the game closed the connection", False
                data += chunk
    except ConnectionRefusedError:
        return False, NOT_RUNNING.format(port=PORT), False
    except TimeoutError:
        return False, f"the game didn't answer within {timeout:g} s (is it busy, minimized, or serving another client?)", False
    except OSError as e:
        return False, f"cannot reach the game: {e}", False
    reply = json.loads(data)
    return (True, reply["result"], True) if reply.get("ok") else (False, reply.get("error", "unknown error"), True)


TEMPERS = {"harmless": "Friendly", "passive": "Fight back when hit", "aggressive": "Attack on sight"}
CATALOG_TIMEOUT = 2  # seconds: tools/list must not hang on a busy game
NO_CATALOG = False  # the game answered `catalog` with an error (an older build): stop asking


def fetch():
    """The schema values from the game's catalog: (solid, materials, kind ids, kind description), or None."""
    global NO_CATALOG
    if NO_CATALOG:
        return None
    try:
        ok, cat, answered = game("catalog", {}, CATALOG_TIMEOUT)
        if not ok:
            NO_CATALOG = answered
            return None
        groups = {}
        for e in cat["entities"]:
            traits = [f"Lv {e['level']}", f"{e['health']} HP"] + (["talks"] if e["talks"] else [])
            traits += [f"sees {e['aggro_range']:g}"] if e["aggro_range"] else []
            traits += [f"chases at {e['chase_speed']:g} b/s"] if e["chase_speed"] else []
            traits += [f"wields {e['weapon']}"] if "weapon" in e else []
            traits += [f"drops {e['loot']} {e['loot_chance']:.0%}"] if "loot" in e else []
            traits += [f"wanders {e['wander_radius']:g}"] if e["wander_radius"] else []
            groups.setdefault(e["temper"], []).append(f"{e['id']} ({', '.join(traits)})")
        order = sorted(groups, key=lambda t: list(TEMPERS).index(t) if t in TEMPERS else len(TEMPERS))
        kinds = ("Spawn only: the NPC kind (wanders: its default radius; the player walks 5 and runs 9 b/s). " +
                 ". ".join(f"{TEMPERS.get(t, t)}: {', '.join(groups[t])}" for t in order))
        return (list(cat["solid_materials"]), list(cat["materials"]), [e["id"] for e in cat["entities"]], kinds)
    except Exception:  # a malformed catalog: the game answered, but not usefully
        NO_CATALOG = True
        return None


def fill(values):
    """Sets the material and NPC kind schemas from fetch()'s values, or back to unlisted for None."""
    for schema, key, i, desc in ((SOLID, "solid", 0, "A solid block"),
                                 (MATERIALS, "materials", 1, "A block; Air clears, Water makes water"),
                                 (MARKER_FIELDS["entity"], "entity", 2, None)):
        if values is None:
            schema.pop("enum", None)
            schema["description"] = UNFILLED[key] + NOT_LISTED
        else:
            schema.update(enum=values[i], description=desc or values[3])


LISTED = None  # the catalog values the last tools/list showed (None: unfilled)
NOTICES = []  # notifications to send before the current reply


def call(name, args):
    if name not in TOOLS:
        return {"content": [{"type": "text", "text": f"unknown tool {name}"}], "isError": True}
    global LISTED
    ok, result, _ = game(name, args)
    if ok and LISTED is None and (values := fetch()):  # the game is up now: the client should list the tools again
        LISTED = values
        NOTICES.append({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
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
    global LISTED
    method, params = msg.get("method"), msg.get("params") or {}
    if method == "initialize":
        asked = params.get("protocolVersion")
        return {"protocolVersion": asked if asked in KNOWN_PROTOCOLS else PROTOCOL,
                "capabilities": {"tools": {"listChanged": True}},
                "serverInfo": {"name": "flycoos", "version": "0.1.0"},
                "instructions": "Shape the flycoos story world in the running game together with the user. " + WORLD +
                                " Look before and after editing (map, heights, screenshot); every edit is undoable. Houses, camps, trees "
                                "and rocks are prefabs: place, move and remove them as instances. Story places (player spawn, "
                                "NPC spawns, named points and regions) are markers."}
    if method == "ping":
        return {}
    if method == "tools/list":
        LISTED = fetch()
        fill(LISTED)
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
        sys.stdout.write("".join(json.dumps(m) + "\n" for m in NOTICES + [reply]))
        NOTICES.clear()
        sys.stdout.flush()


if __name__ == "__main__":
    main()
