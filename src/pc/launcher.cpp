#include "launcher.h"
#include "widescreen.h"
#include "launcher_data.hpp"
#include <aurora/dvd.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/rmlui.hpp>
#include <RmlUi/Core.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <vector>

namespace {
launcher::Preferences prefs;
std::filesystem::path config_path;

std::string identity(const std::string& path) {
    struct stat s{};
    if (stat(path.c_str(), &s) != 0) return {};
    return std::to_string(s.st_dev) + ":" + std::to_string(s.st_ino) + ":" +
        std::to_string(s.st_size) + ":" + std::to_string(s.st_mtim.tv_sec) + ":" +
        std::to_string(s.st_mtim.tv_nsec) + ":" + std::to_string(s.st_ctim.tv_sec) + ":" +
        std::to_string(s.st_ctim.tv_nsec);
}

// The native dialog can finish after the window closes. Its callback owns a
// shared state reference and never touches the document or SDL window.
struct DialogResult {
    std::mutex mutex;
    bool ready = false;
    std::string path, error;
};
void dialog_done(void* userdata, const char* const* files, int) {
    std::unique_ptr<std::shared_ptr<DialogResult>> owner(static_cast<std::shared_ptr<DialogResult>*>(userdata));
    std::lock_guard lock((*owner)->mutex);
    if (!files) (*owner)->error = SDL_GetError();
    else if (files[0]) (*owner)->path = files[0];
    (*owner)->ready = true;
}

class Launcher final : public Rml::EventListener {
    SDL_Window* window;
    Rml::ElementDocument* document;
    std::shared_ptr<DialogResult> dialog;
    std::future<launcher::DiscInfo> inspection;
    std::future<launcher::Verification> verification;
    std::atomic_bool cancel{false};
    std::atomic_uint progress{0};
    std::string pending_path, verification_identity, selected_identity;
    bool supported = false, settings = false;
    uint64_t last_identity_check = 0, last_axis = 0;
    unsigned last_progress = 101;
    std::vector<std::string> focus_ids;
    int result = -2;

