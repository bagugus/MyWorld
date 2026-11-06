#include "render/FieldLayout.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lw {

namespace {

// Доли культур среди лоскутов (накопленные): как CROP_SHARES в kFieldGlsl.
constexpr float kCropShares[] = {0.10f, 0.40f, 0.55f, 0.65f, 0.77f, 0.87f, 0.94f, 1.0f};

CropKind cropOf(std::uint32_t h) {
    const float u = hashUnit(h);
    for (int i = 0; i < 8; ++i) {
        if (u < kCropShares[i]) {
            return static_cast<CropKind>(i);
        }
    }
    return CropKind::Rapeseed;
}

struct Site {
    float x = 0.0f;
    float z = 0.0f;
};

Site patchSite(int cx, int cz) {
    return {(static_cast<float>(cx) + 0.2f + 0.6f * hashUnit(fieldHash(cx, cz, 1u))) * kFieldPatchSize,
            (static_cast<float>(cz) + 0.2f + 0.6f * hashUnit(fieldHash(cx, cz, 2u))) * kFieldPatchSize};
}

} // namespace

std::uint32_t fieldHash(int x, int y, std::uint32_t salt) {
    std::uint32_t h = (static_cast<std::uint32_t>(x) * 0x8DA6B343u) ^ (static_cast<std::uint32_t>(y) * 0xD8163841u) ^
                      (salt * 0xCB1AB31Fu);
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

FieldPatch fieldPatchAt(float x, float z) {
    const int baseX = static_cast<int>(std::floor(x / kFieldPatchSize));
    const int baseZ = static_cast<int>(std::floor(z / kFieldPatchSize));
    Site sites[9];
    Site best{};
    int bestX = baseX;
    int bestZ = baseZ;
    float bestDistance = 1.0e20f;
    // Порядок обхода и сравнения — как в fieldPatch (kFieldGlsl).
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            const Site site = patchSite(baseX + i, baseZ + j);
            sites[(j + 1) * 3 + (i + 1)] = site;
            const float dx = x - site.x;
            const float dz = z - site.z;
            const float distance = dx * dx + dz * dz;
            if (distance < bestDistance) {
                bestDistance = distance;
                best = site;
                bestX = baseX + i;
                bestZ = baseZ + j;
            }
        }
    }
    float edge = 1.0e20f;
    for (const Site& site : sites) {
        const float dx = best.x - site.x;
        const float dz = best.z - site.z;
        const float length = std::sqrt(dx * dx + dz * dz);
        if (length < 1.0e-3f) {
            continue;
        }
        const float mx = 0.5f * (best.x + site.x);
        const float mz = 0.5f * (best.z + site.z);
        edge = std::min(edge, ((x - mx) * dx + (z - mz) * dz) / length);
    }
    FieldPatch result;
    result.crop = cropOf(fieldHash(bestX, bestZ, 4u));
    result.angle = hashUnit(fieldHash(bestX, bestZ, 5u)) * std::numbers::pi_v<float>;
    result.timing = hashUnit(fieldHash(bestX, bestZ, 6u));
    result.edge = edge;
    result.id = bestX * 65536 + bestZ;
    result.bits = fieldHash(bestX, bestZ, 7u);
    return result;
}

float rowSpacing(CropKind crop) {
    switch (crop) {
    case CropKind::Corn:
    case CropKind::Potato:
        return 0.75f;
    case CropKind::Sunflower:
        return 0.7f;
    case CropKind::Garden:
        return 0.6f;
    default:
        return 0.5f;
    }
}

void rowNormal(float angle, float& nx, float& nz) {
    nx = -std::sin(angle);
    nz = std::cos(angle);
}

