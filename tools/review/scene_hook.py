#!/usr/bin/env python3
"""Patches a COPY of the source tree with scripted scenes for review films (never run it on the repo).

    python3 tools/review/scene_hook.py TREE      (tools/review/tree.sh runs it for you)

The hooked game only changes when it runs as a benchmark (--frames N) with FLY_SCENE=1 set. 30 frames
in, the scene is set up from FLY_* variables; from then on every second frame is written to FLY_SHOTS
as fNNNN.bmp (NNNN counts frames from the scene's start), and a box at the top left shows the scene
clock, each NPC's AI state and health, and yours.

Positions are blocks from the world's middle (the arena's centre), "x z" or "x y z".
  Setup
    FLY_PLAYER="x yaw z"      put the player there, facing yaw degrees
    FLY_WEAPON=id             give and wield an item          FLY_GEAR="id id"  give and equip each
    FLY_SPAWN="kind n x z;.." spawn n of a creature at a point (FLY_SEED: the spawn seed, default 7)
    FLY_NODUMMIES=1           remove the arena's own NPCs     FLY_KILL=1  kill every spawned NPC
    FLY_ANGRY=1               spawned NPCs start chasing      FLY_FACE="yaw"  turn them
    FLY_PHASE=n FLY_BOSSHP=share FLY_BOSSSTANCE=s   a boss's phase, health share and stance (ADR-059)
    FLY_FXAMOUNT=x            the effects' amount (ADR-058)
  Every frame
    FLY_GOD=1                 the player can't die            FLY_NOAI=1  hide the AI lines in the box
    FLY_CALM=1                NPCs never swing                FLY_NPCGUARD=1  NPCs guard, never swing
    FLY_NOBLEED=1             no status buildup               FLY_CRIT=1  every hit of yours crits
    FLY_GUARD=1               guard the whole time            FLY_GUARDS="from to;.."  guard in windows
    FLY_ATTACK="from to every" a light attack at the nearest foe every N frames
    FLY_HEAVY="start hold;.."  press heavy at start, hold it for hold frames
    FLY_AIMNEAR=1             aim at the nearest foe          FLY_AUTOPARRY=1  parry blows, riposte when broken
    FLY_PATH="x z;x z" FLY_GO=frame FLY_RUN=1   walk (or run) through waypoints from frame FLY_GO
    FLY_KILLNAME=name FLY_KILLAT=frame          kill the named NPC at a frame
  Camera and overlay
    FLY_EYE="x y z" FLY_AT="x y z"  a fixed camera (both needed); y is absolute height
    FLY_TXT="x y"             where the box goes               FLY_CORPSES=1  count corpses in the box
    FLY_FXSTAT=1              print particle counts each second (for perf runs)
    FLY_TAB=1                 open the debug window's Entities tab
Every run also prints "hitch frame N M ms" for frames over 100 ms.

Blocks that need a feature the tree doesn't have yet (effects, bosses, grabs) are left out, so a
"before" tree from a few commits back hooks too. A line that no longer matches fails loudly: fix the
anchor here, in the repo, not in a copy.
"""
import glob
import os
import sys

ROOT = sys.argv[1]
if os.path.realpath(ROOT) == os.path.realpath(os.path.join(os.path.dirname(__file__), '..', '..')):
    sys.exit('scene_hook.py patches a copy of the source, never the repo itself')
ALL = ''.join(open(f).read() for f in glob.glob(ROOT + '/src/*.h') + glob.glob(ROOT + '/src/*.cpp'))


def has(symbol):
    return symbol in ALL


def opt(symbol, code):
    return code if has(symbol) else ''


p = ROOT + '/src/main.cpp'
s = open(p).read()
if '// SCENE-HOOK' in s:
    sys.exit('already hooked: ' + p)


def rep(a, b):
    global s
    assert s.count(a) == 1, f'anchor not found once in main.cpp (fix tools/review/scene_hook.py): {a!r}'
    s = s.replace(a, b)


def walk_list(var, fmt, n, body):
    """C++ that splits the env var on ';' and runs body for each item read with sscanf(fmt) into n values."""
    return f'''if (const char* lst = std::getenv("{var}"); lst && sceneReady) {{
                    std::string all = lst;
                    for (size_t from = 0; from < all.size();) {{
                        size_t to = all.find(';', from);
                        int a = 0, b = 0;
                        if (std::sscanf(all.substr(from, to - from).c_str(), "{fmt}", &a, &b) == {n}) {{ {body} }}
                        if (to == std::string::npos) break;
                        from = to + 1;
                    }}
                }}'''


