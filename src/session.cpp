#include "session.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "defs.h"
#include "melee.h"

void startSession(Session& s) {
    s.player = {};
    s.entities.clear();
    s.elapsed = 0;
    s.regionId = 0;
    s.regionSince = 0;
    s.player.teleport(playerSpawn(s.world, s.markers));
    giveItem(s.player.inv, Shortsword);
    giveItem(s.player.inv, Satchel);
    syncSpawns(s.entities, s.world, s.markers, s.seed, s.player.pos, s.timeOfDay);
    s.rng.seed(s.seed);
    resetQuests(s);
}

void respawnPlayer(Session& s) {
    Player& p = s.player;
    p.teleport(playerSpawn(s.world, s.markers));
    p.vel = {};
    p.health = float(maxHealth(sheet(p)));
    p.deadTime = 0;
    p.combatTimer = 0;
}

StepResult step(Session& s, const SimInput& in, float dt) {
    StepResult r;
    Player& p = s.player;
    s.elapsed += dt;
    p.prevPos = p.pos, p.prevYaw = p.yaw;  // drawn from here to where this tick leaves them
    for (Entity& e : s.entities) e.body.prevPos = e.body.pos, e.body.prevYaw = e.body.yaw;
    if (p.deadTime >= kPlayerRespawnSeconds) {
        respawnPlayer(s);
        r.respawned = true;
    }
    if (in.interact && p.alive()) {
        if (Entity* corpse = lootTarget(s.entities, p)) takeLoot(*corpse, p.inv);
        else if (Entity* npc = talkTarget(s.entities, p)) {
            talk(*npc, pickLine(s, *npc));  // answered from the state the player walked up in; the quests react below
            if (npc->spawn.id) s.events.push_back(TalkedTo{npc->spawn.id});
        }
    }
    Input move = !p.alive() && !in.editing ? Input{} : in.move;
    if (in.editing) p.pressing = false, p.want = Want::None;  // nothing half-pressed swings on leaving the editor
    else {  // attacks, the guard and stamina (ADR-035); they shape this tick's movement
        MeleeInput melee{in.attack, in.attackHeld, in.heavy, in.heavyHeld, in.guard, in.aim};
        if (const Entity* e = riposteTarget(s.entities, p)) melee.riposte = true, melee.aim = {e->body.pos.x - p.pos.x, e->body.pos.z - p.pos.z};
        if (updateMelee(p, melee, move, dt) || (in.attack && !p.alive())) r.attacked = true;
    }
    if (!s.loaded || s.loaded(p.pos)) updatePlayer(p, s.world, move, dt);
    if (!in.editing) {
        updateEntities(s.entities, s.world, p, dt, s.loaded);
        separate(p, s.world, s.entities);
        int level = p.level;
        auto skills = p.progress.skills;
        updateCombat(p, s.entities, dt, s.rng, s.armorK, &s.events);
        for (size_t k = 0; k < skills.size(); ++k)  // progression news (ADR-027) for the HUD, like quest news
            if (int now = p.progress.skills[k].level; now > skills[k].level && s.toasts.size() < 8)
                s.toasts.emplace_back(-1, std::format("{} {}", skillDef(Skill(k)).name, now));
        if (int points = talentPoints(p.progress, p.level); p.level > level && s.toasts.size() < 8)
            s.toasts.emplace_back(-1, points == 1 ? std::format("Level {}! A talent point to spend (K)", p.level)
                                                  : std::format("Level {}! {} talent points to spend (K)", p.level, points));
    }
    s.timeOfDay = std::fmod(s.timeOfDay + dt / kDayLength * s.timeScale * in.timeRate, 1.0f);
    if (std::ranges::any_of(s.markers, [](const Marker& m) { return m.activeFrom != m.activeTo; }))  // timed spawns come and go
        syncSpawns(s.entities, s.world, s.markers, s.seed, p.pos, s.timeOfDay);
    const Marker* region = regionAt(s.markers, p.pos);  // its name shows for a few seconds on entering
    if (uint32_t id = region ? region->id : 0; id != s.regionId) {
        s.regionId = id, s.regionSince = s.elapsed;
        if (id) s.events.push_back(RegionEntered{id});
    }
    // Item counts: derived here rather than pushed by every inventory change (loot, quests, the UIs, a load).
    std::vector<int> counts(defs().items.size());
    for (size_t id = 1; id < counts.size(); ++id) counts[id] = countItem(p.inv, ItemId(id));
    for (size_t id = 1; id < counts.size(); ++id)
        if (counts[id] != (id < s.itemCounts.size() ? s.itemCounts[id] : 0)) s.events.push_back(ItemCountChanged{ItemId(id), counts[id]});
    s.itemCounts = std::move(counts);
    updateQuests(s);
    return r;
}
