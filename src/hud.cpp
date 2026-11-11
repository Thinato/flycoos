#include "hud.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include "inventory_ui.h"
#include "lockon.h"
#include "melee.h"
#include "projectile.h"

// The swapchain is linear (sRGB format), so sRGB colors are linearized like the debug UI style.
unsigned uiColor(float r, float g, float b, float a) {
    return ImGui::GetColorU32(ImVec4(std::pow(r, 2.2f), std::pow(g, 2.2f), std::pow(b, 2.2f), a));
}

namespace {

constexpr float kNameplateRange = 40;  // blocks from the camera
constexpr float kRegionSeconds = 4;    // how long a region's name shows after entering it

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
    else if (e.loot != NoItem) std::snprintf(text, sizeof text, "[E] Loot %s", itemType(e.loot).name);
    else return;
    ImVec2 size = ImGui::CalcTextSize(text);
    shadowText(dl, ImVec2(p.x - size.x / 2, p.y + 8), uiColor(1, 1, 1, alpha), alpha, text);
}

// The status colors (ADR-046): bleed crimson (apart from health's red), poison green, frost ice blue
// (deep enough to read against the sky).
constexpr glm::vec3 kStatusRgb[] = {{0.9f, 0.1f, 0.4f}, {0.45f, 0.85f, 0.2f}, {0.3f, 0.65f, 1}};
static_assert(std::size(kStatusRgb) == size_t(Status::Count));

// Damage numbers over `head`: each pops big, rises and fades over a second. A critical hit's (ADR-045)
// is bigger, orange and marked "!"; a status's (ADR-046) is in its color, beside the hit's: a bleed or
// frostbite burst big, poison's ticks small.
void damageNumbers(ImDrawList* dl, const glm::mat4& viewProj, ImVec2 screen, glm::vec3 head, const Player& b, glm::vec3 hit) {
    for (const DamageNumber& n : b.numbers) {
        float t = n.age;
        if (t >= 1) continue;
        glm::vec4 c = viewProj * glm::vec4(head + glm::vec3(0, 0.4f + 1.2f * t, 0), 1);
        if (c.w < 0.1f) continue;  // behind the camera
        bool status = n.kind != Status::Count;
        char num[16];
        std::snprintf(num, sizeof num, n.crit || n.kind == Status::Bleed ? "%d!" : "%d", n.amount);
        glm::vec3 rgb = status ? kStatusRgb[size_t(n.kind)] : n.crit ? glm::vec3(1, 0.5f, 0.1f) : hit;
        float scale = n.crit ? 1.5f : n.kind == Status::Poison ? 0.75f : status ? 1.3f : 1;
        float size = ImGui::GetFontSize() * (2.2f - 0.8f * std::min(t * 4, 1.0f)) * scale, alpha = 1 - t * t;
        ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, num);
        // Beside the hit's number (which is about 1.3 lines wide): bleed and poison right of it, frost left.
        float gap = ImGui::GetFontSize() * 1.6f, side = !status ? 0 : n.kind == Status::Frost ? -gap - ts.x / 2 : gap + ts.x / 2;
        ImVec2 q((c.x / c.w * 0.5f + 0.5f) * screen.x - ts.x / 2 + side, (0.5f - c.y / c.w * 0.5f) * screen.y - ts.y / 2);
        dl->AddText(ImGui::GetFont(), size, ImVec2(q.x + 2, q.y + 2), uiColor(0, 0, 0, 0.8f * alpha), num);
        dl->AddText(ImGui::GetFont(), size, q, uiColor(rgb.r, rgb.g, rgb.b, alpha), num);
    }
}

