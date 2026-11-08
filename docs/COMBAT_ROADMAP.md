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

- [ ] Shield weight (with equip load, below) and parrying with light weapons (daggers, fists: with
      their weapon classes)
- [ ] Perfect guard / just-block? (decide: a timed-block bonus or not)
- [ ] Backstep (roll button with no direction) and its retreat attack
- [ ] Damage types (slash, strike, pierce, and later elemental) with per-shield negation

## Melee weapon classes (each needs its own moveset, timings, stance damage and animation set)

- [ ] Daggers / knives: fast, low stance, good for crits (now they reuse the sword's moveset)
- [x] Straight swords (the baseline moveset, ADR-035)
- [ ] Greatswords / two-handed swords: slow, high stance, wide sweeps
- [ ] Curved swords / sabres: flowing slashes, long chains
- [ ] Thrusting swords / rapiers: pierce, counter-attack bonus
- [ ] Axes: hard chops, high stance; greataxes
- [ ] Hammers / maces: strike damage, the most stance damage; great hammers
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
- [ ] Backstabs and criticals (from behind an unaware or turned enemy)
- [ ] Status effects: bleed, poison, frost, stun build-up bars
- [ ] Weapon scaling with attributes (strength, agility, intellect, spirit)
- [ ] Equip load: light, medium and heavy rolls
- [ ] Healing flasks (limited charges, refilled at rest points)
- [ ] Per-weapon timings and stance values in the data (items.json), not one shared table

## Enemies

- [ ] Combos and delayed attacks (varied timings within a string)
- [ ] Enemies that guard, parry, dodge and back off
- [ ] Ranged enemies keeping their distance (needs projectiles)
- [ ] Attack tokens: only a few in a pack swing at once, the rest circle (ADR-034 follow-up)
- [ ] Grabs, area attacks, charges, jumping attacks
- [ ] Bosses: phases, unique movesets, stance bars shown
- [ ] Enemy weakness and resistance per damage type

## Feel

- [ ] Hit sparks, blood, a slash trail on the blade
- [ ] Sounds: swings, hits, blocks, parries, footsteps
- [ ] Camera shake on heavy hits; hit-stop tuned per weapon class
- [ ] Knockback and launch on big hits; ragdoll-ish falls
- [ ] Charge glow; a riposte and parry flash
- [ ] Damage numbers by type; a crit marker
