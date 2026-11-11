#include "session.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

#include "defs.h"
#include "melee.h"

void startSession(Session& s) {
    s.player = {};
    s.entities.clear();
    s.elapsed = 0;
    s.regionId = 0;
    s.regionSince = 0;
    s.restPoint.reset();
    s.projectiles.clear(), s.bursts.clear();
    s.player.teleport(playerSpawn(s.world, s.markers));
    giveItem(s.player.inv, Shortsword);
    giveItem(s.player.inv, Satchel);
    s.player.flasks = maxFlasks(sheet(s.player));
    fillPouch(s.player);  // a full pouch of each throwable (ADR-053)
    syncSpawns(s.entities, s.world, s.markers, s.seed, s.player.pos, s.timeOfDay);
    s.rng.seed(s.seed);
    resetQuests(s);
}

void respawnPlayer(Session& s) {
    Player& p = s.player;
    p.teleport(s.restPoint ? clearOfTerrain(s.world, *s.restPoint) : playerSpawn(s.world, s.markers));
    p.vel = {};
    p.health = float(maxHealth(sheet(p)));
    p.flasks = maxFlasks(sheet(p));
    p.arrows = std::max(p.arrows, kQuiver);
    fillPouch(p);
    p.deadTime = 0;
    p.combatTimer = 0;
}

bool canRest(const Session& s) {
    const Player& p = s.player;
    // Hit lately is what counts, not the combat timer: poison's ticks keep that running, and a rest cures them.
    if (!p.alive() || p.hitTime < kCombatSeconds || p.punching() || p.rolling() || p.stagger > 0) return false;
    for (const Entity& e : s.entities)
        if (e.angry() && e.body.alive() && glm::distance(e.body.pos, p.pos) < 24) return false;
    glm::ivec3 at(glm::floor(p.pos));
    int r = int(kRestReach);
    for (int dy = -2; dy <= 1; ++dy)
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx)
                if (s.world.get(at + glm::ivec3(dx, dy, dz)) == Ember) return true;
    return false;
}

// The noises `b` made this tick into s.sounds, then clears them. A foot comes down where the gait
// passes a quarter turn (the walk's feet in model.cpp land there; quadrupeds step 1.5 times as often).
static void hear(Session& s, Player& b, int kind) {
    Block ground = s.world.get(glm::ivec3(glm::floor(b.pos - glm::vec3(0, 0.05f, 0))));
    if (b.inWater && b.onGround) ground = Water;  // wading
    Model model = kind < 0 ? ModelPlayer : typeOf(EntityKind(kind)).model;
    float rate = model == ModelBoar || model == ModelWolf ? 1.5f : 1, pi = std::numbers::pi_v<float>;
    float from = b.prevGait * rate / pi - 0.5f, to = from + std::remainder(b.gaitPhase - b.prevGait, 2 * pi) * rate / pi;
    if (std::floor(from) != std::floor(to) && b.alive() && !b.rolling() && b.gaitAmount > 0.15f && (b.onGround || b.inWater) &&
        !b.made(Noise::Land))  // a landing has its own footfall
        b.noise(b.onGround ? Noise::Step : Noise::Swim);
    for (int n = 0; n < int(Noise::Count) && b.noises; ++n) {
        if (!(b.noises >> n & 1) || s.sounds.size() >= kMaxSounds) continue;
        Sound h{Noise(n), b.pos, kind, ground, b.move, b.swingWeapon, 0};
        switch (Noise(n)) {
            case Noise::Step: h.amount = b.gaitAmount; break;
            case Noise::Swing:  // a shield's parry swings the shield
                h.move = b.swung;
                if (h.move == Move::Parry && shieldOf(b)) h.with = b.inv.equipped[SlotOffHand];
                break;
            case Noise::Roll: case Noise::Backstep: h.amount = float(b.rollLoad); break;
            case Noise::Land: h.amount = b.landSpeed; break;
            case Noise::Hurt: case Noise::Death: case Noise::Crit: h.with = b.hitWith, h.amount = b.hitStance; break;
            case Noise::Block: case Noise::Bounce: case Noise::Parry: case Noise::GuardBreak: case Noise::PerfectGuard:
                h.with = shieldOf(b) ? b.inv.equipped[SlotOffHand] : b.inv.equipped[SlotMainHand];
                break;
            default: break;
        }
        s.sounds.push_back(h);
    }
    b.noises = 0;
}

