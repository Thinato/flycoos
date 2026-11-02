#include "hud.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

// The swapchain is linear (sRGB format), so sRGB colors are linearized like the debug UI style.
unsigned uiColor(float r, float g, float b, float a) {
    return ImGui::GetColorU32(ImVec4(std::pow(r, 2.2f), std::pow(g, 2.2f), std::pow(b, 2.2f), a));
}

namespace {

constexpr float kNameplateRange = 40;  // blocks from the camera

void shadowText(ImDrawList* dl, ImVec2 pos, ImU32 color, float alpha, const char* text) {
    dl->AddText(ImVec2(pos.x + 1, pos.y + 1), uiColor(0, 0, 0, 0.8f * alpha), text);
    dl->AddText(pos, color, text);
}

void bar(ImDrawList* dl, ImVec2 center, ImVec2 size, float fraction, ImU32 fill, float alpha) {
    ImVec2 lo(center.x - size.x / 2, center.y - size.y / 2), hi(center.x + size.x / 2, center.y + size.y / 2);
    dl->AddRectFilled(lo, hi, uiColor(0.05f, 0.05f, 0.05f, 0.75f * alpha), 3);
    if (fraction > 0) dl->AddRectFilled(lo, ImVec2(lo.x + size.x * fraction, hi.y), fill, 3);
    dl->AddRect(lo, hi, uiColor(0, 0, 0, alpha), 3);
}

// Rounded balloon with a tail pointing down at `anchor`.
void balloon(ImDrawList* dl, ImVec2 anchor, const char* text, float alpha) {
    constexpr float kWrap = 280, kPad = 9, kTail = 9;
    ImFont* font = ImGui::GetFont();
    float size = ImGui::GetFontSize();
    ImVec2 t = font->CalcTextSizeA(size, FLT_MAX, kWrap, text);
    ImVec2 hi(anchor.x + t.x / 2 + kPad, anchor.y - kTail), lo(anchor.x - t.x / 2 - kPad, hi.y - t.y - 2 * kPad);
    ImU32 paper = uiColor(0.98f, 0.96f, 0.90f, 0.95f * alpha), ink = uiColor(0.12f, 0.10f, 0.08f, alpha);
    dl->AddRectFilled(lo, hi, paper, 8);
    dl->AddTriangleFilled(ImVec2(anchor.x - kTail * 0.7f, hi.y), ImVec2(anchor.x + kTail * 0.7f, hi.y), anchor, paper);
    dl->AddText(font, size, ImVec2(lo.x + kPad, lo.y + kPad), ink, text, nullptr, kWrap);
}

void hint(ImDrawList* dl, ImVec2 p, float alpha, const Entity& e) {
    char text[64];
    if (e.body.alive()) std::snprintf(text, sizeof text, "[E] Talk");
    else std::snprintf(text, sizeof text, "[E] Loot %s", itemType(e.loot).name);
    ImVec2 size = ImGui::CalcTextSize(text);
    shadowText(dl, ImVec2(p.x - size.x / 2, p.y + 8), uiColor(1, 1, 1, alpha), alpha, text);
}

// Damage number over `head`: pops big, rises and fades over a second.
void damageNumber(ImDrawList* dl, const glm::mat4& viewProj, ImVec2 screen, glm::vec3 head, const Player& b, glm::vec3 rgb) {
    float t = b.hitTime;
    if (t >= 1) return;
    glm::vec4 c = viewProj * glm::vec4(head + glm::vec3(0, 0.4f + 1.2f * t, 0), 1);
    if (c.w < 0.1f) return;  // behind the camera
    char num[16];
    std::snprintf(num, sizeof num, "%d", b.lastDamage);
    float size = ImGui::GetFontSize() * (2.2f - 0.8f * std::min(t * 4, 1.0f)), alpha = 1 - t * t;
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, num);
    ImVec2 q((c.x / c.w * 0.5f + 0.5f) * screen.x - ts.x / 2, (0.5f - c.y / c.w * 0.5f) * screen.y - ts.y / 2);
    dl->AddText(ImGui::GetFont(), size, ImVec2(q.x + 2, q.y + 2), uiColor(0, 0, 0, 0.8f * alpha), num);
    dl->AddText(ImGui::GetFont(), size, q, uiColor(rgb.r, rgb.g, rgb.b, alpha), num);
}

}  // namespace

