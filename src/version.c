/* version.c — semver parsing, comparison, stable selection, constraints. */
#include "version.h"

#include "common.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void gf_version_free(gf_version *v)
{
    if (!v)
        return;
    free(v->prerelease);
    free(v->raw);
    memset(v, 0, sizeof(*v));
}

gf_version gf_version_copy(const gf_version *v)
{
    gf_version out = { v->major, v->minor, v->patch,
                       v->prerelease ? gf_strdup(v->prerelease) : NULL,
                       v->raw ? gf_strdup(v->raw) : NULL };
    return out;
}

static bool parse_digits(const char *s, size_t len, int *out)
{
    if (len == 0 || len > 9)
        return false;
    long v = 0;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)s[i]))
            return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = (int)v;
    return true;
}

bool gf_version_parse(const char *s, gf_version *out)
{
    memset(out, 0, sizeof(*out));
    if (!s || !*s)
        return false;
    const char *p = s;
    /* optional prefix: v / V / release- / rel- / r (digit must follow) */
    if ((*p == 'v' || *p == 'V') && isdigit((unsigned char)p[1]))
        p++;
    else if (strncmp(p, "release-", 8) == 0 && isdigit((unsigned char)p[8]))
        p += 8;
    else if (strncmp(p, "rel-", 4) == 0 && isdigit((unsigned char)p[4]))
        p += 4;

    /* strip build metadata */
    const char *plus = strchr(p, '+');
    size_t corelen = plus ? (size_t)(plus - p) : strlen(p);

    /* split core from prerelease */
    const char *dash = memchr(p, '-', corelen);
    size_t coreend = dash ? (size_t)(dash - p) : corelen;

    /* numeric core: MAJOR[.MINOR[.PATCH]] */
    const char *core = p;
    const char *cend = p + coreend;
    int nums[3] = { 0, 0, 0 };
    int nnum = 0;
    const char *q = core;
    while (q < cend) {
        const char *start = q;
        while (q < cend && isdigit((unsigned char)*q))
            q++;
        if (q == start)
            return false; /* non-numeric component */
        if (nnum >= 3)
            return false; /* too many components */
        if (!parse_digits(start, (size_t)(q - start), &nums[nnum]))
            return false;
        nnum++;
        if (q < cend) {
            if (*q != '.')
                return false;
            q++;
            if (q == cend)
                return false; /* trailing dot */
        }
    }
    if (nnum == 0)
        return false;

    out->major = nums[0];
    out->minor = nnum >= 2 ? nums[1] : 0;
    out->patch = nnum >= 3 ? nums[2] : 0;
    if (dash)
        out->prerelease = gf_strndup(dash + 1, corelen - (size_t)(dash - p) - 1);
    out->raw = gf_strdup(s);
    return true;
}

/* semver prerelease identifier comparison */
static int ident_cmp(const char *a, const char *b)
{
    bool anum = *a && isdigit((unsigned char)a[0]);
    bool bnum = *b && isdigit((unsigned char)b[0]);
    if (anum && bnum) {
        /* numeric compare, drop leading zeros semantically */
        while (*a == '0')
            a++;
        while (*b == '0')
            b++;
        size_t la = strlen(a), lb = strlen(b);
        if (la != lb)
            return la < lb ? -1 : 1;
        int c = strcmp(a, b);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    if (anum)
        return -1; /* numeric < alphanumeric */
    if (bnum)
        return 1;
    int c = strcmp(a, b);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

static int prerelease_cmp(const char *a, const char *b)
{
    /* empty (stable) > any prerelease */
    if (!a && !b)
        return 0;
    if (!a)
        return 1;
    if (!b)
        return -1;
    const char *pa = a, *pb = b;
    for (;;) {
        if (!*pa && !*pb)
            return 0;
        if (!*pa)
            return -1; /* fewer identifiers = lower precedence */
        if (!*pb)
            return 1;
        const char *ea = strchr(pa, '.');
        const char *eb = strchr(pb, '.');
        size_t la = ea ? (size_t)(ea - pa) : strlen(pa);
        size_t lb = eb ? (size_t)(eb - pb) : strlen(pb);
        char *ida = gf_strndup(pa, la);
        char *idb = gf_strndup(pb, lb);
        int c = ident_cmp(ida, idb);
        free(ida);
        free(idb);
        if (c != 0)
            return c;
        pa += la;
        pb += lb;
        if (*pa)
            pa++; /* skip '.' */
        if (*pb)
            pb++;
    }
}

int gf_version_cmp(const gf_version *a, const gf_version *b)
{
    if (a->major != b->major)
        return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor)
        return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch)
        return a->patch < b->patch ? -1 : 1;
    return prerelease_cmp(a->prerelease, b->prerelease);
}

