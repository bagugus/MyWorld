#include "ui/Saves.h"

#include "render/CameraController.h"
#include "sim/Calendar.h"
#include "sim/SimDriver.h"
#include "sim/World.h"
#include "ui/Format.h"

#include <imgui.h>
#include <rlImGui.h>
#include <rlgl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <system_error>
#include <utility>

namespace lw::ui {

namespace {

constexpr int kManualSlots = 6;
constexpr int kAutoSlots = 3;
constexpr const char* kExtension = ".lwsave";
// Миниатюра в файле — не шире и не выше этого (кадр 16:9 — 256 × 144).
constexpr int kThumbWidth = 256;
constexpr int kThumbHeight = 160;
// Миниатюра в окне.
constexpr float kRowThumbWidth = 128.0f;
constexpr double kStatusSeconds = 4.0;

// Уменьшить кадр экрана (RGBA) до миниатюры (RGB) усреднением по клеткам.
SaveThumbnail shrink(const Image& screen) {
    SaveThumbnail thumb;
    if (screen.data == nullptr || screen.width <= 0 || screen.height <= 0 ||
        screen.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) {
        return thumb;
    }
    const double scale = std::max(static_cast<double>(screen.width) / kThumbWidth,
                                  static_cast<double>(screen.height) / kThumbHeight);
    thumb.width = std::max(1, static_cast<int>(std::lround(screen.width / scale)));
    thumb.height = std::max(1, static_cast<int>(std::lround(screen.height / scale)));
    thumb.rgb.resize(static_cast<std::size_t>(thumb.width) * thumb.height * 3);
    const auto* source = static_cast<const unsigned char*>(screen.data);
    for (int ty = 0; ty < thumb.height; ++ty) {
        const int y0 = ty * screen.height / thumb.height;
        const int y1 = std::max(y0 + 1, (ty + 1) * screen.height / thumb.height);
        for (int tx = 0; tx < thumb.width; ++tx) {
            const int x0 = tx * screen.width / thumb.width;
            const int x1 = std::max(x0 + 1, (tx + 1) * screen.width / thumb.width);
            unsigned sum[3] = {};
            for (int y = y0; y < y1; ++y) {
                const unsigned char* row = source + (static_cast<std::size_t>(y) * screen.width + x0) * 4;
                for (int x = x0; x < x1; ++x, row += 4) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                }
            }
            const unsigned count = static_cast<unsigned>((y1 - y0) * (x1 - x0));
            unsigned char* out = &thumb.rgb[(static_cast<std::size_t>(ty) * thumb.width + tx) * 3];
            for (int c = 0; c < 3; ++c) {
                out[c] = static_cast<unsigned char>(sum[c] / count);
            }
        }
    }
    return thumb;
}

std::string sizeText(std::size_t bytes) {
    char text[32];
    if (bytes >= 1024 * 1024) {
        std::snprintf(text, sizeof(text), "%.1f МБ", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(text, sizeof(text), "%zu КБ", (bytes + 1023) / 1024);
    }
    return text;
}

// Когда записано — по часам компьютера.
std::string realTimeText(std::int64_t savedAt) {
    const std::time_t time = static_cast<std::time_t>(savedAt);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%d.%m.%Y %H:%M", &local);
    return text;
}

} // namespace

Saves::Saves(std::filesystem::path folder) : folder_(std::move(folder)) {
    const auto add = [&](SlotKind kind, int number, const std::string& file) {
        Slot slot;
        slot.kind = kind;
        slot.number = number;
        slot.path = folder_ / (file + kExtension);
        slots_.push_back(std::move(slot));
    };
    add(SlotKind::Quick, 0, "quick");
    for (int i = 1; i <= kManualSlots; ++i) {
        add(SlotKind::Manual, i, "slot" + std::to_string(i));
    }
    for (int i = 1; i <= kAutoSlots; ++i) {
        add(SlotKind::Auto, i, "auto" + std::to_string(i));
    }
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        refresh(i);
    }
    writer_ = std::thread([this] { writerLoop(); });
}

Saves::~Saves() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    writer_.join();
    if (loader_.joinable()) {
        loader_.join();
    }
    for (Slot& slot : slots_) {
        if (slot.thumbnail.id != 0) {
            UnloadTexture(slot.thumbnail);
        }
    }
}

std::string Saves::slotName(const Slot& slot) const {
    switch (slot.kind) {
    case SlotKind::Quick:
        return "Быстрое";
    case SlotKind::Manual:
        return "Слот " + std::to_string(slot.number);
    case SlotKind::Auto:
        return "Авто " + std::to_string(slot.number);
    }
    return {};
}

