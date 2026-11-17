// sandbox.c - after start-up the server only needs to: accept connections, read and write
// sockets, keep time, and save files in its own folder. This file makes Linux enforce that,
// so even if someone finds a bug in the C code, they can't use the server for anything else.
//
//   Landlock  (Linux 5.13+): files only inside the current folder (+ /tmp in dev for hot
//             reload). No new network connections out, no new ports.
//   seccomp   (Linux 3.5+):  an allowlist of system calls. Anything else stops the server
//             with a clear message instead of running.
// Both are built into the kernel: no library needed.
#define _GNU_SOURCE
#include "sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static char status[96] = "off";
const char *fk_sandbox_status(void) { return status; }

// ---------------------------------------------------------------- Landlock
static int landlock_abi(void) {
#ifdef SYS_landlock_create_ruleset
    return (int)syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
#else
    return -1;
#endif
}

static int allow_dir(int ruleset, const char *path, uint64_t access) {
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return -1;
    struct landlock_path_beneath_attr pb = {.allowed_access = access, .parent_fd = fd};
    int r = (int)syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
    close(fd);
    return r;
}

static int landlock(void) {
    int abi = landlock_abi();
    if (abi < 1) return 0;
    uint64_t fs = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
                  LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                  LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
                  LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                  LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2) fs |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3) fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
    struct landlock_ruleset_attr attr = {.handled_access_fs = fs};
    size_t attr_size = offsetof(struct landlock_ruleset_attr, handled_access_fs) + sizeof attr.handled_access_fs;
#ifdef LANDLOCK_ACCESS_NET_BIND_TCP
    if (abi >= 4) {   // handle network rights and grant none: no new ports, no connections out
        attr.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
        attr_size = offsetof(struct landlock_ruleset_attr, handled_access_net) + sizeof attr.handled_access_net;
    }
#endif
    int rs = (int)syscall(SYS_landlock_create_ruleset, &attr, attr_size, 0);
    if (rs < 0) return 0;
    uint64_t here = fs & ~(uint64_t)(LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_MAKE_CHAR |
                                     LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SOCK |
                                     LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_SYM);
    int ok = allow_dir(rs, ".", here) == 0;   // its own folder: snapshots, recordings
#ifndef RELEASE
    ok = ok && allow_dir(rs, "/tmp", here) == 0;  // dev: private copies of libapp.so for hot reload
#endif
    if (ok && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && syscall(SYS_landlock_restrict_self, rs, 0) == 0) {
        close(rs);
        return abi;
    }
    close(rs);
    return 0;
}

// ---------------------------------------------------------------- seccomp
static void on_sigsys(int sig, siginfo_t *info, void *ctx) {
    (void)sig; (void)ctx;
    char msg[96];
    int n = snprintf(msg, sizeof msg, "\nsandbox: blocked system call %d. Stopping.\n", info->si_syscall);
    if (n > 0) { ssize_t w = write(2, msg, (size_t)n); (void)w; }
    _exit(70);
}

#define ALLOW(nr) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

static int seccomp(void) {
#if defined(__x86_64__)
    const uint32_t arch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    const uint32_t arch = AUDIT_ARCH_AARCH64;
#else
    return 0;
#endif
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, arch, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        // network loop
        ALLOW(SYS_epoll_ctl), ALLOW(SYS_epoll_pwait), ALLOW(SYS_accept4), ALLOW(SYS_setsockopt),
        ALLOW(SYS_recvfrom), ALLOW(SYS_sendto), ALLOW(SYS_read), ALLOW(SYS_write), ALLOW(SYS_writev), ALLOW(SYS_close),
#ifdef SYS_epoll_wait
        ALLOW(SYS_epoll_wait),
#endif
        // files in its own folder (snapshots, recording) and the hot-reload copy
        ALLOW(SYS_openat), ALLOW(SYS_fstat), ALLOW(SYS_newfstatat), ALLOW(SYS_statx), ALLOW(SYS_lseek),
        ALLOW(SYS_renameat), ALLOW(SYS_renameat2), ALLOW(SYS_unlinkat), ALLOW(SYS_ioctl), ALLOW(SYS_pread64),
#ifdef SYS_rename
        ALLOW(SYS_rename), ALLOW(SYS_unlink), ALLOW(SYS_stat),
#endif
        // memory and time
        ALLOW(SYS_brk), ALLOW(SYS_mmap), ALLOW(SYS_munmap), ALLOW(SYS_mprotect), ALLOW(SYS_madvise), ALLOW(SYS_mremap),
        ALLOW(SYS_clock_gettime), ALLOW(SYS_futex), ALLOW(SYS_getrandom),
        // signals and stopping (Ctrl+C, abort() on a failed check)
        ALLOW(SYS_rt_sigreturn), ALLOW(SYS_rt_sigprocmask), ALLOW(SYS_getpid), ALLOW(SYS_gettid), ALLOW(SYS_tgkill),
        ALLOW(SYS_exit), ALLOW(SYS_exit_group), ALLOW(SYS_restart_syscall),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),   // anything else: SIGSYS -> clear message, stop
    };
    struct sock_fprog prog = {.len = (unsigned short)(sizeof f / sizeof f[0]), .filter = f};
    struct sigaction sa = {.sa_sigaction = on_sigsys, .sa_flags = SA_SIGINFO};
    sigaction(SIGSYS, &sa, NULL);
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return 0;
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) == 0;
}

void fk_sandbox(void) {
    int ll = landlock();
    int sc = seccomp();
    if (ll && sc) snprintf(status, sizeof status, "on (seccomp + Landlock v%d)", ll);
    else if (sc) snprintf(status, sizeof status, "on (seccomp; this kernel has no Landlock)");
    else if (ll) snprintf(status, sizeof status, "partial (Landlock v%d, no seccomp)", ll);
    else snprintf(status, sizeof status, "off (not supported here)");
}