void drawHud(const glm::mat4& view, float fovY, glm::vec3 eye, const Grid& world, const Player& player,
             const std::vector<Entity>& entities, const Entity* target) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    if (screen.x <= 0 || screen.y <= 0) return;
    glm::mat4 viewProj = glm::perspectiveRH_ZO(fovY, screen.x / screen.y, 0.1f, 1000.0f) * view;
    float line = ImGui::GetFontSize();

    for (const Entity& e : entities) {
        glm::vec3 head = e.body.pos + glm::vec3(0, e.type().model == ModelBoar ? 1.6f : 2.5f, 0);  // above the model
        glm::vec3 toHead = head - eye;
        float dist = glm::length(toHead);
        glm::vec4 clip = viewProj * glm::vec4(head, 1);
        if (clip.w < 0.1f) continue;  // behind the camera
        ImVec2 p((clip.x / clip.w * 0.5f + 0.5f) * screen.x, (0.5f - clip.y / clip.w * 0.5f) * screen.y);
        damageNumber(dl, viewProj, screen, head, e.body, {1, 0.85f, 0.3f});
        // Speech and the talk hint always show; nameplates only nearby and in line of sight (same
        // box march the camera uses, with a thin box).
        const char* speech = e.speech();
        if (speech) balloon(dl, ImVec2(p.x, p.y - line - 10), speech, std::min(1.0f, e.speechTimer / 0.4f));
        if (dist > kNameplateRange || cameraClearance(world, eye, toHead / dist, dist, 0.05f) < dist - 0.5f) {
            if (!speech && &e == target) hint(dl, p, 1, e);
            continue;
        }
        float alpha = std::clamp((kNameplateRange - dist) / 8, 0.0f, 1.0f);  // fade out at range

        const EntityType& t = e.type();
        char label[96];
        std::snprintf(label, sizeof label, "Lv %d  %s", e.level, e.name.c_str());
        ImVec2 size = ImGui::CalcTextSize(label);
        // Names: red attacks on sight, amber can be attacked, green friendly. Attackable ones have red health.
        ImU32 nameColor = t.temper == Temper::Aggressive ? uiColor(1, 0.35f, 0.3f, alpha)
                        : t.attackable                   ? uiColor(1, 0.78f, 0.35f, alpha)
                                                         : uiColor(0.6f, 1, 0.6f, alpha);
        ImU32 healthColor = t.attackable ? uiColor(0.85f, 0.2f, 0.15f, alpha) : uiColor(0.3f, 0.8f, 0.3f, alpha);
        shadowText(dl, ImVec2(p.x - size.x / 2, p.y - line - 6), nameColor, alpha, label);
        bar(dl, p, ImVec2(90, 7), e.body.health / float(std::max(1, maxHealth(e.body.stats))), healthColor, alpha);
        if (!speech && &e == target) hint(dl, p, alpha, e);
    }

    damageNumber(dl, viewProj, screen, player.pos + glm::vec3(0, 2.2f, 0), player, {1, 0.3f, 0.25f});
    if (!player.alive()) {
        char msg[64];
        std::snprintf(msg, sizeof msg, "Knocked out - back on your feet in %.0f s",
                      std::ceil(std::max(0.0f, kPlayerRespawnSeconds - player.deadTime)));
        float size = line * 1.6f;
        ImVec2 ms = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, msg);
        ImVec2 at(screen.x / 2 - ms.x / 2, screen.y * 0.35f);
        dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + 2, at.y + 2), uiColor(0, 0, 0, 0.8f), msg);
        dl->AddText(ImGui::GetFont(), size, at, uiColor(1, 0.4f, 0.35f), msg);
    }

    // Player health, bottom center.
    ImVec2 center(screen.x / 2, screen.y - 40);
    bar(dl, center, ImVec2(280, 16), player.health / float(std::max(1, maxHealth(player.stats))),
        uiColor(0.8f, 0.15f, 0.12f), 1);
    char hp[32];
    std::snprintf(hp, sizeof hp, "%d / %d", int(std::ceil(player.health)), maxHealth(player.stats));
    ImVec2 hs = ImGui::CalcTextSize(hp);
    shadowText(dl, ImVec2(center.x - hs.x / 2, center.y - hs.y / 2), uiColor(1, 1, 1), 1, hp);
}
