#include "melee.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "defs.h"
#include "item.h"
#include "projectile.h"

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

glm::vec2 facing(const Body& b) { return {std::sin(b.yaw), std::cos(b.yaw)}; }

void spend(Combatant& c, float amount) {
    c.stamina = std::max(0.0f, c.stamina - amount);
    c.staminaWait = gMelee.staminaDelay;
}

void endMove(Combatant& c, Move last) {
    c.lastMove = last;
    c.sinceMove = 0;
    c.move = Move::None;
    c.hitPending = c.holding = false;
    c.hold = 0;
}

bool charges(Move m) { return m == Move::Heavy1 || m == Move::Heavy2; }
bool jumpAttack(Move m) { return m == Move::JumpLight || m == Move::JumpHeavy; }
// In the air for a jump attack (flying doesn't count: it would never land).
bool airborne(const Player& p) { return !p.onGround && !p.inWater && !p.tuning.fly && !p.tuning.noclip; }
bool wantsAttack(Want w) { return w == Want::Light || w == Want::Heavy || w == Want::Counter; }

// The attack `want` makes now, from what the body is doing (the first that holds): a guard counter
// just after a block (a bow: always its shot; a crossbow: its shot loaded, else its reload), a riposte at a stance-broken enemy, a jump attack in the air, a rolling attack
// late in a roll or just after, a running attack, else the next light of the chain (or heavy).
Move pick(const Player& p, Want want, bool riposte, bool running) {
    ItemId hand = p.inv.equipped[SlotMainHand];  // (what's in hand now, not the swing's)
    if (const WeaponClassDef& w = weaponClass(classOf(hand)); w.ranged)
        return w.loads && p.loaded != hand ? Move::Reload : Move::Shot;  // a bow only shoots (ADR-050), a crossbow loads first (ADR-052)
    if (want == Want::Counter && p.blockTime < gMelee.counterWindow) return Move::GuardCounter;
    bool heavy = want == Want::Heavy;  // a counter press that waited too long is a light
    if (riposte) return Move::Riposte;
    if (airborne(p)) return heavy ? Move::JumpHeavy : Move::JumpLight;
    bool rolledInto = p.move == Move::Rolling || (p.lastMove == Move::Rolling && p.sinceMove < 0.3f);  // one per roll
    bool rolledOut = !heavy && p.rollTime > 0.6f * p.rollLength && p.rollTime < p.rollLength + 0.3f;
    bool backstepInto = p.move == Move::Backstep || (p.lastMove == Move::Backstep && p.sinceMove < 0.3f);  // one per backstep
    if (rolledOut && p.backstep && !backstepInto) return Move::Backstep;
    if (rolledOut && !p.backstep && !rolledInto) return Move::Rolling;
    if (!heavy && running) return Move::Running;
    // Goes on from the move it chains from (the one still playing, or one that just ended).
    Move prev = p.punching() ? p.move : p.sinceMove < gMelee.chainGrace ? p.lastMove : Move::None;
    if (heavy) return prev == Move::Heavy1 ? Move::Heavy2 : Move::Heavy1;
    if (prev >= Move::Light1 && prev < Move::Light4) return Move(int(prev) + 1);
    return Move::Light1;
}

void turnTo(Body& b, glm::vec2 dir, float rate, float dt) {
    float diff = std::remainder(std::atan2(dir.x, dir.y) - b.yaw, 2 * kPi);
    b.yaw = std::remainder(b.yaw + diff * (1 - std::exp(-rate * dt)), 2 * kPi);
}

}  // namespace

const WeaponClassDef& weaponClass(WeaponClass c) { return defs().weaponClasses[size_t(c)]; }

void startMove(Player& b, Move m, glm::vec2 aim, float hold) {
    b.fromMove = b.punching() ? b.move : Move::None;
    b.fromU = moveProgress(b);
    b.fromClass = classOf(b.swingWeapon);
    b.swingWeapon = b.inv.equipped[SlotMainHand];
    b.moveScale = swingTime(sheet(b, skillOf(b.swingWeapon)), weaponAttack(b.swingWeapon)) * weaponClass(classOf(b.swingWeapon)).speed;
    b.move = m;
    b.moveTime = 0;
    b.hold = hold;
    b.holding = b.charged = false;
    b.charge = 0;
    b.freeze = 0;
    b.hitPending = true;
    b.punchRight = !b.punchRight;
    if (glm::length(aim) > 1e-4f) b.yaw = std::atan2(aim.x, aim.y);
}

