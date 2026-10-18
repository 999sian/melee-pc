/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "launcher.h"
#include "textures.h"
#include "widescreen.h"
#include "launcher_data.hpp"
#include "discfont.h"
#include "version.hpp"
#include "updater.hpp"
#include "net_match.h"
#include "net.h"
#include "pc.h"
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <aurora/dvd.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/rmlui.hpp>
#include <aurora/input.hpp>
/* TargetConditionals.h defines TARGET_OS_IPHONE on macOS too, as 0, so this
 * has to test its value: defined() alone pulled the iOS-only dialog into the
 * macOS build and left the link short a symbol. */
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
#include "ios_dialog.h"
#endif
#include <RmlUi/Core.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <random>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <vector>

namespace {
launcher::Preferences prefs;
std::filesystem::path config_path;

/* Aurora's input router no longer feeds RmlUi itself: the host registers a
 * layer that forwards events to aurora::rmlui::process_input. Without it the
 * launcher and F1 menu get no mouse, touch or keyboard input at all. */
void ensure_rmlui_input_layer() {
    static aurora::input::LayerId layer = aurora::input::kInvalidLayerId;
    if (layer != aurora::input::kInvalidLayerId)
        return;
    layer = aurora::input::register_layer({
        .label = "melee.rmlui",
        .priority = aurora::input::kRmlUiLayerPriority,
        .onEvent =
            [](const aurora::input::InputEvent& event, void*) {
                return aurora::rmlui::process_input(event).handled ?
                           aurora::input::EventResult::Consume :
                           aurora::input::EventResult::Pass;
            },
    });
}

void refresh_online(Rml::ElementDocument* doc) {
    pc_net_set_input_delay(prefs.net_delay);
    for (const auto& pair :
        {std::pair{"net-name", prefs.net_name}, std::pair{"net-target", prefs.net_target}})
    {
        if (auto* e = dynamic_cast<Rml::ElementFormControl*>(doc->GetElementById(pair.first)))
            e->SetValue(pair.second);
    }
    if (auto* e = doc->GetElementById("net-delay"))
        e->SetInnerRML(prefs.net_delay < 0 ? "Auto" : std::to_string(prefs.net_delay) + " frames");
    if (auto* e = doc->GetElementById("net-code"))
        e->SetInnerRML(pc_net_match_local_code());
}
bool change_online(Rml::Event& event) {
    auto* target = event.GetTargetElement();
    if (!target)
        return false;
    auto id = target->GetId();
    if (id != "net-name" && id != "net-target")
        return false;
    auto value = event.GetParameter<Rml::String>("value", "");
    const size_t limit = id == "net-name" ? 8 : 17;
    if (value.size() > limit)
        return true;
    for (char& c : value) {
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (id == "net-target" && c == '#')))
            return true;
    }
    if (id == "net-name") {
        if (!value.empty())
            prefs.net_name = value;
    } else
        prefs.net_target = value;
    return true;
}

const char* filter_name(int mode) {
    // Mode 0 is the plain present blit -- no effect at all, so it is the
    // "off" setting; the bilinear filter it uses is just the scaler.
    static const char* const names[] = {"Off", "Area sampling", "CRT scanlines", "Vibrant"};
    return names[mode & 3];
}

const char* backend_name(int mode) {
#if defined(__APPLE__)
    return "Metal";
#else
    static const char* const names[] = {"Auto", "Direct3D 12", "Vulkan", "Direct3D 11"};
    if (mode < 0 || mode > 3)
        return "Auto";
    return names[mode];
#endif
}

bool load_menu_fonts(const std::filesystem::path& resources) {
    if (!Rml::LoadFontFace((resources / "font.ttf").string(), true)) {
        SDL_Log("Menus: could not load font from %s", resources.c_str());
        return false;
    }
    Rml::LoadFontFace((resources / "font-bold.ttf").string());
    /* The menus' face (Noto Sans SemiCondensed, cut to Latin), with its
     * ExtraBold for the plates' names. Liberation covers what it lacks. */
    for (const char* face : {"menu.ttf", "menu-bold.ttf"})
        if (!Rml::LoadFontFace((resources / face).string()))
            SDL_Log("Menus: could not load %s", face);
    if (!Rml::LoadFontFace((resources / "menu-heavy.ttf").string(), false,
            static_cast<Rml::Style::FontWeight>(800)))
        SDL_Log("Menus: could not load menu-heavy.ttf");
    return true;
}

/* The two Direct3D entries only exist on Windows, so elsewhere the cycle goes
 * straight from Auto to Vulkan: offering a backend that can only ever fail
 * over leaves the button showing a label for something the run is not using.
 * ponytail: indices stay identical on every platform so the prefs file and
 * the BACKEND_* mapping below need no per-platform cases. */
int backend_next(int mode, int direction = 1) {
#if defined(_WIN32)
    return (mode + direction + 4) % 4;
#else
    return mode == 0 ? 2 : 0;
#endif
}

constexpr const char* tab_ids[] = {
    "tab-graphics", "tab-audio", "tab-cheats", "tab-controls", "tab-online", "tab-launcher"};

template <size_t N>
int index_of(const char* const (&ids)[N], const Rml::String& id) {
    const auto it = std::find(std::begin(ids), std::end(ids), id);
    return it == std::end(ids) ? -1 : static_cast<int>(it - std::begin(ids));
}
std::string resolution_name(float scale) {
    return scale == 0 ? std::string("Auto") : std::to_string(int(scale)) + "x";
}
const char* dsu_label() {
    return pc_dsu_env_override() ? "Environment override" : prefs.dsu ? "On" : "Off";
}

std::string identity(const std::string& path) {
    if (path.rfind("content://", 0) == 0)
        return path;
    struct stat s = {};
    if (stat(path.c_str(), &s) != 0)
        return {};
#ifdef _WIN32
    return std::to_string(s.st_dev) + ":" + std::to_string(s.st_ino) + ":" +
           std::to_string(s.st_size) + ":" + std::to_string(s.st_mtime) + ":" +
           std::to_string(s.st_ctime);
#elif defined(__APPLE__)
    return std::to_string(s.st_dev) + ":" + std::to_string(s.st_ino) + ":" +
           std::to_string(s.st_size) + ":" + std::to_string(s.st_mtimespec.tv_sec) + ":" +
           std::to_string(s.st_mtimespec.tv_nsec) + ":" + std::to_string(s.st_ctimespec.tv_sec) +
           ":" + std::to_string(s.st_ctimespec.tv_nsec);
#else
    return std::to_string(s.st_dev) + ":" + std::to_string(s.st_ino) + ":" +
           std::to_string(s.st_size) + ":" + std::to_string(s.st_mtim.tv_sec) + ":" +
           std::to_string(s.st_mtim.tv_nsec) + ":" + std::to_string(s.st_ctim.tv_sec) + ":" +
           std::to_string(s.st_ctim.tv_nsec);
#endif
}

// The native dialog can finish after the window closes. Its callback owns a
// shared state reference and never touches the document or SDL window.
struct DialogResult {
    std::mutex mutex;
    bool ready = false;
    std::string path, error;
};
void dialog_done(void* userdata, const char* const* files, int) {
    std::unique_ptr<std::shared_ptr<DialogResult>> owner(
        static_cast<std::shared_ptr<DialogResult>*>(userdata));
    std::lock_guard lock((*owner)->mutex);
    if (!files)
        (*owner)->error = SDL_GetError();
    else if (files[0])
        (*owner)->path = files[0];
    (*owner)->ready = true;
}

/* Aurora compiles the known pipeline configs ahead of use -- the ~12k-config
 * seed on desktop, on Android the ones this device has built before -- on
 * worker threads from startup, or, without workers (Android), in this loop's
 * idle time. Until a config is compiled, every draw that needs it is skipped
 * (issue #46), so the drain finishing before a match is the difference between
 * a match that pops and one that does not. The launcher is the one screen
 * where the player is already waiting; spend it there. */
static uint32_t pending_pipelines() {
    return aurora_wait_pipelines(0);
}

/* The platform, as a class on the launcher's <body>: launcher.rcss hides
 * what it has no use for (.no-mobile: the window mode, the keyboard). */
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
constexpr bool is_mobile = true;
#else
constexpr bool is_mobile = false;
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
constexpr const char* disc_note =
    "Use Choose Disc, or place a disc image in Files > On My iPhone > Melee.";
#elif defined(__ANDROID__)
constexpr const char* disc_note = "Use Choose Disc to pick a disc image.";
#else
constexpr const char* disc_note = "Drop a disc image here, or use Choose Disc.";
#endif
/* Melee colours the menu frame after the entry under the cursor. */
constexpr const char* main_ids[] = {"play", "choose", "verify", "settings", "discord", "quit"};

class Launcher final : public Rml::EventListener {
    SDL_Window* window;
    Rml::ElementDocument* document;
    std::shared_ptr<DialogResult> dialog;
    std::future<launcher::DiscInfo> inspection;
    std::future<launcher::Verification> verification;
    std::atomic_bool cancel{false};
    std::atomic_uint progress{0};
    std::string pending_path, verification_identity, selected_identity;
    bool supported = false, settings = false, quiet = false;
    uint64_t last_identity_check = 0, last_axis = 0, last_axis_x = 0;
    unsigned last_progress = 101;
    std::vector<std::string> focus_ids;
    int result = -2, tab = 0;
    bool update_card_dismissed = false;
    pc::updater::Status last_updater_status = pc::updater::Status::Idle;
    float last_download_progress = -1.0f;
    // Set once the player asks to play while the pipeline queue is still
    // draining: the game starts by itself when the queue empties, and a second
    // press starts it immediately.
    bool shaders_waited = false;
    uint32_t last_pending = 0;
    // The settings row holding the focus, lit as Melee lights its options.
    Rml::Element* current_row = nullptr;
    // Where the pointer last moved, so only a real move steers the cursor.
    int pointer_x = -1, pointer_y = -1;

