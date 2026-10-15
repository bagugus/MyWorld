#include "sim/Weapons.h"

#include <iterator>

namespace lw {

namespace {

// ranged, досягаемость / уверенный выстрел, предел, интервал, точность, урон,
// в голову, кровотечение, громкость.
constexpr AttackSpec kWeapons[] = {
    {false, 1.4f, 1.4f, 1.0f, 0.75f, 0.07f, 0.0f, 0.05f, 0.0f},      // кулаки
    {false, 1.5f, 1.5f, 0.9f, 0.75f, 0.2f, 0.05f, 0.2f, 0.0f},       // нож
    {false, 1.8f, 1.8f, 1.4f, 0.7f, 0.42f, 0.15f, 0.3f, 0.0f},       // топор
    {true, 25.0f, 50.0f, 0.8f, 0.65f, 0.5f, 0.15f, 0.4f, 250.0f},    // пистолет
    {true, 18.0f, 35.0f, 1.6f, 0.85f, 0.8f, 0.35f, 0.6f, 300.0f},    // дробовик
    {true, 100.0f, 200.0f, 2.0f, 0.8f, 0.85f, 0.3f, 0.5f, 450.0f},   // винтовка
};
static_assert(std::size(kWeapons) == static_cast<std::size_t>(WeaponKind::Count));

// Ружьё без патронов — дубина.
constexpr AttackSpec kClub = {false, 1.6f, 1.6f, 1.2f, 0.7f, 0.13f, 0.03f, 0.1f, 0.0f};
constexpr AttackSpec kZombie = {false, 1.5f, 1.5f, 1.5f, 0.55f, 0.09f, 0.0f, 0.08f, 0.0f};
constexpr AttackSpec kBoar = {false, 1.6f, 1.6f, 1.2f, 0.6f, 0.15f, 0.0f, 0.2f, 0.0f};

// Уверенность в бою: от кулаков до дробовика.
constexpr float kPower[] = {0.15f, 0.4f, 0.65f, 0.7f, 0.85f, 0.8f};
static_assert(std::size(kPower) == static_cast<std::size_t>(WeaponKind::Count));
constexpr float kClubPower = 0.25f;

} // namespace

const AttackSpec& weaponSpec(WeaponKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    return kWeapons[index < std::size(kWeapons) ? index : 0];
}

bool isFirearm(WeaponKind kind) {
    return weaponSpec(kind).ranged;
}

const AttackSpec& attackSpec(const Weapon& weapon) {
    if (isFirearm(weapon.kind) && weapon.ammo == 0) {
        return kClub;
    }
    return weaponSpec(weapon.kind);
}

const AttackSpec& zombieAttack() {
    return kZombie;
}

const AttackSpec& boarAttack() {
    return kBoar;
}

float weaponPower(const Weapon& weapon) {
    if (isFirearm(weapon.kind) && weapon.ammo == 0) {
        return kClubPower;
    }
    const auto index = static_cast<std::size_t>(weapon.kind);
    return kPower[index < std::size(kPower) ? index : 0];
}

} // namespace lw