std::size_t Saves::nextAutoSlot() const {
    // Свободный, иначе самый старый.
    std::size_t best = slots_.size();
    std::int64_t oldest = 0;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const Slot& slot = slots_[i];
        if (slot.kind != SlotKind::Auto) {
            continue;
        }
        if (!slot.info) {
            return i;
        }
        if (best == slots_.size() || slot.info->savedAt < oldest) {
            best = i;
            oldest = slot.info->savedAt;
        }
    }
    return best;
}

void Saves::refresh(std::size_t index) {
    Slot& slot = slots_[index];
    if (slot.thumbnail.id != 0) {
        UnloadTexture(slot.thumbnail);
        slot.thumbnail = {};
    }
    slot.info.reset();
    slot.problem.clear();
    std::error_code code;
    if (!std::filesystem::exists(slot.path, code)) {
        return;
    }
    std::string error;
    slot.info = readSaveInfo(slot.path, &error);
    if (!slot.info) {
        slot.problem = error;
        return;
    }
    const SaveThumbnail& thumb = slot.info->thumbnail;
    if (thumb.width > 0 && thumb.height > 0) {
        Image image{};
        image.data = const_cast<std::uint8_t*>(thumb.rgb.data());
        image.width = thumb.width;
        image.height = thumb.height;
        image.mipmaps = 1;
        image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8;
        slot.thumbnail = LoadTextureFromImage(image);
        SetTextureFilter(slot.thumbnail, TEXTURE_FILTER_BILINEAR);
    }
}

void Saves::setStatus(std::string text, bool error) {
    status_ = std::move(text);
    statusError_ = error;
    statusAt_ = GetTime();
    TraceLog(error ? LOG_WARNING : LOG_INFO, "SAVE: %s", status_.c_str());
}

void Saves::beginSession(const World& world) {
    lastAutoDay_ = calendarAt(world.tick()).day;
    pending_.clear();
}

void Saves::request(std::size_t slot, std::string title, bool automatic) {
    if (loading_ || slot >= slots_.size()) {
        return;
    }
    for (const Request& queued : pending_) {
        if (queued.slot == slot) {
            return;
        }
    }
    pending_.push_back({slot, std::move(title), automatic});
}

void Saves::handleHotkeys(bool keyboardFree) {
    if (!keyboardFree) {
        return;
    }
    if (IsKeyPressed(KEY_F5)) {
        request(quickSlot(), "", false);
    }
    if (IsKeyPressed(KEY_F9)) {
        if (slots_[quickSlot()].info) {
            startLoad(quickSlot());
        } else {
            setStatus("Быстрого сохранения нет — F5 сохраняет", true);
        }
    }
}

void Saves::update(const World& world, SimDriver& driver) {
    // Итоги фоновой записи.
    std::vector<Done> finished;
    {
        const std::lock_guard lock(mutex_);
        finished.swap(done_);
    }
    for (const Done& done : finished) {
        refresh(done.slot);
        const std::string name = slotName(slots_[done.slot]);
        if (done.ok) {
            char text[160];
            std::snprintf(text, sizeof(text), "%s: %s, %s за %.2f с", done.automatic ? "Автосохранение" : "Сохранено",
                          name.c_str(), sizeText(done.bytes).c_str(), done.seconds);
            setStatus(text);
        } else {
            setStatus("Не сохранено (" + name + "): " + done.error, true);
        }
    }

    // Загрузка: мир на паузе, пока строится новый.
    if (loading_) {
        if (!pausedForLoad_) {
            wasPaused_ = driver.paused();
            driver.setPaused(true);
            driver.cancelSkip();
            pausedForLoad_ = true;
        }
        if (loadReady_.load(std::memory_order_acquire)) {
            loader_.join();
            loading_ = false;
            if (!loaded_) {
                setStatus("Не загружено (" + loadName_ + "): " + loadError_, true);
                driver.setPaused(wasPaused_);
                pausedForLoad_ = false;
            }
        }
    }

    // Автосохранение: перед сменой фазы (мир встал на границе тика) и в
    // начале новых суток.
    driver.setHoldBeforePhase(autosave_ && !loading_);
    if (!autosave_ || loading_ || loaded_) {
        return;
    }
    const int day = calendarAt(world.tick()).day;
    if (const std::optional<WorldPhase> phase = driver.heldBeforePhase()) {
        request(nextAutoSlot(), std::string("Перед фазой «") + phaseName(*phase) + "»", true);
        lastAutoDay_ = day;
    } else if (day != lastAutoDay_) {
        lastAutoDay_ = day;
        request(nextAutoSlot(), "Начало дня " + std::to_string(day), true);
    }
}