    Rml::Element* element(const char* id) {
        if (std::strcmp(id, "fullscreen") == 0) {
            auto* e = document->GetElementById("display");
            if (e)
                return e;
        }
        if (std::strcmp(id, "vsync") == 0) {
            auto* e = document->GetElementById("sync");
            if (e)
                return e;
        }
        return document->GetElementById(id);
    }
    void text(const char* id, const std::string& value) {
        auto* e = element(id);
        if (!e)
            return;
        e->SetInnerRML("");
        auto node = document->CreateTextNode(value);
        e->AppendChild(std::move(node));
    }
    void enabled(const char* id, bool value) {
        auto* e = element(id);
        if (!e)
            return;
        e->SetPseudoClass("disabled", !value);
        e->SetProperty("focus", value ? "auto" : "none");
        e->SetProperty("tab-index", value ? "auto" : "none");
        if (!value)
            e->Blur();
    }
    bool busy() const { return dialog || inspection.valid() || verification.valid(); }
    void status(const std::string& value, bool error = false) {
        text("status", value);
        auto* e = element("status");
        if (e)
            e->SetClass("error", error);
        // The disc panel is hidden behind the settings; say it there too.
        if (settings)
            note(value);
    }
    void save() {
        std::string error;
        if (!launcher::save_preferences(config_path, prefs, error))
            status(error, true);
    }
    std::vector<std::string> page_focus() const {
        switch (tab) {
        case 0:
            return {"display", "sync", "resolution", "aspect", "hud-mode", "aa", "filter",
                "filter-mode", "fps", "custom-textures", "backend", "performance"};
        case 1:
            return {"volume", "music-volume", "sfx-volume", "mute", "reverb"};
        case 2:
            return {"unlock-all", "frozen-stadium", "free-camera", "ucf"};
        case 3:
            return {"dsu"};
        case 4:
            return {"net-name", "net-target", "net-delay"};
        case 5:
            return {"scale", "check-updates", "check-now", "settings-discord"};
        default:
            return {};
        }
    }
    static bool shown(Rml::Element* e) { return e && e->IsVisible(true); }
    static bool typing(Rml::Element* e) {
        return e && e->GetTagName() == "input" && !is_slider(e->GetId());
    }
    bool within(Rml::Element* e, const char* id) const {
        for (; e && e != document; e = e->GetParentNode())
            if (e->GetId() == id)
                return true;
        return false;
    }
    bool in_rows(Rml::Element* e) const { return within(e, "settings-content"); }
    /* What Up and Down walk through: the main menu's plates, or in the
     * settings whichever column holds the focus, the categories or the rows
     * of the open page. */
    std::vector<std::string> column() {
        if (!settings)
            return focus_ids;
        if (in_rows(document->GetContext()->GetFocusElement()))
            return page_focus();
        std::vector<std::string> ids(std::begin(tab_ids), std::end(tab_ids));
        ids.push_back("back");
        return ids;
    }
    void set_theme(const char* name) {
        for (const char* t : main_ids)
            document->SetClass(std::string("theme-") + t, std::strcmp(t, name) == 0);
    }
    /* The description bar says what the focused thing does, as Melee's does,
     * and its row lights up. */
    void describe(Rml::Element* e) {
        Rml::Element* row = nullptr;
        Rml::String desc;
        for (auto* p = e; p && p != document; p = p->GetParentNode()) {
            if (!row && p->IsClassSet("row"))
                row = p;
            if (desc.empty())
                desc = p->GetAttribute<Rml::String>("data-desc", "");
        }
        if (row != current_row) {
            if (current_row)
                current_row->SetClass("current", false);
            if (row)
                row->SetClass("current", true);
            current_row = row;
        }
        if (!desc.empty())
            text("desc", desc);
        if (!e)
            return;
        // Only one plate lit: the open page's gives way to Back's.
        document->SetClass("on-back", e->GetId() == "back");
        if (!settings) {
            for (const char* t : main_ids)
                if (e->GetId() == t)
                    set_theme(t);
        } else if (const int i = index_of(tab_ids, e->GetId()); i >= 0 && i != tab)
            select_tab(i);  // the cursor on a category opens it, as Melee previews
    }
    /* A pointer moving onto a control moves the cursor there, so the mouse,
     * the Quest's laser and the pad share one highlight. Only a move: when the
     * layout shifts under a resting pointer (a fullscreen toggle, a page
     * change) RmlUi hovers whatever is now beneath it, and taking that as
     * pointing pulled the focus away from the keys. Not while a text field
     * holds the focus either: that would pull it out from under the typing.
     * Not onto the settings categories: focusing one opens its page, so the
     * pointer crossing them on its way to the rows flipped pages. They only
     * light up under it (launcher.rcss), and a click opens one. */
    void hover(Rml::Event& event) {
        const int x = event.GetParameter<int>("mouse_x", -1);
        const int y = event.GetParameter<int>("mouse_y", -1);
        if (x == pointer_x && y == pointer_y)
            return;
        pointer_x = x;
        pointer_y = y;
        auto* e = event.GetTargetElement();
        while (e && e != document && e->GetTagName() != "button" &&
               !(e->GetTagName() == "input" && is_slider(e->GetId())))
            e = e->GetParentNode();
        if (!e || e == document || e->IsPseudoClassSet("disabled"))
            return;
        if (settings && within(e, "categories"))
            return;
        auto* focus = document->GetContext()->GetFocusElement();
        if (e != focus && !typing(focus))
            e->Focus();
    }
    void controls() {
        enabled("play", supported && !busy());
        enabled("choose", !busy());
        enabled("verify", verification.valid() || (supported && !busy()));
        enabled("settings", !busy());
        enabled("discord", !busy());
        text("verify-label", verification.valid() ? "Cancel Verify" : "Verify Disc");
        focus_ids.assign(std::begin(main_ids), std::end(main_ids));
        auto ustate = pc::updater::get_state();
        if ((ustate.status == pc::updater::Status::UpdateAvailable ||
                ustate.status == pc::updater::Status::Downloaded) &&
            !update_card_dismissed)
        {
            focus_ids.insert(
                focus_ids.begin(), {"update-action", "update-browser", "update-later"});
        }
    }
    // Opens a page without moving the focus (describe() calls it whenever a
    // category takes the focus, by keys or by pointer).
    void select_tab(int next) {
        const int count = std::size(tab_ids);
        tab = (next + count) % count;
        for (int i = 0; i < count; ++i) {
            if (auto* e = element(tab_ids[i]))
                e->SetClass("selected", i == tab);
            const auto page_id = "page-" + Rml::String(tab_ids[i]).substr(4);
            if (auto* e = element(page_id.c_str()))
                e->SetClass("selected", i == tab);
        }
        if (auto* e = element("settings-content"))
            e->SetScrollTop(0);
    }
    void show_tab(int next) {
        select_tab(next);
        if (auto* e = element(tab_ids[tab])) {
            e->Focus();
            e->ScrollIntoView();
        }
    }
    // Into the open page's first row (Right or A on a category).
    void enter_rows() {
        document->UpdateDocument();  // the page's display just changed
        for (const auto& id : page_focus()) {
            auto* e = element(id.c_str());
            if (shown(e) && !e->IsPseudoClassSet("disabled")) {
                e->Focus();
                e->ScrollIntoView();
                return;
            }
        }
    }
    // B or Escape: rows to categories, categories to the main menu.
    void back() {
        if (!settings) {
            if (verification.valid())
                cancel = true;
        } else if (in_rows(document->GetContext()->GetFocusElement()))
            show_tab(tab);
        else
            show_settings(false);
    }
    void focus_step(int direction) {
        auto* focus = document->GetContext()->GetFocusElement();
        const auto ids = column();
        const int count = static_cast<int>(ids.size());
        int index = direction > 0 ? -1 : 0;
        for (int i = 0; i < count; ++i)
            if (element(ids[i].c_str()) == focus)
                index = i;
        for (int i = 0; i < count; ++i) {
            index = (index + direction + count) % count;
            auto* e = element(ids[index].c_str());
            if (shown(e) && !e->IsPseudoClassSet("disabled")) {
                e->Focus();
                e->ScrollIntoView();
                break;
            }
        }
    }
    void activate_focus() {
        auto* e = document->GetContext()->GetFocusElement();
        if (!e || e == document || e->IsPseudoClassSet("disabled"))
            return;
        const auto id = e->GetId();
        if (settings && index_of(tab_ids, id) >= 0) {
            show_tab(index_of(tab_ids, id));
            enter_rows();
        } else if (e->GetTagName() == "button" || is_slider(id))
            action(id);
    }
    /* Left and Right: Melee's arrows. They step a value either way, move a
     * slider, or cross between the categories and the rows. */
    void horizontal(int direction) {
        auto* focus = document->GetContext()->GetFocusElement();
        if (!focus || focus->IsPseudoClassSet("disabled"))
            return;
        const auto id = focus->GetId();
        if (settings && index_of(tab_ids, id) >= 0) {
            if (direction > 0)
                enter_rows();
        } else if (is_slider(id))
            step_slider(direction);
        else if (focus->IsClassSet("value") && !focus->IsClassSet("action")) {
            if (!cycle(id, direction))
                action(id);  // two states: either arrow flips it
        } else if (settings && direction < 0)
            show_tab(tab);
    }
    /* The settings with more than two values, stepped either way. */
    bool cycle(const std::string& id, int direction) {
        auto wrap = [](int value, int count) { return (value % count + count) % count; };
        if (id == "backend") {
            action(id, direction);
            return true;
        } else if (id == "aspect")
            prefs.widescreen = wrap(prefs.widescreen + direction, 3);
        else if (id == "filter") {
            int power = 0;
            while (power < 4 && (1 << power) < prefs.anisotropy)
                ++power;
            prefs.anisotropy = 1 << wrap(power + direction, 5);
        } else if (id == "filter-mode") {
            prefs.filter_mode = wrap(prefs.filter_mode + direction, 4);
            aurora_set_resampler(static_cast<AuroraSampler>(prefs.filter_mode));
        } else if (id == "net-delay")
            prefs.net_delay = wrap(prefs.net_delay + 1 + direction, 6) - 1;  // Auto, 0-4
        else
            return false;
        save();
        refresh_settings();
        return true;
    }
    void slider(const char* id, float value) {
        auto* e = document->GetElementById(id);
        if (e)
            e->SetAttribute("value", value);
    }
    static bool is_slider(const Rml::String& id) {
        return id == "volume" || id == "resolution" || id == "scale" || id == "music-volume" ||
               id == "sfx-volume";
    }
    // Writing a slider's value fires Change; `quiet` keeps that from looping
    // back into another save.
    void refresh_settings() {
        if (!settings)
            return;
        quiet = true;
        refresh_online(document);
        text("display", prefs.fullscreen ? "Fullscreen" : "Windowed");
        const char* override = std::getenv("MELEE_VSYNC");
        const bool effective_vsync = override ? override[0] != '0' : prefs.vsync;
        text("sync",
            std::string(effective_vsync ? "On" : "Off") + (override ? " (environment)" : ""));
        slider("resolution", prefs.render_scale);
        text("resolution-val", resolution_name(prefs.render_scale));
        text("aspect", prefs.widescreen == 0 ? "Original (73:60)" :
                       prefs.widescreen == 1 ? "Widescreen 16:9" :
                                               "Auto (window aspect)");
        text("aa", prefs.msaa == 1 ? "Off" : std::to_string(prefs.msaa) + "x MSAA");
        text("filter", std::to_string(prefs.anisotropy) + "x");
        text("filter-mode", filter_name(prefs.filter_mode));
        text("custom-textures", prefs.custom_textures ? "Enabled" : "Disabled");
        text("hud-mode", prefs.hud_mode == 0 ? "Classic (4:3)" : "Wide (16:9)");
        text("frozen-stadium", prefs.frozen_stadium ? "Hazardless" : "Normal");
        text("free-camera", prefs.free_camera ? "Free" : "Normal");
        text("ucf", std::getenv("MELEE_UCF") ? "Environment override" : prefs.ucf ? "On" : "Off");
        text("dsu", dsu_label());
        text("unlock-all", prefs.unlock_all ? "Unlocked" : "Normal");
        text("backend", backend_name(prefs.backend));
        slider("volume", prefs.volume * 100.0f);
        text("volume-val", std::to_string(int(prefs.volume * 100 + 0.5f)) + "%");
        slider("music-volume", prefs.music_volume * 100.0f);
        text("music-volume-val", std::to_string(int(prefs.music_volume * 100 + 0.5f)) + "%");
        slider("sfx-volume", prefs.sfx_volume * 100.0f);
        text("sfx-volume-val", std::to_string(int(prefs.sfx_volume * 100 + 0.5f)) + "%");
        text("mute", prefs.mute ? "On" : "Off");
        text("reverb", prefs.reverb ? "On" : "Off");
        text("fps", prefs.fps ? "On" : "Off");
        slider("scale", prefs.scale * 100.0f);
        text("scale-val", std::to_string(int(prefs.scale * 100 + 0.5f)) + "%");
        text("check-updates", prefs.check_updates ? "On" : "Off");
        auto ustate = pc::updater::get_state();
        if (ustate.status == pc::updater::Status::UpdateAvailable) {
            text("check-status", "Update available: " + ustate.latest_release.tag_name);
        } else if (ustate.status == pc::updater::Status::Checking) {
            text("check-status", "Checking for updates...");
        } else if (ustate.status == pc::updater::Status::Failed) {
            text("check-status", ustate.message);
        } else {
            text("check-status", "Melee-PC is up to date (" + pc::get_app_version() + ").");
        }
        notice();
        quiet = false;
    }
    // The line under the settings rows, shown only while it has something
    // to say (settings save as they change, so that goes without saying).
    void note(const std::string& value) {
        text("settings-status", value);
        if (auto* e = element("settings-status"))
            e->SetProperty("display", value.empty() ? "none" : "block");
    }
    void notice() {
        note(prefs.render_scale >= 6 ?
                 resolution_name(prefs.render_scale) +
                     " native needs a lot of video memory; drop it if the game stutters." :
                 std::string());
    }
    void show_settings(bool show) {
        settings = show;
        document->SetClass("in-settings", show);
        refresh_settings();
        if (show) {
            set_theme("settings");
            show_tab(tab);
            return;
        }
        controls();
        element("settings")->Focus();
    }
    // Left/right on a slider.
    void step_slider(int direction) {
        auto* focus = document->GetContext()->GetFocusElement();
        if (!focus)
            return;
        auto id = focus->GetId();
        if (id == "volume")
            prefs.volume = std::clamp(prefs.volume + direction * 0.05f, 0.0f, 1.0f);
        else if (id == "music-volume")
            prefs.music_volume = std::clamp(prefs.music_volume + direction * 0.05f, 0.0f, 1.0f);
        else if (id == "sfx-volume")
            prefs.sfx_volume = std::clamp(prefs.sfx_volume + direction * 0.05f, 0.0f, 1.0f);
        else if (id == "resolution")
            prefs.render_scale = std::clamp(prefs.render_scale + direction, 0.0f, 10.0f);
        else if (id == "scale") {
            prefs.scale = std::clamp(prefs.scale + direction * 0.25f, 0.75f, 1.5f);
            aurora::rmlui::set_ui_scale(prefs.scale);
        } else
            return;
        refresh_settings();
        save();
    }
    /* Open the selected disc and leave for the game. False when the disc will
     * not open, in which case the status line says why and we stay here. */
    bool begin_game() {
        auto check = launcher::inspect_disc(prefs.disc);
        if (!check.supported) {
            supported = false;
            status(check.message, true);
            controls();
            return false;
        }
        if (!aurora_dvd_open(prefs.disc.c_str())) {
            aurora_dvd_close();
            status("Could not load this disc. Choose another image or verify it.", true);
            return false;
        }
        pc_load_disc_fonts(prefs.disc.c_str());
        save();
        result = 1;
        return true;
    }
    void inspect(const std::string& path) {
        pending_path =
            (path.rfind("content://", 0) == 0) ? path : std::filesystem::absolute(path).string();
        status("Checking disc image...");
        inspection =
            std::async(std::launch::async, [path] { return launcher::inspect_disc(path); });
        controls();
    }
    void action(const std::string& id, int direction = 1) {
        if (id == "net-delay" || id == "aspect" || id == "filter" || id == "filter-mode") {
            cycle(id, 1);
            return;
        }
        if (id == "quit") {
            result = 0;
            cancel = true;
            return;
        }
        if (id == "discord" || id == "settings-discord") {
            status("Opening Discord in browser...");
            SDL_OpenURL("https://discord.gg/aurt34svq");
            return;
        }
        if (index_of(tab_ids, id) >= 0) {
            show_tab(index_of(tab_ids, id));
            return;
        }
        if (id == "verify" && verification.valid()) {
            cancel = true;
            status("Canceling verification...");
            return;
        }
        if (busy())
            return;
        if (id == "choose") {
            static const SDL_DialogFileFilter filters[] = {
                {"GameCube disc images", "iso;gcm;ciso;rvz;gcz;wia"}, {"All files", "*"}};
            dialog = std::make_shared<DialogResult>();
#if defined(__APPLE__) && TARGET_OS_IPHONE
            ios_show_open_file_dialog(
                dialog_done, new std::shared_ptr<DialogResult>(dialog), window);
#else
            SDL_ShowOpenFileDialog(dialog_done, new std::shared_ptr<DialogResult>(dialog), window,
                filters, 2, prefs.disc.empty() ? nullptr : prefs.disc.c_str(), false);
#endif
            controls();
        } else if (id == "play" && supported) {
            /* The pipeline queue may still be draining. Take the wait here,
             * where the player is already looking at a screen, rather than
             * during the first match; pressing Play again skips it. */
            if (const uint32_t pending = pending_pipelines(); !shaders_waited && pending > 0) {
                SDL_Log("Launcher: Play waits for %u pipelines", pending);
                shaders_waited = true;
                last_pending = 0;
                return;
            }
            SDL_Log("Launcher: starting the game");
            begin_game();
        } else if (id == "verify" && supported) {
            cancel = false;
            progress = 0;
            last_progress = 101;
            verification_identity = identity(prefs.disc);
            auto path = prefs.disc;
            verification = std::async(std::launch::async,
                [this, path] { return launcher::verify_disc(path, cancel, progress); });
            controls();
            element("verify")->Focus();
        } else if (id == "settings" || id == "back")
            show_settings(id == "settings");
        else if (id == "display" || id == "fullscreen") {
            if (SDL_SetWindowFullscreen(window, !prefs.fullscreen))
                prefs.fullscreen = !prefs.fullscreen;
            else
                status(std::string("Could not change display mode: ") + SDL_GetError(), true);
            save();
            refresh_settings();
            element("display")->Focus();
        } else if (id == "sync" || id == "vsync") {
            if (!std::getenv("MELEE_VSYNC")) {
                prefs.vsync = !prefs.vsync;
                aurora_enable_vsync(prefs.vsync);
                save();
            }
            refresh_settings();
            if (std::getenv("MELEE_VSYNC"))
                status("VSync is controlled by the MELEE_VSYNC environment setting.");
            element("sync")->Focus();
        } else if (id == "resolution") {
            prefs.render_scale = prefs.render_scale >= 10 ? 0 : int(prefs.render_scale) + 1;
            save();
            refresh_settings();
            element("resolution")->Focus();
        } else if (id == "aa") {
            prefs.msaa = prefs.msaa == 1 ? 4 : 1;
            save();
            refresh_settings();
            element("aa")->Focus();
        } else if (id == "backend") {
#if defined(__APPLE__)
            status("Graphics backend is Metal (hardware accelerated).");
#else
            if (std::getenv("MELEE_BACKEND")) {
                status("Backend is controlled by the MELEE_BACKEND environment setting.");
            } else {
                prefs.backend = backend_next(prefs.backend, direction);
                save();
                refresh_settings();
                status("Graphics backend set to " + std::string(backend_name(prefs.backend)) +
                       " (takes effect on restart).");
            }
#endif
            element("backend")->Focus();
        } else if (id == "custom-textures") {
            prefs.custom_textures = !prefs.custom_textures;
            save();
            refresh_settings();
            element("custom-textures")->Focus();
        } else if (id == "hud-mode") {
            prefs.hud_mode = (prefs.hud_mode + 1) % 2;
            save();
            refresh_settings();
            element("hud-mode")->Focus();
        } else if (id == "frozen-stadium") {
            prefs.frozen_stadium = !prefs.frozen_stadium;
            save();
            refresh_settings();
            element("frozen-stadium")->Focus();
        } else if (id == "free-camera") {
            prefs.free_camera = !prefs.free_camera;
            save();
            refresh_settings();
            element("free-camera")->Focus();
        } else if (id == "ucf") {
            prefs.ucf = !prefs.ucf;
            save();
            refresh_settings();
            element("ucf")->Focus();
        } else if (id == "dsu") {
            prefs.dsu = !prefs.dsu;
            pc_dsu_set_enabled(prefs.dsu);
            save();
            refresh_settings();
            element("dsu")->Focus();
        } else if (id == "unlock-all") {
            prefs.unlock_all = !prefs.unlock_all;
            save();
            refresh_settings();
            element("unlock-all")->Focus();
        } else if (id == "volume") {
            int step = int(prefs.volume * 10 + 0.5f);
            prefs.volume = (step >= 10 ? 0 : step + 1) / 10.0f;
            save();
            refresh_settings();
            element("volume")->Focus();
        } else if (id == "music-volume") {
            int step = int(prefs.music_volume * 10 + 0.5f);
            prefs.music_volume = (step >= 10 ? 0 : step + 1) / 10.0f;
            save();
            refresh_settings();
            element("music-volume")->Focus();
        } else if (id == "sfx-volume") {
            int step = int(prefs.sfx_volume * 10 + 0.5f);
            prefs.sfx_volume = (step >= 10 ? 0 : step + 1) / 10.0f;
            save();
            refresh_settings();
            element("sfx-volume")->Focus();
        } else if (id == "mute") {
            prefs.mute = !prefs.mute;
            save();
            refresh_settings();
            element("mute")->Focus();
        } else if (id == "reverb") {
            prefs.reverb = !prefs.reverb;
            save();
            refresh_settings();
            element("reverb")->Focus();
        } else if (id == "performance") {
            // The cheapest settings for a slow GPU or CPU, in one press.
            prefs.render_scale = 1;
            prefs.msaa = 1;
            prefs.anisotropy = 1;
            prefs.reverb = false;
            save();
            refresh_settings();
            note("Performance preset: internal resolution 1x, anti-aliasing off, filtering 1x "
                 "and reverb off. Anti-aliasing and filtering apply after a restart.");
            element("performance")->Focus();
        } else if (id == "fps") {
            prefs.fps = !prefs.fps;
            save();
            refresh_settings();
            element("fps")->Focus();
        } else if (id == "scale") {
            prefs.scale = prefs.scale >= 1.5f ? 0.75f : prefs.scale + 0.25f;
            aurora::rmlui::set_ui_scale(prefs.scale);
            save();
            refresh_settings();
            element("scale")->Focus();
        } else if (id == "check-updates") {
            prefs.check_updates = !prefs.check_updates;
            save();
            refresh_settings();
            element("check-updates")->Focus();
        } else if (id == "check-now") {
            pc::updater::check_for_updates_async(true);
            status("Checking for updates...");
            text("check-status", "Checking for updates...");
            element("check-now")->Focus();
        } else if (id == "update-action") {
            auto ustate = pc::updater::get_state();
            if (ustate.status == pc::updater::Status::Downloaded) {
                std::string err;
                if (!pc::updater::apply_update_and_restart(err)) {
                    status(err, true);
                }
            } else {
                pc::updater::start_download_async();
            }
        } else if (id == "update-browser") {
            pc::updater::open_release_in_browser();
        } else if (id == "update-later") {
            update_card_dismissed = true;
            if (auto* e = element("update-card"))
                e->SetProperty("display", "none");
            controls();
            element("play")->Focus();
        }
    }

public:
    Launcher(SDL_Window* w, Rml::ElementDocument* d) : window(w), document(d) {
        document->AddEventListener(Rml::EventId::Click, this);
        document->AddEventListener(Rml::EventId::Keydown, this);
        document->AddEventListener(Rml::EventId::Change, this);
        // Focus does not bubble; the capture pass still reaches the document.
        document->AddEventListener(Rml::EventId::Focus, this, true);
        document->AddEventListener(Rml::EventId::Mousemove, this);
        /* MELEE_LAUNCHER_PLATFORM=mobile lays the launcher out as
         * another platform does, to check a phone's from a desktop. Only the
         * layout follows it. */
        const char* platform = std::getenv("MELEE_LAUNCHER_PLATFORM");
        if (is_mobile || (platform && std::strcmp(platform, "mobile") == 0))
            document->SetClass("mobile", true);
        document->Show();
        text("app-version", pc::get_app_version());
        text("check-status", "Current version: " + pc::get_app_version());
        text("disc-note", disc_note);
        if (prefs.check_updates) {
            pc::updater::check_for_updates_async(true);
        }
        controls();
        element("choose")->Focus();
    }
    ~Launcher() override {
        cancel = true;
        pc::updater::cancel();
        if (verification.valid())
            verification.wait();
        if (inspection.valid())
            inspection.wait();
        document->RemoveEventListener(Rml::EventId::Click, this);
        document->RemoveEventListener(Rml::EventId::Keydown, this);
        document->RemoveEventListener(Rml::EventId::Change, this);
        document->RemoveEventListener(Rml::EventId::Focus, this, true);
        document->RemoveEventListener(Rml::EventId::Mousemove, this);
        auto* context = document->GetContext();
        document->Close();
        context->Update();
    }
    void ProcessEvent(Rml::Event& event) override {
        if (event.GetId() == Rml::EventId::Focus) {
            describe(event.GetTargetElement());
        } else if (event.GetId() == Rml::EventId::Mousemove) {
            hover(event);
        } else if (event.GetId() == Rml::EventId::Click) {
            auto* target = event.GetTargetElement();
            while (target && target != document) {
                if (target->GetTagName() == "button") {
                    if (!target->IsPseudoClassSet("disabled"))
                        action(target->GetId());
                    break;
                }
                target = target->GetParentNode();
            }
        } else if (event.GetId() == Rml::EventId::Change) {
            auto* target = event.GetTargetElement();
            if (!target || quiet)
                return;
            if (change_online(event)) {
                save();
                return;
            }
            auto id = target->GetId();
            if (id == "volume") {
                prefs.volume =
                    std::clamp(event.GetParameter<float>("value", 100.0f) / 100.0f, 0.0f, 1.0f);
                text("volume-val", std::to_string(int(prefs.volume * 100 + 0.5f)) + "%");
            } else if (id == "music-volume") {
                prefs.music_volume =
                    std::clamp(event.GetParameter<float>("value", 100.0f) / 100.0f, 0.0f, 1.0f);
                text(
                    "music-volume-val", std::to_string(int(prefs.music_volume * 100 + 0.5f)) + "%");
            } else if (id == "sfx-volume") {
                prefs.sfx_volume =
                    std::clamp(event.GetParameter<float>("value", 100.0f) / 100.0f, 0.0f, 1.0f);
                text("sfx-volume-val", std::to_string(int(prefs.sfx_volume * 100 + 0.5f)) + "%");
            } else if (id == "resolution") {
                prefs.render_scale = std::clamp(
                    float(int(event.GetParameter<float>("value", 0.0f) + 0.5f)), 0.0f, 10.0f);
                text("resolution-val", resolution_name(prefs.render_scale));
                notice();
            } else if (id == "scale") {
                prefs.scale =
                    std::clamp(event.GetParameter<float>("value", 100.0f) / 100.0f, 0.75f, 1.5f);
                text("scale-val", std::to_string(int(prefs.scale * 100 + 0.5f)) + "%");
                aurora::rmlui::set_ui_scale(prefs.scale);
            } else
                return;
            save();
        } else if (event.GetId() == Rml::EventId::Keydown) {
            auto key = event.GetParameter<int>("key_identifier", 0);
            if (key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN) {
                focus_step(key == Rml::Input::KI_UP ? -1 : 1);
                event.StopPropagation();
            } else if (key == Rml::Input::KI_RETURN) {
                // Stopping it here also stops RmlUi's own click on Enter.
                auto* focus = document->GetContext()->GetFocusElement();
                if (focus && (is_slider(focus->GetId()) ||
                                 (settings && index_of(tab_ids, focus->GetId()) >= 0)))
                {
                    activate_focus();
                    event.StopPropagation();
                }
            } else if (key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT) {
                // A text field keeps its arrows for the caret.
                if (!typing(document->GetContext()->GetFocusElement())) {
                    horizontal(key == Rml::Input::KI_LEFT ? -1 : 1);
                    event.StopPropagation();
                }
            } else if (key == Rml::Input::KI_ESCAPE) {
                back();
                event.StopPropagation();
            }
        }
    }
    int run(const std::string& initial_error) {
        if (!prefs.disc.empty())
            inspect(prefs.disc);
        if (!initial_error.empty())
            status(initial_error, true);
        while (result == -2) {
            if (pc_exit_requested) { /* SIGINT/SIGTERM, src/pc/main.c */
                result = 0;
                cancel = true;
                break;
            }
            auto* events = aurora_update();
            for (auto* e = events; e && e->type != AURORA_NONE; ++e) {
                if (e->type == AURORA_EXIT) {
                    result = 0;
                    cancel = true;
                }
                if (e->type != AURORA_SDL_EVENT)
                    continue;
                if (e->sdl.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    auto button = e->sdl.gbutton.button;
                    if (button == SDL_GAMEPAD_BUTTON_DPAD_DOWN)
                        focus_step(1);
                    else if (button == SDL_GAMEPAD_BUTTON_DPAD_UP)
                        focus_step(-1);
                    else if (button == SDL_GAMEPAD_BUTTON_DPAD_LEFT)
                        horizontal(-1);
                    else if (button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT)
                        horizontal(1);
                    else if (button == SDL_GAMEPAD_BUTTON_SOUTH ||
                             button == SDL_GAMEPAD_BUTTON_START)
                        activate_focus();
                    else if (button == SDL_GAMEPAD_BUTTON_EAST)
                        back();
                } else if (e->sdl.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                    if (e->sdl.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
                        auto value = e->sdl.gaxis.value;
                        auto now = SDL_GetTicks();
                        if (std::abs(value) > 18000 && now - last_axis > 200) {
                            focus_step(value > 0 ? 1 : -1);
                            last_axis = now;
                        }
                    } else if (e->sdl.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
                        auto value = e->sdl.gaxis.value;
                        auto now = SDL_GetTicks();
                        if (std::abs(value) > 18000 && now - last_axis_x > 150) {
                            horizontal(value > 0 ? 1 : -1);
                            last_axis_x = now;
                        }
                    }
                } else if (e->sdl.type == SDL_EVENT_DROP_FILE && !busy() && e->sdl.drop.data) {
                    inspect(e->sdl.drop.data);
                }
            }
            if (dialog) {
                bool ready;
                std::string path, error;
                {
                    std::lock_guard lock(dialog->mutex);
                    ready = dialog->ready;
                    path = dialog->path;
                    error = dialog->error;
                }
                if (ready) {
                    dialog.reset();
                    if (!error.empty())
                        status("File chooser failed: " + error, true);
                    else if (!path.empty())
                        inspect(path);
                    controls();
                    element("choose")->Focus();
                }
            }
            if (inspection.valid() &&
                inspection.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                auto info = inspection.get();
                if (info.supported) {
                    prefs.disc = pending_path;
                    supported = true;
                    selected_identity = identity(prefs.disc);
                    std::string display_name;
                    if (prefs.disc.rfind("content://", 0) == 0) {
                        auto last_slash = prefs.disc.find_last_of('/');
                        display_name = (last_slash != std::string::npos) ?
                                           prefs.disc.substr(last_slash + 1) :
                                           prefs.disc;
                        if (auto col = display_name.find_last_of("%3A"); col != std::string::npos)
                            display_name = display_name.substr(col + 1);
                        if (auto col = display_name.find_last_of(':'); col != std::string::npos)
                            display_name = display_name.substr(col + 1);
                    } else {
                        display_name = std::filesystem::path(prefs.disc).filename().string();
                    }
                    text("disc-name", display_name);
                    text("disc-path", prefs.disc);
                    text("disc-info", info.message);
                    status("Ready to play / Disc not verified.");
                    save();
                } else {
                    if (prefs.disc == pending_path) {
                        supported = false;
                        prefs.disc.clear();
                        save();
                    }
                    status(info.message, true);
                }
                controls();
                element(supported ? "play" : "choose")->Focus();
            }
            if (verification.valid()) {
                auto percent = progress.load();
                if (percent != last_progress && !cancel) {
                    status("Verifying disc / " + std::to_string(percent) + "%");
                    last_progress = percent;
                }
                if (verification.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    auto checked = verification.get();
                    if (checked.state == launcher::VerifyState::Error)
                        supported = false;
                    if (identity(prefs.disc) != verification_identity)
                        status("Disc changed during verification. Verify it again.", true);
                    else
                        status(checked.message, checked.state == launcher::VerifyState::Mismatch ||
                                                    checked.state == launcher::VerifyState::Error);
                    controls();
                    element("verify")->Focus();
                }
            }
            if (supported && !busy() && SDL_GetTicks() - last_identity_check > 1000) {
                last_identity_check = SDL_GetTicks();
                if (identity(prefs.disc) != selected_identity) {
                    supported = false;
                    status("Disc changed or was removed. Choose the image again.", true);
                    controls();
                }
            }
            auto ustate = pc::updater::get_state();
            if (ustate.status != last_updater_status ||
                std::abs(ustate.download_progress - last_download_progress) > 0.01f)
            {
                last_updater_status = ustate.status;
                last_download_progress = ustate.download_progress;

                if (auto* card = element("update-card")) {
                    if (ustate.status == pc::updater::Status::UpdateAvailable &&
                        !update_card_dismissed && !settings)
                    {
                        card->SetProperty("display", "block");
                        text("update-tag", ustate.latest_release.tag_name);
                        text("update-title", ustate.latest_release.name.empty() ?
                                                 ustate.latest_release.tag_name :
                                                 ustate.latest_release.name);
                        std::string desc = "A newer version of Melee PC is available (" +
                                           ustate.latest_release.tag_name + ").";
#if defined(_WIN32)
                        desc += " Download and extract the Windows ZIP from the release page.";
                        text("update-action", "Open release page");
#else
                        if (!ustate.target_asset_name.empty()) {
                            desc += " Ready to download: " + ustate.target_asset_name;
                        }
                        text("update-action", "Update Now");
#endif
                        text("update-desc", desc);
                        enabled("update-action", true);
                        if (auto* prog = element("update-progress-row"))
                            prog->SetProperty("display", "none");
                        controls();
                    } else if (ustate.status == pc::updater::Status::Downloading) {
                        card->SetProperty("display", "block");
                        text("update-title", "Downloading Update...");
                        int pct = static_cast<int>(ustate.download_progress * 100.0f + 0.5f);
                        text("update-desc", "Downloading " + ustate.target_asset_name + " (" +
                                                std::to_string(pct) + "%)");
                        text("update-action", "Downloading...");
                        enabled("update-action", false);
                        if (auto* prog = element("update-progress-row"))
                            prog->SetProperty("display", "flex");
                        if (auto* fill = element("update-progress-fill"))
                            fill->SetProperty("width", std::to_string(pct) + "%");
                        text("update-progress-text", std::to_string(pct) + "%");
                    } else if (ustate.status == pc::updater::Status::Downloaded) {
                        card->SetProperty("display", "block");
                        text("update-title", "Update Ready!");
                        text("update-desc", ustate.restart_supported ?
                                                "Downloaded successfully. Click Restart to apply." :
                                                ("Saved to: " + ustate.downloaded_path));
                        text("update-action",
                            ustate.restart_supported ? "Restart to apply" : "Open folder");
                        enabled("update-action", true);
                        if (auto* prog = element("update-progress-row"))
                            prog->SetProperty("display", "none");
                        controls();
                    } else if (ustate.status == pc::updater::Status::Failed &&
                               last_download_progress > 0.0f)
                    {
                        text("update-title", "Download Failed");
                        text("update-desc", ustate.message);
                        text("update-action", "Retry");
                        enabled("update-action", true);
                    } else if (ustate.status != pc::updater::Status::UpdateAvailable &&
                               ustate.status != pc::updater::Status::Downloading &&
                               ustate.status != pc::updater::Status::Downloaded)
                    {
                        card->SetProperty("display", "none");
                    }
                }
                refresh_settings();
            }
            if (shaders_waited && result == -2) {
                const uint32_t pending = pending_pipelines();
                if (pending == 0) {
                    SDL_Log("Launcher: pipelines built, starting the game");
                    shaders_waited = false;
                    begin_game(); /* reports its own failure in the status line */
                } else if (pending != last_pending) {
                    last_pending = pending;
                    status("Compiling shaders (" + std::to_string(pending) +
                           " left). Press Play again to start now.");
                }
            }
            if (result != -2)
                break;
            if (aurora_begin_frame())
                aurora_end_frame();
            /* Spend the idle time until the next frame on the pipeline queue:
             * with worker threads this only waits, as a plain delay did;
             * without them (Android) it builds queued pipelines here, the only
             * place they are built before a match. */
            const uint64_t idle_start = SDL_GetTicks();
            aurora_wait_pipelines(8);
            const uint64_t idle = SDL_GetTicks() - idle_start;
            if (idle < 8)
                SDL_Delay(uint32_t(8 - idle));
        }
        return result;
    }
};
}  // namespace

