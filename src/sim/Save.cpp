#include "sim/Save.h"

#include "sim/Chronicle.h"
#include "sim/Hash.h"
#include "sim/Profiler.h"
#include "sim/Scenario.h"
#include "sim/Schema.h"
#include "sim/systems/LodSystem.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace lw {

// --- Поля вне схемы мира: только для файла -----------------------------------

template <typename Ar> void visit(Ar& ar, Id& c) { io(ar, c.value); }

template <typename Ar>
void visit(Ar& ar, WorldConfig& c) {
    io(ar, c.mapWidth);
    io(ar, c.mapHeight);
    io(ar, c.people);
    io(ar, c.zombies);
    io(ar, c.animals);
    io(ar, c.flocks);
    io(ar, c.events);
    io(ar, c.phaseEvents);
    io(ar, c.peacetime);
    io(ar, c.outbreakDays);
    io(ar, c.lod);
}

template <typename Ar>
void visit(Ar& ar, SaveThumbnail& c) {
    io(ar, c.width);
    io(ar, c.height);
    ioVector(ar, c.rgb);
}

template <typename Ar>
void visit(Ar& ar, SaveView& c) {
    io(ar, c.valid);
    io(ar, c.mode);
    io(ar, c.x);
    io(ar, c.y);
    io(ar, c.zoom);
    io(ar, c.yaw);
    io(ar, c.pitch);
}

template <typename Ar>
void visit(Ar& ar, SaveInfo& c) {
    io(ar, c.seed);
    io(ar, c.config);
    io(ar, c.tick);
    io(ar, c.hasScenario);
    io(ar, c.phase);
    io(ar, c.people);
    io(ar, c.zombies);
    io(ar, c.savedAt);
    ar.text(c.title);
    io(ar, c.thumbnail);
    io(ar, c.view);
    io(ar, c.mapHash);
    io(ar, c.sitesHash);
    io(ar, c.stateHash);
}

namespace {

// «LWWORLD» и Ctrl+Z: не текст, сразу видно, что это сохранение мира.
constexpr std::array<std::uint8_t, 8> kMagic = {'L', 'W', 'W', 'O', 'R', 'L', 'D', 0x1A};
// Заголовок до сжатых сведений: магия, версия, отпечаток, два размера сведений.
constexpr std::size_t kPrefixSize = kMagic.size() + 4 + 8 + 4 + 4;
// Уровень zstd: быстро и вдвое-втрое плотнее deflate по скорости того же порядка.
constexpr int kCompressionLevel = 3;
// Пределы против повреждённого файла: таких размеров не бывает.
constexpr std::uint64_t kMaxPayloadBytes = std::uint64_t{4} << 30;
constexpr std::uint32_t kMaxInfoBytes = 64u << 20;

void setError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

// --- Запись ------------------------------------------------------------------

class WriteArchive {
public:
    static constexpr bool kHashing = false;
    static constexpr bool kReading = false;
    static constexpr bool kSchema = false;

    explicit WriteArchive(std::vector<std::uint8_t>& out) : out_(out) {}

    template <typename T>
    void value(const T& value) {
        if constexpr (std::is_enum_v<T>) {
            put(static_cast<std::underlying_type_t<T>>(value));
        } else if constexpr (std::is_same_v<T, bool>) {
            put(static_cast<std::uint8_t>(value ? 1 : 0));
        } else if constexpr (std::is_same_v<T, float>) {
            put(std::bit_cast<std::uint32_t>(value));
        } else if constexpr (std::is_same_v<T, double>) {
            put(std::bit_cast<std::uint64_t>(value));
        } else {
            put(value);
        }
    }
    template <typename T>
    void unhashed(const T& value) {
        this->value(value);
    }
    void text(const std::string& text) {
        put(static_cast<std::uint32_t>(text.size()));
        bytes(text.data(), text.size());
    }
    void length(std::uint64_t length) { put(static_cast<std::uint32_t>(length)); }
    bool lod() const { return true; }

