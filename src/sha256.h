/* sha256.h — self-contained SHA-256 (FIPS 180-4). Used for content
 * addressing, checksums and build identities everywhere in gitfull. */
#ifndef GF_SHA256_H
#define GF_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define GF_SHA256_DIGEST_LEN 32
/* Hex digest length including NUL. */
#define GF_SHA256_HEXLEN 65

typedef struct gf_sha256 {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t block[64];
    size_t blocklen;
} gf_sha256;

void gf_sha256_init(gf_sha256 *ctx);
void gf_sha256_update(gf_sha256 *ctx, const void *data, size_t len);
/* Finalize; out must have 32 bytes. ctx is reset afterwards. */
void gf_sha256_final(gf_sha256 *ctx, uint8_t out[32]);

/* One-shot helpers. */
void gf_sha256_buf(const void *data, size_t len, uint8_t out[32]);
/* Returns malloc'd lowercase hex string, NULL on error. */
char *gf_sha256_file_hex(const char *path);
char *gf_sha256_buf_hex(const void *data, size_t len);

#endif /* GF_SHA256_H */
