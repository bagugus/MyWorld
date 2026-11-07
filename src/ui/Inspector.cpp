#include "ui/Inspector.h"

#include "render/CameraController.h"
#include "sim/Components.h"
#include "sim/Events.h"
#include "sim/Map.h"
#include "sim/Perception.h"
#include "sim/Family.h"
#include "sim/Infection.h"
#include "sim/Names.h"
#include "sim/Relations.h"
#include "sim/Scenario.h"
#include "sim/TimeScale.h"
#include "sim/Items.h"
#include "sim/Weapons.h"
#include "sim/Work.h"
#include "sim/World.h"
#include "sim/systems/AnimalSystem.h"
#include "sim/systems/GroupSystem.h"
#include "sim/systems/ResourceSystem.h"
#include "sim/systems/UtilityAiSystem.h"
#include "sim/systems/WorkSystem.h"
#include "sim/systems/ZombieSystem.h"
#include "ui/ChronicleView.h"
#include "ui/Format.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace lw::ui {

namespace {

// «08:00–18:00, пн вт ср чт пт сб» — смена и рабочие дни (этап 7.2).
void formatShift(char* buffer, std::size_t size, const Job& job) {
    constexpr const char* kDays[kDaysPerWeek] = {"пн", "вт", "ср", "чт", "пт", "сб", "вс"};
    int used = std::snprintf(buffer, size, "%02d:%02d–%02d:%02d,", job.start / 60, job.start % 60,
                             job.end / 60, job.end % 60);
    for (int d = 0; d < kDaysPerWeek && used > 0 && static_cast<std::size_t>(used) < size; ++d) {
        if (isWorkday(job, d)) {
            used += std::snprintf(buffer + used, size - static_cast<std::size_t>(used), " %s", kDays[d]);
        }
    }
}

constexpr float kPickPixels = 8.0f;
constexpr Color kSelectionColor = {255, 255, 255, 255};
constexpr Color kPathColor = {255, 236, 140, 220};
constexpr Color kTargetColor = {255, 120, 90, 255};
constexpr Color kHomeColor = {120, 220, 255, 255};
// Восприятие: дальность зрения и слуха, линии к замеченным.
constexpr Color kSightColor = {170, 255, 170, 70};
constexpr Color kHearingColor = {255, 220, 120, 70};
constexpr Color kSeenColor = {120, 255, 120, 220};
constexpr Color kHeardColor = {255, 210, 90, 220};
constexpr Color kRememberedColor = {170, 170, 170, 160};
// Орда: участники и вожак.
constexpr Color kHordeColor = {214, 96, 80, 200};
// Опасные места, которые помнит человек.
constexpr Color kDangerColor = {230, 70, 60, 110};
constexpr Color kLeaderColor = {255, 170, 120, 255};
// Отношения: линии к друзьям и недругам.
constexpr Color kFriendColor = {255, 150, 220, 170};
constexpr Color kFoeColor = {200, 50, 50, 170};
// Группа (этап 6.4): участники, вожак и база.
constexpr Color kGroupColor = {110, 230, 220, 200};
constexpr Color kKinColor = {255, 205, 110, 200}; // родня (этап 7.3)
constexpr Color kGroupLeaderColor = {190, 255, 250, 255};
// Ивенты (этап 7.1): место, грузы, кто туда идёт; у человека — вести.
constexpr Color kEventColor = {255, 120, 100, 230};
constexpr Color kLeadColor = {255, 170, 90, 170};
// Заражение (этап 7.5): скрытый период, лихорадка, нулевой пациент, цепочка
// «кто от кого».
constexpr Color kLatentColor = {190, 230, 120, 200};
constexpr Color kFeverColor = {255, 140, 60, 230};
constexpr Color kPatientZeroColor = {255, 255, 140, 255};
constexpr Color kContagionColor = {190, 120, 255, 220};

// Позиция, в которой сущность сейчас нарисована: между началом тика и текущей
// (как в WorldRenderer).
Vector2 drawnPosition(const entt::registry& registry, entt::entity entity, float alpha) {
    const Position& position = registry.get<Position>(entity);
    Vector2 at = {position.x, position.y};
    if (const auto* velocity = registry.try_get<Velocity>(entity)) {
        const float back = (1.0f - std::clamp(alpha, 0.0f, 1.0f)) * kTickDt;
        at.x -= velocity->x * back;
        at.y -= velocity->y * back;
    }
    return at;
}

Vector2 cellCenter(Cell cell) {
    return {Map::cellCenter(cell.x), Map::cellCenter(cell.y)};
}

Rectangle buildingRect(const Building& building) {
    return {static_cast<float>(building.origin.x) * Map::kCellSize,
            static_cast<float>(building.origin.y) * Map::kCellSize,
            static_cast<float>(building.width) * Map::kCellSize,
            static_cast<float>(building.height) * Map::kCellSize};
}

bool contains(const Building& building, Cell cell) {
    return cell.x >= building.origin.x && cell.y >= building.origin.y &&
           cell.x < building.origin.x + building.width &&
           cell.y < building.origin.y + building.height;
}

EntityId idOf(const entt::registry& registry, entt::entity entity) {
    return registry.get<Id>(entity).value;
}

void drawSupplies(const entt::registry& registry, entt::entity building) {
    if (const auto* inventory = registry.try_get<Inventory>(building)) {
        char text[256];
        formatInventory(text, sizeof(text), *inventory);
        ImGui::TextWrapped("Внутри: %s", text);
        if (const auto* info = registry.try_get<Building>(building);
            info != nullptr && rainBarrel(info->kind) > 0) {
            ImGui::Text("Бочка для дождя: до %d порций воды", rainBarrel(info->kind));
        }
    }
}

} // namespace

Inspector::Inspector(const World& world) : world_(world) {}

void Inspector::select(EntityId id) {
    if (id != selected_) {
        follow_ = false;
    }
    selected_ = id;
}

EntityId Inspector::pick(float x, float y, float zoom) const {
    const entt::registry& registry = world_.registry();
    const float radius = std::max(1.0f, kPickPixels / zoom);
    if (const auto person = world_.spatial().nearest(x, y, radius, [&registry](const SpatialHit& hit) {
            return registry.any_of<Person, Zombie, Animal, Flock, Corpse, Cache>(hit.entity);
        })) {
        return person->id;
    }
    const Cell cell = Map::cellAt(x, y);
    for (const auto [entity, id, building] : registry.view<const Id, const Building>().each()) {
        if (contains(building, cell)) {
            return id.value;
        }
    }
    return kInvalidId;
}

void Inspector::handleInput(const CameraController& camera, bool mouseFree) {
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (!mouseFree || ctrl || !IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        return;
    }
    if (const auto point = camera.screenToWorld(GetMousePosition())) {
        select(pick(point->x, point->y, camera.zoomAt(*point)));
    }
}

void Inspector::updateCamera(CameraController& camera, float alpha) {
    if (!follow_) {
        return;
    }
    const entt::registry& registry = world_.registry();
    const entt::entity entity = world_.find(selected_);
    if (entity == entt::null) {
        follow_ = false;
        return;
    }
    if (registry.all_of<Position>(entity)) {
        const Vector2 at = drawnPosition(registry, entity, alpha);
        camera.moveTo(at.x, at.y);
    } else if (const auto* building = registry.try_get<Building>(entity)) {
        const Rectangle rect = buildingRect(*building);
        camera.moveTo(rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f);
    } else if (const auto* horde = registry.try_get<Horde>(entity)) {
        // За вожаком: центр орды пересчитывается раз в минуту и прыгает.
        if (const entt::entity leader = world_.find(horde->leader); leader != entt::null) {
            const Vector2 at = drawnPosition(registry, leader, alpha);
            camera.moveTo(at.x, at.y);
        }
    } else if (const auto* group = registry.try_get<Group>(entity)) {
        if (const entt::entity leader = world_.find(group->leader);
            leader != entt::null && registry.all_of<Position>(leader)) {
            const Vector2 at = drawnPosition(registry, leader, alpha);
            camera.moveTo(at.x, at.y);
        }
    } else if (const auto* event = registry.try_get<WorldEvent>(entity)) {
        const Vector2 at = cellCenter(event->site);
        camera.moveTo(at.x, at.y);
    }
}

std::optional<CameraSubject> Inspector::cameraSubject(float alpha) const {
    const entt::registry& registry = world_.registry();
    const entt::entity entity = world_.find(selected_);
    if (entity == entt::null || !registry.all_of<Position>(entity) ||
        !registry.any_of<Person, Zombie, Animal, Corpse>(entity)) {
        return std::nullopt;
    }
    CameraSubject subject;
    subject.id = selected_;
    subject.at = drawnPosition(registry, entity, alpha);
    if (const auto* velocity = registry.try_get<Velocity>(entity);
        velocity != nullptr && (velocity->x != 0.0f || velocity->y != 0.0f)) {
        subject.heading = std::atan2(velocity->x, velocity->y);
    }
    if (registry.any_of<Animal>(entity)) {
        subject.height = 0.9f;
    } else if (registry.any_of<Corpse>(entity)) {
        subject.height = 0.5f;
    }
    return subject;
}

