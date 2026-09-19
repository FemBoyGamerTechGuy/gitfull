/* test_toml.c — TOML subset parser producing JSON trees. */
#include "checks.h"
#include "tests.h"
#include "../src/toml.h"

int test_toml(void)
{
    char err[128];

    /* full gitfull.toml-style document */
    const char *doc =
        "# gitfull recipe\n"
        "name = \"mylib\"\n"
        "version = \"1.0.0\"\n"
        "description = 'literal string desc'\n"
        "enabled = true\n"
        "count = 42\n"
        "ratio = 2.5\n"
        "\n"
        "[build]\n"
        "system = \"make\"\n"
        "jobs = 4\n"
        "options = [\"DEBUG=1\", \"PREFIX=/usr\"]\n"
        "\n"
        "[dependencies]\n"
        "runtime = [\"owner/liba@^1.0\"]\n"
        "build = []\n"
        "\n"
        "[dependencies.optional.zlib]\n"
        "spec = \">=1.2\"\n"
        "\n"
        "[[patches]]\n"
        "file = \"a.patch\"\n"
        "sha256 = \"aa11\"\n"
        "[[patches]]\n"
        "file = \"b.patch\"\n";

    gf_json *j = gf_toml_parse(doc, strlen(doc), err, sizeof(err));
    CHECK(j != NULL);
    if (j) {
        CHECK_STR(gf_json_str(gf_json_get(j, "name")), "mylib");
        CHECK_STR(gf_json_str(gf_json_get(j, "description")),
                  "literal string desc");
        CHECK(gf_json_bool(gf_json_get(j, "enabled"), false));
        CHECK_INT(gf_json_int(gf_json_get(j, "count"), -1), 42);
        CHECK(gf_json_num(gf_json_get(j, "ratio"), 0) == 2.5);
        const gf_json *b = gf_json_get(j, "build");
        CHECK_STR(gf_json_str(gf_json_get(b, "system")), "make");
        CHECK_INT(gf_json_int(gf_json_get(b, "jobs"), -1), 4);
        const gf_json *opts = gf_json_get(b, "options");
        CHECK_INT(gf_json_len(opts), 2);
        CHECK_STR(gf_json_str(gf_json_at(opts, 0)), "DEBUG=1");
        CHECK_STR(gf_json_str(gf_json_at(opts, 1)), "PREFIX=/usr");
        const gf_json *deps = gf_json_get(j, "dependencies");
        CHECK_INT(gf_json_len(gf_json_get(deps, "runtime")), 1);
        CHECK_STR(gf_json_str(gf_json_at(gf_json_get(deps, "runtime"), 0)),
                  "owner/liba@^1.0");
        CHECK_INT(gf_json_len(gf_json_get(deps, "build")), 0);
        const gf_json *z = gf_json_get(gf_json_get(deps, "optional"), "zlib");
        CHECK_STR(gf_json_str(gf_json_get(z, "spec")), ">=1.2");
        const gf_json *patches = gf_json_get(j, "patches");
        CHECK_INT(gf_json_len(patches), 2);
        CHECK_STR(gf_json_str(gf_json_get(gf_json_at(patches, 1), "file")),
                  "b.patch");
        gf_json_free(j);
    }

    /* inline tables and escapes */
    {
        const char *t = "point = { x = 1, y = -2 }\n"
                        "esc = \"a\\tb\\n\\\"q\\\"\"\n";
        gf_json *jj = gf_toml_parse(t, strlen(t), NULL, 0);
        CHECK(jj != NULL);
        if (jj) {
            const gf_json *pt = gf_json_get(jj, "point");
            CHECK_INT(gf_json_int(gf_json_get(pt, "x"), 0), 1);
            CHECK_INT(gf_json_int(gf_json_get(pt, "y"), 0), -2);
            CHECK_STR(gf_json_str(gf_json_get(jj, "esc")), "a\tb\n\"q\"");
            gf_json_free(jj);
        }
    }

    /* multi-line arrays */
    {
        const char *t = "list = [\n  1,\n  2,\n  3,\n]\n";
        gf_json *jj = gf_toml_parse(t, strlen(t), NULL, 0);
        CHECK(jj != NULL);
        if (jj) {
            CHECK_INT(gf_json_len(gf_json_get(jj, "list")), 3);
            gf_json_free(jj);
        }
    }

    /* dotted keys */
    {
        const char *t = "a.b.c = \"deep\"\n";
        gf_json *jj = gf_toml_parse(t, strlen(t), NULL, 0);
        CHECK(jj != NULL);
        if (jj) {
            CHECK_STR(gf_json_str(gf_json_get(gf_json_get(gf_json_get(jj, "a"),
                                                          "b"), "c")),
                      "deep");
            gf_json_free(jj);
        }
    }

    /* ---------------- rejected inputs ---------------- */
    const char *bad[] = {
        "x = \"\"\"triple\"\"\"\n",      /* multi-line strings unsupported */
        "x = \n",                        /* missing value */
        "[unclosed\n",                   /* bad table header */
        "x 1\n",                          /* missing = */
        "x = [1, \"mixed\", true\n",     /* unclosed array */
        "x = 2024-01-01\n",              /* dates unsupported */
        "x = @junk\n",                   /* invalid value */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        gf_json *jj = gf_toml_parse(bad[i], strlen(bad[i]), err, sizeof(err));
        CHECK(jj == NULL);
        if (jj)
            gf_json_free(jj);
    }
    return 0;
}