void advanceMove(Combatant& c, float dt) {
    if (!c.punching()) {
        c.sinceMove = std::min(c.sinceMove + dt, 99.0f);
        return;
    }
    if (c.freeze > 0) {
        c.freeze = std::max(0.0f, c.freeze - dt);
        return;
    }
    float at = holdTime(c);
    if (c.hitPending && c.moveTime <= at && c.moveTime + dt >= at && (c.hold > 0 || c.holding)) {  // waits in the windup pose
        c.moveTime = at;
        c.hold = std::max(0.0f, c.hold - dt);
        return;
    }
    if (c.moveTime <= at && c.moveTime + dt > at) c.noise(Noise::Swing), c.swung = c.move;  // out of the windup: the blade moves
    c.moveTime += dt;
    if (c.moveTime >= moveLength(c)) endMove(c, c.move);
}

void interrupt(Combatant& c, float seconds, bool broken) {
    if (c.punching()) endMove(c, Move::None);  // and the chain with it
    c.hitPending = false;
    if (seconds >= c.stagger) c.stagger = c.staggerLength = seconds;
    c.broken |= broken;
    c.guarding = false;
}

bool updateMelee(Player& p, const MeleeInput& in, Input& move, float dt) {
    const MeleeTuning& t = gMelee;
    if (!p.alive()) {
        p.want = Want::None;
        p.pressing = p.guarding = p.holding = p.aiming = false;
        p.guardBlend = p.aimBlend = 0;
        return false;
    }
    if (p.rolling() && !p.rollPaid) {  // updatePlayer started the roll last tick
        spend(p, p.backstep ? t.backstepStamina : t.rollStamina);
        p.rollPaid = true;
        if (p.want == Want::Roll) p.want = Want::None;
    }
    if (!p.rolling()) p.rollPaid = false;
    p.blockTime = std::min(p.blockTime + dt, 99.0f);

    // Presses become wants: an attack press is a light once let go, a heavy once held tapSeconds,
    // either a guard counter right after a block. A newer press replaces an older one.
    auto wish = [&](Want w) { p.want = w, p.wantAge = 0, p.reloadWant = false; };
    bool counter = p.blockTime < t.counterWindow && !p.punching();
    bool bow = ranged(p);
    if (in.attack) p.pressing = true, p.pressTime = 0;
    if (p.pressing) {
        if (bow) p.pressing = false, p.chargeByHeavy = false, wish(Want::Light);  // a bow draws at once (ADR-050)
        else if (counter) p.pressing = false, wish(Want::Counter);
        else if (!in.attackHeld) p.pressing = false, wish(Want::Light);
        else if ((p.pressTime += dt) >= t.tapSeconds) p.pressing = false, p.chargeByHeavy = false, wish(Want::Heavy);
    }
    if (in.heavy) p.chargeByHeavy = true, wish(counter ? Want::Counter : Want::Heavy);
    if (in.parry && parryOf(p) > 0) wish(Want::Parry);  // with nothing that parries (a greatshield, a sword), Q does nothing
    if (in.drink) wish(Want::Drink);
    if (in.toss && !bow && p.readied && p.pouch[p.readied] > 0) wish(Want::Throw);  // with nothing to throw, or a bow in hand, nothing
    if (in.grip) wish(Want::Grip);
    if (!p.weapon()) {  // bare hands have nothing to two-hand: let go (the next weapon starts one-handed; a swap keeps the grip)
        p.twoHanded = false;
        if (p.want == Want::Grip) p.want = Want::None;
    } else if (weaponClass(classOf(p.weapon())).twoHanded && p.want == Want::Grip) {
        p.want = Want::None;  // a great weapon (ADR-049) stays in both hands: the press does nothing
    }
    ItemId hand = p.inv.equipped[SlotMainHand];
    const WeaponClassDef& held = weaponClass(classOf(hand));
    bool quiver = !held.ranged || p.arrows > 0 || (held.loads && p.loaded == hand);  // a bow needs an arrow (ADR-050), a crossbow one in it or to load (ADR-052)
    if (!quiver && wantsAttack(p.want)) p.want = Want::None;  // an empty quiver: the press does nothing (it doesn't wait for one)
    if (p.reloadWant && (p.want != Want::Light || !held.loads || p.loaded == hand)) {  // the automatic reload, with no crossbow to reload now
        if (p.want == Want::Light) p.want = Want::None;
        p.reloadWant = false;
    }
    bool reloads = held.loads && p.loaded != hand;  // (a reload costs no stamina: it goes on at 0)
    bool over = p.load == Load::Over && !p.tuning.fly && !p.tuning.noclip;  // overloaded: no roll, no run (ADR-039)
    if (move.roll && !over) wish(Want::Roll);
    if (over) move.run = false;
    move.roll = false;
    if (p.want != Want::None && (p.wantAge += dt) > t.bufferSeconds) p.want = Want::None;

    glm::vec2 face = facing(p);
    glm::vec2 aim = glm::length(in.aim) > 1e-3f ? glm::normalize(in.aim) : glm::length(move.move) > 0.1f ? glm::normalize(move.move) : face;
    bool running = move.run && p.onGround && glm::length(glm::vec2(p.vel.x, p.vel.z)) > p.tuning.walkSpeed + 1;
    bool started = false;
    if (p.stagger <= 0) {
        if (p.punching()) {
            float u = moveProgress(p);
            const MoveSpec& m = spec(p);
            if (charges(p.move) && !p.charged) {  // a heavy holds its windup while its button is held, charging
                bool held = p.chargeByHeavy ? in.heavyHeld : in.attackHeld;
                p.holding = held && p.moveTime <= holdTime(p);
                if (p.holding && p.moveTime >= holdTime(p) && (p.charge += dt) >= t.chargeSeconds) p.charged = true, p.holding = false, p.noise(Noise::Charged);
            }
            if (p.move == Move::Shot && p.hitPending && !weaponClass(classOf(p.swingWeapon)).loads) {  // draws while held; at full draw it holds on until let go (ADR-050)
                bool held = p.chargeByHeavy ? in.heavyHeld : in.attackHeld;
                p.holding = held && p.moveTime <= holdTime(p);
                if (p.holding && p.moveTime >= holdTime(p) && !p.charged && (p.charge += dt) >= gRanged.drawSeconds)
                    p.charged = true, p.noise(Noise::Charged);
            }
            if (jumpAttack(p.move)) p.holding = airborne(p);  // strikes on landing
            bool wants = p.want == Want::Roll || p.want == Want::Parry || p.want == Want::Drink || p.want == Want::Grip || p.want == Want::Throw;
            bool cut = wants || in.guard || (glm::length(move.move) > 0.1f && p.want == Want::None && !p.pressing);
            // A draw can be let down any time (the arrow kept), a crossbow's raise or its reload cut (ADR-052: the bolt not yet in, it stays empty).
            bool letDown = (p.move == Move::Shot || p.move == Move::Reload) && p.hitPending && wants;
            if (((u >= m.cancel && cut) || letDown) && !wantsAttack(p.want)) endMove(p, p.move);
        }
        // A wanted attack starts once the body is free, or from the current one's chain point.
        bool free = !p.punching() || moveProgress(p) >= spec(p).chain;
        bool rollDone = !p.rolling() || p.rollTime > 0.6f * p.rollLength;  // late in a roll: a rolling attack
        bool stunned = p.want == Want::Counter && p.blockTime < t.blockStun;
        if (wantsAttack(p.want) && free && rollDone && !stunned && (p.stamina > 0 || reloads) && !p.inWater) {
            Move next = pick(p, p.want, in.riposte, running);
            Move from = p.move;
            float fromU = moveProgress(p);
            WeaponClass fromClass = classOf(p.swingWeapon);
            if (p.punching()) endMove(p, p.move);
            if (p.rolling()) p.rollLength = p.rollTime;  // the attack takes over from the roll
            startMove(p, next, next == Move::Riposte || glm::length(in.aim) > 1e-3f ? in.aim : aim);
            p.fromMove = from, p.fromU = fromU, p.fromClass = fromClass;  // chained: it winds up from where that one was
            bool draws = next == Move::Shot && !loads(p);  // a bow's draw; a crossbow's shot doesn't hold (ADR-052)
            if (charges(next) || draws) p.holding = p.chargeByHeavy ? in.heavyHeld : in.attackHeld;
            if (draws || next == Move::Reload) p.noise(Noise::Windup);  // the draw's creak (ADR-050), the reload's crank (ADR-052)
            spend(p, spec(p).stamina);
            p.want = Want::None, p.reloadWant = false;
            started = true;
        }
        // A parry (a shield that can, else a knife or fists) starts once the body is free; it commits
        // through its recovery.
        if (p.want == Want::Parry && !p.punching() && !p.rolling() && !airborne(p) && !p.inWater && p.stamina > 0) {
            if (float length = parryOf(p); length > 0) {
                startMove(p, Move::Parry, glm::length(in.aim) > 1e-3f ? in.aim : aim);
                p.moveScale = length;
                p.hitPending = false;  // it strikes nothing
                spend(p, kParrySpec.stamina);
                started = true;
            }
            p.want = Want::None;
        }
        // A drink from the flask (ADR-047) starts once the body is free, on the ground; it commits too.
        if (p.want == Want::Drink && !p.punching() && !p.rolling() && !airborne(p) && !p.inWater) {
            startMove(p, Move::Drink, {});
            p.moveScale = 1;  // the heal is pending until its drink frame (hitPending)
            p.want = Want::None;
            started = true;
        }
        // A throw (ADR-053) starts once the body is free, on the ground, with stamina; with none of the
        // readied throwable left, or a bow or crossbow in hand, the press does nothing. It commits like an attack.
        if (p.want == Want::Throw && !p.punching() && !p.rolling() && !airborne(p) && !p.inWater && p.stamina > 0) {
            if (p.readied && p.pouch[p.readied] > 0 && !ranged(p)) {  // a bow or crossbow fills both hands
                startMove(p, Move::Throw, glm::length(in.aim) > 1e-3f ? in.aim : aim);
                p.moveScale = 1;  // weapon speed doesn't change it
                p.throwing = p.readied;
                spend(p, kThrowSpec.stamina);
                started = true;
            }
            p.want = Want::None;
        }
        // Two-handing (ADR-048) changes once the body is free; mid-roll it waits.
        if (p.want == Want::Grip && !p.punching() && !p.rolling()) {
            if (!weaponClass(classOf(p.weapon())).twoHanded)  // a great weapon swapped in mid-swing: nothing (ADR-049)
                p.twoHanded = !p.twoHanded, p.noise(Noise::Grip);  // (with no weapon the press was dropped above)
            p.want = Want::None;
        }
        if (!p.punching() && p.want == Want::Roll && p.stamina > 0) move.roll = true;  // until updatePlayer can roll
    }
    bool wasGuarding = p.guarding;
    p.guarding = in.guard && !bow && p.stagger <= 0 && !p.punching() && !p.rolling() && !move.roll && p.offGround < 0.1f && !p.inWater;
    // With a bow the guard button aims instead (ADR-050): walking slowly, drawing and loosing while it's down.
    p.aiming = in.guard && ranged(p) && p.stagger <= 0 && !p.rolling() && !move.roll && !p.inWater;
    if (p.guarding != wasGuarding) {  // a guard raised after a rest starts with a perfect guard's moment
        if (p.guarding) p.noise(Noise::GuardUp), p.perfectReady = p.guardTime >= t.perfectRearm;
        p.guardTime = 0;
    } else {
        p.guardTime = std::min(p.guardTime + dt, 99.0f);
    }
    p.guardBlend += ((p.guarding ? 1.0f : 0.0f) - p.guardBlend) * (1 - std::exp(-25 * dt));
    p.gripBlend += ((twoHanding(p) ? 1.0f : 0.0f) - p.gripBlend) * (1 - std::exp(-15 * dt));
    p.aimBlend += ((p.aiming ? 1.0f : 0.0f) - p.aimBlend) * (1 - std::exp(-15 * dt));

    // What the body does: nothing while reeling; an attack steps forward into its hit (in the air it
    // keeps the jump's way) and turns toward the aim through its windup; the guard walks.
    if (p.stagger > 0) {
        move = {};
    } else if (p.move == Move::Drink || p.move == Move::Throw) {  // walks slowly, may turn (a lock-on still faces its target)
        move.speed = t.drinkSpeed;
        move.run = move.jump = false;
        if (p.move == Move::Throw && p.hitPending) move.face = {}, turnTo(p, aim, 10, dt);  // a throw turns toward the aim until it lets go
    } else if (p.move == Move::Shot || p.move == Move::Reload || p.aiming) {  // drawing or aiming a bow, reloading a crossbow: walks slowly, facing the aim (ADR-050)
        move.speed = gRanged.aimWalk;
        move.run = false;
        move.jump &= p.move != Move::Shot && p.move != Move::Reload;
        move.face = aim;
    } else if (p.punching()) {
        const MoveSpec& m = spec(p);
        if (!jumpAttack(p.move) || !airborne(p)) {  // in the air a jump attack keeps the jump's way; landed, it commits
            float x = std::clamp(moveProgress(p) / m.impact, 0.0f, 1.0f);
            bool held = (p.holding || p.hold > 0) && p.moveTime >= holdTime(p);
            move.speed = held ? 0 : m.lunge * std::sin(kPi * x);
            move.move = move.speed > 0.05f ? face : glm::vec2(0);
            move.run = move.jump = false;
        }
        move.face = {};
        if (p.hitPending && p.moveTime <= holdTime(p)) turnTo(p, aim, 10, dt);
    } else if (p.guarding) {
        move.run = false;
    }
    bool runs = move.run && glm::length(move.move) > 0.1f && p.combatTimer > 0;  // out of combat running is free
    if (runs && p.stamina <= 0) move.run = runs = false;
    if (runs) p.stamina = std::max(0.0f, p.stamina - t.runDrain * dt);

    if (p.punching() || p.rolling() || runs) p.staminaWait = t.staminaDelay;  // reeling doesn't stop it: a broken guard refills
    else if (p.staminaWait > 0) p.staminaWait -= dt;
    else p.stamina += t.staminaRegen * (p.guarding ? t.guardRegen : 1) * (p.load >= Load::Heavy ? t.heavyRegen : 1) * dt;
    p.stamina = std::min(p.stamina, t.staminaMax);
    return started;
}