    template <std::integral T>
    void put(T value) {
        using U = std::make_unsigned_t<T>;
        const auto bits = static_cast<U>(value);
        for (unsigned i = 0; i < sizeof(T); ++i) {
            out_.push_back(static_cast<std::uint8_t>(bits >> (8u * i)));
        }
    }
    void bytes(const void* data, std::size_t size) {
        const auto* first = static_cast<const std::uint8_t*>(data);
        out_.insert(out_.end(), first, first + size);
    }
    // Блок: имя, длина (вписывается в endBlock), данные.
    void beginBlock(std::string_view name) {
        put(static_cast<std::uint32_t>(name.size()));
        bytes(name.data(), name.size());
        blockStart_ = out_.size();
        put(std::uint64_t{0});
    }
    void endBlock() {
        const std::uint64_t size = out_.size() - blockStart_ - sizeof(std::uint64_t);
        for (unsigned i = 0; i < sizeof(size); ++i) {
            out_[blockStart_ + i] = static_cast<std::uint8_t>(size >> (8u * i));
        }
    }

private:
    std::vector<std::uint8_t>& out_;
    std::size_t blockStart_ = 0;
};

// --- Чтение ------------------------------------------------------------------

// Чтение байтов с проверкой границ. Первая ошибка запоминается, дальше все
// чтения дают нули — проверять failed() можно редко.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data), end_(data.size()) {}

    bool failed() const { return failed_; }
    const std::string& error() const { return error_; }
    void fail(std::string message) {
        if (!failed_) {
            failed_ = true;
            error_ = block_.empty() ? std::move(message) : "блок «" + block_ + "»: " + message;
        }
        pos_ = end_;
    }
    std::size_t remaining() const { return end_ - pos_; }

    bool take(void* out, std::size_t size) {
        if (failed_ || size > remaining()) {
            fail("файл обрывается");
            std::memset(out, 0, size);
            return false;
        }
        std::memcpy(out, data_.data() + pos_, size);
        pos_ += size;
        return true;
    }
    template <std::integral T>
    T get() {
        std::uint8_t raw[sizeof(T)];
        if (!take(raw, sizeof(T))) {
            return T{};
        }
        using U = std::make_unsigned_t<T>;
        U bits = 0;
        for (unsigned i = 0; i < sizeof(T); ++i) {
            bits = static_cast<U>(bits | static_cast<U>(static_cast<U>(raw[i]) << (8u * i)));
        }
        return static_cast<T>(bits);
    }
    std::span<const std::uint8_t> span(std::size_t size) {
        if (failed_ || size > remaining()) {
            fail("файл обрывается");
            return {};
        }
        const std::span<const std::uint8_t> result = data_.subspan(pos_, size);
        pos_ += size;
        return result;
    }

    // Блок с ожидаемым именем; читать — до endBlock, ровно до его конца.
    void beginBlock(std::string_view name) {
        const auto length = get<std::uint32_t>();
        std::string found(std::min<std::size_t>(length, remaining()), '\0');
        take(found.data(), found.size());
        const auto size = get<std::uint64_t>();
        if (failed_) {
            return;
        }
        if (found != name) {
            fail("ждали блок «" + std::string(name) + "», а там «" + found + "»");
            return;
        }
        if (size > remaining()) {
            fail("блок «" + found + "» обрывается");
            return;
        }
        block_ = found;
        outerEnd_ = end_;
        end_ = pos_ + static_cast<std::size_t>(size);
    }
    void endBlock() {
        if (!failed_ && pos_ != end_) {
            fail("лишние данные в конце блока");
        }
        if (failed_) {
            return;
        }
        end_ = outerEnd_;
        block_.clear();
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
    std::size_t end_ = 0;
    std::size_t outerEnd_ = 0;
    std::string block_;
    bool failed_ = false;
    std::string error_;
};

class ReadArchive {
public:
    static constexpr bool kHashing = false;
    static constexpr bool kReading = true;
    static constexpr bool kSchema = false;

    explicit ReadArchive(Reader& reader) : reader_(reader) {}

