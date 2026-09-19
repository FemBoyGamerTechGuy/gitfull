/* test_depgraph.c — cycle detection with chains, topo order, conflicts. */
#include "checks.h"
#include "tests.h"
#include "../src/depgraph.h"

int test_depgraph(void)
{
    gf_dep_graph *g = gf_depgraph_new();

    /* A -> B -> C, A -> D -> C (shared C resolved once) */
    gf_dep_node *a = gf_depgraph_require(g, "app", NULL, false);
    gf_dep_node *b = gf_depgraph_require(g, "libb", "owner/libb@^1.0", false);
    gf_dep_node *c = gf_depgraph_require(g, "libc", "owner/libc@>=2.0", true);
    gf_dep_node *d = gf_depgraph_require(g, "libd", "owner/libd", false);
    CHECK(a && b && c && d);
    CHECK(gf_depgraph_add_edge(g, a, b) == 0);
    CHECK(gf_depgraph_add_edge(g, b, c) == 0);
    CHECK(gf_depgraph_add_edge(g, a, d) == 0);
    CHECK(gf_depgraph_add_edge(g, d, c) == 0);
    /* dedup: same edge twice is ignored */
    CHECK(gf_depgraph_add_edge(g, a, b) == 0);
    CHECK_INT(b->ndeps, 1);
    /* self-edges ignored */
    CHECK(gf_depgraph_add_edge(g, a, a) == 0);
    CHECK_INT(a->ndeps, 2);

    CHECK(gf_depgraph_find(g, "libc") == c);
    CHECK(gf_depgraph_find(g, "missing") == NULL);
    CHECK_INT(g->n, 4);

    /* no cycles here */
    char **chain = NULL;
    size_t chainlen = 0;
    CHECK(gf_depgraph_check_cycles(g, &chain, &chainlen) == 0);

    /* topo order: dependencies before dependents */
    gf_dep_node **order = NULL;
    size_t norder = 0;
    CHECK(gf_depgraph_topo(g, &order, &norder) == 0);
    CHECK_INT(norder, 4);
    if (norder == 4) {
        size_t ia = SIZE_MAX, ib = SIZE_MAX, ic = SIZE_MAX;
        for (size_t i = 0; i < norder; i++) {
            if (order[i] == a) ia = i;
            if (order[i] == b) ib = i;
            if (order[i] == c) ic = i;
        }
        CHECK(ic < ib && ib < ia); /* c before b before a */
    }
    free(order);

    /* tree rendering mentions every package */
    char *tree = gf_depgraph_render(g);
    CHECK(tree != NULL);
    if (tree) {
        CHECK(strstr(tree, "app") != NULL);
        CHECK(strstr(tree, "libc") != NULL);
        free(tree);
    }
    gf_depgraph_free(g);

    /* ---------------- cycle detection with chain ---------------- */
    {
        gf_dep_graph *cg = gf_depgraph_new();
        gf_dep_node *x = gf_depgraph_require(cg, "foo", NULL, false);
        gf_dep_node *y = gf_depgraph_require(cg, "bar", NULL, false);
        gf_dep_node *z = gf_depgraph_require(cg, "baz", NULL, false);
        gf_depgraph_add_edge(cg, x, y);
        gf_depgraph_add_edge(cg, y, z);
        gf_depgraph_add_edge(cg, z, x);
        char **ch = NULL;
        size_t chn = 0;
        CHECK(gf_depgraph_check_cycles(cg, &ch, &chn) == -1);
        CHECK(chn >= 3); /* chain shows the loop */
        if (chn >= 3) {
            /* chain must contain all three names (and loop back) */
            bool saw[3] = { false, false, false };
            for (size_t i = 0; i < chn; i++) {
                if (strcmp(ch[i], "foo") == 0) saw[0] = true;
                if (strcmp(ch[i], "bar") == 0) saw[1] = true;
                if (strcmp(ch[i], "baz") == 0) saw[2] = true;
            }
            CHECK(saw[0] && saw[1] && saw[2]);
        }
        gf_strv_free(ch, chn);
        /* topo fails on cycles */
        gf_dep_node **ord2 = NULL;
        size_t no2 = 0;
        CHECK(gf_depgraph_topo(cg, &ord2, &no2) == -1);
        gf_depgraph_free(cg);
    }

    /* ---------------- version conflicts ---------------- */
    {
        gf_dep_graph *kg = gf_depgraph_new();
        /* same package required with two incompatible exact constraints */
        gf_depgraph_require(kg, "libx", "=1.0.0", false);
        gf_dep_node *x = gf_depgraph_require(kg, "libx", "=2.0.0", false);
        CHECK(x != NULL);
        CHECK_INT(x->want.n, 2); /* both constraints accumulated */
        CHECK(gf_depgraph_check_conflicts(kg) == -1);
        gf_depgraph_free(kg);
    }

    /* resolved version violating accumulated constraints is a conflict */
    {
        gf_dep_graph *kg = gf_depgraph_new();
        gf_depgraph_require(kg, "libv", ">=2.0", false);
        gf_dep_node *v = gf_depgraph_require(kg, "libv", NULL, false);
        v->version = gf_strdup("1.5.0"); /* resolver picked a bad version */
        CHECK(gf_depgraph_check_conflicts(kg) == -1);
        gf_depgraph_free(kg);
    }

    /* no conflict when compatible */
    {
        gf_dep_graph *ok = gf_depgraph_new();
        gf_dep_node *x = gf_depgraph_require(ok, "libz", ">=1.0", false);
        gf_depgraph_require(ok, "libz", "<3.0", false);
        CHECK_INT(x->want.n, 2);
        CHECK(gf_depgraph_check_conflicts(ok) == 0);
        gf_depgraph_free(ok);
    }
    return 0;
}
