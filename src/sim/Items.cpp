#include "sim/Items.h"

#include "sim/Weapons.h"

#include <cmath>
#include <iterator>

namespace lw {

namespace {

// Вес, кг: еда, вода, мясо, бинт, лекарства, патроны трёх калибров, нож,
// топор, пистолет, дробовик, винтовка.
constexpr float kWeights[] = {0.5f,  1.0f,  0.5f, 0.1f, 0.2f, 0.02f, 0.05f,
                              0.03f, 0.3f,  1.5f, 1.0f, 3.0f, 4.0f};
static_assert(std::size(kWeights) == kItemKinds);

} // namespace

float itemWeight(ItemKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    return index < std::size(kWeights) ? kWeights[index] : 0.0f;
}

float inventoryWeight(const Inventory& inventory) {
    float weight = 0.0f;
    for (std::size_t i = 0; i < kItemKinds; ++i) {
        weight += static_cast<float>(inventory.items[i]) * kWeights[i];
    }
    return weight;
}

int roomFor(const Inventory& inventory, ItemKind kind, float capacity) {
    const float free = capacity - inventoryWeight(inventory);
    const float weight = itemWeight(kind);
    if (free <= 0.0f || weight <= 0.0f) {
        return 0;
    }
    return static_cast<int>(std::floor(free / weight + 1e-4f));
}

ItemKind itemOf(WeaponKind weapon) {
    switch (weapon) {
    case WeaponKind::Knife:
        return ItemKind::Knife;
    case WeaponKind::Axe:
        return ItemKind::Axe;
    case WeaponKind::Pistol:
        return ItemKind::Pistol;
    case WeaponKind::Shotgun:
        return ItemKind::Shotgun;
    case WeaponKind::Rifle:
        return ItemKind::Rifle;
    default:
        return ItemKind::Count;
    }
}

ItemKind ammoFor(WeaponKind weapon) {
    switch (weapon) {
    case WeaponKind::Pistol:
        return ItemKind::PistolAmmo;
    case WeaponKind::Shotgun:
        return ItemKind::ShotgunShells;
    case WeaponKind::Rifle:
        return ItemKind::RifleAmmo;
    default:
        return ItemKind::Count;
    }
}

WeaponKind weaponOf(ItemKind item) {
    switch (item) {
    case ItemKind::Knife:
        return WeaponKind::Knife;
    case ItemKind::Axe:
        return WeaponKind::Axe;
    case ItemKind::Pistol:
        return WeaponKind::Pistol;
    case ItemKind::Shotgun:
        return WeaponKind::Shotgun;
    case ItemKind::Rifle:
        return WeaponKind::Rifle;
    default:
        return WeaponKind::Fists;
    }
}

bool equipBest(Weapon& weapon, Inventory& inventory) {
    // Патроны к тому, что в руках, — в оружие.
    const auto load = [&inventory](Weapon& w) {
        if (const ItemKind ammo = ammoFor(w.kind); ammo != ItemKind::Count) {
            w.ammo = static_cast<std::uint16_t>(w.ammo + inventory.take(ammo, 0xFFFF));
        }
    };
    load(weapon);
    WeaponKind best = weapon.kind;
    float bestPower = weaponPower(weapon);
    for (std::size_t i = 0; i < kItemKinds; ++i) {
        const auto item = static_cast<ItemKind>(i);
        const WeaponKind kind = weaponOf(item);
        if (kind == WeaponKind::Fists || inventory.count(item) == 0) {
            continue;
        }
        const ItemKind ammo = ammoFor(kind);
        const Weapon candidate{kind, ammo != ItemKind::Count ? inventory.count(ammo)
                                                              : std::uint16_t{0}};
        const float power = weaponPower(candidate);
        if (power > bestPower) {
            best = kind;
            bestPower = power;
        }
    }
    if (best == weapon.kind) {
        return false;
    }
    // Прежнее — в инвентарь вместе с патронами.
    if (const ItemKind item = itemOf(weapon.kind); item != ItemKind::Count) {
        inventory.add(item, 1);
    }
    if (const ItemKind ammo = ammoFor(weapon.kind); ammo != ItemKind::Count) {
        inventory.add(ammo, weapon.ammo);
    }
    inventory.take(itemOf(best), 1);
    weapon = {best, 0};
    load(weapon);
    return true;
}

bool eatFrom(Inventory& inventory) {
    return inventory.take(ItemKind::Meat, 1) == 1 || inventory.take(ItemKind::Food, 1) == 1;
}

} // namespace lw
