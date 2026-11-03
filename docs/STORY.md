# Story: Act 1, "The Ravensford Vale"

A starter act on the seed-1337 world (`worlds/story.flyw`). It shows what the editor and game can do: sculpted terrain, painted roads, prefab instances, regions and points of interest, and spawn markers with named characters and dialogue lines. All names are original.

## Premise
After a long winter road the player reaches the Wayfarer's Shrine, at the edge of the Ravensford Vale.
- **Ravensford:** a frontier ford village. Its elder asks for help.
- **Hakon the Red:** leader of the bandits who hold the Old Watchtower on Greyfang Rise. They raid the road and stole the smith's iron.
- **The Greywood:** a wolf-filled forest.
- **The Barrow of King Ulfar:** someone broke its seal, and since then the dead walk at the treeline.

The story is carried by dialogue, region names and place labels, and by one quest, "Torvald's Helm" (below; quests are data, ADR-026).

**Suggested route:** shrine → Ravensford (Maren, Torvald) → Mirrormere (Wenna) → Greywood (Sigrun, wolves) → Greyfang Rise (raiders, Hakon, Iron Helm) → the Barrow (ghouls).

## Places (x, z)
| Place | Marker | Where | What's there |
|---|---|---|---|
| Ravensford Vale | region #7, r300 | (520, 500) | The whole act area |
| Wayfarer's Shrine | poi #6, player_spawn #5 | (514, 498) | Stone wayshrine where the player starts and respawns |
| Ravensford | region #8, r55 | (560, 408) | Longhouse, 3 cottages, forge, well, market stalls, training yard, palisade with gate (548, 447) |
| Ravensford Ford | poi #9 | (602, 418) | Bridge over the stream from the north pond to Mirrormere |
| Mirrormere | region #16, poi #17 | (722, 496), dock (718, 457) | Lake, dock, Wenna's fishing hut |
| Greywood | region #19, poi #20 | (400, 452), lodge (410, 450) | Forest, Hunter's Lodge clearing, three wolf pairs, boars |
| Greyfang Rise | region #28, pois #29, #30 | (632, 625) | Raised crag, Old Watchtower on the summit (y83), Hakon's Camp on a ledge (627, 597), switchback path from (598, 578) |
| Barrow of King Ulfar | region #31, pois #32, #33 | (290, 603) | Mound with a stone portal, tunnel down to a burial chamber (276, 603), standing-stone ring |

**Roads:**
- 3-wide Dirt: shrine → gate → green → bridge → Mirrormere dock.
- Branch south-east: to the foot of Greyfang Rise.
- Forest track west: to the lodge, then a faint track south-west to the barrow.

## Characters
| Who | Kind (marker) | Where | Lines (in order; talkers on E, fighters shout when they turn hostile) |
|---|---|---|---|
| Maren Holt, the elder | villager (#12) | Longhouse | Welcome · Hakon holds the Old Watchtower · they took Torvald's iron · see Torvald, mind the Greywood · Sigrun says the barrow was opened |
| Torvald, the smith | villager (#13) | Forge | His iron is gone · bring back the helm Hakon wears · keep your blade up |
| Captain Brenna | guard (#14) | Gate | Halt · sheathe your blade inside · bandits sighted near the ford |
| Edric | guard (#15) | Bridge | Hakon wants the ford · Wenna fishes at Mirrormere |
| Old Wenna | villager (#18) | Dock | Fish are biting · pale lights the night the barrow opened · wolves don't come to the shore · find Sigrun |
| Sigrun, the hunter | villager (#21) | Hunter's Lodge | Quiet · wolves run in pairs · the barrow's seal is broken · the dead walk · bring a good blade |
| Hakon the Red | bandit_chief (#27) | Watchtower door | Taunts; always drops the Iron Helm |
| Hakon's Raiders | bandit ×2 (#25, #4, #26) | Lower path, camp, behind the tower | "Your coin or your life!" · "Hakon pays well for heads!" |
| Wolves | wolf ×2 (#22, #23, #24) | Greywood N / W / S | Not named |
| Barrow Ghouls | ghoul ×2 (#34 stone ring, #35 chamber), night only | Barrow | "Ulfar... sleeps... no more..." · "Leave... the king's... gold..." |

The village also has two generic villagers (#2), a training dummy (#1) and boars (#3, ×2): 28 NPCs in all, 24 by day.

**Night:** the ghoul markers are active from 0.8 to 0.2 (about 19:12 to 04:48; ADR-023). By day the barrow is empty; at dusk the ghouls rise at their markers, and at dawn they go once they're out of a fight (a fight that runs past dawn is finished first; a corpse with loot waits to be looted).

**Balance:** a level-1 player in the Iron set with the Shortsword beats Hakon alone, or one pair of raiders, wolves or ghouls. Three at once is meant to be too much.

## Quest: Torvald's Helm (`data/quests/torvalds_helm.json`)
The Act 1 quest, in the journal (J) once started. Its triggers and lines name the spawn markers
Maren Holt (#12), Torvald (#13), Captain Brenna (#14) and Hakon the Red (#27); each name is unique
in the world (the game warns on load if one isn't).

| Stage | When | Journal / objectives |
|---|---|---|
| 0 | Not started; talking to Maren starts it at 10, talking to Torvald at 20 (holding an Iron Helm already: straight to 40) | — |
| 10 | Maren sent you to the smith | Shows "Speak with Torvald at his forge in Ravensford". Talking to Torvald → 20 (holding the helm: 40) |
| 20 | Torvald asks for the helm Hakon wears | Completes the talk; shows "Take the helm Hakon the Red wears". Holding an Iron Helm → 30; killing Hakon → 25 |
| 25 | Hakon is dead; take the helm from his body (he always drops it; if he respawned first, kill him again) | Looting it (any Iron Helm) → 30 |
| 30 | You have the helm | Completes "Take the helm"; shows "Bring the helm back to Torvald". Talking to Torvald while holding (or wearing) it → 40 |
| 40 | Torvald takes the helm and gives the **Iron Cuirass** (armor 12) he hid from the raiders | Completes the return; flag `ravensford_friend` = 1; quest complete. With no room for the cuirass (bags full and a chest piece worn) he waits until there is |

Dialogue reacting to it (quest lines, first passing wins, taking turns with the markers' own lines; with none passing those cycle):
- **Maren:** at 0 "start with Torvald" (the talk that starts the quest); at 10 where the forge is; from 20 where Greyfang Rise is; from 30 "take it to him"; done: Torvald sings at his anvil again.
- **Torvald:** at 0 the helm Hakon wears; at 10 "Maren sent you?"; at 20 where Hakon stands; at 25 "bring me my helm"; with the helm at 0, 10 or 30 "That's my mark on the brow!" (the hand-over), at 30 without it "find it"; done: wear the cuirass well.
- **Captain Brenna:** from 20 until done, the road to Hakon; while Hakon lies dead, word of his fall; with `ravensford_friend`, the gate is open to you.

The reward is the cuirass rather than gold (there's no currency) or the helm itself (that is
Torvald's): it is a piece of the Iron set the balance assumes, and nothing else in play drops one.

## Follow-ups
- Light from Ember blocks (done: ADR-019).
- A trinket slot for the Bone Charm (done: ADR-022, +2 spirit).
