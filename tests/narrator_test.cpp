// Тесты рассказчика (этап 12а.13) — без графики: знакомство с героем, мотив
// сцены, связки между сценами, реплики о характере, очередь голоса за кадром.
// Рассказчик только смотрит на мир и не меняет его хеш.
// Без фреймворка: CHECK печатает место сбоя, код возврата — число сбоев.

#include "sim/Components.h"
#include "sim/Memory.h"
#include "sim/Names.h"
#include "sim/TimeScale.h"
#include "sim/World.h"
#include "ui/Format.h"
#include "ui/Narrator.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

lw::WorldConfig peacefulConfig() {
    lw::WorldConfig config;
    config.mapWidth = 512;
    config.mapHeight = 512;
    config.people = 60;
    config.zombies = 0;
    config.animals = 10;
    config.flocks = 1;
    return config;
}

// Живые люди по возрастанию id.
std::vector<entt::entity> peopleOf(lw::World& world) {
    std::vector<std::pair<lw::EntityId, entt::entity>> found;
    entt::registry& registry = world.registry();
    for (const auto [entity, person] : registry.view<const lw::Person>().each()) {
        if (!registry.any_of<lw::Zombie, lw::Corpse>(entity) && registry.all_of<lw::Name>(entity)) {
            found.emplace_back(registry.get<lw::Id>(entity).value, entity);
        }
    }
    std::sort(found.begin(), found.end());
    std::vector<entt::entity> result;
    for (const auto& [id, entity] : found) {
        result.push_back(entity);
    }
    return result;
}

lw::EntityId idOf(lw::World& world, entt::entity entity) {
    return world.registry().get<lw::Id>(entity).value;
}

std::string fullName(lw::World& world, entt::entity entity) {
    const lw::Name& name = world.registry().get<lw::Name>(entity);
    return name.first + " " + name.last;
}

void testAgo() {
    using lw::kTicksPerGameDay;
    using lw::kTicksPerGameHour;
    const std::uint64_t now = 30 * kTicksPerGameDay + 12 * kTicksPerGameHour;
    CHECK(lw::ui::agoPhrase(now, now) == "Только что");
    CHECK(lw::ui::agoPhrase(now - 10 * kTicksPerGameDay, now) == "Неделю назад" ||
          lw::ui::agoPhrase(now - 10 * kTicksPerGameDay, now) == "Давно");
    CHECK(lw::ui::agoPhrase(now - 20 * kTicksPerGameDay, now) == "Давно");
    CHECK(lw::ui::agoPhrase(now - 3 * kTicksPerGameDay, now) == "Три дня назад");
}

void testTexts() {
    lw::World world(33, peacefulConfig());
    for (int i = 0; i < 400; ++i) {
        world.step();
    }
    lw::ui::setNameSource(&world);
    const std::vector<entt::entity> people = peopleOf(world);
    CHECK(people.size() >= 2);
    if (people.size() < 2) {
        return;
    }
    const std::uint64_t before = world.stateHash();

    // Знакомство: у каждого есть, без номеров, с правильным родом.
    for (const entt::entity entity : people) {
        const std::string text = lw::ui::introduction(world, idOf(world, entity));
        CHECK(!text.empty());
        CHECK(!contains(text, "#"));
        const bool female = lw::isFemaleName(world.registry().get<lw::Name>(entity).first);
        CHECK(!(female && contains(text, "Живёт один")));
        CHECK(!(!female && contains(text, "Живёт одна")));
        CHECK(!contains(text, "{")); // шаблон рода раскрыт целиком
    }

    // Петер обокрал дом Джона: Джон это помнит.
    const entt::entity john = people[0];
    const entt::entity peter = people[1];
    const lw::EntityId johnId = idOf(world, john);
    const lw::EntityId peterId = idOf(world, peter);
    entt::registry& registry = world.registry();
    lw::Memories& memories = registry.get_or_emplace<lw::Memories>(john);
    memories = {};
    lw::remember(memories, lw::MemoryKind::Robbed, peterId, registry.get<lw::Person>(john).home, 0.9f,
                 world.tick() - lw::kTicksPerGameDay);
    const std::string intro = lw::ui::introduction(world, johnId);
    CHECK(contains(intro, fullName(world, peter)));
    CHECK(contains(intro, "обокрал"));

    // Мотив: мститель идёт к обидчику.
    lw::Foresight omen;
    omen.kind = lw::ForesightKind::Revenge;
    omen.hero = johnId;
    omen.other = peterId;
    const std::string why = lw::ui::motive(world, nullptr, &omen, johnId);
    CHECK(contains(why, "прост")); // «не может простить» или «так и не простил»
    CHECK(contains(why, fullName(world, peter)));

    // Связка: от Джона к Петеру — «которого Джон не может простить».
    const std::string link = lw::ui::bridge(world, johnId, peterId);
    CHECK(!link.empty());
    CHECK(contains(link, "не может простить"));
    // От Петера к Джону — свои счёты.
    CHECK(contains(lw::ui::bridge(world, peterId, johnId), "свои счёты"));
    CHECK(lw::ui::bridge(world, johnId, johnId).empty());

    // Характер: мстит и злопамятен — «не умеет прощать».
    registry.get<lw::Mind>(john).action = lw::Action::Avenge;
    registry.get<lw::Mind>(john).target = peterId;
    registry.get<lw::Traits>(john).aggression = 0.9f;
    lw::ui::Remark kind = lw::ui::Remark::Count;
    CHECK(contains(lw::ui::remark(world, johnId, kind), "не умеет прощать"));
    CHECK(kind == lw::ui::Remark::Unforgiving);
    // Робкий бежит.
    registry.get<lw::Mind>(john).action = lw::Action::Flee;
    registry.get<lw::Traits>(john).bravery = 0.1f;
    kind = lw::ui::Remark::Count;
    CHECK(contains(lw::ui::remark(world, johnId, kind), "Смелостью"));
    CHECK(kind == lw::ui::Remark::Coward);

    // Тексты мир не меняют (память и черты выше мы правили сами — до замера).
    const std::uint64_t planted = world.stateHash();
    (void)lw::ui::introduction(world, johnId);
    (void)lw::ui::motive(world, nullptr, &omen, johnId);
    (void)lw::ui::bridge(world, johnId, peterId);
    (void)lw::ui::remark(world, johnId, kind);
    CHECK(world.stateHash() == planted);
    CHECK(planted != before); // правки выше действительно в хеше
}

