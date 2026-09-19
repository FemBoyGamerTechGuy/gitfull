#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void) {
    int fails = 0;
    if (mount("none", "/tmp", "tmpfs", 0, "") == 0) {
        printf("UNFILTERED: mount succeeded\n");
        fails++;
    } else {
        printf("mount blocked: %s\n", strerror(errno));
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        printf("UNFILTERED: inet socket created\n");
        fails++;
    } else {
        printf("inet socket blocked: %s\n", strerror(errno));
    }
    if (chroot("/") == 0) {
        printf("UNFILTERED: chroot succeeded\n");
        fails++;
    } else {
        printf("chroot blocked: %s\n", strerror(errno));
    }
    return fails;
}