// A bar per status building up on `b` or on it (ADR-046), from `at` on, `step` apart: build-up toward
// its resist, or the time its effect has left (brighter). `label`: its name on the left.
void statusBars(ImDrawList* dl, ImVec2 at, ImVec2 size, float step, const Player& b, float alpha, bool label) {
    StatSheet s = sheet(b);
    for (size_t i = 0; i < size_t(Status::Count); ++i) {
        const TimedEffect* on = i == size_t(Status::Poison) ? b.effect(Effect::Poisoned)
                              : i == size_t(Status::Frost)  ? b.effect(Effect::Frostbitten)
                                                            : nullptr;
        float resist = s[resistStat(Status(i))], fill = on ? on->seconds / std::max(on->length, 1e-3f) : resist > 0 ? b.buildup[i] / resist : 0;
        if (fill <= 0) continue;
        glm::vec3 c = kStatusRgb[i] * (on ? 1.0f : 0.7f);
        bar(dl, at, size, std::min(fill, 1.0f), uiColor(c.r, c.g, c.b, alpha), alpha);
        if (label) {
            const char* name = on ? kEffectNames[size_t(on->kind)] : kStatNames[size_t(buildupStat(Status(i)))];
            ImVec2 ts = ImGui::CalcTextSize(name);
            shadowText(dl, ImVec2(at.x - size.x / 2 - ts.x - 8, at.y - ts.y / 2), uiColor(c.r, c.g, c.b, alpha), alpha, name);
        }
        at.y += step;
    }
}

// The healing flask (ADR-047) at `c`: a round-bellied bottle, red with charges left, else empty glass,
// and how many.
void flaskIcon(ImDrawList* dl, ImVec2 c, int charges) {
    ImU32 glass = uiColor(0.85f, 0.9f, 0.95f, 0.9f), ink = uiColor(0, 0, 0, 0.9f);
    dl->AddRectFilled(ImVec2(c.x - 3, c.y - 15), ImVec2(c.x + 3, c.y - 5), glass, 1);  // the neck
    dl->AddRectFilled(ImVec2(c.x - 4, c.y - 19), ImVec2(c.x + 4, c.y - 15), uiColor(0.55f, 0.35f, 0.2f));  // the cork
    dl->AddCircleFilled(ImVec2(c.x, c.y + 3), 10, ink);
    dl->AddCircleFilled(ImVec2(c.x, c.y + 3), 8.5f, charges > 0 ? uiColor(0.85f, 0.12f, 0.15f) : uiColor(0.3f, 0.32f, 0.35f, 0.9f));
    dl->AddCircleFilled(ImVec2(c.x - 3, c.y), 2, uiColor(1, 1, 1, 0.6f));  // a glint
    char n[8];
    std::snprintf(n, sizeof n, "%d", charges);
    shadowText(dl, ImVec2(c.x + 13, c.y - ImGui::GetFontSize() / 2 + 3), charges > 0 ? uiColor(1, 1, 1) : uiColor(0.6f, 0.6f, 0.6f), 1, n);
}

// Health's color: red, green while poisoned (ADR-046).
glm::vec3 healthRgb(const Player& b) { return b.effect(Effect::Poisoned) ? glm::vec3(0.35f, 0.6f, 0.12f) : glm::vec3(0.8f, 0.15f, 0.12f); }

}  // namespace

