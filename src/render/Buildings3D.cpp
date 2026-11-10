#include "render/Buildings3D.h"

#include "render/InteriorStage.h"
#include "render/Lighting3D.h"
#include "render/Terrain3D.h"
#include "sim/Components.h"
#include "sim/World.h"

#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace lw {

namespace {

// Вершин в одной сетке не больше, чем вмещают индексы unsigned short; кратно четырём.
constexpr std::size_t kMaxMeshVertices = 65532;

template <typename T>
T* copyOf(const T* data, std::size_t count) {
    T* result = static_cast<T*>(MemAlloc(static_cast<unsigned int>(count * sizeof(T))));
    std::copy(data, data + count, result);
    return result;
}

// Грани [first, first + count) вершин из захвата — в видеопамять. На процессоре
// остаются только индексы: по ним DrawMesh понимает, что сетка индексная.
Mesh uploadQuads(const ModelMesh& source, std::size_t first, std::size_t count) {
    Mesh mesh{};
    mesh.vertexCount = static_cast<int>(count);
    mesh.triangleCount = static_cast<int>(count / 2);
    mesh.vertices = copyOf(source.vertices.data() + 3 * first, 3 * count);
    mesh.normals = copyOf(source.normals.data() + 3 * first, 3 * count);
    mesh.texcoords = copyOf(source.texcoords.data() + 2 * first, 2 * count);
    mesh.colors = copyOf(source.colors.data() + 4 * first, 4 * count);
    // Четырёхугольник a-b-c-d — два треугольника, как у батча rlgl.
    mesh.indices = static_cast<unsigned short*>(MemAlloc(static_cast<unsigned int>(count / 4 * 6 * sizeof(unsigned short))));
    for (std::size_t q = 0; q < count / 4; ++q) {
        const auto base = static_cast<unsigned short>(4 * q);
        unsigned short* out = mesh.indices + 6 * q;
        out[0] = base;
        out[1] = static_cast<unsigned short>(base + 1);
        out[2] = static_cast<unsigned short>(base + 2);
        out[3] = base;
        out[4] = static_cast<unsigned short>(base + 2);
        out[5] = static_cast<unsigned short>(base + 3);
    }
    UploadMesh(&mesh, false);
    MemFree(mesh.vertices);
    MemFree(mesh.normals);
    MemFree(mesh.texcoords);
    MemFree(mesh.colors);
    mesh.vertices = nullptr;
    mesh.normals = nullptr;
    mesh.texcoords = nullptr;
    mesh.colors = nullptr;
    return mesh;
}

} // namespace

Buildings3D::~Buildings3D() {
    for (Chunk& chunk : chunks_) {
        unload(chunk);
    }
    if (materials_) {
        // UnloadMaterial выгрузил бы и шейдер, а он не наш.
        MemFree(material_.maps);
        MemFree(caster_.maps);
    }
}

void Buildings3D::build(const World& world, const Terrain3D& terrain) {
    if (built_) {
        return;
    }
    const Map& map = world.map();
    chunksX_ = std::max(1, static_cast<int>(std::ceil(map.worldWidth() / kChunkMeters)));
    chunksZ_ = std::max(1, static_cast<int>(std::ceil(map.worldHeight() / kChunkMeters)));
    chunks_.resize(static_cast<std::size_t>(chunksX_) * static_cast<std::size_t>(chunksZ_));
    for (Chunk& chunk : chunks_) {
        chunk.min = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                     std::numeric_limits<float>::max()};
        chunk.max = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                     std::numeric_limits<float>::lowest()};
    }

    const entt::registry& registry = world.registry();
    for (const auto [entity, building, id] : registry.view<const Building, const Id>().each()) {
        Entry entry;
        entry.id = id.value;
        entry.model = buildingModelOf(building, id.value, terrain);
        entry.bounds = {{entry.model.x0, entry.model.low, entry.model.z0},
                        {entry.model.x1, buildingTop(entry.model), entry.model.z1}};
        const float cx = 0.5f * (entry.model.x0 + entry.model.x1);
        const float cz = 0.5f * (entry.model.z0 + entry.model.z1);
        entry.chunk = chunkIndex(cz, chunksZ_) * chunksX_ + chunkIndex(cx, chunksX_);
        Chunk& chunk = chunks_[static_cast<std::size_t>(entry.chunk)];
        chunk.entries.push_back(static_cast<int>(entries_.size()));
        // Коробка чанка — по коробкам зданий (с фундаментом и запасом на трубу).
        chunk.min = Vector3Min(chunk.min, Vector3Subtract(entry.bounds.min, {0.2f, 1.0f, 0.2f}));
        chunk.max = Vector3Max(chunk.max, Vector3Add(entry.bounds.max, {0.2f, 1.0f, 0.2f}));
        bounds_.push_back(entry.bounds);
        entries_.push_back(entry);
    }

    material_ = LoadMaterialDefault();
    caster_ = LoadMaterialDefault();
    materials_ = true;
    built_ = true;
    // Первый кадр — сразу все сетки: иначе города проявлялись бы кусками.
    for (Chunk& chunk : chunks_) {
        if (!chunk.entries.empty()) {
            rebuild(chunk);
        }
    }
}