    Rml::Element* element(const char* id) { return document->GetElementById(id); }
    void text(const char* id, const std::string& value) {
        auto* e = element(id);
        e->SetInnerRML("");
        auto node = document->CreateTextNode(value);
        e->AppendChild(std::move(node));
    }
    void enabled(const char* id, bool value) {
        auto* e = element(id);
        e->SetPseudoClass("disabled", !value);
        e->SetProperty("focus", value ? "auto" : "none");
        e->SetProperty("tab-index", value ? "auto" : "none");
        if (!value) e->Blur();
    }
    bool busy() const { return dialog || inspection.valid() || verification.valid(); }
    void status(const std::string& value, bool error = false) {
        text("status", value); element("status")->SetClass("error", error);
    }
    void save() {
        std::string error;
        if (!launcher::save_preferences(config_path, prefs, error)) status(error, true);
    }
    void controls() {
        enabled("play", supported && !busy());
        enabled("choose", !busy());
        enabled("verify", verification.valid() || (supported && !busy()));
        enabled("settings", !busy());
        text("verify", verification.valid() ? "Cancel verification" : "Verify disc");
        focus_ids = settings ? std::vector<std::string>{"fullscreen", "vsync", "scale", "back"}
                             : std::vector<std::string>{"play", "choose", "verify", "settings", "quit"};
    }
    void focus_step(int direction) {
        auto* focus = document->GetContext()->GetFocusElement();
        int index = direction > 0 ? -1 : 0;
        for (size_t i = 0; i < focus_ids.size(); ++i)
            if (element(focus_ids[i].c_str()) == focus) index = static_cast<int>(i);
        for (size_t i = 0; i < focus_ids.size(); ++i) {
            index = (index + direction + static_cast<int>(focus_ids.size())) % static_cast<int>(focus_ids.size());
            auto* e = element(focus_ids[index].c_str());
            if (!e->IsPseudoClassSet("disabled")) { e->Focus(); e->ScrollIntoView(); break; }
        }
    }
    void activate_focus() {
        auto* e = document->GetContext()->GetFocusElement();
        if (!e) return;
        for (auto& id : focus_ids) if (e == element(id.c_str())) {
            if (!e->IsPseudoClassSet("disabled")) action(id);
            break;
        }
    }
    void show_settings(bool show) {
        settings = show;
        element("home")->SetProperty("display", show ? "none" : "block");
        element("preferences")->SetProperty("display", show ? "block" : "none");
        text("fullscreen", prefs.fullscreen ? "Display: Fullscreen" : "Display: Windowed");
        const char* override = std::getenv("MELEE_VSYNC");
        const bool effective_vsync = override ? override[0] != '0' : prefs.vsync;
        text("vsync", std::string(effective_vsync ? "VSync: On" : "VSync: Off") + (override ? " (environment)" : ""));
        text("scale", "UI scale: " + std::to_string(static_cast<int>(prefs.scale * 100)) + "%");
        controls();
        element(show ? "fullscreen" : "settings")->Focus();
    }
    void inspect(const std::string& path) {
        pending_path = std::filesystem::absolute(path).string();
        status("Checking disc image...");
        inspection = std::async(std::launch::async, [path] { return launcher::inspect_disc(path); });
        controls();
    }
    void action(const std::string& id) {
        if (id == "quit") { result = 0; cancel = true; return; }
        if (id == "verify" && verification.valid()) { cancel = true; status("Canceling verification..."); return; }
        if (busy()) return;
        if (id == "choose") {
            static const SDL_DialogFileFilter filters[] = {{"GameCube disc images", "iso;gcm;ciso;rvz;gcz;wia"}, {"All files", "*"}};
            dialog = std::make_shared<DialogResult>();
            SDL_ShowOpenFileDialog(dialog_done, new std::shared_ptr<DialogResult>(dialog), window,
                                  filters, 2, prefs.disc.empty() ? nullptr : prefs.disc.c_str(), false);
            controls();
        } else if (id == "play" && supported) {
            auto check = launcher::inspect_disc(prefs.disc);
            if (!check.supported) { supported = false; status(check.message, true); controls(); return; }
            if (aurora_dvd_open(prefs.disc.c_str())) { save(); result = 1; }
            else { aurora_dvd_close(); status("Could not load this disc. Choose another image or verify it.", true); }
        } else if (id == "verify" && supported) {
            cancel = false; progress = 0; last_progress = 101;
            verification_identity = identity(prefs.disc);
            auto path = prefs.disc;
            verification = std::async(std::launch::async, [this, path] {
                return launcher::verify_disc(path, cancel, progress);
            });
            controls(); element("verify")->Focus();
        } else if (id == "settings" || id == "back") show_settings(id == "settings");
        else if (id == "fullscreen") {
            if (SDL_SetWindowFullscreen(window, !prefs.fullscreen)) prefs.fullscreen = !prefs.fullscreen;
            else status(std::string("Could not change display mode: ") + SDL_GetError(), true);
            save(); show_settings(true);
        } else if (id == "vsync") {
            if (std::getenv("MELEE_VSYNC")) status("VSync is controlled by the MELEE_VSYNC environment setting.");
            else { prefs.vsync = !prefs.vsync; aurora_enable_vsync(prefs.vsync); save(); }
            show_settings(true); element("vsync")->Focus();
        } else if (id == "scale") {
            prefs.scale = prefs.scale >= 1.5f ? 0.75f : prefs.scale + 0.25f;
            aurora::rmlui::set_ui_scale(prefs.scale); save(); show_settings(true); element("scale")->Focus();
        }
    }
public:
    Launcher(SDL_Window* w, Rml::ElementDocument* d) : window(w), document(d) {
        document->AddEventListener(Rml::EventId::Click, this);
        document->AddEventListener(Rml::EventId::Keydown, this);
        document->Show();
        controls(); element("choose")->Focus();
    }
    ~Launcher() override {
        cancel = true;
        if (verification.valid()) verification.wait();
        if (inspection.valid()) inspection.wait();
        document->RemoveEventListener(Rml::EventId::Click, this);
        document->RemoveEventListener(Rml::EventId::Keydown, this);
        auto* context = document->GetContext();
        document->Close();
        context->Update();
    }
    void ProcessEvent(Rml::Event& event) override {
        if (event.GetId() == Rml::EventId::Click) {
            auto* target = event.GetTargetElement();
            while (target && target != document) {
                if (target->GetTagName() == "button") {
                    if (!target->IsPseudoClassSet("disabled")) action(target->GetId());
                    break;
                }
                target = target->GetParentNode();
            }
        } else if (event.GetId() == Rml::EventId::Keydown) {
            auto key = event.GetParameter<int>("key_identifier", 0);
            if (key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN) {
                focus_step(key == Rml::Input::KI_UP ? -1 : 1); event.StopPropagation();
            } else if (key == Rml::Input::KI_ESCAPE) {
                if (settings) show_settings(false);
                else if (verification.valid()) { cancel = true; }
                event.StopPropagation();
            }
        }
    }
    int run(const std::string& initial_error) {
        if (!prefs.disc.empty()) inspect(prefs.disc);
        if (!initial_error.empty()) status(initial_error, true);
        while (result == -2) {
            auto* events = aurora_update();
            for (auto* e = events; e && e->type != AURORA_NONE; ++e) {
                if (e->type == AURORA_EXIT) { result = 0; cancel = true; }
                if (e->type != AURORA_SDL_EVENT) continue;
                if (e->sdl.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    auto button = e->sdl.gbutton.button;
                    if (button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) focus_step(1);
                    else if (button == SDL_GAMEPAD_BUTTON_DPAD_UP) focus_step(-1);
                    else if (button == SDL_GAMEPAD_BUTTON_SOUTH || button == SDL_GAMEPAD_BUTTON_START) activate_focus();
                    else if (button == SDL_GAMEPAD_BUTTON_EAST) {
                        if (settings) show_settings(false);
                        else if (verification.valid()) cancel = true;
                    }
                } else if (e->sdl.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e->sdl.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
                    auto value = e->sdl.gaxis.value;
                    auto now = SDL_GetTicks();
                    if (std::abs(value) > 18000 && now - last_axis > 200) {
                        focus_step(value > 0 ? 1 : -1); last_axis = now;
                    }
                } else if (e->sdl.type == SDL_EVENT_DROP_FILE && !busy() && e->sdl.drop.data) {
                    inspect(e->sdl.drop.data);
                }
            }
            if (dialog) {
                bool ready; std::string path, error;
                { std::lock_guard lock(dialog->mutex); ready = dialog->ready; path = dialog->path; error = dialog->error; }
                if (ready) {
                    dialog.reset();
                    if (!error.empty()) status("File chooser failed: " + error, true);
                    else if (!path.empty()) inspect(path);
                    controls(); element("choose")->Focus();
                }
            }
            if (inspection.valid() && inspection.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto info = inspection.get();
                if (info.supported) {
                    prefs.disc = pending_path; supported = true; selected_identity = identity(prefs.disc);
                    text("disc-name", std::filesystem::path(prefs.disc).filename().string());
                    text("disc-path", prefs.disc); text("disc-info", info.message);
                    status("Ready to play / Disc not verified."); save();
                } else {
                    if (prefs.disc == pending_path) { supported = false; prefs.disc.clear(); save(); }
                    status(info.message, true);
                }
                controls(); element(supported ? "play" : "choose")->Focus();
            }
            if (verification.valid()) {
                auto percent = progress.load();
                if (percent != last_progress && !cancel) { status("Verifying disc / " + std::to_string(percent) + "%"); last_progress = percent; }
                if (verification.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    auto checked = verification.get();
                    if (checked.state == launcher::VerifyState::Error) supported = false;
                    if (identity(prefs.disc) != verification_identity)
                        status("Disc changed during verification. Verify it again.", true);
                    else status(checked.message, checked.state == launcher::VerifyState::Mismatch || checked.state == launcher::VerifyState::Error);
                    controls(); element("verify")->Focus();
                }
            }
            if (supported && !busy() && SDL_GetTicks() - last_identity_check > 1000) {
                last_identity_check = SDL_GetTicks();
                if (identity(prefs.disc) != selected_identity) {
                    supported = false; status("Disc changed or was removed. Choose the image again.", true); controls();
                }
            }
            if (result != -2) break;
            if (aurora_begin_frame()) aurora_end_frame();
            SDL_Delay(8);
        }
        return result;
    }
};
}

