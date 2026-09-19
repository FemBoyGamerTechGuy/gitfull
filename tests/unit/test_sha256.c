/* test_sha256.c — FIPS 180-4 test vectors + file hashing. */
#include "checks.h"
#include "tests.h"
#include "../src/sha256.h"

int test_sha256(void)
{
    uint8_t d[32];
    char hex[GF_SHA256_HEXLEN];

    /* empty string */
    gf_sha256_buf("", 0, d);
    gf_hex_encode(hex, sizeof(hex), d, 32);
    hex[64] = '\0';
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934"
                   "ca495991b7852b855");

    /* "abc" */
    gf_sha256_buf("abc", 3, d);
    gf_hex_encode(hex, sizeof(hex), d, 32);
    hex[64] = '\0';
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9"
                   "cb410ff61f20015ad");

    /* 448-bit message */
    const char *m2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlm"
                     "nomnopnopq";
    gf_sha256_buf(m2, strlen(m2), d);
    gf_hex_encode(hex, sizeof(hex), d, 32);
    hex[64] = '\0';
    CHECK_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff21"
                   "67f6ecedd419db06c1");

    /* one million 'a' (multi-block, 1 MiB) */
    {
        char *big = gf_malloc(1000000);
        memset(big, 'a', 1000000);
        gf_sha256_buf(big, 1000000, d);
        gf_hex_encode(hex, sizeof(hex), d, 32);
        hex[64] = '\0';
        CHECK_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48"
                       "a497200e046d39ccc7112cd0");
        free(big);
    }

    /* incremental == one-shot, odd chunk sizes crossing block boundaries */
    {
        const char *msg = "The quick brown fox jumps over the lazy dog. "
                          "0123456789 abcdefghijklmnopqrstuvwxyz";
        size_t mlen = strlen(msg);
        gf_sha256 inc;
        gf_sha256_init(&inc);
        size_t off = 0, step = 1;
        while (off < mlen) {
            size_t take = step;
            if (off + take > mlen)
                take = mlen - off;
            gf_sha256_update(&inc, msg + off, take);
            off += take;
            step = (step % 17) + 3;
        }
        uint8_t d2[32];
        gf_sha256_final(&inc, d2);
        gf_sha256_buf(msg, mlen, d);
        CHECK(memcmp(d, d2, 32) == 0);
    }

    /* 896-bit multi-block boundary case: 119 bytes then pad */
    {
        char buf[119];
        for (int i = 0; i < 119; i++)
            buf[i] = (char)(i * 7 + 1);
        gf_sha256 a, b;
        gf_sha256_init(&a);
        gf_sha256_update(&a, buf, 119);
        uint8_t da[32], db[32];
        gf_sha256_final(&a, da);
        gf_sha256_buf(buf, 119, db);
        CHECK(memcmp(da, db, 32) == 0);
        (void)b;
    }

    /* file hashing == buffer hashing */
    {
        char *dir = test_tmpdir("sha");
        char *path = gf_path_join(dir, "f.bin");
        const char *data = "gitfull sha256 file test";
        test_write(path, data);
        char *fh = gf_sha256_file_hex(path);
        char *bh = gf_sha256_buf_hex(data, strlen(data));
        CHECK_STR(fh, bh);
        free(fh);
        free(bh);
        free(path);
        test_rmdir(dir);
    }
    return 0;
}
