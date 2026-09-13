#include "pc/launcher_data.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <future>
#include <thread>
#include <unistd.h>

int main(int argc, char** argv) {
    using namespace launcher;
    auto dir = std::filesystem::temp_directory_path() / ("melee-launcher-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    assert(!inspect_disc("").supported);
    assert(!inspect_disc((dir / "missing.iso").string()).supported);
    auto image = dir / "disc with spaces.iso";
    std::string bytes(0x10000, '\0');
    bytes.replace(0, 6, "GALE01");
    bytes[7] = 1;
    bytes.replace(0x1c, 4, "\xc2\x33\x9f\x3d", 4);
    auto write_image = [&] { std::ofstream f(image, std::ios::binary); f.write(bytes.data(), bytes.size()); };
    write_image();
    assert(!inspect_disc(image.string()).supported);
    bytes[7] = 2;
    bytes.replace(0, 6, "GZ2E01");
    write_image();
    assert(!inspect_disc(image.string()).supported);
    bytes.replace(0, 6, "GALE01");
    write_image();
    assert(inspect_disc(image.string()).supported);
    std::atomic_bool cancel{true}; std::atomic_uint progress{0};
    assert(verify_disc(image.string(), cancel, progress).state == VerifyState::Canceled);
    cancel = false;
    assert(verify_disc(image.string(), cancel, progress).state != VerifyState::Verified);
    std::filesystem::resize_file(image, 1459978240);
    auto worker = std::async(std::launch::async, [&] { return verify_disc(image.string(), cancel, progress); });
    while (progress < 1 && worker.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {}
    cancel = true;
    assert(worker.get().state == VerifyState::Canceled);
    cancel = false;
    assert(verify_disc(image.string(), cancel, progress).state == VerifyState::Mismatch);
    auto config = dir / "launcher.cfg";
    auto prefs = load_preferences(config);
    assert(prefs.disc.empty() && prefs.vsync && !prefs.fullscreen && prefs.scale == 1.0f);
    prefs.disc = (dir / "quoted \"disc\".iso").string(); prefs.fullscreen = true; prefs.scale = 1.25f;
    std::string error;
    assert(save_preferences(config, prefs, error));
    auto loaded = load_preferences(config);
    assert(loaded.disc == prefs.disc && loaded.fullscreen && loaded.scale == 1.25f);
    { std::ofstream f(config); f << "scale nan\nvsync rubbish\n"; }
    loaded = load_preferences(config);
    assert(loaded.scale == 1.0f && loaded.vsync);
    { std::ofstream f(config); f << "render_scale 2\nmsaa 4\nanisotropy 8\nvolume 0.4\nmute 1\nfps 1\n"; }
    loaded = load_preferences(config);
    assert(save_preferences(config, loaded, error));
    { std::ifstream f(config); std::string saved((std::istreambuf_iterator<char>(f)), {});
      assert(saved.find("render_scale 2\n") != std::string::npos);
      assert(saved.find("volume 0.4\n") != std::string::npos);
      assert(saved.find("msaa 4\n") != std::string::npos);
    }
    { std::ofstream f(config); f << "render_scale -1\nmsaa 3\nanisotropy 32\nvolume 2\nmute 9\nfps -1\n"; }
    loaded = load_preferences(config);
    assert(loaded.render_scale == 0 && loaded.msaa == 1 && loaded.anisotropy == 16);
    assert(loaded.volume == 1 && !loaded.mute && !loaded.fps);
    for (int mode = 0; mode <= 2; ++mode) {
        loaded.widescreen = mode;
        assert(save_preferences(config, loaded, error));
        assert(load_preferences(config).widescreen == mode);
    }
    { std::ofstream f(config); f << "widescreen -1\nwidescreen 99\n"; }
    assert(load_preferences(config).widescreen == 0);
    assert(!save_preferences(dir / "missing" / "config", prefs, error));
    assert(!error.empty());
    std::filesystem::remove_all(dir);
    std::cout << "PASS: disc validation, cancellation, mismatch, preferences and write errors\n";
    if (argc > 1) {
        auto info = inspect_disc(argv[1]);
        std::cout << info.message << '\n';
        if (!info.supported) return 1;
        auto check = verify_disc(argv[1], cancel, progress);
        std::cout << check.message << '\n';
        if (check.state == VerifyState::Error || check.state == VerifyState::Canceled) return 1;
    }
}
