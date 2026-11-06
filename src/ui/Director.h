#pragma once

#include "sim/Cast.h"
#include "sim/Foresight.h"
#include "sim/Id.h"
#include "sim/Story.h"
#include "ui/Camerawork.h"
#include "ui/Captions.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lw {

class CameraController;
class SimDriver;
class World;

namespace ui {

class Inspector;

// Режиссёр (этап 12а): собирает сюжетные линии из летописи (StoryTracker) и,
// если включён, сам ведёт камеру — выбирает в инспекторе героя самой
// интересной линии и следит за ним. Пока только вид сверху: камера движется
// через слежение инспектора (CameraController::moveTo).
//
// Предвкушение (12а.2): режиссёр смотрит и на то, что вот-вот случится
// (foresee) — мститель идёт к обидчику, орда к деревне, двое к одному грузу.
// Предвестие поднимает интересность своей линии (или само становится
// кандидатом, если линии нет), а когда до кульминации остаётся несколько
// реальных секунд — перехватывает камеру, не дожидаясь срока, и держит её,
// пока всё не случится.
//
// Главные герои (12а.3): режиссёр знает состав сериала (CastTracker) и ведёт
// эпизоды — помнит, когда показывал каждого героя. Линии и предвестия с
// главным героем интереснее остальных (тем больше, чем дольше его не
// показывали), в линии камера берёт героя сериала; в затишье режиссёр просто
// навещает давно не виденного героя. Новый герой входит в кадр с подписью,
// через кого и каким событием он связан с уже знакомыми.
//
// Темп и монтаж (12а.4): режиссёр чередует напряжение и тишину — после
// досмотренной кульминации даёт передышку (не бросается к следующей погоне,
// а остаётся с героем или навещает героя за мирным делом), две горячие линии
// ведёт параллельным монтажом (камера ходит между ними, «Тем временем…»), а
// в затишье, когда ничего не надвигается, сам ускоряет время и возвращает
// скорость, как только что-то начинает надвигаться. Скорость пользователя —
// основа: её режиссёр запоминает и возвращает, когда его выключают.
//
// Операторская работа (12а.5, только вид сверху): у кадра есть план — общий
// (орда у поселения), средний (группа, двое перед столкновением, гонка за
// грузом), крупный (герой); режиссёр кадрирует всё, что должно быть в кадре,
// далёкую смену кадра делает пролётом с отъездом, в кульминации медленно
// наезжает и замедляет время до x1, пока всё не случится (Camerawork).
//
// Подписи и титры (12а.6): внизу кадра — имя того, за кем следят, его
// положение (вожак, укушен, ранен) и одна строка, что он хочет и почему
// («идёт мстить Peter Green за то, что он обокрал его дом»); при возвращении
// к главному герою — «ранее в сериях» и «пока нас не было»; когда камера
// уходит от человека (или он гибнет в кадре) — итог эпизода («Mary Cole
// погибла, защищая дом»). Итоги копятся в окне — краткое содержание сериала.
//
// Мир режиссёр только читает (скорость времени — не состояние мира). Выбор
// пользователя (щелчок по другому объекту) выключает режиссёра.
class Director {
public:
    explicit Director(const World& world);

    void setEnabled(bool enabled);
    bool enabled() const { return enabled_; }

    // Раз за кадр после шага мира, до Inspector::updateCamera: дочитать
    // летопись, при надобности сменить линию и героя, поправить скорость
    // времени (темп). dt — реальные секунды.
    void update(Inspector& inspector, SimDriver& driver, float dt);
    // Раз за кадр после Inspector::updateCamera: план кадра, пролёты, наезд
    // (только вид сверху). alpha — доля тика для плавной отрисовки.
    void updateCamera(CameraController& camera, float alpha, float dt);
    // Окно «Режиссёр»: включение, текущая линия, список линий, что вот-вот
    // случится, события выбранной линии.
    void drawPanel(Inspector& inspector);
    // Подпись в кадре сверху: какая линия, кто в ней, последнее событие и что
    // вот-вот случится.
    void drawCaption(Inspector& inspector);

private:
    // Кандидат на кадр: линия (с предвестием или без) или предвестие без линии.
    struct Pick {
        std::uint32_t story = 0; // Story::id; 0 — линии нет
        bool hasOmen = false;
        Foresight omen;          // предвестие, если hasOmen
        // Навестить главного героя без линии и предвестия (12а.3).
        EntityId visit = kInvalidId;
        EntityId target = kInvalidId;
        float interest = 0.0f;