s = '#include <imgui.h>  // SCENE-HOOK\n' + s
rep('''    std::vector<double> frameMs;''', r'''    // SCENE-HOOK: scripted scenes (tools/review/scene_hook.py)
    auto env3 = [](const char* n, glm::vec3& v) { const char* e = std::getenv(n); return e && std::sscanf(e, "%f %f %f", &v.x, &v.y, &v.z) == 3; };
    const char* shotDir = std::getenv("FLY_SHOTS");
    bool sceneReady = false;
    int sceneStart = 0;
    auto sceneSetup = [&] {
        glm::vec2 mid(worldMiddle(world));
        glm::vec3 at;
        if (env3("FLY_PLAYER", at)) player.teleport(clearOfTerrain(world, {mid.x + at.x, float(groundTop(world, int(std::floor(mid.x + at.x)), int(std::floor(mid.y + at.z))) + 1), mid.y + at.z})), player.yaw = glm::radians(at.y);
        player.vel = {};
        if (const char* w = std::getenv("FLY_WEAPON")) giveItem(player.inv, defs().itemById.at(w)), quickMove(player.inv, {SlotRef::Item, 0, 0});
        if (const char* g = std::getenv("FLY_GEAR")) {  // "id id id": each given and equipped
            std::string all = g;
            for (size_t from = 0; from < all.size();) {
                size_t to = all.find(' ', from);
                ItemId id = defs().itemById.at(all.substr(from, to == std::string::npos ? std::string::npos : to - from));
                giveItem(player.inv, id);
                auto& bag = player.inv.containers[0];
                quickMove(player.inv, {SlotRef::Item, int(std::ranges::find(bag, id) - bag.begin()), 0});
                if (to == std::string::npos) break;
                from = to + 1;
            }
        }
        if (const char* sp = std::getenv("FLY_SPAWN")) {  // "kind count x z;kind count x z"
            std::string all = sp;
            for (size_t from = 0; from < all.size();) {
                size_t to = all.find(';', from);
                std::string one = all.substr(from, to == std::string::npos ? std::string::npos : to - from);
                char kind[32]; int n; float x, z;
                if (std::sscanf(one.c_str(), "%31s %d %f %f", kind, &n, &x, &z) == 4) {
                    SpawnSettings ss{.kind = EntityKind(std::ranges::find(defs().creatureIds, std::string(kind)) - defs().creatureIds.begin()), .count = n, .atPoint = true, .point = {x, z}};
                    spawnNpcs(session, ss, std::getenv("FLY_SEED") ? uint32_t(std::atoi(std::getenv("FLY_SEED"))) : 7u);
                }
                if (to == std::string::npos) break;
                from = to + 1;
            }
        }
        if (std::getenv("FLY_NODUMMIES")) std::erase_if(entities, [](const Entity& e) { return e.spawn.id != 0; });
        if (std::getenv("FLY_ANGRY")) for (Entity& e : entities) if (!e.spawn.id) setState(e, AiState::Chase, "forced");
        if (glm::vec3 f; env3("FLY_FACE", f)) for (Entity& e : entities) if (!e.spawn.id) e.body.yaw = glm::radians(f.x);
        @@BOSSSETUP@@
        @@FXSETUP@@
        if (std::getenv("FLY_KILL")) for (Entity& e : entities) if (!e.spawn.id) damage(e, int(std::ceil(e.body.health))), e.loot = NoItem;
    };

    std::vector<double> frameMs;''')