    template <typename T>
    void value(T& value) {
        if constexpr (std::is_enum_v<T>) {
            value = static_cast<T>(reader_.get<std::underlying_type_t<T>>());
        } else if constexpr (std::is_same_v<T, bool>) {
            const auto raw = reader_.get<std::uint8_t>();
            if (raw > 1) {
                reader_.fail("повреждённые данные: логическое значение не 0 и не 1");
            }
            value = raw != 0;
        } else if constexpr (std::is_same_v<T, float>) {
            value = std::bit_cast<float>(reader_.get<std::uint32_t>());
        } else if constexpr (std::is_same_v<T, double>) {
            value = std::bit_cast<double>(reader_.get<std::uint64_t>());
        } else {
            value = reader_.get<T>();
        }
    }
    template <typename T>
    void unhashed(T& value) {
        this->value(value);
    }
    void text(std::string& text) {
        const auto size = reader_.get<std::uint32_t>();
        if (size > reader_.remaining()) {
            reader_.fail("строка длиннее файла");
            return;
        }
        text.resize(size);
        reader_.take(text.data(), size);
    }
    void length(std::uint64_t& length) { length = reader_.get<std::uint32_t>(); }
    // Хватит ли файла на length элементов (каждый — хотя бы байт).
    bool reserve(std::uint64_t length, std::size_t /*elementSize*/) {
        if (length > reader_.remaining()) {
            reader_.fail("список длиннее файла");
            return false;
        }
        return true;
    }
    void fail(const char* message) { reader_.fail(std::string("повреждённые данные: ") + message); }
    bool lod() const { return true; }

private:
    Reader& reader_;
};

// --- Отпечаток схемы -----------------------------------------------------------

// Не значения, а вид полей: роль (в хеше или только в файле), тип, размер,
// вместимость массивов. Другой отпечаток — другое устройство файла.
class SchemaArchive {
public:
    static constexpr bool kHashing = false;
    static constexpr bool kReading = false;
    static constexpr bool kSchema = true;

    explicit SchemaArchive(Hasher& hasher) : hasher_(hasher) {}

    template <typename T>
    void value(const T&) {
        code<T>('v');
    }
    template <typename T>
    void unhashed(const T&) {
        code<T>('d');
    }
    void text(const std::string&) { hasher_.add(static_cast<std::uint8_t>('s')); }
    void length(std::uint64_t) {}
    void marker(std::uint32_t marker) { hasher_.add(marker); }
    bool lod() const { return true; }

private:
    template <typename T>
    void code(char role) {
        char kind = 'u';
        if constexpr (std::is_enum_v<T>) {
            kind = 'e';
        } else if constexpr (std::is_same_v<T, bool>) {
            kind = 'b';
        } else if constexpr (std::is_floating_point_v<T>) {
            kind = 'f';
        } else if constexpr (std::is_signed_v<T>) {
            kind = 'i';
        }
        hasher_.add(static_cast<std::uint8_t>(role));
        hasher_.add(static_cast<std::uint8_t>(kind));
        hasher_.add(static_cast<std::uint8_t>(sizeof(T)));
    }

    Hasher& hasher_;
};

template <typename T>
void addSchemaOf(Hasher& hasher, std::string_view name) {
    for (const char c : name) {
        hasher.add(static_cast<std::uint8_t>(c));
    }
    hasher.add(std::uint8_t{0});
    SchemaArchive archive(hasher);
    T sample{};
    io(archive, sample);
}

// --- Сжатие ------------------------------------------------------------------

bool compress(std::span<const std::uint8_t> raw, std::vector<std::uint8_t>& packed, std::string* error) {
    packed.resize(ZSTD_compressBound(raw.size()));
    ZSTD_CCtx* context = ZSTD_createCCtx();
    if (context == nullptr) {
        setError(error, "zstd: не создать контекст сжатия");
        return false;
    }
    ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, kCompressionLevel);
    // Контрольная сумма кадра: повреждённый файл не прочтётся.
    ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 1);
    const std::size_t size = ZSTD_compress2(context, packed.data(), packed.size(), raw.data(), raw.size());
    ZSTD_freeCCtx(context);
    if (ZSTD_isError(size)) {
        setError(error, std::string("zstd: ") + ZSTD_getErrorName(size));
        return false;
    }
    packed.resize(size);
    return true;
}

bool decompress(std::span<const std::uint8_t> packed, std::uint64_t rawSize, std::vector<std::uint8_t>& raw,
                std::string* error) {
    const unsigned long long content = ZSTD_getFrameContentSize(packed.data(), packed.size());
    if (content == ZSTD_CONTENTSIZE_ERROR || content == ZSTD_CONTENTSIZE_UNKNOWN || content != rawSize) {
        setError(error, "файл повреждён: сжатые данные не того размера");
        return false;
    }
    raw.resize(static_cast<std::size_t>(rawSize));
    const std::size_t size = ZSTD_decompress(raw.data(), raw.size(), packed.data(), packed.size());
    if (ZSTD_isError(size) || size != rawSize) {
        setError(error, std::string("файл повреждён: ") +
                            (ZSTD_isError(size) ? ZSTD_getErrorName(size) : "не тот размер"));
        return false;
    }
    return true;
}

