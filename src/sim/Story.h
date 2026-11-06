#pragma once

#include "sim/Chronicle.h"
#include "sim/Foresight.h"
#include "sim/Id.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lw {

class World;

// Сюжетные линии (этап 12а.1): из летописи собираются арки — у арки есть
// участники, завязка, нарастание, кульминация и развязка. По ним режиссёр
// выбирает, за кем следить.
//
// Линии — производные данные: их можно пересобрать из летописи в любой момент,
// поэтому в World::stateHash они не входят и на решения существ не влияют.

// Вид сюжетной линии.
enum class StoryKind : std::uint8_t {
    Feud,      // кража, нападение, бросил в беде → обида → месть или примирение
    Group,     // сбор группы → вступления, изгнания, смена вожака, побег → распад
    Infection, // укус → лихорадка → смерть → обращение
    Salvage,   // ивент → кто узнал → кто добрался до груза → ивент кончился
    Count
};

// Где линия сейчас. У события линии (StoryBeat) — куда оно её продвинуло.
enum class StoryStage : std::uint8_t {
    Setup,  // завязка
    Rising, // нарастание
    Climax, // кульминация
    Ended,  // развязка: линия закончилась
    Count
};

// Чем линия кончилась (или, пока идёт, — чем, скорее всего, кончится:
// expectedEnding).
enum class StoryEnding : std::uint8_t {
    None,
    Revenge,    // пострадавший отомстил обидчику
    Reconciled, // помирились
    Killed,     // обидчик убил пострадавшего
    Death,      // кто-то из главных погиб не от рук другого
    Faded,      // забылось: давно ничего не происходит
    Turned,     // укушенный встал зомби
    Disbanded,  // группа распалась
    Claimed,    // груз забрали
    Unclaimed,  // ивент кончился, до груза никто не добрался
    Count
};

// Событие линии: номер записи в Chronicle::events() и куда оно продвинуло линию.
struct StoryBeat {
    std::uint32_t event = 0;
    StoryStage stage = StoryStage::Setup;
};

struct Story {
    std::uint32_t id = 0; // номер линии с 1, в порядке завязки
    StoryKind kind = StoryKind::Feud;
    StoryStage stage = StoryStage::Setup;
    StoryEnding ending = StoryEnding::None;
    // Главный герой — за кем следить: пострадавший во вражде, укушенный, вожак
    // группы, кто первым узнал о грузе или добрался до него (нет — kInvalidId).
    EntityId hero = kInvalidId;
    EntityId other = kInvalidId; // противник: обидчик во вражде, укусивший зомби
    EntityId focus = kInvalidId; // о чём линия: группа, ивент
    // Вражда между теми, кто был друзьями (или тот, кому доверяли, обокрал
    // или бросил группу): предательство.
    bool betrayal = false;
    std::vector<EntityId> cast;   // все участники, по порядку появления
    std::vector<StoryBeat> beats; // события линии по порядку
    std::uint64_t begun = 0;      // тик завязки
    std::uint64_t updated = 0;    // тик последнего события
    std::uint64_t ended = 0;      // тик развязки (0 — идёт)

    bool active() const { return stage != StoryStage::Ended; }
};

// Собирает линии из летописи. update читает только новые записи с прошлого
// вызова, поэтому звать его можно хоть каждый кадр. Порядок линий и их
// содержимое зависят только от летописи — одинаковы при одном seed.
class StoryTracker {
public:
    // Дочитать новые записи летописи мира и закрыть заглохшие линии.
    void update(const World& world);

    const std::vector<Story>& stories() const { return stories_; }
    // Линия по номеру (Story::id); nullptr — нет такой.
    const Story* find(std::uint32_t id) const;
    // Номера линий (Story::id), где участвует id, по возрастанию.
    std::vector<std::uint32_t> storiesOf(EntityId id) const;
    // Идущая линия, к кульминации которой ведёт предвестие (этап 12а.2):
    // вражда мстителя с обидчиком или вора с хозяином, заражение укушенного,
    // гонка за грузом ивента; nullptr — такой линии нет (орда, первая кража).
    const Story* lineOf(const Foresight& foresight) const;

private:
    void consume(const World& world, const ChronicleEvent& event, std::uint32_t index);
    // Вражда пары: найти идущую (или только что закрытую — для эпилога) или,
    // если open, начать новую; nullptr — нет и не начата.
    Story* feud(EntityId victim, EntityId offender, bool open, std::uint64_t tick);
    Story* keyed(const std::unordered_map<EntityId, std::uint32_t>& map, EntityId key);
    Story& start(StoryKind kind, EntityId hero, EntityId other, EntityId focus,
                 std::uint64_t tick);
    // Добавить событие; линия идёт дальше, только если stage выше нынешнего.
    // Событие закрытой линии — эпилог, если линия закрылась недавно.
    void beat(Story& story, std::uint32_t index, StoryStage stage, std::uint64_t tick);
    void finish(Story& story, std::uint32_t index, StoryEnding ending, std::uint64_t tick);
    void join(Story& story, EntityId id); // в участники и в поиск по id
    void index(const Story& story, EntityId id); // только в поиск по id
    // Пострадавший victim и обидчик offender: начать вражду или продолжить её.
    void offence(EntityId victim, EntityId offender, std::uint32_t index, std::uint64_t tick);
    // Закрыть линии, где давно ничего не происходит.
    void expire(std::uint64_t now);
    // Недавно закрытая линия ещё принимает события (похороны, «узнал в зомби»).
    static bool accepts(const Story& story, std::uint64_t tick);

    std::size_t cursor_ = 0; // сколько записей летописи уже прочитано
    std::uint64_t nextExpire_ = 0;
    std::vector<Story> stories_;
    // Только для поиска (обходить нельзя — порядок не детерминирован): по ключу —
    // индекс последней линии в stories_.
    using Pair = std::pair<EntityId, EntityId>; // пара id без порядка: меньший первым
    struct PairHash {
        std::size_t operator()(const Pair& pair) const {
            return static_cast<std::size_t>(pair.first * 0x9E3779B97F4A7C15ULL ^ pair.second);
        }
    };
    static Pair pairOf(EntityId a, EntityId b) { return a < b ? Pair{a, b} : Pair{b, a}; }
    std::unordered_map<Pair, std::uint32_t, PairHash> feuds_;
    std::unordered_map<EntityId, std::uint32_t> groups_;     // группа
    std::unordered_map<EntityId, std::uint32_t> infections_; // укушенный
    std::unordered_map<EntityId, std::uint32_t> salvages_;   // ивент
    std::unordered_map<EntityId, std::vector<std::uint32_t>> byEntity_;
    std::unordered_set<Pair, PairHash> friends_; // пары, когда-то ставшие друзьями
};

// Насколько линия интересна сейчас (0 — совсем нет, ~1 — живая линия в
// нарастании): вид, стадия, свежесть последнего события, сколько в ней событий
// и участников. По ней режиссёр выбирает линию.
float storyInterest(const Story& story, std::uint64_t now);

// Чем, скорее всего, кончится линия этого вида.
StoryEnding expectedEnding(const Story& story);

// Короткие английские имена — для lw_headless.
const char* storyKindName(StoryKind kind);
const char* storyStageName(StoryStage stage);
const char* storyEndingName(StoryEnding ending);

} // namespace lw