const char* const kFieldGlsl = R"(
uint hash3(int x, int y, uint salt) {
    uint h = (uint(x) * 0x8DA6B343u) ^ (uint(y) * 0xD8163841u) ^ (salt * 0xCB1AB31Fu);
    h ^= h >> 16u;
    h *= 0x7FEB352Du;
    h ^= h >> 15u;
    h *= 0x846CA68Bu;
    h ^= h >> 16u;
    return h;
}
float hashUnit(uint h) {
    return float(h >> 8u) / 16777216.0;
}
float ramp(float a, float b, float y) {
    return clamp((y - a) / (b - a), 0.0, 1.0);
}

const float kPatchSize = 112.0;
const float kBalkWidth = 1.2;
const int CROP_FALLOW = 0;
const int CROP_WHEAT = 1;
const int CROP_RYE = 2;
const int CROP_BARLEY = 3;
const int CROP_CORN = 4;
const int CROP_POTATO = 5;
const int CROP_SUNFLOWER = 6;
const int CROP_RAPESEED = 7;
const int CROP_GARDEN = 8;
const float CROP_SHARES[8] = float[8](0.10, 0.40, 0.55, 0.65, 0.77, 0.87, 0.94, 1.0);

vec2 patchSite(ivec2 c) {
    return (vec2(c) + 0.2 + 0.6 * vec2(hashUnit(hash3(c.x, c.y, 1u)), hashUnit(hash3(c.x, c.y, 2u)))) * kPatchSize;
}

struct Patch {
    int crop;
    float angle;
    float timing;
    float edge;
};

// Лоскут поля в точке: как fieldPatchAt (render/FieldLayout.cpp).
Patch fieldPatch(vec2 p) {
    ivec2 base = ivec2(floor(p / kPatchSize));
    vec2 sites[9];
    vec2 best = vec2(0.0);
    ivec2 bestCell = base;
    float bestDistance = 1.0e20;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            ivec2 c = base + ivec2(i, j);
            vec2 s = patchSite(c);
            sites[(j + 1) * 3 + (i + 1)] = s;
            vec2 d = p - s;
            float distance = dot(d, d);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = s;
                bestCell = c;
            }
        }
    }
    float edge = 1.0e20;
    for (int k = 0; k < 9; ++k) {
        vec2 diff = best - sites[k];
        float len = length(diff);
        if (len < 1.0e-3) {
            continue;
        }
        edge = min(edge, dot(p - 0.5 * (best + sites[k]), diff / len));
    }
    Patch result;
    float u = hashUnit(hash3(bestCell.x, bestCell.y, 4u));
    result.crop = 7;
    for (int i = 7; i >= 0; --i) {
        if (u < CROP_SHARES[i]) {
            result.crop = i;
        }
    }
    result.angle = hashUnit(hash3(bestCell.x, bestCell.y, 5u)) * 3.14159265;
    result.timing = hashUnit(hash3(bestCell.x, bestCell.y, 6u));
    result.edge = edge;
    return result;
}

float rowSpacing(int crop) {
    if (crop == CROP_CORN || crop == CROP_POTATO) {
        return 0.75;
    }
    if (crop == CROP_SUNFLOWER) {
        return 0.7;
    }
    return crop == CROP_GARDEN ? 0.6 : 0.5;
}

// Когда убирают: после этого на поле стерня или голая земля (и тюки соломы).
float harvestTime(int crop) {
    if (crop == CROP_RYE) {
        return 0.40;
    }
    if (crop == CROP_RAPESEED) {
        return 0.37;
    }
    if (crop == CROP_CORN) {
        return 0.56;
    }
    if (crop == CROP_POTATO) {
        return 0.48;
    }
    if (crop == CROP_SUNFLOWER) {
        return 0.55;
    }
    if (crop == CROP_GARDEN) {
        return 0.52;
    }
    return 0.44;
}

// Посев на долю года (с 1 марта; весна — 0..0,25, лето, осень, зима — по четверти).
// h — высота растений, м; cover — доля ширины ряда, занятая ими; plant — цвет;
// stubble — 0 пашня, 1 стерня; meadow — 1 пар (луг вместо посева).
struct Crop {
    float h;
    float cover;
    vec3 plant;
    float stubble;
    float meadow;
};

