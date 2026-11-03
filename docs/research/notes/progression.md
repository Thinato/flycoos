# RPG Progression Systems: Use-Based Skills, XP/Levels, Talent Trees, Stat Modifier Pipelines

Context for the reader: flycoos today has a flat `Stats` struct (`stamina`, `strength`, `agility`, `intellect`, `spirit`) with free-function derived stats in `src/stats.h`. `maxHealth = stamina * kHealthPerStamina`, `healthRegen = spirit * kRegenPerSpirit`, `swingTime = speed / (1 + agility * kHastePerAgility)`, `strengthBonus = round(strength * attack.speed)`, and `armorReduction = min(0.8, armor / (armor + kArmorK * attackerLevel))` with `kArmorK = 40`. Armor is summed from equipped items (`armor(player.inv)`). Tests are in `tests/core_test.cpp`, using a `CHECK` macro. NPCs have levels and the player does not. (Source: local repo, `src/stats.h` lines 8-48, `tests/core_test.cpp` lines 350-372 and 711-747.)

## Q1. What exact formulas does Skyrim use for skill XP and for character level from skills? What are the known design problems and fixes?

### Takeaway
Skyrim uses three tables of constants and three closed-form formulas.
- **XP per use:** `UseMult * baseXP + UseOffset`, set per skill.
- **Cost of a skill level:** `ImproveMult * (L-1)^1.95 + ImproveOffset`, set per skill.
- **Character XP:** each skill level gained adds its new level number to character XP. Level N→N+1 costs `25*(N+3)`.

The main design problem is that enemy scaling keys off total character level. Leveling non-combat skills therefore makes the world harder without making the player stronger at combat. Use-based XP also invites degenerate loops: hitting invulnerable allies, crafting spam, and re-picking locks. Bethesda's fixes are per-skill guards on what counts as a valid use: a real target, actual healing, and no XP for re-picking an opened lock.

