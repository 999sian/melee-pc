#include "launcher_data.hpp"
#include <nod.h>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#define fsync _commit
#if defined(_MSC_VER)
static inline int mkstemp(char* tmpl) {
    char* name = _mktemp(tmpl);
    if (!name) return -1;
    return _open(name, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
}
#endif
#else
#include <unistd.h>
#endif

#if defined(__has_include)
#if __has_include(<openssl/evp.h>) && !defined(USE_BCRYPT)
#define MELEE_USE_OPENSSL 1
#include <openssl/evp.h>
#endif
#endif

#if !defined(MELEE_USE_OPENSSL) && defined(_WIN32)
#define MELEE_USE_BCRYPT 1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#endif

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

class Sha1Hasher {
public:
    bool ok = false;
#if defined(MELEE_USE_OPENSSL)
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{nullptr, EVP_MD_CTX_free};
    Sha1Hasher() {
        ctx.reset(EVP_MD_CTX_new());
        if (ctx && EVP_DigestInit_ex(ctx.get(), EVP_sha1(), nullptr) == 1) {
            ok = true;
        }
    }
    bool update(const void* data, size_t len) {
        return EVP_DigestUpdate(ctx.get(), data, len) == 1;
    }
    bool finish(std::string& hex) {
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned len = 0;
        if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &len) != 1) return false;
        std::ostringstream ss;
        for (unsigned i = 0; i < len; ++i)
            ss << std::hex << std::setw(2) << std::setfill('0') << unsigned(digest[i]);
        hex = ss.str();
        return true;
    }
#elif defined(MELEE_USE_BCRYPT)
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    std::vector<UCHAR> hashObj;
    Sha1Hasher() {
        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM, NULL, 0) >= 0) {
            DWORD objSize = 0, dataLen = 0;
            if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objSize, sizeof(objSize), &dataLen, 0) >= 0) {
                hashObj.resize(objSize);
                if (BCryptCreateHash(hAlg, &hHash, hashObj.data(), objSize, NULL, 0, 0) >= 0) {
                    ok = true;
                }
            }
        }
    }
    ~Sha1Hasher() {
        if (hHash) BCryptDestroyHash(hHash);
        if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    }
    bool update(const void* data, size_t len) {
        return BCryptHashData(hHash, (PUCHAR)data, (ULONG)len, 0) >= 0;
    }
    bool finish(std::string& hex) {
        UCHAR digest[20] = {0};
        if (BCryptFinishHash(hHash, digest, sizeof(digest), 0) < 0) return false;
        std::ostringstream ss;
        for (unsigned i = 0; i < 20; ++i)
            ss << std::hex << std::setw(2) << std::setfill('0') << unsigned(digest[i]);
        hex = ss.str();
        return true;
    }