Crop cropState(int crop, float year, float timing) {
    float y = fract(year + 0.03 * (timing - 0.5));
    Crop c;
    c.h = 0.0;
    c.cover = 0.0;
    c.plant = vec3(0.30, 0.47, 0.16);
    c.stubble = 0.0;
    c.meadow = 0.0;
    vec3 straw = vec3(0.74, 0.66, 0.42);
    if (crop == CROP_FALLOW) {
        c.meadow = 1.0;
    } else if (crop == CROP_WHEAT || crop == CROP_BARLEY) {
        float tall = crop == CROP_BARLEY ? 0.75 : 0.95;
        vec3 gold = crop == CROP_BARLEY ? vec3(0.80, 0.72, 0.45) : vec3(0.80, 0.63, 0.28);
        if (y >= 0.11 && y < 0.44) {
            float g = ramp(0.11, 0.30, y);
            c.h = mix(0.04, tall, g);
            c.cover = mix(0.25, 0.9, g);
            c.plant = mix(mix(vec3(0.30, 0.47, 0.16), vec3(0.52, 0.56, 0.22), ramp(0.30, 0.34, y)), gold,
                          ramp(0.33, 0.42, y));
        } else if (y >= 0.44 && y < 0.66) {
            c.h = 0.12;
            c.cover = 0.85;
            c.plant = straw;
            c.stubble = 1.0 - ramp(0.62, 0.66, y);
            c.h *= c.stubble;
        }
    } else if (crop == CROP_RYE) {
        vec3 green = vec3(0.28, 0.45, 0.18);
        if (y >= 0.55 || y < 0.02) {
            c.h = 0.1 * (y >= 0.55 ? ramp(0.55, 0.62, y) : 1.0);
            c.cover = 0.4;
            c.plant = green;
        } else if (y < 0.40) {
            float g = ramp(0.02, 0.26, y);
            c.h = mix(0.1, 1.3, g);
            c.cover = mix(0.4, 0.9, g);
            c.plant = mix(green, vec3(0.74, 0.64, 0.40), ramp(0.30, 0.38, y));
        } else if (y < 0.50) {
            c.h = 0.14;
            c.cover = 0.85;
            c.plant = straw;
            c.stubble = 1.0 - ramp(0.47, 0.50, y);
            c.h *= c.stubble;
        }
    } else if (crop == CROP_RAPESEED) {
        vec3 green = vec3(0.22, 0.40, 0.18);
        if (y >= 0.53 || y < 0.03) {
            c.h = 0.15 * (y >= 0.53 ? ramp(0.53, 0.60, y) : 1.0);
            c.cover = 0.6;
            c.plant = green;
        } else if (y < 0.37) {
            float g = ramp(0.03, 0.12, y);
            c.h = mix(0.15, 1.2, g);
            c.cover = mix(0.6, 0.95, g);
            vec3 bloom = vec3(0.93, 0.83, 0.18);
            c.plant = mix(green, bloom, ramp(0.11, 0.14, y) * (1.0 - ramp(0.19, 0.23, y)));
            c.plant = mix(c.plant, vec3(0.40, 0.50, 0.20), ramp(0.19, 0.23, y));
            c.plant = mix(c.plant, vec3(0.48, 0.40, 0.24), ramp(0.29, 0.35, y));
        } else if (y < 0.48) {
            c.h = 0.2;
            c.cover = 0.6;
            c.plant = vec3(0.56, 0.50, 0.34);
            c.stubble = 1.0 - ramp(0.45, 0.48, y);
            c.h *= c.stubble;
        }
    } else if (crop == CROP_CORN) {
        if (y >= 0.13 && y < 0.56) {
            float g = ramp(0.13, 0.36, y);
            c.h = mix(0.08, 2.3, g * (2.0 - g));
            c.cover = mix(0.3, 0.75, g);
            c.plant = mix(vec3(0.27, 0.45, 0.15), vec3(0.70, 0.60, 0.38), ramp(0.44, 0.52, y));
        } else if (y >= 0.56 && y < 0.68) {
            c.h = 0.25;
            c.cover = 0.3;
            c.plant = vec3(0.66, 0.56, 0.36);
            c.stubble = 1.0 - ramp(0.64, 0.68, y);
            c.h *= c.stubble;
        }
    } else if (crop == CROP_POTATO) {
        if (y >= 0.12 && y < 0.48) {
            float g = ramp(0.12, 0.27, y);
            float wither = ramp(0.40, 0.47, y);
            c.h = mix(0.05, 0.55, g) * (1.0 - 0.4 * wither);
            c.cover = mix(0.3, 0.95, g);
            c.plant = mix(vec3(0.21, 0.40, 0.15), vec3(0.46, 0.40, 0.22), wither);
        }
    } else if (crop == CROP_SUNFLOWER) {
        if (y >= 0.12 && y < 0.55) {
            float g = ramp(0.12, 0.32, y);
            c.h = mix(0.06, 1.9, g * (2.0 - g));
            c.cover = mix(0.25, 0.6, g);
            c.plant = mix(vec3(0.30, 0.46, 0.16), vec3(0.38, 0.32, 0.20), ramp(0.44, 0.52, y));
        } else if (y >= 0.55 && y < 0.68) {
            c.h = 0.3;
            c.cover = 0.25;
            c.plant = vec3(0.34, 0.28, 0.20);
            c.stubble = 1.0 - ramp(0.64, 0.68, y);
            c.h *= c.stubble;
        }
    } else if (crop == CROP_GARDEN) {
        if (y >= 0.12 && y < 0.56) {
            float g = ramp(0.12, 0.26, y);
            c.h = mix(0.05, 0.45, g) * (1.0 - ramp(0.52, 0.56, y));
            c.cover = 0.95;
            c.plant = mix(vec3(0.26, 0.46, 0.18), vec3(0.40, 0.44, 0.22), ramp(0.42, 0.52, y));
        }
    }
    return c;
}

