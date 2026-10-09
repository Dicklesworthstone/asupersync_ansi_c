/*
 * posix/process_posix.c — native child processes for the process API
 *
 * Spawning: fork + execve. Everything the child needs (argv, envp, the
 * resolved program path, working directory, stdio descriptors) is
 * prepared in the parent, so between fork and exec the child only makes
 * async-signal-safe calls. Signals are blocked across fork; the child
 * resets caught handlers to SIG_DFL and clears its signal mask before
 * exec. Exec (or chdir) failure is reported through a close-on-exec error
 * pipe, so spawn returns the real errno and never leaves a child behind.
 *
 * Waiting: on Linux a pidfd (pidfd_open) becomes readable when the child
 * exits and is registered with the IO driver, so a waiting task parks on
 * reactor readiness. Elsewhere (or if pidfd_open is unavailable, or with
 * ASX_PROCESS_NO_PIDFD defined) the task arms a backed-off timer
 * (1 ms .. 50 ms) and parks. waitpid() is only ever called for our own
 * pids, never waitpid(-1), so other children of the process are left
 * alone.
 *
 * Pipes: the parent ends are non-blocking and close-on-exec; would-block
 * arms one-shot readiness for the polled task (asx_io_wait). Writes to a
 * child's stdin block SIGPIPE on the calling thread and consume a SIGPIPE
 * they caused, so a closed pipe reports ASX_E_DISCONNECTED instead of
 * killing the process.
 *
 * Reaping: released children that are still running are killed
 * (kill_on_drop) or handed to a bounded detached-child list that is
 * reaped with WNOHANG on every spawn/wait/release/reset.
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef ASX_PROFILE_POSIX

#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "../../process/process_native.h"
#include <asx/runtime/io_driver.h>
#include <asx/runtime/runtime.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

extern char **environ;

#ifndef ASX_NATIVE_MAX_PROCESSES
#define ASX_NATIVE_MAX_PROCESSES 16u
#endif

#ifndef ASX_NATIVE_MAX_DETACHED
#define ASX_NATIVE_MAX_DETACHED 64u
#endif

#ifndef ASX_PROCESS_ENV_INHERIT_MAX
#define ASX_PROCESS_ENV_INHERIT_MAX 1024u
#endif

#ifndef ASX_PROCESS_RESOLVE_MAX
#define ASX_PROCESS_RESOLVE_MAX 1024u
#endif

#define PROC_BACKOFF_MIN_MS 1u
#define PROC_BACKOFF_MAX_MS 50u
#define PROC_IO_MAX 0x40000000u

#define PROC_STDIN 0
#define PROC_STDOUT 1
#define PROC_STDERR 2

typedef struct {
    int fd;
    int registered;
    asx_io_token token;
} proc_fd;

typedef struct {
    pid_t pid;
    uint32_t generation;
    int in_use;
    int reaped;
    asx_process_state state;
    int32_t code;
    int32_t term_signal;
    int kill_on_drop;
    asx_process_stdio modes[3];
    proc_fd io[3];  /* parent ends: stdin (write), stdout/stderr (read) */
    proc_fd exitfd; /* pidfd, or fd == -1 */
    uint32_t backoff_ms;
    char program[ASX_PROCESS_PROGRAM_MAX];
} native_proc;

/* Spawn plan: built in the parent, read by the child after fork. Static
 * because spawning is single-threaded (runtime contract) and the plan is
 * large. */
typedef struct {
    char path[ASX_PROCESS_RESOLVE_MAX];
    char args[ASX_PROCESS_MAX_ARGS + 1u][ASX_PROCESS_ARG_MAX];
    char *argv[ASX_PROCESS_MAX_ARGS + 2u];
    char env_kv[ASX_PROCESS_MAX_ENV][2u * ASX_PROCESS_ARG_MAX + 2u];
    char *envp[ASX_PROCESS_ENV_INHERIT_MAX + ASX_PROCESS_MAX_ENV + 1u];
    char cwd[ASX_PROCESS_ARG_MAX];
    int child_fd[3]; /* -1 = inherit */
} proc_plan;

static native_proc g_procs[ASX_NATIVE_MAX_PROCESSES];
static pid_t g_detached[ASX_NATIVE_MAX_DETACHED];
static uint32_t g_detached_count;
static proc_plan g_plan;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint32_t proc_next_gen(uint32_t g) {
    g++;
    return g == 0u ? 1u : g;
}

