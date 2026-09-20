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

static bool name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool pc_identity_code_valid(const char* code) {
    if (!code)
        return false;
    size_t n = strlen(code);
    if (n < 6 || n > 13 || code[n - 5] != '#')
        return false;
    for (size_t i = 0; i < n - 5; ++i)
        if (!name_char(code[i]))
            return false;
    for (size_t i = n - 4; i < n; ++i)
        if (!((code[i] >= 'A' && code[i] <= 'Z') || (code[i] >= '2' && code[i] <= '7')))
            return false;
    return true;
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
    FILE* f = fopen(path, "rb");
    if (!f) {
        if (errno != ENOENT || !pc_identity_random(seed, sizeof seed))
            return false;
#ifdef _WIN32
        f = fopen(path, "wbx");
#else
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        f = fd < 0 ? NULL : fdopen(fd, "wb");
        if (!f && fd >= 0)
            close(fd);
#endif
        if (!f) {
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
            crypto_wipe(seed, sizeof seed);
            return false;
        }
    } else {
        bool ok = fread(seed, 1, sizeof seed, f) == sizeof seed && fgetc(f) == EOF && !ferror(f);
        fclose(f);
        if (!ok) {
            crypto_wipe(seed, sizeof seed);
            return false;
        }
    }
    crypto_ed25519_key_pair(id->secret_key, id->public_key, seed);
    crypto_wipe(seed, sizeof seed);
    uint8_t digest[20];
    pc_dht_sha1(id->public_key, sizeof id->public_key, digest);
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    unsigned bits = (unsigned)digest[0] << 12 | (unsigned)digest[1] << 4 | digest[2] >> 4;
    memcpy(id->code, label, len);
    id->code[len++] = '#';
    for (int i = 0; i < 4; ++i)
        id->code[len++] = alphabet[(bits >> (15 - 5 * i)) & 31];
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
