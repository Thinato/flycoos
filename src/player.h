#pragma once
#include <array>

#include <glm/glm.hpp>

#include "world.h"

// Player AABB: feet-centered, kHalfWidth on x/z, kHeight tall.
constexpr float kHalfWidth = 0.35f;
constexpr float kHeight = 1.8f;

struct Input {
    glm::vec2 move{};  // world-space XZ direction, length <= 1
    bool jump = false, run = false;
};

struct Player {
    glm::vec3 pos{}, vel{};  // pos = feet center
    float yaw = 0;           // radians; 0 faces +Z
    bool onGround = false, inWater = false;
    float gaitPhase = 0;     // advances with distance walked
    float gaitAmount = 0;    // 0 idle .. ~1.4 running
    float airAmount = 0;     // 0 grounded .. 1 airborne
    float breathPhase = 0;  // idle breathing cycle, radians
};

bool overlapsSolid(const Grid& g, glm::vec3 feet);
void updatePlayer(Player& p, const Grid& g, const Input& in, float dt);

// Character model: 6 floating voxel parts, each its own tiny grid meshed by meshRegion.
enum Part { Head, Torso, HandL, HandR, FootL, FootR, PartCount };
Grid buildPart(Part part);
// Part-grid space -> world space for the current animation pose.
std::array<glm::mat4, PartCount> posePlayer(const Player& p);
