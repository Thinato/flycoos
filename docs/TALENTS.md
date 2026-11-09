# Talents (plan)

Status: **planned, not built**. The talent trees come after every combat feature works, which means the whole of docs/COMBAT_ROADMAP.md, not only ranged attacks, magic, critical hits and poisons. Until then, new combat features follow the foundations below, so the trees can plug in later without a rewrite.

## Foundations: rules for combat work until then

1. **What a talent could raise goes in the stat pipeline.** Per-character numbers are new `Stat`s, so items, buffs and (later) talents raise them the same way, through `collect()` modifiers. Examples: crit chance and crit damage, poison or bleed build-up, spell power, max mana, draw speed. The rule tunables in `gMelee` stay global for now; when the trees arrive, the ones talents bend get read through one per-combatant accessor.
2. **Every weapon gets a class in its data**, e.g. `"class": "dagger"` in items.json, and code asks the class, never the item id. Conditions key on the class. This also replaces `heldModel`'s knife-or-sword guess.
3. **Pressed actions share one path.** Spells, weapon skills and special shots all go through one action path with a cost (stamina or mana), a cooldown and a cast or windup. The ability bar will then just call it, and a talent or spell is only data naming an action.
4. **Timed effects share one list.** Poison, bleed, frost, buffs, invisibility and weapon enchants are timed effects on the combatant: a kind, a source, a duration, stacks and modifiers. Spells, items and talents all add to it.
5. **No new use-based skills.** No Archery or Destruction skill that levels as you use it. Use-based skills are going away. Until the rework, new weapons earn the existing XP the way swords do.

## Decisions (2026-11-08)

- **No classes.** There are three talent trees: **Warrior**, **Ranger** and **Mage**. Points can be spent in any of them.
- **Budget.** You get one point per level up to level 100, so 99 points in all.
- **No use-based skills.** One-Handed, Unarmed and Armor no longer level up as you use them. Character XP comes from:
  - kills;
  - quests;
  - discovery (the first time you enter a region).
- **Respec costs something**, and each respec costs more than the last. There's no gold yet, so until it exists the debug window respecs for free.
- **Gear is open to everyone; talents make it good.** Anyone can wield any weapon or wear any armor. Talents make it better (more damage, less weight, new rules). Spells are the exception: casting one needs its talent.

## Rules

- **Tree size.** Each tree holds about 140 points of ranks, so the three trees together are about 4× the budget. A character who spends everything in one tree still leaves about 30% of it unbought.
- **Tiers.** Tiers run 0–7. Tier N needs 5 × N points spent in that tree, so a tree's top talents (tier 7) need 35 points in it. The tier size stays a single number in the data, so it can be tuned.
- **Requirements.** A talent may need another talent of its tree at max rank.
  - **Cross-tree talents** need points in a second tree as well, e.g. `"needs": {"mage": 15}`. They make the hybrids real: spellblade, paladin, assassin.
- **Branches.** Inside a tree, each talent belongs to a branch (Shield, Plate, ...). Branches only group talents in the window; the tier gate counts the whole tree.
- **Three kinds of talent:**
  - **Stat:** a modifier through the existing stat pipeline, e.g. +4% armor. These are filler; few talents should be only this.
  - **Perk:** changes a combat rule, with a value per rank, e.g. a wider guard angle or a longer perfect-guard window. Code checks for perks by id, from a closed list.
  - **Ability:** something you press: a shield bash, a war cry, a spell. This needs an ability bar, so abilities come in a later ADR (see Phases).
- **Conditions.** A modifier or perk can say when it counts. This replaces today's `with` (a weapon skill). The conditions are a closed list:
  - by weapon class (fists, dagger, sword, ...);
  - by grip (two-handed, dual-wielding);
  - holding a shield;
  - by load (light, medium, heavy);
  - while guarding;
  - in a heavy attack;
  - in a riposte.

## Warrior (draft)

The **When** column gives the condition a talent counts under; a dash means always.

**Shield**

