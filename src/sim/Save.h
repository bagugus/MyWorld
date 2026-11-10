#pragma once

// Сохранение и загрузка мира (этап С.1). Без графики: работает и без окна.
//
// Снимок состояния, а не повтор ввода. Карта и участки (поселения, здания)
// строятся заново из seed; в файл идёт всё, что меняется: сущности с
// компонентами по общему списку sim/Schema.h, потоки рандома, летопись,
// погода, звуки, ресурсы, номер тика и следующий EntityId. Производные данные
// (пространственный индекс, навигация, карта активности LOD, сюжетные линии и
// герои интерфейса) пересчитываются после загрузки.
//
// Реестр возвращается точно: те же entt-идентификаторы с версиями, тот же
// список свободных и тот же порядок в каждом хранилище — системы обходят
// реестр в порядке хранения, а смерть и эвакуация его перемешивают. Поэтому
// мир после загрузки идёт дальше побитово так же, как без перерыва.
//
// Формат — свой, двоичный, little-endian: заголовок (магическое число,
// версия, отпечаток схемы), сведения для меню (сжаты отдельно — их можно
// прочесть, не трогая мир), состояние блоками по компонентам (сжато zstd).
// Файл другой версии или с другой схемой не читается — без миграций.
// Подробно — docs/architecture.md, «Сохранение и загрузка».

#include "sim/Components.h"
#include "sim/World.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lw {

// Версия формата. Состав компонентов и вид полей сверяются сами (отпечаток
// схемы, saveSchemaFingerprint); версию поднимать, когда меняется смысл
// полей или устройство файла.
// 2 — точка взгляда камеры в сведениях (этап С.2).
inline constexpr std::uint32_t kSaveFormatVersion = 2;

// Миниатюра для меню (С.2, С.3): RGB по строкам сверху вниз.
struct SaveThumbnail {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb; // width × height × 3
};

// Откуда смотрел наблюдатель, когда сохранял (этап С.2): окно после загрузки
// ставит камеру туда же. Не часть мира и в хеш не входит; смысл чисел знает
// только интерфейс (render/CameraController).
struct SaveView {
    bool valid = false;
    std::uint8_t mode = 0; // 0 — сверху, 1 — 3D
    float x = 0.0f;        // точка в центре кадра, метры
    float y = 0.0f;
    float zoom = 1.0f;     // пикселей на метр
    float yaw = 0.0f;      // курс и наклон 3D, градусы
    float pitch = 0.0f;
};

// Сведения о сохранении — для меню, без загрузки мира.
struct SaveInfo {
    std::uint32_t version = kSaveFormatVersion;
    std::uint64_t seed = 0;
    WorldConfig config;
    std::uint64_t tick = 0; // дата и время — calendarAt(tick)
    // Фаза сценария; у мира без сценария (без мирной жизни) её нет.
    bool hasScenario = false;
    WorldPhase phase = WorldPhase::Survival;
    std::int32_t people = 0;  // живых людей в мире (эвакуированные не в счёт)
    std::int32_t zombies = 0; // зомби на ногах
    std::int64_t savedAt = 0; // когда записано, секунды Unix — не часть мира
    std::string title;        // подпись слота (С.2)
    SaveThumbnail thumbnail;
    SaveView view;
    // Сверка при загрузке: карта и участки этой сборки из того же seed и
    // хеш мира после загрузки.
    std::uint64_t mapHash = 0;
    std::uint64_t sitesHash = 0;
    std::uint64_t stateHash = 0;
};

// Снимок мира в памяти: состояние, ещё не сжатое. Подпись и миниатюру можно
// дописать в info до encodeSave.
struct SaveSnapshot {
    SaveInfo info;
    std::vector<std::uint8_t> payload;
};

// Снять состояние мира — на границе тика (между World::step), в том же
// потоке, что двигает мир. nullopt — мир сохранить нельзя (причина в error):
// в реестре компонент не из списка sim/Schema.h.
std::optional<SaveSnapshot> captureSave(const World& world, std::string* error = nullptr);
// Сжать снимок в байты файла. Мира не касается — можно в фоновом потоке.
// Пусто — не вышло (error).
std::vector<std::uint8_t> encodeSave(const SaveSnapshot& snapshot, std::string* error = nullptr);
// Записать байты файла: сначала во временный файл рядом, потом подменить им
// прежний — оборванная запись не портит прежнее сохранение.
bool writeSaveFile(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                   std::string* error = nullptr);
// Всё сразу: снять, сжать, записать.
bool saveWorld(const World& world, const std::filesystem::path& path, std::string* error = nullptr);

// Сведения о сохранении из файла или из байтов — без загрузки мира.
std::optional<SaveInfo> readSaveInfo(const std::filesystem::path& path, std::string* error = nullptr);
std::optional<SaveInfo> decodeSaveInfo(std::span<const std::uint8_t> bytes, std::string* error = nullptr);
// Загрузить мир: карта заново из seed, состояние — из файла. nullptr — не
// вышло (error): не сохранение, другая версия или схема, файл повреждён,
// карта этой сборки из того же seed другая.
std::unique_ptr<World> loadWorld(const std::filesystem::path& path, std::string* error = nullptr);
std::unique_ptr<World> decodeSave(std::span<const std::uint8_t> bytes, std::string* error = nullptr);

// Отпечаток схемы этой сборки: состав и порядок компонентов, вид их полей,
// число потоков рандома, настройки мира.
std::uint64_t saveSchemaFingerprint();

} // namespace lw
