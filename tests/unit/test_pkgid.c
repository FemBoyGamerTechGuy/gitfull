/* test_pkgid.c — package identifier parsing for every supported form. */
#include "checks.h"
#include "tests.h"
#include "../src/pkgid.h"

int test_pkgid(void)
{
    gf_pkgref r;

    /* plain owner/repo (default forge applied) */
    CHECK(gf_pkgref_parse("femboy/hello", "github", &r) == 0);
    CHECK_STR(r.owner, "femboy");
    CHECK_STR(r.repo, "hello");
    CHECK_STR(r.forge, "github");
    CHECK(!r.local);
    CHECK(r.local_path == NULL);
    CHECK(r.version == NULL);
    gf_pkgref_free(&r);

    /* forge-prefixed forms */
    CHECK(gf_pkgref_parse("github:owner/repo", "github", &r) == 0);
    CHECK_STR(r.forge, "github");
    CHECK_STR(r.owner, "owner");
    CHECK_STR(r.repo, "repo");
    gf_pkgref_free(&r);
    CHECK(gf_pkgref_parse("codeberg:o/r", "github", &r) == 0);
    CHECK_STR(r.forge, "codeberg");
    gf_pkgref_free(&r);
    CHECK(gf_pkgref_parse("gitlab:o/r", "github", &r) == 0);
    CHECK_STR(r.forge, "gitlab");
    gf_pkgref_free(&r);
    CHECK(gf_pkgref_parse("gitea:o/r", "github", &r) == 0);
    CHECK_STR(r.forge, "gitea");
    gf_pkgref_free(&r);
    CHECK(gf_pkgref_parse("myforge:o/repo", "github", &r) == 0);
    CHECK_STR(r.forge, "myforge");
    CHECK_STR(r.repo, "repo");
    gf_pkgref_free(&r);

    /* version suffix */
    CHECK(gf_pkgref_parse("owner/repo@v1.2.3", "github", &r) == 0);
    CHECK_STR(r.version, "v1.2.3");
    CHECK_STR(r.repo, "repo");
    CHECK_STR(r.owner, "owner");
    gf_pkgref_free(&r);
    CHECK(gf_pkgref_parse("local:/some/path@2.0", "github", &r) == 0);
    CHECK_STR(r.version, "2.0");
    CHECK_STR(r.local_path, "/some/path");
    gf_pkgref_free(&r);

    /* git+https and https URLs */
    CHECK(gf_pkgref_parse("git+https://gitlab.com/group/proj.git", "github",
                          &r) == 0);
    CHECK_STR(r.host, "gitlab.com");
    CHECK_STR(r.owner, "group");
    CHECK_STR(r.repo, "proj");
    CHECK_STR(r.url, "git+https://gitlab.com/group/proj.git");
    gf_pkgref_free(&r);
    CHECK(gf_pkgref_parse("https://example.com/repos/tool.git", "github",
                          &r) == 0);
    CHECK_STR(r.host, "example.com");
    CHECK_STR(r.repo, "tool");
    gf_pkgref_free(&r);

    /* local: references */
    CHECK(gf_pkgref_parse("local:/abs/checkout", "github", &r) == 0);
    CHECK(r.local);
    CHECK_STR(r.local_path, "/abs/checkout");
    CHECK_STR(r.repo, "checkout");
    gf_pkgref_free(&r);

    /* bare name (installed lookup) */
    CHECK(gf_pkgref_parse("hello", "github", &r) == 0);
    CHECK_STR(r.repo, "hello");
    CHECK(r.owner == NULL);
    CHECK_STR(r.forge, "github");
    gf_pkgref_free(&r);
    CHECK(gf_pkgref_parse("hello", NULL, &r) == 0);
    CHECK(r.forge == NULL);
    gf_pkgref_free(&r);

    /* ---------------- invalid forms ---------------- */
    CHECK(gf_pkgref_parse("", "github", &r) == -1);
    gf_pkgref_free(&r);
    CHECK(gf_pkgref_parse(NULL, "github", &r) == -1);
    gf_pkgref_free(&r);
    CHECK(gf_pkgref_parse("http://insecure/x", "github", &r) == -1);
    gf_pkgref_free(&r);
    CHECK(gf_pkgref_parse("git+http://insecure/x", "github", &r) == -1);
    gf_pkgref_free(&r);
    CHECK(gf_pkgref_parse("ssh://git@host/x", "github", &r) == -1);
    gf_pkgref_free(&r);
    CHECK(gf_pkgref_parse("git@host:x.git", "github", &r) == -1);
    gf_pkgref_free(&r);
    CHECK(gf_pkgref_parse("github:onlyrepo", "github", &r) == -1);
    gf_pkgref_free(&r);
    CHECK(gf_pkgref_parse("/leading/slash", "github", &r) == -1);
    gf_pkgref_free(&r);
    CHECK(gf_pkgref_parse("a/b/c", "github", &r) == -1);
    gf_pkgref_free(&r);
    CHECK(gf_pkgref_parse("local:", "github", &r) == -1);
    gf_pkgref_free(&r);

    /* ---------------- helpers ---------------- */
    CHECK(gf_pkgref_parse("github:o/repo", "github", &r) == 0);
    char *name = gf_pkgref_name(&r);
    CHECK_STR(name, "repo");
    free(name);
    char *url = gf_pkgref_clone_url(&r, "https://github.com");
    CHECK_STR(url, "https://github.com/o/repo");
    free(url);
    gf_pkgref c = gf_pkgref_copy(&r);
    CHECK_STR(c.repo, "repo");
    CHECK_STR(c.raw, "github:o/repo");
    gf_pkgref_free(&c);
    gf_pkgref_free(&r);
    return 0;
}