rep('''            if (benchmark) {''', r'''            if (benchmark && std::getenv("FLY_SCENE")) {  // SCENE-HOOK
                if (!sceneReady && frame > 30) sceneSetup(), sceneReady = true, sceneStart = frame;
                in = {};
                if (std::getenv("FLY_GUARD")) sim.guard = true;
                if (std::getenv("FLY_GOD")) player.health = float(maxHealth(sheet(player)));
                int sf = frame - sceneStart;
                @@FXSTAT@@
                if (std::getenv("FLY_CALM")) for (Entity& e : entities) e.body.swingTimer = 1;  // they don't swing
                if (const char* at = std::getenv("FLY_ATTACK"); at && sceneReady) {  // "from to every" (frames)
                    int a0 = 0, a1 = 0, every = 30;
                    std::sscanf(at, "%d %d %d", &a0, &a1, &every);
                    const Entity* best = nullptr;
                    for (const Entity& e : entities)
                        if (e.type().attackable && e.body.alive() && (!best || glm::distance(e.body.pos, player.pos) < glm::distance(best->body.pos, player.pos))) best = &e;
                    if (best) sim.aim = glm::normalize(glm::vec2(best->body.pos.x - player.pos.x, best->body.pos.z - player.pos.z) + 1e-6f);
                    if (sf >= a0 && sf <= a1 && (sf - a0) % every == 0) punchQueued = true;
                }
                @@HEAVY@@
                if (sceneReady && (std::getenv("FLY_AIMNEAR") || std::getenv("FLY_HEAVY") || std::getenv("FLY_AUTOPARRY"))) {
                    const Entity* best = nullptr;
                    for (const Entity& e : entities)
                        if (e.type().attackable && e.body.alive() && (!best || glm::distance(e.body.pos, player.pos) < glm::distance(best->body.pos, player.pos))) best = &e;
                    if (best) sim.aim = glm::normalize(glm::vec2(best->body.pos.x - player.pos.x, best->body.pos.z - player.pos.z) + 1e-6f);
                }
                if (std::getenv("FLY_AUTOPARRY") && sceneReady)  // a parry as a swing is about to land; a riposte at a broken stance
                    for (const Entity& e : entities) {
                        const Player& b = e.body;
                        float left = impactTime(b) - b.moveTime + (b.moveTime >= holdTime(b) - 1e-4f ? b.hold : b.hold + 99);
                        if (b.punching() && b.hitPending && left > 0.12f && left < 0.2f && !player.punching()) parryQueued = true;
                        if (b.broken && b.stagger > 0 && !player.punching()) punchQueued = true;
                    }
                @@GUARDS@@
                if (std::getenv("FLY_NPCGUARD")) for (Entity& e : entities) e.body.guarding = true, e.body.stamina = gMelee.staminaMax, e.body.swingTimer = 1;
                if (std::getenv("FLY_NOBLEED")) for (Entity& e : entities) e.body.buildup = {};
                if (std::getenv("FLY_CRIT") && sceneReady && sf == 1) player.innate.push_back({Stat::CritChance, ModKind::Flat, 1});
                if (const char* who = std::getenv("FLY_KILLNAME"); who && sceneReady && sf == std::atoi(std::getenv("FLY_KILLAT")))
                    for (Entity& e : entities) if (e.spawn.name == who) damage(e, 1000000);
                static std::vector<glm::vec2> pts;  // FLY_PATH "x z;x z": waypoints from the middle, from frame FLY_GO
                static size_t wp = 0;
                if (const char* pth = std::getenv("FLY_PATH"); pth && pts.empty()) {
                    std::string all = pth;
                    for (size_t from = 0; from < all.size();) {
                        size_t to = all.find(';', from);
                        float x, z;
                        if (std::sscanf(all.substr(from, to - from).c_str(), "%f %f", &x, &z) == 2) pts.push_back({x, z});
                        if (to == std::string::npos) break;
                        from = to + 1;
                    }
                }
                const char* go = std::getenv("FLY_GO");
                if (sceneReady && sf >= (go ? std::atoi(go) : 0)) {
                    glm::vec2 mid(worldMiddle(world)), at(player.pos.x, player.pos.z);
                    while (wp < pts.size() && glm::distance(at, mid + pts[wp]) < 0.8f) ++wp;
                    if (wp < pts.size()) in.move = glm::normalize(mid + pts[wp] - at), in.run = std::getenv("FLY_RUN") != nullptr;
                }
            } else if (benchmark) {''')
rep('''        viewEye = fp.eye, viewDir = glm::normalize(camTarget + camLook - fp.eye);''', r'''        if (glm::vec3 e, t; env3("FLY_EYE", e) && env3("FLY_AT", t)) {  // SCENE-HOOK: a fixed camera, from the middle
            glm::vec3 m(worldMiddle(world).x, 0, worldMiddle(world).y);
            fp.eye = m + e, fp.view = glm::lookAtRH(fp.eye, m + t, glm::vec3(0, 1, 0));
        }
        viewEye = fp.eye, viewDir = glm::normalize(camTarget + camLook - fp.eye);''')
rep('''        if (!renderer.render(fp, render, uiDraw, lastFrame ? opt.screenshot : nullptr)) return 1;''', '''        std::string shot = shotDir && sceneReady && (frame - sceneStart) % 2 == 0 ? std::format("{}/f{:04d}.bmp", shotDir, frame - sceneStart) : "";  // SCENE-HOOK
        if (!renderer.render(fp, render, uiDraw, !shot.empty() ? shot.c_str() : lastFrame ? opt.screenshot : nullptr)) return 1;''')