StepResult step(Session& s, const SimInput& in, float dt) {
    StepResult r;
    Player& p = s.player;
    s.elapsed += dt;
    p.prevPos = p.pos, p.prevYaw = p.yaw, p.prevGait = p.gaitPhase;  // drawn from here to where this tick leaves them
    for (Entity& e : s.entities) e.body.prevPos = e.body.pos, e.body.prevYaw = e.body.yaw, e.body.prevGait = e.body.gaitPhase;
    if (p.deadTime >= kPlayerRespawnSeconds) {
        respawnPlayer(s);
        r.respawned = true;
    }
    if (in.interact && p.alive()) {
        if (Entity* corpse = lootTarget(s.entities, p)) {
            if (takeLoot(*corpse, p.inv)) p.noise(Noise::Loot);
        } else if (Entity* npc = talkTarget(s.entities, p)) {
            p.noise(Noise::Talk);
            talk(*npc, pickLine(s, *npc));  // answered from the state the player walked up in; the quests react below
            if (npc->spawn.id) s.events.push_back(TalkedTo{npc->spawn.id});
        } else if (canRest(s)) {  // a rest (ADR-047): where it gets back up from now on
            rest(p);
            s.restPoint = p.pos;
            r.rested = true;
        }
    }
    Input move = !p.alive() && !in.editing ? Input{} : in.move;
    if (in.editing) p.pressing = false, p.want = Want::None;  // nothing half-pressed swings on leaving the editor
    else {  // attacks, the guard and stamina (ADR-035); they shape this tick's movement
        MeleeInput melee{in.attack, in.attackHeld, in.heavy, in.heavyHeld, in.guard, in.parry, in.drink, in.grip, in.aim};
        melee.toss = in.toss;
        // At an NPC open to a critical (ADR-045) an attack is the riposte move, aimed at it; once under way,
        // still aimed at the one it caught.
        if (const Entity* e = ranged(p) ? nullptr : criticalTarget(s.entities, p, p.move == Move::Riposte && p.hitPending))
            melee.riposte = true, melee.aim = {e->body.pos.x - p.pos.x, e->body.pos.z - p.pos.z};
        if (updateMelee(p, melee, move, dt) || (in.attack && !p.alive())) r.attacked = true;
        holdCritical(s.entities, p);  // before the NPCs think: the one it backstabs can't turn away
    }
    if (!s.loaded || s.loaded(p.pos)) updatePlayer(p, s.world, move, dt);
    if (!in.editing) {
        updateEntities(s.entities, s.world, p, dt, s.loaded);
        separate(p, s.world, s.entities);
        int level = p.level;
        auto skills = p.progress.skills;
        updateCombat(p, s.entities, dt, s.rng, s.armorK, &s.events);
        if (p.loosed) p.loosed = false, loose(s, in.aimAt);  // a bow's release (ADR-050)
        if (p.hurled) p.hurled = false, hurl(s, in.aimAt);  // a throw's (ADR-053)
        for (Entity& e : s.entities)  // an archer's (ADR-051)
            if (e.body.loosed) e.body.loosed = false, loose(s, e);
        updateProjectiles(s, dt);
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
    hear(s, p, -1);
    for (Entity& e : s.entities)
        if (!s.loaded || s.loaded(e.body.pos)) hear(s, e.body, int(e.kind));
        else e.body.noises = 0;  // frozen out there: nothing to hear
    return r;
}
