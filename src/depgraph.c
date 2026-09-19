/* depgraph.c — dependency graph: cycle detection, topo order, conflicts. */
#include "depgraph.h"

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

gf_dep_graph *gf_depgraph_new(void)
{
    gf_dep_graph *g = gf_calloc(1, sizeof(gf_dep_graph));
    return g;
}

static void node_free(gf_dep_node *n)
{
    if (!n)
        return;
    free(n->name);
    free(n->version);
    free(n->spec);
    gf_vcset_free(&n->want);
    free(n->owner);
    free(n->forge);
    free(n->deps);
    free(n);
}

void gf_depgraph_free(gf_dep_graph *g)
{
    if (!g)
        return;
    for (size_t i = 0; i < g->n; i++)
        node_free(g->nodes[i]);
    free(g->nodes);
    free(g);
}

gf_dep_node *gf_depgraph_find(gf_dep_graph *g, const char *name)
{
    for (size_t i = 0; i < g->n; i++) {
        if (strcmp(g->nodes[i]->name, name) == 0)
            return g->nodes[i];
    }
    return NULL;
}

gf_dep_node *gf_depgraph_require(gf_dep_graph *g, const char *name,
                                 const char *spec, bool build_dep)
{
    gf_dep_node *n = gf_depgraph_find(g, name);
    if (n) {
        if (spec && *spec && strcmp(spec, "*") != 0) {
            gf_vcset add;
            if (gf_vcset_parse(spec, &add)) {
                /* append constraints; the gf_vc entries (and their strings)
                 * transfer ownership from 'add' to n->want */
                n->want.cons = gf_realloc(
                    n->want.cons, (n->want.n + add.n) * sizeof(gf_vc));
                memcpy(n->want.cons + n->want.n, add.cons,
                       add.n * sizeof(gf_vc));
                n->want.n += add.n;
                free(add.cons);
                add.cons = NULL;
                add.n = 0;
            }
        }
        if (!build_dep)
            n->build_dep = false; /* runtime need dominates */
        return n;
    }
    n = gf_calloc(1, sizeof(gf_dep_node));
    n->name = gf_strdup(name);
    n->spec = spec ? gf_strdup(spec) : NULL;
    gf_vcset_parse(spec ? spec : "*", &n->want);
    n->build_dep = build_dep;
    g->nodes = gf_realloc(g->nodes, (g->n + 1) * sizeof(gf_dep_node *));
    g->nodes[g->n++] = n;
    return n;
}

int gf_depgraph_add_edge(gf_dep_graph *g, gf_dep_node *parent,
                         gf_dep_node *dep)
{
    (void)g;
    if (!parent || !dep || parent == dep)
        return 0;
    for (size_t i = 0; i < parent->ndeps; i++) {
        if (parent->deps[i] == dep)
            return 0; /* dedupe */
    }
    parent->deps =
        gf_realloc(parent->deps, (parent->ndeps + 1) * sizeof(gf_dep_node *));
    parent->deps[parent->ndeps++] = dep;
    return 0;
}

/* DFS cycle detection collecting the chain. */
static int dfs(gf_dep_node *n, gf_dep_node ***stack, size_t *depth,
               size_t *cap, char ***cycle_chain, size_t *cycle_len)
{
    if (n->state == GF_DEP_VISITING) {
        /* found a cycle: chain from first occurrence of n in the stack */
        size_t start = 0;
        for (size_t i = 0; i < *depth; i++) {
            if ((*stack)[i] == n) {
                start = i;
                break;
            }
        }
        *cycle_len = *depth - start + 1;
        *cycle_chain = gf_malloc(*cycle_len * sizeof(char *));
        for (size_t i = start; i < *depth; i++)
            (*cycle_chain)[i - start] = gf_strdup((*stack)[i]->name);
        (*cycle_chain)[*depth - start] = gf_strdup(n->name);
        return -1;
    }
    if (n->state == GF_DEP_DONE)
        return 0;
    n->state = GF_DEP_VISITING;
    if (*depth == *cap) {
        *cap = *cap ? *cap * 2 : 32;
        *stack = gf_realloc(*stack, *cap * sizeof(gf_dep_node *));
    }
    (*stack)[(*depth)++] = n;
    for (size_t i = 0; i < n->ndeps; i++) {
        if (dfs(n->deps[i], stack, depth, cap, cycle_chain, cycle_len) != 0)
            return -1;
    }
    (*depth)--;
    n->state = GF_DEP_DONE;
    return 0;
}

int gf_depgraph_check_cycles(gf_dep_graph *g, char ***cycle_chain,
                             size_t *cycle_len)
{
    *cycle_chain = NULL;
    *cycle_len = 0;
    gf_dep_node **stack = NULL;
    size_t depth = 0, cap = 0;
    for (size_t i = 0; i < g->n; i++)
        g->nodes[i]->state = GF_DEP_NEW;
    for (size_t i = 0; i < g->n; i++) {
        if (g->nodes[i]->state == GF_DEP_NEW) {
            if (dfs(g->nodes[i], &stack, &depth, &cap, cycle_chain,
                    cycle_len) != 0) {
                free(stack);
                return -1;
            }
        }
    }
    free(stack);
    return 0;
}