void Inspector::drawOverlay(const Camera2D& camera, float alpha) const {
    const entt::registry& registry = world_.registry();
    const entt::entity entity = world_.find(selected_);
    if (entity == entt::null) {
        return;
    }
    const float px = 1.0f / camera.zoom;
    const float line = 2.0f * px;

    if (const auto* building = registry.try_get<Building>(entity)) {
        const Rectangle rect = buildingRect(*building);
        DrawRectangleLinesEx({rect.x - line, rect.y - line, rect.width + 2.0f * line,
                              rect.height + 2.0f * line},
                             line, kSelectionColor);
        DrawCircleV(cellCenter(building->door), std::max(Map::kCellSize * 0.3f, 3.0f * px),
                    kSelectionColor);
        return;
    }
    if (const auto* settlement = registry.try_get<Settlement>(entity)) {
        DrawRing(cellCenter(settlement->center),
                 static_cast<float>(settlement->radius) * Map::kCellSize - line,
                 static_cast<float>(settlement->radius) * Map::kCellSize, 0.0f, 360.0f, 96,
                 kSelectionColor);
        return;
    }
    if (const auto* horde = registry.try_get<Horde>(entity)) {
        // Кольцо у каждого участника, у вожака — ярче; к цели — линия от вожака.
        const float radius = std::max(1.2f, 6.0f * px);
        for (const EntityId member : horde->members) {
            const entt::entity other = world_.find(member);
            if (other == entt::null) {
                continue;
            }
            const bool leader = member == horde->leader;
            const Vector2 at = drawnPosition(registry, other, alpha);
            DrawRing(at, radius, radius + line, 0.0f, 360.0f, 24, leader ? kLeaderColor : kHordeColor);
            if (leader && horde->travelling) {
                DrawLineEx(at, cellCenter(horde->goal), line, kTargetColor);
            }
        }
        return;
    }
    if (const auto* group = registry.try_get<Group>(entity)) {
        // Кольцо у каждого участника, у вожака — ярче; база — рамкой.
        const float radius = std::max(1.2f, 7.0f * px);
        for (const EntityId member : group->members) {
            const entt::entity other = world_.find(member);
            if (other == entt::null || !registry.all_of<Position>(other)) {
                continue;
            }
            DrawRing(drawnPosition(registry, other, alpha), radius, radius + line, 0.0f, 360.0f, 24,
                     member == group->leader ? kGroupLeaderColor : kGroupColor);
        }
        if (const entt::entity base = world_.find(group->base); base != entt::null) {
            DrawRectangleLinesEx(buildingRect(registry.get<Building>(base)), line, kGroupColor);
        }
        return;
    }
    if (const auto* event = registry.try_get<WorldEvent>(entity)) {
        // Место — кольцом, грузы — линиями от него, идущие туда — линиями к нему.
        const Vector2 site = cellCenter(event->site);
        const float radius = std::max(6.0f, 12.0f * px);
        DrawRing(site, radius, radius + line, 0.0f, 360.0f, 48, kSelectionColor);
        for (const EntityId cache : event->caches) {
            if (const entt::entity crate = world_.find(cache); crate != entt::null) {
                DrawLineEx(site, drawnPosition(registry, crate, alpha), line, kEventColor);
            }
        }
        for (const auto [other, mind] : registry.view<const Mind>().each()) {
            if (mind.action == Action::Salvage && mind.target == selected_) {
                DrawLineEx(drawnPosition(registry, other, alpha), site, line, kLeadColor);
            }
        }
        return;
    }
    if (const auto* scenario = registry.try_get<Scenario>(entity)) {
        // Вспышка (этап 7.5): кольцо у каждого заражённого — бледное в скрытый
        // период, оранжевое в лихорадке; нулевой пациент — ещё одно, яркое.
        const float radius = std::max(1.2f, 6.0f * px);
        for (const auto [other, person, wounds] : registry.view<const Person, const Wounds>().each()) {
            if (isInfected(wounds)) {
                DrawRing(drawnPosition(registry, other, alpha), radius, radius + line, 0.0f, 360.0f, 24,
                         isFeverish(wounds) ? kFeverColor : kLatentColor);
            }
        }
        if (const entt::entity zero = world_.find(scenario->patientZero);
            zero != entt::null && registry.all_of<Position>(zero)) {
            const float outer = radius + 3.0f * line;
            DrawRing(drawnPosition(registry, zero, alpha), outer, outer + line, 0.0f, 360.0f, 32,
                     kPatientZeroColor);
        }
        return;
    }
    if (!registry.all_of<Position>(entity)) {
        return;
    }

    const Vector2 at = drawnPosition(registry, entity, alpha);
    if (const auto* flock = registry.try_get<Flock>(entity)) {
        if (flock->flying) {
            DrawLineEx(at, {flock->toX, flock->toY}, line, kTargetColor);
        }
        const float radius = std::max(8.0f, 8.0f * px);
        DrawRing(at, radius, radius + line, 0.0f, 360.0f, 32, kSelectionColor);
        return;
    }
    // Восприятие: круги — дальность зрения (идущего по открытому месту) и
    // слышимость шагов, линии — к замеченным: зелёные — видит, жёлтые —
    // слышит, серые — помнит, где видел в последний раз.
    if (const auto* senses = registry.try_get<Senses>(entity)) {
        const SenseConditions conditions = senseConditions(world_);
        const bool asleep = isAsleep(registry, entity);
        const float sight = sightRange(*senses, asleep, conditions);
        if (sight > 0.0f) {
            DrawRing(at, sight - line, sight, 0.0f, 360.0f, 96, kSightColor);
        }
        const float footsteps = kFootstepsLoudness * hearingFactor(*senses, asleep, conditions);
        DrawRing(at, footsteps - line, footsteps, 0.0f, 360.0f, 64, kHearingColor);
    }
    if (const auto* awareness = registry.try_get<Awareness>(entity)) {
        const std::uint64_t tick = world_.tick();
        for (std::size_t i = 0; i < awareness->count; ++i) {
            const Contact& contact = awareness->contacts[i];
            Vector2 to = {contact.x, contact.y};
            Color color = kRememberedColor;
            if (isFresh(contact, tick)) {
                color = contact.seen ? kSeenColor : kHeardColor;
                // Видимого — к тому месту, где он нарисован сейчас.
                if (const entt::entity other = world_.find(contact.id);
                    contact.seen && other != entt::null && registry.all_of<Position>(other)) {
                    to = drawnPosition(registry, other, alpha);
                }
            }
            DrawLineEx(at, to, line, color);
        }
    }
    // Опасные места — красными кругами.
    if (const auto* mind = registry.try_get<Mind>(entity)) {
        for (const Danger& danger : mind->dangers) {
            if (danger.until > world_.tick()) {
                DrawRing(cellCenter(danger.cell), kDangerRadius - line, kDangerRadius, 0.0f, 360.0f,
                         64, kDangerColor);
            }
        }
    }
    // Места ивентов, о которых знает и куда ещё не ходил, — оранжевыми линиями.
    if (const auto* leads = registry.try_get<Leads>(entity)) {
        for (std::size_t i = 0; i < leads->count; ++i) {
            const Lead& lead = leads->items[i];
            if (!lead.visited && leadFresh(lead, world_.tick())) {
                DrawLineEx(at, cellCenter(lead.site), line, kLeadColor);
            }
        }
    }
    // Товарищи по группе — бирюзовыми линиями.
    if (const auto* person = registry.try_get<Person>(entity)) {
        if (const entt::entity group = world_.find(person->group); group != entt::null) {
            for (const EntityId member : registry.get<Group>(group).members) {
                const entt::entity other = world_.find(member);
                if (other != entity && other != entt::null && registry.all_of<Position>(other)) {
                    DrawLineEx(at, drawnPosition(registry, other, alpha), line, kGroupColor);
                }
            }
        }
    }
    // Родня, которая жива, — тёплыми линиями (этап 7.3).
    if (const auto* kin = registry.try_get<Kin>(entity)) {
        for (std::size_t i = 0; i < kin->count; ++i) {
            const entt::entity other = world_.find(kin->relatives[i].id);
            if (other != entt::null && registry.all_of<Person>(other)) {
                DrawLineEx(at, drawnPosition(registry, other, alpha), line, kKinColor);
            }
        }
    }
    // Друзья и недруги, которые живы, — линиями.
    if (const auto* relations = registry.try_get<Relations>(entity)) {
        for (std::size_t i = 0; i < relations->count; ++i) {
            const Bond& bond = relations->bonds[i];
            const entt::entity other = world_.find(bond.other);
            // Родня — своим цветом выше.
            if ((bond.friendly() || bond.hostile()) && other != entt::null &&
                registry.all_of<Person>(other) && kinOf(registry, entity, bond.other) == KinKind::Count) {
                DrawLineEx(at, drawnPosition(registry, other, alpha), line,
                           bond.hostile() ? kFoeColor : kFriendColor);
            }
        }
    }
    // Цепочка заражения (этап 7.5): от кого заразился и кого заразил сам —
    // кто из них ещё в мире (живой, тело или зомби).
    {
        const auto chain = [&](EntityId other) {
            const entt::entity link = world_.find(other);
            if (link != entt::null && link != entity && registry.all_of<Position>(link)) {
                DrawLineEx(at, drawnPosition(registry, link, alpha), line, kContagionColor);
            }
        };
        if (const auto* wounds = registry.try_get<Wounds>(entity); wounds != nullptr && isInfected(*wounds)) {
            chain(wounds->infectedBy);
        }
        const auto& events = world_.chronicle().events();
        for (const std::uint32_t index : world_.chronicle().eventsOf(selected_)) {
            const ChronicleEvent& event = events[index];
            if (event.kind == ChronicleKind::Infected && event.object == selected_) {
                chain(event.subject);
            }
        }
    }
    // Дом — рамкой, место текущего действия — крестиком, оставшийся путь — линией.
    if (const auto* person = registry.try_get<Person>(entity)) {
        if (const entt::entity home = world_.find(person->home); home != entt::null) {
            DrawRectangleLinesEx(buildingRect(registry.get<Building>(home)), line, kHomeColor);
        }
    }
    if (const auto* walker = registry.try_get<Walker>(entity); walker != nullptr && walker->moving()) {
        Vector2 from = at;
        for (std::size_t i = walker->next; i < walker->waypoints.size(); ++i) {
            const Vector2 to = cellCenter(walker->waypoints[i]);
            DrawLineEx(from, to, line, kPathColor);
            from = to;
        }
    }
    if (const auto* animal = registry.try_get<Animal>(entity)) {
        // Куда идёт — крестиком, родные места — кольцом.
        const Vector2 spot = cellCenter(animal->goal);
        const float arm = std::max(Map::kCellSize * 0.5f, 5.0f * px);
        DrawLineEx({spot.x - arm, spot.y - arm}, {spot.x + arm, spot.y + arm}, line, kTargetColor);
        DrawLineEx({spot.x - arm, spot.y + arm}, {spot.x + arm, spot.y - arm}, line, kTargetColor);
        DrawRing(cellCenter(animal->anchor), arm, arm + line, 0.0f, 360.0f, 24, kHomeColor);
    }
    if (const auto* zombie = registry.try_get<Zombie>(entity)) {
        // Куда идёт — крестиком, родные места — кольцом, вожак орды — линией.
        const Vector2 spot = cellCenter(zombie->goal);
        const float arm = std::max(Map::kCellSize * 0.5f, 5.0f * px);
        DrawLineEx({spot.x - arm, spot.y - arm}, {spot.x + arm, spot.y + arm}, line, kTargetColor);
        DrawLineEx({spot.x - arm, spot.y + arm}, {spot.x + arm, spot.y - arm}, line, kTargetColor);
        if (zombie->horde == kInvalidId) {
            const float anchor = std::max(Map::kCellSize * 0.5f, 5.0f * px);
            DrawRing(cellCenter(zombie->anchor), anchor, anchor + line, 0.0f, 360.0f, 24, kHomeColor);
        } else if (const entt::entity horde = world_.find(zombie->horde); horde != entt::null) {
            const entt::entity leader = world_.find(registry.get<Horde>(horde).leader);
            if (leader != entt::null && leader != entity) {
                DrawLineEx(at, drawnPosition(registry, leader, alpha), line, kHordeColor);
            }
        }
    }
    if (const auto* mind = registry.try_get<Mind>(entity)) {
        if (const entt::entity target = world_.find(mind->target); target != entt::null) {
            if (const auto* building = registry.try_get<Building>(target)) {
                DrawRectangleLinesEx(buildingRect(*building), line, kTargetColor);
            }
        }
        const Vector2 spot = cellCenter(mind->spot);
        const float arm = std::max(Map::kCellSize * 0.5f, 5.0f * px);
        DrawLineEx({spot.x - arm, spot.y - arm}, {spot.x + arm, spot.y + arm}, line, kTargetColor);
        DrawLineEx({spot.x - arm, spot.y + arm}, {spot.x + arm, spot.y - arm}, line, kTargetColor);
    }
    const float radius = std::max(0.9f, 8.0f * px);
    DrawRing(at, radius, radius + line, 0.0f, 360.0f, 32, kSelectionColor);
}