bool invulnerable(const Player& b) {
    const MeleeTuning& t = gMelee;
    float from = b.backstep ? t.backstepIframeFrom : t.iframeFrom;
    float to = b.backstep ? t.backstepIframeTo : b.rollLoad == Load::Heavy ? t.heavyIframeTo : t.iframeTo;
    return (b.rolling() && b.rollTime >= from && b.rollTime < to) || b.move == Move::Riposte;
}

bool hyperarmor(const Combatant& b) { return (spec(b).heavy && b.hitPending) || b.move == Move::Riposte; }

float playerPoise(const Combatant& b) { return sheet(b)[Stat::Armor] * gMelee.poisePerArmor; }

// Whether `from` is in front of `b`, within the guard's angle.
static bool inFront(const Player& b, glm::vec3 from) {
    glm::vec2 d(from.x - b.pos.x, from.z - b.pos.z);
    float dist = glm::length(d);
    return dist < 1e-3f || glm::dot(d / dist, facing(b)) >= gMelee.guardAngle;
}

bool guards(const Player& b, glm::vec3 from) { return b.guarding && inFront(b, from); }

const Shield* shieldOf(const Player& b) {
    const Shield& s = itemType(b.inv.equipped[SlotOffHand]).shield;
    return s.size != ShieldSize::None && !twoHanding(b) ? &s : nullptr;
}