void Saves::capture(const World& world, const CameraController& camera, bool blank) {
    if (pending_.empty()) {
        return;
    }
    std::vector<Request> requests;
    requests.swap(pending_);
    if (loading_) {
        return;
    }
    std::string error;
    std::optional<SaveSnapshot> snapshot = captureSave(world, &error);
    if (!snapshot) {
        setStatus("Не сохранено: " + error, true);
        return;
    }
    SaveView& view = snapshot->info.view;
    view.valid = true;
    view.mode = camera.is3d() ? 1 : 0;
    const Vector2 focus = camera.focus();
    view.x = focus.x;
    view.y = focus.y;
    view.zoom = camera.zoom();
    view.yaw = camera.orbitYaw();
    view.pitch = camera.orbitPitch();

    Image screen{};
    if (!blank) {
        rlDrawRenderBatchActive(); // всё нарисованное — в кадре
        screen = LoadImageFromScreen();
    }
    for (std::size_t i = 0; i < requests.size(); ++i) {
        Job job;
        job.slot = requests[i].slot;
        job.automatic = requests[i].automatic;
        const bool last = i + 1 == requests.size();
        job.snapshot = last ? std::move(*snapshot) : *snapshot;
        job.snapshot.info.title = requests[i].title;
        if (screen.data != nullptr) {
            job.screen = last ? screen : ImageCopy(screen);
        }
        {
            const std::lock_guard lock(mutex_);
            jobs_.push_back(std::move(job));
        }
        wake_.notify_one();
    }
}

void Saves::writerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty()) {
                return; // stopping_: очередь дописана
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
            writing_ = true;
        }
        const auto start = std::chrono::steady_clock::now();
        if (job.screen.data != nullptr) {
            job.snapshot.info.thumbnail = shrink(job.screen);
            UnloadImage(job.screen);
        }
        Done done;
        done.slot = job.slot;
        done.automatic = job.automatic;
        const std::vector<std::uint8_t> bytes = encodeSave(job.snapshot, &done.error);
        done.ok = !bytes.empty() && writeSaveFile(slots_[job.slot].path, bytes, &done.error);
        done.bytes = bytes.size();
        done.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        {
            const std::lock_guard lock(mutex_);
            done_.push_back(std::move(done));
            writing_ = false;
        }
        idle_.notify_all();
    }
}

void Saves::waitWriterIdle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] { return jobs_.empty() && !writing_; });
}

void Saves::startLoad(std::size_t index) {
    if (loading_ || loaded_ || index >= slots_.size()) {
        return;
    }
    loading_ = true;
    loadReady_.store(false, std::memory_order_relaxed);
    loadStarted_ = GetTime();
    loadName_ = slotName(slots_[index]);
    loadError_.clear();
    pending_.clear();
    const std::filesystem::path path = slots_[index].path;
    loader_ = std::thread([this, path] {
        // Только что заказанная запись в этот же слот — сперва дописать.
        waitWriterIdle();
        std::string error;
        std::optional<SaveInfo> info = readSaveInfo(path, &error);
        std::unique_ptr<World> world;
        if (info) {
            world = loadWorld(path, &error);
        }
        loaded_ = std::move(world);
        if (info) {
            loadedInfo_ = std::move(*info);
        }
        loadError_ = error;
        loadReady_.store(true, std::memory_order_release);
    });
}

std::unique_ptr<World> Saves::takeLoaded(SaveInfo& info, bool& wasPaused) {
    if (loading_ || !loaded_) {
        return nullptr;
    }
    info = loadedInfo_;
    wasPaused = wasPaused_;
    pausedForLoad_ = false;
    char text[160];
    std::snprintf(text, sizeof(text), "Загружено: %s за %.1f с", loadName_.c_str(), GetTime() - loadStarted_);
    setStatus(text);
    return std::move(loaded_);
}

