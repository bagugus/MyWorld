#pragma once

#include "sim/Foresight.h"
#include "sim/Id.h"

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace lw {

struct Story;
class World;

namespace ui {

// Рассказчик сериала (этап 12а.13): голос за кадром вместо статусов.
// Говорит о людях языком истории — из летописи, памяти, отношений и черт:
//
//   знакомство — при первом появлении человека в кадре: кто он, чем живёт, с
//     кем, что с ним было («Фермер из Old Village. Живёт с семьёй: Mary и Tom.
//     Вчера Peter Green обокрал его дом.»);
//   мотив сцены — зачем мы здесь («John не может простить Peter Green: тот
//     обокрал его дом. Теперь он идёт за ним.»);
//   связка между сценами — чем новый герой связан с прежним («Тем временем
//     Peter Green, о котором John слышал от Mary, идёт к лесу.»);
//   реплика-характеристика — когда поступок выдаёт характер («John не умеет
//     прощать.»), и перемены в положении («John укусили.»).
//
// Всё только читает мир, рандома нет: тот же мир — те же слова. Имена — без
// номеров (Name), безымянные — «зомби».

// «Только что», «Сегодня утром», «Вчера», «Три дня назад», «Давно» — с большой
// буквы, для начала фразы.
std::string agoPhrase(std::uint64_t then, std::uint64_t now);

// Знакомство с человеком: две-три короткие фразы без имени (имя — в титре
// крупно). Пусто — не человек.
std::string introduction(const World& world, EntityId id);

// Мотив сцены: о чём она — по предвестию (omen, может быть nullptr), линии
// (story, может быть nullptr) или нуждам того, за кем камера (target). Пусто —
// сказать нечего (обычный быт).
std::string motive(const World& world, const Story* story, const Foresight* omen, EntityId target);

// Связка «Тем временем …»: чем человек to связан с from — помнит ли тот его
// поступок, в долгу ли, семья, товарищ, друг, недруг — и что to делает сейчас.
// Пусто — не связаны (или не люди).
std::string bridge(const World& world, EntityId from, EntityId to);

// Виды реплик-характеристик: каждая — о своей черте и поступке.
enum class Remark : std::uint8_t {
    Unforgiving, // мстит и злопамятен
    Coward,      // бежит и робок
    Fearless,    // дерётся и смел
    Generous,    // делится и добр
    Greedy,      // крадёт из жадности
    Desperate,   // крадёт впервые — от голода
    Lonely,      // ищет компанию и общителен
    Restless,    // гуляет, собирает и любопытен
    Hoarder,     // скупает впрок и запаслив
    Leader,      // вожак группы и прирождённый лидер
    Dutiful,     // работает, когда остальные бросили
    Shrewd,      // меняется и своего не упустит
    Count
};

// Реплика о характере человека — если то, что он делает сейчас, выдаёт черту.
// Пусто — нет. kind — какая это реплика (чтобы не повторяться).
std::string remark(const World& world, EntityId id, Remark& kind);

// Голос за кадром во время показа: очередь фраз, каждая держится по длине
// текста и плавно сменяется. Помнит, с кем зритель уже знаком и какие реплики
// звучали, — между сценами и эпизодами.
class Narrator {
public:
    // Сериал начался заново: забыть всё.
    void reset();
    // Новая сцена: связка от прошлого героя previous (kInvalidId — нет),
    // знакомство с subject, если он ещё не представлен, мотив сцены.
    // Недосказанное о прошлой сцене забывается.
    void beginScene(const World& world, EntityId previous, EntityId subject, std::string motiveLine);
    // Раз за кадр: subject — о ком титр, visible — титры видны (часы фраз идут
    // только тогда). dt — реальные секунды.
    void update(const World& world, EntityId subject, float dt, bool visible);

    // Что звучит сейчас и насколько видно, 0..1; пусто — ничего (титр
    // показывает, что человек делает).
    std::string_view line() const { return current_; }
    float alpha() const;
    // Рассказчик говорит: фраза, пауза между фразами или очередь — титр в это
    // время не переключается на «что делает».
    bool speaking() const { return !current_.empty() || !queue_.empty() || gap_ > 0.0f; }
    // Человек уже представлен зрителю.
    bool introduced(EntityId id) const { return introduced_.count(id) != 0; }

private:
    // Положение, о котором уже сказали: укушен, лихорадка, кровь, при смерти.
    static std::uint8_t conditionOf(const World& world, EntityId id);
    void queueIntroduction(const World& world, EntityId id);
    void say(std::string text);

    EntityId subject_ = kInvalidId;
    std::deque<std::string> queue_;
    std::string current_;
    float age_ = 0.0f;    // секунд показа текущей фразы
    float length_ = 0.0f; // сколько её держать
    float gap_ = 0.0f;    // пауза между фразами
    float clock_ = 0.0f;  // реальные секунды показа (только когда видно)
    std::unordered_set<EntityId> introduced_;
    std::unordered_map<EntityId, std::uint8_t> condition_;       // что о положении уже сказано
    std::unordered_map<std::uint64_t, float> remarked_;           // (id, вид) → clock_
    std::unordered_map<std::string, float> spoken_;               // фраза → clock_, когда звучала
};

} // namespace ui
} // namespace lw
