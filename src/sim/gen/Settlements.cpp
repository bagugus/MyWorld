// Выбор мест для городов и деревень.

#include "sim/gen/GenContext.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace lw::gen {

namespace {

constexpr float kCellArea = Map::kCellSize * Map::kCellSize;

// Пригодность места оценивается по блокам клеток, а не по каждой клетке.
constexpr int kBlock = 8;
// Центр поселения — середина блока, и он должен быть узлом дорожной сетки.
static_assert(kBlock / 2 % kRoadStep == 0 && kBlock % kRoadStep == 0);
constexpr int kEdgeMargin = 6; // клетки от границы поселения до края карты

struct KindRules {
    SettlementKind kind;
    int minRadius; // клетки
    int maxRadius;
    float landPerSettlement; // км² суши на одно поселение
    float maxWater;          // допустимая доля воды в круге поселения
    float maxSlope;          // средний уклон, проценты
    float maxRelief;         // перепад высот в круге, метры
    float coastBonus;        // насколько важна близость моря
    float jitter;            // случайная добавка к оценке
};
constexpr KindRules kTown = {SettlementKind::Town, 64, 88, 18.0f, 0.05f, 8.0f, 30.0f, 0.8f, 0.2f};
constexpr KindRules kVillage = {SettlementKind::Village, 20, 32, 3.0f, 0.08f, 11.0f, 18.0f, 0.0f, 0.35f};

// Минимальные расстояния между центрами, клетки.
constexpr int kTownSpacing = 550;
constexpr int kMixedSpacing = 170;
constexpr int kVillageSpacing = 140;

constexpr float kWaterNear = 150.0f;     // клетки: дальше вода уже не плюс
constexpr float kCoastNear = 120.0f;
constexpr float kSpreadDistance = 500.0f; // клетки: дальше от соседей — не лучше
constexpr float kSpreadWeight = 0.6f;

const char* const kTownNames[] = {
    "Северогорск", "Каменск",      "Светлогорск", "Приморск",   "Зеленогорск", "Берёзовск",
    "Черноречье",  "Краснополье",  "Староозёрск", "Новосёлов",  "Усть-Ольшанск", "Белогорье",
    "Тихореченск", "Верхнекаменск", "Дальнегорск", "Сосновоборск",
};
const char* const kVillageNames[] = {
    "Берёзовка",  "Каменка",        "Ольховка",    "Сосновка",   "Дубки",       "Малиновка",
    "Тихая Заводь", "Рябиновка",    "Липки",       "Заречье",    "Подгорное",   "Красное",
    "Мшистое",    "Ключи",          "Медвежье",    "Волчий Лог", "Старая Мельница", "Орехово",
    "Вишнёвое",   "Калиновка",      "Луговое",     "Пчельники",  "Гремячее",    "Студёное",
    "Дальнее",    "Верхние Ручьи",  "Нижние Ручьи", "Погост",    "Журавли",     "Озёрное",
    "Глинки",     "Хмелёвка",       "Сухой Лог",   "Осиновка",   "Борки",       "Высокое",
    "Родники",    "Кривой Яр",      "Тополёвка",   "Ясное",
};

// Выдаёт названия из списка в случайном порядке без повторов; когда список
// кончился, идёт по второму кругу с номером.
class NamePool {
public:
    template <std::size_t N>
    explicit NamePool(const char* const (&names)[N]) : names_(names), count_(N) {}

    std::string take(Rng& rng) {
        if (remaining_.empty()) {
            ++round_;
            for (std::size_t i = 0; i < count_; ++i) {
                remaining_.push_back(i);
            }
        }
        const std::size_t slot = rng.below(static_cast<std::uint32_t>(remaining_.size()));
        const std::size_t index = remaining_[slot];
        remaining_[slot] = remaining_.back();
        remaining_.pop_back();

        std::string name = names_[index];
        if (round_ > 1) {
            name += ' ';
            name += std::to_string(round_);
        }
        return name;
    }

private:
    const char* const* names_;
    std::size_t count_;
    std::vector<std::size_t> remaining_;
    int round_ = 0;
};

struct Block {
    int blocked = 0; // клетки вне главной суши (вода, острова)
    int river = 0;
    int slopeSum = 0;   // проценты
    float elevation = 0.0f; // средняя, метры
};

struct Candidate {
    Cell center;
    float score = 0.0f;
};

int spacing(SettlementKind a, SettlementKind b) {
    if (a == SettlementKind::Town && b == SettlementKind::Town) {
        return kTownSpacing;
    }
    if (a == SettlementKind::Village && b == SettlementKind::Village) {
        return kVillageSpacing;
    }
    return kMixedSpacing;
}

} // namespace

