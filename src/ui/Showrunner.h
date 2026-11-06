#pragma once

#include "sim/Foresight.h"
#include "sim/Id.h"
#include "ui/Camerawork.h"

#include <raylib.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lw {

class CastTracker;
class StoryTracker;
class World;

namespace ui {

// Кандидат на кадр: линия (с предвестием или без), предвестие без линии или
// главный герой, которого просто навещают. Общий для режиссёра и шоураннера.
struct StoryPick {
    std::uint32_t story = 0; // Story::id; 0 — линии нет
    bool hasOmen = false;
    Foresight omen;          // предвестие, если hasOmen
    // Навестить главного героя без линии и предвестия (12а.3).
    EntityId visit = kInvalidId;
    EntityId target = kInvalidId; // за кем следит камера
    EntityId hero = kInvalidId;   // главный герой сериала, о котором кандидат; kInvalidId — нет
    float interest = 0.0f;

    bool valid() const { return story != 0 || hasOmen || visit != kInvalidId; }
    // Тот же кандидат: та же линия, а без линии — то же предвестие (или тот же,
    // кого навещают).
    bool same(const StoryPick& other) const;
};

// Где сейчас объект (метры): существо (между тиками, как рисует рендер — по
// доле тика alpha), здание, орда и группа — по вожаку, ивент и поселение;
// false — нет в мире.
bool positionOf(const World& world, EntityId id, float alpha, Vector2& at);

// Шоураннер режима «Сериал» (этап 12а.8). Режиссёр 12а.1–12а.6 — диспетчер:
// «где сейчас интереснее». Шоураннер собирает из того же материала сериал:
//
//   эпизод (5–10 реальных минут) — 2–3 главных героя, линия A и побочная B;
//     начинается с «Ранее в сериях» и титра «Эпизод N», кончается
//     клиффхэнгером («Продолжение следует») и «Конец эпизода N»; на эти титры
//     мир останавливается, чтобы следующий эпизод начался ровно с того места;
//   сцена — единица показа: место, участники, о чём она, минимальная длина;
//     ради «поинтереснее» её не перебивают, только ради кульминации линии
//     эпизода (и то после минимальной длины);
//   переход между сценами — затемнение, вводный титр места и времени
//     («Old Village, дом семьи Miller. Вечер»), установочный общий план места
//     и только потом ближе;
//   скучное время вырезается скачком — мгновенной склейкой (12а.10): под
//     чёрным кадром мир мотается вперёд без отрисовки порциями промотки за
//     1–2 реальные секунды, пока у героев что-то не назреет, — «Три часа
//     спустя»; ускорения на экране не видно;
//   на экране только знакомый зрителю (он уже был в кадре) или угроза
//     знакомому; новые лица входят через героев;
//   перебивка с драматической иронией: угроза знакомому, о которой он не
//     знает, — «Орда, которую мы видели утром, в двух километрах от дома
//     Mary Cole. Она ещё не знает.» — и назад к сцене.
//
// Без пустых моментов (12а.9):
//   у сцены есть вопрос и ответ — она идёт, пока в ней что-то меняется
//     (действие героя, сон, кто рядом, события летописи с участниками) или
//     вот-вот случится, и кончается, когда ответ получен (предвестие сбылось,
//     герой погиб, встал из тела, проснулся, дошёл) или ничего не меняется
//     несколько секунд, — а не по таймеру длины;
//   ожидание (тело вот-вот встанет, герой спит, идёт долго, предвестие ещё
//     далеко) не показывают целиком: сцену откладывают (Wait) и возвращаются
//     к ней заранее, за несколько секунд до развязки; а пока — монтаж:
//     реакция близких на гибель знакомого, другие герои эпизода, линия B,
//     угроза знакомому, кадры-паузы (рассвет и закат таймлапсом, гроза,
//     туман над рекой, орда вдали, дым) — только связкой между событиями;
//     ждать долго — скачок времени ровно до развязки;
//   сцена не повторяет предыдущую по месту, плану и действию сразу.
//
// Шоураннер мир только читает: команды (сменить кадр, промотать, остановить
// мир, таймлапс) исполняет режиссёр.
enum class ShowPhase : std::uint8_t {
    Off,
    Opening,      // начало эпизода: «Ранее в сериях», «Эпизод N» — на чёрном, мир стоит
    FadeOut,      // затемнение в конце сцены
    Jump,         // скачок времени: на чёрном мир мотается вперёд
    Intro,        // вводный титр места и времени на чёрном
    Establishing, // установочный общий план места
    Scene,        // сцена
    Cutaway,      // перебивка: угроза знакомому, о которой он не знает, реакция близких
    Interlude,    // кадр-пауза: рассвет, гроза, туман, орда вдали — связка между событиями
    Cliffhanger,  // конец эпизода: то, что не решено, — «Продолжение следует»
    Closing,      // «Конец эпизода N» на чёрном, мир стоит
    Count
};

// Что режиссёр знает на этом кадре.
struct ShowInput {
    const World* world = nullptr;
    const StoryTracker* stories = nullptr;
    const CastTracker* cast = nullptr;
    const std::vector<Foresight>* omens = nullptr; // по убыванию интересности
    const std::vector<StoryPick>* picks = nullptr; // кандидаты по убыванию интересности
    bool refreshed = false; // кандидаты пересчитаны в этом кадре
    StoryPick current;      // кого показывает режиссёр
    bool lost = false;      // предвестие без линии сбылось или сорвалось
    float lostFor = 0.0f;   // сколько секунд с тех пор
    float currentInterest = 0.0f;
    bool climax = false;    // вот-вот кульминация текущего кадра
    bool quiet = false;     // затишье: ничего не надвигается, в кадре скучно
    bool paused = false;    // мир на паузе (не по воле шоураннера)
    bool skipping = false;  // идёт промотка
    float simRate = 1.0f;   // секунд симуляции на реальную секунду (сглажено)
};

// Команды режиссёру.
struct ShowOrders {
    std::optional<StoryPick> show; // новая сцена (или перебивка): перевести камеру со склейкой
    bool recut = false;            // тот же кадр, новый план: переставить камеру
    std::uint64_t skip = 0;        // промотать столько тиков
    std::optional<bool> pause;     // остановить мир (true) или вернуть, как было (false)
    // Кадр-пауза таймлапсом: время ×kTimelapseSpeed (true) или как было (false).
    std::optional<bool> timelapse;
};

// Скорость времени в таймлапсе кадра-паузы.
inline constexpr int kTimelapseSpeed = 100;

class Showrunner {
public:
    // Сериал начался (эпизод 1) или кончился.
    void start();
    void stop();
    bool active() const { return phase_ != ShowPhase::Off; }

