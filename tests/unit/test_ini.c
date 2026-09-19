/* test_ini.c — INI parser: sections, quoting, comments, errors. */
#include "checks.h"
#include "tests.h"
#include "../src/ini.h"

int test_ini(void)
{
    char err[128];
    const char *text =
        "# top comment\n"
        "global = yes\n"
        "\n"
        "[repositories.github]\n"
        "kind = github\n"
        "api_url = \"https://api.github.com\"\n"
        "enabled = true\n"
        "; another comment\n"
        "[rollback]\n"
        "hold = 3\n"
        "interactive = no\n";

    gf_ini *ini = gf_ini_parse(text, err, sizeof(err));
    CHECK(ini != NULL);
    if (!ini)
        return 0;

    CHECK_STR(gf_ini_get(ini, NULL, "global"), "yes");
    CHECK_STR(gf_ini_get(ini, "repositories.github", "kind"), "github");
    /* quoted values: quotes stripped */
    CHECK_STR(gf_ini_get(ini, "repositories.github", "api_url"),
              "https://api.github.com");
    CHECK(gf_ini_get_bool(ini, "repositories.github", "enabled", false));
    CHECK(!gf_ini_get_bool(ini, "rollback", "interactive", true));
    /* defaults */
    CHECK(gf_ini_get(ini, "rollback", "missing") == NULL);
    CHECK_STR(gf_ini_get_def(ini, "rollback", "missing", "dv"), "dv");
    CHECK(gf_ini_get(ini, "nope", "kind") == NULL);

    /* section iteration (unique, order of appearance) */
    CHECK_INT(gf_ini_section_count(ini), 2);
    CHECK_STR(gf_ini_section(ini, 0), "repositories.github");
    CHECK_STR(gf_ini_section(ini, 1), "rollback");

    /* key iteration within a section */
    size_t nk = gf_ini_key_count(ini, "repositories.github");
    CHECK_INT(nk, 3);
    CHECK_STR(gf_ini_key(ini, "repositories.github", 0), "kind");
    CHECK_STR(gf_ini_key(ini, "repositories.github", 1), "api_url");
    CHECK_STR(gf_ini_key(ini, "repositories.github", 2), "enabled");
    gf_ini_free(ini);

    /* values are trimmed */
    ini = gf_ini_parse("k =   spaces everywhere   \n", NULL, 0);
    CHECK(ini != NULL);
    CHECK_STR(gf_ini_get(ini, NULL, "k"), "spaces everywhere");
    gf_ini_free(ini);

    /* ---------------- syntax errors ---------------- */
    const char *bad[] = {
        "[unclosed\n",      /* section header not closed */
        "key without eq\n", /* no '=' */
        "=noname\n",        /* empty key */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        gf_ini *b = gf_ini_parse(bad[i], err, sizeof(err));
        CHECK(b == NULL);
        if (b)
            gf_ini_free(b);
    }

    /* ---------------- file loading ---------------- */
    {
        char *dir = test_tmpdir("ini");
        char *path = gf_path_join(dir, "x.conf");
        test_write(path, "[s]\nv = 1\n");
        gf_ini *f = gf_ini_parse_file(path, err, sizeof(err));
        CHECK(f != NULL);
        CHECK_STR(gf_ini_get(f, "s", "v"), "1");
        gf_ini_free(f);
        /* missing file */
        {
            char *nopath = gf_path_join(dir, "no.conf");
            f = gf_ini_parse_file(nopath, err, sizeof(err));
            CHECK(f == NULL);
            gf_ini_free(f);
            free(nopath);
        }
        free(path);
        test_rmdir(dir);
    }
    return 0;
}
