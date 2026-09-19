/* depgraph.h — dependency graph with cycle detection and topo ordering.
 *
 * Nodes are packages (name + resolved-or-unresolved version). Edges come
 * from recipes and dependency discovery. Shared dependencies resolve once
 * (A->B->C, A->D->C keeps a single C node).
 */
#ifndef GF_DEPGRAPH_H
#define GF_DEPGRAPH_H

#include "version.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    GF_DEP_NEW = 0,
    GF_DEP_VISITING,
    GF_DEP_DONE
} gf_dep_state;

typedef struct gf_dep_node {
    char *name;
    char *version;        /* exact version when resolved, else NULL */
    char *spec;           /* original spec string (NULL when root) */
    gf_vcset want;        /* accumulated constraints */
    bool build_dep;       /* true when only needed at build time */
    gf_dep_state state;
    struct gf_dep_node **deps;   /* edges to dependencies */
    size_t ndeps;
    /* resolution payload (filled by resolver) */
    char *owner;          /* forge owner when known */
    char *forge;          /* repository name when known */
} gf_dep_node;

typedef struct gf_dep_graph {
    gf_dep_node **nodes;
    size_t n;
} gf_dep_graph;

gf_dep_graph *gf_depgraph_new(void);
void gf_depgraph_free(gf_dep_graph *g);

/* Find node by name. */
gf_dep_node *gf_depgraph_find(gf_dep_graph *g, const char *name);

/* Get or create a node; spec is duplicated into the node when created. */
gf_dep_node *gf_depgraph_require(gf_dep_graph *g, const char *name,
                                 const char *spec, bool build_dep);

/* Add edge parent -> dep (deduplicated; ignores self-edges). */
int gf_depgraph_add_edge(gf_dep_graph *g, gf_dep_node *parent,
                         gf_dep_node *dep);

/* Detect cycles. On success returns 0. On cycle returns -1 and fills
 * *cycle_chain with a malloc'd array of node names forming the chain
 * (e.g. foo, bar, baz, foo). */
int gf_depgraph_check_cycles(gf_dep_graph *g, char ***cycle_chain,
                             size_t *cycle_len);

/* Topological order (dependencies first). Returns -1 on cycles. */
int gf_depgraph_topo(gf_dep_graph *g, gf_dep_node ***order, size_t *n);

/* Check version conflicts: same package with incompatible constraints.
 * Returns -1 and logs the conflicting node + accumulated spec. */
int gf_depgraph_check_conflicts(gf_dep_graph *g);

/* Render a dependency tree (for `gitfull install --dry-run` style output). */
char *gf_depgraph_render(const gf_dep_graph *g);

#endif /* GF_DEPGRAPH_H */