static asx_status proc_errno_status(int err) {
    switch (err) {
    case ENOENT:
    case ENOTDIR: return ASX_E_NOT_FOUND;
    case EACCES:
    case EPERM:
    case ENOEXEC: return ASX_E_PERMISSION_DENIED;
    case EAGAIN:
    case ENOMEM:
    case EMFILE:
    case ENFILE:
    case E2BIG: return ASX_E_RESOURCE_EXHAUSTED;
    case ENAMETOOLONG: return ASX_E_BUFFER_TOO_SMALL;
    case EPIPE:
    case ECONNRESET: return ASX_E_DISCONNECTED;
    case EINVAL:
    case EBADF: return ASX_E_INVALID_ARGUMENT;
    default: return ASX_E_INVALID_STATE;
    }
}

static native_proc *proc_lookup(asx_process_handle h) {
    uint32_t idx;
    if (!asx_process_slot_is_native(h.slot)) return NULL;
    idx = h.slot & ~ASX_PROCESS_NATIVE_SLOT_BIT;
    if (idx >= ASX_NATIVE_MAX_PROCESSES) return NULL;
    if (!g_procs[idx].in_use || g_procs[idx].generation != h.generation) return NULL;
    return &g_procs[idx];
}

static int proc_set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) ? -1 : 0;
}

#if !defined(__linux__)
static int proc_set_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFD, 0);
    return (fl < 0 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) < 0) ? -1 : 0;
}
#endif

static int proc_pipe(int fds[2]) {
#if defined(__linux__)
    return pipe2(fds, O_CLOEXEC);
#else
    if (pipe(fds) != 0) return -1;
    if (proc_set_cloexec(fds[0]) != 0 || proc_set_cloexec(fds[1]) != 0) {
        int err = errno;
        (void)close(fds[0]);
        (void)close(fds[1]);
        errno = err;
        return -1;
    }
    return 0;
#endif
}

