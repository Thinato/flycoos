#include "quest.h"

#include <algorithm>
#include <format>

#include "defs.h"
#include "session.h"

namespace {

constexpr int kMaxStageChain = 16;  // set_stage from a stage's own actions, this deep at most (a cycle stops here)

const Marker* markerById(const std::vector<Marker>& markers, uint32_t id) {
    auto it = std::ranges::lower_bound(markers, id, {}, &Marker::id);
    return it != markers.end() && it->id == id ? &*it : nullptr;
}
bool named(const std::vector<Marker>& markers, uint32_t id, const std::string& name) {
    const Marker* m = markerById(markers, id);
    return m && m->name == name;
}

bool compare(int a, Cmp op, int b) {
    switch (op) {
    case Cmp::Eq: return a == b;
    case Cmp::Ne: return a != b;
    case Cmp::Lt: return a < b;
    case Cmp::Le: return a <= b;
    case Cmp::Gt: return a > b;
    case Cmp::Ge: return a >= b;
    }
    return false;
}

const QuestState& stateOf(const Session& s, size_t q) {
    static const QuestState none;
    return q < s.quests.size() ? s.quests[q] : none;
}

// Session::quests matches the loaded quests (a defs reload may append some).
void fit(Session& s) {
    const Defs& d = defs();
    s.quests.resize(d.quests.size());
    for (size_t q = 0; q < d.quests.size(); ++q) s.quests[q].objectives.resize(d.quests[q].objectives.size());
}

// One piece of quest news for the HUD. Of several about the same quest in a row the first stays
// ("started" over "updated"), unless a later one is `final` (complete, failed).
void toast(Session& s, int quest, std::string text, bool final = false) {
    bool same = !s.toasts.empty() && quest >= 0 && s.toasts.back().first == quest;
    if (same && final) s.toasts.back().second = std::move(text);
    else if (!same && s.toasts.size() < 8) s.toasts.emplace_back(quest, std::move(text));
}

void log(Session& s, std::string line) {
    s.eventLog.push_back(std::format("{:7.1f} s  {}", s.elapsed, line));
    if (s.eventLog.size() > kEventLogSize) s.eventLog.pop_front();
}

std::string describe(const Session& s, const GameEvent& ev) {
    auto marker = [&](uint32_t id) {
        const Marker* m = markerById(s.markers, id);
        return std::format("#{} {}", id, m ? m->name : "(gone)");
    };
    const Defs& d = defs();
    if (auto* k = std::get_if<Killed>(&ev)) return std::format("killed {} of {}", d.creatureIds[size_t(k->kind)], marker(k->spawnMarkerId));
    if (auto* c = std::get_if<ItemCountChanged>(&ev)) return std::format("{} x{}", d.itemIds[c->item], c->count);
    if (auto* t = std::get_if<TalkedTo>(&ev)) return std::format("talked to {}", marker(t->spawnMarkerId));
    return std::format("entered {}", marker(std::get<RegionEntered>(ev).markerId));
}

bool matches(const Session& s, const Trigger& t, const GameEvent& ev) {
    if (auto* k = std::get_if<Killed>(&ev))
        return t.kind == Trigger::KilledCreature ? t.creature == k->kind : t.kind == Trigger::Killed && named(s.markers, k->spawnMarkerId, t.name);
    if (auto* c = std::get_if<ItemCountChanged>(&ev))  // the count now: the event's may be stale by the time it's handled
        return t.kind == Trigger::HasItem && t.item == c->item && countItem(s.player.inv, t.item) >= t.count;
    if (auto* talked = std::get_if<TalkedTo>(&ev)) return t.kind == Trigger::TalkedTo && named(s.markers, talked->spawnMarkerId, t.name);
    return t.kind == Trigger::Entered && named(s.markers, std::get<RegionEntered>(ev).markerId, t.name);
}

void setObjective(Session& s, size_t q, size_t o, ObjectiveState to) {
    ObjectiveState& st = s.quests[q].objectives[o];
    if (st == to) return;
    st = to;
    if (s.quests[q].active()) toast(s, int(q), "Quest updated: " + defs().quests[q].title);
}

void enter(Session& s, size_t q, int id, int depth);

// Whether every item `actions` give (and the stages they enter give) can be stowed, played out on
// `inv`, a copy of the player's: a trigger that can't hand over its reward doesn't fire, so it is never lost.
bool fits(const Session& s, Inventory& inv, const std::vector<Action>& actions, int depth) {
    const Defs& d = defs();
    for (const Action& a : actions) {
        const Stage* next = nullptr;
        if (a.kind == Action::GiveItem && !stowItem(inv, a.item)) return false;
        if (a.kind == Action::TakeItem) takeItem(inv, a.item);
        if (a.kind == Action::SetStage) next = findStage(d.quests[a.quest], a.value);
        if (a.kind == Action::StartQuest && s.quests[a.quest].stage == 0)
            for (const Stage& st : d.quests[a.quest].stages)
                if (st.id > 0 && (!next || st.id < next->id)) next = &st;
        if (next && depth < kMaxStageChain && !fits(s, inv, next->actions, depth + 1)) return false;
    }
    return true;
}

void run(Session& s, const std::vector<Action>& actions, int depth) {
    const Defs& d = defs();
    for (const Action& a : actions) {
        QuestState& st = s.quests[a.quest];
        const std::string& title = d.quests[a.quest].title;
        switch (a.kind) {
        case Action::ShowObjective: setObjective(s, a.quest, size_t(a.value), ObjectiveState::Shown); break;
        case Action::CompleteObjective: setObjective(s, a.quest, size_t(a.value), ObjectiveState::Completed); break;
        case Action::FailObjective: setObjective(s, a.quest, size_t(a.value), ObjectiveState::Failed); break;
        case Action::GiveItem:  // triggers check first that it fits (fits); only the debug window's set stage gets here without
            if (!stowItem(s.player.inv, a.item)) log(s, std::format("give_item {}: no room, not given", d.itemIds[a.item]));
            break;
        case Action::TakeItem: takeItem(s.player.inv, a.item); break;
        case Action::SetFlag: s.flags[a.flag] = a.value; break;
        case Action::SetStage: enter(s, a.quest, a.value, depth + 1); break;
        case Action::StartQuest:
            if (st.stage == 0 && !st.done && !st.failed) {
                int first = kMaxStageId + 1;
                for (const Stage& stage : d.quests[a.quest].stages)
                    if (stage.id > 0) first = std::min(first, stage.id);
                enter(s, a.quest, first, depth + 1);
            }
            break;
        case Action::CompleteQuest:
        case Action::FailQuest:
            if (st.done || st.failed) break;
            (a.kind == Action::CompleteQuest ? st.done : st.failed) = true;
            toast(s, a.quest, (a.kind == Action::CompleteQuest ? "Quest complete: " : "Quest failed: ") + title, true);
            log(s, std::format("{} {}", a.kind == Action::CompleteQuest ? "completed" : "failed", d.questIds[a.quest]));
            break;
        }
    }
}

void enter(Session& s, size_t q, int id, int depth) {
    const QuestDef& def = defs().quests[q];
    const Stage* stage = findStage(def, id);
    if (!stage) return;
    if (depth > kMaxStageChain) {
        log(s, std::format("{}: set_stage chain longer than {}, stopped at {}", def.id, kMaxStageChain, id));
        return;
    }
    QuestState& st = s.quests[q];
    if (st.done || st.failed || st.stage == id) return;
    bool started = st.stage == 0 && id != 0;
    st.stage = id;
    log(s, std::format("{} stage {}", def.id, id));
    if (id != 0 && !st.done && !st.failed) toast(s, int(q), (started ? "Quest started: " : "Quest updated: ") + def.title);
    run(s, stage->actions, depth);
    // "Has item" is a state: the player may hold it already. Checked with next tick's events.
    for (const Trigger& t : stage->on)
        if (t.kind == Trigger::HasItem) s.events.push_back(ItemCountChanged{t.item, countItem(s.player.inv, t.item)});
}

}  // namespace

