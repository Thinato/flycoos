#include "hud.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

namespace {

constexpr float kNameplateRange = 40;  // blocks from the camera

// The swapchain is linear (sRGB format), so sRGB colors are linearized like the debug UI style.
ImU32 col(float r, float g, float b, float a = 1) {
    return ImGui::GetColorU32(ImVec4(std::pow(r, 2.2f), std::pow(g, 2.2f), std::pow(b, 2.2f), a));
}

void shadowText(ImDrawList* dl, ImVec2 pos, ImU32 color, float alpha, const char* text) {
    dl->AddText(ImVec2(pos.x + 1, pos.y + 1), col(0, 0, 0, 0.8f * alpha), text);
    dl->AddText(pos, color, text);
}

void bar(ImDrawList* dl, ImVec2 center, ImVec2 size, float fraction, ImU32 fill, float alpha) {
    ImVec2 lo(center.x - size.x / 2, center.y - size.y / 2), hi(center.x + size.x / 2, center.y + size.y / 2);
    dl->AddRectFilled(lo, hi, col(0.05f, 0.05f, 0.05f, 0.75f * alpha), 3);
    if (fraction > 0) dl->AddRectFilled(lo, ImVec2(lo.x + size.x * fraction, hi.y), fill, 3);
    dl->AddRect(lo, hi, col(0, 0, 0, alpha), 3);
}

// Rounded balloon with a tail pointing down at `anchor`.
void balloon(ImDrawList* dl, ImVec2 anchor, const char* text, float alpha) {
    constexpr float kWrap = 280, kPad = 9, kTail = 9;
    ImFont* font = ImGui::GetFont();
    float size = ImGui::GetFontSize();
    ImVec2 t = font->CalcTextSizeA(size, FLT_MAX, kWrap, text);
    ImVec2 hi(anchor.x + t.x / 2 + kPad, anchor.y - kTail), lo(anchor.x - t.x / 2 - kPad, hi.y - t.y - 2 * kPad);
    ImU32 paper = col(0.98f, 0.96f, 0.90f, 0.95f * alpha), ink = col(0.12f, 0.10f, 0.08f, alpha);
    dl->AddRectFilled(lo, hi, paper, 8);
    dl->AddTriangleFilled(ImVec2(anchor.x - kTail * 0.7f, hi.y), ImVec2(anchor.x + kTail * 0.7f, hi.y), anchor, paper);
    dl->AddText(font, size, ImVec2(lo.x + kPad, lo.y + kPad), ink, text, nullptr, kWrap);
}

void hint(ImDrawList* dl, ImVec2 p, float alpha) {
    const char* text = "[E] Talk";
    ImVec2 size = ImGui::CalcTextSize(text);
    shadowText(dl, ImVec2(p.x - size.x / 2, p.y + 8), col(1, 1, 1, alpha), alpha, text);
}

}  // namespace

void drawHud(const glm::mat4& view, float fovY, glm::vec3 eye, const Grid& world, const Player& player,
             const std::vector<Entity>& entities, const Entity* talkTarget) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    if (screen.x <= 0 || screen.y <= 0) return;
    glm::mat4 viewProj = glm::perspectiveRH_ZO(fovY, screen.x / screen.y, 0.1f, 1000.0f) * view;
    float line = ImGui::GetFontSize();

    for (const Entity& e : entities) {
        glm::vec3 head = e.body.pos + glm::vec3(0, 2.5f, 0);
        glm::vec3 toHead = head - eye;
        float dist = glm::length(toHead);
        glm::vec4 clip = viewProj * glm::vec4(head, 1);
        if (clip.w < 0.1f) continue;  // behind the camera
        ImVec2 p((clip.x / clip.w * 0.5f + 0.5f) * screen.x, (0.5f - clip.y / clip.w * 0.5f) * screen.y);
        if (e.body.hitTime < 1) {  // floating damage number: pops big, rises and fades over a second
            float t = e.body.hitTime;
            glm::vec4 c = viewProj * glm::vec4(head + glm::vec3(0, 0.4f + 1.2f * t, 0), 1);
            if (c.w < 0.1f) c = clip;  // its rising point is behind the camera
            char num[16];
            std::snprintf(num, sizeof num, "%d", e.body.lastDamage);
            float size = line * (2.2f - 0.8f * std::min(t * 4, 1.0f)), alpha = 1 - t * t;
            ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, num);
            ImVec2 q((c.x / c.w * 0.5f + 0.5f) * screen.x - ts.x / 2, (0.5f - c.y / c.w * 0.5f) * screen.y - ts.y / 2);
            dl->AddText(ImGui::GetFont(), size, ImVec2(q.x + 2, q.y + 2), col(0, 0, 0, 0.8f * alpha), num);
            dl->AddText(ImGui::GetFont(), size, q, col(1, 0.85f, 0.3f, alpha), num);
        }
        // Speech and the talk hint always show; nameplates only nearby and in line of sight (same
        // box march the camera uses, with a thin box).
        const char* speech = e.speech();
        if (speech) balloon(dl, ImVec2(p.x, p.y - line - 10), speech, std::min(1.0f, e.speechTimer / 0.4f));
        if (dist > kNameplateRange || cameraClearance(world, eye, toHead / dist, dist, 0.05f) < dist - 0.5f) {
            if (!speech && &e == talkTarget) hint(dl, p, 1);
            continue;
        }
        float alpha = std::clamp((kNameplateRange - dist) / 8, 0.0f, 1.0f);  // fade out at range

        const EntityType& t = e.type();
        char label[96];
        std::snprintf(label, sizeof label, "Lv %d  %s", e.level, e.name.c_str());
        ImVec2 size = ImGui::CalcTextSize(label);
        // Attackable entities get an amber name and red health, friendly ones green.
        ImU32 nameColor = t.attackable ? col(1, 0.78f, 0.35f, alpha) : col(0.6f, 1, 0.6f, alpha);
        ImU32 healthColor = t.attackable ? col(0.85f, 0.2f, 0.15f, alpha) : col(0.3f, 0.8f, 0.3f, alpha);
        shadowText(dl, ImVec2(p.x - size.x / 2, p.y - line - 6), nameColor, alpha, label);
        bar(dl, p, ImVec2(90, 7), e.body.health / float(std::max(1, maxHealth(e.body.stats))), healthColor, alpha);
        if (!speech && &e == talkTarget) hint(dl, p, alpha);
    }

    // Player health, bottom center.
    ImVec2 center(screen.x / 2, screen.y - 40);
    bar(dl, center, ImVec2(280, 16), player.health / float(std::max(1, maxHealth(player.stats))),
        col(0.8f, 0.15f, 0.12f), 1);
    char hp[32];
    std::snprintf(hp, sizeof hp, "%d / %d", int(std::ceil(player.health)), maxHealth(player.stats));
    ImVec2 hs = ImGui::CalcTextSize(hp);
    shadowText(dl, ImVec2(center.x - hs.x / 2, center.y - hs.y / 2), col(1, 1, 1), 1, hp);
}
