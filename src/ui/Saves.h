#pragma once

// Сохранения в окне (этап С.2): слоты, F5 / F9, автосохранение, окно
// «Сохранения». Ядро — sim/Save.h; здесь — только когда и куда.
//
// Снимок мира (captureSave) — в главном потоке на границе тика, между
// отрисовкой мира и интерфейсом: тогда же с экрана снимается миниатюра.
// Сжатие и запись — в фоновом потоке, кадр ждёт только снимка. Загрузка тоже в
// фоне: карта строится из seed несколько секунд, окно всё это время рисуется,
// а мир стоит. Готовый мир забирает main.cpp (takeLoaded) и пересоздаёт всё,
// что от мира зависит.
//
// Слоты — файлы в папке saves рядом с camera.ini: быстрое (F5 / F9),
// шесть своих и три автосохранения по кругу (затирается самое старое).
// Автосохранение — раз в игровые сутки и перед сменой фазы сценария
// (SimDriver::setHoldBeforePhase).

#include "sim/Save.h"

#include <raylib.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace lw {

class CameraController;
class SimDriver;
class World;

namespace ui {

class Saves {
public:
    explicit Saves(std::filesystem::path folder = "saves");
    ~Saves(); // дожидается записи: начатое сохранение не обрывается
    Saves(const Saves&) = delete;
    Saves& operator=(const Saves&) = delete;

    // Автосохранение (по умолчанию включено; снимок с командной строки — нет).
    void setAutosave(bool enabled) { autosave_ = enabled; }
    bool autosave() const { return autosave_; }

    // Мир создан или загружен: сутки автосохранения считаются от него.
    void beginSession(const World& world);

    // F5 — быстрое сохранение, F9 — загрузить быстрое.
    void handleHotkeys(bool keyboardFree);
    // После SimDriver::advance: итоги фоновой записи, автосохранения (новые
    // сутки, мир встал перед сменой фазы), пока идёт загрузка — мир на паузе.
    void update(const World& world, SimDriver& driver);
    // Между отрисовкой мира и интерфейсом: заказано сохранение — снять
    // миниатюру с экрана и снимок мира. blank — кадр пустой (скачок времени в
    // сериале): без миниатюры.
    void capture(const World& world, const CameraController& camera, bool blank);

    bool loading() const { return loading_; }
    // Загруженный мир, если готов (и его сведения — откуда смотреть); иначе
    // nullptr. Пауза, которая была до загрузки, — в wasPaused.
    std::unique_ptr<World> takeLoaded(SaveInfo& info, bool& wasPaused);

    // Окно «Сохранения»: слоты с миниатюрой, датой и фазой, сохранить,
    // загрузить, удалить; флажок автосохранения.
    void drawPanel(const World& world);
    // Строка о последнем действии и «Загрузка…» поверх кадра.
    void drawStatus(bool series);

private:
    enum class SlotKind { Quick, Manual, Auto };
    struct Slot {
        SlotKind kind = SlotKind::Manual;
        int number = 0; // 1.. у своих и автосохранений
        std::filesystem::path path;
        std::optional<SaveInfo> info; // нет файла или не читается — пусто
        std::string problem;          // почему не читается (другая версия и т.п.)
        Texture2D thumbnail{};        // id 0 — нет
    };
    struct Job {
        std::size_t slot = 0;
        SaveSnapshot snapshot;
        Image screen{}; // кадр для миниатюры; data == nullptr — нет
        bool automatic = false;
    };
    struct Done {
        std::size_t slot = 0;
        bool ok = false;
        bool automatic = false;
        std::string error;
        std::size_t bytes = 0;
        double seconds = 0.0;
    };
    struct Request {
        std::size_t slot = 0;
        std::string title;
        bool automatic = false;
    };

    std::string slotName(const Slot& slot) const;
    std::size_t quickSlot() const { return 0; }
    std::size_t nextAutoSlot() const;
    void refresh(std::size_t slot);
    void request(std::size_t slot, std::string title, bool automatic);
    void startLoad(std::size_t slot);
    void setStatus(std::string text, bool error = false);
    void writerLoop();
    void waitWriterIdle();

    std::filesystem::path folder_;
    std::vector<Slot> slots_;
    bool autosave_ = true;
    int lastAutoDay_ = 0;
    std::vector<Request> pending_; // заказано в этом кадре
    char title_[64] = "";
    std::optional<std::size_t> confirmDelete_;
    std::optional<std::size_t> confirmLoad_;

    std::string status_;
    bool statusError_ = false;
    double statusAt_ = -100.0;

    // Фоновая запись: одна очередь, один поток — файлы пишутся по порядку.
    std::thread writer_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Job> jobs_;
    std::vector<Done> done_;
    bool writing_ = false;
    bool stopping_ = false;

    // Фоновая загрузка.
    std::thread loader_;
    std::atomic<bool> loadReady_{false};
    bool loading_ = false;
    bool pausedForLoad_ = false;
    bool wasPaused_ = false;
    double loadStarted_ = 0.0;
    std::string loadName_;
    std::unique_ptr<World> loaded_;
    SaveInfo loadedInfo_;
    std::string loadError_;
};

} // namespace ui
} // namespace lw