void drawHud(const glm::mat4& view, float fovY, glm::vec3 eye, const Grid& world, const Player& player,
             const std::vector<Entity>& entities, const Entity* target, float tickAlpha, const Entity* locked, bool rest) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    if (screen.x <= 0 || screen.y <= 0) return;
    glm::mat4 viewProj = glm::perspectiveRH_ZO(fovY, screen.x / screen.y, 0.1f, 1000.0f) * view;
    float line = ImGui::GetFontSize();

    for (const Entity& e : entities) {
        Model model = e.type().model;
        float top = model == ModelBoar ? 1.6f : model == ModelWolf ? 1.9f : model == ModelChief ? 2.8f : 2.5f;
        glm::vec3 head = drawPos(e.body, tickAlpha) + glm::vec3(0, top, 0);  // above the model
        glm::vec3 toHead = head - eye;
        float dist = glm::length(toHead);
        glm::vec4 clip = viewProj * glm::vec4(head, 1);
        if (clip.w < 0.1f) continue;  // behind the camera
        ImVec2 p((clip.x / clip.w * 0.5f + 0.5f) * screen.x, (0.5f - clip.y / clip.w * 0.5f) * screen.y);
        damageNumbers(dl, viewProj, screen, head, e.body, {1, 0.85f, 0.3f});
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
        std::snprintf(label, sizeof label, "Lv %d  %s", e.body.level, e.displayName().c_str());
        ImVec2 size = ImGui::CalcTextSize(label);
        // Names: red attacks on sight, amber can be attacked, green friendly. Attackable ones have red health.
        ImU32 nameColor = t.temper == Temper::Aggressive ? uiColor(1, 0.35f, 0.3f, alpha)
                        : t.attackable                   ? uiColor(1, 0.78f, 0.35f, alpha)
                                                         : uiColor(0.6f, 1, 0.6f, alpha);
        glm::vec3 hc = t.attackable ? healthRgb(e.body) : glm::vec3(0.3f, 0.8f, 0.3f);
        ImU32 healthColor = uiColor(hc.r, hc.g, hc.b, alpha);
        shadowText(dl, ImVec2(p.x - size.x / 2, p.y - line - 6), nameColor, alpha, label);
        // Health: always for the aggressive, for the rest (friendly, passive) only in combat.
        if (t.temper == Temper::Aggressive || e.angry() || e.body.combatTimer > 0)
            bar(dl, p, ImVec2(90, 7), e.body.health / float(maxHealth(sheet(e.body))), healthColor, alpha);
        statusBars(dl, ImVec2(p.x, p.y + 8), ImVec2(90, 4), 6, e.body, alpha, false);  // under it
        if (!speech && &e == target) hint(dl, p, alpha, e);
    }

    if (locked) {  // the lock-on dot: white with a dark rim, on the chest
        glm::vec4 c = viewProj * glm::vec4(drawPos(locked->body, tickAlpha) + (lockPoint(*locked) - locked->body.pos), 1);
        if (c.w > 0.1f) {
            ImVec2 p((c.x / c.w * 0.5f + 0.5f) * screen.x, (0.5f - c.y / c.w * 0.5f) * screen.y);
            dl->AddCircleFilled(p, 7, uiColor(0, 0, 0, 0.55f));
            dl->AddCircleFilled(p, 4.5f, uiColor(1, 1, 1, 0.95f));
        }
    }
    damageNumbers(dl, viewProj, screen, drawPos(player, tickAlpha) + glm::vec3(0, 2.2f, 0), player, {1, 0.3f, 0.25f});
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
    int most = maxHealth(sheet(player));
    glm::vec3 hc = healthRgb(player);
    bar(dl, center, ImVec2(280, 16), player.health / float(most), uiColor(hc.r, hc.g, hc.b), 1);
    statusBars(dl, ImVec2(center.x, center.y - 18), ImVec2(160, 6), -10, player, 1, true);  // over it, up
    char hp[32];
    std::snprintf(hp, sizeof hp, "%d / %d", int(std::ceil(player.health)), most);
    ImVec2 hs = ImGui::CalcTextSize(hp);
    shadowText(dl, ImVec2(center.x - hs.x / 2, center.y - hs.y / 2), uiColor(1, 1, 1), 1, hp);
    flaskIcon(dl, ImVec2(center.x - 140 - 34, center.y + 4), player.flasks);  // left of the bars
    if (player.readied) {  // the readied throwable (ADR-053), left of the flask, and how many
        ImVec2 c(center.x - 140 - 84, center.y + 4);
        int n = player.pouch[player.readied];
        itemIcon(dl, player.readied, ImVec2(c.x - 12, c.y - 12), 24);
        if (n == 0) dl->AddRectFilled(ImVec2(c.x - 12, c.y - 12), ImVec2(c.x + 12, c.y + 12), uiColor(0, 0, 0, 0.5f), 3);
        char t[8];
        std::snprintf(t, sizeof t, "%d", n);
        shadowText(dl, ImVec2(c.x + 13, c.y - ImGui::GetFontSize() / 2 + 3), n > 0 ? uiColor(1, 1, 1) : uiColor(0.6f, 0.6f, 0.6f), 1, t);
    }
    if (ranged(player)) {  // a bow (ADR-050): right of the bars, an arrow and the quiver's count
        ImVec2 c(center.x + 140 + 16, center.y + 4);
        ImU32 shaft = uiColor(0.75f, 0.6f, 0.4f), ink = uiColor(0, 0, 0, 0.9f), head = uiColor(0.8f, 0.82f, 0.85f);
        dl->AddLine(ImVec2(c.x - 9, c.y + 9), ImVec2(c.x + 7, c.y - 7), ink, 4);
        dl->AddLine(ImVec2(c.x - 9, c.y + 9), ImVec2(c.x + 7, c.y - 7), shaft, 2);
        dl->AddTriangleFilled(ImVec2(c.x + 10, c.y - 10), ImVec2(c.x + 3, c.y - 7), ImVec2(c.x + 7, c.y - 3), head);
        dl->AddLine(ImVec2(c.x - 10, c.y + 5), ImVec2(c.x - 7, c.y + 8), uiColor(0.8f, 0.2f, 0.2f), 3);  // fletching
        char n[8];
        std::snprintf(n, sizeof n, "%d", player.arrows);
        shadowText(dl, ImVec2(c.x + 14, c.y - ImGui::GetFontSize() / 2), player.arrows > 0 ? uiColor(1, 1, 1) : uiColor(0.6f, 0.6f, 0.6f), 1, n);
        if (loads(player)) {  // a crossbow (ADR-052): under the arrow, a bar lit while a bolt is in it
            bool in = player.loaded == player.weapon();
            dl->AddRectFilled(ImVec2(c.x - 10, c.y + 13), ImVec2(c.x + 10, c.y + 17), ink, 2);
            dl->AddRectFilled(ImVec2(c.x - 9, c.y + 14), ImVec2(c.x + 9, c.y + 16), in ? uiColor(1, 0.85f, 0.4f) : uiColor(0.3f, 0.3f, 0.3f), 1);
        }
    } else if (twoHanding(player)) {  // right of them, a tag: the weapon in both hands (ADR-048)
        const char* tag = "2H";
        ImVec2 ts = ImGui::CalcTextSize(tag), at(center.x + 140 + 12, center.y + 4 - ts.y / 2);
        dl->AddRectFilled(ImVec2(at.x - 4, at.y - 2), ImVec2(at.x + ts.x + 4, at.y + ts.y + 2), uiColor(0, 0, 0, 0.6f), 3);
        dl->AddRect(ImVec2(at.x - 4, at.y - 2), ImVec2(at.x + ts.x + 4, at.y + ts.y + 2), uiColor(0.95f, 0.8f, 0.45f, 0.9f), 3);
        shadowText(dl, at, uiColor(1, 0.9f, 0.6f), 1, tag);
    }
    if (rest && !target && player.alive()) {
        const char* text = "[E] Rest by the fire";
        ImVec2 ts = ImGui::CalcTextSize(text);
        shadowText(dl, ImVec2(screen.x / 2 - ts.x / 2, center.y - 80 - ts.y), uiColor(1, 0.85f, 0.5f), 1, text);  // over the status bars
    }
    // Aiming a bow (ADR-050): a crosshair at the middle of the screen, where the camera looks; it
    // closes in as the string comes back.
    if (float a = std::clamp(player.aimBlend * 2 - 1, 0.0f, 1.0f); a > 0 && player.alive()) {
        float gap = loads(player) ? (player.loaded == player.weapon() ? 4 : 12) : 9 - 5 * (player.move == Move::Shot ? drawOf(player) : 0), len = 6;  // a crossbow: tight loaded (ADR-052)
        ImVec2 m(screen.x / 2, screen.y / 2);
        for (int k = 0; k < 2; ++k) {  // a dark outline under a light cross
            ImU32 col = k ? uiColor(1, 1, 1, 0.9f * a) : uiColor(0, 0, 0, 0.6f * a);
            float w = k ? 1.5f : 3.5f;
            for (ImVec2 d : {ImVec2(1, 0), ImVec2(-1, 0), ImVec2(0, 1), ImVec2(0, -1)})
                dl->AddLine(ImVec2(m.x + d.x * gap, m.y + d.y * gap), ImVec2(m.x + d.x * (gap + len), m.y + d.y * (gap + len)), col, w);
            dl->AddCircleFilled(m, k ? 1.5f : 2.5f, col);
        }
    }
    // Stamina under it (ADR-035): what attacks, rolls and blocks spend.
    bar(dl, ImVec2(center.x, center.y + 15), ImVec2(280, 7), player.stamina / gMelee.staminaMax, uiColor(0.3f, 0.75f, 0.25f), 1);
}