### Cited Findings
**Skill XP (per use and per level)**
- XP per use = `Skill Use Mult × (base XP × skill-specific multipliers) + Skill Use Offset`. Example: Lockpicking at AdvSkill 50 gives `45 × 50 + 10 = 2260`. — [UESP Skyrim:Leveling](https://en.uesp.net/wiki/Skyrim:Leveling)
- XP to advance a skill = `Skill Improve Mult × (level − 1)^1.95 + Skill Improve Offset`. The exponent is the global `fSkillUseCurve` (1.95). UESP flags this formula as "needs verification". — [UESP Skyrim:Leveling](https://en.uesp.net/wiki/Skyrim:Leveling)
- Per-skill constants (Use Mult / Use Offset / Improve Mult / Improve Offset → total skill XP to go 15→100) — [UESP Skyrim:Leveling](https://en.uesp.net/wiki/Skyrim:Leveling):
  - Combat and magic skills mostly use Improve Mult 2 and offset 0, for a total of 528,804 XP. Use Mults: One-Handed 6.3, Two-Handed 5.95, Archery 9.3, Block 8.1, Heavy Armor 3.8, Light Armor 4, Destruction 1.35, Restoration 2, Alteration 3, Conjuration 2.1, Illusion 4.6, Speech 0.36.
  - Crafting and utility skills use small Improve Mults with large offsets, giving much cheaper totals:
    - Smithing: 1/0/0.25/300 (91,600)
    - Lockpicking: 45/10/0.25/300 (91,600)
    - Pickpocket: 8.1/0/0.25/250 (87,350)
    - Sneak: 11.25/0/0.5/120 (142,401)
    - Alchemy: 0.75/0/1.6/65 (428,568)
    - Enchanting: 900/0/1/170 (278,852)
  - Smithing's Use Mult is 160 in the original Skyrim.esm, and Destruction XP is reduced on difficulties above Adept.
- Skills start at 15, plus racial bonuses. A skill at 100 can be made Legendary (patch 1.9), which resets it to 15 and refunds its perks. — [UESP Skyrim:Skills](https://en.uesp.net/wiki/Skyrim:Skills)

**What counts as a use (anti-exploit guards)**
- Destruction needs a target. Conjuration needs a foe present. Restoration healing must actually heal damage. Re-picking an opened lock gives no XP, and neither does failing with the Skeleton Key. Speech challenges usually work once. — [UESP Skyrim:Skills](https://en.uesp.net/wiki/Skyrim:Skills)
- Weapon skill XP counts base hits only. Enchantment damage does not count. Tempering and perks raise damage "without actually doubling the experience gained". Smithing XP scales with the value of the forged item, and Speech with the value of goods sold. — [UESP Skyrim:Skills](https://en.uesp.net/wiki/Skyrim:Skills)
- Trainers can raise skills at most 5 times per character level, across all skills combined. — [UESP Skyrim:Leveling](https://en.uesp.net/wiki/Skyrim:Leveling)

**Skill levels to character level**
- Character XP gained = new skill level × `fXPPerSkillRank` (default 1). For example, Alchemy 20→21 gives 21 XP. — [UESP Skyrim:Leveling](https://en.uesp.net/wiki/Skyrim:Leveling)
- XP needed for the next level = `(level + 3) × 25` = `fXPLevelUpBase (75) + level × fXPLevelUpMult (25)`. Cumulative XP from level 1 to N is `12.5N² + 62.5N − 75`. Level from total XP is `floor(−2.5 + sqrt(8·XP + 1225)/10)`. — [UESP Skyrim:Leveling](https://en.uesp.net/wiki/Skyrim:Leveling)
- Each level gives +10 Health, Magicka or Stamina (player's choice) and 1 perk point, which can be saved. Before patch 1.9 the cap was level 81, at 88,085 XP. Taking one skill from 15 to 100 yields 4,930 character XP. Reaching level 252 takes 809,475 XP and gives 251 perks. — [UESP Skyrim:Leveling](https://en.uesp.net/wiki/Skyrim:Leveling)

**Predecessor: Oblivion's attribute-multiplier system (and why it needed "efficient leveling")**
- In Oblivion, a level-up comes only from 10 increases in major skills. Minor skills do not count. — [UESP Oblivion:Efficient Leveling](https://en.uesp.net/wiki/Oblivion:Efficient_Leveling)
- At level-up the player raises 3 of 8 attributes. Each attribute's bonus depends on how many times its governed skills rose since the last level: 0 → +1, 1–4 → +2, 5–7 → +3, 8–9 → +4, 10+ → +5. Getting +5/+5/+5 means tracking governed skill-ups and avoiding accidental major-skill level-ups. — [UESP Oblivion:Efficient Leveling](https://en.uesp.net/wiki/Oblivion:Efficient_Leveling)
- UESP says "under-leveling" through minor skills "produces extremely powerful characters relative to the enemies encountered". This is the inverse of the scaling problem: the character is strong while the world stays low. — [UESP Oblivion:Efficient Leveling](https://en.uesp.net/wiki/Oblivion:Efficient_Leveling)

**Level scaling and grinding problems**
- Skyrim enemies and loot are generated from the player's current level. A dungeon with a minimum level uses that minimum when the player is below it, and some dungeons also have a maximum. — [UESP Skyrim:Dungeons (redirect from Encounter Zone Level)](https://en.uesp.net/wiki/Skyrim:Encounter_Zone_Level)
- Leveled lists take a requested level, usually the player's level and possibly modified by the encounter zone. Each list picks uniformly among eligible entries. Some lists cap item quality: civilian armor at "Advanced", civilian weapons at elven. — [UESP Skyrim:Leveled Lists](https://en.uesp.net/wiki/Skyrim:Leveled_Lists)
- Critics note that scaling uses total level, so "if you leveled up from non-combat skills then you'll have a bad time". One proposal is to gate content on combat-skill totals instead. — [Steam discussion](https://steamcommunity.com/app/489830/discussions/0/3884977132825320881); [Nexus forums](https://forums.nexusmods.com/profile/3222214-chatt7/content/) (forum opinion, not authoritative)
- In a reported exploit, a player reached level 241 without leaving Helgen by repeatedly hitting a companion who cannot die but still grants XP. In a related case, a player's smithing stayed low, so they couldn't get gear to match the scaled enemies. — [GamesRadar](https://gamesradar.com/skyrim-player-explains-how-levelling-exploit-broke-their-whole-game); [GameRant](https://gamerant.com/skyrim-player-level-241-helgen/)
- Smithing and enchanting loops are a recurring complaint ("easy to break"). — [waltoriouswritesaboutgames](https://waltoriouswritesaboutgames.com/2017/03/03/finding-the-grind-in-skyrim/) (blog opinion)

### Inferences
- The Skyrim structure is fully data-driven: 4 floats per skill plus 3 globals (`fSkillUseCurve`, `fXPLevelUpBase`, `fXPLevelUpMult`). Each formula is pure and closed-form, so in C++23 it fits a `constexpr` table and `static_assert`-checked unit tests. For example, assert that cumulative XP to level 81 is 88,085, and that one skill 15→100 gives 4,930.
- Because character XP equals the new skill level, raising high skills levels the character faster than raising low ones. Leveling cheap crafting skills (Smithing at about 91.6k total vs 528.8k for combat) pushes character level, and with it enemy scaling, ahead of combat ability. This is the root of the "Smithing/Alchemy trap".
- Fix options ranked by simplicity for flycoos:
  1. Don't use world level scaling. Fixed-level zones, as in WoW, remove the trap entirely; flycoos NPCs already have fixed levels.
  2. If scaling is used, key it on combat power (gear armor and weapon DPS, or combat skills), not total level.
  3. Count only "real" uses: damage dealt to a hostile non-immune target that can grant XP, a diminishing-returns window per target, and craft XP scaled by the item's value delta.
  4. Cap XP per target or per time window. Skyrim's trainer cap of 5 per level is a cheap version of this.
- The flycoos armor formula already uses attacker level (`armor/(armor + K*level)`). Adding player level therefore automatically makes high-level NPCs pierce armor more. Wherever the player is the attacker, the player's level also needs to feed that formula.

### Gaps
- UESP flags the exact skill-improve formula as needing verification. The "base XP" per action is not listed per skill: weapon XP per hit is base damage-related, but no exact number was found.
- No authoritative source (Bethesda GDC or postmortem) on the design rationale for Skyrim's scaling was found. Critiques found were forum and press opinion.
- The Encounter Zone page did not document zone-level locking on first entry or NPC "level mult" (PC-level-relative actors), so these are not confirmed here.

## Q2. How do talent trees work as data structures (nodes, ranks, prerequisites, point gating), and how are their effects applied?

### Takeaway
A talent tree is a graph of node definitions. Each node has an id, a max rank, a row/tier, an optional prerequisite node at full rank, and an effect per rank. A point pool is granted by level. Gating is either per-tier (vanilla WoW: 5 points spent in a tree per row) or by thresholds of total points spent (Dragonflight: rows 5 and 8 need 8 and 20 points). The only state a player holds is `map<nodeId, rank>`. Effects are applied by turning the ranks into modifiers or ability grants each time stats are recomputed.

### Cited Findings
- Classic WoW has three trees per class. "Most talents had multiple ranks", often 5 points for the full effect. Deeper tiers unlock after investment, with the original system requiring "spending 5 points in each tier before moving to the next one". Trees had 11 rows, condensed to 7 in Cataclysm. Some talents require specific earlier talents. — [Warcraft Wiki: Talent](https://warcraft.wiki.gg/wiki/Talent)
- Vanilla WoW points: one point per level starting at level 10, 51 total at level 60 (60 − 10 + 1). — [west-games WoW Classic talent calculator](https://west-games.com/wow-classic-talent-calculator/); [wowclassicstalentcalculator How It Works](https://wowclassicstalentcalculator.vercel.app/how-it-works) (fan sites, consistent with each other)
- Respec happens only at a class trainer, and the cost "rose with each reset". Dual Specialization arrived in 3.1.0 for 1000 gold, and Cataclysm cut it to 100 gold at level 30. — [Warcraft Wiki: Talent](https://warcraft.wiki.gg/wiki/Talent)
- Warcraft Wiki gives conflicting totals for later expansions ("76 talent points" vs "reduced from 71 to 41" at Cataclysm). This is an internal inconsistency in the source. — [Warcraft Wiki: Talent](https://warcraft.wiki.gg/wiki/Talent)
- Mists of Pandaria (5.0.4) replaced trees with 6 tiers of 3 mutually exclusive choices, unlocked at levels 15/30/45/60/75/90. Shadowlands (9.0.1) moved them to 15/25/30/35/40/45/50. — [Warcraft Wiki: Talent](https://warcraft.wiki.gg/wiki/Talent)
- Dragonflight (10.0.0) restored trees, with one class tree and one spec tree each having its own pool. Points arrive at alternating levels. The 5-per-tier rule was dropped: row 5 needs 8 points spent in the tree and row 8 needs 20. — [Warcraft Wiki: Talent](https://warcraft.wiki.gg/wiki/Talent)
- At level 70 Dragonflight gives 61 points (31 class, 30 spec), unlocking from level 10 with one point per level alternating trees. It is impossible to take every talent. Class trees hold broad utility and spec trees hold rotation and throughput. Loadouts can be saved, imported and exported. Visually there are two trees, but "in the API it's all 1 tree". — [Icy Veins Dragonflight Talent System Guide](https://icy-veins.com/wow/dragonflight-talent-system-guide); [Roccat blog](https://eu.roccat.com/blog/world-of-warcraft-dragonflight-talent-trees-explained-pc)
- Skyrim perks follow a simpler model: 1 perk point per level, saveable, with per-skill trees whose perks require a skill level. Legendary resets refund the skill's perks. Dragonborn can spend a dragon soul to reset one tree. — [UESP Skyrim:Leveling](https://en.uesp.net/wiki/Skyrim:Leveling)
- In Unreal GAS, effects (buffs, talents, passives) are `GameplayEffect` assets holding modifiers. A modifier can require Source/Target tags ("considered only when the effect is applied") or use tag filters ("Modifiers which source or target didn't have all of the tags of the filter are excluded"). This is the reference pattern for conditional effects. — [tranek GASDocumentation](https://github.com/tranek/GASDocumentation)

### Inferences
- Minimal data model, kept as a constexpr table in the same style as the existing item and NPC tables:
  ```cpp
  struct TalentDef {
      std::string_view name;
      uint8_t tree, row, maxRank;
      int16_t requires = -1;              // node index that must be at maxRank, or -1
      std::array<Modifier, 2> perRank;    // stat modifiers scaled by current rank
      AbilityId grants = NoAbility;       // unlocked at rank >= 1
  };
  struct Talents { std::array<uint8_t, kTalentCount> rank{}; };   // the only saved state
  int pointsEarned(int level) { return std::max(0, level - kFirstTalentLevel + 1); }
  int spentInTree(const Talents&, int tree);
  bool canLearn(const Talents&, int level, int node);  // pool > spent, rank < max, prereq maxed, spentInTree >= row*kPointsPerRow
  ```
- All validation lives in one pure function, `canLearn`. A respec is `rank.fill(0)`. Effects are never applied imperatively. Instead, the stat pipeline in Q3 iterates learned talents and emits `Modifier{stat, op, value * rank}`. This removes "apply/unapply" bugs.
- Unlocking abilities is a set query (`hasAbility = any learned talent grants it`), not a stored flag.
- Conditional effects ("+10% damage vs beasts", "while below 30% health") can't be precomputed into cached stats. Keep two categories:
  1. Static modifiers, which go in the cached stat sheet.
  2. Conditional modifiers, evaluated in the damage function at hit time against a context: target kind, health fraction, and so on. This mirrors GAS's split between attribute aggregation and ExecutionCalculation.
- WoW's history (deep trees → 1-of-3 rows → trees again) suggests the cheapest viable version for a small game: rows of 2–3 exclusive choices at fixed levels (MoP style). It is the same data model with `maxRank = 1` and a "one per row" rule.

### Gaps
- No primary Blizzard documentation was found for vanilla talent point rules. The figures come from consistent fan calculators. Exact respec cost progression in vanilla was not found.
- The node-level mechanics of Dragonflight choice nodes (one node with two options) were not documented in the sources fetched.

## Q3. What is a robust, testable architecture for computing final stats from many modifier sources, with caching and dirty flags?

### Takeaway
The industry-standard pattern is `final = (base + Σflat) × (1 + Σincreased%) × Π(1 + more%)`, optionally followed by an override and a clamp:
- **Base:** class/level, plus attributes from level-ups.
- **Flat:** gear and talents.
- **Increased%:** additive percentages pooled into one multiplier.
- **More%:** independent multipliers.

Path of Exile's "increased vs more" is the clearest player-facing version. Unreal GAS uses `((Base + Add) × Mul) / Div`, but by default it sums its Multiply modifiers, which Tranek calls surprising. Store modifiers tagged with their source, so removing an item or buff means removing by source. Recompute lazily behind a dirty flag.

### Cited Findings
- PoE rule (community explanation): `[flat added] × [1 + Σincreased − Σreduced] × Π more/less`. Example: 100 with 20% increased gives 120, while 100 with 20% increased and 20% more gives 144. — [PoE forum: Increased, more, additive, multiplicative](https://pathofexile.com/forum/view-thread/261646); [PoE forum: Additive and Multiplicative modifiers](https://www.pathofexile.com/forum/view-thread/892570) (community posts. The PoE wikis returned 402 or a bot block, so this was not verified against poewiki.)
- PoE has edge cases where a group of "increased" modifiers is itself applied as a "more". For example, shock sums with other "enemies take increased damage" sources, and that sum acts as a separate multiplier. — [PoE forum thread 2936414](https://www.pathofexile.com/forum/view-thread/2936414)
- GAS modifier ops are Add, Multiply, Divide and Override. Aggregation (`FAggregatorModChannel::EvaluateWithBase`) is `((InlineBaseValue + Additive) * Multiplicitive) / Division`. Override replaces the result, and the last applied override wins. — [tranek GASDocumentation](https://github.com/tranek/GASDocumentation)
- By default GAS sums Multiply and Divide modifiers before applying them, so two ×1.5 give ×2.0, not ×2.25. Tranek calls this unexpected, and true multiplicative stacking requires engine changes. — [tranek GASDocumentation](https://github.com/tranek/GASDocumentation)
- GAS attributes hold a BaseValue and a CurrentValue. Instant effects change BaseValue permanently, while Duration and Infinite effects change CurrentValue and are removed on expiry. Epic's example: base 100 with a fatigue effect gives 70. Raising the base to 110 by leveling then gives 77 with the effect still active. In other words, modifiers are re-derived from the base, not baked in. — [Epic: Gameplay Attributes and Attribute Sets](https://dev.epicgames.com/documentation/en-us/unreal-engine/gameplay-attributes-and-attribute-sets-for-the-gameplay-ability-system-in-unreal-engine); [tranek GASDocumentation](https://github.com/tranek/GASDocumentation)
- GAS has no built-in clamping. Epic clamps in the setter (written to the base value so modifiers still apply) and in `PostGameplayEffectExecute`. Tranek says clamping in `PreAttributeChange` "does not permanently change the modifier", so anything that recomputes CurrentValue must clamp again. — [Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/gameplay-attributes-and-attribute-sets-for-the-gameplay-ability-system-in-unreal-engine); [tranek GASDocumentation](https://github.com/tranek/GASDocumentation)
- GAS uses `Custom Calculation Class` (ModifierMagnitudeCalculation) for computed magnitudes, and ExecutionCalculations hold damage logic that reads buffs and debuffs at hit time. — [tranek GASDocumentation](https://github.com/tranek/GASDocumentation)
- The Kryzarel "Character Stats in Unity" pattern has these parts:
  - Modifier types Flat, PercentAdd and PercentMult, applied in enum order by default, with an `Order` override (for example, a Flat at order 201–299 lands between PercentAdd and PercentMult).
  - Consecutive PercentAdd modifiers are summed, then multiplied in once.
  - The final value is cached behind a dirty flag.
  - Each modifier carries a `Source`, so `RemoveAllModifiersFromSource` works.

  — [Kryzarel pt.1](https://medium.com/@kryzarel/character-stats-attributes-in-unity-pt-1-70f90ade9788); [Kryzarel pt.2](https://medium.com/@kryzarel/character-stats-attributes-in-unity-pt-2-additive-modifiers-optimizations-1dfb2d42f3c8) (Medium returned 403 on direct fetch. These details come from search snippets.)

### Inferences
- **Recommended pipeline for flycoos.** Keep it a pure function, with no class hierarchy:
  ```cpp
  enum class Op : uint8_t { Flat, Increased, More };          // order == enum order
  struct Modifier { StatId stat; Op op; float value; };      // Increased/More as fractions (0.1 = 10%)
  struct StatSheet { std::array<float, kStatCount> v; };
  // base: race/class table + per-level growth + allocated points
  StatSheet compute(const StatSheet& base, std::span<const Modifier> mods) {
      std::array<float, kStatCount> flat{}, inc{}, more; more.fill(1);
      for (auto& m : mods) switch (m.op) {
          case Op::Flat: flat[m.stat] += m.value; break;
          case Op::Increased: inc[m.stat] += m.value; break;
          case Op::More: more[m.stat] *= 1 + m.value; break; }
      StatSheet out;
      for (int s = 0; s < kStatCount; ++s)
          out.v[s] = (base.v[s] + flat[s]) * std::max(0.f, 1 + inc[s]) * more[s];
      return out;   // clamp/round at the consumer (maxHealth, swingTime...), as stats.h does today
  }
  ```
- **Collecting modifiers.** `collect(player) → vector<Modifier>` concatenates modifiers from equipped items, learned talents × rank, and active buffs. The stat sheet is `compute(base(level, allocations), collect(player))`. Because the list is rebuilt from sources each time, "remove by source" is free: unequip or expire, then rebuild. No per-modifier ids or unapply logic are needed.
- **Caching.** A single `bool statsDirty` on the player, set by equip/unequip, talent learn, level-up and buff add/expire, is enough. With about 5 attributes and fewer than 100 modifiers, recomputing is microseconds. Even caching may be optional: computing once per frame for the player plus each NPC in combat is cheap. Start uncached and add the dirty flag only if profiling shows a need.
- **Derived stats stay derived.** Keep `maxHealth(stamina)`, `swingTime(agility)` and similar as functions of the final sheet, as they already are in `src/stats.h`. A "+5% max health" then becomes an `Increased` on a derived `MaxHealth` stat, applied after `stamina × 10`. This gives two layers: primary attributes, then derived stats, each with its own modifiers. GAS does the same with attribute-based modifiers.
- **Override.** Skip it unless needed. If added, apply it after `More` with last-writer-wins, as GAS does.
- **Testability.** `compute` is a pure function, so unit tests can be literal arithmetic checks:
  - `(10 + 5) × (1 + 0.1 + 0.2) × 1.5 = 29.25`
  - Two Increased 50% give 2.0×, while two More 50% give 2.25×. This documents the deliberate difference from GAS's default.
  - Removing an item's modifiers restores the base exactly. This is the order-independence and round-trip property.
- **Floating point.** Round only at display or integer consumers. `strengthBonus` already uses `lround` and `int maxHealth`. Keep the sheet in float so small percentages don't truncate. Treat this as an assumption, since the Kryzarel rounding detail could not be verified.

### Gaps
- The poewiki "Modifier" page was blocked (Anubis bot protection or 402). The PoE formula is cited from forum posts, not the wiki or GGG documentation.
- The Epic GameplayEffect page was not fetched directly. Duration-policy and aggregation details come from Tranek's community documentation, which is widely used but unofficial.
- No GDC talk on stat pipelines was found within the search budget.

## Q4. How do these systems interact with combat formulas (damage, and the armor reduction already present)?

### Takeaway
Level enters combat in two ways. First, it raises base stats and grants points, which raise damage and health through the pipeline. Second, it is a direct term in mitigation formulas: flycoos's `armor/(armor + K·attackerLevel)` follows the WoW-style design where the same armor value is worth less against higher-level attackers. Scaling has to be planned for both together, or player power grows faster or slower than intended.

### Cited Findings
- flycoos today:
  - `armorReduction(armor, attackerLevel) = min(0.8, armor/(armor + 40·attackerLevel))`.
  - Hits after armor are never below 1.
  - Damage = weapon roll + `round(strength × attack.speed)`.
  - Health = `stamina × kHealthPerStamina`.
  - Tests confirm 37 armor gives about 0.316 reduction against a level-2 attacker, and about 0.481 against level 1 (40 armor halves a level-1 hit).

  — local `src/stats.h` lines 25-48; `tests/core_test.cpp` lines 711-720
- Skyrim's level-up gives a flat +10 to one pool (Health, Magicka or Stamina) plus a perk. Most power growth comes from skills and perks multiplying gear, not from the level itself. — [UESP Skyrim:Leveling](https://en.uesp.net/wiki/Skyrim:Leveling)
- Skyrim weapon skill XP ignores tempering, perk and enchantment damage. XP is decoupled from the damage multipliers, so stacking gear and perks doesn't accelerate the grind. — [UESP Skyrim:Skills](https://en.uesp.net/wiki/Skyrim:Skills)
- GAS separates attribute aggregation (buffs and gear into stats) from ExecutionCalculations (the damage formula, which reads captured source and target attributes and applies buffs and debuffs at hit time). — [tranek GASDocumentation](https://github.com/tranek/GASDocumentation)
- Skyrim generates enemies and loot from player level, with a minimum per dungeon. Leveled lists cap item tiers, for example civilian weapons at elven. — [UESP Skyrim:Dungeons](https://en.uesp.net/wiki/Skyrim:Encounter_Zone_Level); [UESP Skyrim:Leveled Lists](https://en.uesp.net/wiki/Skyrim:Leveled_Lists)

### Inferences
- **Damage function shape.** Keep a single `resolveHit(attackerSheet, defenderSheet, attack, ctx)` that does the following:
  1. Roll the weapon damage.
  2. Add the strength bonus.
  3. Apply attacker `Increased`/`More` damage modifiers, including conditional ones from `ctx` such as target kind and health fraction.
  4. Apply `armorReduction(defender.armor, attacker.level)`.
  5. Apply defender damage-taken modifiers.
  6. Floor at 1.

  This matches the existing order (`src/stats.h` already floors at 1) and the GAS ExecutionCalculation split.
- **Armor and level interplay.** Because K scales with attacker level, a fixed armor value loses about half its effectiveness as the attacker's level doubles. Expected gear armor per tier should therefore grow roughly linearly with level to keep reduction constant. That is a testable invariant: "Iron set vs a same-level attacker ≈ X% reduction at each tier's level". Armor as an `Increased%` from talents compounds with this, so cap the result at the existing 0.8, not raw armor.
- **Player level in the formula.** Once the player has a level, NPC armor needs it too, since the player is the attacker. If the player levels by skill use (Skyrim) while NPCs have fixed levels, the player's character level directly weakens NPC armor. That is a feedback loop to tune deliberately.
- **Balance tuning with data.** Keep all constants in tables:
  - XP curve: base and mult.
  - Points per level.
  - Per-skill use and improve multipliers.
  - Talent per-rank values.
  - kArmorK.

  Also add a headless test or benchmark that simulates "level-N player in tier-N gear vs a level-N NPC". It should assert time-to-kill stays within a band (for example 3–8 swings) for every N. The existing fight tests in `tests/core_test.cpp` (armored player vs bandit) are the template for this. The `debug_ui` sliders (`armorK`) already provide the live-tuning knob.
- **Skill-by-use and combat.** If use-based weapon skills are adopted, compute XP from base weapon damage or swing count against a valid hostile target, not from final damage, as Skyrim does. Add diminishing XP per repeated target so that training dummies or immortal NPCs (for example story NPCs) can't be farmed. This would likely be a single guard in the XP-award function, where all damage routes through.

### Gaps
- No source quantified "time-to-kill" targets or level-scaling curves for armor formulas of the `armor/(armor + K·level)` form. The WoW provenance of this formula was not re-verified here.
- No authoritative source covered how WoW or Skyrim combine level-difference hit and miss penalties with mitigation, so that is not covered.