// --- Заголовок -----------------------------------------------------------------

// Разобрать заголовок и сведения. payload — сжатое состояние и его размер без
// сжатия, если файл прочитан целиком (иначе пусто).
struct Parsed {
    SaveInfo info;
    std::span<const std::uint8_t> payload;
    std::uint64_t payloadRaw = 0;
    bool hasPayload = false;
};

bool parseHeader(std::span<const std::uint8_t> bytes, Parsed& parsed, bool needPayload, std::string* error) {
    if (bytes.size() < kMagic.size() || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        setError(error, "это не сохранение мира");
        return false;
    }
    Reader reader(bytes.subspan(kMagic.size()));
    const auto version = reader.get<std::uint32_t>();
    const auto fingerprint = reader.get<std::uint64_t>();
    const auto infoRaw = reader.get<std::uint32_t>();
    const auto infoPacked = reader.get<std::uint32_t>();
    if (reader.failed()) {
        setError(error, "файл сохранения обрывается");
        return false;
    }
    if (version != kSaveFormatVersion) {
        setError(error, "сохранение формата " + std::to_string(version) + ", эта сборка читает формат " +
                            std::to_string(kSaveFormatVersion));
        return false;
    }
    if (fingerprint != saveSchemaFingerprint()) {
        setError(error, "сохранение сделано другой сборкой: состав состояния мира изменился");
        return false;
    }
    if (infoRaw > kMaxInfoBytes || infoPacked > kMaxInfoBytes) {
        setError(error, "файл повреждён: сведения о сохранении слишком велики");
        return false;
    }
    const std::span<const std::uint8_t> packedInfo = reader.span(infoPacked);
    if (reader.failed()) {
        setError(error, "файл сохранения обрывается");
        return false;
    }
    std::vector<std::uint8_t> rawInfo;
    if (!decompress(packedInfo, infoRaw, rawInfo, error)) {
        return false;
    }
    Reader infoReader(rawInfo);
    ReadArchive archive(infoReader);
    parsed.info = SaveInfo{};
    io(archive, parsed.info);
    parsed.info.version = version;
    if (!infoReader.failed() && infoReader.remaining() != 0) {
        infoReader.fail("лишние данные в сведениях");
    }
    const SaveThumbnail& thumbnail = parsed.info.thumbnail;
    if (!infoReader.failed() &&
        (thumbnail.width < 0 || thumbnail.height < 0 ||
         thumbnail.rgb.size() != static_cast<std::size_t>(thumbnail.width) * static_cast<std::size_t>(thumbnail.height) * 3)) {
        infoReader.fail("миниатюра не того размера");
    }
    if (infoReader.failed()) {
        setError(error, "сведения о сохранении: " + infoReader.error());
        return false;
    }
    if (!needPayload) {
        return true;
    }
    parsed.payloadRaw = reader.get<std::uint64_t>();
    const auto payloadPacked = reader.get<std::uint64_t>();
    if (reader.failed() || parsed.payloadRaw > kMaxPayloadBytes || payloadPacked > reader.remaining()) {
        setError(error, "файл сохранения обрывается");
        return false;
    }
    parsed.payload = reader.span(static_cast<std::size_t>(payloadPacked));
    if (reader.remaining() != 0) {
        setError(error, "файл повреждён: лишние данные в конце");
        return false;
    }
    parsed.hasPayload = true;
    return true;
}

bool readFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes, std::size_t limit,
              std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        setError(error, "не открыть файл " + path.string());
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size < 0) {
        setError(error, "не прочитать файл " + path.string());
        return false;
    }
    bytes.resize(std::min<std::size_t>(static_cast<std::size_t>(size), limit));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        setError(error, "не прочитать файл " + path.string());
        return false;
    }
    return true;
}

} // namespace

// --- Сохранение и загрузка: доступ к закрытому состоянию мира -----------------