        bool valid() const { return story != 0 || hasOmen || visit != kInvalidId; }
        // Тот же кандидат: та же линия, а без линии — то же предвестие.
        bool same(const Pick& other) const;
    };

    // За кем следить в линии: герой, иначе то, о чём она (группа, ивент);
    // kInvalidId — ни того, ни другого уже нет в мире.
    EntityId targetOf(const Story& story) const;
    bool exists(EntityId id) const;
    // Главный герой, о котором кандидат: кого навещают, за кем следят, герой
    // или противник линии, кто-то из её участников, герой предвестия.
    EntityId protagonistOf(const Pick& pick) const;
    // Насколько по герою соскучились: 0 — видели только что, 1 — давно (или
    // ни разу).
    float missed(EntityId hero) const;
    // Поправка на главных героев: в линии камера берёт героя сериала, а
    // интересность растёт.
    void rate(Pick& pick) const;
    // Кандидаты по убыванию интересности.
    std::vector<Pick> candidates() const;
    // Кандидат для линии или предвестия, выбранных в окне.
    Pick pickOf(const Story& story) const;
    Pick pickOf(const Foresight& omen) const;
    // Через сколько реальных секунд кульминация предвестия при нынешней
    // скорости времени (на паузе — очень нескоро).
    float realEta(const Foresight& omen) const;
    bool urgent(const Pick& pick) const;
    // Почему сменился кадр (12а.4): для подписи и монтажа.
    enum class Cut : std::uint8_t {
        Start,    // режиссёра включили, или прежний кадр остыл
        Switch,   // нашлось заметно интереснее
        Rush,     // успеть к кульминации
        Parallel, // параллельный монтаж: к второй горячей линии
        Retarget, // в том же кадре сменился герой
        Calm,     // передышка: мирная сцена после напряжения
        Chosen,   // выбрали в окне
    };

    // Перевести камеру на кандидата.
    void show(const Pick& pick, Inspector& inspector, Cut cut);
    // Насколько напряжён кадр сейчас, 0..1: срочное предвестие — 1, иначе по
    // интересности.
    float intensity() const;
    // Занят ли человек мирным делом: спит, ест, работает, разговаривает.
    bool calm(EntityId id) const;
    // Вторая горячая линия для параллельного монтажа; nullptr — нет.
    const Pick* parallelPartner(const std::vector<Pick>& picks, float currentInterest) const;
    // Мирная сцена для передышки; nullptr — нет.
    const Pick* calmPick(const std::vector<Pick>& picks) const;
    // Затишье: ничего не надвигается и в кадре скучно.
    bool quiet() const;
    // Темп: ускорить время в затишье, замедлить в кульминации, вернуть
    // скорость пользователя.
    void pace(SimDriver& driver, float dt);
    // Где сейчас объект (метры): существо, здание, орда и группа — по вожаку,
    // ивент и поселение; false — нет в мире.
    bool positionOf(EntityId id, float alpha, Vector2& at) const;
    // План кадра для текущего кандидата (12а.5).
    ShotPlan planOf(float alpha) const;
    // «John Miller идёт мстить Peter Green — 120 м, ~2 мин».
    std::string omenLine(const Foresight& omen) const;
    // Учёт эпизодов: кого показывают сейчас и когда показывали каждого.
    void trackEpisode();
    // Раздел «Главные герои» в окне; щелчок по герою — к нему.
    void drawCast(Inspector& inspector);
    // Строки о герое сериала в подписи: эпизод, «возвращаемся», дебют.
    void drawHeroCaption(EntityId hero, Inspector& inspector);
    // Подписи и титры (12а.6): за кем камера сейчас и что он делает; ушли от
    // него или он погиб — итог эпизода.
    void trackSubject();
    void closeSubject();
    // Титры внизу кадра: итог эпизода, «ранее в сериях», имя и намерение.
    void drawTitles(Inspector& inspector);
    // Раздел «Итоги эпизодов» в окне.
    void drawSummaries(Inspector& inspector);

