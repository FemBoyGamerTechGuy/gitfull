/* test_json.c — strict parser, accessors, builders, canonical serializer. */
#include "checks.h"
#include "tests.h"
#include "../src/json.h"

#include <fcntl.h>

int test_json(void)
{
    char err[128];

    /* ---------------- valid parses ---------------- */
    {
        const char *doc = "{\"a\": 1, \"b\": [true, false, null], "
                          "\"c\": \"x\\ny\\u0041\", \"d\": 2.5e2}";
        gf_json *j = gf_json_parse(doc, strlen(doc), err, sizeof(err));
        CHECK(j != NULL);
        CHECK_INT(gf_json_get(j, "a")->type, GF_JSON_NUMBER);
        CHECK_INT(gf_json_int(gf_json_get(j, "a"), -1), 1);
        CHECK_INT(gf_json_len(gf_json_get(j, "b")), 3);
        CHECK(gf_json_bool(gf_json_at(gf_json_get(j, "b"), 0), false));
        CHECK(!gf_json_bool(gf_json_at(gf_json_get(j, "b"), 1), true));
        CHECK_INT(gf_json_at(gf_json_get(j, "b"), 2)->type, GF_JSON_NULL);
        CHECK_STR(gf_json_str(gf_json_get(j, "c")), "x\nyA");
        CHECK(gf_json_num(gf_json_get(j, "d"), 0.0) == 250.0);
        gf_json_free(j);
    }
    /* length limits: trailing garbage beyond len must be ignored */
    {
        const char *text = "{\"k\": \"v\"}  GARBAGE";
        gf_json *j = gf_json_parse(text, 11, NULL, 0);
        CHECK(j != NULL);
        CHECK_STR(gf_json_str(gf_json_get(j, "k")), "v");
        gf_json_free(j);
    }
    /* string escapes */
    {
        const char *escdoc = "\"\\\"\\\\\\/\\b\\f\\r\\n\\t\"";
        gf_json *j = gf_json_parse(escdoc, strlen(escdoc), NULL, 0);
        CHECK(j != NULL);
        CHECK_STR(gf_json_str(j), "\"\\/\b\f\r\n\t");
        gf_json_free(j);
    }

    /* ---------------- invalid inputs ---------------- */
    const char *bad[] = {
        "{",                          /* unclosed */
        "{\"a\":}",                   /* missing value */
        "{\"a\":1,}",                 /* trailing comma */
        "[1, 2,",                     /* unclosed array */
        "\"unterminated",             /* no quote */
        "\"bad \\x escape\"",          /* invalid escape */
        "{\"a\":1}{\"b\":2}",          /* two documents */
        "nul",                        /* bareword */
        "{\"a\" 1}",                  /* missing colon */
        "",                           /* empty */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        gf_json *j = gf_json_parse(bad[i], strlen(bad[i]), err, sizeof(err));
        CHECK(j == NULL);
        if (j)
            gf_json_free(j);
    }
    /* depth limit: 200 nested arrays must be rejected */
    {
        gf_strbuf sb;
        gf_strbuf_init(&sb);
        for (int i = 0; i < 200; i++)
            gf_strbuf_appendc(&sb, '[');
        for (int i = 0; i < 200; i++)
            gf_strbuf_appendc(&sb, ']');
        gf_json *j = gf_json_parse(gf_strbuf_str(&sb), sb.len, err,
                                   sizeof(err));
        CHECK(j == NULL);
        if (j)
            gf_json_free(j);
        gf_strbuf_free(&sb);
    }

    /* ---------------- accessors are type-safe ---------------- */
    {
        const char *sdoc = "{\"s\": \"str\"}";
        gf_json *j = gf_json_parse(sdoc, strlen(sdoc), NULL, 0);
        CHECK_STR(gf_json_str(gf_json_get(j, "s")), "str");
        CHECK(gf_json_str(gf_json_get(j, "missing")) == NULL);
        CHECK(gf_json_str(NULL) == NULL);
        CHECK_INT(gf_json_int(gf_json_get(j, "s"), 7), 7); /* wrong type */
        CHECK(gf_json_num(gf_json_get(j, "s"), 0.5) == 0.5);
        CHECK(gf_json_bool(gf_json_get(j, "s"), true));
        CHECK(gf_json_len(gf_json_get(j, "s")) == 0);
        gf_json_free(j);
    }

    /* ---------------- builders + canonical dump ---------------- */
    {
        gf_json *o = gf_json_new_object();
        gf_json_object_set(o, "zebra", gf_json_new_int(1));
        gf_json_object_set(o, "alpha", gf_json_new_string("first"));
        gf_json_object_set(o, "mid", gf_json_new_bool(true));
        gf_json *arr = gf_json_new_array();
        gf_json_array_push(arr, gf_json_new_string("x"));
        gf_json_array_push(arr, gf_json_new_num(2.5));
        gf_json_object_set(o, "list", arr);
        /* key replace semantics */
        gf_json_object_set(o, "alpha", gf_json_new_string("second"));
        CHECK_INT(gf_json_len(o), 4);
        CHECK_STR(gf_json_str(gf_json_get(o, "alpha")), "second");
        char *dump = gf_json_dump(o);
        CHECK(dump != NULL);
        /* canonical: keys sorted lexicographically */
        const char *expect = "{\"alpha\":\"second\",\"list\":[\"x\",2.5],"
                             "\"mid\":true,\"zebra\":1}";
        CHECK_STR(dump, expect);
        /* roundtrip: parse(dump) dumps identically */
        gf_json *rt = gf_json_parse(dump, strlen(dump), NULL, 0);
        char *dump2 = gf_json_dump(rt);
        CHECK_STR(dump2, dump);
        /* pretty contains newlines */
        char *pretty = gf_json_dump_pretty(rt);
        CHECK(strchr(pretty, '\n') != NULL);
        free(pretty);
        gf_json_free(rt);
        free(dump2);
        free(dump);
        gf_json_free(o);
    }
    /* parse(dump(x)) == x for nested random-ish structure */
    {
        gf_json *o = gf_json_new_object();
        gf_json *inner = gf_json_new_object();
        gf_json *karr = gf_json_new_array();
        gf_json_array_push(karr, gf_json_new_int(-3));
        gf_json_object_set(inner, "k", karr);
        gf_json_object_set(o, "outer", inner);
        char *d = gf_json_dump(o);
        gf_json *rt = gf_json_parse(d, strlen(d), NULL, 0);
        char *d2 = gf_json_dump(rt);
        CHECK_STR(d2, d);
        free(d2);
        free(d);
        gf_json_free(rt);
        gf_json_free(o);
    }
    return 0;
}