const Stage* findStage(const QuestDef& q, int id) {
    auto it = std::ranges::find(q.stages, id, &Stage::id);
    return it != q.stages.end() ? &*it : nullptr;
}

int countItem(const Inventory& inv, ItemId id) {
    int n = int(std::ranges::count(inv.equipped, id) + std::ranges::count(inv.bags, id));
    for (const std::vector<ItemId>& c : inv.containers) n += int(std::ranges::count(c, id));
    return id == NoItem ? 0 : n;
}

bool passes(const Session& s, const Condition& c) {
    bool r = false;
    const QuestState& q = stateOf(s, c.quest);
    switch (c.kind) {
    case Condition::Stage: r = compare(q.stage, c.op, c.value); break;
    case Condition::Done: r = q.done; break;
    case Condition::Failed: r = q.failed; break;
    case Condition::Objective:
        r = int(c.objective < q.objectives.size() ? q.objectives[c.objective] : ObjectiveState::Hidden) == c.value;
        break;
    case Condition::Flag: {
        auto it = s.flags.find(c.name);
        r = compare(it != s.flags.end() ? it->second : 0, c.op, c.value);
        break;
    }
    case Condition::HasItem: r = compare(countItem(s.player.inv, c.item), c.op, c.value); break;
    case Condition::InRegion:  // any region of that name around the player, not just the innermost
        r = std::ranges::any_of(s.markers, [&](const Marker& m) {
            return m.kind == MarkerKind::Region && m.name == c.name &&
                   glm::distance(glm::vec2(s.player.pos.x, s.player.pos.z), glm::vec2(m.pos.x, m.pos.z)) <= m.radius;
        });
        break;
    case Condition::NpcDead: {  // every NPC of the spawns of that name (and there is one)
        bool any = false;
        r = true;
        for (const Entity& e : s.entities)
            if (e.spawn.id && e.spawn.name == c.name) any = true, r = r && !e.body.alive();
        r = r && any;
        break;
    }
    }
    return r != c.negate;
}