void drawPlaces(const glm::mat4& view, float fovY, const Player& player, const std::vector<Marker>& markers,
                const Marker* region, float regionSeconds) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    if (screen.x <= 0 || screen.y <= 0) return;
    glm::mat4 viewProj = glm::perspectiveRH_ZO(fovY, screen.x / screen.y, 0.1f, 1000.0f) * view;
    for (const Marker& m : markers) {
        if (m.kind != MarkerKind::Poi) continue;
        float d = glm::distance(glm::vec2(player.pos.x, player.pos.z), glm::vec2(m.pos.x, m.pos.z));
        float alpha = std::clamp((m.radius - d) / std::max(2.0f, m.radius / 4), 0.0f, 1.0f);
        glm::vec4 clip = viewProj * glm::vec4(m.pos + glm::vec3(0, 3, 0), 1);
        if (alpha <= 0 || clip.w < 0.1f) continue;
        ImVec2 size = ImGui::CalcTextSize(m.name.c_str());
        ImVec2 p((clip.x / clip.w * 0.5f + 0.5f) * screen.x - size.x / 2, (0.5f - clip.y / clip.w * 0.5f) * screen.y - size.y);
        shadowText(dl, p, uiColor(1, 0.9f, 0.6f, alpha), alpha, m.name.c_str());
    }
    if (!region || regionSeconds >= kRegionSeconds) return;
    float alpha = std::min({1.0f, regionSeconds / 0.5f, kRegionSeconds - regionSeconds});  // fades in, then out over the last second
    ImFont* font = titleFont ? titleFont : ImGui::GetFont();
    float size = titleFont ? 48 : ImGui::GetFontSize() * 2, px = titleFont ? 3 : 2;  // 3 screen pixels per font pixel
    const char* name = region->name.c_str();
    ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, name);
    ImVec2 at(std::round(screen.x / 2 - ts.x / 2), std::round(screen.y * 0.16f));  // whole pixels keep it crisp
    dl->AddText(font, size, ImVec2(at.x + px, at.y + px), uiColor(0.1f, 0.05f, 0, 0.85f * alpha), name);
    dl->AddText(font, size, at, uiColor(1, 0.85f, 0.5f, alpha), name);
}

void drawNotice(const char* text, float age) {
    constexpr float kNoticeSeconds = 2.5f;
    if (!*text || age >= kNoticeSeconds) return;
    float alpha = std::min(1.0f, kNoticeSeconds - age);  // fades out over the last second
    ImVec2 screen = ImGui::GetIO().DisplaySize, size = ImGui::CalcTextSize(text);
    shadowText(ImGui::GetBackgroundDrawList(), ImVec2(screen.x / 2 - size.x / 2, screen.y * 0.08f), uiColor(1, 1, 1, alpha), alpha, text);
}