extern "C" void pc_launcher_configure(AuroraConfig* config) {
    config_path = std::filesystem::path(config->userPath ? config->userPath : ".") / "launcher.cfg";
    prefs = launcher::load_preferences(config_path);
    config->startFullscreen = prefs.fullscreen;
    config->msaa = prefs.msaa;
    config->maxTextureAnisotropy = prefs.anisotropy;
    if (!std::getenv("MELEE_VSYNC")) config->vsync = prefs.vsync;
}

extern "C" int pc_launcher_run(const char* command_line_disc, SDL_Window* window) {
    try {
        std::string error;
        if (command_line_disc) {
            auto info = launcher::inspect_disc(command_line_disc);
            if (info.supported && aurora_dvd_open(command_line_disc)) {
                prefs.disc = std::filesystem::absolute(command_line_disc).string();
                if (!launcher::save_preferences(config_path, prefs, error)) SDL_Log("%s", error.c_str());
                return 1;
            }
            aurora_dvd_close();
            error = info.supported ? "Could not load the requested disc. Choose another image." : info.message;
        }
        auto* context = aurora::rmlui::get_context();
        if (!context) { SDL_Log("Launcher: RmlUi context is unavailable."); return -1; }
        auto resources = std::filesystem::path(SDL_GetBasePath()) / "resources";
        if (!Rml::LoadFontFace((resources / "font.ttf").string())) {
            SDL_Log("Launcher: could not load font from %s", resources.c_str()); return -1;
        }
        Rml::LoadFontFace((resources / "font-bold.ttf").string());
        aurora::rmlui::set_ui_scale(prefs.scale);
        auto* document = context->LoadDocument((resources / "launcher.rml").string());
        if (!document) { SDL_Log("Launcher: could not load launcher.rml"); return -1; }
        Launcher launcher(window, document);
        return launcher.run(error);
    } catch (const std::exception& e) {
        SDL_Log("Launcher failed: %s", e.what()); return -1;
    }
}

