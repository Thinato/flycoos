# Combat roadmap

What the combat system has and what's still to build, to make it the best it can be. Soulslike
mechanics (genre conventions) are fair game; no other game's names, assets or characters. Each item
gets its own ADR when built; tick it here and point to the ADR. The research behind the melee
system is summarized in ADR-035.

## Done

- [x] Light chain, heavies with a charge, input buffer, cancel windows (ADR-035)
- [x] Running, rolling, jumping attacks; guard counter; riposte on a broken stance (ADR-035)
- [x] Stamina bar; guard with a sword or bare arms; guard break (ADR-035)
- [x] NPC stance and poise (flinch, stance break), player poise from armor, hyperarmor, roll i-frames (ADR-035)
- [x] NPC windup telegraphs with tracking, varied timing, a sword's-length spacing (ADR-035)
- [x] Directional roll, jump-roll, coyote time (f0ff042); lock-on (ADR-033); packs flank (ADR-034)
- [x] Shields in the off hand: small, medium and great, by negation and guard boost; shield pose (ADR-036)
- [x] Parry (Q / L2): a short window that breaks the attacker open to a riposte; greatshields can't;
      unparryable kinds; bosses that take several (ADR-036)
- [x] Greatshield bounce: weak attacks recoil off a strong guard (ADR-036)

## Defense

- [x] Shield weight (with the equip load, ADR-039) and parrying with a knife or bare fists (ADR-041)
- [x] Perfect guard: decided yes, a small bonus (no damage, half the stamina; ADR-041)
- [x] Backstep (roll button with no direction) and its attack (ADR-038)
- [x] Damage types (slash, strike, pierce) with per-shield negation and creature resistances (ADR-040);
      elemental types later, with magic
- [x] NPCs with shields that guard and parry (the guard, ADR-043)

## Melee weapon classes (each needs its own moveset, timings, stance damage and animation set)

- [x] Daggers / knives: fast, low stance, good for crits (ADR-044)
- [x] Straight swords (the baseline moveset, ADR-035)
- [ ] Greatswords / two-handed swords: slow, high stance, wide sweeps
- [ ] Curved swords / sabres: flowing slashes, long chains
- [ ] Thrusting swords / rapiers: pierce, counter-attack bonus
- [x] Axes: hard chops, high stance (ADR-044)
- [ ] Greataxes
- [x] Maces: strike damage, the most stance damage (ADR-044)
- [ ] Great hammers
- [ ] Spears / halberds: reach, thrusts, attacking from behind a shield
- [ ] Fists / claws: multi-hit chains (fists now reuse the sword's swings)
- [ ] Whips, flails, twinblades (later)
- [ ] Colossal weapons: very slow, huge stance, hyperarmor

## Ranged

- [ ] **Bows:** draw and hold, aim mode (over the shoulder), arrows as items, arc and drop
- [ ] **Crossbows:** load and fire, slower and stronger, no draw time
- [ ] Throwables (knives, pots, bombs)
- [ ] Projectiles in the sim: travel, hit detection, sticking into terrain, blocked by guards
- [ ] Lock-on aiming for ranged weapons

## Magic

- [ ] A **mana (FP) bar** and its regen and potions
- [ ] **Staff** (sorcery): cast, charge, projectiles and area spells
- [ ] **Wand:** a quick one-handed caster, lighter spells
- [ ] Holy/nature focus (a seal-like catalyst): buffs, heals, area effects
- [ ] Casting animations, casting in combat (movement while casting?), spell slots
- [ ] Weapon buffs (enchant a blade)

## Systems

- [ ] Two-handing (a toggle: more strength, its own moveset, no shield)
- [ ] Dual wielding / power stance
- [ ] Weapon skills (a special move per weapon or per skill gem, costing mana)
- [x] Backstabs and criticals (from behind an unaware or turned enemy; random crits as stats, ADR-045)
- [ ] Status effects: bleed, poison, frost, stun build-up bars
- [ ] Weapon scaling with attributes (strength, agility, intellect, spirit)
- [x] Equip load: light, medium and heavy rolls; overloaded can't roll (ADR-039)
- [ ] Healing flasks (limited charges, refilled at rest points)
- [x] Per-weapon timings and stance values in the data: a moveset per weapon class (ADR-044)
- [ ] Talent trees (Warrior, Ranger, Mage; planned in docs/TALENTS.md): last, once every other item on this roadmap is done. Until then, follow the foundations rules in that file

## Enemies

- [x] Combos and delayed attacks (varied timings within a string; ADR-042)
- [x] Enemies that guard, parry, dodge and back off (ADR-043)
- [ ] Ranged enemies keeping their distance (needs projectiles)
- [x] Attack tokens: only a few in a pack swing at once, the rest circle (ADR-042)
- [ ] Grabs, area attacks, charges, jumping attacks
- [ ] Bosses: phases, unique movesets, stance bars shown
- [x] Enemy weakness and resistance per damage type (ADR-040)

## Feel

- [ ] Hit sparks, blood, a slash trail on the blade
- [x] Sounds: swings, hits, blocks, parries, footsteps, voices, ambience (ADR-037)
- [x] Hit-stop tuned per weapon class (ADR-044)
- [x] Camera shake on heavy hits (and criticals, big hits taken; ADR-045)
- [ ] Knockback and launch on big hits; ragdoll-ish falls
- [ ] Charge glow; a riposte and parry flash
- [ ] Damage numbers by type; a crit marker (the marker done, ADR-045)
