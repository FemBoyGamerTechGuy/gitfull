/* test_version.c — semver parsing, comparison, stable selection, constraints. */
#include "checks.h"
#include "tests.h"
#include "../src/version.h"

static bool vparse(const char *s, int *maj, int *min, int *pat,
                   char *prebuf, size_t prelen)
{
    gf_version v;
    if (!gf_version_parse(s, &v))
        return false;
    *maj = v.major;
    *min = v.minor;
    *pat = v.patch;
    if (prebuf)
        snprintf(prebuf, prelen, "%s", v.prerelease ? v.prerelease : "");
    gf_version_free(&v);
    return true;
}

static int vcmp(const char *a, const char *b)
{
    gf_version va, vb;
    if (!gf_version_parse(a, &va) || !gf_version_parse(b, &vb))
        return 999;
    int r = gf_version_cmp(&va, &vb);
    gf_version_free(&va);
    gf_version_free(&vb);
    return r;
}

int test_version(void)
{
    int maj = 0, min = 0, pat = 0;
    char pre[64] = { 0 };

    /* ---------------- parsing ---------------- */
    CHECK(vparse("1.2.3", &maj, &min, &pat, pre, sizeof(pre)));
    CHECK_INT(maj, 1); CHECK_INT(min, 2); CHECK_INT(pat, 3);
    CHECK(pre[0] == '\0');
    CHECK(vparse("v1.2.3", &maj, &min, &pat, pre, sizeof(pre)));
    CHECK_INT(maj, 1);
    CHECK(vparse("V2.0", &maj, &min, &pat, pre, sizeof(pre)));
    CHECK_INT(maj, 2); CHECK_INT(min, 0); CHECK_INT(pat, 0);
    CHECK(vparse("3", &maj, &min, &pat, pre, sizeof(pre)));
    CHECK_INT(maj, 3);
    CHECK(vparse("release-4.5.6", &maj, &min, &pat, pre, sizeof(pre)));
    CHECK_INT(maj, 4); CHECK_INT(min, 5); CHECK_INT(pat, 6);
    CHECK(vparse("1.2.3-rc1", &maj, &min, &pat, pre, sizeof(pre)));
    CHECK_STR(pre, "rc1");
    CHECK(vparse("1.2.3-rc.1+build.5", &maj, &min, &pat, pre, sizeof(pre)));
    CHECK_STR(pre, "rc.1");
    CHECK(vparse("1.2.3+meta", &maj, &min, &pat, pre, sizeof(pre)));
    CHECK(pre[0] == '\0'); /* build metadata only: still stable */

    /* invalid */
    gf_version v;
    CHECK(!gf_version_parse("", &v));
    CHECK(!gf_version_parse("x", &v));
    CHECK(!gf_version_parse("1.x", &v));
    CHECK(!gf_version_parse("1.2.3.4", &v));
    CHECK(!gf_version_parse("v", &v));
    CHECK(!gf_version_parse("-1.0.0", &v));
    /* note: leading zeros are tolerated in tags found in the wild */

    /* ---------------- comparison (semver 2.0.0) ---------------- */
    CHECK(vcmp("1.0.0", "1.0.1") < 0);
    CHECK(vcmp("1.0.1", "1.0.0") > 0);
    CHECK(vcmp("1.0.0", "1.1.0") < 0);
    CHECK(vcmp("1.1.0", "2.0.0") < 0);
    CHECK(vcmp("1.0", "1.0.0") == 0);       /* missing parts = 0 */
    CHECK(vcmp("2.0", "1.9.9") > 0);
    CHECK(vcmp("1.0.0-rc1", "1.0.0") < 0);  /* prerelease < release */
    CHECK(vcmp("1.0.0-alpha", "1.0.0-beta") < 0);
    CHECK(vcmp("1.0.0-alpha.1", "1.0.0-alpha.beta") < 0);
    CHECK(vcmp("1.0.0-alpha.beta", "1.0.0-beta") < 0);
    CHECK(vcmp("1.0.0-rc.1", "1.0.0-rc.2") < 0);
    CHECK(vcmp("1.0.0+b1", "1.0.0+b2") == 0); /* build metadata ignored */
    CHECK(vcmp("10.0.0", "9.0.0") > 0);       /* numeric, not lexicographic */
    CHECK(vcmp("1.10.0", "1.9.0") > 0);

    /* ---------------- stable policy ---------------- */
    CHECK(gf_version_tag_stable("1.2.3"));
    CHECK(gf_version_tag_stable("v1.0"));
    CHECK(!gf_version_tag_stable("1.2.3-rc1"));
    CHECK(!gf_version_tag_stable("0.1.0-beta.1"));
    CHECK(!gf_version_tag_stable("not-a-version"));

    /* best tag: numeric ordering, prerelease filtering */
    {
        char t1[] = "v1.9.0", t2[] = "v1.10.0", t3[] = "v1.10.0-rc1",
             t4[] = "v0.3";
        char *tags[] = { t1, t2, t3, t4 };
        char *best = gf_version_best_tag(tags, 4, false);
        CHECK_STR(best, "v1.10.0");
        free(best);
        /* newest overall is still the stable 1.10.0 (rc sorts lower);
         * a prerelease only wins when it is genuinely the newest tag */
        char *best2 = gf_version_best_tag(tags, 4, true);
        CHECK_STR(best2, "v1.10.0");
        free(best2);
        char t5[] = "v1.11.0-rc1";
        char *tags2[] = { t1, t2, t5 };
        char *best3 = gf_version_best_tag(tags2, 3, true);
        CHECK_STR(best3, "v1.11.0-rc1");
        free(best3);
        char junk[] = "junk";
        char *none[] = { junk };
        CHECK(gf_version_best_tag(none, 1, false) == NULL);
    }

    /* ---------------- constraints ---------------- */
    {
        gf_vc c;
        CHECK(gf_vc_parse("=1.2.3", &c));
        CHECK_INT(c.op, GF_VC_EXACT);
        gf_vc_free(&c);
        CHECK(gf_vc_parse(">=1.0", &c));
        CHECK_INT(c.op, GF_VC_GTE);
        gf_vc_free(&c);
        CHECK(gf_vc_parse("<2.0.0", &c));
        CHECK_INT(c.op, GF_VC_LT);
        gf_vc_free(&c);
        CHECK(gf_vc_parse("!=1.0.0", &c));
        CHECK_INT(c.op, GF_VC_NE);
        gf_vc_free(&c);
        CHECK(gf_vc_parse("^1.2.3", &c));
        CHECK_INT(c.op, GF_VC_CARET);
        gf_vc_free(&c);
        CHECK(gf_vc_parse("~1.2.3", &c));
        CHECK_INT(c.op, GF_VC_TILDE);
        gf_vc_free(&c);
        CHECK(gf_vc_parse("1.2.3", &c)); /* bare = exact */
        CHECK_INT(c.op, GF_VC_EXACT);
        gf_vc_free(&c);
        CHECK(!gf_vc_parse(">=junk", &c));
        CHECK(!gf_vc_parse("=1.2.3.4", &c));

        /* matching semantics via vcset */
        gf_vcset set;
        CHECK(gf_vcset_parse(">=1.0, <2.0", &set));
        gf_version v1;
        gf_version_parse("1.5.0", &v1);
        CHECK(gf_vcset_match(&set, &v1));
        gf_version_free(&v1);
        gf_version_parse("2.1.0", &v1);
        CHECK(!gf_vcset_match(&set, &v1));
        gf_version_free(&v1);
        gf_version_parse("0.9.0", &v1);
        CHECK(!gf_vcset_match(&set, &v1));
        gf_version_free(&v1);
        char *s = gf_vcset_str(&set);
        CHECK(s != NULL && strlen(s) > 0);
        free(s);
        gf_vcset_free(&set);

        /* caret: same major, >= given */
        CHECK(gf_vcset_parse("^1.2.3", &set));
        gf_version_parse("1.9.9", &v1);
        CHECK(gf_vcset_match(&set, &v1));
        gf_version_free(&v1);
        gf_version_parse("1.2.2", &v1);
        CHECK(!gf_vcset_match(&set, &v1));
        gf_version_free(&v1);
        gf_version_parse("2.0.0", &v1);
        CHECK(!gf_vcset_match(&set, &v1));
        gf_version_free(&v1);
        gf_vcset_free(&set);

        /* tilde: same major.minor */
        CHECK(gf_vcset_parse("~1.2.3", &set));
        gf_version_parse("1.2.9", &v1);
        CHECK(gf_vcset_match(&set, &v1));
        gf_version_free(&v1);
        gf_version_parse("1.3.0", &v1);
        CHECK(!gf_vcset_match(&set, &v1));
        gf_version_free(&v1);
        gf_vcset_free(&set);

        /* wildcards */
        CHECK(gf_vcset_parse("1.2.x", &set));
        gf_version_parse("1.2.7", &v1);
        CHECK(gf_vcset_match(&set, &v1));
        gf_version_free(&v1);
        gf_version_parse("1.3.0", &v1);
        CHECK(!gf_vcset_match(&set, &v1));
        gf_version_free(&v1);
        gf_vcset_free(&set);
        CHECK(gf_vcset_parse("1.x", &set));
        gf_version_parse("1.9.9", &v1);
        CHECK(gf_vcset_match(&set, &v1));
        gf_version_free(&v1);
        gf_version_parse("2.0.0", &v1);
        CHECK(!gf_vcset_match(&set, &v1));
        gf_version_free(&v1);
        gf_vcset_free(&set);

        /* not-equal */
        CHECK(gf_vcset_parse("!=1.0.0", &set));
        gf_version_parse("1.0.1", &v1);
        CHECK(gf_vcset_match(&set, &v1));
        gf_version_free(&v1);
        gf_version_parse("1.0.0", &v1);
        CHECK(!gf_vcset_match(&set, &v1));
        gf_version_free(&v1);
        gf_vcset_free(&set);

        /* empty set matches anything */
        CHECK(gf_vcset_parse("", &set));
        CHECK(gf_vcset_empty(&set));
        gf_version_parse("9.9.9", &v1);
        CHECK(gf_vcset_match(&set, &v1));
        gf_version_free(&v1);
        gf_vcset_free(&set);
    }
    return 0;
}
