#pragma once
#include <array>

#include <glm/glm.hpp>

#include "item.h"
#include "world.h"

struct Player;

// Character models: 6 floating voxel parts, each its own tiny grid meshed by meshRegion.
// The training dummy reuses the slots: FootL is its post, FootR its base, hands its arms.
enum Part { Head, Torso, HandL, HandR, FootL, FootR, PartCount };
// The boar and the wolf reuse them as quadrupeds: hands are front legs, feet hind legs.
// Weapons and shields are held items: only their Head slot has voxels, posed by poseModel's `held`
// and `shield` outputs.
// Armor is a humanoid whose parts are plated: a worn piece swaps in the part it covers.
// The guard, the bandit chief and the ghoul are humanoids (the chief scaled up, the ghoul thinner).
enum Model { ModelPlayer, ModelVillager, ModelDummy, ModelBoar, ModelBandit, ModelSword, ModelDagger, ModelIronArmor,
             ModelWolf, ModelGuard, ModelChief, ModelGhoul, ModelBuckler, ModelHeater, ModelTower, ModelAxe, ModelMace,
             ModelFlask, ModelFlaskEmpty,  // the healing flask (ADR-047), held while drinking
             ModelGreatsword, ModelGreataxe,  // great weapons (ADR-049), held in both hands
             ModelBow, ModelBowDrawn, ModelArrow,  // a bow (its string drawn back) and its arrow (ADR-050)
             ModelCrossbow, ModelCrossbowSpanned, ModelBolt,  // a crossbow (spanned: loaded, or the reload past its seat) and its bolt (ADR-052)
             ModelThrowingKnife, ModelBomb, ModelPoisonPot,   // throwables (ADR-053), in the left hand and in flight
             ModelBurst, ModelPoisonBurst, ModelCount };      // a bomb's and a pot's burst, a puff that swells and goes
Grid buildPart(Model model, Part part);
Model heldModel(ItemId weapon);
// What `p` has in hand drawn as: heldModel, but a bow with its string well back is ModelBowDrawn (ADR-050),
// a crossbow with a bolt in it ModelCrossbowSpanned (ADR-052).
Model heldModel(const Player& p);
// An arrow's pose (ADR-050) with its tip at `tip`, pointing along `dir`.
glm::mat4 arrowPose(glm::vec3 tip, glm::vec3 dir);
// A throwable's model (ADR-053), by its look; a burst's, by what burst.
Model throwableModel(ItemId item);
Model burstModel(ItemId item);
// A thrown bomb or pot's pose: its middle at `at`, tumbling by `spin` radians (a knife flies as an arrow: arrowPose).
glm::mat4 tumblePose(glm::vec3 at, float spin);
// A burst's puff (ADR-053) at `at`: swells to `radius` across most of its `u` (0 .. 1 of its time), then shrinks away.
glm::mat4 burstPose(glm::vec3 at, float radius, float u);
Model shieldModel(ItemId shield);
// How tall a body of `model` stands, in blocks (its hit box: hitBox).
constexpr float bodyHeight(Model m) { return m == ModelBoar || m == ModelWolf ? 1.1f : m == ModelChief ? 2.0f : 1.8f; }
// Whether `model` wearing `inv` has plate over its chest (an iron cuirass, the guard's): hits on it spark (ADR-058).
bool plated(Model model, const Inventory& inv);
// A held weapon's edge in its part grid (ADR-058): the line from `base` to `tip` its swings sweep; none (false)
// for what has no blade (fists, bows, crossbows, throwables).
bool bladeEdge(Model held, glm::vec3& base, glm::vec3& tip);  // an off-hand shield's, by its size (ADR-036)
// Which model each part of `model` is drawn from: its own, or the armor worn over it. Greaves have
// no part (the rig has no legs), so they don't show.
std::array<Model, PartCount> partModels(Model model, const Inventory& inv);
// Part-grid space -> world space for the body's current animation pose (the dummy only wobbles
// when hit and topples when out of health; everyone else falls over). With a main-hand weapon
// equipped, a humanoid's `held` gets its pose (gripped in the right hand) and attacks are slashes
// instead of punches; the boar and the wolf lunge with their heads.
// `alpha`: where between its last two ticks the body is drawn (see drawPos). With a shield in the off
// hand, `shield` gets its pose (strapped to the left forearm: at the side, raised by the guard, swept
// out by a parry). Drinking (ADR-047), `flask` gets the flask's pose (in the left hand, tipped to the lips);
// throwing (ADR-053), the throwable's in the left hand until it lets go (draw it as throwableModel(p.throwing)).
// A bow (ADR-050) is held in the left hand: down at the side, raised to aim or shoot with the right hand
// drawing the string back; `arrow` gets the nocked arrow's pose while there is one (a draw, aiming).
// A crossbow (ADR-052) is held in both hands: low at the right hip at rest, its butt to the right shoulder
// to aim or shoot, tipped nose down in front to reload (the right hand drawing the string back to the
// latch); `arrow` gets the bolt's pose (draw it as ModelBolt) while one is in it.
std::array<glm::mat4, PartCount> poseModel(const Player& body, Model model, glm::mat4* held = nullptr, float alpha = 1,
                                           glm::mat4* shield = nullptr, glm::mat4* flask = nullptr, glm::mat4* arrow = nullptr);
