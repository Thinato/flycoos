# Story: Act 1, "The Ravensford Vale"

A starter act on the seed-1337 world (`worlds/story.flyw`). It shows what the editor and game can do: sculpted terrain, painted roads, prefab instances, regions and points of interest, and spawn markers with named characters and dialogue lines. All names are original.

## Premise
After a long winter road the player reaches the Wayfarer's Shrine, at the edge of the Ravensford Vale.
- **Ravensford:** a frontier ford village. Its elder asks for help.
- **Hakon the Red:** leader of the bandits who hold the Old Watchtower on Greyfang Rise. They raid the road and stole the smith's iron.
- **The Greywood:** a wolf-filled forest.
- **The Barrow of King Ulfar:** someone broke its seal, and since then the dead walk at the treeline.

There's no quest system yet, so the story is carried by dialogue, region names and place labels.

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
| Barrow Ghouls | ghoul ×2 (#34 stone ring, #35 chamber) | Barrow | "Ulfar... sleeps... no more..." · "Leave... the king's... gold..." |

The village also has two generic villagers (#2), a training dummy (#1) and boars (#3, ×2): 28 NPCs in all.

**Balance:** a level-1 player in the Iron set with the Shortsword beats Hakon alone, or one pair of raiders, wolves or ghouls. Three at once is meant to be too much.

## Follow-ups
- A quest/journal system ("bring Hakon's helm to Torvald").
- Night-only spawns for the ghouls.
- Light from Ember blocks (done: ADR-019).
- A trinket slot for the Bone Charm (done: ADR-022, +2 spirit).
