#pragma once
#include <cmath>
#include <random>

// The five attributes. Every other stat derives from them (WoW-style, but classless, so there's
// no attack power or other in-between step).
struct Stats {
    int stamina = 10;   // max health
    int strength = 2;   // physical damage
    int agility = 2;    // physical attack speed
    int intellect = 2;  // unused until magic
    int spirit = 2;     // health regeneration out of combat
};

constexpr int kHealthPerStamina = 10;
constexpr float kHastePerAgility = 0.01f;  // +1% attack speed per point
constexpr float kRegenPerSpirit = 0.5f;    // health per second, out of combat only
constexpr float kCombatSeconds = 5;        // dealing or taking damage keeps you in combat this long

// A physical attack: damage range, and base swing time in seconds (WoW's "weapon speed").
struct Attack { int minDamage, maxDamage; float speed; };
constexpr Attack kPunch{6, 10, 1.0f};

constexpr int maxHealth(const Stats& s) { return s.stamina * kHealthPerStamina; }
inline float healthRegen(const Stats& s) { return float(s.spirit) * kRegenPerSpirit; }
// Seconds between swings: agility is haste.
inline float swingTime(const Stats& s, const Attack& a) { return a.speed / (1 + float(s.agility) * kHastePerAgility); }
// damage = baseDamage + strength * speed. Speed is the attack's base speed, not the hasted swing
// time (as in WoW): slower attacks hit harder per swing, and agility adds damage per second only.
inline int strengthBonus(const Stats& s, const Attack& a) { return int(std::lround(float(s.strength) * a.speed)); }
inline int rollDamage(const Stats& s, const Attack& a, std::mt19937& rng) {
    return std::uniform_int_distribution<int>(a.minDamage, a.maxDamage)(rng) + strengthBonus(s, a);
}
