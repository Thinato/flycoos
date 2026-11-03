#include "progress.h"

#include <algorithm>
#include <cmath>

#include "defs.h"

const SkillDef& skillDef(Skill s) { return defs().skills[size_t(s)]; }

int rank(const Progress& p, size_t talent) { return talent < p.talents.size() ? p.talents[talent] : 0; }

int pointsSpent(const Progress& p, Skill tree) {
    const std::vector<TalentDef>& ts = defs().talents;
    int n = 0;
    for (size_t t = 0; t < std::min(p.talents.size(), ts.size()); ++t)
        if (tree == Skill::Count || ts[t].tree == tree) n += p.talents[t];
    return n;
}

int addCharacterXp(Progress& p, int& level, int xp) {
    int gained = 0;
    p.xp = std::clamp(p.xp + std::max(xp, 0), 0, 1 << 30);
    for (; level < kMaxLevel && p.xp >= levelXpToNext(level); ++level, ++gained) p.xp -= levelXpToNext(level);
    if (level >= kMaxLevel) p.xp = 0;
    return gained;
}

int addSkillXp(Progress& p, int& level, Skill s, float xp) {
    const SkillDef& d = skillDef(s);
    SkillLevel& sk = p.skills[size_t(s)];
    if (!(xp > 0) || sk.level >= kMaxSkill) return 0;
    sk.xp = std::min(sk.xp + xp, 1e9f);
    int gained = 0;
    // The loader keeps the cost at least 1, so this ends.
    for (float cost; sk.level < kMaxSkill && sk.xp >= (cost = skillXpToNext(d.improveMult, d.improveOffset, sk.level)); ++gained) {
        sk.xp -= cost;
        addCharacterXp(p, level, characterXpForSkill(++sk.level));
    }
    if (sk.level >= kMaxSkill) sk.xp = 0;
    return gained;
}

bool canLearn(const Progress& p, int level, size_t talent, const char** why) {
    const std::vector<TalentDef>& ts = defs().talents;
    const char* no = nullptr;
    if (talent >= ts.size()) no = "no such talent";
    else if (rank(p, talent) >= ts[talent].maxRank) no = "at its highest rank";
    else if (pointsSpent(p, ts[talent].tree) < kPointsPerTier * ts[talent].tier) no = "needs more points spent in this tree";
    else if (int pre = ts[talent].prerequisite; pre >= 0 && rank(p, size_t(pre)) < ts[size_t(pre)].maxRank) no = "needs its prerequisite at its highest rank";
    else if (talentPoints(p, level) <= 0) no = "no talent point to spend (one per level)";
    if (why) *why = no;
    return !no;
}

bool learn(Progress& p, int level, size_t talent) {
    if (!canLearn(p, level, talent)) return false;
    if (p.talents.size() <= talent) p.talents.resize(talent + 1);
    ++p.talents[talent];
    return true;
}

bool replayTalents(Progress& p, int level) {
    std::vector<uint8_t> want = std::move(p.talents);
    p.talents.clear();
    const std::vector<TalentDef>& ts = defs().talents;
    for (int tier = 0; tier <= kMaxTier; ++tier)  // a prerequisite is in a lower tier, so it comes first
        for (size_t t = 0; t < want.size(); ++t)
            for (int r = 0; r < want[t] && (t >= ts.size() || ts[t].tier == tier); ++r)
                if (!learn(p, level, t)) {
                    respec(p);
                    return false;
                }
    return true;
}

void progressModifiers(const Progress& p, std::vector<Modifier>& out) {
    const Defs& d = defs();
    auto add = [&](const std::vector<Modifier>& mods, int times, const std::string& source) {
        for (Modifier m : mods) {
            m.value *= float(times);
            m.source = source.c_str();
            out.push_back(m);
        }
    };
    for (size_t s = 0; s < size_t(Skill::Count); ++s)
        if (int gained = p.skills[s].level - kMinSkill; gained > 0) add(d.skills[s].perLevel, gained, d.skills[s].name);
    for (size_t t = 0; t < std::min(p.talents.size(), d.talents.size()); ++t)
        if (p.talents[t]) add(d.talents[t].perRank, std::min(int(p.talents[t]), d.talents[t].maxRank), d.talents[t].name);
}