// Головка подсолнечника: бутон, жёлтый цветок, потом бурая корзинка.
vec3 sunflowerHead(float year, float timing) {
    float y = fract(year + 0.03 * (timing - 0.5));
    vec3 c = mix(vec3(0.30, 0.45, 0.16), vec3(0.95, 0.78, 0.10), ramp(0.29, 0.31, y));
    return mix(c, vec3(0.30, 0.22, 0.12), ramp(0.38, 0.44, y));
}

// Луг по сезону: прошлогодняя сухая трава → свежая зелень → лето → сухие
// пятна к концу лета (v — пятно 0..1) → бурая осень → зима.
vec3 meadowColor(float y, float v) {
    vec3 c = vec3(0.48, 0.44, 0.30);
    c = mix(c, vec3(0.40, 0.60, 0.22), ramp(0.04, 0.14, y));
    c = mix(c, vec3(0.35, 0.51, 0.20), ramp(0.20, 0.30, y));
    c = mix(c, vec3(0.62, 0.57, 0.31), ramp(0.36, 0.48, y) * smoothstep(0.35, 0.75, v));
    c = mix(c, vec3(0.50, 0.46, 0.27), ramp(0.50, 0.62, y));
    c = mix(c, vec3(0.46, 0.42, 0.29), ramp(0.72, 0.80, y));
    return c;
}

// Высота травы по сезону, доля от летней.
float meadowHeight(float y) {
    float h = mix(0.35, 1.0, ramp(0.04, 0.24, y));
    h = mix(h, 0.8, ramp(0.50, 0.65, y));
    return mix(h, 0.4, ramp(0.65, 0.80, y));
}
)";

} // namespace lw
