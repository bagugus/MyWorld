#include "sim/Chronicle.h"

#include "sim/Hash.h"

#include <iterator>

namespace lw {

namespace {

// Hasher хранит только 64-битное состояние; продолжить с сохранённого нельзя,
// поэтому хеш записи примешивается к накопленному.
std::uint64_t mix(std::uint64_t accumulated, const ChronicleEvent& event) {
    Hasher hasher;
    hasher.add(accumulated);
    hasher.add(event.tick);
    hasher.add(static_cast<std::uint8_t>(event.kind));
    hasher.add(event.subject);
    hasher.add(event.object);
    hasher.add(event.a);
    hasher.add(event.b);
    hasher.add(event.third);
    return hasher.value();
}

} // namespace

void Chronicle::add(const ChronicleEvent& event) {
    const auto index = static_cast<std::uint32_t>(events_.size());
    events_.push_back(event);
    if (event.subject != kInvalidId) {
        byId_[event.subject].push_back(index);
    }
    if (event.object != kInvalidId && event.object != event.subject) {
        byId_[event.object].push_back(index);
    }
    if (event.third != kInvalidId && event.third != event.subject && event.third != event.object) {
        byId_[event.third].push_back(index);
    }
    ++counts_[static_cast<std::size_t>(event.kind)];
    hash_ = mix(hash_, event);
}

std::span<const std::uint32_t> Chronicle::eventsOf(EntityId id) const {
    const auto it = byId_.find(id);
    if (it == byId_.end()) {
        return {};
    }
    return it->second;
}

const char* chronicleKindName(ChronicleKind kind) {
    constexpr const char* kNames[] = {"settled",    "scavenged",  "brought home", "ate last food",
                                      "drank last water", "slept outside", "starving",
                                      "dehydrated", "exhausted",  "weakened",     "dying",
                                      "recovered",  "fled",       "horde formed", "horde merged",
                                      "horde arrived", "horde scattered", "bitten",
                                      "killed zombie", "hunted", "died", "butchered",
                                      "turned", "feverish", "found weapon", "looted body",
                                      "treated", "befriended", "disliked", "saved by",
                                      "defended by", "abandoned by", "caught thief",
                                      "mourned", "saw turned", "witnessed", "heard of",
                                      "reconciled", "group formed", "joined group",
                                      "left group", "expelled", "led group",
                                      "group disbanded", "shared", "traded", "stole",
                                      "deserted", "betrayed by", "assaulted by", "avenged"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(ChronicleKind::Count));
    const auto index = static_cast<std::size_t>(kind);
    return index < std::size(kNames) ? kNames[index] : "?";
}

} // namespace lw