void testNarrator() {
    lw::World world(34, peacefulConfig());
    for (int i = 0; i < 400; ++i) {
        world.step();
    }
    lw::ui::setNameSource(&world);
    const std::vector<entt::entity> people = peopleOf(world);
    CHECK(people.size() >= 2);
    if (people.size() < 2) {
        return;
    }
    const lw::EntityId a = idOf(world, people[0]);
    const lw::EntityId b = idOf(world, people[1]);
    const std::uint64_t before = world.stateHash();

    lw::ui::Narrator narrator;
    narrator.beginScene(world, lw::kInvalidId, a, "Мотив сцены.");
    CHECK(narrator.speaking());
    // Титров не видно — часы стоят, ничего не звучит.
    narrator.update(world, a, 5.0f, false);
    CHECK(narrator.line().empty());
    // Видно: сперва знакомство, потом мотив.
    narrator.update(world, a, 0.1f, true);
    CHECK(narrator.line() == lw::ui::introduction(world, a));
    CHECK(narrator.introduced(a));
    for (int i = 0; i < 200 && narrator.line() != "Мотив сцены."; ++i) {
        narrator.update(world, a, 0.1f, true);
    }
    CHECK(narrator.line() == "Мотив сцены.");
    // Дальше — тишина (или реплика, но не повтор знакомства).
    for (int i = 0; i < 200; ++i) {
        narrator.update(world, a, 0.1f, true);
        CHECK(narrator.line() != lw::ui::introduction(world, a));
    }
    // Вторая сцена с тем же героем — знакомить снова незачем.
    narrator.beginScene(world, lw::kInvalidId, a, "");
    narrator.update(world, a, 0.1f, true);
    CHECK(narrator.line() != lw::ui::introduction(world, a));
    // Камера перешла к другому — его представят.
    for (int i = 0; i < 200; ++i) {
        narrator.update(world, b, 0.1f, true);
        if (narrator.introduced(b)) {
            break;
        }
    }
    CHECK(narrator.introduced(b));
    CHECK(world.stateHash() == before);
}

// Посмотреть на тексты глазами: мир после вспышки, знакомства, связки, реплики.
void printSamples(int days) {
    lw::WorldConfig config = peacefulConfig();
    config.people = 300;
    config.mapWidth = 1024;
    config.mapHeight = 1024;
    config.outbreakDays = 2;
    lw::World world(7, config);
    const std::uint64_t ticks = static_cast<std::uint64_t>(days) * lw::kTicksPerGameDay;
    while (world.tick() < ticks) {
        world.step();
    }
    lw::ui::setNameSource(&world);
    const std::vector<entt::entity> people = peopleOf(world);
    std::printf("== знакомство (%zu живых)\n", people.size());
    for (std::size_t i = 0; i < people.size() && i < 300; i += 23) {
        std::printf("%s: %s\n", fullName(world, people[i]).c_str(),
                    lw::ui::introduction(world, idOf(world, people[i])).c_str());
    }
    std::printf("== связки\n");
    int links = 0;
    for (std::size_t i = 0; i < people.size() && links < 12; ++i) {
        for (std::size_t j = 0; j < people.size() && links < 12; j += 1) {
            const std::string line = lw::ui::bridge(world, idOf(world, people[i]), idOf(world, people[j]));
            if (!line.empty() && (i * 7 + j) % 5 == 0) {
                std::printf("%s → %s\n", fullName(world, people[i]).c_str(), line.c_str());
                ++links;
            }
        }
    }
    std::printf("== мотивы и реплики\n");
    for (const entt::entity entity : people) {
        const lw::EntityId id = idOf(world, entity);
        const std::string why = lw::ui::motive(world, nullptr, nullptr, id);
        lw::ui::Remark kind = lw::ui::Remark::Count;
        const std::string trait = lw::ui::remark(world, id, kind);
        if (!why.empty() || !trait.empty()) {
            std::printf("%s: %s | %s\n", fullName(world, entity).c_str(), why.c_str(), trait.c_str());
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--print") {
        printSamples(argc >= 3 ? std::atoi(argv[2]) : 12);
        return 0;
    }
    testAgo();
    testTexts();
    testNarrator();
    if (failures == 0) {
        std::printf("narrator tests passed\n");
    }
    return failures;
}