float parryOf(const Player& b) {
    if (twoHanding(b)) return 0;
    if (const Shield* s = shieldOf(b)) return s->parry;
    if (b.inv.equipped[SlotOffHand] != NoItem) return 0;
    ItemId w = b.inv.equipped[SlotMainHand];
    return w ? itemType(w).parry : gMelee.fistParry;
}

Guard guardOf(const Player& b, DamageType type) {
    if (const Shield* s = shieldOf(b)) return {s->negation[size_t(type)], s->boost};
    if (twoHanding(b)) return {gMelee.twoHandNegation, gMelee.twoHandBoost};
    if (b.inv.equipped[SlotMainHand] != NoItem) return {gMelee.guardNegation, gMelee.guardBoost};
    return {gMelee.fistNegation, 0};
}

static float parryClock(const Player& b) { return b.moveTime / b.moveScale; }  // seconds into the parry at speed 1

bool parries(const Player& b, glm::vec3 from) {
    return b.move == Move::Parry && parryClock(b) >= gMelee.parryFrom && parryClock(b) < gMelee.parryTo && inFront(b, from);
}

std::optional<Guard> guardAgainst(const Player& b, glm::vec3 from, DamageType type) {
    if (guards(b, from)) return guardOf(b, type);
    if (b.move != Move::Parry || !inFront(b, from)) return std::nullopt;
    Guard g = guardOf(b, type);
    return Guard{g.negation * gMelee.parryGuard, g.boost * gMelee.parryGuard};
}

bool bounces(const Guard& g, float stance) { return g.boost >= gMelee.bounceBoost && stance < gMelee.bounceStance; }

bool perfectGuard(const Player& b) { return b.guarding && b.perfectReady && b.guardTime < gMelee.perfectGuardSeconds; }

DamageType damageOf(const Combatant& c, DamageType bare) {
    if (!c.swingWeapon) return bare;
    DamageType d = itemType(c.swingWeapon).damage;
    return d == DamageType::Slash && spec(c).thrust ? DamageType::Pierce : d;
}