void Inspector::drawPanel() {
    // Слева под панелью времени, до низа окна.
    constexpr float kTop = 520.0f;
    const float height = std::max(200.0f, ImGui::GetIO().DisplaySize.y - kTop - 12.0f);
    ImGui::SetNextWindowPos({12.0f, kTop}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({360.0f, height}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Инспектор###inspector")) {
        ImGui::SetNextItemWidth(120.0f);
        const bool entered = ImGui::InputInt("##goto", &goToId_, 0, 0,
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Найти по id") || entered) {
            select(static_cast<EntityId>(std::max(goToId_, 0)));
            follow_ = selected_ != kInvalidId;
        }
        ImGui::Separator();

        const entt::registry& registry = world_.registry();
        const entt::entity entity = world_.find(selected_);
        if (selected_ == kInvalidId) {
            ImGui::TextDisabled("ЛКМ по существу, стае птиц или зданию — выбрать");
        } else if (entity == entt::null) {
            ImGui::Text("Объекта #%llu нет", static_cast<unsigned long long>(selected_));
        } else {
            if (registry.any_of<Position, Building, Horde, Group, WorldEvent>(entity)) {
                ImGui::Checkbox("Следить камерой", &follow_);
            }
            if (registry.all_of<Person>(entity)) {
                drawPerson(entity);
            } else if (registry.all_of<Zombie>(entity)) {
                drawZombie(entity);
            } else if (registry.all_of<Animal>(entity)) {
                drawAnimal(entity);
            } else if (registry.all_of<Flock>(entity)) {
                drawFlock(entity);
            } else if (registry.all_of<Corpse>(entity)) {
                drawCorpse(entity);
            } else if (registry.all_of<Horde>(entity)) {
                drawHorde(entity);
            } else if (registry.all_of<Group>(entity)) {
                drawGroup(entity);
            } else if (registry.all_of<WorldEvent>(entity)) {
                drawEvent(entity);
            } else if (registry.all_of<Cache>(entity)) {
                drawCache(entity);
            } else if (registry.all_of<Scenario>(entity)) {
                drawScenario(entity);
            } else if (registry.all_of<Building>(entity)) {
                drawBuilding(entity);
            } else if (registry.all_of<Settlement>(entity)) {
                drawSettlement(entity);
            } else {
                ImGui::Text("Объект #%llu", static_cast<unsigned long long>(selected_));
            }
        }
        if (selected_ != kInvalidId) {
            drawBiography();
        }
    }
    ImGui::End();
}

void Inspector::drawBiography() {
    const Chronicle& chronicle = world_.chronicle();
    const std::span<const std::uint32_t> indices = chronicle.eventsOf(selected_);
    char header[64];
    std::snprintf(header, sizeof(header), "Летопись: %zu###biography", indices.size());
    if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    if (indices.empty()) {
        ImGui::TextDisabled("событий нет");
        return;
    }
    EntityId clicked = kInvalidId;
    // Новые сверху; рисуются только видимые строки.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(indices.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const std::uint32_t index = indices[indices.size() - 1 - static_cast<std::size_t>(row)];
            ImGui::PushID(static_cast<int>(index));
            const EntityId id = drawChronicleEvent(chronicle.events()[index], selected_);
            ImGui::PopID();
            if (id != kInvalidId) {
                clicked = id;
            }
        }
    }
    if (clicked != kInvalidId) {
        select(clicked);
    }
}