void Buildings3D::update(float snow, EntityId open) {
    rebuilds_ = 0;
    snowStep_ = static_cast<int>(std::lround(std::clamp(snow, 0.0f, 1.0f) * kSnowSteps));
    if (open == open_) {
        return;
    }
    // Открытое здание сменилось: его чанк и чанк прежнего — сразу.
    const EntityId before = open_;
    open_ = open;
    for (const Entry& entry : entries_) {
        if (entry.id == before || entry.id == open) {
            rebuild(chunks_[static_cast<std::size_t>(entry.chunk)]);
        }
    }
}

void Buildings3D::unload(Chunk& chunk) {
    for (Mesh& mesh : chunk.meshes) {
        UnloadMesh(mesh);
    }
    chunk.meshes.clear();
}

void Buildings3D::rebuild(Chunk& chunk) {
    const auto start = std::chrono::steady_clock::now();
    unload(chunk);
    scratch_.clear();
    const float snow = static_cast<float>(snowStep_) / static_cast<float>(kSnowSteps);
    beginModelCapture(scratch_);
    for (const int i : chunk.entries) {
        const Entry& entry = entries_[static_cast<std::size_t>(i)];
        if (entry.id == open_) {
            continue;
        }
        BuildingModel model = entry.model;
        model.snow = snow;
        emitBuildingShell(model);
    }
    endModelCapture();
    const std::size_t total = scratch_.vertexCount();
    for (std::size_t first = 0; first < total; first += kMaxMeshVertices) {
        chunk.meshes.push_back(uploadQuads(scratch_, first, std::min(kMaxMeshVertices, total - first)));
    }
    chunk.snowStep = snowStep_;
    chunk.without = open_;
    ++meshesBuilt_;
    buildSeconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void Buildings3D::refresh(Chunk& chunk) {
    if (!stale(chunk)) {
        return;
    }
    if (chunk.without != open_ || rebuilds_ < kRebuildsPerFrame) {
        rebuild(chunk);
        ++rebuilds_;
    }
}

int Buildings3D::drawShells(const SceneShading& shading) {
    // Внутри BeginMode3D модельно-видовая матрица — это вид камеры.
    const Matrix viewProjection = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    material_.shader = shading.modelShader();
    chunksDrawn_ = 0;
    int buildings = 0;
    for (Chunk& chunk : chunks_) {
        if (chunk.entries.empty() || outsideFrustum(viewProjection, chunk.min, chunk.max)) {
            continue;
        }
        refresh(chunk);
        for (const Mesh& mesh : chunk.meshes) {
            DrawMesh(mesh, material_, MatrixIdentity());
        }
        ++chunksDrawn_;
        buildings += static_cast<int>(chunk.entries.size());
    }
    return buildings;
}

void Buildings3D::drawShadows(const SceneShading& shading) {
    for (Chunk& chunk : chunks_) {
        if (chunk.entries.empty()) {
            continue;
        }
        const Vector3 center = Vector3Scale(Vector3Add(chunk.min, chunk.max), 0.5f);
        if (!shading.inShadowArea(center, 0.5f * std::max(chunk.max.x - chunk.min.x, chunk.max.z - chunk.min.z))) {
            continue;
        }
        refresh(chunk);
        for (const Mesh& mesh : chunk.meshes) {
            DrawMesh(mesh, caster_, MatrixIdentity());
        }
    }
}

} // namespace lw