extern "C" void pc_launcher_configure(AuroraConfig* config) {
    config_path = std::filesystem::path(config->userPath ? config->userPath : ".") / "launcher.cfg";
    prefs = launcher::load_preferences(config_path);
    pc_dsu_set_enabled(prefs.dsu);
    pc_net_set_input_delay(prefs.net_delay);
    if (prefs.install_id == 0) {
        prefs.install_id = std::random_device{}() | uint64_t(std::random_device{}()) << 32 | 1;
        std::string error;
        if (!launcher::save_preferences(config_path, prefs, error))
            SDL_Log("%s", error.c_str());
    }
#if defined(__APPLE__) || defined(TARGET_OS_IPHONE)
    auto file_accessible = [](const std::filesystem::path& path) -> bool {
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && !ec)
            return true;
        FILE* f = fopen(path.string().c_str(), "rb");
        if (f) {
            fclose(f);
            return true;
        }
        return false;
    };
    if (prefs.disc.empty() || !file_accessible(prefs.disc)) {
        const char* home = std::getenv("HOME");
        const char* candidates[] = {"Documents/melee.ciso", "Documents/melee.iso",
            "Documents/game.ciso", "Documents/game.iso", "melee.ciso", "melee.iso", nullptr};
        for (int p = 0; candidates[p] != nullptr; p++) {
            if (home && home[0] != '\0') {
                std::filesystem::path full = std::filesystem::path(home) / candidates[p];
                if (file_accessible(full)) {
                    prefs.disc = full.string();
                    break;
                }
            }
            if (file_accessible(candidates[p])) {
                prefs.disc = std::filesystem::absolute(candidates[p]).string();
                break;
            }
        }
        // If not found in standard candidate names, scan Documents directory for any disc
        if ((prefs.disc.empty() || !file_accessible(prefs.disc)) && home && home[0] != '\0') {
            std::filesystem::path docs = std::filesystem::path(home) / "Documents";
            std::error_code dir_ec;
            if (std::filesystem::exists(docs, dir_ec) &&
                std::filesystem::is_directory(docs, dir_ec))
            {
                for (const auto& entry : std::filesystem::directory_iterator(docs, dir_ec)) {
                    if (entry.is_regular_file(dir_ec)) {
                        auto ext = entry.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext == ".iso" || ext == ".ciso" || ext == ".rvz" || ext == ".gcm" ||
                            ext == ".gcz" || ext == ".wia")
                        {
                            auto info = launcher::inspect_disc(entry.path().string());
                            if (info.supported) {
                                prefs.disc = entry.path().string();
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
#endif
    config->startFullscreen = prefs.fullscreen;
    config->msaa = prefs.msaa;
    config->maxTextureAnisotropy = prefs.anisotropy;
    if (!std::getenv("MELEE_VSYNC"))
        config->vsync = prefs.vsync;
    if (!std::getenv("MELEE_BACKEND")) {
#if defined(__APPLE__)
        config->desiredBackend = BACKEND_METAL;
#else
        if (prefs.backend == 1)
            config->desiredBackend = BACKEND_D3D12;
        else if (prefs.backend == 2)
            config->desiredBackend = BACKEND_VULKAN;
        else if (prefs.backend == 3)
            config->desiredBackend = BACKEND_D3D11;
        else
            config->desiredBackend = BACKEND_AUTO;
#endif
    }
}

static std::filesystem::path pc_resources_path() {
#if defined(__ANDROID__)
    return "";
#elif defined(__APPLE__)
    const char* base = SDL_GetBasePath();
    std::filesystem::path p = std::filesystem::path(base ? base : ".");
    std::error_code ec;
    if (std::filesystem::exists(p / "resources", ec) && !ec)
        return p / "resources";
    return p;
#else
    const char* base = SDL_GetBasePath();
    return std::filesystem::path(base ? base : ".") / "resources";
#endif
}

extern "C" int pc_launcher_run(const char* command_line_disc, SDL_Window* window) {
    try {
        std::string error;
        if (command_line_disc) {
            auto info = launcher::inspect_disc(command_line_disc);
            if (info.supported && aurora_dvd_open(command_line_disc)) {
                pc_load_disc_fonts(command_line_disc);
                prefs.disc = (std::string(command_line_disc).rfind("content://", 0) == 0) ?
                                 command_line_disc :
                                 std::filesystem::absolute(command_line_disc).string();
                if (!launcher::save_preferences(config_path, prefs, error))
                    SDL_Log("%s", error.c_str());
                return 1;
            }
            aurora_dvd_close();
            error = info.supported ? "Could not load the requested disc. Choose another image." :
                                     info.message;
        }
        ensure_rmlui_input_layer();
        auto* context = aurora::rmlui::get_context();
        if (!context) {
            SDL_Log("Launcher: RmlUi context is unavailable.");
            return -1;
        }
        auto resources = pc_resources_path();
        if (!load_menu_fonts(resources))
            return -1;
        aurora::rmlui::set_ui_scale(prefs.scale);
        aurora_set_resampler(static_cast<AuroraSampler>(prefs.filter_mode));
        auto* document = context->LoadDocument((resources / "launcher.rml").string());
        if (!document) {
            SDL_Log("Launcher: could not load launcher.rml");
            return -1;
        }
        Launcher launcher(window, document);
        return launcher.run(error);
    } catch (const std::exception& e) {
        SDL_Log("Launcher failed: %s", e.what());
        return -1;
    }
}

// In-game settings use the same preferences and RmlUi context as the launcher.
#include <dolphin/vi.h>
#include <dolphin/pad.h>
extern "C" void pc_audio_set_volume(float volume);
extern "C" void pc_audio_set_music_volume(float volume);
extern "C" void pc_audio_set_sfx_volume(float volume);
extern "C" void pc_audio_set_reverb(bool on);
// Melee's own menu bank: 0 is back/cancel, 1 confirm, 2 the cursor tick.
extern "C" void lbAudioAx_80024030(int);
enum { SFX_BACK = 0, SFX_CONFIRM = 1, SFX_MOVE = 2 };
namespace {
class PortMenu final : public Rml::EventListener {
public:
    Rml::ElementDocument* document = nullptr;
    Rml::ElementDocument* counter = nullptr;
    SDL_Window* window = nullptr;
    bool open = false;
    // Set while the menu writes its own widgets: suppresses the Change and
    // Focus events that echo back, which would otherwise loop or chatter.
    bool quiet = false;
    uint64_t last_fps = 0, last_axis_y = 0, last_axis_x = 0;
    int tab = 0;
    u32 pad_port = 0;
    // -1 when idle, otherwise the mapping slot waiting for input. Axis slots
    // index the axis table instead of the button table.
    int binding = -1;
    bool binding_axis = false;
    // The press that opened the prompt has to be released first, or the A
    // that selected the row binds itself immediately.
    bool binding_armed = false;
    std::vector<std::string> focus_ids;
    Rml::Element* current_row = nullptr;
    // Resizing the framebuffer allocates a fresh, empty texture. While the
    // overlay pauses the game nothing redraws it, so the frozen frame behind
    // the menu would go black: hold the change until the menu closes.
    bool resize_pending = false;

    static bool is_slider(const Rml::String& id) {
        return id == "volume" || id == "resolution" || id == "scale" || id == "music-volume" ||
               id == "sfx-volume";
    }
    void label(const char* id, const std::string& value) {
        auto* e = document->GetElementById(id);
        if (!e)
            return;
        e->SetInnerRML("");
        e->AppendChild(document->CreateTextNode(value));
    }
    void slider(const char* id, float value) {
        auto* e = document->GetElementById(id);
        if (e)
            e->SetAttribute("value", value);
    }
    std::vector<std::string> page_focus() const {
        switch (tab) {
        case 0:
            return {"display", "sync", "resolution", "aspect", "hud-mode", "aa", "filter",
                "filter-mode", "custom-textures", "backend"};
        case 1:
            return {"volume", "music-volume", "sfx-volume", "mute", "reverb", "fps", "scale",
                "port-check-update"};
        case 2:
            return {"unlock-all", "frozen-stadium", "free-camera", "ucf"};
        case 4:
            return {"net-name", "net-target", "net-delay"};
        default: {
            std::vector<std::string> ids{"dsu", "pad-port"};
            for (int i = 0; i < PAD_BUTTON_COUNT; ++i)
                ids.push_back("bind-" + std::to_string(i));
            for (int i = 0; i < PAD_AXIS_COUNT; ++i)
                ids.push_back("axis-" + std::to_string(i));
            ids.push_back("bind-reset");
            return ids;
        }
        }
    }
    void show_tab(int next) {
        // The Launcher category only exists before starting the game.
        const int count = std::size(tab_ids) - 1;
        tab = (next + count) % count;
        for (int i = 0; i < count; ++i) {
            if (auto* e = document->GetElementById(tab_ids[i]))
                e->SetClass("selected", i == tab);
            const auto page_id = "page-" + Rml::String(tab_ids[i]).substr(4);
            if (auto* e = document->GetElementById(page_id))
                e->SetClass("selected", i == tab);
        }
        focus_ids = {tab_ids[tab]};
        for (const auto& id : page_focus())
            focus_ids.push_back(id);
        focus_ids.push_back("resume");
        quiet = true;
        if (auto* e = document->GetElementById(tab_ids[tab])) {
            e->Focus();
            e->ScrollIntoView();
        }
        quiet = false;
    }
    void enter_rows() {
        document->UpdateDocument();
        for (const auto& id : page_focus()) {
            auto* e = document->GetElementById(id);
            if (e && e->IsVisible(true)) {
                e->Focus();
                e->ScrollIntoView();
                return;
            }
        }
    }
    void back() {
        auto* focus = document->GetContext()->GetFocusElement();
        if (focus && index_of(tab_ids, focus->GetId()) < 0 && focus->GetId() != "resume")
            show_tab(tab);
        else
            toggle();
    }
    void focus_step(int direction) {
        auto* focus = document->GetContext()->GetFocusElement();
        const auto id = focus ? focus->GetId() : Rml::String();
        if (index_of(tab_ids, id) >= 0 || id == "resume") {
            const int count = std::size(tab_ids) - 1;
            const int current = id == "resume" ? count : tab;
            const int next = (current + direction + count + 1) % (count + 1);
            if (next < count)
                show_tab(next);
            else if (auto* e = document->GetElementById("resume"))
                e->Focus();
            return;
        }
        int index = direction > 0 ? -1 : 0;
        for (size_t i = 0; i < focus_ids.size(); ++i)
            if (document->GetElementById(focus_ids[i]) == focus)
                index = static_cast<int>(i);
        index = (index + direction + static_cast<int>(focus_ids.size())) %
                static_cast<int>(focus_ids.size());
        if (auto* e = document->GetElementById(focus_ids[index])) {
            e->Focus();
            e->ScrollIntoView();
        }
    }
    void activate_focus() {
        auto* focus = document->GetContext()->GetFocusElement();
        if (focus && index_of(tab_ids, focus->GetId()) >= 0)
            enter_rows();
        else if (focus)
            apply(focus->GetId());
    }
    bool is_back_bound() const {
        for (u32 p = 0; p < 4; ++p) {
            u32 count = 0;
            const PADButtonMapping* bmap = PADGetButtonMappings(p, &count);
            for (u32 i = 0; bmap && i < count; ++i) {
                if (bmap[i].nativeButton == SDL_GAMEPAD_BUTTON_BACK)
                    return true;
            }
            u32 axes = 0;
            const PADAxisMapping* amap = PADGetAxisMappings(p, &axes);
            for (u32 i = 0; amap && i < axes; ++i) {
                if (amap[i].nativeButton == SDL_GAMEPAD_BUTTON_BACK)
                    return true;
            }
        }
        return false;
    }
    void refresh_bindings() {
        label("pad-port", "Port " + std::to_string(pad_port + 1));
        const char* name = PADGetName(pad_port);
        label("pad-name", name && *name ? name : "No controller in this port.");
        u32 count = 0;
        const PADButtonMapping* map = PADGetButtonMappings(pad_port, &count);
        u32 axes = 0;
        const PADAxisMapping* amap = PADGetAxisMappings(pad_port, &axes);
        for (int i = 0; i < PAD_BUTTON_COUNT; ++i) {
            const auto row = std::to_string(i);
            const char* gc = map && u32(i) < count ? PADGetButtonName(map[i].padButton) : nullptr;
            const char* native =
                map && u32(i) < count && map[i].nativeButton != PAD_NATIVE_BUTTON_INVALID ?
                    PADGetNativeButtonName(map[i].nativeButton) :
                    nullptr;
            if (!native && map && u32(i) < count && amap &&
                (map[i].padButton == PAD_TRIGGER_L || map[i].padButton == PAD_TRIGGER_R))
            {
                const PADAxis triggerAxis =
                    map[i].padButton == PAD_TRIGGER_L ? PAD_AXIS_TRIGGER_L : PAD_AXIS_TRIGGER_R;
                for (u32 a = 0; a < axes; ++a) {
                    if (amap[a].padAxis == triggerAxis && amap[a].nativeAxis.nativeAxis >= 0) {
                        native = PADGetNativeAxisName(amap[a].nativeAxis);
                        break;
                    }
                }
            }
            label(("bind-name-" + row).c_str(), gc ? gc : "-");
            label(("bind-" + row).c_str(), binding == i && !binding_axis ? "Press a button..." :
                                           native                        ? native :
                                                                           "Unbound");
        }
        for (int i = 0; i < PAD_AXIS_COUNT; ++i) {
            const auto row = std::to_string(i);
            const char* gc = amap && u32(i) < axes ? PADGetAxisName(amap[i].padAxis) : nullptr;
            std::string bound = "Unbound";
            if (amap && u32(i) < axes) {
                if (amap[i].nativeAxis.nativeAxis >= 0) {
                    const char* a = PADGetNativeAxisName(amap[i].nativeAxis);
                    bound = std::string(a ? a : "axis") +
                            (amap[i].nativeAxis.sign == AXIS_SIGN_NEGATIVE ? " -" : " +");
                } else if (amap[i].nativeButton >= 0) {
                    const char* b = PADGetNativeButtonName(u32(amap[i].nativeButton));
                    if (b)
                        bound = b;
                }
            }
            label(("axis-name-" + row).c_str(), gc ? gc : "-");
            label(
                ("axis-" + row).c_str(), binding == i && binding_axis ? "Move a stick..." : bound);
        }
    }
    void refresh() {
        quiet = true;
        refresh_online(document);
        label("display", VIGetWindowFullscreen() ? "Fullscreen" : "Windowed");
        label("sync", std::getenv("MELEE_VSYNC") ? "Environment override" :
                      prefs.vsync                ? "On" :
                                                   "Off");
        slider("resolution", prefs.render_scale);
        label("resolution-val", resolution_name(prefs.render_scale));
        label("aspect", prefs.widescreen == 0 ? "Original (73:60)" :
                        prefs.widescreen == 1 ? "Widescreen 16:9" :
                                                "Auto (window aspect)");
        label("aa", prefs.msaa == 1 ? "Off" : std::to_string(prefs.msaa) + "x MSAA");
        label("filter", std::to_string(prefs.anisotropy) + "x");
        label("filter-mode", filter_name(prefs.filter_mode));
        label("custom-textures", prefs.custom_textures ? "Enabled" : "Disabled");
        label("hud-mode", prefs.hud_mode == 0 ? "Classic (4:3)" : "Wide (16:9)");
        label("frozen-stadium", prefs.frozen_stadium ? "Hazardless" : "Normal");
        label("free-camera", prefs.free_camera ? "Free" : "Normal");
        label("ucf", std::getenv("MELEE_UCF") ? "Environment override" : prefs.ucf ? "On" : "Off");
        label("dsu", dsu_label());
        label("unlock-all", prefs.unlock_all ? "Unlocked" : "Normal");
        label("backend", backend_name(prefs.backend));
        slider("volume", prefs.volume * 100.0f);
        label("volume-val", std::to_string(int(prefs.volume * 100 + 0.5f)) + "%");
        slider("music-volume", prefs.music_volume * 100.0f);
        label("music-volume-val", std::to_string(int(prefs.music_volume * 100 + 0.5f)) + "%");
        slider("sfx-volume", prefs.sfx_volume * 100.0f);
        label("sfx-volume-val", std::to_string(int(prefs.sfx_volume * 100 + 0.5f)) + "%");
        label("mute", prefs.mute ? "On" : "Off");
        label("reverb", prefs.reverb ? "On" : "Off");
        label("fps", prefs.fps ? "On" : "Off");
        slider("scale", prefs.scale * 100.0f);
        label("scale-val", std::to_string(int(prefs.scale * 100 + 0.5f)) + "%");
        label("menu-version", pc::get_app_version());
        auto ustate = pc::updater::get_state();
        if (ustate.status == pc::updater::Status::UpdateAvailable) {
            label("port-update-status", "Update available: " + ustate.latest_release.tag_name);
        } else if (ustate.status == pc::updater::Status::Checking) {
            label("port-update-status", "Checking for updates...");
        } else if (ustate.status == pc::updater::Status::Failed) {
            label("port-update-status", "Check failed");
        } else {
            label("port-update-status", "Up to date (" + pc::get_app_version() + ")");
        }
        refresh_bindings();
        quiet = false;
    }
    void saved() {
        std::string error;
        label("menu-status",
            launcher::save_preferences(config_path, prefs, error) ? "Saved." : error);
        refresh();
    }
    void toggle() {
        if (!document)
            return;
        open = !open;
        binding = -1;
        PADBlockInput(open);
        lbAudioAx_80024030(open ? SFX_CONFIRM : SFX_BACK);
        if (open) {
            refresh();
            document->Show();
            show_tab(tab);
        } else {
            document->Hide();
            // Both reallocate the framebuffer; each is a no-op when the value
            // has not actually moved.
            if (resize_pending) {
                pc_widescreen_set_mode(prefs.widescreen);
                VISetFrameBufferScale(prefs.render_scale);
                resize_pending = false;
            }
        }
    }
    void start_binding(int slot, bool axis) {
        u32 count = 0;
        if (!PADGetButtonMappings(pad_port, &count) || count == 0) {
            label("menu-status", "No controller in port " + std::to_string(pad_port + 1) + ".");
            return;
        }
        binding = slot;
        binding_axis = axis;
        binding_armed = false;
        label("menu-status", axis ? "Move a stick or press a button. Escape cancels." :
                                    "Press a button on the controller. Escape cancels.");
        refresh_bindings();
    }
    // Runs once a frame while a bind prompt is up.
    void poll_binding() {
        if (binding < 0)
            return;
        constexpr uint32_t kNativeButtonAxisBit = 0x40000000u;
        const int native = PADGetNativeButtonPressed(pad_port);
        const PADSignedNativeAxis pulled = PADGetNativeAxisPulled(pad_port);
        const bool triggerPulled = pulled.nativeAxis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                                   pulled.nativeAxis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        const PADSignedNativeAxis axis =
            (binding_axis || triggerPulled) ? pulled : PADSignedNativeAxis{-1, AXIS_SIGN_POSITIVE};
        if (!binding_armed) {
            binding_armed = native < 0 && axis.nativeAxis < 0;
            return;
        }
        if (native < 0 && axis.nativeAxis < 0)
            return;
        u32 count = 0;
        if (binding_axis) {
            const PADAxisMapping* map = PADGetAxisMappings(pad_port, &count);
            if (map && u32(binding) < count) {
                // One source or the other, never neither: aurora reads the
                // axis when nativeAxis is set and asserts on the button
                // otherwise.
                PADSetAxisMapping(pad_port,
                    axis.nativeAxis >= 0 ?
                        PADAxisMapping{axis, SDL_GAMEPAD_BUTTON_INVALID, map[binding].padAxis} :
                        PADAxisMapping{{-1, AXIS_SIGN_POSITIVE}, native, map[binding].padAxis});
                PADSerializeMappings();
                label("menu-status", "Bound.");
            }
        } else {
            const PADButtonMapping* map = PADGetButtonMappings(pad_port, &count);
            if (map && u32(binding) < count) {
                const u32 encoded =
                    native >= 0 ? u32(native) :
                                  (kNativeButtonAxisBit | static_cast<uint32_t>(axis.nativeAxis));
                PADSetButtonMapping(pad_port, {encoded, map[binding].padButton});
                if (native < 0 && axis.nativeAxis >= 0 &&
                    (map[binding].padButton == PAD_TRIGGER_L ||
                        map[binding].padButton == PAD_TRIGGER_R))
                {
                    const PADAxis triggerAxis = map[binding].padButton == PAD_TRIGGER_L ?
                                                    PAD_AXIS_TRIGGER_L :
                                                    PAD_AXIS_TRIGGER_R;
                    PADSetAxisMapping(
                        pad_port, PADAxisMapping{axis, SDL_GAMEPAD_BUTTON_INVALID, triggerAxis});
                }
                PADSerializeMappings();
                label("menu-status", "Bound.");
            }
        }
        binding = -1;
        lbAudioAx_80024030(SFX_CONFIRM);
        refresh_bindings();
    }
    void cancel_binding() {
        if (binding < 0)
            return;
        binding = -1;
        lbAudioAx_80024030(SFX_BACK);
        label("menu-status", "Canceled.");
        refresh_bindings();
    }
    void apply(const Rml::String& id, int direction = 1) {
        if (id == "net-delay") {
            prefs.net_delay = (prefs.net_delay + 1 + direction + 6) % 6 - 1;
            saved();
            refresh();
            return;
        }
        if (id == "resume") {
            toggle();
            return;
        }
        if (index_of(tab_ids, id) >= 0) {
            lbAudioAx_80024030(SFX_CONFIRM);
            show_tab(index_of(tab_ids, id));
            return;
        }
        if (id == "pad-port") {
            pad_port = (pad_port + direction + 4) % 4;
            binding = -1;
            lbAudioAx_80024030(SFX_CONFIRM);
            refresh_bindings();
            return;
        }
        if (id == "bind-reset") {
            PADRestoreDefaultMapping(pad_port);
            PADSerializeMappings();
            binding = -1;
            lbAudioAx_80024030(SFX_CONFIRM);
            label("menu-status", "Defaults restored.");
            refresh_bindings();
            return;
        }
        if (id.compare(0, 5, "bind-") == 0 || id.compare(0, 5, "axis-") == 0) {
            lbAudioAx_80024030(SFX_CONFIRM);
            start_binding(std::atoi(id.c_str() + 5), id[0] == 'a');
            return;
        }
        if (id == "display") {
            const bool fullscreen = !VIGetWindowFullscreen();
            if (!SDL_SetWindowFullscreen(window, fullscreen)) {
                label("menu-status", SDL_GetError());
                return;
            }
            // SDL applies the window change asynchronously; save the requested mode.
            prefs.fullscreen = fullscreen;
        } else if (id == "sync" && !std::getenv("MELEE_VSYNC")) {
            prefs.vsync = !prefs.vsync;
            aurora_enable_vsync(prefs.vsync);
        } else if (id == "resolution") {
            prefs.render_scale = prefs.render_scale >= 10 ? 0 : int(prefs.render_scale) + 1;
            resize_pending = true;
        } else if (id == "aspect") {
            prefs.widescreen = (prefs.widescreen + direction + 3) % 3;
            resize_pending = true;
        } else if (id == "aa")
            prefs.msaa = prefs.msaa == 1 ? 4 : 1;
        else if (id == "filter")
            prefs.anisotropy = direction > 0 ? (prefs.anisotropy >= 16 ? 1 : prefs.anisotropy * 2) :
                                               (prefs.anisotropy <= 1 ? 16 : prefs.anisotropy / 2);
        else if (id == "filter-mode") {
            prefs.filter_mode = (prefs.filter_mode + direction + 4) % 4;
            aurora_set_resampler(static_cast<AuroraSampler>(prefs.filter_mode));
        } else if (id == "custom-textures") {
            prefs.custom_textures = !prefs.custom_textures;
            pc_textures_reload();
        } else if (id == "hud-mode") {
            prefs.hud_mode = (prefs.hud_mode + 1) % 2;
        } else if (id == "frozen-stadium") {
            prefs.frozen_stadium = !prefs.frozen_stadium;
        } else if (id == "free-camera") {
            prefs.free_camera = !prefs.free_camera;
        } else if (id == "ucf") {
            prefs.ucf = !prefs.ucf;
        } else if (id == "dsu") {
            prefs.dsu = !prefs.dsu;
            pc_dsu_set_enabled(prefs.dsu);
        } else if (id == "unlock-all") {
            prefs.unlock_all = !prefs.unlock_all;
        } else if (id == "backend") {
#if defined(__APPLE__)
            label("menu-status", "Graphics backend is Metal (hardware accelerated).");
#else
            if (std::getenv("MELEE_BACKEND")) {
                label("menu-status", "Backend set by MELEE_BACKEND environment.");
            } else {
                prefs.backend = backend_next(prefs.backend, direction);
                label("menu-status", "Backend set to " + std::string(backend_name(prefs.backend)) +
                                         " (restart required).");
            }
#endif
        } else if (id == "volume") {
            int step = int(prefs.volume * 10 + 0.5f);
            prefs.volume = (step >= 10 ? 0 : step + 1) / 10.0f;
        } else if (id == "music-volume") {
            int step = int(prefs.music_volume * 10 + 0.5f);
            prefs.music_volume = (step >= 10 ? 0 : step + 1) / 10.0f;
        } else if (id == "sfx-volume") {
            int step = int(prefs.sfx_volume * 10 + 0.5f);
            prefs.sfx_volume = (step >= 10 ? 0 : step + 1) / 10.0f;
        } else if (id == "mute")
            prefs.mute = !prefs.mute;
        else if (id == "reverb")
            prefs.reverb = !prefs.reverb;
        else if (id == "fps")
            prefs.fps = !prefs.fps;
        else if (id == "scale") {
            prefs.scale = prefs.scale >= 1.5f ? 0.75f : prefs.scale + 0.25f;
            aurora::rmlui::set_ui_scale(prefs.scale);
        } else if (id == "port-check-update") {
            pc::updater::check_for_updates_async(true);
            label("port-update-status", "Checking for updates...");
            label("menu-status", "Checking for updates...");
            lbAudioAx_80024030(SFX_CONFIRM);
            return;
        } else
            return;
        pc_audio_set_volume(prefs.mute ? 0 : prefs.volume);
        pc_audio_set_music_volume(prefs.music_volume);
        pc_audio_set_sfx_volume(prefs.sfx_volume);
        pc_audio_set_reverb(prefs.reverb);
        lbAudioAx_80024030(SFX_CONFIRM);
        saved();
    }
    void drag(const Rml::String& id, float value) {
        if (id == "volume")
            prefs.volume = std::clamp(value / 100.0f, 0.0f, 1.0f);
        else if (id == "music-volume")
            prefs.music_volume = std::clamp(value / 100.0f, 0.0f, 1.0f);
        else if (id == "sfx-volume")
            prefs.sfx_volume = std::clamp(value / 100.0f, 0.0f, 1.0f);
        else if (id == "resolution") {
            prefs.render_scale = std::clamp(float(int(value + 0.5f)), 0.0f, 10.0f);
            resize_pending = true;
        } else if (id == "scale") {
            prefs.scale = std::clamp(value / 100.0f, 0.75f, 1.5f);
            aurora::rmlui::set_ui_scale(prefs.scale);
        } else
            return;
        pc_audio_set_volume(prefs.mute ? 0 : prefs.volume);
        pc_audio_set_music_volume(prefs.music_volume);
        pc_audio_set_sfx_volume(prefs.sfx_volume);
        lbAudioAx_80024030(SFX_MOVE);
        saved();
    }
    // Right enters a category; within a page, the arrows adjust its values.
    void step(int direction) {
        auto* focus = document->GetContext()->GetFocusElement();
        if (!focus)
            return;
        const auto id = focus->GetId();
        if (index_of(tab_ids, id) >= 0) {
            if (direction > 0)
                enter_rows();
            return;
        }
        if (focus->GetTagName() == "input" && !is_slider(id))
            return;  // Leave the caret's arrow keys to the text field.
        if (focus->IsClassSet("value") && !focus->IsClassSet("action")) {
            apply(id, direction);
            return;
        }
        if (id == "volume")
            prefs.volume = std::clamp(prefs.volume + direction * 0.05f, 0.0f, 1.0f);
        else if (id == "music-volume")
            prefs.music_volume = std::clamp(prefs.music_volume + direction * 0.05f, 0.0f, 1.0f);
        else if (id == "sfx-volume")
            prefs.sfx_volume = std::clamp(prefs.sfx_volume + direction * 0.05f, 0.0f, 1.0f);
        else if (id == "resolution") {
            prefs.render_scale = std::clamp(prefs.render_scale + direction, 0.0f, 10.0f);
            resize_pending = true;
        } else if (id == "scale") {
            prefs.scale = std::clamp(prefs.scale + direction * 0.25f, 0.75f, 1.5f);
            aurora::rmlui::set_ui_scale(prefs.scale);
        } else {
            if (direction < 0)
                show_tab(tab);
            return;
        }
        pc_audio_set_volume(prefs.mute ? 0 : prefs.volume);
        pc_audio_set_music_volume(prefs.music_volume);
        pc_audio_set_sfx_volume(prefs.sfx_volume);
        lbAudioAx_80024030(SFX_MOVE);
        saved();
    }
    // Gamepad drives the overlay directly: while it is open PADBlockInput
    // keeps the game from seeing any of this.
    void gamepad(const SDL_Event& e) {
        if (!document)
            return;
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            const auto button = e.gbutton.button;
            if (binding < 0 &&
                (button == SDL_GAMEPAD_BUTTON_GUIDE ||
                    (button == SDL_GAMEPAD_BUTTON_BACK && (open || !is_back_bound()))))
            {
                toggle();
                return;
            }
            if (!open || binding >= 0)
                return;  // a bind prompt owns every button
            switch (button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP:
                focus_step(-1);
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                focus_step(1);
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                step(-1);
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                step(1);
                break;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
                show_tab(tab - 1);
                break;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
                show_tab(tab + 1);
                break;
            case SDL_GAMEPAD_BUTTON_SOUTH:
                activate_focus();
                break;
            case SDL_GAMEPAD_BUTTON_EAST:
                back();
                break;
            case SDL_GAMEPAD_BUTTON_START:
                toggle();
                break;
            default:
                break;
            }
        } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && open && binding < 0) {
            const auto value = e.gaxis.value;
            const auto now = SDL_GetTicks();
            if (std::abs(value) <= 18000)
                return;
            if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY && now - last_axis_y > 200) {
                focus_step(value > 0 ? 1 : -1);
                last_axis_y = now;
            } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX && now - last_axis_x > 200) {
                step(value > 0 ? 1 : -1);
                last_axis_x = now;
            }
        }
    }
    void ProcessEvent(Rml::Event& event) override {
        /* Hide() leaves RmlUi's focus inside the document it just hid, and
         * RmlUi keeps delivering key events to that focus, so the closed menu
         * went on eating input -- Return synthesises a Click on the hidden
         * control, which plays the select sound (#84). While closed, nothing
         * the menu could do is wanted. */
        if (!open)
            return;
        if (event.GetId() == Rml::EventId::Focus) {
            auto* target = event.GetTargetElement();
            Rml::Element* row = nullptr;
            Rml::String description;
            for (auto* e = target; e && e != document; e = e->GetParentNode()) {
                if (!row && e->IsClassSet("row"))
                    row = e;
                if (description.empty())
                    description = e->GetAttribute<Rml::String>("data-desc", "");
            }
            if (current_row)
                current_row->SetClass("current", false);
            current_row = row;
            if (current_row)
                current_row->SetClass("current", true);
            document->SetClass("on-back", target && target->GetId() == "resume");
            if (!description.empty())
                label("menu-status", description);
            if (!quiet)
                lbAudioAx_80024030(SFX_MOVE);
            return;
        }
        if (event.GetId() == Rml::EventId::Keydown) {
            const auto key = event.GetParameter<int>("key_identifier", 0);
            if (key == Rml::Input::KI_ESCAPE) {
                if (binding >= 0)
                    cancel_binding();
                else
                    back();
                event.StopPropagation();
                return;
            }
            if (binding >= 0)
                return;
            auto* focus = document->GetContext()->GetFocusElement();
            const auto id = focus ? focus->GetId() : Rml::String();
            if (key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN) {
                focus_step(key == Rml::Input::KI_UP ? -1 : 1);
                event.StopPropagation();
            } else if (key == Rml::Input::KI_RETURN && focus &&
                       (is_slider(id) || index_of(tab_ids, id) >= 0))
            {
                activate_focus();
                event.StopPropagation();
            } else if ((key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT) && focus &&
                       (index_of(tab_ids, id) >= 0 || is_slider(id) || focus->IsClassSet("value")))
            {
                step(key == Rml::Input::KI_LEFT ? -1 : 1);
                event.StopPropagation();
            }
            return;
        }
        if (event.GetId() == Rml::EventId::Change) {
            auto* target = event.GetTargetElement();
            if (target && !quiet && change_online(event)) {
                saved();
                return;
            }
            if (target && !quiet)
                drag(target->GetId(), event.GetParameter<float>("value", 0.0f));
            return;
        }
        if (binding >= 0)
            return;
        auto* target = event.GetTargetElement();
        while (target && target != document && target->GetTagName() != "button")
            target = target->GetParentNode();
        if (!target || target == document)
            return;
        apply(target->GetId());
    }
};
PortMenu port_menu;
}  // namespace
extern "C" bool pc_menu_is_open(void) {
    return port_menu.open;
}
extern "C" void pc_menu_init(SDL_Window* window) {
    pc_widescreen_set_mode(prefs.widescreen);
    VISetFrameBufferScale(prefs.render_scale);
    pc_audio_set_volume(prefs.mute ? 0 : prefs.volume);
    pc_audio_set_music_volume(prefs.music_volume);
    pc_audio_set_sfx_volume(prefs.sfx_volume);
    pc_audio_set_reverb(prefs.reverb);
    ensure_rmlui_input_layer();
    auto* context = aurora::rmlui::get_context();
    if (!context) {
        SDL_Log("F1 menu: RmlUi context unavailable");
        return;
    }
    auto resources = pc_resources_path();
    if (!load_menu_fonts(resources))
        return;
    aurora::rmlui::set_ui_scale(prefs.scale);
    port_menu.window = window;
    port_menu.document = context->LoadDocument((resources / "port-menu.rml").string());
    port_menu.counter = context->LoadDocument((resources / "fps.rml").string());
    if (!port_menu.document) {
        SDL_Log("F1 menu: could not load port-menu.rml");
        return;
    }
    port_menu.document->AddEventListener(Rml::EventId::Click, &port_menu);
    port_menu.document->AddEventListener(Rml::EventId::Keydown, &port_menu);
    port_menu.document->AddEventListener(Rml::EventId::Change, &port_menu);
    // Focus does not bubble; the capture pass still reaches the document.
    port_menu.document->AddEventListener(Rml::EventId::Focus, &port_menu, true);
    aurora_set_resampler(static_cast<AuroraSampler>(prefs.filter_mode));
}
extern "C" void pc_menu_toggle(void) {
    port_menu.toggle();
}
extern "C" void pc_menu_event(const SDL_Event* event) {
    if (event)
        port_menu.gamepad(*event);
}
extern "C" void pc_menu_update(void) {
    if (port_menu.counter) {
        if (prefs.fps) {
            if (!port_menu.counter->IsVisible())
                port_menu.counter->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
            if (SDL_GetTicks() - port_menu.last_fps >= 500) {
                /* Say when the pipeline queue is still draining: a draw whose
                 * pipeline is not built yet is skipped, and that is what a pop
                 * in the first minutes of a fresh install actually is. */
                std::string text = std::to_string(int(aurora_get_fps() + 0.5f)) + " FPS";
                const uint32_t pending = pending_pipelines();
                if (pending > 0)
                    text += " \xc2\xb7 " + std::to_string(pending) + " shaders";
                port_menu.counter->GetElementById("count")->SetInnerRML(text);
                port_menu.last_fps = SDL_GetTicks();
            }
        } else if (port_menu.counter->IsVisible())
            port_menu.counter->Hide();
    }
    port_menu.poll_binding();
}

extern "C" bool pc_is_custom_textures_enabled(void) {
    return prefs.custom_textures;
}
extern "C" bool pc_net_rules(bool* unlock_all, bool* frozen_stadium);
extern "C" bool pc_is_unlock_all_enabled(void) {
    bool unlock_all, frozen;
    return pc_net_rules(&unlock_all, &frozen) ? unlock_all : prefs.unlock_all;
}
extern "C" bool pc_is_frozen_stadium_enabled(void) {
    bool unlock_all, frozen;
    if (pc_net_rules(&unlock_all, &frozen)) {
        return frozen;
    }
    /* MELEE_FROZEN_STADIUM=0|1 overrides the preference, so a test can reach
     * the transformations whatever the machine's launcher says. The host's
     * value is what RULES carries, so setting it on the host is enough. */
    static const char* env = std::getenv("MELEE_FROZEN_STADIUM");
    return env ? env[0] != '0' : prefs.frozen_stadium;
}
/* Free camera and UCF are read by the simulation (camera.c's pause camera
 * feeds offscreen damage; UCF decides dashbacks and shield drops), so a
 * local preference must not reach it while the run has to reproduce on
 * another machine: two peers with different settings would desync on the
 * first dashback UCF changes. Such a run plays the competitive standard,
 * which is Slippi's too: UCF on, free camera off. Both peers run the same
 * build (the handshake binds it), so forcing here keeps them agreed without
 * a RULES field. */
extern "C" bool pc_is_free_camera_enabled(void) {
    return !pc_net_deterministic() && prefs.free_camera;
}
extern "C" uint64_t pc_install_id(void) {
    return prefs.install_id;
}
extern "C" const char* pc_app_rev(void) {
    const char* env = std::getenv("MELEE_APP_REV");
    if (env != nullptr && env[0] != '\0') {
        return env;
    }
    return pc::get_app_version().c_str();
}
extern "C" bool pc_is_ucf_enabled(void) {
    if (pc_net_deterministic()) {
        return true;
    }
    static const char* env = std::getenv("MELEE_UCF");
    return env ? env[0] != '0' : prefs.ucf;
}
extern "C" int pc_get_hud_mode(void) {
    return prefs.hud_mode;
}
extern "C" float pc_get_music_volume(void) {
    return prefs.music_volume;
}
extern "C" float pc_get_sfx_volume(void) {
    return prefs.sfx_volume;
}

extern "C" const char* pc_get_net_name(void) {
    return prefs.net_name.c_str();
}
extern "C" const char* pc_get_net_target(void) {
    return prefs.net_target.c_str();
}
/* The lobby's own code entry (gmonlinemode.c) writes back here so the next
 * session and the F1 menu field agree with what the player just typed. */
extern "C" void pc_set_net_target(const char* code) {
    prefs.net_target = code ? code : "";
    std::string error;
    launcher::save_preferences(config_path, prefs, error);
    if (port_menu.document) {
        if (auto* e = dynamic_cast<Rml::ElementFormControl*>(
                port_menu.document->GetElementById("net-target")))
            e->SetValue(prefs.net_target);
    }
}
extern "C" int pc_get_net_port(void) {
    const char* env_port = getenv("MELEE_NET_PORT");
    if (env_port && *env_port) {
        int p = std::atoi(env_port);
        if (p > 0 && p < 65536)
            return p;
    }
    return prefs.net_port;
}
extern "C" int pc_get_net_delay(void) {
    return prefs.net_delay;
}