void Inspector::drawPerson(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const std::uint64_t tick = world_.tick();
    char duration[32];

    ImGui::Text("Человек %s", idLabel(selected_).c_str());
    if (registry.all_of<Age>(entity)) {
        const int years = ageOf(registry, entity);
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", ageText(years).c_str(), years < kAdultAge ? ", ребёнок" : "");
    }
    if (const auto* mind = registry.try_get<Mind>(entity)) {
        const auto* job = registry.try_get<Job>(entity);
        const bool lesson = mind->action == Action::Work && job != nullptr && job->profession == Profession::Pupil;
        ImGui::Text("Сейчас: %s%s%s", lesson ? "в школе" : actionName(mind->action),
                    mind->phase == ActionPhase::Travel ? " (в пути)" : "",
                    registry.get<Person>(entity).indoors ? " (дома, в укрытии)" : "");
        if (mind->target != kInvalidId) {
            ImGui::SameLine();
            ImGui::TextUnformatted("цель");
            ImGui::SameLine();
            if (idLink(mind->target)) {
                select(mind->target);
                return;
            }
        }
        if (mind->action == Action::Forage) {
            ImGui::Text("Ягоды и грибы на месте сбора: %d из %d",
                        world_.resources().forestAt(mind->spot), Resources::kForestMax);
        }
        ImGui::Text("Оценка при выборе: %.2f", mind->score);
        if (mind->phase == ActionPhase::Perform &&
            mind->until != std::numeric_limits<std::uint64_t>::max() && mind->until > tick) {
            formatGameDuration(duration, sizeof(duration), mind->until - tick);
            ImGui::Text("До конца: %s", duration);
        }
        formatGameDuration(duration, sizeof(duration),
                           mind->nextThink > tick ? mind->nextThink - tick : 0);
        ImGui::Text("Пересмотр решения через %s", duration);
        if (mind->failures > 0) {
            ImGui::TextColored({1.0f, 0.55f, 0.3f, 1.0f}, "Не нашлось пути: %u раз подряд",
                               mind->failures);
        }
        if (mind->emptySearches > 0) {
            ImGui::Text("Обысков без еды подряд: %d", mind->emptySearches);
        }
        if (mind->fledAt > 0) {
            formatGameDuration(duration, sizeof(duration), tick - mind->fledAt);
            ImGui::Text("Бежал от зомби %s назад", duration);
        }
        int dangers = 0;
        float nearest = 0.0f;
        const Position& at = registry.get<Position>(entity);
        for (const Danger& danger : mind->dangers) {
            if (danger.until > tick) {
                const float d = std::hypot(Map::cellCenter(danger.cell.x) - at.x,
                                           Map::cellCenter(danger.cell.y) - at.y);
                nearest = dangers++ == 0 ? d : std::min(nearest, d);
            }
        }
        if (dangers > 0) {
            ImGui::Text("Помнит опасных мест: %d, ближайшее в %.0f м", dangers, nearest);
        }
    }

    const Person& person = registry.get<Person>(entity);
    if (const entt::entity home = world_.find(person.home); home != entt::null) {
        ImGui::TextUnformatted("Дом:");
        ImGui::SameLine();
        if (idLink(person.home)) {
            select(person.home);
            return;
        }
        ImGui::SameLine();
        const auto* supplies = registry.try_get<Inventory>(home);
        ImGui::Text("%s, еды %d, воды %d", buildingName(registry.get<Building>(home).kind),
                    supplies != nullptr ? supplies->food() : 0,
                    supplies != nullptr ? supplies->count(ItemKind::Water) : 0);
    } else {
        ImGui::TextUnformatted("Дом: нет");
    }
    // Группа (этап 6.4).
    if (const entt::entity group = world_.find(person.group); group != entt::null) {
        const Group& info = registry.get<Group>(group);
        ImGui::TextUnformatted("Группа:");
        ImGui::SameLine();
        if (idLink(person.group)) {
            select(person.group);
            return;
        }
        ImGui::SameLine();
        if (info.leader == selected_) {
            ImGui::Text("вожак, %zu человек", info.members.size());
        } else {
            ImGui::Text("%zu человек, вожак", info.members.size());
            ImGui::SameLine();
            if (idLink(info.leader)) {
                select(info.leader);
                return;
            }
        }
        if (person.formerHome != kInvalidId) {
            ImGui::TextUnformatted("Живёт на базе; прежний дом");
            ImGui::SameLine();
            if (idLink(person.formerHome)) {
                select(person.formerHome);
                return;
            }
        }
    }
    // Работа и деньги (этап 7.2); школьник — учёба (этап 7.3).
    if (const auto* job = registry.try_get<Job>(entity);
        job != nullptr && job->profession != Profession::None) {
        if (job->profession == Profession::Pupil) {
            ImGui::TextUnformatted("Учится в школе");
        } else {
            ImGui::Text("Работа: %s,", professionName(job->profession));
        }
        ImGui::SameLine();
        if (idLink(job->workplace)) {
            select(job->workplace);
            return;
        }
        char shift[96];
        formatShift(shift, sizeof(shift), *job);
        const bool now = onShift(*job, calendarAt(tick));
        const auto* traits = registry.try_get<Traits>(entity);
        if (!keepsWorking(world_, entity)) {
            // Фаза мира (этап 7.4): на работу больше не выходит.
            ImGui::Text("Смена %s; бросил работу (%s, долг %.2f)", shift,
                        phaseName(worldPhase(world_)),
                        traits != nullptr ? dutyOf(job->profession, *traits) : 0.0f);
        } else {
            ImGui::Text("Смена %s; %s", shift,
                        !now ? "сейчас не его смена"
                        : isOnDuty(world_, entity) ? "сейчас на месте"
                                                   : "сейчас смена, а его нет на месте");
        }
    } else if (registry.all_of<Wallet>(entity)) {
        ImGui::TextUnformatted("Работа: нет");
    }
    if (const auto* wallet = registry.try_get<Wallet>(entity)) {
        ImGui::Text("Деньги: %d", wallet->money);
    }
    if (const auto* carry = registry.try_get<Inventory>(entity)) {
        char text[256];
        formatInventory(text, sizeof(text), *carry);
        ImGui::TextWrapped("Несёт (%.1f из %.0f кг): %s", inventoryWeight(*carry), kCarryCapacity,
                           text);
    }
    if (const auto* weapon = registry.try_get<Weapon>(entity)) {
        if (isFirearm(weapon->kind)) {
            ImGui::Text("Оружие: %s, патронов %d", weaponName(weapon->kind), weapon->ammo);
        } else {
            ImGui::Text("Оружие: %s", weaponName(weapon->kind));
        }
    }
    if (const auto* combat = registry.try_get<Combat>(entity);
        combat != nullptr && combat->target != kInvalidId) {
        ImGui::TextUnformatted("Бьёт или целится в");
        ImGui::SameLine();
        if (idLink(combat->target)) {
            select(combat->target);
            return;
        }
    }
    if (const auto* wounds = registry.try_get<Wounds>(entity)) {
        if (wounds->bleeding > 0.0f) {
            ImGui::TextColored({1.0f, 0.4f, 0.35f, 1.0f}, "Кровотечение: %.0f%% здоровья в час",
                               wounds->bleeding * 100.0f);
        }
        if (wounds->bites > 0) {
            ImGui::TextColored({1.0f, 0.4f, 0.35f, 1.0f}, "Укушен: %d раз", wounds->bites);
        }
        if (isInfected(*wounds)) {
            meter("Заражение", wounds->infection);
            // Как, от кого и когда (этап 7.5) — зритель знает, сам больной нет.
            char ago[32];
            formatGameDuration(ago, sizeof(ago), world_.tick() - wounds->infectedAt);
            ImGui::Text("Заразился: %s, %s назад", infectionRouteText(wounds->route), ago);
            if (wounds->infectedBy != kInvalidId && wounds->route != InfectionRoute::Origin) {
                ImGui::SameLine();
                ImGui::TextUnformatted("от");
                ImGui::SameLine();
                if (idLink(wounds->infectedBy)) {
                    select(wounds->infectedBy);
                    return;
                }
            }
            char left[32];
            if (!isFeverish(*wounds)) {
                formatGameDuration(left, sizeof(left),
                                   static_cast<std::uint64_t>(hoursToFever(*wounds) * kTicksPerGameHour));
                ImGui::Text("Скрытый период: жар через ~%s", left);
            } else {
                formatGameDuration(left, sizeof(left),
                                   static_cast<std::uint64_t>(hoursToEnd(*wounds) * kTicksPerGameHour));
                ImGui::TextColored({1.0f, 0.55f, 0.25f, 1.0f}, "Лихорадка: до конца ~%s", left);
            }
            if (const float shedding = contagiousness(*wounds); shedding > 0.0f) {
                ImGui::TextColored({0.7f, 0.95f, 0.35f, 1.0f}, "Заразен%s",
                                   shedding < 1.0f ? " (слабо)" : "");
            }
        }
        // Кого заразил сам (по летописи).
        int spread = 0;
        const auto& events = world_.chronicle().events();
        for (const std::uint32_t index : world_.chronicle().eventsOf(selected_)) {
            spread += events[index].kind == ChronicleKind::Infected && events[index].object == selected_ ? 1 : 0;
        }
        if (spread > 0) {
            ImGui::Text("Заразил сам: %d", spread);
        }
    }

    if (const auto* needs = registry.try_get<Needs>(entity)) {
        ImGui::SeparatorText("Нужды");
        meter("Голод", needs->hunger);
        meter("Жажда", needs->thirst);
        meter("Усталость", needs->fatigue);
        meter("Страх", needs->fear);
        meter("Одиночество", needs->loneliness);
        meter("Здоровье", needs->health);
    }

    if (!drawPerception(entity)) {
        return;
    }

    if (const auto* traits = registry.try_get<Traits>(entity);
        traits != nullptr && ImGui::CollapsingHeader("Черты характера", ImGuiTreeNodeFlags_DefaultOpen)) {
        const std::pair<const char*, float> rows[] = {
            {"Смелость", traits->bravery},       {"Жадность", traits->greed},
            {"Альтруизм", traits->altruism},     {"Агрессия", traits->aggression},
            {"Лидерство", traits->leadership},   {"Общительность", traits->sociability},
            {"Любопытство", traits->curiosity},  {"Запасливость", traits->prudence},
            {"Сова", traits->chronotype},
        };
        if (ImGui::BeginTable("traits", 4, ImGuiTableFlags_SizingStretchProp)) {
            for (const auto& [name, value] : rows) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", value * 100.0f);
            }
            ImGui::EndTable();
        }
    }

    if (!drawFamily(entity) || !drawRelations(entity) || !drawMemories(entity) || !drawLeads(entity)) {
        return;
    }

    if (registry.all_of<Mind, Needs>(entity) && ImGui::CollapsingHeader("Оценки действий")) {
        const ActionScores scores = evaluateActions(world_, entity);
        const Action current = registry.get<Mind>(entity).action;
        for (std::size_t i = 0; i < scores.size(); ++i) {
            const auto action = static_cast<Action>(i);
            ImGui::Text("%s%s: %.2f", action == current ? "> " : "  ", actionName(action),
                        scores[i]);
        }
    }

    drawMovement(entity);

    if (const auto* mind = registry.try_get<Mind>(entity);
        mind != nullptr && ImGui::CollapsingHeader("Память: обысканные здания")) {
        int shown = 0;
        // От последнего обысканного к первому.
        for (std::size_t k = 0; k < Mind::kSearchedMemory; ++k) {
            const std::size_t index =
                (mind->searchedNext + Mind::kSearchedMemory - 1 - k) % Mind::kSearchedMemory;
            const EntityId building = mind->searched[index];
            if (building == kInvalidId) {
                continue;
            }
            if (shown++ % 6 != 0) {
                ImGui::SameLine();
            }
            if (idLink(building)) {
                select(building);
                return;
            }
        }
        if (shown == 0) {
            ImGui::TextDisabled("пусто");
        }
    }
}