class SaveCodec {
public:
    static std::optional<SaveSnapshot> capture(const World& world, std::string* error);
    static std::unique_ptr<World> restore(const SaveInfo& info, std::span<const std::uint8_t> payload,
                                          std::string* error);

private:
    template <typename T>
    static void writeStorage(WriteArchive& archive, const entt::registry& registry, std::string_view name);
    template <typename T>
    static void readStorage(Reader& reader, ReadArchive& archive, entt::registry& registry, std::string_view name);
};

template <typename T>
void SaveCodec::writeStorage(WriteArchive& archive, const entt::registry& registry, std::string_view name) {
    archive.beginBlock(name);
    const auto* storage = registry.storage<T>();
    const std::size_t count = storage != nullptr ? storage->size() : 0;
    archive.put(static_cast<std::uint32_t>(count));
    // Порядок хранения: packed[0], packed[1], … — загрузка ставит их в том же
    // порядке, и обход реестра (а с ним мир) идёт так же.
    for (std::size_t i = 0; i < count; ++i) {
        const entt::entity entity = storage->data()[i];
        archive.put(entt::to_integral(entity));
        io(archive, const_cast<T&>(storage->get(entity)));
    }
    archive.endBlock();
}

template <typename T>
void SaveCodec::readStorage(Reader& reader, ReadArchive& archive, entt::registry& registry,
                            std::string_view name) {
    reader.beginBlock(name);
    const auto count = reader.get<std::uint32_t>();
    auto& storage = registry.storage<T>();
    if (count > registry.storage<entt::entity>().free_list()) {
        reader.fail("компонентов больше, чем сущностей");
    } else {
        storage.reserve(count);
    }
    for (std::uint32_t i = 0; i < count && !reader.failed(); ++i) {
        const auto entity = static_cast<entt::entity>(reader.get<std::uint32_t>());
        if (!reader.failed() && !registry.valid(entity)) {
            reader.fail("компонент у сущности, которой нет");
            break;
        }
        if (storage.contains(entity)) {
            reader.fail("компонент у сущности дважды");
            break;
        }
        T value{};
        io(archive, value);
        if (reader.failed()) {
            break;
        }
        // Сигналы реестра не подключены (attachLod подключается после загрузки).
        storage.emplace(entity, std::move(value));
    }
    reader.endBlock();
}

std::optional<SaveSnapshot> SaveCodec::capture(const World& world, std::string* error) {
    const entt::registry& registry = world.registry_;

    // Каждое непустое хранилище реестра должно быть в схеме: компонент не из
    // списка потерялся бы при загрузке молча.
    std::unordered_set<entt::id_type> known{entt::type_hash<Id>::value(), entt::type_hash<entt::entity>::value()};
    forEachType(SavedComponents{}, [&](auto type) {
        using T = typename decltype(type)::type;
        known.insert(entt::type_hash<T>::value());
    });
    for (const auto [id, storage] : registry.storage()) {
        if (!storage.empty() && !known.contains(id)) {
            setError(error, "компонент " + std::string(storage.info().name()) +
                                " не входит в схему сохранения (sim/Schema.h, SavedComponents)");
            return std::nullopt;
        }
    }

    SaveSnapshot snapshot;
    SaveInfo& info = snapshot.info;
    info.seed = world.seed_;
    info.config = world.config_;
    info.tick = world.tick_;
    if (const Scenario* scenario = scenarioOf(world)) {
        info.hasScenario = true;
        info.phase = scenario->phase;
    }
    const auto* people = registry.storage<Person>();
    const auto* zombies = registry.storage<Zombie>();
    info.people = people != nullptr ? static_cast<std::int32_t>(people->size()) : 0;
    info.zombies = zombies != nullptr ? static_cast<std::int32_t>(zombies->size()) : 0;
    info.savedAt = static_cast<std::int64_t>(std::time(nullptr));
    info.mapHash = world.mapHash_;
    info.sitesHash = world.sitesHash();
    info.stateHash = world.stateHash();

    std::vector<std::uint8_t>& out = snapshot.payload;
    out.reserve(std::size_t{1} << 20);
    WriteArchive archive(out);

    archive.beginBlock("world");
    archive.put(world.tick_);
    archive.put(world.nextId_);
    archive.put(static_cast<std::uint32_t>(world.streams_.size()));
    for (const Rng& stream : world.streams_) {
        archive.put(stream.seed());
        archive.put(stream.state());
        archive.put(stream.increment());
    }
    archive.endBlock();

    archive.beginBlock("weather");
    io(archive, const_cast<Weather&>(world.weather_));
    archive.endBlock();

    archive.beginBlock("sounds");
    archive.put(static_cast<std::uint32_t>(world.sounds_.size()));
    for (const Sound& sound : world.sounds_) {
        io(archive, const_cast<Sound&>(sound));
    }
    archive.endBlock();

    archive.beginBlock("chronicle");
    const std::vector<ChronicleEvent>& events = world.chronicle_.events();
    archive.put(static_cast<std::uint32_t>(events.size()));
    for (const ChronicleEvent& event : events) {
        io(archive, const_cast<ChronicleEvent&>(event));
    }
    archive.endBlock();

    archive.beginBlock("resources");
    const std::vector<std::uint8_t>& forest = world.resources_.forest().cells();
    archive.put(static_cast<std::uint32_t>(forest.size()));
    archive.bytes(forest.data(), forest.size());
    archive.value(world.resources_.rainAccumulated());
    archive.put(world.resources_.rainPortions());
    archive.endBlock();

    // Сущности — как лежат в хранилище: живые, затем удалённые (их номера
    // вернутся с новой версией). Так и новые сущности после загрузки получат
    // те же entt-идентификаторы, что без перерыва.
    archive.beginBlock("entities");
    const auto* entities = registry.storage<entt::entity>();
    archive.put(static_cast<std::uint32_t>(entities->size()));
    archive.put(static_cast<std::uint32_t>(entities->free_list()));
    for (std::size_t i = 0; i < entities->size(); ++i) {
        archive.put(entt::to_integral(entities->data()[i]));
    }
    archive.endBlock();

    writeStorage<Id>(archive, registry, "Id");
    forEachType(SavedComponents{}, [&](auto type) {
        using T = typename decltype(type)::type;
        writeStorage<T>(archive, registry, ComponentInfo<T>::kName);
    });
    return snapshot;
}