bool gf_version_is_stable(const gf_version *v)
{
    return v->prerelease == NULL || v->prerelease[0] == '\0';
}

bool gf_version_tag_stable(const char *tag)
{
    gf_version v;
    if (!gf_version_parse(tag, &v))
        return false;
    bool stable = gf_version_is_stable(&v);
    gf_version_free(&v);
    return stable;
}

char *gf_version_best_tag(char *const *tags, size_t n, bool allow_prerelease)
{
    gf_version best;
    bool have = false;
    char *best_tag = NULL;
    for (size_t i = 0; i < n; i++) {
        gf_version v;
        if (!gf_version_parse(tags[i], &v))
            continue;
        if (!allow_prerelease && !gf_version_is_stable(&v)) {
            gf_version_free(&v);
            continue;
        }
        if (!have || gf_version_cmp(&v, &best) > 0) {
            if (have)
                gf_version_free(&best);
            best = v;
            have = true;
            free(best_tag);
            best_tag = gf_strdup(tags[i]);
        } else {
            gf_version_free(&v);
        }
    }
    if (have)
        gf_version_free(&best);
    return best_tag;
}

/* ----------------------------------------------------------- constraints */

bool gf_vc_parse(const char *spec, gf_vc *out)
{
    memset(out, 0, sizeof(*out));
    if (!spec)
        return false;
    const char *p = spec;
    gf_vc_op op = GF_VC_EXACT;
    if (*p == '=') {
        op = GF_VC_EXACT;
        p += p[1] == '=' ? 2 : 1;
    } else if (p[0] == '>' && p[1] == '=') {
        op = GF_VC_GTE;
        p += 2;
    } else if (p[0] == '<' && p[1] == '=') {
        op = GF_VC_LTE;
        p += 2;
    } else if (p[0] == '>' && p[1] == '=') {
        op = GF_VC_GTE;
        p += 2;
    } else if (*p == '>') {
        op = GF_VC_GT;
        p += 1;
    } else if (*p == '<') {
        op = GF_VC_LT;
        p += 1;
    } else if (p[0] == '!' && p[1] == '=') {
        op = GF_VC_NE;
        p += 2;
    } else if (*p == '!') {
        op = GF_VC_NE;
        p += 1;
    } else if (*p == '^') {
        op = GF_VC_CARET;
        p += 1;
    } else if (*p == '~') {
        op = GF_VC_TILDE;
        p += 1;
    }
    if (*p == '\0') {
        out->op = GF_VC_ANY;
        out->ver.raw = gf_strdup(spec);
        return true;
    }
    if (strcmp(p, "*") == 0 || strcmp(p, "x") == 0) {
        out->op = GF_VC_ANY;
        out->ver.raw = gf_strdup(spec);
        return true;
    }
    /* wildcard handling: 1.2.x / 1.x / 1.2.* */
    char *copy = gf_strdup(p);
    size_t nseg = 1;
    bool wild = false;
    for (char *q = copy; *q; q++) {
        if (*q == '.')
            nseg++;
        if (*q == 'x' || *q == 'X' || *q == '*')
            wild = true;
    }
    if (wild) {
        /* convert to >=lower bound, <upper bound via caret/tilde-like ops */
        /* "1.2.x" == >=1.2.0 <1.3.0 ; "1.x" == >=1.0.0 <2.0.0 */
        char *xpos = strpbrk(copy, "xX*");
        if (!xpos) {
            free(copy);
            return false;
        }
        /* replace wildcard and everything after with 0 */
        *xpos = '\0';
        if (xpos > copy && *(xpos - 1) == '.')
            *(xpos - 1) = '\0';
        gf_version lo;
        if (!gf_version_parse(copy[0] ? copy : "0", &lo)) {
            free(copy);
            return false;
        }
        out->ver = lo;
        free(out->ver.raw); /* raw from parse; replaced by the spec string */
        out->ver.raw = gf_strdup(spec);
        out->op = nseg >= 3 ? GF_VC_TILDE : GF_VC_CARET;
        out->has_patch = nseg >= 3;
        out->has_minor = nseg >= 2;
        free(copy);
        return true;
    }
    gf_version v;
    if (!gf_version_parse(copy, &v)) {
        free(copy);
        return false;
    }
    free(copy);
    out->op = op;
    out->ver = v;
    /* count segments for ^/~ semantics */
    const char *vp = p;
    int segs = 0;
    while (*vp) {
        if (*vp == '.')
            segs++;
        vp++;
    }
    out->has_minor = segs >= 1;
    out->has_patch = segs >= 2;
    return true;
}

