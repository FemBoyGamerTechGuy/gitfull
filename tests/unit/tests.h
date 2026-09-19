/* tests.h — declarations of all unit test entry points. */
#ifndef GF_TESTS_H
#define GF_TESTS_H

int test_util(void);
int test_json(void);
int test_ini(void);
int test_sha256(void);
int test_version(void);
int test_pkgid(void);
int test_toml(void);
int test_tar(void);
int test_depgraph(void);
int test_package(void);
int test_xact(void);
int test_seccomp(void);

#endif /* GF_TESTS_H */