| Talent | Tier | Ranks | Kind | Per rank | When |
|---|---|---|---|---|---|
| Braced | 0 | 5 | stat | blocked hits cost 6% less stamina | shield |
| Wide Guard | 1 | 1 | perk | the guard covers 100° instead of 70° | shield |
| Clean Timing | 2 | 3 | perk | +0.05 s perfect-guard window | shield |
| Rebuke | 3 | 3 | stat | guard counters do +15% damage and stance damage | — |
| Iron Rebuke | 4 | 1 | perk | a perfect guard bounces any attack, like a greatshield; needs Clean Timing | shield |
| Unbroken | 5 | 1 | perk | half as long reeling from a guard break | — |
| Living Wall | 7 | 1 | perk | the guard covers every side, and stamina regenerates at full speed while guarding | shield |

**Plate**

| Talent | Tier | Ranks | Kind | Per rank | When |
|---|---|---|---|---|---|
| Plate Training | 0 | 5 | perk | armor weight counts 8% less toward the load | — |
| Thick Hide | 1 | 5 | stat | +4% armor | heavy load |
| Steady Feet | 2 | 3 | stat | +10% poise | — |
| Armored Roll | 3 | 1 | perk | a roll under a heavy load keeps the full i-frames | heavy load |
| Walking Fortress | 7 | 1 | perk | heavy attacks can't be interrupted by hits below 40 stance damage | heavy load |

**Endurance**

| Talent | Tier | Ranks | Kind | Per rank | When |
|---|---|---|---|---|---|
| Deep Lungs | 0 | 5 | stat | +6 max stamina | — |
| Hearty | 0 | 5 | stat | +2 vitality | — |
| Second Wind | 2 | 3 | perk | stamina regen starts 0.1 s sooner | — |
| Hard to Kill | 4 | 3 | stat | 4% less damage taken | — |
| Last Stand | 7 | 1 | perk | once every 2 minutes, a killing blow leaves you at 1 health | — |

**Battle**

| Talent | Tier | Ranks | Kind | Per rank | When |
|---|---|---|---|---|---|
| Heavy Hand | 0 | 5 | stat | +4% damage | heavy attack |
| Breaker | 1 | 5 | stat | +5% stance damage | — |
| Quick Charge | 2 | 3 | perk | a heavy charges 0.08 s sooner | — |
| Executioner | 3 | 3 | stat | +15% damage | riposte |
| Unstoppable | 5 | 1 | perk | a charged heavy can't be interrupted | — |
| Momentum | 7 | 1 | perk | each hit of a string does +10% damage, up to +30% | — |

**Arms** (later, with the weapon classes):
- one-handed, two-handed and dual-wield mastery;
- per-class talents (dagger, sword, axe, mace, greatsword, great hammer, spear);
- the berserker's dual-wield strings.

## Ranger (only what the engine can do today)

| Talent | Tier | Ranks | Kind | Per rank | When |
|---|---|---|---|---|---|
| Light Feet | 0 | 5 | stat | rolls cost 8% less stamina | — |
| Knife Work | 0 | 5 | stat | +4% damage | dagger |
| Slip | 1 | 3 | perk | +0.03 s of i-frames | light load |
| Long Stride | 2 | 3 | stat | +4% move speed | light load |
| Backstep Strike | 3 | 1 | perk | an attack right after a backstep is a rising thrust, like the attack after a roll | — |

The rest of the Ranger tree waits for its combat features, among them bows, poisons, critical hits and stealth.

## Mage

Empty until magic exists. Planned branches:
- weapon enchantment;
- elemental;
- healing;
- nature;
- necromancy;
- light, which the paladin build uses;
- utility: invisibility and teleport.

## Phases

**First, every combat feature is built.** That's every item on docs/COMBAT_ROADMAP.md: weapon classes, two-handing and dual wielding, ranged combat, magic, criticals, status effects, weapon skills, stealth and the rest. The trees are filled from what exists then, and these draft tables get revised to match.

1. **The talent framework (its own ADR):**
   - the three trees, branches, tiers, requirements and cross-tree talents;
   - perks and conditions;
   - the use-based skills dropped;
   - XP from kills, quests and discovery;
   - the costly respec;
   - the new Progress window.

   Old saves: their skills are dropped, and their old talents are dropped with the points refunded, each with a note.
2. **The tree contents:**
   - Warrior, Ranger and Mage talents for every combat system;
   - spells and abilities as talents, on the shared action path;
   - an ability bar, if combat hasn't brought one by then.