    // Эпизоды главного героя (реальные секунды, только для режиссёра).
    struct Episodes {
        int count = 0;          // сколько раз камера возвращалась к нему
        float lastSeen = 0.0f;  // когда показывали в последний раз (clock_)
        std::uint64_t lastTick = 0; // тик мира тогда же
    };

    const World& world_;
    StoryTracker stories_;
    std::vector<Foresight> omens_; // что вот-вот случится, по убыванию интересности
    bool enabled_ = false;
    Pick current_;               // кого ведёт режиссёр
    // Кандидат пропал из списка (предвестие без линии сбылось): режиссёр
    // досматривает, что вышло, а held_ считает с этого мига.
    bool lost_ = false;
    EntityId shown_ = kInvalidId; // кого он выбрал в инспекторе
    float held_ = 0.0f;          // секунд на этом кандидате (или с его кульминации)
    float sinceChoice_ = 0.0f;   // секунд с прошлого пересмотра
    // Секунд симуляции на реальную секунду (сглажено): x1 — 1, пауза — 0.
    float simRate_ = 1.0f;
    std::uint64_t lastTick_ = 0;
    std::uint32_t opened_ = 0;   // линия, открытая в окне
    CastTracker cast_;           // главные герои (12а.3)
    std::unordered_map<EntityId, Episodes> episodes_; // только поиск
    float clock_ = 0.0f;               // реальные секунды с запуска режиссёра
    EntityId episodeHero_ = kInvalidId; // чей эпизод идёт
    float episodeStart_ = 0.0f;
    float returnedAfter_ = 0.0f;  // сколько его не показывали до этого эпизода

    // Темп и монтаж (12а.4).
    bool tempo_ = true;           // ускорять время в затишье
    Cut cut_ = Cut::Start;        // почему сменился кадр
    float cutAt_ = 0.0f;          // когда (clock_)
    float tension_ = 0.0f;        // сглаженное напряжение последних секунд, 0..1
    float breather_ = 0.0f;       // сколько секунд передышки осталось
    Pick partner_;                // вторая линия параллельного монтажа
    float currentInterest_ = 0.0f; // интересность текущего кадра на прошлом пересмотре
    float quietFor_ = 0.0f;       // сколько секунд затишье
    // Скорость пользователя (0 — режиссёр скоростью не управляет) и та, что
    // поставил сам режиссёр: если они разошлись, скорость сменил пользователь.
    int baseSpeed_ = 0;
    int setSpeed_ = 0;
    float userPaced_ = 0.0f;      // секунд с тех пор, как пользователь сменил скорость

    // Операторская работа (12а.5).
    Camerawork camerawork_;
    ShotSize shot_ = ShotSize::Close; // план текущего кадра
    // Замедление в кульминации: держится, пока предвестие не сбудется (не
    // дольше kSlowMax); для того же кадра — один раз.
    bool slowmo_ = false;
    bool slowmoSpent_ = false;
    float slowmoFor_ = 0.0f;

    // Подписи и титры (12а.6).
    EntityId subject_ = kInvalidId;  // за кем камера (для итога эпизода)
    std::uint64_t subjectSince_ = 0; // с какого тика
    float subjectShownAt_ = 0.0f;    // с какой секунды (clock_)
    bool subjectAlive_ = false;      // был жив, когда за ним следили
    Witnessed subjectWatch_;         // что он делал в последний раз
    struct Summary {
        EntityId who = kInvalidId;
        std::uint64_t tick = 0;
        std::string text;
    };
    std::vector<Summary> summaries_; // итоги эпизодов по порядку
    float summaryAt_ = -1.0e9f;      // когда показали последний итог (clock_)
    // «Ранее в сериях»: к кому вернулись, когда и что показать.
    EntityId recapHero_ = kInvalidId;
    float recapAt_ = -1.0e9f;
    std::vector<std::uint32_t> recapBefore_; // ранее в сериях
    std::vector<std::uint32_t> recapSince_;  // пока нас не было
};

} // namespace ui
} // namespace lw
