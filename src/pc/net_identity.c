/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net_identity.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

/* The same SHA-1 as the DHT infohash, not a password/secret hash. */
extern void pc_dht_sha1(const void* data, size_t length, uint8_t out[20]);
extern void pc_log_line(const char* fmt, ...);

bool pc_identity_random(void* bytes, size_t length) {
#ifdef _WIN32
    return length <= ULONG_MAX &&
           BCryptGenRandom(NULL, bytes, (ULONG)length, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    size_t pos = 0;
    while (pos < length) {
        ssize_t n = read(fd, (uint8_t*)bytes + pos, length - pos);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            return false;
        }
        pos += (size_t)n;
    }
    close(fd);
    return true;
#endif
}

static char upper(char c) {
    return c >= 'a' && c <= 'z' ? (char)(c - ('a' - 'A')) : c;
}
static bool name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool pc_identity_code_valid(const char* code) {
    if (!code)
        return false;
    size_t n = strlen(code);
    if (n < 10 || n > 17 || code[n - 9] != '#')
        return false;
    for (size_t i = 0; i < n - 9; ++i)
        if (!name_char(code[i]))
            return false;
    for (size_t i = n - 8; i < n; ++i)
        if (!((code[i] >= 'A' && code[i] <= 'Z') || (code[i] >= '2' && code[i] <= '7')))
            return false;
    return true;
}

const char* pc_identity_code_suffix(const char* code) {
    return pc_identity_code_valid(code) ? code + strlen(code) - 8 : "";
}

/* One character of a base32 suffix as a person might type it, or 0. The
 * alphabet has no 0, 1 or 8, so those can only be O, I and B misread. */