bool passesAll(const Session& s, const std::vector<Condition>& cs) {
    return std::ranges::all_of(cs, [&](const Condition& c) { return passes(s, c); });
}

int pickLine(const Session& s, const Entity& e) {
    if (!e.spawn.id) return -1;
    const std::vector<QuestLine>& lines = defs().questLines;
    for (size_t i = 0; i < lines.size(); ++i)
        if (lines[i].spawn == e.spawn.name && passesAll(s, lines[i].conditions)) return int(i);
    return -1;
}

void resetQuests(Session& s) {
    s.quests.clear();
    fit(s);
    s.flags.clear();
    s.events.clear();
    s.itemCounts.clear();
    s.eventLog.clear();
    s.toasts.clear();
}

void resetQuest(Session& s, size_t quest) {
    fit(s);
    QuestState& st = s.quests[quest];
    st = {};
    st.objectives.resize(defs().quests[quest].objectives.size());
    log(s, std::format("{} reset", defs().questIds[quest]));
}

void setQuestStage(Session& s, size_t quest, int id) {
    fit(s);
    enter(s, quest, id, 0);
}

void updateQuests(Session& s) {
    fit(s);
    const Defs& d = defs();
    std::vector<GameEvent> batch;
    batch.swap(s.events);  // what handling these raises lands in s.events: the next batch
    std::vector<bool> counted(d.items.size());  // an item's count is handled once per batch (matches reads it live)
    std::vector<QuestState> before;
    for (const GameEvent& ev : batch) {
        if (auto* c = std::get_if<ItemCountChanged>(&ev); c && c->item < counted.size()) {
            if (counted[c->item]) continue;
            counted[c->item] = true;
        }
        log(s, describe(s, ev));
        before = s.quests;  // a quest another one moved (start_quest) during this event hears it next time: quest order doesn't matter
        for (size_t q = 0; q < d.quests.size(); ++q) {
            const QuestState& was = before[q];
            if (was.done || was.failed || s.quests[q].stage != was.stage || s.quests[q].done || s.quests[q].failed) continue;
            const Stage* stage = findStage(d.quests[q], was.stage);
            if (!stage) continue;
            for (const Trigger& t : stage->on)
                if (matches(s, t, ev) && passesAll(s, t.conditions)) {
                    if (Inventory trial = s.player.inv; !fits(s, trial, t.actions, 0)) {  // fires again on the next event, with room
                        toast(s, int(q), "Make room in your bags: " + d.quests[q].title);
                        log(s, std::format("{}: no room for what it gives, waits", d.questIds[q]));
                    } else run(s, t.actions, 0);
                    break;  // one trigger per event and quest: its actions may have moved the quest on
                }
        }
    }
}

std::vector<std::string> checkQuestMarkers(const Defs& d, const std::vector<Marker>& markers) {
    std::vector<std::string> out;
    auto check = [&](const std::string& quest, const std::string& name, MarkerKind kind) {
        auto n = std::ranges::count_if(markers, [&](const Marker& m) { return m.kind == kind && m.name == name; });
        if (n != 1)
            out.push_back(std::format("quest '{}': {} {} named '{}'", quest, n ? "more than one" : "no", kind == MarkerKind::Region ? "region" : "spawn marker", name));
    };
    auto conditions = [&](const std::string& quest, const std::vector<Condition>& cs) {
        for (const Condition& c : cs)
            if (c.kind == Condition::InRegion || c.kind == Condition::NpcDead)
                check(quest, c.name, c.kind == Condition::InRegion ? MarkerKind::Region : MarkerKind::Spawn);
    };
    for (const QuestDef& q : d.quests)
        for (const Stage& st : q.stages)
            for (const Trigger& t : st.on) {
                if (t.kind == Trigger::TalkedTo || t.kind == Trigger::Killed) check(q.id, t.name, MarkerKind::Spawn);
                if (t.kind == Trigger::Entered) check(q.id, t.name, MarkerKind::Region);
                conditions(q.id, t.conditions);
            }
    for (const QuestLine& l : d.questLines) {
        check(d.questIds[l.quest], l.spawn, MarkerKind::Spawn);
        conditions(d.questIds[l.quest], l.conditions);
    }
    std::ranges::sort(out);
    auto [first, last] = std::ranges::unique(out);
    out.erase(first, last);
    return out;
}