// In-game settings use the same preferences and RmlUi context as the launcher.
#include <dolphin/vi.h>
#include <dolphin/pad.h>
extern "C" void pc_audio_set_volume(float volume);
namespace {
class PortMenu final : public Rml::EventListener {
public:
    Rml::ElementDocument* document = nullptr;
    Rml::ElementDocument* counter = nullptr;
    SDL_Window* window = nullptr;
    bool open = false;
    uint64_t last_fps = 0;
    void label(const char* id, const std::string& value) {
        auto* e = document->GetElementById(id);
        e->SetInnerRML("");
        e->AppendChild(document->CreateTextNode(value));
    }
    void refresh() {
        label("display", VIGetWindowFullscreen() ? "Fullscreen" : "Windowed");
        label("sync", std::getenv("MELEE_VSYNC") ? "Environment override" : prefs.vsync ? "On" : "Off");
        label("resolution", prefs.render_scale == 0 ? "Auto (window size)" : "" + std::to_string(int(prefs.render_scale)) + "x native");
        label("aspect", prefs.widescreen == 0 ? "Original 4:3" : prefs.widescreen == 1 ? "Widescreen 16:9" : "Auto (window aspect)");
        label("aa", prefs.msaa == 1 ? "Off" : "4x MSAA");
        label("filter", std::to_string(prefs.anisotropy) + "x");
        label("volume", std::to_string(int(prefs.volume * 100 + 0.5f)) + "%");
        label("mute", prefs.mute ? "On" : "Off");
        label("fps", prefs.fps ? "On" : "Off");
    }
    void toggle() {
        if (!document) return;
        open = !open;
        PADBlockInput(open);
        if (open) { refresh(); document->Show(); document->GetElementById("display")->Focus(); }
        else document->Hide();
    }
    void ProcessEvent(Rml::Event& event) override {
        if (event.GetId() == Rml::EventId::Keydown) {
            if (event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_ESCAPE && open) toggle();
            return;
        }
        auto* target = event.GetTargetElement();
        while (target && target != document && target->GetTagName() != "button") target = target->GetParentNode();
        if (!target || target == document) return;
        auto id = target->GetId();
        if (id == "resume") { toggle(); return; }
        if (id == "display") {
            if (!SDL_SetWindowFullscreen(window, !VIGetWindowFullscreen())) { label("menu-status", SDL_GetError()); return; }
            prefs.fullscreen = VIGetWindowFullscreen();
        } else if (id == "sync" && !std::getenv("MELEE_VSYNC")) {
            prefs.vsync = !prefs.vsync; aurora_enable_vsync(prefs.vsync);
        } else if (id == "resolution") {
            prefs.render_scale = prefs.render_scale >= 4 ? 0 : int(prefs.render_scale) + 1;
            VISetFrameBufferScale(prefs.render_scale);
        } else if (id == "aspect") {
            prefs.widescreen = (prefs.widescreen + 1) % 3;
            pc_widescreen_set_mode(prefs.widescreen);
        } else if (id == "aa") prefs.msaa = prefs.msaa == 1 ? 4 : 1;
        else if (id == "filter") prefs.anisotropy = prefs.anisotropy >= 16 ? 1 : prefs.anisotropy * 2;
        else if (id == "volume") {
            int step = int(prefs.volume * 10 + 0.5f);
            prefs.volume = (step >= 10 ? 0 : step + 1) / 10.0f;
        } else if (id == "mute") prefs.mute = !prefs.mute;
        else if (id == "fps") prefs.fps = !prefs.fps;
        pc_audio_set_volume(prefs.mute ? 0 : prefs.volume);
        std::string error;
        label("menu-status", launcher::save_preferences(config_path, prefs, error) ? "Settings saved. Gameplay continues while this menu is open." : error);
        refresh();
    }
};
PortMenu port_menu;
}
extern "C" bool pc_menu_is_open(void) { return port_menu.open; }
extern "C" void pc_menu_init(SDL_Window* window) {
    pc_widescreen_set_mode(prefs.widescreen);
    VISetFrameBufferScale(prefs.render_scale);
    pc_audio_set_volume(prefs.mute ? 0 : prefs.volume);
    auto* context = aurora::rmlui::get_context();
    if (!context) { SDL_Log("F1 menu: RmlUi context unavailable"); return; }
    auto resources = std::filesystem::path(SDL_GetBasePath()) / "resources";
    Rml::LoadFontFace((resources / "font.ttf").string());
    Rml::LoadFontFace((resources / "font-bold.ttf").string());
    aurora::rmlui::set_ui_scale(prefs.scale);
    port_menu.window = window;
    port_menu.document = context->LoadDocument((resources / "port-menu.rml").string());
    port_menu.counter = context->LoadDocument((resources / "fps.rml").string());
    if (!port_menu.document) { SDL_Log("F1 menu: could not load port-menu.rml"); return; }
    port_menu.document->AddEventListener(Rml::EventId::Click, &port_menu);
    port_menu.document->AddEventListener(Rml::EventId::Keydown, &port_menu);
}
extern "C" void pc_menu_toggle(void) { port_menu.toggle(); }
extern "C" void pc_menu_update(void) {
    if (port_menu.counter) {
        if (prefs.fps) {
            if (!port_menu.counter->IsVisible())
                port_menu.counter->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
            if (SDL_GetTicks() - port_menu.last_fps >= 500) {
                port_menu.counter->GetElementById("count")->SetInnerRML(std::to_string(int(aurora_get_fps() + 0.5f)) + " FPS");
                port_menu.last_fps = SDL_GetTicks();
            }
        } else if (port_menu.counter->IsVisible()) port_menu.counter->Hide();
    }
}