    // Раз за кадр, после пересмотра кандидатов режиссёром. dt — реальные секунды.
    ShowOrders update(const ShowInput& in, float dt);
    // Нужны свежие кандидаты каждый кадр (скачок времени: иначе проскочим).
    bool wantsFresh() const { return phase_ == ShowPhase::Jump; }
    // Итог, подведённый режиссёром (камера ушла от человека): для «ранее в сериях».
    void note(std::string text);

    ShowPhase phase() const { return phase_; }
    // Чёрный кадр поверх всего: 1 — чёрный, 0 — нет.
    float curtain() const { return curtain_; }
    // Сейчас нужен установочный план места сцены (и под вводным титром — чтобы
    // камера уже стояла там, когда кадр проявится).
    bool establishing() const;
    // Установочный план: место сцены общим планом.
    ShotPlan placePlan(const World& world, float alpha) const;
    // Идёт кадр-пауза и её план (12а.9).
    bool interlude() const { return phase_ == ShowPhase::Interlude; }
    ShotPlan interludePlan(const World& world, float alpha) const;
    // Финал (12а.10): клиффхэнгер эпизода или герой погиб в кадре — камера
    // медленно отъезжает.
    bool finale() const;
    // Прятать ли обычные титры режиссёра (имя, намерение, линию).
    bool hidesTitles() const;
    // Подпись монтажа слева вверху: «Тем временем…», «Передышка»; пусто — нет.
    std::string label() const;
    int episode() const { return episode_; }
    // Строка для окна режиссёра: что сейчас делает шоураннер.
    std::string status() const;
    // Титры шоураннера прямо на кадре: карточки на чёрном, место и время,
    // перебивка, клиффхэнгер. bar — высота полосы широкого кадра, пикселей.
    void draw(float bar) const;

private:
    // Чего ждёт отложенная сцена (12а.9).
    enum class WaitKind : std::uint8_t {
        Omen,   // предвестие ещё далеко: мститель, орда, вор только идут
        Rise,   // тело укушенного встанет зомби (Corpse::turnsAt)
        Wake,   // герой спит
        Arrive, // герой идёт долго
    };
    // Как сцена выглядит сейчас: перемена здесь — что-то произошло.
    struct Look {
        std::uint8_t state = 0;  // 0 — нет, 1 — человек, 2 — тело, 3 — зомби, 4 — другое
        std::uint8_t action = 0; // Action человека
        bool asleep = false;
        bool indoors = false;
        int company = 0;         // сколько людей рядом (не больше 3)
        bool operator==(const Look&) const = default;
    };
    // Чем сцена отличается от соседней: место, план, действие.
    struct ShotKey {
        std::uint64_t place = 0;
        ShotSize size = ShotSize::Close;
        int doing = -1;
        bool operator==(const ShotKey&) const = default;
    };
    struct Scene {
        StoryPick pick;
        std::string place;   // «Old Village, дом семьи Miller»
        std::string time;    // «Вечер», «День 9, вечер»
        std::string caption; // перебивка: «Орда, которую мы видели утром, …»
        EntityId building = kInvalidId; // здание, где сцена (установочный план)
        Vector2 at{};        // где сцена на её начало, метры
        bool calm = false;   // мирная: быт, передышка
        bool breather = false;
        bool hurry = false;  // к кульминации: без установочного плана
        float age = 0.0f;    // секунд в сцене
        bool sawClimax = false;
        bool wasAlive = false; // за кем следим, был жив в начале
        float deadFor = 0.0f;
        // Перемены (12а.9): как сцена выглядела, когда что-то менялось в
        // последний раз (clock_), с какой записи летописи смотреть новое.
        bool looked = false;
        Look look;
        float lastChange = 0.0f;
        std::size_t cursor = 0;
        float answeredFor = 0.0f; // секунд, как ответ получен
        // Возврат к отложенной сцене: чего ждали и в который раз.
        bool resumed = false;
        WaitKind waitKind = WaitKind::Omen;
        int returns = 0;
        bool familiar = false; // место зритель видел недавно: без установочного плана
    };
    // Сцена выдохлась (ничего не менялось): снова её не берут, пока в ней
    // что-то не изменится.
    struct Stale {
        StoryPick pick;
        ShotKey key;
        std::uint64_t tick = 0;
    };
    // Отложенная сцена: к ней вернутся к развязке.
    struct Wait {
        WaitKind kind = WaitKind::Omen;
        Scene scene;
        EntityId who = kInvalidId;  // чья развязка: тело, спящий, идущий, герой предвестия
        Foresight omen;             // для Omen
        std::size_t cursor = 0;     // запись летописи, с которой искать, сбылось ли
        std::uint64_t since = 0;    // тик, когда отложили
        int returns = 0;
    };
    struct WaitState {
        bool gone = false;  // ждать больше нечего
        bool due = false;   // пора возвращаться
        float simLeft = 0.0f; // секунд симуляции до развязки; бесконечность — неизвестно
    };
    // Кадр-пауза.
    enum class InterludeKind : std::uint8_t { Dawn, Dusk, Storm, Fog, Horde, Smoke, Count };
    struct Interlude {
        InterludeKind kind = InterludeKind::Dawn;
        EntityId focus = kInvalidId; // за чем следить (орда); kInvalidId — точка at
        Vector2 at{};                // куда смотреть, метры
        float yaw = 0.0f;            // откуда (курс камеры, как ShotPlan::viewFrom)
        float distance = 0.0f;
        std::string caption;         // «Рассвет. Old Village»
        bool timelapse = false;
    };
    // Знакомый зрителю погиб или обратился: близким нужна реакция.
    struct Loss {
        EntityId dead = kInvalidId;
        std::uint64_t tick = 0;
        bool turned = false;
        int shown = 0; // сколько реакций уже показали
    };
    // Куда идти, когда кадр станет чёрным.
    enum class Next : std::uint8_t { Choose, Closing };

