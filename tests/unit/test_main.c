/* test_main.c — gitfull unit test harness.
 *
 * Minimal assert-based framework (see checks.h); every test returns to the
 * caller so all tests run even after failures. Run under ASan/UBSan in CI.
 */
#include "checks.h"
#include "tests.h"

typedef struct {
    const char *name;
    int (*fn)(void);
} test_entry;

static const test_entry TESTS[] = {
    { "util", test_util },
    { "json", test_json },
    { "ini", test_ini },
    { "sha256", test_sha256 },
    { "version", test_version },
    { "pkgid", test_pkgid },
    { "toml", test_toml },
    { "tar", test_tar },
    { "depgraph", test_depgraph },
    { "package", test_package },
    { "xact", test_xact },
    { "seccomp", test_seccomp },
    { NULL, NULL }
};

int failures = 0;
int checks = 0;

int main(void)
{
    gf_log_set_level(GF_LOG_ERROR);
    for (int i = 0; TESTS[i].name; i++) {
        printf("== %s\n", TESTS[i].name);
        int before = failures;
        TESTS[i].fn();
        printf("   %s (%d checks so far)\n",
               failures == before ? "ok" : "FAILED", checks);
    }
    printf("----\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