void placeSettlements(Context& ctx) {
    const Map& map = ctx.map;
    const int blocksX = ctx.width / kBlock;
    const int blocksY = ctx.height / kBlock;
    if (blocksX == 0 || blocksY == 0) {
        return;
    }

    std::size_t mainlandCells = 0;
    for (const std::uint8_t cell : ctx.mainland.cells()) {
        mainlandCells += cell;
    }
    const float landKm2 = static_cast<float>(mainlandCells) * kCellArea / 1.0e6f;

    Grid<Block> blocks(blocksX, blocksY);
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            Block& block = blocks.at(bx, by);
            int elevationSum = 0;
            for (int y = by * kBlock; y < (by + 1) * kBlock; ++y) {
                for (int x = bx * kBlock; x < (bx + 1) * kBlock; ++x) {
                    block.blocked += ctx.mainland.at(x, y) ? 0 : 1;
                    block.river += map.water.at(x, y) == Water::River ? 1 : 0;
                    block.slopeSum += ctx.slope.at(x, y);
                    elevationSum += map.elevation.at(x, y);
                }
            }
            block.elevation = static_cast<float>(elevationSum) * Map::kElevationStep /
                              static_cast<float>(kBlock * kBlock);
        }
    }

    // Оценка каждого блока как центра поселения данного вида; непригодные отброшены.
    const auto findCandidates = [&](const KindRules& rules) {
        std::vector<Candidate> candidates;
        const int radius = rules.maxRadius;
        const int reach = (radius + kBlock - 1) / kBlock;
        for (int by = 0; by < blocksY; ++by) {
            for (int bx = 0; bx < blocksX; ++bx) {
                const Cell center{bx * kBlock + kBlock / 2, by * kBlock + kBlock / 2};
                // Случайная добавка тянется для каждого блока, чтобы оценка
                // одного не зависела от пригодности остальных.
                const float jitter = ctx.rng.unit() * rules.jitter;

                const int margin = radius + kEdgeMargin;
                if (center.x < margin || center.y < margin || center.x >= ctx.width - margin ||
                    center.y >= ctx.height - margin) {
                    continue;
                }
                if (!ctx.mainland.at(center.x, center.y) ||
                    map.water.at(center.x, center.y) != Water::None) {
                    continue;
                }

                int cells = 0;
                int blocked = 0;
                int river = 0;
                int slopeSum = 0;
                float low = blocks.at(bx, by).elevation;
                float high = low;
                bool inside = true;
                for (int dy = -reach; dy <= reach && inside; ++dy) {
                    for (int dx = -reach; dx <= reach; ++dx) {
                        if (dx * dx + dy * dy > reach * reach) {
                            continue;
                        }
                        if (!blocks.contains(bx + dx, by + dy)) {
                            inside = false;
                            break;
                        }
                        const Block& block = blocks.at(bx + dx, by + dy);
                        cells += kBlock * kBlock;
                        blocked += block.blocked;
                        river += block.river;
                        slopeSum += block.slopeSum;
                        low = std::min(low, block.elevation);
                        high = std::max(high, block.elevation);
                    }
                }
                if (!inside) {
                    continue;
                }

                const float total = static_cast<float>(cells);
                const float water = (static_cast<float>(blocked) + 0.5f * static_cast<float>(river)) / total;
                const float slope = static_cast<float>(slopeSum) / total;
                if (water > rules.maxWater || slope > rules.maxSlope || high - low > rules.maxRelief) {
                    continue;
                }

                // Вода рядом — плюс, а городу особенно нужен берег моря (порт).
                const float toSea = static_cast<float>(ctx.seaDistance.at(center.x, center.y) - radius);
                const float toWater = std::min(
                    toSea, static_cast<float>(ctx.freshDistance.at(center.x, center.y) - radius));
                float score = 1.0f - slope / rules.maxSlope;
                score += 0.6f * (1.0f - std::clamp(toWater / kWaterNear, 0.0f, 1.0f));
                score += rules.coastBonus * (1.0f - std::clamp(toSea / kCoastNear, 0.0f, 1.0f));
                candidates.push_back({center, score + jitter});
            }
        }
        return candidates;
    };

    NamePool townNames(kTownNames);
    NamePool villageNames(kVillageNames);

    // Жадный выбор: лучшее место, не слишком близкое к уже выбранным; чем
    // дальше от соседей, тем лучше — иначе все сядут в одной долине.
    const auto place = [&](const KindRules& rules, NamePool& names) {
        const int wanted = static_cast<int>(std::lround(landKm2 / rules.landPerSettlement));
        if (wanted <= 0) {
            return;
        }
        const std::vector<Candidate> candidates = findCandidates(rules);
        for (int n = 0; n < wanted; ++n) {
            const Candidate* best = nullptr;
            float bestScore = 0.0f;
            for (const Candidate& candidate : candidates) {
                float nearest = kSpreadDistance;
                bool allowed = true;
                for (const SettlementSite& site : ctx.out.settlements) {
                    const float dx = static_cast<float>(candidate.center.x - site.center.x);
                    const float dy = static_cast<float>(candidate.center.y - site.center.y);
                    const float distance = std::sqrt(dx * dx + dy * dy);
                    if (distance < static_cast<float>(spacing(rules.kind, site.kind))) {
                        allowed = false;
                        break;
                    }
                    nearest = std::min(nearest, distance);
                }
                if (!allowed) {
                    continue;
                }
                const float score = candidate.score + kSpreadWeight * nearest / kSpreadDistance;
                if (best == nullptr || score > bestScore) {
                    best = &candidate;
                    bestScore = score;
                }
            }
            if (best == nullptr) {
                break;
            }

            SettlementSite site;
            site.kind = rules.kind;
            site.center = best->center;
            site.radius = ctx.rng.rangeInt(rules.minRadius, rules.maxRadius);
            site.name = names.take(ctx.rng);
            ctx.out.settlements.push_back(std::move(site));
        }
    };

    place(kTown, townNames);
    place(kVillage, villageNames);
}

} // namespace lw::gen