    // Сцена — кому и что показывать (0 — нечего): с учётом линий эпизода,
    // главных героев и правила «только знакомые или угроза знакомому».
    float score(const ShowInput& in, const StoryPick& pick) const;
    bool involves(const ShowInput& in, const StoryPick& pick, bool knownOnly) const;
    bool isHero(EntityId id) const;
    bool isKnown(EntityId id) const;
    // Знакомый зрителю живёт в поселении (для орды); kInvalidId — нет.
    EntityId knownIn(const World& world, EntityId settlement) const;
    const StoryPick* best(const ShowInput& in, float& bestScore) const;
    // Срочное (кульминация в пределах нескольких секунд) у линии эпизода — не в
    // этой сцене.
    bool urgentElsewhere(const ShowInput& in) const;
    // Перебивка с драматической иронией; false — нет.
    bool irony(const ShowInput& in, StoryPick& pick, std::string& text) const;

    void beginEpisode(const ShowInput& in, ShowOrders& orders);
    void chooseHeroes(const ShowInput& in);
    // Чья это линия: герой сериала, герой линии, кто идёт к кульминации, за кем
    // камера — первый живой человек; kInvalidId — нет (орда).
    EntityId leadOf(const ShowInput& in, const StoryPick& pick) const;
    // Следующая сцена после затемнения: перебивка, сцена, скачок или быт.
    void proceed(const ShowInput& in, ShowOrders& orders);
    void startScene(const ShowInput& in, const StoryPick& pick, ShowOrders& orders, bool calm, bool hurry);
    void startCutaway(const ShowInput& in, const StoryPick& pick, std::string text, bool resume,
                      ShowOrders& orders);
    void startJump(const ShowInput& in, ShowOrders& orders);
    // Можно ли скакнуть ещё (после скачка — только пока цепочка не затянулась).
    bool canJump(const ShowInput& in) const;
    bool wake(const ShowInput& in);
    void endScene(const ShowInput& in, bool hurry);
    void startCliffhanger(const ShowInput& in, ShowOrders& orders);
    // Зритель видит участников сцены рядом с тем, за кем следят: теперь они знакомы.
    void meet(const ShowInput& in);
    // Навестить героя, которого дольше всех не показывали (не того, чьей
    // развязки ждём, и не повторяя прошлую сцену); invalid — некого.
    StoryPick visitPick(const ShowInput& in) const;
    void setPhase(ShowPhase phase);

