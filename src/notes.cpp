#include "notes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <fstream>

#include "defs.h"
#include "quest.h"

using nlohmann::json;

namespace {

double round2(double v) { return std::round(v * 100) / 100; }  // 2 decimals, without float noise in the JSON
json vec(glm::vec3 v) { return {round2(v.x), round2(v.y), round2(v.z)}; }

json itemId(ItemId id) { return id ? json(defs().itemIds[id]) : json(nullptr); }

// What the body is doing, in one word: the first that holds of knocked_out, flying, swimming,
// falling, jumping, on_ground.
const char* motion(const Player& p) {
    if (!p.alive()) return "knocked_out";
    if (p.tuning.fly || p.tuning.noclip) return "flying";
    if (p.inWater) return "swimming";
    if (p.onGround) return "on_ground";
    return p.vel.y < 0 ? "falling" : "jumping";
}

json combat(const Combatant& c) {
    return {{"health", round2(c.health)}, {"max_health", maxHealth(sheet(c))}, {"level", c.level},
            {"in_combat", c.combatTimer > 0}, {"combat_timer", round2(c.combatTimer)}, {"seconds_since_hit", round2(std::min(c.hitTime, 99.0f))},
            {"last_damage", c.lastDamage}, {"swinging", c.punching()}, {"weapon", itemId(c.weapon())}};
}

std::string localTime(const char* format) {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char out[64];
    return std::strftime(out, sizeof out, format, &tm) ? out : "";
}

}  // namespace

json noteContext(const Session& s, const GameView& view) {
    const Defs& d = defs();
    const Player& p = s.player;
    int minutes = int(s.timeOfDay * 24 * 60);
    const Marker* region = regionAt(s.markers, p.pos);

    json player = combat(p);
    player.update({{"pos", vec(p.pos)}, {"vel", vec(p.vel)}, {"yaw_degrees", std::round(glm::degrees(p.yaw))},
                   {"motion", motion(p)}, {"on_ground", p.onGround}, {"in_water", p.inWater}, {"fly", p.tuning.fly},
                   {"noclip", p.tuning.noclip}, {"seconds_knocked_out", round2(p.deadTime)}, {"xp", p.progress.xp},
                   {"block_at_feet", kBlockNames[s.world.get(glm::ivec3(glm::floor(p.pos)))]},
                   {"block_below", kBlockNames[s.world.get(glm::ivec3(glm::floor(p.pos - glm::vec3(0, 0.05f, 0))))]}});
    StatSheet st = sheet(p);
    for (size_t i = 0; i < size_t(Stat::Count); ++i) player["stats"][kStatIds[i]] = round2(st.v[i]);
    for (int slot = 0; slot < EquipCount; ++slot)
        if (p.inv.equipped[slot]) player["equipped"][kSlotNames[slot]] = itemId(p.inv.equipped[slot]);
    player["bags"] = json::array();
    for (const std::vector<ItemId>& bag : p.inv.containers)
        for (ItemId id : bag)
            if (id) player["bags"].push_back(itemId(id));

    std::vector<const Entity*> near;
    for (const Entity& e : s.entities)
        if (glm::distance(e.body.pos, p.pos) <= kNoteNearby) near.push_back(&e);
    std::ranges::sort(near, {}, [&](const Entity* e) { return glm::distance(e->body.pos, p.pos); });
    if (near.size() > kNoteNearbyMax) near.resize(kNoteNearbyMax);
    json npcs = json::array();
    for (const Entity* e : near) {
        json npc = combat(e->body);
        json history = json::array();
        for (uint32_t i = 0; i < std::min(e->transitions, 3u); ++i) {  // newest first
            const AiTransition& t = e->history[(e->transitions - 1 - i) % e->history.size()];
            history.push_back(std::format("{} -> {} ({}, after {:.1f} s)", kAiStateNames[int(t.from)], kAiStateNames[int(t.to)], t.reason, t.after));
        }
        npc.update({{"name", e->displayName()}, {"kind", d.creatureIds[size_t(e->kind)]}, {"distance", round2(glm::distance(e->body.pos, p.pos))},
                    {"pos", vec(e->body.pos)}, {"state", kAiStateNames[int(e->state)]}, {"seconds_in_state", round2(e->stateTime)},
                    {"spawn", e->spawn.id ? std::format("#{} {}", e->spawn.id, e->spawn.name) : ""}, {"ai_history", history}});
        npcs.push_back(npc);
    }

    json quests = json::array();
    for (size_t q = 0; q < s.quests.size() && q < d.quests.size(); ++q) {
        const QuestState& qs = s.quests[q];
        if (qs.stage == 0 && !qs.done && !qs.failed) continue;  // not started
        json quest = {{"id", d.quests[q].id}, {"stage", qs.stage}, {"done", qs.done}, {"failed", qs.failed}};
        for (size_t o = 0; o < qs.objectives.size(); ++o)
            if (qs.objectives[o] != ObjectiveState::Hidden) quest["objectives"][d.quests[q].objectives[o].id] = kObjectiveStateNames[int(qs.objectives[o])];
        quests.push_back(quest);
    }

    size_t skip = s.eventLog.size() - std::min(s.eventLog.size(), kNoteEvents);
    return {{"time", localTime("%F %T")},
            {"world", view.path},
            {"seed", s.seed},
            {"unsaved_edits", view.unsaved},
            {"editor", view.editing},
            {"sim_seconds", round2(s.elapsed)},
            {"time_of_day", std::format("{:02}:{:02}", minutes / 60, minutes % 60)},
            {"region", region ? std::format("#{} {}", region->id, region->name) : ""},
            {"player", player},
            {"camera", {{"eye", vec(view.eye)}, {"yaw_degrees", round2(view.yawDegrees)}, {"pitch_degrees", round2(view.pitchDegrees)}, {"fov_degrees", view.fovDegrees}}},
            {"nearby_npcs", npcs},
            {"quests", quests},
            {"flags", s.flags},
            {"recent_events", json(std::vector<std::string>(s.eventLog.begin() + long(skip), s.eventLog.end()))}};
}

int saveNote(const std::filesystem::path& dir, const std::string& text, json context, const std::vector<uint8_t>& rgb, int w,
             int h, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path file = dir / "notes.jsonl";
    int line = 1;
    if (std::ifstream in(file); in)
        for (std::string l; std::getline(in, l);) ++line;
    context["note"] = text;
    context["screenshot"] = "";
    if (!rgb.empty()) {
        std::filesystem::path png = dir / std::format("{}-{}.png", localTime("%Y%m%d-%H%M%S"), line);
        if (writePngFile(png.c_str(), w, h, rgb.data())) context["screenshot"] = std::filesystem::absolute(png, ec).string();
    }
    std::ofstream out(file, std::ios::app);
    out << context.dump(-1, ' ', false, json::error_handler_t::replace) << '\n';
    if (!out) {
        error = "cannot write " + file.string();
        return 0;
    }
    return line;
}
