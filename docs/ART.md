# Art direction: voxel colour and trees

How voxels get their colour. These rules hold for every block, model and future asset; the
renderer implements them (ADR-028, `kTones` in `src/world.h`, `albedoOf` in `shaders/world.metal`).

## The rules

1. **One colour per voxel.** A voxel shows exactly one colour on every face. Colour is chosen once
   per voxel cell and never blends across a face.
2. **Gradients, never random noise.** No per-voxel random brightness, speckle or white noise, ever.
   Colour inside a material changes only through smooth, low-frequency gradients that slowly turn
   one colour into another: neighbouring voxels of the same material differ only slightly (Rock
   excepted by design, see Stone vs Rock).
3. **Sharp edges between materials.** Where two materials meet, the colour changes at once. There is
   no blending across a material boundary.
4. **Soft patches inside a material.** Each material has a main colour and up to two variant colours.
   Patches of the variants fade in and out of the main colour, and the main colour is the most common
   one:
   - grass is mostly green, with dry yellow-green and lush dark-green patches;
   - wood is brown with darker brown spots;
   - stone and rock are grey with lighter and darker spots;
   - leaves, dirt, sand and snow follow the same pattern.
5. **Natural clear, man-made faint, some plain.** Natural materials get clear patches. Man-made
   materials get faint ones, like weathering. Water, light sources and model-only blocks are plain.
6. **Models are flat.** Characters, creatures, items and held gear have one flat colour per part,
   even where a part uses a world block such as Wood or Steel. Flat is decided per draw, not per block.
7. **Patch size follows the material.** Grass patches are large; spots in wood and stone are small.

## Three tones per material

Values are sRGB (the renderer linearises them). Main is `kPalette`; variants, size (voxels between
noise lattice points, roughly the patch size) and strength are `kTones`. Strength 0 is plain.

| Block | Main | Variant a | Variant b | Size | Strength |
|---|---|---|---|---|---|
| Grass | 0.36 0.62 0.24 green | 0.60 0.68 0.26 dry yellow-green | 0.20 0.46 0.17 lush dark green | 24 | 0.9 |
| Leaves (oak shade 2) | 0.20 0.48 0.18 | 0.38 0.60 0.20 yellow-green | 0.12 0.36 0.14 dark green | 12 | 0.8 |
| Leaf shades | kind colour × ramp | kind a × ramp | kind b × ramp | 12 | 0.8 |
| BirchBark | 0.86 0.84 0.78 white | 0.24 0.23 0.22 dark knots | 0.95 0.94 0.90 bright | 4 | 0.9 |
| PineBark | 0.36 0.22 0.14 | 0.26 0.15 0.09 | 0.48 0.32 0.20 | 5 | 0.8 |
| AraucariaBark | 0.40 0.33 0.28 grey-brown | 0.30 0.24 0.20 | 0.52 0.44 0.38 scaly plates | 4 | 0.8 |
| Dirt | 0.47 0.33 0.20 | 0.36 0.24 0.14 damp | 0.56 0.42 0.27 dry | 12 | 0.7 |
| Sand | 0.80 0.72 0.48 | 0.88 0.82 0.60 pale | 0.74 0.62 0.40 warm | 20 | 0.6 |
| Snow | 0.95 0.96 1.00 | 0.82 0.87 0.96 cool shadow | 1.00 1.00 1.00 pure white | 24 | 0.6 |
| Stone | 0.50 0.50 0.52 | 0.60 0.60 0.62 lighter | 0.41 0.41 0.44 darker | 10 | 1.0 |
| Rock | 0.52 0.50 0.47 | 0.66 0.64 0.60 lighter | 0.39 0.37 0.34 darker | 5 | 1.0 |
| Wood | 0.42 0.28 0.15 | 0.30 0.19 0.10 dark spots | 0.50 0.35 0.20 lighter | 5 | 0.8 |
| Planks | 0.66 0.48 0.29 | 0.56 0.40 0.24 darker | 0.70 0.56 0.38 faded | 6 | 0.3 |
| DarkPlanks | 0.50 0.33 0.20 | 0.40 0.26 0.16 | 0.55 0.40 0.28 | 6 | 0.3 |
| Brick | 0.62 0.31 0.23 | 0.50 0.25 0.19 | 0.66 0.42 0.34 | 5 | 0.3 |
| Cobble | 0.45 0.44 0.43 | 0.36 0.36 0.36 | 0.54 0.53 0.51 | 5 | 0.3 |
| MossyCobble | 0.40 0.47 0.33 | 0.30 0.40 0.23 mossier | 0.46 0.48 0.42 bare stone | 6 | 0.35 |
| Thatch | 0.56 0.45 0.26 | 0.46 0.36 0.20 | 0.64 0.54 0.34 | 8 | 0.3 |
| Hay | 0.90 0.72 0.30 | 0.80 0.62 0.24 | 0.94 0.80 0.44 | 6 | 0.25 |
| Clay | 0.74 0.52 0.38 | 0.64 0.44 0.32 | 0.78 0.60 0.47 | 8 | 0.25 |
| Slate | 0.35 0.37 0.42 | 0.28 0.30 0.35 | 0.42 0.44 0.48 | 8 | 0.25 |