    // Вопрос и ответ сцены (12а.9).
    Look lookOf(const ShowInput& in) const;
    // Заметить перемены в сцене (lastChange).
    void observe(const ShowInput& in);
    // Ответ получен: предвестие сбылось, герой погиб, встал, проснулся, дошёл.
    bool answered(const ShowInput& in) const;
    // В сцене что-то идёт: недавно менялось, драка или бегство, развязка ближе
    // нескольких секунд.
    bool live(const ShowInput& in) const;
    ShotKey keyOf(const ShowInput& in, const StoryPick& pick) const;
    ShotSize sizeOf(const ShowInput& in, const StoryPick& pick) const;
    // Та же сцена, что прошлая: место, план и действие.
    bool repeats(const ShowInput& in, const StoryPick& pick) const;
    // Сцена выдохлась и с тех пор в ней ничего не изменилось.
    bool stale(const ShowInput& in, const StoryPick& pick) const;
    // Секунд симуляции на реальную секунду сейчас.
    float rateOf(const ShowInput& in) const;
    // Отложить текущую сцену до развязки; false — ждать нечего.
    bool makeWait(const ShowInput& in, Wait& wait) const;
    WaitState waitState(const ShowInput& in, const Wait& wait) const;
    bool waiting(EntityId id) const;
    // Самая срочная отложенная сцена, к которой пора вернуться; −1 — нет.
    // Заодно забывает те, ждать которых больше нечего.
    int dueWait(const ShowInput& in);
    // Через сколько секунд симуляции ближайшая известная развязка.
    float earliestWait(const ShowInput& in) const;
    // Вернуться к отложенной сцене; false — некуда (герой исчез).
    bool returnTo(const ShowInput& in, std::size_t index, ShowOrders& orders);
    // Порция промотки скачка: не дальше, чем нужно, чтобы успеть к развязке.
    std::uint64_t jumpChunk(const ShowInput& in) const;
    // Монтаж ожидания: реакция близких на гибель знакомого.
    void trackLosses(const World& world);
    bool reaction(const ShowInput& in, StoryPick& pick, std::string& text);
    // Кадр-пауза, уместный сейчас; false — нет.
    bool chooseInterlude(const ShowInput& in, Interlude& out) const;
    void startInterlude(Interlude interlude, ShowOrders& orders);
    // Новая сцена или перебивка: запомнить её место, план и действие (чтобы
    // следующая не повторила), после неё снова можно кадр-паузу.
    void sceneStarted(const ShowInput& in, const StoryPick& pick);
    // Сколько секунд вводный титр.
    float introLength() const;
    // Для строки состояния и лога: «встанет из тела».
    static const char* waitTitle(WaitKind kind);