void Inspector::drawMovement(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    if (ImGui::CollapsingHeader("Движение")) {
        const Position& position = registry.get<Position>(entity);
        const Cell cell = Map::cellAt(position.x, position.y);
        ImGui::Text("Позиция: %.1f, %.1f м (клетка %d, %d)", position.x, position.y, cell.x, cell.y);
        if (const auto* velocity = registry.try_get<Velocity>(entity)) {
            ImGui::Text("Скорость: %.2f м/с", std::hypot(velocity->x, velocity->y));
        }
        if (const auto* walker = registry.try_get<Walker>(entity)) {
            if (walker->moving()) {
                float left = 0.0f;
                Vector2 from = {position.x, position.y};
                for (std::size_t i = walker->next; i < walker->waypoints.size(); ++i) {
                    const Vector2 to = cellCenter(walker->waypoints[i]);
                    left += std::hypot(to.x - from.x, to.y - from.y);
                    from = to;
                }
                ImGui::Text("Путь: осталось %.0f м, точек %zu из %zu", left,
                            walker->waypoints.size() - walker->next, walker->waypoints.size());
            } else {
                ImGui::TextUnformatted("Путь: стоит на месте");
            }
            ImGui::Text("Шаг по дороге: %.2f м/с", walker->speed);
        }
    }
}

bool Inspector::drawPerception(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const auto* senses = registry.try_get<Senses>(entity);
    const auto* awareness = registry.try_get<Awareness>(entity);
    if (senses == nullptr || awareness == nullptr ||
        !ImGui::CollapsingHeader("Восприятие", ImGuiTreeNodeFlags_DefaultOpen)) {
        return true;
    }
    const SenseConditions conditions = senseConditions(world_);
    const bool asleep = isAsleep(registry, entity);
    const float sight = sightRange(*senses, asleep, conditions);
    const float hearing = hearingFactor(*senses, asleep, conditions);
    if (sight > 0.0f) {
        ImGui::Text("Видит идущего до %.0f м (стоящего — до %.0f м)", sight,
                    sight * kStillVisibility);
    } else {
        ImGui::TextUnformatted("Спит: не видит, слышит хуже");
    }
    ImGui::Text("Слышит шаги до %.0f м, обыск до %.0f м", kFootstepsLoudness * hearing,
                kRummageLoudness * hearing);
    if (awareness->count == 0) {
        ImGui::TextDisabled("никого не замечает");
        return true;
    }
    const Position& position = registry.get<Position>(entity);
    const std::uint64_t tick = world_.tick();
    if (!ImGui::BeginTable("contacts", 3, ImGuiTableFlags_SizingFixedFit)) {
        return true;
    }
    EntityId clicked = kInvalidId;
    char ago[32];
    for (std::size_t i = 0; i < awareness->count; ++i) {
        const Contact& contact = awareness->contacts[i];
        ImGui::TableNextColumn();
        if (idLink(contact.id)) {
            clicked = contact.id;
        }
        ImGui::TableNextColumn();
        if (!isFresh(contact, tick)) {
            formatGameDuration(ago, sizeof(ago), tick - contact.tick);
            ImGui::TextDisabled("помнит, %s назад", ago);
        } else if (contact.seen && contact.heard != SoundKind::None) {
            ImGui::Text("видит, слышит: %s", soundName(contact.heard));
        } else if (contact.seen) {
            ImGui::TextUnformatted("видит");
        } else {
            ImGui::Text("слышит: %s", soundName(contact.heard));
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.0f м", std::hypot(contact.x - position.x, contact.y - position.y));
    }
    ImGui::EndTable();
    if (clicked != kInvalidId) {
        select(clicked);
        return false;
    }
    return true;
}

bool Inspector::drawFamily(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const auto* kin = registry.try_get<Kin>(entity);
    if (kin == nullptr) {
        return true;
    }
    char header[64];
    std::snprintf(header, sizeof(header), "Семья: %d###family", kin->count);
    if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
        return true;
    }
    if (kin->count == 0) {
        ImGui::TextDisabled("родни нет");
    }
    const auto* person = registry.try_get<Person>(entity);
    EntityId clicked = kInvalidId;
    if (kin->count > 0 && ImGui::BeginTable("family", 4, ImGuiTableFlags_SizingFixedFit)) {
        for (std::size_t i = 0; i < kin->count; ++i) {
            const Relative& relative = kin->relatives[i];
            const entt::entity other = world_.find(relative.id);
            const auto* name = other != entt::null ? registry.try_get<Name>(other) : nullptr;
            const bool female = name != nullptr && isFemaleName(name->first);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kinWord(relative.kind, female));
            ImGui::TableNextColumn();
            if (idLink(relative.id)) {
                clicked = relative.id;
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", ageText(ageOf(registry, other)).c_str());
            ImGui::TableNextColumn();
            if (other == entt::null || registry.all_of<Corpse>(other)) {
                ImGui::TextDisabled("погиб");
            } else if (registry.all_of<Zombie>(other)) {
                ImGui::TextColored({1.0f, 0.4f, 0.35f, 1.0f}, "стал зомби");
            } else if (const auto* theirs = registry.try_get<Person>(other);
                       theirs != nullptr && person != nullptr && theirs->home != person->home) {
                ImGui::TextDisabled("живёт отдельно");
            } else {
                ImGui::TextUnformatted("дома вместе");
            }
        }
        ImGui::EndTable();
    }
    if (registry.all_of<Person>(entity) && isMinor(registry, entity)) {
        if (kin->guardian != kInvalidId) {
            ImGui::TextUnformatted("Опекун:");
            ImGui::SameLine();
            if (idLink(kin->guardian)) {
                clicked = kin->guardian;
            }
        } else {
            ImGui::TextColored({1.0f, 0.6f, 0.4f, 1.0f}, "Опекуна нет — сам по себе");
        }
    }
    if (clicked != kInvalidId) {
        select(clicked);
        return false;
    }
    return true;
}

bool Inspector::drawRelations(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const auto* relations = registry.try_get<Relations>(entity);
    if (relations == nullptr) {
        return true;
    }
    char header[64];
    std::snprintf(header, sizeof(header), "Отношения: %d###relations", relations->count);
    if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
        return true;
    }
    // Как к нему относятся другие (этап 6.3).
    if (const Reputation reputation = reputationOf(world_, selected_); reputation.known > 0) {
        ImGui::Text("Репутация: симпатия %+.0f у %d знакомых", reputation.affinity * 100.0f,
                    reputation.known);
    } else {
        ImGui::TextDisabled("Репутация: его почти никто не знает");
    }
    if (relations->count == 0) {
        ImGui::TextDisabled("никого не знает");
        return true;
    }
    // Сначала самые крепкие связи.
    std::vector<const Bond*> bonds;
    for (std::size_t i = 0; i < relations->count; ++i) {
        bonds.push_back(&relations->bonds[i]);
    }
    std::stable_sort(bonds.begin(), bonds.end(), [](const Bond* a, const Bond* b) {
        return bondStrength(*a) > bondStrength(*b);
    });
    ImGui::TextDisabled("знакомство, симпатия, доверие (%%); совместимость; долг, обида");
    if (!ImGui::BeginTable("relations", 6, ImGuiTableFlags_SizingFixedFit)) {
        return true;
    }
    const std::uint64_t tick = world_.tick();
    EntityId clicked = kInvalidId;
    char ago[32];
    for (const Bond* bond : bonds) {
        ImGui::TableNextColumn();
        if (idLink(bond->other)) {
            clicked = bond->other;
        }
        ImGui::TableNextColumn();
        ImGui::Text("%3.0f %+4.0f %+4.0f", bond->familiarity * 100.0f, bond->affinity * 100.0f,
                    bond->trust * 100.0f);
        ImGui::TableNextColumn();
        ImGui::Text("%+.2f", bond->chemistry);
        ImGui::TableNextColumn();
        if (bond->debt > 0.0f && bond->grudge > 0.0f) {
            ImGui::Text("долг %.0f, обида %.0f", bond->debt * 100.0f, bond->grudge * 100.0f);
        } else if (bond->debt > 0.0f) {
            ImGui::TextColored({0.55f, 0.85f, 0.55f, 1.0f}, "долг %.0f", bond->debt * 100.0f);
        } else if (bond->grudge > 0.0f) {
            ImGui::TextColored({1.0f, 0.6f, 0.4f, 1.0f}, "обида %.0f", bond->grudge * 100.0f);
        }
        ImGui::TableNextColumn();
        const entt::entity other = world_.find(bond->other);
        if (other == entt::null || registry.all_of<Corpse>(other)) {
            ImGui::TextDisabled("погиб");
        } else if (registry.all_of<Zombie>(other)) {
            ImGui::TextColored({1.0f, 0.4f, 0.35f, 1.0f}, "стал зомби");
        } else if (bond->friendly()) {
            ImGui::TextColored({1.0f, 0.6f, 0.86f, 1.0f}, "друг");
        } else if (bond->hostile()) {
            ImGui::TextColored({1.0f, 0.4f, 0.35f, 1.0f}, "недруг");
        } else {
            ImGui::TextDisabled("знакомый");
        }
        // Откуда знакомы (этап 7.3): родня — кем приходится.
        {
            const auto* name = other != entt::null ? registry.try_get<Name>(other) : nullptr;
            const bool female = name != nullptr && isFemaleName(name->first);
            const KinKind kind = kinOf(registry, entity, bond->other);
            const char* origin = kind != KinKind::Count ? kinWord(kind, female) : bondOriginName(bond->origin, female);
            if (origin[0] != '\0') {
                ImGui::SameLine();
                ImGui::TextDisabled("· %s", origin);
            }
        }
        ImGui::TableNextColumn();
        if (tick - bond->seen <= kTicksPerGameMinute) { // вместе в последнюю минуту
            ImGui::TextUnformatted("рядом");
        } else {
            formatGameDuration(ago, sizeof(ago), tick - bond->seen);
            ImGui::TextDisabled("%s назад", ago);
        }
    }
    ImGui::EndTable();
    if (clicked != kInvalidId) {
        select(clicked);
        return false;
    }
    return true;
}