void Saves::drawPanel(const World& world) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // Место — под развёрнутое окно: свёрнутое не должно прилипать к низу экрана.
    constexpr float kHeight = 420.0f;
    ImGui::SetNextWindowPos({12.0f, std::max(12.0f, display.y - 12.0f - kHeight)}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({520.0f, kHeight}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Сохранения (F5 / F9)###saves")) {
        ImGui::End();
        return;
    }
    ImGui::Checkbox("Автосохранение: раз в сутки и перед сменой фазы", &autosave_);
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##title", "подпись своего сохранения", title_, sizeof(title_));
    ImGui::SameLine();
    const CalendarTime now = calendarAt(world.tick());
    ImGui::TextDisabled("сейчас день %d, %02d:%02d", now.day, now.hour, now.minute);
    ImGui::Separator();

    ImGui::BeginDisabled(loading_);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        ImGui::PushID(static_cast<int>(i));
        if (i > 0 && slot.kind != slots_[i - 1].kind) {
            ImGui::Separator();
        }
        // Миниатюра.
        const float thumbHeight = kRowThumbWidth * 9.0f / 16.0f;
        if (slot.thumbnail.id != 0) {
            const float height = kRowThumbWidth * slot.thumbnail.height / std::max(1, slot.thumbnail.width);
            rlImGuiImageSize(&slot.thumbnail, static_cast<int>(kRowThumbWidth), static_cast<int>(height));
        } else {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(at, {at.x + kRowThumbWidth, at.y + thumbHeight},
                                                      IM_COL32(40, 44, 52, 255));
            ImGui::Dummy({kRowThumbWidth, thumbHeight});
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        const std::string name = slotName(slot);
        if (slot.info) {
            const SaveInfo& info = *slot.info;
            if (info.title.empty()) {
                ImGui::TextUnformatted(name.c_str());
            } else {
                ImGui::Text("%s — %s", name.c_str(), info.title.c_str());
            }
            const CalendarTime time = calendarAt(info.tick);
            if (info.hasScenario) {
                ImGui::Text("День %d, %02d:%02d · %s · живых %d, зомби %d", time.day, time.hour, time.minute,
                            phaseName(info.phase), info.people, info.zombies);
            } else {
                ImGui::Text("День %d, %02d:%02d · живых %d, зомби %d", time.day, time.hour, time.minute,
                            info.people, info.zombies);
            }
            ImGui::TextDisabled("%s · seed %llu, %.0f × %.0f км", realTimeText(info.savedAt).c_str(),
                                static_cast<unsigned long long>(info.seed), info.config.mapWidth * 0.004,
                                info.config.mapHeight * 0.004);
        } else if (!slot.problem.empty()) {
            ImGui::Text("%s — не читается", name.c_str());
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 340.0f);
            ImGui::TextDisabled("%s", slot.problem.c_str());
            ImGui::PopTextWrapPos();
        } else {
            ImGui::Text("%s — пусто", name.c_str());
        }
        if (slot.kind != SlotKind::Auto) {
            if (ImGui::SmallButton("Сохранить")) {
                request(i, title_, false);
            }
            ImGui::SameLine();
        }
        ImGui::BeginDisabled(!slot.info);
        if (ImGui::SmallButton("Загрузить")) {
            confirmLoad_ = i;
            ImGui::OpenPopup("load");
        }
        ImGui::EndDisabled();
        const bool exists = slot.info || !slot.problem.empty();
        if (exists) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Удалить")) {
                confirmDelete_ = i;
                ImGui::OpenPopup("delete");
            }
        }
        if (ImGui::BeginPopup("load")) {
            ImGui::TextUnformatted("Заменить нынешний мир сохранённым?\nНесохранённое пропадёт.");
            if (ImGui::Button("Загрузить") && confirmLoad_ == i) {
                startLoad(i);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Отмена")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("delete")) {
            ImGui::Text("Удалить «%s»?", name.c_str());
            if (ImGui::Button("Удалить") && confirmDelete_ == i) {
                waitWriterIdle();
                std::error_code code;
                std::filesystem::remove(slot.path, code);
                refresh(i);
                setStatus(code ? "Не удалено: " + code.message() : "Удалено: " + name, static_cast<bool>(code));
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Отмена")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Папка: %s", std::filesystem::absolute(folder_).string().c_str());
    ImGui::End();
}

void Saves::drawStatus(bool series) {
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const auto banner = [&](const std::string& text, float y, ImU32 color) {
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        const ImVec2 at{(display.x - size.x) * 0.5f, y};
        draw->AddRectFilled({at.x - 12.0f, at.y - 6.0f}, {at.x + size.x + 12.0f, at.y + size.y + 6.0f},
                            IM_COL32(12, 14, 18, 210), 6.0f);
        draw->AddText(at, color, text.c_str());
    };
    if (loading_) {
        char text[160];
        std::snprintf(text, sizeof(text), "Загрузка: %s… %.0f с (карта строится заново из seed)", loadName_.c_str(),
                      GetTime() - loadStarted_);
        banner(text, display.y * 0.45f, IM_COL32(235, 235, 235, 255));
        return;
    }
    // В сериале — только ошибки: кадр не загромождать.
    if (GetTime() - statusAt_ < kStatusSeconds && (!series || statusError_)) {
        banner(status_, 48.0f, statusError_ ? IM_COL32(255, 150, 120, 255) : IM_COL32(200, 230, 200, 255));
    }
}

} // namespace lw::ui
