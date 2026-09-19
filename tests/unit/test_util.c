/* test_util.c — strbuf, strings, paths, fs helpers, exec. */
#include "checks.h"
#include "tests.h"

#include <sys/stat.h>
#include <unistd.h>

int test_util(void)
{
    /* ---------------- strbuf ---------------- */
    {
        gf_strbuf sb;
        gf_strbuf_init(&sb);
        CHECK_STR(gf_strbuf_str(&sb), "");
        CHECK(gf_strbuf_append(&sb, "hello") == 0);
        CHECK(gf_strbuf_appendc(&sb, ' ') == 0);
        CHECK(gf_strbuf_append(&sb, "world") == 0);
        CHECK_STR(gf_strbuf_str(&sb), "hello world");
        CHECK_INT(sb.len, 11);
        /* appendn must be binary-safe (embedded NULs) */
        const char bin[] = { 'a', '\0', 'b', '\0', 'c' };
        gf_strbuf_reset(&sb);
        CHECK(gf_strbuf_appendn(&sb, bin, sizeof(bin)) == 0);
        CHECK_INT(sb.len, 5);
        CHECK(memcmp(gf_strbuf_str(&sb), bin, 5) == 0);
        CHECK(gf_strbuf_appendf(&sb, "%d-%s", 42, "x") == 0);
        CHECK_INT(sb.len, 9);
        uint8_t bytes[2] = { 0x00, 0xff };
        gf_strbuf_reset(&sb);
        CHECK(gf_strbuf_append_hex(&sb, bytes, 2) == 0);
        CHECK_STR(gf_strbuf_str(&sb), "00ff");
        char *stolen = gf_strbuf_steal(&sb);
        CHECK_STR(stolen, "00ff");
        free(stolen);
        CHECK_STR(gf_strbuf_str(&sb), "");
        gf_strbuf_free(&sb);
    }

    /* ---------------- strings ---------------- */
    {
        CHECK(gf_str_eq("a", "a"));
        CHECK(!gf_str_eq("a", "b"));
        CHECK(!gf_str_eq("a", NULL));
        CHECK(gf_str_ieq("AbC", "aBc"));
        CHECK(gf_str_starts_with("foobar", "foo"));
        CHECK(!gf_str_starts_with("foo", "foobar"));
        CHECK(gf_str_ends_with("foobar", "bar"));
        char t1[] = "  \t spaced \n ";
        CHECK_STR(gf_str_trim(t1), "spaced");
        /* natural ordering: digit runs compare numerically */
        CHECK(gf_str_naturalcmp("a2", "a10") < 0);
        CHECK(gf_str_naturalcmp("a10", "a2") > 0);
        CHECK(gf_str_naturalcmp("a10", "a10") == 0);
        CHECK(gf_str_naturalcmp("abc", "abd") < 0);
        size_t n = 0;
        char **parts = gf_str_split("a,,b;c", ",;", &n);
        CHECK_INT(n, 3);
        CHECK_STR(parts[0], "a");
        CHECK_STR(parts[1], "b");
        CHECK_STR(parts[2], "c");
        char *joined = gf_strv_join(parts, n, ':');
        CHECK_STR(joined, "a:b:c");
        free(joined);
        gf_strv_free(parts, n);
        char *enc = gf_url_encode("a b~c/d?e");
        CHECK_STR(enc, "a%20b~c%2Fd%3Fe");
        free(enc);
        /* shell quoting: single quotes must never break out */
        char *q = gf_str_shell_quote("plain");
        CHECK_STR(q, "'plain'");
        free(q);
        q = gf_str_shell_quote("");
        CHECK_STR(q, "''");
        free(q);
        q = gf_str_shell_quote("it's");
        CHECK_STR(q, "'it'\\''s'");
        free(q);
        q = gf_str_shell_quote("a;rm -rf /");
        CHECK_STR(q, "'a;rm -rf /'");
        free(q);
        q = gf_str_shell_quote("$(x)`y`|z&");
        CHECK_STR(q, "'$(x)`y`|z&'");
        free(q);
    }

    /* ---------------- paths ---------------- */
    {
        char *p = gf_path_join("/a", "b");
        CHECK_STR(p, "/a/b");
        free(p);
        p = gf_path_join("/a/", "/b/");
        CHECK_STR(p, "/a/b/"); /* trailing slash in b is preserved */
        free(p);
        p = gf_path_join_multi("/a", "b", "c", NULL);
        CHECK_STR(p, "/a/b/c");
        free(p);
        p = gf_path_normalize("/a/./b/../b//c/");
        CHECK_STR(p, "/a/b/c");
        free(p);
        p = gf_path_normalize("a/../../b");
        CHECK_STR(p, "../b"); /* leading .. kept in relative paths */
        free(p);
        p = gf_path_normalize("/../..");
        CHECK_STR(p, "/");
        free(p);
        CHECK(gf_path_is_abs("/x"));
        CHECK(!gf_path_is_abs("x"));
        CHECK(gf_path_inside("/usr/local", "/usr/local/bin/x", false));
        CHECK(gf_path_inside("/usr/local", "/usr/local", true));
        CHECK(!gf_path_inside("/usr/local", "/usr/local", false));
        CHECK(!gf_path_inside("/usr/local", "/usr/localshare/x", false));
        p = gf_path_dirname("/a/b/c");
        CHECK_STR(p, "/a/b");
        free(p);
        p = gf_path_dirname("/");
        CHECK_STR(p, "/");
        free(p);
        p = gf_path_basename("/a/b/c");
        CHECK_STR(p, "c");
        free(p);
        p = gf_path_basename("/");
        CHECK_STR(p, "");
        free(p);
        CHECK(gf_path_component_ok("name"));
        CHECK(!gf_path_component_ok(".."));
        CHECK(!gf_path_component_ok("."));
        CHECK(!gf_path_component_ok("a/b"));
        CHECK(!gf_path_component_ok(""));
    }

    /* ---------------- filesystem ---------------- */
    {
        char *dir = test_tmpdir("fs");
        char *deep = gf_path_join(dir, "a/b/c/d");
        CHECK(gf_fs_mkdir_p(deep) == 0);
        CHECK(gf_fs_is_dir(deep));
        CHECK(!gf_fs_is_file(deep));
        char *f = gf_path_join(deep, "f.txt");
        const char payload[] = "l\0ne"; /* embedded NUL: 4 bytes */
        CHECK(gf_fs_write_file_atomic(f, payload, 4) == 0);
        size_t len = 0;
        char *back = gf_fs_read_file(f, &len);
        CHECK(back != NULL);
        CHECK_INT(len, 4);
        CHECK(memcmp(back, payload, 4) == 0);
        free(back);
        /* limit read */
        back = gf_fs_read_file_limit(f, 2, &len);
        CHECK(back == NULL || len == 0 || len == 2); /* limit enforced */
        free(back);
        /* copy preserves content and mode */
        char *f2 = gf_path_join(dir, "f2.txt");
        CHECK(gf_fs_copy_file(f, f2, 0600) == 0);
        struct stat st;
        CHECK(lstat(f2, &st) == 0 && (st.st_mode & 0777) == 0600);
        /* reflink_or_copy: same content, INDEPENDENT inode (store-activation
         * semantics: modifying the live copy must never touch the source) */
        char *f3 = gf_path_join(dir, "f3.txt");
        CHECK(gf_fs_reflink_or_copy(f, f3, 0644) == 0);
        {
            size_t rl = 0;
            char *rb = gf_fs_read_file(f3, &rl);
            CHECK(rb != NULL && rl == 4 && memcmp(rb, payload, 4) == 0);
            free(rb);
        }
        test_write(f3, "MUTATED");
        {
            size_t rl = 0;
            char *rb = gf_fs_read_file(f, &rl);
            CHECK(rb != NULL && rl == 4 && memcmp(rb, payload, 4) == 0);
            free(rb);
        }
        free(f3);
        /* trees: subdir + symlink */
        char *tree = gf_path_join(dir, "tree");
        CHECK(gf_fs_mkdir_p(tree) == 0);
        test_wf(tree, "x", "x");
        test_wf(tree, "sub/y", "y");
        char *lnk1 = gf_path_join(tree, "link");
        CHECK(gf_fs_symlink("x", lnk1) == 0);
        CHECK(gf_fs_is_symlink(lnk1));
        free(lnk1);
        char *tree2 = gf_path_join(dir, "tree2");
        CHECK(gf_fs_copy_tree(tree, tree2) == 0);
        {
            char *lnk2 = gf_path_join(tree2, "link");
            CHECK(gf_fs_is_symlink(lnk2));
            free(lnk2);
        }
        char *sypath = gf_path_join(tree2, "sub/y");
        back = test_read(sypath);
        CHECK_STR(back, "y");
        free(back);
        free(sypath);
        char *tree3 = gf_path_join(dir, "tree3");
        CHECK(gf_fs_hardlink_tree(tree, tree3) == 0);
        char *txpath = gf_path_join(tree3, "x");
        back = test_read(txpath);
        CHECK_STR(back, "x");
        free(back);
        free(txpath);
        /* hardlink means shared inode */
        struct stat s1, s2;
        char *x1 = gf_path_join(tree, "x");
        char *x3 = gf_path_join(tree3, "x");
        CHECK(lstat(x1, &s1) == 0);
        CHECK(lstat(x3, &s2) == 0);
        free(x1);
        free(x3);
        CHECK(s1.st_ino == s2.st_ino);
        /* rm_rf must not follow a symlink pointing outside the tree */
        char *outside = gf_path_join(dir, "outside.txt");
        test_write(outside, "precious");
        char *trap = gf_path_join(dir, "trap");
        CHECK(gf_fs_mkdir_p(trap) == 0);
        {
            char *sneak = gf_path_join(trap, "sneaky");
            CHECK(gf_fs_symlink(outside, sneak) == 0);
            free(sneak);
        }
        CHECK(gf_fs_rm_rf(trap) == 0);
        CHECK(!gf_fs_exists(trap));
        CHECK(gf_fs_is_file(outside)); /* target survived */
        char *ocontent = test_read(outside);
        CHECK_STR(ocontent, "precious");
        free(ocontent);
        /* list_dir */
        size_t count = 0;
        char **entries = NULL;
        CHECK(gf_fs_list_dir(tree, &entries, &count) == 0);
        CHECK_INT(count, 3); /* x, sub, link */
        gf_strv_free(entries, count);
        uint64_t du = 0;
        CHECK(gf_fs_du(tree, &du) == 0);
        CHECK(du > 0);
        CHECK(gf_fs_rm_rf(dir) == 0);
        CHECK(!gf_fs_exists(dir));
        free(outside);
        free(trap);
        free(deep);
        free(tree3);
        free(tree2);
        free(tree);
        free(f2);
        free(f);
        test_rmdir(dir);
    }

    /* ---------------- exec (no system/popen anywhere) ---------------- */
    {
        const char *argv[] = { "/bin/echo", "hello", "world", NULL };
        gf_exec_opts o = gf_exec_opts_default();
        gf_exec_result r;
        gf_strbuf out, err;
        gf_strbuf_init(&out);
        gf_strbuf_init(&err);
        o.out = &out;
        o.err = &err;
        CHECK(gf_exec_capture(argv, &o, &r) == 0);
        CHECK(!r.signaled && !r.timed_out);
        CHECK_INT(r.status, 0);
        CHECK_STR(gf_strbuf_str(&out), "hello world\n");
        gf_strbuf_free(&out);
        gf_strbuf_free(&err);
        /* exit code propagation */
        const char *argv2[] = { "/bin/sh", "-c", "exit 3", NULL };
        gf_strbuf_init(&out);
        gf_strbuf_init(&err);
        o = gf_exec_opts_default();
        o.out = &out;
        o.err = &err;
        CHECK(gf_exec_capture(argv2, &o, &r) == 0);
        CHECK_INT(r.status, 3);
        gf_strbuf_free(&out);
        gf_strbuf_free(&err);
        /* timeout kills the whole group */
        const char *argv3[] = { "/bin/sleep", "30", NULL };
        gf_strbuf_init(&out);
        gf_strbuf_init(&err);
        o = gf_exec_opts_default();
        o.out = &out;
        o.err = &err;
        o.timeout_ms = 150;
        CHECK(gf_exec_capture(argv3, &o, &r) == 0);
        CHECK(r.timed_out);
        CHECK(r.signaled);
        gf_strbuf_free(&out);
        gf_strbuf_free(&err);
        /* nonexistent binary */
        const char *argv4[] = { "/nonexistent-gitfull-probe", NULL };
        gf_strbuf_init(&out);
        gf_strbuf_init(&err);
        o = gf_exec_opts_default();
        o.out = &out;
        o.err = &err;
        /* spawn of nonexistent binary: child reports exit 127 */
        CHECK(gf_exec(argv4, &o, &r) == 0);
        CHECK(!r.timed_out && !r.signaled);
        CHECK_INT(r.status, 127);
        gf_strbuf_free(&out);
        gf_strbuf_free(&err);
        /* argv dup/len */
        char **dup = gf_argv_dup(argv);
        CHECK_INT((long)gf_argv_len((const char *const *)dup), 3);
        gf_strv_free(dup, 3);
    }

    /* ---------------- hex + misc ---------------- */
    {
        uint8_t bin[4] = { 0x00, 0x12, 0xab, 0xff };
        char hex[9];
        CHECK_INT(gf_hex_encode(hex, sizeof(hex), bin, 4), 8);
        CHECK_STR(hex, "0012abff");
        uint8_t back[4];
        CHECK(gf_hex_decode(back, sizeof(back), "0012abff", 8) == 0);
        CHECK(memcmp(back, bin, 4) == 0);
        CHECK(gf_hex_decode(back, sizeof(back), "00zz", 4) == -1);
        CHECK(gf_hex_decode(back, sizeof(back), "0012ABFF", 8) == 0);
        CHECK(gf_hex_decode(back, sizeof(back), "0", 1) == -1);
        CHECK_INT(gf_ct_memcmp("abc", "abc", 3), 0);
        char iso[32];
        gf_time_iso8601_now(iso, sizeof(iso));
        CHECK(strlen(iso) == 20);
        CHECK(iso[4] == '-' && iso[10] == 'T' && iso[19] == 'Z');
    }
    return 0;
}