static void topo_visit(gf_dep_node *n, gf_dep_node ***out, size_t *count,
                       size_t *cap)
{
    if (n->state != GF_DEP_NEW)
        return;
    n->state = GF_DEP_VISITING;
    for (size_t i = 0; i < n->ndeps; i++)
        topo_visit(n->deps[i], out, count, cap);
    n->state = GF_DEP_DONE;
    if (*count == *cap) {
        *cap = *cap ? *cap * 2 : 32;
        *out = gf_realloc(*out, *cap * sizeof(gf_dep_node *));
    }
    (*out)[(*count)++] = n;
}

int gf_depgraph_topo(gf_dep_graph *g, gf_dep_node ***order, size_t *n)
{
    *order = NULL;
    *n = 0;
    /* cycle check first */
    char **chain = NULL;
    size_t chain_len = 0;
    if (gf_depgraph_check_cycles(g, &chain, &chain_len) != 0) {
        gf_strv_free(chain, chain_len);
        return -1;
    }
    for (size_t i = 0; i < g->n; i++)
        g->nodes[i]->state = GF_DEP_NEW;
    gf_dep_node **out = NULL;
    size_t count = 0, cap = 0;
    for (size_t i = 0; i < g->n; i++)
        topo_visit(g->nodes[i], &out, &count, &cap);
    *order = out;
    *n = count;
    return 0;
}

int gf_depgraph_check_conflicts(gf_dep_graph *g)
{
    for (size_t i = 0; i < g->n; i++) {
        gf_dep_node *n = g->nodes[i];
        if (n->version && n->want.n > 0) {
            gf_version v;
            if (gf_version_parse(n->version, &v)) {
                if (!gf_vcset_match(&n->want, &v)) {
                    char *specs = gf_vcset_str(&n->want);
                    gf_log(GF_LOG_ERROR,
                           "dependency conflict: %s %s does not satisfy %s",
                           n->name, n->version, specs ? specs : "?");
                    free(specs);
                    gf_version_free(&v);
                    return -1;
                }
                gf_version_free(&v);
            }
        }
        /* internal consistency: pairwise constraint satisfiability */
        for (size_t a = 0; a + 1 < n->want.n; a++) {
            for (size_t b = a + 1; b < n->want.n; b++) {
                const gf_vc *ca = &n->want.cons[a];
                const gf_vc *cb = &n->want.cons[b];
                /* exact-vs-exact mismatch is a hard conflict */
                if (ca->op == GF_VC_EXACT && cb->op == GF_VC_EXACT) {
                    gf_version va = gf_version_copy(&ca->ver);
                    gf_version vb = gf_version_copy(&cb->ver);
                    int cmp = gf_version_cmp(&va, &vb);
                    gf_version_free(&va);
                    gf_version_free(&vb);
                    if (cmp != 0) {
                        gf_log(GF_LOG_ERROR,
                               "version conflict: %s requires both %s and %s",
                               n->name, ca->ver.raw, cb->ver.raw);
                        return -1;
                    }
                }
            }
        }
    }
    return 0;
}

static void render_node(const gf_dep_node *n, gf_strbuf *sb, int depth,
                        bool last)
{
    for (int i = 0; i < depth; i++)
        gf_strbuf_append(sb, "  ");
    if (depth > 0)
        gf_strbuf_append(sb, last ? "└── " : "├── ");
    gf_strbuf_append(sb, n->name);
    if (n->version)
        gf_strbuf_appendf(sb, " %s", n->version);
    else if (n->want.n > 0) {
        char *specs = gf_vcset_str(&n->want);
        gf_strbuf_appendf(sb, " [%s]", specs);
        free(specs);
    }
    if (n->build_dep)
        gf_strbuf_append(sb, " (build)");
    gf_strbuf_appendc(sb, '\n');
    for (size_t i = 0; i < n->ndeps; i++)
        render_node(n->deps[i], sb, depth + 1, i + 1 == n->ndeps);
}

char *gf_depgraph_render(const gf_dep_graph *g)
{
    gf_strbuf sb;
    gf_strbuf_init(&sb);
    /* roots: nodes nobody depends on */
    bool *is_root = gf_calloc(g->n ? g->n : 1, sizeof(bool));
    for (size_t i = 0; i < g->n; i++)
        is_root[i] = true;
    for (size_t i = 0; i < g->n; i++) {
        for (size_t k = 0; k < g->nodes[i]->ndeps; k++) {
            for (size_t j = 0; j < g->n; j++) {
                if (g->nodes[i]->deps[k] == g->nodes[j])
                    is_root[j] = false;
            }
        }
    }
    for (size_t i = 0; i < g->n; i++) {
        if (is_root[i])
            render_node(g->nodes[i], &sb, 0, true);
    }
    free(is_root);
    return gf_strbuf_steal(&sb);
}