static void proc_close_quiet(int *fd) {
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static void proc_fd_init(proc_fd *p) {
    p->fd = -1;
    p->registered = 0;
}

/* Deregister (a stale token after an IO-driver reset is a no-op) and close. */
static void proc_fd_close(proc_fd *p) {
    if (p->registered) asx_io_deregister(&p->token);
    p->registered = 0;
    proc_close_quiet(&p->fd);
}

/* Arm readiness for the current task and park it. Outside a task poll
 * nothing is armed. ASX_E_PENDING unless arming fails. */
static asx_status proc_fd_register(proc_fd *p) {
    asx_status st;
    if (!asx_io_driver_is_initialized()) {
        st = asx_io_driver_init();
        if (st != ASX_OK) return st;
    }
    st = asx_io_register_fd(p->fd, &p->token);
    if (st == ASX_OK) p->registered = 1;
    return st;
}

static asx_status proc_fd_wait(proc_fd *p, asx_io_interest interest) {
    asx_status st;
    if (asx_task_current() == ASX_INVALID_ID) return ASX_E_PENDING;
    if (!p->registered) {
        st = proc_fd_register(p);
        if (st != ASX_OK) return st;
    }
    st = asx_io_wait(&p->token, interest);
    if (st == ASX_E_NOT_FOUND) {
        /* The IO driver was re-initialized under us: register afresh. */
        p->registered = 0;
        st = proc_fd_register(p);
        if (st != ASX_OK) return st;
        st = asx_io_wait(&p->token, interest);
    }
    return st;
}

static void proc_record_status(native_proc *p, int status) {
    p->reaped = 1;
    if (WIFSIGNALED(status)) {
        p->state = ASX_PROCESS_TERMINATED;
        p->term_signal = (int32_t)WTERMSIG(status);
        p->code = 128 + p->term_signal;
    } else {
        p->state = ASX_PROCESS_EXITED;
        p->term_signal = 0;
        p->code = WIFEXITED(status) ? (int32_t)WEXITSTATUS(status) : -1;
    }
    proc_fd_close(&p->exitfd);
}

/* Non-blocking reap. Returns 1 once the child's status is known. */
static int proc_try_reap(native_proc *p) {
    int status = 0;
    pid_t r;
    if (p->reaped) return 1;
    do { r = waitpid(p->pid, &status, WNOHANG); } while (r < 0 && errno == EINTR);
    if (r == p->pid) {
        proc_record_status(p, status);
        return 1;
    }
    if (r < 0 && errno == ECHILD) {
        /* Reaped behind our back (e.g. SIGCHLD set to SIG_IGN): the exit
         * status is unknowable. */
        p->reaped = 1;
        p->state = ASX_PROCESS_EXITED;
        p->code = -1;
        p->term_signal = 0;
        proc_fd_close(&p->exitfd);
        return 1;
    }
    return 0;
}

static void proc_reap_blocking(native_proc *p) {
    int status = 0;
    pid_t r;
    if (p->reaped) return;
    do { r = waitpid(p->pid, &status, 0); } while (r < 0 && errno == EINTR);
    if (r == p->pid) {
        proc_record_status(p, status);
    } else {
        p->reaped = 1;
        proc_fd_close(&p->exitfd);
    }
}

static void proc_reap_detached(void) {
    uint32_t i = 0u;
    while (i < g_detached_count) {
        int status;
        pid_t r = waitpid(g_detached[i], &status, WNOHANG);
        if (r == g_detached[i] || (r < 0 && errno == ECHILD)) {
            g_detached[i] = g_detached[--g_detached_count];
            continue;
        }
        i++;
    }
}

static void proc_detach(pid_t pid) {
    proc_reap_detached();
    /* Beyond the bound the child is left for init to reap at our exit. */
    if (g_detached_count < ASX_NATIVE_MAX_DETACHED) g_detached[g_detached_count++] = pid;
}

/* Wait for exit: ASX_OK once reaped, else arm a wake source and park. */
static asx_status proc_wait_exit(native_proc *p, int park) {
    asx_task_id self;
    asx_time now = 0;
    asx_status st;

    if (proc_try_reap(p)) return ASX_OK;
    if (!park) return ASX_E_PENDING;
    self = asx_task_current();
    if (self == ASX_INVALID_ID) return ASX_E_PENDING;
    if (p->exitfd.fd >= 0) {
        st = proc_fd_wait(&p->exitfd, ASX_IO_READABLE);
        if (st == ASX_E_PENDING) return st;
        /* Registration failed (e.g. IO driver unavailable): use a timer. */
        proc_fd_close(&p->exitfd);
    }
    if (asx_runtime_now_ns(&now) == ASX_OK &&
        asx_task_arm_timer(self, now + (asx_time)p->backoff_ms * 1000000u) == ASX_OK) {
        st = asx_task_park(self);
        (void)st;
    }
    p->backoff_ms =
        p->backoff_ms * 2u > PROC_BACKOFF_MAX_MS ? PROC_BACKOFF_MAX_MS : p->backoff_ms * 2u;
    return ASX_E_PENDING;
}

/* Define ASX_PROCESS_NO_PIDFD to exercise the timer fallback on Linux. */
static int proc_open_pidfd(pid_t pid) {
#if defined(__linux__) && defined(SYS_pidfd_open) && !defined(ASX_PROCESS_NO_PIDFD)
    /* pidfd_open() always sets close-on-exec on the new descriptor. */
    long fd = syscall(SYS_pidfd_open, (long)pid, 0L);
    return fd < 0 ? -1 : (int)fd;
#else
    (void)pid;
    return -1;
#endif
}

/* ------------------------------------------------------------------ */
/* Spawn plan                                                          */
/* ------------------------------------------------------------------ */

/* strchr and memchr are called as (strchr)(...): with _GNU_SOURCE, glibc
 * 2.43 defines them as C23 _Generic macros, which clang rejects in C99. */
static size_t proc_key_len(const char *kv) {
    const char *eq = (strchr)(kv, '=');
    return eq != NULL ? (size_t)(eq - kv) : strlen(kv);
}

/* The command's override for `key`, or NULL. */
static const asx_process_env_pair *proc_env_override(const asx_process_command *cmd,
                                                     const char *key, size_t klen) {
    uint32_t i;
    for (i = 0; i < cmd->env_count; i++) {
        const asx_process_env_pair *e = &cmd->env_vars[i];
        if (strlen(e->key) == klen && memcmp(e->key, key, klen) == 0) return e;
    }
    return NULL;
}

static asx_status proc_build_env(const asx_process_command *cmd) {
    uint32_t n = 0u;
    uint32_t i;

    if (!cmd->env_clear && environ != NULL) {
        char **e;
        for (e = environ; *e != NULL; e++) {
            if (proc_env_override(cmd, *e, proc_key_len(*e)) != NULL) continue;
            if (n >= ASX_PROCESS_ENV_INHERIT_MAX) return ASX_E_RESOURCE_EXHAUSTED;
            g_plan.envp[n++] = *e;
        }
    }
    for (i = 0; i < cmd->env_count; i++) {
        const asx_process_env_pair *e = &cmd->env_vars[i];
        size_t klen = strlen(e->key);
        size_t vlen = strlen(e->value);
        if (e->remove) continue;
        memcpy(g_plan.env_kv[i], e->key, klen);
        g_plan.env_kv[i][klen] = '=';
        memcpy(&g_plan.env_kv[i][klen + 1u], e->value, vlen + 1u);
        g_plan.envp[n++] = g_plan.env_kv[i];
    }
    g_plan.envp[n] = NULL;
    return ASX_OK;
}

/* Resolve the executable like execvp, but in the parent. */
static asx_status proc_resolve(const asx_process_command *cmd) {
    const asx_process_env_pair *ov;
    const char *path_var;
    const char *p;
    size_t plen = strlen(cmd->program);
    int saw_eacces = 0;

    if ((strchr)(cmd->program, '/') != NULL) {
        if (plen >= sizeof(g_plan.path)) return ASX_E_BUFFER_TOO_SMALL;
        memcpy(g_plan.path, cmd->program, plen + 1u);
        return ASX_OK;
    }
    ov = proc_env_override(cmd, "PATH", 4u);
    if (ov != NULL && !ov->remove) {
        path_var = ov->value;
    } else {
        path_var = getenv("PATH");
        if (path_var == NULL) path_var = "/usr/local/bin:/usr/bin:/bin";
    }
    for (p = path_var;; p++) {
        const char *end = (strchr)(p, ':');
        size_t dlen = end != NULL ? (size_t)(end - p) : strlen(p);
        g_plan.path[0] = '\0';
        if (dlen == 0u) {
            /* Empty PATH element means the current directory. */
            memcpy(g_plan.path, cmd->program, plen + 1u);
        } else if (dlen + 1u + plen + 1u <= sizeof(g_plan.path)) {
            memcpy(g_plan.path, p, dlen);
            g_plan.path[dlen] = '/';
            memcpy(&g_plan.path[dlen + 1u], cmd->program, plen + 1u);
        }
        if (g_plan.path[0] != '\0') {
            if (access(g_plan.path, X_OK) == 0) return ASX_OK;
            if (errno == EACCES) saw_eacces = 1;
        }
        if (end == NULL) break;
        p = end;
    }
    return saw_eacces ? ASX_E_PERMISSION_DENIED : ASX_E_NOT_FOUND;
}

/* Builder strings must be NUL-terminated within their fixed buffers. */
static int proc_terminated(const char *s, size_t cap) { return (memchr)(s, '\0', cap) != NULL; }

static asx_status proc_build_plan(const asx_process_command *cmd) {
    uint32_t i;
    size_t len;
    asx_status st;

    for (i = 0; i < cmd->arg_count; i++) {
        if (!proc_terminated(cmd->args[i], ASX_PROCESS_ARG_MAX)) return ASX_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < cmd->env_count; i++) {
        if (!proc_terminated(cmd->env_vars[i].key, ASX_PROCESS_ARG_MAX) ||
            !proc_terminated(cmd->env_vars[i].value, ASX_PROCESS_ARG_MAX)) {
            return ASX_E_INVALID_ARGUMENT;
        }
    }
    if (!proc_terminated(cmd->current_dir, ASX_PROCESS_ARG_MAX)) return ASX_E_INVALID_ARGUMENT;

    st = proc_resolve(cmd);
    if (st != ASX_OK) return st;
    len = strlen(cmd->program);
    if (len >= ASX_PROCESS_ARG_MAX) return ASX_E_BUFFER_TOO_SMALL;
    memcpy(g_plan.args[0], cmd->program, len + 1u);
    g_plan.argv[0] = g_plan.args[0];
    for (i = 0; i < cmd->arg_count; i++) {
        len = strlen(cmd->args[i]);
        memcpy(g_plan.args[i + 1u], cmd->args[i], len + 1u);
        g_plan.argv[i + 1u] = g_plan.args[i + 1u];
    }
    g_plan.argv[cmd->arg_count + 1u] = NULL;
    len = strlen(cmd->current_dir);
    memcpy(g_plan.cwd, cmd->current_dir, len + 1u);
    return proc_build_env(cmd);
}

/* ------------------------------------------------------------------ */
/* Child side (async-signal-safe calls only)                           */
/* ------------------------------------------------------------------ */

static void proc_child_fail(int errfd, int err) {
    ssize_t w;
    do { w = write(errfd, &err, sizeof(err)); } while (w < 0 && errno == EINTR);
    _exit(127);
}

static void proc_child(int errfd) {
    static const int reset_signals[] = {SIGHUP,  SIGINT,  SIGQUIT, SIGTERM, SIGUSR1,
                                        SIGUSR2, SIGCHLD, SIGALRM, SIGPIPE, SIGWINCH};
    struct sigaction sa;
    sigset_t empty;
    size_t i;
    int fd;

    /* Caught handlers (e.g. the asx signal self-pipe) must not run in the
     * child; ignored dispositions are inherited as usual. */
    for (i = 0; i < sizeof(reset_signals) / sizeof(reset_signals[0]); i++) {
        if (sigaction(reset_signals[i], NULL, &sa) == 0 && sa.sa_handler != SIG_DFL &&
            sa.sa_handler != SIG_IGN) {
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = SIG_DFL;
            (void)sigemptyset(&sa.sa_mask);
            (void)sigaction(reset_signals[i], &sa, NULL);
        }
    }

    /* Lift child descriptors out of 0..2 first so dup2 cannot clobber one
     * that is still needed. */
    for (i = 0; i < 3u; i++) {
        fd = g_plan.child_fd[i];
        if (fd >= 0 && fd < 3) {
            fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
            if (fd < 0) proc_child_fail(errfd, errno);
            g_plan.child_fd[i] = fd;
        }
    }
    for (i = 0; i < 3u; i++) {
        int rv;
        fd = g_plan.child_fd[i];
        if (fd < 0) continue;
        do {
            rv = dup2(fd, (int)i); /* the duplicate is not close-on-exec */
        } while (rv < 0 && errno == EINTR);
        if (rv < 0) proc_child_fail(errfd, errno);
    }

    if (g_plan.cwd[0] != '\0' && chdir(g_plan.cwd) != 0) proc_child_fail(errfd, errno);

    (void)sigemptyset(&empty);
    (void)sigprocmask(SIG_SETMASK, &empty, NULL);
    (void)execve(g_plan.path, g_plan.argv, g_plan.envp);
    proc_child_fail(errfd, errno);
}

/* ------------------------------------------------------------------ */
/* Spawn                                                               */
/* ------------------------------------------------------------------ */

asx_status asx_native_process_spawn(const asx_process_command *cmd, asx_process_handle *out) {
    asx_process_stdio modes[3];
    int parent_fd[3] = {-1, -1, -1};
    int child_fd[3] = {-1, -1, -1};
    int null_fd = -1;
    int errpipe[2] = {-1, -1};
    sigset_t all;
    sigset_t old;
    native_proc *p;
    asx_status st = ASX_OK;
    uint32_t idx;
    int err = 0;
    ssize_t n;
    pid_t pid;
    int i;

    proc_reap_detached();
    for (idx = 0; idx < ASX_NATIVE_MAX_PROCESSES; idx++) {
        if (!g_procs[idx].in_use) break;
    }
    if (idx >= ASX_NATIVE_MAX_PROCESSES) return ASX_E_RESOURCE_EXHAUSTED;

    st = proc_build_plan(cmd);
    if (st != ASX_OK) return st;

    modes[0] = cmd->stdin_mode;
    modes[1] = cmd->stdout_mode;
    modes[2] = cmd->stderr_mode;
    for (i = 0; i < 3; i++) {
        if (modes[i] == ASX_PROCESS_STDIO_PIPED) {
            int fds[2];
            if (proc_pipe(fds) != 0) {
                st = proc_errno_status(errno);
                goto fail;
            }
            /* stdin: child reads fds[0]; stdout/stderr: child writes fds[1]. */
            child_fd[i] = i == 0 ? fds[0] : fds[1];
            parent_fd[i] = i == 0 ? fds[1] : fds[0];
            if (proc_set_nonblock(parent_fd[i]) != 0) {
                st = proc_errno_status(errno);
                goto fail;
            }
        } else if (modes[i] == ASX_PROCESS_STDIO_NULL) {
            if (null_fd < 0) {
                null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
                if (null_fd < 0) {
                    st = proc_errno_status(errno);
                    goto fail;
                }
            }
            child_fd[i] = null_fd;
        }
    }
    if (proc_pipe(errpipe) != 0) {
        st = proc_errno_status(errno);
        goto fail;
    }
    for (i = 0; i < 3; i++) g_plan.child_fd[i] = child_fd[i];

    (void)sigfillset(&all);
    (void)pthread_sigmask(SIG_SETMASK, &all, &old);
    pid = fork();
    if (pid == 0) {
        (void)close(errpipe[0]);
        proc_child(errpipe[1]); /* never returns */
    }
    err = errno;
    (void)pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (pid < 0) {
        st = proc_errno_status(err);
        goto fail;
    }

    /* Parent: drop the child's ends, then learn whether exec succeeded. */
    proc_close_quiet(&errpipe[1]);
    for (i = 0; i < 3; i++) {
        if (child_fd[i] >= 0 && child_fd[i] != null_fd) proc_close_quiet(&child_fd[i]);
        child_fd[i] = -1;
    }
    proc_close_quiet(&null_fd);
    do { n = read(errpipe[0], &err, sizeof(err)); } while (n < 0 && errno == EINTR);
    proc_close_quiet(&errpipe[0]);
    if (n == (ssize_t)sizeof(err)) {
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        st = proc_errno_status(err);
        goto fail;
    }

    p = &g_procs[idx];
    {
        uint32_t gen = proc_next_gen(p->generation);
        size_t len = strlen(cmd->program);
        memset(p, 0, sizeof(*p));
        p->generation = gen;
        memcpy(p->program, cmd->program, len + 1u);
    }
    p->in_use = 1;
    p->pid = pid;
    p->state = ASX_PROCESS_RUNNING;
    p->kill_on_drop = cmd->kill_on_drop != 0u;
    p->backoff_ms = PROC_BACKOFF_MIN_MS;
    for (i = 0; i < 3; i++) {
        p->modes[i] = modes[i];
        proc_fd_init(&p->io[i]);
        p->io[i].fd = parent_fd[i];
    }
    proc_fd_init(&p->exitfd);
    p->exitfd.fd = proc_open_pidfd(pid);
    out->slot = idx | ASX_PROCESS_NATIVE_SLOT_BIT;
    out->generation = p->generation;
    return ASX_OK;

fail:
    for (i = 0; i < 3; i++) {
        if (child_fd[i] != null_fd) proc_close_quiet(&child_fd[i]);
        proc_close_quiet(&parent_fd[i]);
    }
    proc_close_quiet(&null_fd);
    proc_close_quiet(&errpipe[0]);
    proc_close_quiet(&errpipe[1]);
    return st;
}

/* ------------------------------------------------------------------ */
/* Exit status and signals                                             */
/* ------------------------------------------------------------------ */

asx_status asx_native_process_poll_status(asx_process_handle h, asx_process_exit_status *out,
                                          int park) {
    native_proc *p = proc_lookup(h);
    asx_status st;
    if (p == NULL) return ASX_E_NOT_FOUND;
    proc_reap_detached();
    st = proc_wait_exit(p, park);
    if (st != ASX_OK) return st;
    out->state = p->state;
    out->code = p->code;
    out->signal = p->term_signal;
    return ASX_OK;
}

asx_status asx_native_process_state(asx_process_handle h, asx_process_state *out) {
    native_proc *p = proc_lookup(h);
    if (p == NULL) return ASX_E_NOT_FOUND;
    *out = proc_try_reap(p) ? p->state : ASX_PROCESS_RUNNING;
    return ASX_OK;
}

static asx_status proc_send(asx_process_handle h, int sig) {
    native_proc *p = proc_lookup(h);
    if (p == NULL) return ASX_E_NOT_FOUND;
    /* Until we reap it the pid cannot be recycled, so this never hits a
     * stranger; after reaping there is nothing left to signal. */
    if (p->reaped) return ASX_OK;
    if (kill(p->pid, sig) != 0 && errno != ESRCH) return proc_errno_status(errno);
    return ASX_OK;
}

asx_status asx_native_process_kill(asx_process_handle h) { return proc_send(h, SIGKILL); }

asx_status asx_native_process_terminate(asx_process_handle h) { return proc_send(h, SIGTERM); }

asx_status asx_native_process_program(asx_process_handle h, const char **out) {
    native_proc *p = proc_lookup(h);
    if (p == NULL) return ASX_E_NOT_FOUND;
    *out = p->program;
    return ASX_OK;
}

uint32_t asx_native_process_id(asx_process_handle h) {
    native_proc *p = proc_lookup(h);
    return p == NULL ? 0u : (uint32_t)p->pid;
}

int asx_native_process_is_alive(asx_process_handle h) { return proc_lookup(h) != NULL; }

/* ------------------------------------------------------------------ */
/* Piped stdio                                                         */
/* ------------------------------------------------------------------ */

asx_status asx_native_process_stdin_write(asx_process_handle h, const uint8_t *src, uint32_t len,
                                          uint32_t *out_written) {
    native_proc *p = proc_lookup(h);
    sigset_t pipe_set;
    sigset_t old;
    sigset_t pending;
    int was_pending;
    ssize_t n;
    int err;

    if (p == NULL) return ASX_E_NOT_FOUND;
    if (p->modes[PROC_STDIN] != ASX_PROCESS_STDIO_PIPED || p->io[PROC_STDIN].fd < 0) {
        return ASX_E_INVALID_STATE;
    }
    if (len == 0u) return ASX_OK;
    if (len > PROC_IO_MAX) len = PROC_IO_MAX;

    /* Keep a broken pipe from raising SIGPIPE: block it, and consume the
     * one this write generated (a SIGPIPE already pending is left alone). */
    (void)sigemptyset(&pipe_set);
    (void)sigaddset(&pipe_set, SIGPIPE);
    (void)pthread_sigmask(SIG_BLOCK, &pipe_set, &old);
    was_pending = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1;
    do { n = write(p->io[PROC_STDIN].fd, src, (size_t)len); } while (n < 0 && errno == EINTR);
    err = errno;
    if (n < 0 && err == EPIPE && !was_pending && sigpending(&pending) == 0 &&
        sigismember(&pending, SIGPIPE) == 1) {
        int sig;
        (void)sigwait(&pipe_set, &sig);
    }
    (void)pthread_sigmask(SIG_SETMASK, &old, NULL);

    if (n >= 0) {
        *out_written = (uint32_t)n;
        return ASX_OK;
    }
    if (err == EAGAIN || err == EWOULDBLOCK) {
        return proc_fd_wait(&p->io[PROC_STDIN], ASX_IO_WRITABLE);
    }
    return proc_errno_status(err);
}

asx_status asx_native_process_stdin_close(asx_process_handle h) {
    native_proc *p = proc_lookup(h);
    if (p == NULL) return ASX_E_NOT_FOUND;
    if (p->modes[PROC_STDIN] != ASX_PROCESS_STDIO_PIPED) return ASX_E_INVALID_STATE;
    proc_fd_close(&p->io[PROC_STDIN]);
    return ASX_OK;
}

static asx_status proc_read(native_proc *p, int which, uint8_t *dst, uint32_t cap,
                            uint32_t *out_read, int park) {
    proc_fd *f = &p->io[which];
    ssize_t n;

    *out_read = 0u;
    if (p->modes[which] != ASX_PROCESS_STDIO_PIPED) return ASX_E_INVALID_STATE;
    if (f->fd < 0) return ASX_OK; /* already at EOF */
    if (cap > PROC_IO_MAX) cap = PROC_IO_MAX;
    do { n = read(f->fd, dst, (size_t)cap); } while (n < 0 && errno == EINTR);
    if (n > 0) {
        *out_read = (uint32_t)n;
        return ASX_OK;
    }
    if (n == 0) {
        proc_fd_close(f); /* EOF is sticky */
        return ASX_OK;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return park ? proc_fd_wait(f, ASX_IO_READABLE) : ASX_E_PENDING;
    }
    return proc_errno_status(errno);
}

asx_status asx_native_process_read(asx_process_handle h, int which, uint8_t *dst, uint32_t cap,
                                   uint32_t *out_read) {
    native_proc *p = proc_lookup(h);
    if (p == NULL) return ASX_E_NOT_FOUND;
    return proc_read(p, which, dst, cap, out_read, 1);
}

/* ------------------------------------------------------------------ */
/* Output capture                                                      */
/* ------------------------------------------------------------------ */

/* Drain one output stream into its capture buffer (excess is discarded
 * and flagged). ASX_OK at EOF, ASX_E_PENDING when it would block. */
static asx_status proc_drain(native_proc *p, int which, uint8_t *buf, uint32_t *len,
                             uint8_t *truncated, int park) {
    uint8_t scratch[512];
    uint32_t n;
    asx_status st;

    while (p->io[which].fd >= 0) {
        uint32_t room = ASX_PROCESS_OUTPUT_MAX - *len;
        if (room > 0u) {
            st = proc_read(p, which, &buf[*len], room, &n, park);
        } else {
            st = proc_read(p, which, scratch, (uint32_t)sizeof(scratch), &n, park);
        }
        if (st != ASX_OK) return st;
        if (n == 0u) break;
        if (room > 0u) {
            *len += n;
        } else {
            *truncated = 1u;
        }
    }
    return ASX_OK;
}

asx_status asx_native_process_poll_output(asx_process_handle h, asx_process_output *out, int park) {
    native_proc *p = proc_lookup(h);
    asx_status st_out;
    asx_status st_err;
    asx_status st;

    if (p == NULL) return ASX_E_NOT_FOUND;
    proc_fd_close(&p->io[PROC_STDIN]); /* the child must see EOF on stdin */
    st_out = proc_drain(p, PROC_STDOUT, out->stdout_data, &out->stdout_len, &out->stdout_truncated,
                        park);
    if (st_out != ASX_OK && st_out != ASX_E_PENDING) return st_out;
    st_err = proc_drain(p, PROC_STDERR, out->stderr_data, &out->stderr_len, &out->stderr_truncated,
                        park);
    if (st_err != ASX_OK && st_err != ASX_E_PENDING) return st_err;
    if (st_out == ASX_E_PENDING || st_err == ASX_E_PENDING) return ASX_E_PENDING;

    st = proc_wait_exit(p, park);
    if (st != ASX_OK) return st;
    out->exit_code = p->code;
    out->term_signal = p->term_signal;
    out->success = (p->state == ASX_PROCESS_EXITED && p->code == 0) ? 1 : 0;
    return ASX_OK;
}

asx_status asx_native_process_wait_with_output(asx_process_handle h, asx_process_output *out) {
    for (;;) {
        native_proc *p;
        struct pollfd pfds[3];
        nfds_t n = 0;
        int timeout_ms = -1;
        int i;
        asx_status st = asx_native_process_poll_output(h, out, 0);
        if (st != ASX_E_PENDING) return st;

        p = proc_lookup(h);
        if (p == NULL) return ASX_E_NOT_FOUND;
        for (i = PROC_STDOUT; i <= PROC_STDERR; i++) {
            if (p->io[i].fd < 0) continue;
            pfds[n].fd = p->io[i].fd;
            pfds[n].events = POLLIN;
            pfds[n].revents = 0;
            n++;
        }
        if (n == 0u) {
            /* Pipes are done; only the exit is outstanding. */
            if (p->exitfd.fd >= 0) {
                pfds[n].fd = p->exitfd.fd;
                pfds[n].events = POLLIN;
                pfds[n].revents = 0;
                n++;
            } else {
                timeout_ms = (int)p->backoff_ms;
                p->backoff_ms = p->backoff_ms * 2u > PROC_BACKOFF_MAX_MS ? PROC_BACKOFF_MAX_MS
                                                                         : p->backoff_ms * 2u;
            }
        }
        if (poll(n > 0u ? pfds : NULL, n, timeout_ms) < 0 && errno != EINTR) {
            return proc_errno_status(errno);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Release and reset                                                   */
/* ------------------------------------------------------------------ */

static void proc_release(native_proc *p) {
    int i;
    for (i = 0; i < 3; i++) proc_fd_close(&p->io[i]);
    if (!proc_try_reap(p)) {
        if (p->kill_on_drop) {
            (void)kill(p->pid, SIGKILL);
            proc_reap_blocking(p);
        } else {
            proc_detach(p->pid);
        }
    }
    proc_fd_close(&p->exitfd);
    p->in_use = 0;
}

asx_status asx_native_process_release(asx_process_handle h) {
    native_proc *p = proc_lookup(h);
    if (p == NULL) return ASX_E_NOT_FOUND;
    proc_release(p);
    proc_reap_detached();
    return ASX_OK;
}

void asx_native_process_reset(void) {
    uint32_t i;
    for (i = 0; i < ASX_NATIVE_MAX_PROCESSES; i++) {
        if (g_procs[i].in_use) proc_release(&g_procs[i]);
        g_procs[i].generation = proc_next_gen(g_procs[i].generation);
    }
    proc_reap_detached();
}

#else
typedef int asx_process_posix_empty_translation_unit;
#endif /* ASX_PROFILE_POSIX */
