#include "ui/Captions.h"

#include "sim/Cast.h"
#include "sim/Map.h"
#include "sim/Names.h"
#include "sim/World.h"
#include "ui/Format.h"

#include <algorithm>
#include <cmath>

namespace lw::ui {

namespace {

// У дома — ближе этого к двери, метры.
constexpr float kNearHome = 25.0f;
// Нужда, с которой её называют причиной.
constexpr float kPressing = 0.6f;
// Итог эпизода — только если случилось что-то не легче этого (вес биографии).
constexpr float kNotableSummary = 1.0f;

// Мужская или женская форма.
const char* g(bool female, const char* male, const char* femaleForm) {
    return female ? femaleForm : male;
}

entt::entity entityOf(const World& world, EntityId id) {
    return id == kInvalidId ? entt::null : world.find(id);
}

// Почему мститель идёт к обидчику: самое сильное воспоминание о том, что
// обидчик сделал с ним или с близким. «за то, что он обокрал его дом».
std::string grudgeReason(const World& world, entt::entity self, EntityId selfId, EntityId offender) {
    const auto* memories = world.registry().try_get<Memories>(self);
    if (memories == nullptr) {
        return {};
    }
    const Memory* best = nullptr;
    for (std::size_t i = 0; i < memories->count; ++i) {
        const Memory& memory = memories->items[i];
        const bool his = memory.who == offender &&
                         (memory.kind == MemoryKind::Robbed || memory.kind == MemoryKind::Betrayed ||
                          memory.kind == MemoryKind::Assaulted || memory.kind == MemoryKind::Abandoned ||
                          memory.kind == MemoryKind::Killed || memory.kind == MemoryKind::Expelled);
        const bool killedKin = memory.kind == MemoryKind::Bereaved && memory.about == offender;
        if ((his || killedKin) && (best == nullptr || memory.strength > best->strength)) {
            best = &memory;
        }
    }
    if (best == nullptr) {
        return {};
    }
    const bool he = !isFemale(world, offender); // обидчик
    const char* pronoun = he ? "он" : "она";
    const char* mine = isFemale(world, selfId) ? "её" : "его";
    const bool toSelf = best->victim == kInvalidId || best->victim == selfId;
    const std::string victim = toSelf ? std::string(mine) : idLabel(best->victim);
    switch (best->kind) {
    case MemoryKind::Robbed:
        return toSelf ? std::string("за то, что ") + pronoun + " " + g(!he, "обокрал", "обокрала") +
                            " " + mine + " дом"
                      : std::string("за то, что ") + pronoun + " " + g(!he, "обокрал", "обокрала") +
                            " " + victim;
    case MemoryKind::Betrayed:
        return toSelf ? "за предательство" : "за то, что " + std::string(pronoun) + " " +
                                                 g(!he, "предал", "предала") + " " + victim;
    case MemoryKind::Assaulted:
        return toSelf ? "за нападение" : "за нападение на " + victim;
    case MemoryKind::Abandoned:
        return std::string("за то, что ") + pronoun + " " + g(!he, "бросил", "бросила") + " " +
               victim + " в беде";
    case MemoryKind::Killed:
        return "за убийство " + victim;
    case MemoryKind::Expelled:
        return toSelf ? "за изгнание из группы" : "за то, что изгнал из группы " + victim;
    case MemoryKind::Bereaved:
        return "за гибель " + idLabel(best->who);
    default:
        return {};
    }
}

// Причина по нуждам и чертам: « — голоден», « — из жадности»; пусто — нет.
std::string needReason(const World& world, entt::entity entity, bool female, bool greedMatters) {
    const entt::registry& registry = world.registry();
    if (const auto* needs = registry.try_get<Needs>(entity)) {
        if (needs->hunger >= kPressing) {
            return std::string(" — ") + g(female, "голоден", "голодна");
        }
        if (needs->thirst >= kPressing) {
            return " — мучает жажда";
        }
    }
    if (greedMatters) {
        if (const auto* traits = registry.try_get<Traits>(entity); traits != nullptr && traits->greed >= 0.7f) {
            return " — из жадности";
        }
    }
    return {};
}

bool isPerson(const World& world, EntityId id) {
    const entt::entity entity = entityOf(world, id);
    return entity != entt::null && world.registry().all_of<Person>(entity);
}

// «зомби» или имя человека, на кого направлено действие.
std::string foeName(const World& world, EntityId id) {
    return isPerson(world, id) ? idLabel(id) : std::string("зомби");
}

const char* causePhrase(DeathCause cause) {
    switch (cause) {
    case DeathCause::Bleeding:
        return " от потери крови";
    case DeathCause::Starvation:
        return " от голода";
    case DeathCause::Dehydration:
        return " от жажды";
    case DeathCause::Exhaustion:
        return " от изнеможения";
    case DeathCause::Infection:
        return " от заражения";
    default:
        return "";
    }
}

// «, защищая дом» — что он делал, когда погиб.
const char* dyingPhrase(const Witnessed& witnessed) {
    switch (witnessed.action) {
    case Action::Fight:
        return witnessed.nearHome ? ", защищая дом" : ", в схватке";
    case Action::Flee:
        return ", убегая";
    case Action::Avenge:
        return ", пытаясь отомстить";
    case Action::Salvage:
        return ", добираясь до груза";
    case Action::Scavenge:
    case Action::Steal:
        return ", обыскивая чужой дом";
    case Action::Hunt:
        return ", на охоте";
    case Action::Sleep:
        return witnessed.nearHome ? ", во сне" : ", уснув под открытым небом";
    default:
        return witnessed.nearHome ? ", у своего дома" : "";
    }
}

// События, итог которых можно сказать и о том, с кем это сделали (object):
// его спасли, предали, изгнали; погибший от его руки.
bool aboutObject(ChronicleKind kind) {
    using K = ChronicleKind;
    switch (kind) {
    case K::Died:
    case K::SavedBy:
    case K::DefendedBy:
    case K::AbandonedBy:
    case K::BetrayedBy:
    case K::AssaultedBy:
    case K::Expelled:
        return true;
    default:
        return false;
    }
}

} // namespace

bool isFemale(const World& world, EntityId id) {
    const entt::entity entity = entityOf(world, id);
    if (entity == entt::null) {
        return false;
    }
    const auto* name = world.registry().try_get<Name>(entity);
    return name != nullptr && isFemaleName(name->first);
}

std::string intentLine(const World& world, EntityId id) {
    const entt::entity entity = entityOf(world, id);
    if (entity == entt::null) {
        return {};
    }
    const entt::registry& registry = world.registry();
    const bool female = isFemale(world, id);
    if (const auto* corpse = registry.try_get<Corpse>(entity)) {
        if (corpse->kind != CorpseKind::Human) {
            return {};
        }
        std::string line = g(female, "погиб", "погибла");
        line += causePhrase(corpse->cause);
        if (corpse->killer != kInvalidId && isPerson(world, corpse->killer)) {
            line += " — убийца " + idLabel(corpse->killer);
        }
        if (corpse->turnsAt != 0) {
            line += "; скоро встанет";
        }
        return line;
    }
    if (registry.all_of<Zombie>(entity)) {
        return registry.all_of<Name>(entity) ? g(female, "стал зомби", "стала зомби") : "";
    }
    const auto* mind = registry.try_get<Mind>(entity);
    const auto* person = registry.try_get<Person>(entity);
    if (mind == nullptr || person == nullptr) {
        return {};
    }
    const bool travel = mind->phase == ActionPhase::Travel;
    const EntityId target = mind->target;
    switch (mind->action) {
    case Action::Avenge: {
        std::string line = (travel ? "идёт мстить " : "мстит ") + idLabel(target);
        if (const std::string reason = grudgeReason(world, entity, id, target); !reason.empty()) {
            line += " " + reason;
        }
        return line;
    }
    case Action::Steal:
        if (person->group != kInvalidId && target == person->home) {
            return "хочет сбежать из группы с её запасами";
        }
        return (travel ? "идёт обокрасть дом " : "обворовывает дом ") + idLabel(target) +
               needReason(world, entity, female, true);
    case Action::Flee:
        return isPerson(world, target) ? "бежит от " + idLabel(target) : "спасается от зомби";
    case Action::Fight:
        return "дерётся с " + foeName(world, target);
    case Action::Hunt:
        return std::string("охотится") + needReason(world, entity, female, false);
    case Action::Salvage:
        return std::string(travel ? "спешит к грузу" : "забирает груз") +
               needReason(world, entity, female, true);
    case Action::Scavenge:
        return std::string(travel ? "идёт обыскать чужой дом" : "обыскивает чужой дом") +
               needReason(world, entity, female, true);
    case Action::Forage:
        return "собирает ягоды и грибы в лесу";
    case Action::Eat:
        return "ест";
    case Action::Drink:
        return "пьёт";
    case Action::Sleep:
        return person->indoors || !travel ? (person->indoors ? "спит дома" : "спит под открытым небом")
                                          : "идёт спать";
    case Action::Socialize:
        if (isPerson(world, target)) {
            return (travel ? "идёт к " : "разговаривает с ") + idLabel(target);
        }
        return "ищет компанию — одиноко";
    case Action::Share:
        return (travel ? "несёт помощь " : "делится с ") + idLabel(target);
    case Action::Trade:
        return (travel ? "идёт меняться с " : "меняется с ") + idLabel(target);
    case Action::Treat:
        return "перевязывает раны";
    case Action::Work:
        if (const auto* job = registry.try_get<Job>(entity); job != nullptr && job->profession == Profession::Pupil) {
            return travel ? "идёт в школу" : "на уроках"; // этап 7.3
        }
        if (const auto* job = registry.try_get<Job>(entity)) {
            return std::string(travel ? "идёт на работу — " : "на работе — ") +
                   professionName(job->profession);
        }
        return travel ? "идёт на работу" : "на работе";
    case Action::Buy:
        return travel ? "идёт за покупками" : "покупает еду";
    case Action::SeeDoctor:
        return travel ? "идёт к врачу" : "у врача";
    case Action::GoHome: {
        const auto* needs = registry.try_get<Needs>(entity);
        return needs != nullptr && needs->fear >= 0.5f ? "идёт домой — страшно" : "идёт домой";
    }
    case Action::Wander:
        return "гуляет";
    case Action::Idle:
    default:
        return person->indoors ? "отдыхает дома" : "отдыхает";
    }
}

std::string statusLine(const World& world, EntityId id) {
    const entt::entity entity = entityOf(world, id);
    if (entity == entt::null) {
        return {};
    }
    const entt::registry& registry = world.registry();
    const auto* person = registry.try_get<Person>(entity);
    if (person == nullptr) {
        return {};
    }
    const bool female = isFemale(world, id);
    std::string line;
    const auto append = [&](const std::string& part) {
        line += line.empty() ? part : ", " + part;
    };
    if (const entt::entity group = entityOf(world, person->group);
        group != entt::null && registry.all_of<Group>(group)) {
        const Group& info = registry.get<Group>(group);
        append(info.leader == id ? "вожак группы из " + std::to_string(info.members.size())
                                 : "в группе " + idLabel(info.leader));
    }
    if (const auto* wounds = registry.try_get<Wounds>(entity)) {
        if (wounds->infection >= 0.5f) {
            append(std::string(g(female, "укушен", "укушена")) + ", лихорадка");
        } else if (wounds->infection > 0.0f) {
            append(g(female, "укушен", "укушена"));
        }
        if (wounds->bleeding > 0.0f) {
            append("истекает кровью");
        }
    }
    if (const auto* needs = registry.try_get<Needs>(entity)) {
        if (needs->health < 0.2f) {
            append("при смерти");
        } else if (needs->health < 0.5f) {
            append(g(female, "ранен", "ранена"));
        }
    }
    return line;
}

bool observe(const World& world, EntityId id, Witnessed& witnessed) {
    const entt::entity entity = entityOf(world, id);
    if (entity == entt::null) {
        return false;
    }
    const entt::registry& registry = world.registry();
    const auto* mind = registry.try_get<Mind>(entity);
    const auto* person = registry.try_get<Person>(entity);
    const auto* position = registry.try_get<Position>(entity);
    if (mind == nullptr || person == nullptr || position == nullptr) {
        return false;
    }
    witnessed.action = mind->action;
    witnessed.nearHome = person->indoors;
    if (const entt::entity home = entityOf(world, person->home);
        !witnessed.nearHome && home != entt::null) {
        if (const auto* building = registry.try_get<Building>(home)) {
            const float dx = Map::cellCenter(building->door.x) - position->x;
            const float dy = Map::cellCenter(building->door.y) - position->y;
            witnessed.nearHome = dx * dx + dy * dy <= kNearHome * kNearHome;
        }
    }
    return true;
}

std::string episodeSummary(const World& world, EntityId id, std::uint64_t since,
                           const Witnessed& witnessed) {
    const Chronicle& chronicle = world.chronicle();
    const std::vector<ChronicleEvent>& events = chronicle.events();
    const ChronicleEvent* best = nullptr;
    float bestWeight = 0.0f;
    int zombies = 0;
    for (const std::uint32_t index : chronicle.eventsOf(id)) {
        const ChronicleEvent& event = events[index];
        if (event.tick < since) {
            continue;
        }
        if (event.kind == ChronicleKind::KilledZombie && event.subject == id) {
            ++zombies;
        }
        if (event.subject != id && !aboutObject(event.kind)) {
            continue; // с ним что-то сделали, но фразы с этой стороны нет
        }
        // Гибель и обращение — всегда итог, остальное — по весу.
        float weight = chronicleWeight(event, id);
        if ((event.kind == ChronicleKind::Died || event.kind == ChronicleKind::Turned) &&
            event.subject == id) {
            weight += 100.0f;
        }
        if (weight >= bestWeight && weight > 0.0f) {
            best = &event;
            bestWeight = weight;
        }
    }
    const bool female = isFemale(world, id);
    const std::string name = idLabel(id);
    if (best == nullptr || bestWeight < kNotableSummary) {
        if (zombies >= 2) {
            return name + " " + g(female, "упокоил", "упокоила") + " зомби: " + std::to_string(zombies);
        }
        return {};
    }
    const ChronicleEvent& event = *best;
    const bool subject = event.subject == id;
    const std::string other = idLabel(event.object);
    const bool otherFemale = isFemale(world, event.object);
    using K = ChronicleKind;
    switch (event.kind) {
    case K::Died: {
        if (!subject) {
            return name + " " + g(female, "убил", "убила") + " " + idLabel(event.subject);
        }
        std::string line = name + " " + g(female, "погиб", "погибла");
        const auto cause = static_cast<DeathCause>(event.a);
        if (cause == DeathCause::Wounds && isPerson(world, event.object)) {
            line += " от руки " + other;
        } else {
            line += causePhrase(cause);
        }
        if (cause == DeathCause::Wounds || cause == DeathCause::Bleeding) {
            line += dyingPhrase(witnessed);
        }
        return line;
    }
    case K::Turned:
        return name + " " + g(female, "встал", "встала") + " зомби";
    case K::Avenged:
        return name + " " + g(female, "отомстил", "отомстила") + " " + other +
               (event.a == 1 ? std::string(" — ") + g(female, "убил", "убила") : std::string());
    case K::Stole:
        return name + " " + g(female, "обокрал", "обокрала") + " дом " +
               (event.third != kInvalidId ? idLabel(event.third) : other);
    case K::Deserted:
        return name + " " + g(female, "сбежал", "сбежала") + " из группы с её запасами";
    case K::GroupFormed:
        return name + " " + g(female, "собрал", "собрала") + " группу";
    case K::LedGroup:
        return name + " " + g(female, "возглавил", "возглавила") + " группу";
    case K::JoinedGroup:
        return name + " " + g(female, "вступил", "вступила") + " в группу";
    case K::LeftGroup:
        return name + " " + g(female, "ушёл", "ушла") + " из группы";
    case K::Expelled:
        return subject ? name + " " + g(female, "изгнал", "изгнала") + " " + other + " из группы"
                       : name + " " + g(female, "изгнан", "изгнана") + " из группы";
    case K::Bitten:
        return name + " " + g(female, "укушен", "укушена") + " зомби";
    case K::Feverish:
        return name + ": у " + g(female, "него", "неё") + " лихорадка — заражение";
    case K::SavedBy:
        return subject ? other + " " + g(otherFemale, "спас", "спасла") + " " + name
                       : name + " " + g(female, "спас", "спасла") + " " + idLabel(event.subject);
    case K::DefendedBy:
        return subject ? other + " " + g(otherFemale, "заступился", "заступилась") + " за " + name
                       : name + " " + g(female, "заступился", "заступилась") + " за " +
                             idLabel(event.subject);
    case K::AbandonedBy:
        return subject ? other + " " + g(otherFemale, "бросил", "бросила") + " " + name + " в беде"
                       : name + " " + g(female, "бросил", "бросила") + " " + idLabel(event.subject) +
                             " в беде";
    case K::BetrayedBy:
        return subject ? other + " " + g(otherFemale, "предал", "предала") + " " + name
                       : name + " " + g(female, "предал", "предала") + " " + idLabel(event.subject);
    case K::AssaultedBy:
        return subject ? "на " + name + " " + g(otherFemale, "напал", "напала") + " " + other
                       : name + " " + g(female, "напал", "напала") + " на " + idLabel(event.subject);
    case K::CaughtThief:
        return name + " " + g(female, "застал", "застала") + " " + other + " за кражей";
    case K::Mourned:
        return name + " " + g(female, "узнал", "узнала") + " о гибели " + other;
    case K::SawTurned:
        return name + " " + g(female, "узнал", "узнала") + " в зомби " + other;
    case K::Befriended:
        return name + " " + g(female, "подружился", "подружилась") + " с " + other;
    case K::Disliked:
        return name + " " + g(female, "невзлюбил", "невзлюбила") + " " + other;
    case K::Reconciled:
        return name + " " + g(female, "помирился", "помирилась") + " с " + other;
    case K::Shared:
        return name + " " + g(female, "поделился", "поделилась") + " с " + other;
    case K::Salvaged:
        return name + " " + g(female, "добрался", "добралась") + " до груза";
    case K::Fled:
        return name + " " + g(female, "спасся", "спаслась") + " бегством";
    case K::Dying:
        return name + " " + g(female, "тяжело ранен", "тяжело ранена");
    case K::KilledZombie:
        return name + " " + g(female, "упокоил", "упокоила") + " зомби" +
               (zombies >= 2 ? ": " + std::to_string(zombies) : std::string());
    default:
        return {};
    }
}

std::vector<std::uint32_t> notableEvents(const World& world, EntityId id, std::uint64_t from,
                                         std::uint64_t to, std::size_t count) {
    const std::vector<ChronicleEvent>& events = world.chronicle().events();
    std::vector<std::pair<float, std::uint32_t>> picked;
    for (const std::uint32_t index : world.chronicle().eventsOf(id)) {
        const ChronicleEvent& event = events[index];
        if (event.tick < from || event.tick >= to) {
            continue;
        }
        if (const float weight = chronicleWeight(event, id); weight >= kNotableSummary) {
            picked.emplace_back(weight, index);
        }
    }
    // Самые весомые; при равном весе — поздние.
    std::stable_sort(picked.begin(), picked.end(), [](const auto& a, const auto& b) {
        return a.first > b.first || (a.first == b.first && a.second > b.second);
    });
    picked.resize(std::min(count, picked.size()));
    std::vector<std::uint32_t> result;
    for (const auto& [weight, index] : picked) {
        result.push_back(index);
    }
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace lw::ui