rep('''            drawNotice(notice.c_str(), float(double(SDL_GetTicksNS() - noticeAt) * 1e-9));''', r'''            drawNotice(notice.c_str(), float(double(SDL_GetTicksNS() - noticeAt) * 1e-9));
            if (std::getenv("FLY_SCENE") && sceneReady) {  // SCENE-HOOK: the scene clock and the AI's states
                std::string txt = std::format("{:4.1f} s", double(frame - sceneStart) / 60);
                if (!std::getenv("FLY_NOAI"))
                    for (const Entity& e : entities) {
                        if (e.kind == EntityKind::TrainingDummy && !std::getenv("FLY_DUMMYAI")) continue;
                        txt += std::format("\n{}: {} ({})  health {:.0f}{}", e.displayName(), kAiStateNames[int(e.state)],
                                           e.transitions ? e.history[(e.transitions - 1) % e.history.size()].reason : "", double(e.body.health),
                                           e.body.punching() ? std::format("  [{}]", kMoveNames[size_t(e.body.move)]) : std::string()) @@BOSSINFO@@;
                        if (!e.body.alive()) txt += std::format("  {:.0f} from you, {}", double(glm::distance(e.body.pos, player.pos)), canSee(world, player, e.body) ? "in sight" : "out of sight");
                    }
                txt += std::format("\nyou: {}", player.stagger > 0 ? "reeling" : player.rolling() ? "rolling" : player.guarding ? "guarding" : "free");
                @@HELDLINE@@
                @@FXLINE@@
                if (std::getenv("FLY_CORPSES")) {
                    int n = 0;
                    for (const Entity& e : entities) n += !e.body.alive();
                    txt += std::format("\ncorpses lying: {}", n);
                }
                float tx = 20, ty = 20;
                if (const char* tp = std::getenv("FLY_TXT")) std::sscanf(tp, "%f %f", &tx, &ty);
                ImVec2 size = ImGui::GetFont()->CalcTextSizeA(30, 1e9f, 0, txt.c_str());
                ImGui::GetForegroundDrawList()->AddRectFilled({tx - 8, ty - 6}, {tx + size.x + 8, ty + size.y + 6}, IM_COL32(0, 0, 0, 170), 6);
                ImGui::GetForegroundDrawList()->AddText(ImGui::GetFont(), 30, {tx, ty}, IM_COL32(255, 255, 255, 255), txt.c_str());
            }''')
rep('''        if (frame > 0) frameMs.push_back(realDt * 1000.0);''', '''        if (frame > 0) frameMs.push_back(realDt * 1000.0);
        if (frame > 0 && realDt > 0.1f) std::println("hitch frame {} {:.0f} ms", frame, double(realDt) * 1000);  // SCENE-HOOK''')

fx = os.path.exists(ROOT + '/src/fx.h')
s = s.replace('@@FXSETUP@@', 'if (const char* a = std::getenv("FLY_FXAMOUNT")) fx.set.amount = float(std::atof(a));' if fx else '')
s = s.replace('@@FXSTAT@@', 'if (std::getenv("FLY_FXSTAT") && sceneReady && sf % 60 == 0) std::println("fxstat {} particles {} smear {} ms {:.3f}", sf, fx.particles.size(), fx.ribbon.size() / 3, double(fx.ms));' if fx else '')
s = s.replace('@@FXLINE@@', 'if (std::getenv("FLY_FXN")) txt += std::format("\\nparticles {}, smear triangles {}", fx.particles.size(), fx.ribbon.size() / 3);' if fx else '')
s = s.replace('@@BOSSSETUP@@', opt('enterPhase(', r'''for (Entity& e : entities) if (e.type().boss || !typeOf(e.kind).phases.empty()) {
            if (const char* ph = std::getenv("FLY_PHASE")) enterPhase(e, std::atoi(ph));
            if (const char* hp = std::getenv("FLY_BOSSHP")) e.body.health = float(std::atof(hp)) * float(maxHealth(sheet(e.body)));
            if (const char* st = std::getenv("FLY_BOSSSTANCE")) e.body.stance = float(std::atof(st));
        }'''))
s = s.replace('@@BOSSINFO@@', opt('enterPhase(', r'''+ (typeOf(e.kind).boss ? std::format("  phase {}  stance {:.0f}/{:.0f}  {:.1f} from you", e.phase + 1, double(e.body.stance), double(e.type().stance), double(glm::distance(glm::vec2(e.body.pos.x, e.body.pos.z), glm::vec2(player.pos.x, player.pos.z)))) : std::string())'''))
s = s.replace('@@HELDLINE@@', opt('heldBy', 'if (player.heldBy) txt += "  (HELD by a grab)";'))
s = s.replace('@@HEAVY@@', opt('heavyQueued', walk_list('FLY_HEAVY', '%d %d', 2, 'if (sf == a) heavyQueued = true; if (sf >= a && sf < a + b) sim.heavyHeld = true;')))
s = s.replace('@@GUARDS@@', walk_list('FLY_GUARDS', '%d %d', 2, 'if (sf >= a && sf < b) sim.guard = true;'))
assert '@@' not in s, 'a placeholder was left in main.cpp'
open(p, 'w').write(s)

p = ROOT + '/src/debug_ui.cpp'
s = open(p).read()
rep('''        if (ImGui::BeginTabItem("Entities")) {''', '''        if (ImGui::BeginTabItem("Entities", nullptr, std::getenv("FLY_TAB") ? ImGuiTabItemFlags_SetSelected : 0)) { /* SCENE-HOOK */''')
open(p, 'w').write(s)
print('hooked', ROOT)
