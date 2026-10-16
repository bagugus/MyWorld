#pragma once

#include "sim/Calendar.h"
#include "sim/Components.h"

#include <entt/entt.hpp>

#include <cstdint>

namespace lw {

class World;

// Мирная жизнь (этап 7.2): профессии, смены, деньги, магазины. Подробности и
// числа — docs/architecture.md, раздел «Мирная жизнь».
//
// Профессия задаёт место работы (вид здания), смены и плату. Работают в своём
// поселении: до соседнего — часы пути. Смена не переходит через полночь.

struct ProfessionSpec {
    BuildingKind workplace = BuildingKind::Shop;
    // Дневная смена, минуты суток.
    int start = 0;
    int end = 0;
    // Вечерняя смена — у полиции и врачей, достаётся совам; start == end — нет.
    int lateStart = 0;
    int lateEnd = 0;
    int workdays = 5;   // рабочих дней в неделю
    bool weekdays = false; // работает с понедельника по пятницу (школа)
    int wage = 0;       // плата за игровой час на месте работы
};

const ProfessionSpec& professionSpec(Profession profession);

// Цена предмета в магазине; 0 — не продаётся.
int priceOf(ItemKind kind);
inline constexpr int kFoodPrice = 4;

// Пособие без работы — в сутки: на еду его не хватает, сбережения тают.
inline constexpr int kAllowance = 2;

// Рабочий ли день недели.
bool isWorkday(const Job& job, int dayOfWeek);
// На смене ли сейчас: рабочий день и минута суток внутри смены.
bool onShift(const Job& job, const CalendarTime& time);
// Тик конца сегодняшней смены — идущей или ещё не начавшейся; tick — если
// сегодня выходной или смена кончилась.
std::uint64_t shiftEndTick(const Job& job, std::uint64_t tick);

// Работник на месте работы, на смене и не спит: продавец и врач — у двери,
// фермер — в поле (за работой), полицейский — в обходе (за работой).
bool isOnDuty(const World& world, entt::entity person);

// Открыто ли здание — кто-то из работников на месте (Workplace::onDuty).
bool isOpen(const World& world, EntityId building);

// Полицейский на службе ближе radius метров к точке.
bool policeNearby(const World& world, float x, float y, float radius);

// Раздать профессии жителям поселений и всем — сбережения (поток RngStream::Jobs).
// Места — по числу жителей: в городе врачи, полиция, учителя и продавцы, в
// деревне продавец и фермеры; кому — по чертам характера, с разбросом.
// Остальные без работы. Каждое занятое здание получает Workplace.
void assignJobs(World& world);

} // namespace lw