**Stone vs Rock.** Stone is the ground (terrain, bare-rock patches, mountains, subsoil): its
patches fade softly, at most ~26% brightness between neighbouring voxels (p99 ~13%). Rock is the
crisp material for pillars, spires and boulders: smaller, higher-contrast spots. When building by
hand, pick Rock for crisp spotted stone and Stone for soft ground; per-block tones are tunable in
the Render tab.

**Plain** (strength 0, exactly the palette colour): Air, Water, Ember, Lantern, and the model-only
blocks Skin, Tunic, Belt, Boots, Hair, Eye, Robe, DarkHair, Straw, Burlap, Steel, Iron, Hide, Tusk,
Snout, Hood, Leather, Plate.

A new block picks a class: natural (strength 0.6–1; the tree barks and every leaf shade are
natural), man-made (0.25–0.35, always below every natural one) or plain (0). `core_test`
(`tones()`) checks the classes.

## Where patches go

Placement is smooth random noise, the same everywhere: two independent gradient-noise (Perlin)
fields per block, on a lattice `size` voxels apart, read once at each voxel's cell centre. A
smoothstep turns the noise into round islands of variant colour that fade out into the main colour,
stepping one shade per voxel (Rock's small spots step harder, see Stone vs Rock). The main colour
is the most common one: about 25% of a material is pure main
colour, and about 45% (those included) is within a 20% mix of it. Value noise is not used: its trilinear blend
makes axis-aligned bands that read as stripes once each voxel takes one colour. Placement is not
terrain-aware (no height, water or shade rules); that is possible later.

## Tuning

Debug window (`.`), Render tab, "Colour patches": global strength (0 = the flat palette) and size,
and per block its two variants, size and strength, live. Edits are runtime only: copy good values
into `kTones` and this table. `--patches S` sets the global strength for screenshots.

## Trees

Trees are big, stylised fantasy trees, not realistic ones: thick trunks, visible limbs, crowns of
rounded clumps (ADR-029; `src/trees.cpp`). The player is 1.8 blocks tall.

| Species (prefab) | Full grown | Trunk | Crown | Bark | Leaves |
|---|---|---|---|---|---|
| Oak (`tree`) | 20–28 | 4–5, flared roots | 20–26 across, broad, 3–5 limbs | Wood | warm green |
| Maple (`maple`) | 20–28 | 4–5, flared roots | 20–26, rounder, steeper limbs | Wood | red, orange or gold per tree |
| Birch (`birch`) | 18–24 | 2–3, slight lean | 12–16, oval, no limbs | BirchBark, white with dark knots | light yellow-green |
| Conifer (`pine`) | 26–38 | 3 | 12–16, stacked tiers to a tip | PineBark | dark blue-green |
| Araucária (`araucaria`) | 32–44 | 3–4, bare for most of its height | 22–28, flat candelabra of tufts on 2–3 whorls of upturned limbs | AraucariaBark | deep green |

The rules:

1. **Thick and massive.** Trunk width grows with height. Oak, maple and Araucária flare at the foot
   with buttress roots, so they stand planted.
2. **Branches where real trees have thick ones.** Oak and maple limbs leave the trunk low, run out,
   then bend up into a clump, so the elbow shows under the crown. Araucária limbs reach out level
   and turn up at the tip. Birch and conifer have none.
3. **Crowns are clumps with smooth edges.** Each crown is a few big ellipsoids, their surfaces
   pushed by smooth (OpenSimplex) noise. No per-voxel random shape: no stray single leaves and no
   holes (`jitter` is for rocks only). A leaf with no solid face neighbour is removed.
4. **Leaves shade by height in their crown.** Four shades per leaf kind, from the crown's lowest
   leaves (shade 0, darkest) to its top (shade 3, lightest); every voxel of a row of one tree has the
   same shade. A shade is the kind's colour (and its patch tones) times `kLeafRamp`
   {0.55, 0.78, 1.0, 1.25}; shade 2 is the kind's own colour, and oak's shade 2 is the old `Leaves`
   block, so older worlds keep their look. The Render tab's "Leaf shade" slider scales the steps
   (0: one colour per kind). Colour patches still play on top, per rule 4 of the colour rules.
5. **Variety by size, not by noise.** Each tree's height is drawn from its species' range, and a
   quarter of generated trees are young (55–85% of full grown).

Where they grow (the generator and `replant`): lowland forests mix oak (45%), maple (25%) and
birch (30%); conifers take high ground (above 70) and rocky land; Araucárias stand in groves on
high, flat ground (48 and up, the ground within 2 of level over 4 columns); a rare lone oak grows on
the plains. Anything can be placed anywhere by hand as a prefab.

## Visual reference

The colourful, painterly voxel look of Cube World is a visual reference for the mood: saturated,
readable colours, never muddy. We take the feel only; no names, assets, palettes or designs.