std::unique_ptr<World> SaveCodec::restore(const SaveInfo& info, std::span<const std::uint8_t> payload,
                                          std::string* error) {
    // Карта и участки — заново из seed, как при создании мира.
    std::unique_ptr<World> world(new World(info.seed, info.config, World::Blank{}));
    World& w = *world;
    {
        const StartupScope total(w.profiler_, "world");
        w.generate();
    }
    if (w.mapHash_ != info.mapHash || w.sitesHash() != info.sitesHash) {
        setError(error, "карта этой сборки из seed " + std::to_string(info.seed) +
                            " другая: сохранение сделано сборкой с другой генерацией мира");
        return nullptr;
    }

    const StartupScope step(w.profiler_, "world/restore");
    entt::registry& registry = w.registry_;
    // Пустой реестр: и компоненты, и сами сущности (с версиями и списком
    // свободных) — всё вернётся из файла.
    registry.clear();
    registry.storage<entt::entity>().clear();
    w.idToEntity_.clear();

    Reader reader(payload);
    ReadArchive archive(reader);

    reader.beginBlock("world");
    const auto tick = reader.get<std::uint64_t>();
    const auto nextId = reader.get<std::uint64_t>();
    const auto streams = reader.get<std::uint32_t>();
    if (!reader.failed() && streams != w.streams_.size()) {
        reader.fail("не то число потоков рандома");
    }
    for (std::size_t i = 0; i < w.streams_.size() && !reader.failed(); ++i) {
        const auto seed = reader.get<std::uint64_t>();
        const auto state = reader.get<std::uint64_t>();
        const auto increment = reader.get<std::uint64_t>();
        w.streams_[i] = Rng::restored(seed, state, increment);
    }
    reader.endBlock();
    w.tick_ = tick;
    w.nextId_ = nextId;

    reader.beginBlock("weather");
    io(archive, w.weather_);
    reader.endBlock();

    reader.beginBlock("sounds");
    const auto soundCount = reader.get<std::uint32_t>();
    if (soundCount > reader.remaining()) {
        reader.fail("звуков больше, чем данных");
    } else {
        w.sounds_.assign(soundCount, Sound{});
        for (Sound& sound : w.sounds_) {
            io(archive, sound);
        }
    }
    reader.endBlock();

    reader.beginBlock("chronicle");
    const auto eventCount = reader.get<std::uint32_t>();
    w.chronicle_ = Chronicle{};
    if (eventCount > reader.remaining()) {
        reader.fail("записей больше, чем данных");
    }
    // Заново через add: индекс по id, счётчики и накопленный хеш — те же.
    for (std::uint32_t i = 0; i < eventCount && !reader.failed(); ++i) {
        ChronicleEvent event;
        io(archive, event);
        w.chronicle_.add(event);
    }
    reader.endBlock();

    reader.beginBlock("resources");
    const auto forestSize = reader.get<std::uint32_t>();
    const std::span<const std::uint8_t> forest = reader.span(forestSize);
    float rain = 0.0f;
    archive.value(rain);
    const auto rainPortions = reader.get<std::uint32_t>();
    if (!reader.failed() &&
        !w.resources_.restore(std::vector<std::uint8_t>(forest.begin(), forest.end()), rain, rainPortions)) {
        reader.fail("лес не того размера");
    }
    reader.endBlock();

    reader.beginBlock("entities");
    auto& entities = registry.storage<entt::entity>();
    const auto entityCount = reader.get<std::uint32_t>();
    const auto alive = reader.get<std::uint32_t>();
    if (!reader.failed() && (alive > entityCount || entityCount > reader.remaining() / sizeof(std::uint32_t))) {
        reader.fail("не то число сущностей");
    }
    std::uint32_t lastIndex = 0;
    if (!reader.failed()) {
        entities.reserve(entityCount);
        for (std::uint32_t i = 0; i < entityCount; ++i) {
            const auto entity = static_cast<entt::entity>(reader.get<std::uint32_t>());
            if (entity == entt::null || entity == entt::tombstone || entities.generate(entity) != entity) {
                reader.fail("сущность повторяется или повреждена");
                break;
            }
            lastIndex = std::max(lastIndex, static_cast<std::uint32_t>(entt::to_entity(entity)));
        }
    }
    if (!reader.failed()) {
        // Новые номера — после всех, что были; удалённые ждут в списке свободных.
        if (entityCount > 0) {
            entities.start_from(static_cast<entt::entity>(lastIndex + 1));
        }
        entities.free_list(alive);
    }
    reader.endBlock();

    readStorage<Id>(reader, archive, registry, "Id");
    forEachType(SavedComponents{}, [&](auto type) {
        using T = typename decltype(type)::type;
        if (!reader.failed()) {
            readStorage<T>(reader, archive, registry, ComponentInfo<T>::kName);
        }
    });
    if (!reader.failed() && reader.remaining() != 0) {
        reader.fail("лишние данные в конце");
    }

    // У каждой живой сущности — свой Id, уникальный и выданный раньше nextId.
    if (!reader.failed() && registry.storage<Id>().size() != entities.free_list()) {
        reader.fail("не у всех сущностей есть Id");
    }
    if (!reader.failed()) {
        w.idToEntity_.reserve(registry.storage<Id>().size());
        for (const auto [entity, id] : registry.view<const Id>().each()) {
            if (id.value == kInvalidId || id.value >= w.nextId_ || !w.idToEntity_.emplace(id.value, entity).second) {
                reader.fail("Id повторяется или выдан не по порядку");
                break;
            }
        }
    }
    if (reader.failed()) {
        setError(error, "файл повреждён: " + reader.error());
        return nullptr;
    }

    // Производные данные — заново по восстановленному миру.
    w.connectLod();
    w.spatial_.sync(registry);
    refreshActivityMap(w);

    if (w.stateHash() != info.stateHash) {
        setError(error, "мир после загрузки не совпал с сохранённым (хеш состояния другой)");
        return nullptr;
    }
    return world;
}