    ShowPhase phase_ = ShowPhase::Off;
    bool begin_ = false; // начать эпизод на ближайшем update
    float phaseAge_ = 0.0f;
    float curtain_ = 1.0f;
    float curtainTarget_ = 1.0f;
    Next next_ = Next::Choose;
    bool fadeHurry_ = false;

    // Эпизод.
    int episode_ = 0;
    float episodeClock_ = 0.0f;   // реальные секунды с начала эпизода
    std::uint64_t episodeTick_ = 0; // тик начала эпизода
    int scenes_ = 0;              // сцен в эпизоде
    std::vector<EntityId> heroes_; // главные герои эпизода
    StoryPick lineA_;
    StoryPick lineB_;
    std::vector<Foresight> established_; // предвестия, уже показанные в эпизоде
    std::vector<std::string> notes_;     // итоги сцен эпизода
    std::vector<std::string> recap_;     // «ранее в сериях» для следующего эпизода
    std::string cliffLine_;
    std::string starring_;               // «John Miller, Mary Cole» — для титра эпизода
    std::string dayLine_;                // «День 9 · вечер»
    StoryPick cliff_;
    std::uint64_t lastDay_ = 0;          // день последнего вводного титра

    // Сцена.
    Scene scene_;
    std::optional<Scene> resume_;        // к какой сцене вернуться после перебивки
    StoryPick last_;                     // прошлая сцена
    std::uint64_t lastSceneTick_ = 0;    // тик её начала
    ShotKey lastKey_;                    // место, план и действие прошлой сцены
    bool hasLastKey_ = false;
    bool breatherDue_ = false;           // после кульминации — передышка
    std::string jumpText_;               // «Три часа спустя» для вводного титра

    // Скачок времени.
    std::uint64_t jumpStart_ = 0;
    float jumpReal_ = 0.0f;
    // Скачки подряд (ночь, пока герои спят) — одна склейка (12а.10): титр —
    // от начала цепочки, вся цепочка не дольше kJumpChainReal секунд.
    std::uint64_t jumpFrom_ = 0;
    float jumpChainReal_ = 0.0f;
    std::size_t jumpCursor_ = 0;         // с какой записи летописи смотреть новое
    bool jumpedIdle_ = false;            // прошлый скачок ничего не нашёл

    // Без пустых моментов (12а.9).
    std::vector<Wait> waits_;            // отложенные сцены
    std::vector<Stale> stale_;           // выдохшиеся сцены
    std::unordered_map<std::string, float> places_; // место → clock_, когда его показывали
    std::vector<Loss> losses_;           // недавние потери знакомых
    std::size_t lossCursor_ = 0;         // с какой записи летописи искать потери
    std::vector<std::pair<EntityId, EntityId>> reacted_; // (погибший, близкий) — уже показали
    Interlude interlude_;
    bool lastInterlude_ = false;         // прошлый кадр — пауза: второй подряд нельзя
    bool timelapse_ = false;             // кадр-пауза идёт таймлапсом
    std::array<float, static_cast<std::size_t>(InterludeKind::Count)> interludeAt_{}; // когда был каждый вид

    // Зритель (только поиск: обходить нельзя — порядок не детерминирован;
    // шоураннер мир не меняет, но так проще не ошибиться).
    std::unordered_set<EntityId> known_;
    std::unordered_map<EntityId, float> lastShown_;      // герой → clock_, когда показывали
    std::unordered_map<EntityId, std::uint64_t> seenHordes_; // орда → тик, когда её показывали
    std::vector<std::pair<Foresight, float>> ironies_;   // какие перебивки уже были и когда
    float clock_ = 0.0f;                 // реальные секунды с начала сериала
    // Для строки состояния: как прошёл последний выбор сцены.
    float lastBest_ = 0.0f;
    float lastTopInterest_ = 0.0f;
    std::size_t lastPicks_ = 0;
    std::size_t lastHeroPicks_ = 0;
};

const char* showPhaseTitle(ShowPhase phase);

} // namespace ui
} // namespace lw