bool Inspector::drawMemories(entt::entity entity) {
    const auto* memories = world_.registry().try_get<Memories>(entity);
    if (memories == nullptr) {
        return true;
    }
    char header[64];
    std::snprintf(header, sizeof(header), "Память: %d###memories", memories->count);
    if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
        return true;
    }
    if (memories->count == 0) {
        ImGui::TextDisabled("ничего значимого");
        return true;
    }
    // Сначала свежие.
    std::vector<const Memory*> items;
    for (std::size_t i = 0; i < memories->count; ++i) {
        items.push_back(&memories->items[i]);
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const Memory* a, const Memory* b) { return a->tick > b->tick; });
    ImGui::TextDisabled("кто, что сделал (→ с кем, если не с ним), при чём; сила (%%); "
                        "когда, от кого слышал");
    if (!ImGui::BeginTable("memories", 5, ImGuiTableFlags_SizingFixedFit)) {
        return true;
    }
    const std::uint64_t tick = world_.tick();
    EntityId clicked = kInvalidId;
    char ago[32];
    for (const Memory* memory : items) {
        ImGui::PushID(memory);
        ImGui::TableNextColumn();
        if (idLink(memory->who)) {
            clicked = memory->who;
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(memoryKindName(memory->kind));
        // Чужой поступок (этап 6.3) — с кем.
        if (memory->victim != kInvalidId) {
            ImGui::SameLine();
            ImGui::TextUnformatted("→");
            ImGui::SameLine();
            if (idLink(memory->victim)) {
                clicked = memory->victim;
            }
        }
        ImGui::TableNextColumn();
        if (memory->about != kInvalidId && idLink(memory->about)) {
            clicked = memory->about;
        }
        ImGui::TableNextColumn();
        ImGui::Text("%3.0f", memory->strength * 100.0f);
        ImGui::TableNextColumn();
        formatGameDuration(ago, sizeof(ago), tick > memory->tick ? tick - memory->tick : 0);
        ImGui::TextDisabled("%s назад", ago);
        if (memory->source != kInvalidId) {
            ImGui::SameLine();
            ImGui::TextDisabled("слух от");
            ImGui::SameLine();
            if (idLink(memory->source)) {
                clicked = memory->source;
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (clicked != kInvalidId) {
        select(clicked);
        return false;
    }
    return true;
}

void Inspector::drawZombie(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const std::uint64_t tick = world_.tick();
    const Zombie& zombie = registry.get<Zombie>(entity);
    char duration[32];

    ImGui::Text("Зомби %s", idLabel(selected_).c_str());
    ImGui::Text("Сейчас: %s", zombieStateName(zombie.state));
    if (zombie.target != kInvalidId) {
        ImGui::SameLine();
        ImGui::TextUnformatted("добыча");
        ImGui::SameLine();
        if (idLink(zombie.target)) {
            select(zombie.target);
            return;
        }
    }
    if (const entt::entity horde = world_.find(zombie.horde); horde != entt::null) {
        ImGui::TextUnformatted("Орда:");
        ImGui::SameLine();
        if (idLink(zombie.horde)) {
            select(zombie.horde);
            return;
        }
        const Horde& info = registry.get<Horde>(horde);
        ImGui::SameLine();
        ImGui::Text("%zu зомби%s", info.members.size(), info.leader == selected_ ? ", вожак" : "");
    } else {
        const Position& position = registry.get<Position>(entity);
        ImGui::Text("Одиночка, родные места в %.0f м",
                    std::hypot(Map::cellCenter(zombie.anchor.x) - position.x,
                               Map::cellCenter(zombie.anchor.y) - position.y));
    }
    if ((zombie.state == ZombieState::Idle || zombie.state == ZombieState::Investigate) &&
        zombie.until > tick) {
        formatGameDuration(duration, sizeof(duration), zombie.until - tick);
        ImGui::Text("Стоит ещё %s", duration);
    }
    if (zombie.alerted > 0) {
        formatGameDuration(duration, sizeof(duration), tick - zombie.alerted);
        ImGui::Text("Добычу замечал %s назад", duration);
    }
    if (const auto* walker = registry.try_get<Walker>(entity)) {
        ImGui::Text("Погоня: %.2f м/с, сейчас темп %.0f%%", walker->speed, walker->pace * 100.0f);
    }
    meter("Здоровье", zombie.health);
    if (const auto* weapon = registry.try_get<Weapon>(entity)) {
        ImGui::Text("При себе с прошлой жизни: %s", weaponName(weapon->kind));
    }
    if (!drawPerception(entity)) {
        return;
    }
    drawMovement(entity);
}

void Inspector::drawHorde(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const std::uint64_t tick = world_.tick();
    const Horde& horde = registry.get<Horde>(entity);
    char duration[32];

    ImGui::Text("Орда #%llu: %zu зомби", static_cast<unsigned long long>(selected_),
                horde.members.size());
    ImGui::TextUnformatted("Вожак:");
    ImGui::SameLine();
    if (idLink(horde.leader)) {
        select(horde.leader);
        return;
    }
    if (const entt::entity leader = world_.find(horde.leader); leader != entt::null) {
        if (const auto* zombie = registry.try_get<Zombie>(leader)) {
            ImGui::SameLine();
            ImGui::TextUnformatted(zombieStateName(zombie->state));
        }
    }
    const float toGoal = std::hypot(Map::cellCenter(horde.goal.x) - horde.x,
                                    Map::cellCenter(horde.goal.y) - horde.y);
    if (horde.travelling) {
        ImGui::Text("Идёт к клетке %d, %d: %.0f м", horde.goal.x, horde.goal.y, toGoal);
    } else if (horde.until > tick) {
        formatGameDuration(duration, sizeof(duration), horde.until - tick);
        ImGui::Text("Стоит ещё %s", duration);
    } else {
        ImGui::TextUnformatted("Сейчас тронется");
    }
    if (const entt::entity settlement = world_.find(horde.settlement); settlement != entt::null) {
        ImGui::Text("В поселении %s", registry.get<Settlement>(settlement).name.c_str());
        ImGui::SameLine();
        if (idLink(horde.settlement)) {
            select(horde.settlement);
            return;
        }
    } else {
        ImGui::TextUnformatted("Вне поселений");
    }
    ImGui::SeparatorText("Участники");
    EntityId clicked = kInvalidId;
    for (std::size_t i = 0; i < horde.members.size(); ++i) {
        if (i % 6 != 0) {
            ImGui::SameLine();
        }
        if (idLink(horde.members[i])) {
            clicked = horde.members[i];
        }
    }
    if (clicked != kInvalidId) {
        select(clicked);
    }
}

void Inspector::drawGroup(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const Group& group = registry.get<Group>(entity);
    char duration[32];

    ImGui::Text("Группа #%llu: %zu человек", static_cast<unsigned long long>(selected_),
                group.members.size());
    formatGameDuration(duration, sizeof(duration), world_.tick() - group.formed);
    ImGui::Text("Вместе %s", duration);
    ImGui::TextUnformatted("Вожак:");
    ImGui::SameLine();
    if (idLink(group.leader)) {
        select(group.leader);
        return;
    }
    if (const entt::entity base = world_.find(group.base); base != entt::null) {
        ImGui::TextUnformatted("База:");
        ImGui::SameLine();
        if (idLink(group.base)) {
            select(group.base);
            return;
        }
        drawSupplies(registry, base);
    } else {
        ImGui::TextUnformatted("Базы нет");
    }
    // Участники: авторитет (лидерство и доверие остальных) и как к нему
    // относятся остальные — средняя симпатия.
    ImGui::SeparatorText("Участники");
    ImGui::TextDisabled("авторитет; симпатия остальных (%%); занятие");
    if (!ImGui::BeginTable("members", 4, ImGuiTableFlags_SizingFixedFit)) {
        return;
    }
    EntityId clicked = kInvalidId;
    for (const EntityId member : group.members) {
        ImGui::TableNextColumn();
        if (idLink(member)) {
            clicked = member;
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.2f%s", authorityOf(world_, group, member),
                    member == group.leader ? " вожак" : "");
        ImGui::TableNextColumn();
        float liking = 0.0f;
        int others = 0;
        for (const EntityId other : group.members) {
            const entt::entity mate = world_.find(other);
            const auto* relations =
                mate != entt::null && other != member ? registry.try_get<Relations>(mate) : nullptr;
            if (relations != nullptr) {
                const Bond* bond = relations->find(member);
                liking += bond != nullptr ? bond->affinity : 0.0f;
                ++others;
            }
        }
        ImGui::Text("%+.0f", others > 0 ? liking / static_cast<float>(others) * 100.0f : 0.0f);
        ImGui::TableNextColumn();
        const entt::entity person = world_.find(member);
        const auto* mind = person != entt::null ? registry.try_get<Mind>(person) : nullptr;
        ImGui::TextUnformatted(mind != nullptr ? actionName(mind->action) : "—");
    }
    ImGui::EndTable();
    if (clicked != kInvalidId) {
        select(clicked);
    }
}

void Inspector::drawAnimal(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const std::uint64_t tick = world_.tick();
    const Animal& animal = registry.get<Animal>(entity);
    const AnimalSpec& spec = animalSpec(animal.kind);
    char duration[32];

    ImGui::Text("Животное #%llu: %s", static_cast<unsigned long long>(selected_),
                animalKindName(animal.kind));
    ImGui::Text("Сейчас: %s", animalStateName(animal.state));
    if (animal.state == AnimalState::Flee && animal.threat != kInvalidId) {
        ImGui::SameLine();
        ImGui::TextUnformatted("от");
        ImGui::SameLine();
        if (idLink(animal.threat)) {
            select(animal.threat);
            return;
        }
    }
    if (animal.state == AnimalState::Graze && animal.until > tick) {
        formatGameDuration(duration, sizeof(duration), animal.until - tick);
        ImGui::Text("Пасётся ещё %s", duration);
    }
    const Position& position = registry.get<Position>(entity);
    ImGui::Text("Родные места в %.0f м", std::hypot(Map::cellCenter(animal.anchor.x) - position.x,
                                                    Map::cellCenter(animal.anchor.y) - position.y));
    ImGui::Text("Убегает от увиденного ближе %.0f м, от услышанного — ближе %.0f м",
                spec.fleeSeen, spec.fleeHeard);
    ImGui::Text("Бегом %.1f м/с, шагом %.1f м/с", spec.runSpeed, spec.walkSpeed);
    meter("Здоровье", animal.health);
    if (!drawPerception(entity)) {
        return;
    }
    drawMovement(entity);
}

void Inspector::drawFlock(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const std::uint64_t tick = world_.tick();
    const Flock& flock = registry.get<Flock>(entity);
    char duration[32];
    ImGui::Text("Стая птиц #%llu: %d ворон", static_cast<unsigned long long>(selected_),
                flock.birds);
    const Position& position = registry.get<Position>(entity);
    if (flock.flying) {
        ImGui::Text("Летит: ещё %.0f м", std::hypot(flock.toX - position.x, flock.toY - position.y));
    } else if (flock.until > tick) {
        formatGameDuration(duration, sizeof(duration), flock.until - tick);
        ImGui::Text("Сидит, сама перелетит через %s", duration);
    } else {
        ImGui::TextUnformatted("Сейчас взлетит");
    }
    ImGui::TextDisabled("Вспугнутая взлетает с криком: его слышат зомби");
}

void Inspector::drawCorpse(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const Corpse& corpse = registry.get<Corpse>(entity);
    char duration[32];
    if (corpse.kind == CorpseKind::Animal) {
        ImGui::Text("Туша #%llu: %s", static_cast<unsigned long long>(selected_),
                    animalKindName(corpse.animal));
        ImGui::Text("Мяса осталось: %d порций", corpse.meat);
    } else {
        ImGui::Text("Тело %s: %s", idLabel(selected_).c_str(), corpseKindName(corpse.kind));
    }
    formatGameDuration(duration, sizeof(duration), world_.tick() - corpse.diedAt);
    ImGui::Text("Погиб %s назад: %s", duration, deathCauseName(corpse.cause));
    if (corpse.turnsAt > world_.tick()) {
        formatGameDuration(duration, sizeof(duration), corpse.turnsAt - world_.tick());
        ImGui::TextColored({1.0f, 0.4f, 0.35f, 1.0f}, "Укушен: встанет зомби через %s", duration);
    }
    if (corpse.killer != kInvalidId) {
        ImGui::TextUnformatted("Кто убил:");
        ImGui::SameLine();
        if (idLink(corpse.killer)) {
            select(corpse.killer);
            return;
        }
    }
    if (const auto* weapon = registry.try_get<Weapon>(entity)) {
        ImGui::Text("При теле: %s", weaponName(weapon->kind));
        if (isFirearm(weapon->kind)) {
            ImGui::SameLine();
            ImGui::Text("(патронов %d)", weapon->ammo);
        }
    }
    if (const auto* carry = registry.try_get<Inventory>(entity); carry != nullptr && !carry->empty()) {
        char text[256];
        formatInventory(text, sizeof(text), *carry);
        ImGui::TextWrapped("При теле: %s", text);
    }
}

bool Inspector::drawLeads(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const auto* leads = registry.try_get<Leads>(entity);
    if (leads == nullptr || leads->count == 0) {
        return true;
    }
    char header[64];
    std::snprintf(header, sizeof(header), "Вести об ивентах: %d###leads", leads->count);
    if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
        return true;
    }
    const Position& at = registry.get<Position>(entity);
    char duration[32];
    for (std::size_t i = 0; i < leads->count; ++i) {
        const Lead& lead = leads->items[i];
        ImGui::PushID(static_cast<int>(i));
        if (idLink(lead.event)) {
            ImGui::PopID();
            select(lead.event);
            return false;
        }
        ImGui::SameLine();
        formatGameDuration(duration, sizeof(duration), world_.tick() - lead.since);
        ImGui::Text("в %.0f м, узнали %s назад;",
                    std::hypot(Map::cellCenter(lead.site.x) - at.x,
                               Map::cellCenter(lead.site.y) - at.y),
                    duration);
        ImGui::SameLine();
        if (lead.empty) {
            ImGui::TextUnformatted("там пусто");
        } else if (lead.visited) {
            ImGui::TextUnformatted("побывал");
        } else if (!leadFresh(lead, world_.tick())) {
            ImGui::TextDisabled("устарела");
        } else {
            ImGui::Text("свежесть %.0f%%", leadFreshness(lead, world_.tick()) * 100.0f);
        }
        if (lead.source != kInvalidId) {
            ImGui::SameLine();
            ImGui::TextUnformatted("рассказал");
            ImGui::SameLine();
            if (idLink(lead.source)) {
                ImGui::PopID();
                select(lead.source);
                return false;
            }
        }
        ImGui::PopID();
    }
    return true;
}

void Inspector::drawEvent(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const WorldEvent& event = registry.get<WorldEvent>(entity);
    const std::uint64_t tick = world_.tick();
    char duration[32];

    ImGui::Text("Ивент #%llu: %s", static_cast<unsigned long long>(selected_),
                eventKindName(event.kind));
    formatGameDuration(duration, sizeof(duration), tick - event.began);
    ImGui::Text("Начался %s назад; место (%.0f, %.0f) м", duration,
                Map::cellCenter(event.site.x), Map::cellCenter(event.site.y));
    if (event.settlement != kInvalidId) {
        ImGui::TextUnformatted("Ближайшее поселение:");
        ImGui::SameLine();
        if (idLink(event.settlement)) {
            select(event.settlement);
            return;
        }
    }
    if (tick < event.signalUntil) {
        formatGameDuration(duration, sizeof(duration), event.signalUntil - tick);
        ImGui::Text("Дым виден ещё %s (днём — за %.0f м)", duration,
                    eventSpec(event.kind).signalRange);
    } else {
        ImGui::TextDisabled("Дым погас");
    }

    ImGui::SeparatorText("Грузы");
    for (const EntityId cache : event.caches) {
        ImGui::PushID(static_cast<int>(cache));
        if (idLink(cache)) {
            ImGui::PopID();
            select(cache);
            return;
        }
        ImGui::SameLine();
        const entt::entity crate = world_.find(cache);
        const auto* stock = crate != entt::null ? registry.try_get<Inventory>(crate) : nullptr;
        if (stock == nullptr) {
            ImGui::TextDisabled("исчез");
        } else {
            char text[256];
            formatInventory(text, sizeof(text), *stock);
            ImGui::TextWrapped("%s", text);
        }
        ImGui::PopID();
    }

    // Кто знает и кто идёт: по возрастанию id.
    int knowing = 0;
    int visited = 0;
    for (const auto [other, leads] : registry.view<const Leads>().each()) {
        if (const Lead* lead = leads.find(selected_)) {
            ++knowing;
            visited += lead->visited ? 1 : 0;
        }
    }
    std::vector<EntityId> heading;
    for (const auto [other, id, mind] : registry.view<const Id, const Mind>().each()) {
        if (mind.action == Action::Salvage && mind.target == selected_) {
            heading.push_back(id.value);
        }
    }
    std::sort(heading.begin(), heading.end());
    ImGui::SeparatorText("Люди");
    ImGui::Text("Знают: %d, побывали: %d, идут сейчас: %zu", knowing, visited, heading.size());
    EntityId clicked = kInvalidId;
    for (std::size_t i = 0; i < heading.size(); ++i) {
        if (i % 6 != 0) {
            ImGui::SameLine();
        }
        if (idLink(heading[i])) {
            clicked = heading[i];
        }
    }
    if (clicked != kInvalidId) {
        select(clicked);
    }
}

void Inspector::drawCache(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const Cache& cache = registry.get<Cache>(entity);
    ImGui::Text("Груз #%llu", static_cast<unsigned long long>(selected_));
    ImGui::TextUnformatted("Ивент:");
    ImGui::SameLine();
    if (world_.find(cache.event) == entt::null) {
        ImGui::TextDisabled("#%llu закончился", static_cast<unsigned long long>(cache.event));
    } else if (idLink(cache.event)) {
        select(cache.event);
        return;
    }
    if (const auto* stock = registry.try_get<Inventory>(entity)) {
        char text[256];
        formatInventory(text, sizeof(text), *stock);
        ImGui::TextWrapped("Внутри: %s", text);
    }
}

void Inspector::drawScenario(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const Scenario& scenario = registry.get<Scenario>(entity);
    const std::uint64_t tick = world_.tick();
    char text[320];

    ImGui::Text("Сценарий #%llu: %s", static_cast<unsigned long long>(selected_),
                scenarioKindName(scenario.kind));
    formatGameDuration(text, sizeof(text),
                       tick - scenario.began[static_cast<std::size_t>(scenario.phase)]);
    ImGui::Text("Фаза: %s, идёт %s", phaseName(scenario.phase), text);
    if (scenario.phase != WorldPhase::Survival) {
        formatNextPhase(text, sizeof(text), world_, scenario);
        ImGui::TextWrapped("Дальше: %s", text);
    }

    ImGui::SeparatorText("Фазы");
    for (std::size_t i = 0; i < kWorldPhases; ++i) {
        const std::uint64_t began = scenario.began[i];
        if (began == Scenario::kNever) {
            ImGui::TextDisabled("%s — ещё не было", phaseName(static_cast<WorldPhase>(i)));
            continue;
        }
        const CalendarTime time = calendarAt(began);
        ImGui::Text("%s — с дня %d, %02d:%02d", phaseName(static_cast<WorldPhase>(i)), time.day,
                    time.hour, time.minute);
    }

    const ScenarioStats stats = scenarioStats(world_);
    ImGui::SeparatorText("Беда в цифрах");
    ImGui::Text("Людей к началу вспышки: %d, живы: %d", stats.population, stats.alive);
    ImGui::Text("Погибли и обратились: %d", stats.lost);
    ImGui::Text("Заражены: %d, из них заметно больны: %d", stats.infected, stats.sick);
    // Нулевой пациент (этап 7.5): с кого всё началось.
    if (scenario.patientZero != kInvalidId) {
        ImGui::TextUnformatted("Нулевой пациент:");
        ImGui::SameLine();
        if (idLink(scenario.patientZero)) {
            select(scenario.patientZero);
            return;
        }
    }
    ImGui::Text("Заражений от больных: %zu, укусов: %zu", world_.chronicle().countOf(ChronicleKind::Infected),
                world_.chronicle().countOf(ChronicleKind::Bitten));
    ImGui::Text("Зомби: %d, из них бывших жителей: %d", stats.zombies, stats.residents);
    ImGui::Text("Приток издалека держит %d, орда %d раз в сутки", zombieTarget(world_),
                arrivalsPerDay(world_));
    int onDuty = 0;
    int employed = 0;
    for (const auto [other, person, job] : registry.view<const Person, const Job>().each()) {
        employed += job.profession != Profession::None ? 1 : 0;
        onDuty += isOnDuty(world_, other) ? 1 : 0;
    }
    ImGui::Text("На работе сейчас: %d из %d работающих; бросили работу: %zu", onDuty, employed,
                world_.chronicle().countOf(ChronicleKind::LeftWork));

    const PhaseRules& rules = phaseRules(scenario.phase);
    ImGui::SeparatorText("Правила фазы");
    if (rules.supply > 0.0f) {
        ImGui::Text("Завоз в магазины и больницу: %.0f %% обычного", 100.0f * rules.supply);
    } else {
        ImGui::TextUnformatted("Завоза нет");
    }
    ImGui::Text("Зарплата и пособие: %s; водопровод: %s", rules.economy ? "да" : "нет",
                rules.tapWater ? "да" : "нет");
    // Кто выходит на работу: профессии и чувство долга.
    std::string workers;
    for (std::size_t i = 1; i < static_cast<std::size_t>(Profession::Count); ++i) {
        if ((rules.professions & (1u << i)) != 0) {
            workers += workers.empty() ? "" : ", ";
            workers += professionName(static_cast<Profession>(i));
        }
    }
    if (workers.empty()) {
        ImGui::TextUnformatted("На работу не выходит никто");
    } else if (rules.dutyNeeded > 0.0f) {
        ImGui::TextWrapped("На работу выходят: %s — с чувством долга от %.2f", workers.c_str(),
                           rules.dutyNeeded);
    } else {
        ImGui::TextWrapped("На работу выходят все: %s", workers.c_str());
    }
    ImGui::Text("Кража и нападение — преступление: %s", rules.lawful ? "да" : "нет");
    if (rules.eveningHome > 0.0f) {
        ImGui::Text("Вечером тянет домой: %.2f", rules.eveningHome);
    }
    if (rules.stockpile != 1.0f) {
        ImGui::Text("Еды дома держат в %.1f раза больше: скупают впрок", rules.stockpile);
    }
    ImGui::Text("Группы выживших: %s", rules.groups ? "собираются" : "нет");
}

void Inspector::drawBuilding(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const Building& building = registry.get<Building>(entity);
    ImGui::Text("Здание #%llu: %s", static_cast<unsigned long long>(selected_),
                buildingName(building.kind));
    if (const entt::entity settlement = world_.find(building.settlement); settlement != entt::null) {
        ImGui::Text("Поселение: %s", registry.get<Settlement>(settlement).name.c_str());
        ImGui::SameLine();
        if (idLink(building.settlement)) {
            select(building.settlement);
            return;
        }
    }
    ImGui::Text("Размер: %.0f × %.0f м, вход в клетке %d, %d",
                static_cast<float>(building.width) * Map::kCellSize,
                static_cast<float>(building.height) * Map::kCellSize, building.door.x,
                building.door.y);
    drawSupplies(registry, entity);

    // Место работы (этап 7.2): открыто ли, касса, урожай, работники.
    std::vector<EntityId> workers;
    for (const auto [other, id, job] : registry.view<const Id, const Job>().each()) {
        if (job.workplace == selected_) {
            workers.push_back(id.value);
        }
    }
    std::sort(workers.begin(), workers.end());
    EntityId clicked = kInvalidId;
    if (const auto* workplace = registry.try_get<Workplace>(entity)) {
        ImGui::SeparatorText("Место работы");
        if (workplace->onDuty > 0) {
            ImGui::Text("Открыто: работников на месте %d", workplace->onDuty);
        } else {
            ImGui::TextDisabled("Закрыто: никого из работников нет на месте");
        }
        if (building.kind == BuildingKind::Shop) {
            ImGui::Text("Касса: %d (сдаётся при завозе в %02d:00)", workplace->till, kRestockHour);
        }
        if (building.kind == BuildingKind::Barn) {
            ImGui::Text("Урожай: %.1f порции в час на фермера, набирается %.0f %% порции",
                        harvestPerHour(calendarAt(world_.tick()).season), 100.0f * workplace->harvest);
        }
        ImGui::PushID("workers");
        for (const EntityId worker : workers) {
            if (idLink(worker)) {
                clicked = worker;
            }
            const entt::entity person = world_.find(worker);
            const auto* job = person != entt::null ? registry.try_get<Job>(person) : nullptr;
            if (job != nullptr) {
                char shift[96];
                formatShift(shift, sizeof(shift), *job);
                ImGui::SameLine();
                ImGui::Text("%s, %s%s", professionName(job->profession), shift,
                            registry.all_of<Person>(person) ? (isOnDuty(world_, person) ? " — на месте" : "")
                                                            : " — погиб");
            }
        }
        ImGui::PopID();
    } else if (!workers.empty()) {
        ImGui::TextDisabled("Место работы без работников");
    }

    // Жильцы и те, кто идёт сюда или что-то здесь делает.
    std::vector<EntityId> residents;
    std::vector<std::pair<EntityId, Action>> visitors;
    for (const auto [other, id, person] : registry.view<const Id, const Person>().each()) {
        if (person.home == selected_) {
            residents.push_back(id.value);
        }
        if (const auto* mind = registry.try_get<Mind>(other); mind != nullptr && mind->target == selected_) {
            visitors.emplace_back(id.value, mind->action);
        }
    }
    std::sort(residents.begin(), residents.end());
    std::sort(visitors.begin(), visitors.end());

    // Один и тот же человек может быть в обоих списках: у ссылок разные области id.
    ImGui::SeparatorText("Жильцы");
    if (residents.empty()) {
        ImGui::TextDisabled("никого");
    }
    ImGui::PushID("residents");
    for (std::size_t i = 0; i < residents.size(); ++i) {
        if (i % 6 != 0) {
            ImGui::SameLine();
        }
        if (idLink(residents[i])) {
            clicked = residents[i];
        }
    }
    ImGui::PopID();
    ImGui::SeparatorText("Направляются сюда");
    if (visitors.empty()) {
        ImGui::TextDisabled("никто");
    }
    ImGui::PushID("visitors");
    for (const auto& [id, action] : visitors) {
        if (idLink(id)) {
            clicked = id;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(actionName(action));
    }
    ImGui::PopID();
    if (clicked != kInvalidId) {
        select(clicked);
    }
}

void Inspector::drawSettlement(entt::entity entity) {
    const entt::registry& registry = world_.registry();
    const Settlement& settlement = registry.get<Settlement>(entity);
    ImGui::Text("%s %s (#%llu)", settlement.kind == SettlementKind::Town ? "Город" : "Деревня",
                settlement.name.c_str(), static_cast<unsigned long long>(selected_));
    ImGui::Text("Центр: клетка %d, %d, радиус %.0f м", settlement.center.x, settlement.center.y,
                static_cast<float>(settlement.radius) * Map::kCellSize);

    int buildings = 0;
    int food = 0;
    int water = 0;
    std::vector<EntityId> homes;
    for (const auto [other, id, building] : registry.view<const Id, const Building>().each()) {
        if (building.settlement != selected_) {
            continue;
        }
        ++buildings;
        homes.push_back(id.value);
        if (const auto* supplies = registry.try_get<Inventory>(other)) {
            food += supplies->food();
            water += supplies->count(ItemKind::Water);
        }
    }
    std::sort(homes.begin(), homes.end());
    int people = 0;
    int employed = 0;
    for (const auto [other, person] : registry.view<const Person>().each()) {
        if (std::binary_search(homes.begin(), homes.end(), person.home)) {
            ++people;
            const auto* job = registry.try_get<Job>(other);
            employed += job != nullptr && job->profession != Profession::None ? 1 : 0;
        }
    }
    ImGui::Text("Зданий: %d, жителей: %d (работают %d)", buildings, people, employed);
    ImGui::Text("Припасы во всех зданиях: еды %d, воды %d", food, water);
}

} // namespace lw::ui
