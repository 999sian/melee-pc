#include "pc/net_identity.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    char path[] = "/tmp/melee-identity-XXXXXX";
    assert(mkdtemp(path));
    PcNetIdentity a, b;
    unsigned char sig[64];
    const unsigned char message[] = "meleepc pairing transcript";
    assert(pc_identity_load(&a, path, "alice"));
    assert(pc_identity_load(&b, path, "ALICE"));
    assert(memcmp(a.public_key, b.public_key, 32) == 0);
    assert(strcmp(a.code, b.code) == 0);
    assert(strncmp(a.code, "ALICE#", 6) == 0);
    pc_identity_sign(&a, sig, message, sizeof message);
    assert(pc_identity_verify(a.public_key, sig, message, sizeof message));
    sig[0] ^= 1;
    assert(!pc_identity_verify(a.public_key, sig, message, sizeof message));
    assert(!pc_identity_load(&b, path, "too-long-name"));
    char file[512];
    snprintf(file, sizeof file, "%s/identity.key", path);
    FILE* f = fopen(file, "wb");
    assert(f);
    fputc(1, f);
    fclose(f);
    assert(!pc_identity_load(&b, path, "ALICE")); /* never replace a damaged identity */
    pc_identity_clear(&a);
    for (size_t i = 0; i < sizeof a; ++i)
        assert(((unsigned char*)&a)[i] == 0);
    unlink(file);
    rmdir(path);
    puts("identity persistence, signatures, name validation and corrupt-key rejection passed");
}