void gf_vc_free(gf_vc *c)
{
    gf_version_free(&c->ver);
    memset(c, 0, sizeof(*c));
}

bool gf_vc_match(const gf_vc *c, const gf_version *v)
{
    if (c->op == GF_VC_ANY)
        return true;
    int cmp = gf_version_cmp(v, &c->ver);
    switch (c->op) {
    case GF_VC_EXACT:
        return cmp == 0;
    case GF_VC_GTE:
        return cmp >= 0;
    case GF_VC_LTE:
        return cmp <= 0;
    case GF_VC_GT:
        return cmp > 0;
    case GF_VC_LT:
        return cmp < 0;
    case GF_VC_NE:
        return cmp != 0;
    case GF_VC_CARET:
        if (cmp < 0)
            return false;
        if (c->ver.major > 0)
            return v->major == c->ver.major;
        /* ^0.x semantics: minor must match if specified */
        if (c->has_minor)
            return v->major == 0 && v->minor == c->ver.minor;
        return v->major == 0;
    case GF_VC_TILDE:
        if (cmp < 0)
            return false;
        if (v->major != c->ver.major)
            return false;
        if (c->has_minor)
            return v->minor == c->ver.minor;
        return true;
    case GF_VC_ANY:
    default:
        return false;
    }
}

bool gf_vcset_parse(const char *spec, gf_vcset *out)
{
    out->cons = NULL;
    out->n = 0;
    if (!spec || !*spec)
        return true;
    char **parts = NULL;
    size_t nparts = 0;
    parts = gf_str_split(spec, ",", &nparts);
    out->cons = gf_calloc(nparts ? nparts : 1, sizeof(gf_vc));
    for (size_t i = 0; i < nparts; i++) {
        char *one = gf_str_trim(parts[i]);
        if (!*one) {
            gf_strv_free(parts, nparts);
            return false;
        }
        if (!gf_vc_parse(one, &out->cons[out->n])) {
            gf_strv_free(parts, nparts);
            return false;
        }
        out->n++;
    }
    gf_strv_free(parts, nparts);
    return true;
}

void gf_vcset_free(gf_vcset *set)
{
    if (!set)
        return;
    for (size_t i = 0; i < set->n; i++)
        gf_vc_free(&set->cons[i]);
    free(set->cons);
    set->cons = NULL;
    set->n = 0;
}

bool gf_vcset_match(const gf_vcset *set, const gf_version *v)
{
    for (size_t i = 0; i < set->n; i++) {
        if (!gf_vc_match(&set->cons[i], v))
            return false;
    }
    return true;
}

bool gf_vcset_empty(const gf_vcset *set)
{
    return !set || set->n == 0;
}

char *gf_vcset_str(const gf_vcset *set)
{
    gf_strbuf sb;
    gf_strbuf_init(&sb);
    for (size_t i = 0; i < set->n; i++) {
        if (i > 0)
            gf_strbuf_append(&sb, ", ");
        const gf_vc *c = &set->cons[i];
        const char *ops = "";
        switch (c->op) {
        case GF_VC_EXACT: ops = "="; break;
        case GF_VC_GTE:   ops = ">="; break;
        case GF_VC_LTE:   ops = "<="; break;
        case GF_VC_GT:    ops = ">"; break;
        case GF_VC_LT:    ops = "<"; break;
        case GF_VC_NE:    ops = "!="; break;
        case GF_VC_CARET: ops = "^"; break;
        case GF_VC_TILDE: ops = "~"; break;
        case GF_VC_ANY:   ops = "*"; break;
        default:          break;
        }
        gf_strbuf_appendf(&sb, "%s%s", ops,
                          c->ver.raw ? c->ver.raw : "");
    }
    return gf_strbuf_steal(&sb);
}