// --- Открытые функции ------------------------------------------------------------

std::uint64_t saveSchemaFingerprint() {
    static const std::uint64_t fingerprint = [] {
        Hasher hasher;
        hasher.add(static_cast<std::uint64_t>(RngStream::Count));
        hasher.add(static_cast<std::uint64_t>(sizeof(entt::entity)));
        addSchemaOf<Id>(hasher, "Id");
        forEachType(SavedComponents{}, [&](auto type) {
            using T = typename decltype(type)::type;
            addSchemaOf<T>(hasher, ComponentInfo<T>::kName);
        });
        addSchemaOf<Weather>(hasher, "Weather");
        addSchemaOf<Sound>(hasher, "Sound");
        addSchemaOf<ChronicleEvent>(hasher, "ChronicleEvent");
        addSchemaOf<SaveInfo>(hasher, "SaveInfo");
        return hasher.value();
    }();
    return fingerprint;
}

std::optional<SaveSnapshot> captureSave(const World& world, std::string* error) {
    return SaveCodec::capture(world, error);
}

std::vector<std::uint8_t> encodeSave(const SaveSnapshot& snapshot, std::string* error) {
    std::vector<std::uint8_t> rawInfo;
    WriteArchive infoArchive(rawInfo);
    io(infoArchive, const_cast<SaveInfo&>(snapshot.info));
    std::vector<std::uint8_t> packedInfo;
    std::vector<std::uint8_t> packedPayload;
    if (!compress(rawInfo, packedInfo, error) || !compress(snapshot.payload, packedPayload, error)) {
        return {};
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kPrefixSize + packedInfo.size() + 16 + packedPayload.size());
    WriteArchive out(bytes);
    out.bytes(kMagic.data(), kMagic.size());
    out.put(kSaveFormatVersion);
    out.put(saveSchemaFingerprint());
    out.put(static_cast<std::uint32_t>(rawInfo.size()));
    out.put(static_cast<std::uint32_t>(packedInfo.size()));
    out.bytes(packedInfo.data(), packedInfo.size());
    out.put(static_cast<std::uint64_t>(snapshot.payload.size()));
    out.put(static_cast<std::uint64_t>(packedPayload.size()));
    out.bytes(packedPayload.data(), packedPayload.size());
    return bytes;
}