static char suffix_char(char c) {
    c = upper(c);
    if (c == '0')
        return 'O';
    if (c == '1')
        return 'I';
    if (c == '8')
        return 'B';
    return (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7') ? c : 0;
}

bool pc_identity_parse_code(const char* text, char suffix[9], char name[9]) {
    if (!text)
        return false;
    suffix[0] = name[0] = 0;
    /* Preferred: NAME#SUFFIX, or #SUFFIX, anywhere in the text. Separators
     * people add when reading a code out (spaces, dashes) are skipped. */
    for (const char* hash = strchr(text, '#'); hash; hash = strchr(hash + 1, '#')) {
        char s[9];
        int n = 0;
        const char* p = hash + 1;
        for (; *p && n < 8; p++) {
            if (*p == '-' || *p == ' ')
                continue;
            char c = suffix_char(*p);
            if (!c)
                break;
            s[n++] = c;
        }
        char next = suffix_char(*p);
        if (n != 8 || (next && *p != ' ' && *p != '-'))
            continue; /* too short, or runs on: not a code */
        s[8] = 0;
        /* The name: up to eight name characters directly before the '#'. */
        int len = 0;
        while (len < 8 && hash - len > text && name_char(upper(hash[-len - 1])))
            len++;
        for (int i = 0; i < len; i++)
            name[i] = upper(hash[i - len]);
        name[len] = 0;
        memcpy(suffix, s, 9);
        return true;
    }
    /* Otherwise a bare suffix: exactly eight code characters standing alone. */
    for (const char* p = text; *p; p++) {
        if (p > text && suffix_char(p[-1]))
            continue; /* only at the start of a run */
        char s[9];
        int n = 0;
        const char* q = p;
        while (n < 8 && suffix_char(*q))
            s[n++] = suffix_char(*q++);
        if (n == 8 && !suffix_char(*q)) {
            s[8] = 0;
            memcpy(suffix, s, 9);
            return true;
        }
    }
    return false;
}

bool pc_identity_load(PcNetIdentity* id, const char* directory, const char* name) {
    uint8_t seed[32];
    char label[9], path[4096];
    if (!id || !directory || !name)
        return false;
    memset(id, 0, sizeof *id);
    size_t len = strlen(name);
    if (!len || len > 8)
        return false;
    for (size_t i = 0; i < len; ++i) {
        char c = name[i];
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        if (!name_char(c))
            return false;
        label[i] = c;
    }
    label[len] = 0;
    int n = snprintf(path, sizeof path, "%s/identity.key", directory);
    if (n < 0 || n >= (int)sizeof path)
        return false;
    /* A key is exactly 32 bytes, so a file of any other length cannot hold one
     * (a 0-byte file left by a crash or a full disk between create and write,
     * or a truncated copy). Failing on those stranded the profile for good:
     * every later run reported "identity unavailable" until the file was
     * deleted by hand. A wrong-length file is replaced; a 32-byte file is
     * never replaced, and a read error on one is still an error, because it
     * may hold the identity this profile's rating history belongs to. */
    bool loaded = false, replace = false;
    FILE* f = fopen(path, "rb");
    if (f) {
        int64_t size = fseek(f, 0, SEEK_END) == 0 ? (int64_t)ftell(f) : -1;
        if (size == (int64_t)sizeof seed && fseek(f, 0, SEEK_SET) == 0) {
            loaded = fread(seed, 1, sizeof seed, f) == sizeof seed && !ferror(f);
        } else if (size >= 0) {
            replace = true;
            pc_log_line("net: %s is %lld bytes, not a 32-byte identity key;"
                        " generating a new one",
                path, (long long)size);
        }
        fclose(f);
        if (!loaded && !replace) {
            pc_log_line("net: cannot read %s (errno %d)", path, errno);
            crypto_wipe(seed, sizeof seed);
            return false;
        }
    } else if (errno != ENOENT) {
        pc_log_line("net: cannot open %s (errno %d)", path, errno);
        return false;
    }
    if (!loaded) {
        if (!pc_identity_random(seed, sizeof seed))
            return false;
#ifdef _WIN32
        f = fopen(path, "wb");
#else
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        f = fd < 0 ? NULL : fdopen(fd, "wb");
        if (!f && fd >= 0)
            close(fd);
#endif
        if (!f) {
            pc_log_line("net: cannot write %s (errno %d)", path, errno);
            crypto_wipe(seed, sizeof seed);
            return false;
        }
        bool ok = fwrite(seed, 1, sizeof seed, f) == sizeof seed;
        if (fflush(f) != 0)
            ok = false;
#ifndef _WIN32
        if (fsync(fileno(f)) != 0)
            ok = false;
#endif
        if (fclose(f) != 0)
            ok = false;
        if (!ok) {
            pc_log_line("net: failed to write %s", path);
            crypto_wipe(seed, sizeof seed);
            return false;
        }
    }
    crypto_ed25519_key_pair(id->secret_key, id->public_key, seed);
    crypto_wipe(seed, sizeof seed);
    uint8_t digest[20];
    pc_dht_sha1(id->public_key, sizeof id->public_key, digest);
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    /* 40 digest bits (five bytes) packed big-endian: the same bit-packing as
     * the DHT infohash, 8 base32 chars ~ 2^40 grind cost. */
    uint64_t bits = (uint64_t)digest[0] << 32 | (uint64_t)digest[1] << 24 |
                    (uint64_t)digest[2] << 16 | (uint64_t)digest[3] << 8 | digest[4];
    memcpy(id->code, label, len);
    id->code[len++] = '#';
    for (int i = 0; i < 8; ++i)
        id->code[len++] = alphabet[(bits >> (35 - 5 * i)) & 31];
    id->code[len] = 0;
    return true;
}

void pc_identity_sign(
    const PcNetIdentity* id, uint8_t signature[64], const void* message, size_t length) {
    crypto_ed25519_sign(signature, id->secret_key, message, length);
}
bool pc_identity_verify(
    const uint8_t key[32], const uint8_t signature[64], const void* message, size_t length) {
    return crypto_ed25519_check(signature, key, message, length) == 0;
}
void pc_identity_clear(PcNetIdentity* id) {
    crypto_wipe(id, sizeof *id);
}