#else
    struct Sha1Internal {
        uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
        uint64_t count = 0;
        uint8_t buffer[64] = {0};

        static inline uint32_t rol(uint32_t value, size_t bits) {
            return (value << bits) | (value >> (32 - bits));
        }

        static void transform(uint32_t state[5], const uint8_t buf[64]) {
            uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
            uint32_t block[80];
            for (size_t i = 0; i < 16; ++i) {
                block[i] = ((uint32_t)buf[i * 4] << 24) | ((uint32_t)buf[i * 4 + 1] << 16) |
                           ((uint32_t)buf[i * 4 + 2] << 8) | (uint32_t)buf[i * 4 + 3];
            }
            for (size_t i = 16; i < 80; ++i) {
                block[i] = rol(block[i - 3] ^ block[i - 8] ^ block[i - 14] ^ block[i - 16], 1);
            }
            for (size_t i = 0; i < 80; ++i) {
                uint32_t f, k;
                if (i < 20) {
                    f = (b & c) | ((~b) & d);
                    k = 0x5A827999;
                } else if (i < 40) {
                    f = b ^ c ^ d;
                    k = 0x6ED9EBA1;
                } else if (i < 60) {
                    f = (b & c) | (b & d) | (c & d);
                    k = 0x8F1BBCDC;
                } else {
                    f = b ^ c ^ d;
                    k = 0xCA62C1D6;
                }
                uint32_t temp = rol(a, 5) + f + e + k + block[i];
                e = d;
                d = c;
                c = rol(b, 30);
                b = a;
                a = temp;
            }
            state[0] += a;
            state[1] += b;
            state[2] += c;
            state[3] += d;
            state[4] += e;
        }

        void update(const uint8_t* data, size_t len) {
            for (size_t i = 0; i < len; ++i) {
                buffer[count % 64] = data[i];
                count++;
                if ((count % 64) == 0) {
                    transform(state, buffer);
                }
            }
        }

        void finish(uint8_t digest[20]) {
            uint64_t totalBits = count * 8;
            update((const uint8_t*)"\x80", 1);
            while ((count % 64) != 56) {
                update((const uint8_t*)"\0", 1);
            }
            for (int i = 7; i >= 0; --i) {
                uint8_t byte = (totalBits >> (i * 8)) & 0xFF;
                update(&byte, 1);
            }
            for (size_t i = 0; i < 5; ++i) {
                digest[i * 4]     = (state[i] >> 24) & 0xFF;
                digest[i * 4 + 1] = (state[i] >> 16) & 0xFF;
                digest[i * 4 + 2] = (state[i] >> 8) & 0xFF;
                digest[i * 4 + 3] = state[i] & 0xFF;
            }
        }
    } sha1;

    Sha1Hasher() { ok = true; }
    bool update(const void* data, size_t len) {
        sha1.update(reinterpret_cast<const uint8_t*>(data), len);
        return true;
    }
    bool finish(std::string& hex) {
        uint8_t digest[20];
        sha1.finish(digest);
        std::ostringstream ss;
        for (unsigned i = 0; i < 20; ++i)
            ss << std::hex << std::setw(2) << std::setfill('0') << unsigned(digest[i]);
        hex = ss.str();
        return true;
    }
#endif
};
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
    Sha1Hasher hash;
    if (!hash.ok)
        return {VerifyState::Error, "Could not initialize disc verification."};
    if (nod_seek(disc.get(), 0, SEEK_SET) < 0) return {VerifyState::Error, nod_error()};
    std::vector<uint8_t> buffer(1024 * 1024);
    uint64_t total = 0;
    while (total < size) {
        if (cancel) return {VerifyState::Canceled, "Verification canceled."};
        auto count = nod_read(disc.get(), buffer.data(), std::min<uint64_t>(buffer.size(), size - total));
        if (count <= 0) return {VerifyState::Error, "Disc read failed during verification: " + nod_error()};
        if (!hash.update(buffer.data(), count))
            return {VerifyState::Error, "Could not hash disc data."};
        total += count;
        progress = static_cast<unsigned>(total * 100 / size);
    }
    if (cancel) return {VerifyState::Canceled, "Verification canceled."};
    std::string hex;
    if (!hash.finish(hex))
        return {VerifyState::Error, "Could not finish disc verification."};
    if (hex != expected_sha1)
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
            float value; if (row >> value && std::isfinite(value) && value >= 0 && value <= (key == "volume" ? 1 : 10))
                (key == "volume" ? prefs.volume : prefs.render_scale) = value;
        } else if (key == "filter_mode") {
            int value; if (row >> value && value >= 0 && value <= 3) prefs.filter_mode = value;
        } else if (key == "msaa") {
            // Only 1x and 4x exist on this renderer; see aurora's clamp.
            int value; if (row >> value && (value == 1 || value == 4)) prefs.msaa = value;
        } else if (key == "anisotropy") {
            int value; if (row >> value && (value == 1 || value == 2 || value == 4 || value == 8 || value == 16))
                prefs.anisotropy = value;
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
         << "\nwidescreen " << prefs.widescreen << "\nmute " << prefs.mute << "\nfps " << prefs.fps
         << "\nfilter_mode " << prefs.filter_mode << '\n';
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