bool writeSaveFile(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, std::string* error) {
    std::error_code code;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), code);
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            setError(error, "не создать файл " + temporary.string());
            return false;
        }
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.close();
        if (!file) {
            setError(error, "не записать файл " + temporary.string());
            std::filesystem::remove(temporary, code);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, code);
    if (code) {
        setError(error, "не заменить файл " + path.string() + ": " + code.message());
        std::filesystem::remove(temporary, code);
        return false;
    }
    return true;
}

bool saveWorld(const World& world, const std::filesystem::path& path, std::string* error) {
    const std::optional<SaveSnapshot> snapshot = captureSave(world, error);
    if (!snapshot) {
        return false;
    }
    const std::vector<std::uint8_t> bytes = encodeSave(*snapshot, error);
    return !bytes.empty() && writeSaveFile(path, bytes, error);
}

std::optional<SaveInfo> decodeSaveInfo(std::span<const std::uint8_t> bytes, std::string* error) {
    Parsed parsed;
    if (!parseHeader(bytes, parsed, false, error)) {
        return std::nullopt;
    }
    return parsed.info;
}

std::optional<SaveInfo> readSaveInfo(const std::filesystem::path& path, std::string* error) {
    // Сначала заголовок, потом ровно столько, сколько занимают сведения: само
    // состояние мира для меню не читается.
    std::vector<std::uint8_t> bytes;
    if (!readFile(path, bytes, kPrefixSize, error)) {
        return std::nullopt;
    }
    if (bytes.size() == kPrefixSize) {
        std::uint32_t infoPacked = 0;
        for (unsigned i = 0; i < 4; ++i) {
            infoPacked |= static_cast<std::uint32_t>(bytes[kPrefixSize - 4 + i]) << (8u * i);
        }
        if (infoPacked <= kMaxInfoBytes && !readFile(path, bytes, kPrefixSize + infoPacked, error)) {
            return std::nullopt;
        }
    }
    return decodeSaveInfo(bytes, error);
}

std::unique_ptr<World> decodeSave(std::span<const std::uint8_t> bytes, std::string* error) {
    Parsed parsed;
    if (!parseHeader(bytes, parsed, true, error)) {
        return nullptr;
    }
    std::vector<std::uint8_t> payload;
    if (!decompress(parsed.payload, parsed.payloadRaw, payload, error)) {
        return nullptr;
    }
    return SaveCodec::restore(parsed.info, payload, error);
}

std::unique_ptr<World> loadWorld(const std::filesystem::path& path, std::string* error) {
    std::vector<std::uint8_t> bytes;
    if (!readFile(path, bytes, static_cast<std::size_t>(kMaxPayloadBytes), error)) {
        return nullptr;
    }
    return decodeSave(bytes, error);
}

} // namespace lw
