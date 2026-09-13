#include "launcher_data.hpp"
#include <nod.h>
#include <openssl/evp.h>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>
#include <unistd.h>

namespace launcher {
namespace {
using Disc = std::unique_ptr<NodHandle, decltype(&nod_free)>;
std::string nod_error() {
    const char* message = nod_error_message();
    return message ? message : "Could not read disc image.";
}
Disc open_disc(const std::string& path) {
    NodHandle* handle = nullptr;
    nod_disc_open(path.c_str(), nullptr, &handle);
    return Disc(handle, nod_free);
}
DiscInfo inspect_handle(NodHandle* disc) {
    NodDiscHeader header{};
    if (nod_disc_header(disc, &header) != NOD_RESULT_OK)
        return {false, "Invalid disc header: " + nod_error()};
    constexpr unsigned char magic[] = {0xc2, 0x33, 0x9f, 0x3d};
    if (std::memcmp(header.gcn_magic, magic, 4) != 0)
        return {false, "Choose a GameCube disc image."};
    if (std::memcmp(header.game_id, "GALE01", 6) != 0) {
        if (std::memcmp(header.game_id, "GAL", 3) == 0)
            return {false, "This region is not supported. Choose Melee USA revision 2 (NTSC-U 1.02)."};
        return {false, "Wrong game. Choose Super Smash Bros. Melee USA revision 2."};
    }
    if (header.disc_version != 2 || header.disc_num != 0)
        return {false, "Unsupported revision. This port requires Melee NTSC-U 1.02 (revision 2)."};
    return {true, "Super Smash Bros. Melee / USA / Revision 2 (1.02)"};
}
}

DiscInfo inspect_disc(const std::string& path) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec))
        return {false, "Disc image is missing or cannot be accessed. Choose a disc to continue."};
    auto disc = open_disc(path);
    if (!disc) return {false, "Cannot open disc image: " + nod_error()};
    return inspect_handle(disc.get());
}

Verification verify_disc(const std::string& path, std::atomic_bool& cancel, std::atomic_uint& progress) {
    progress = 0;
    if (cancel) return {VerifyState::Canceled, "Verification canceled."};
    auto disc = open_disc(path);
    if (!disc) return {VerifyState::Error, nod_error()};
    auto info = inspect_handle(disc.get());
    if (!info.supported) return {VerifyState::Error, info.message};
    // Redump DAT: libretro/libretro-database, metadat/redump/Nintendo - GameCube.dat
    // Super Smash Bros. Melee (USA) (En,Ja) (Rev 2), decoded ISO size 1459978240.
    constexpr uint64_t expected_size = 1459978240;
    constexpr char expected_sha1[] = "d4e70c064cc714ba8400a849cf299dbd1aa326fc";
    auto size = nod_disc_size(disc.get());
    if (size != expected_size)
        return {VerifyState::Mismatch, "Disc size does not match the original USA revision 2 image."};
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha1(), nullptr) != 1)
        return {VerifyState::Error, "Could not initialize disc verification."};
    if (nod_seek(disc.get(), 0, SEEK_SET) < 0) return {VerifyState::Error, nod_error()};
    std::vector<uint8_t> buffer(1024 * 1024);
    uint64_t total = 0;
    while (total < size) {
        if (cancel) return {VerifyState::Canceled, "Verification canceled."};
        auto count = nod_read(disc.get(), buffer.data(), std::min<uint64_t>(buffer.size(), size - total));
        if (count <= 0) return {VerifyState::Error, "Disc read failed during verification: " + nod_error()};
        if (EVP_DigestUpdate(hash.get(), buffer.data(), count) != 1)
            return {VerifyState::Error, "Could not hash disc data."};
        total += count;
        progress = static_cast<unsigned>(total * 100 / size);
    }
    if (cancel) return {VerifyState::Canceled, "Verification canceled."};
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned length = 0;
    if (EVP_DigestFinal_ex(hash.get(), digest.data(), &length) != 1)
        return {VerifyState::Error, "Could not finish disc verification."};
    std::ostringstream hex;
    for (unsigned i = 0; i < length; ++i)
        hex << std::hex << std::setw(2) << std::setfill('0') << unsigned(digest[i]);
    if (hex.str() != expected_sha1)
        return {VerifyState::Mismatch, "Hash mismatch. This image differs from the original USA revision 2 disc."};
    return {VerifyState::Verified, "Verified / matches the original USA revision 2 disc."};
}

Preferences load_preferences(const std::filesystem::path& path) {
    Preferences prefs;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream row(line);
        std::string key; row >> key;
        if (key == "disc") { std::string value; if (row >> std::quoted(value)) prefs.disc = value; }
        else if (key == "vsync" || key == "fullscreen") {
            int value; if (row >> value && (value == 0 || value == 1))
                (key == "vsync" ? prefs.vsync : prefs.fullscreen) = value;
        } else if (key == "widescreen") {
            int value; if (row >> value && value >= 0 && value <= 2) prefs.widescreen = value;
        } else if (key == "mute" || key == "fps") {
            int value; if (row >> value && (value == 0 || value == 1))
                (key == "mute" ? prefs.mute : prefs.fps) = value;
        } else if (key == "render_scale" || key == "volume") {
            float value; if (row >> value && std::isfinite(value) && value >= 0 && value <= (key == "volume" ? 1 : 4))
                (key == "volume" ? prefs.volume : prefs.render_scale) = value;
        } else if (key == "msaa" || key == "anisotropy") {
            int value; if (row >> value && (value == 1 || value == 4 || (key == "anisotropy" && (value == 2 || value == 8 || value == 16))))
                (key == "msaa" ? prefs.msaa : prefs.anisotropy) = value;
        } else if (key == "scale") {
            float value; if (row >> value && std::isfinite(value) && value >= 0.75f && value <= 1.5f)
                prefs.scale = value;
        }
    }
    return prefs;
}

bool save_preferences(const std::filesystem::path& path, const Preferences& prefs, std::string& error) {
    // A sibling temporary file keeps replacement atomic on the preference filesystem.
    std::string pattern = path.string() + ".XXXXXX";
    int fd = mkstemp(pattern.data());
    if (fd < 0) { error = "Could not save launcher settings: " + std::string(std::strerror(errno)); return false; }
    std::ostringstream text;
    text << "disc " << std::quoted(prefs.disc) << "\nvsync " << prefs.vsync
         << "\nfullscreen " << prefs.fullscreen << "\nscale " << prefs.scale << '\n'
         << "render_scale " << prefs.render_scale << "\nvolume " << prefs.volume
         << "\nmsaa " << prefs.msaa << "\nanisotropy " << prefs.anisotropy
         << "\nwidescreen " << prefs.widescreen << "\nmute " << prefs.mute << "\nfps " << prefs.fps << '\n';
    auto data = text.str();
    size_t done = 0;
    bool ok = true;
    while (done < data.size()) {
        auto n = write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        done += n;
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    std::error_code ec;
    if (ok) { std::filesystem::rename(pattern, path, ec); ok = !ec; }
    if (!ok) { std::filesystem::remove(pattern, ec); error = "Could not save launcher settings."; }
    return ok;
}
}
