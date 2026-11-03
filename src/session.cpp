#include "session.h"

#include <cmath>

void startSession(Session& s) {
    s.player = {};
    s.entities.clear();
    s.elapsed = 0;
    s.regionId = 0;
    s.regionSince = 0;
    s.player.pos = playerSpawn(s.world, s.markers);
    giveItem(s.player.inv, Shortsword);
    giveItem(s.player.inv, Satchel);
    syncSpawns(s.entities, s.world, s.markers, s.seed, s.player.pos);
    s.rng.seed(s.seed);
}

void respawnPlayer(Session& s) {
    Player& p = s.player;
    p.pos = playerSpawn(s.world, s.markers);
    p.vel = {};
    p.health = float(maxHealth(p.stats));
    p.deadTime = 0;
    p.combatTimer = 0;
}

StepResult step(Session& s, const SimInput& in, float dt) {
    StepResult r;
    Player& p = s.player;
    s.elapsed += dt;
    if (p.deadTime >= kPlayerRespawnSeconds) {
        respawnPlayer(s);
        r.respawned = true;
    }
    if (in.interact && p.alive()) {
        if (Entity* corpse = lootTarget(s.entities, p)) takeLoot(*corpse, p.inv);
        else if (Entity* npc = talkTarget(s.entities, p)) talk(*npc);
    }
    if (!in.editing && in.attack && (startPunch(p, in.aim) || !p.alive())) r.attacked = true;
    updatePlayer(p, s.world, !p.alive() && !in.editing ? Input{} : in.move, dt);
    if (!in.editing) {
        updateEntities(s.entities, s.world, p, dt);
        separate(p, s.world, s.entities);
        updateCombat(p, s.entities, dt, s.rng, s.armorK);
    }
    s.timeOfDay = std::fmod(s.timeOfDay + dt / kDayLength * s.timeScale * in.timeRate, 1.0f);
    const Marker* region = regionAt(s.markers, p.pos);  // its name shows for a few seconds on entering
    if (uint32_t id = region ? region->id : 0; id != s.regionId) s.regionId = id, s.regionSince = s.elapsed;
    return r;
}
