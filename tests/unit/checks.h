/* checks.h — shared assert macros and helpers for unit tests.
 *
 * Every test function returns normally so the whole suite runs even after
 * failures; CHECK prints file/line/expression and accumulates counts.
 */
#ifndef GF_TEST_CHECKS_H
#define GF_TEST_CHECKS_H

#include "../src/common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int failures;
extern int checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                    \
    } while (0)

#define CHECK_STR(a, b)                                                      \
    do {                                                                     \
        checks++;                                                            \
        const char *_va = (a), *_vb = (b);                                   \
        if (!_va || !_vb || strcmp(_va, _vb) != 0) {                         \
            failures++;                                                      \
            fprintf(stderr, "  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__,    \
                    __LINE__, _va ? _va : "(null)", _vb ? _vb : "(null)");   \
        }                                                                    \
    } while (0)

#define CHECK_INT(a, b)                                                      \
    do {                                                                     \
        checks++;                                                            \
        long _la = (long)(a), _lb = (long)(b);                               \
        if (_la != _lb) {                                                    \
            failures++;                                                      \
            fprintf(stderr, "  FAIL %s:%d: %ld != %ld\n", __FILE__,           \
                    __LINE__, _la, _lb);                                     \
        }                                                                    \
    } while (0)

/* Create a fresh temp directory (mkdtemp). Aborts on failure. */
static inline char *test_tmpdir(const char *tag)
{
    char tmpl[128];
    snprintf(tmpl, sizeof(tmpl), "/tmp/gitfull-test-%s-XXXXXX", tag);
    char *dir = gf_strdup(tmpl);
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        abort();
    }
    return dir;
}

static inline void test_rmdir(char *dir)
{
    gf_fs_rm_rf(dir);
    free(dir);
}

/* Write a small file with contents (mkdir -p of parent). */
static inline void test_write(const char *path, const char *data)
{
    char *parent = gf_path_dirname(path);
    gf_fs_mkdir_p(parent);
    free(parent);
    if (gf_fs_write_file_atomic(path, data, strlen(data)) != 0) {
        fprintf(stderr, "  FAIL cannot write %s\n", path);
        failures++;
    }
}

/* Write dir/rel with contents; frees the joined path (own convenience). */
static inline void test_wf(const char *dir, const char *rel, const char *data)
{
    char *p = gf_path_join(dir, rel);
    test_write(p, data);
    free(p);
}

/* Read a whole file into a malloc'd buffer (NULL on error). */
static inline char *test_read(const char *path)
{
    return gf_fs_read_file(path, NULL);
}

#endif /* GF_TEST_CHECKS_H */
