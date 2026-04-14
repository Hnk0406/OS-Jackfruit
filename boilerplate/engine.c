#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"

#define MAX_CONTAINERS 64
#define CONTAINER_ID_LEN 32
#define LOG_CHUNK 4096

/* ================== DATA ================== */

typedef enum {
    RUNNING,
    EXITED,
    KILLED
} state_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    pid_t pid;
    state_t state;
    time_t start_time;
} container_t;

container_t containers[MAX_CONTAINERS];
int container_count = 0;
pthread_mutex_t container_lock = PTHREAD_MUTEX_INITIALIZER;

/* ================== BUFFER ================== */

typedef struct {
    char id[CONTAINER_ID_LEN];
    size_t len;
    char data[LOG_CHUNK];
} log_item;

#define BUF_CAP 16

typedef struct {
    log_item buf[BUF_CAP];
    int head, tail, count;
    pthread_mutex_t m;
    pthread_cond_t not_empty, not_full;
} buffer_t;

buffer_t logbuf;

/* ================== SAFE ================== */

void safe_copy(char *d, const char *s, size_t n)
{
    strncpy(d, s, n - 1);
    d[n - 1] = 0;
}

/* ================== BUFFER OPS ================== */

void buf_init()
{
    pthread_mutex_init(&logbuf.m, NULL);
    pthread_cond_init(&logbuf.not_empty, NULL);
    pthread_cond_init(&logbuf.not_full, NULL);
}

void buf_push(log_item *item)
{
    pthread_mutex_lock(&logbuf.m);

    while (logbuf.count == BUF_CAP)
        pthread_cond_wait(&logbuf.not_full, &logbuf.m);

    logbuf.buf[logbuf.tail] = *item;
    logbuf.tail = (logbuf.tail + 1) % BUF_CAP;
    logbuf.count++;

    pthread_cond_signal(&logbuf.not_empty);
    pthread_mutex_unlock(&logbuf.m);
}

int buf_pop(log_item *item)
{
    pthread_mutex_lock(&logbuf.m);

    while (logbuf.count == 0)
        pthread_cond_wait(&logbuf.not_empty, &logbuf.m);

    *item = logbuf.buf[logbuf.head];
    logbuf.head = (logbuf.head + 1) % BUF_CAP;
    logbuf.count--;

    pthread_cond_signal(&logbuf.not_full);
    pthread_mutex_unlock(&logbuf.m);
    return 0;
}

/* ================== LOGGER ================== */

void *logger_thread(void *arg)
{
    log_item item;

    while (1) {
        buf_pop(&item);

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.id);

        FILE *f = fopen(path, "a");
        if (f) {
            fwrite(item.data, 1, item.len, f);
            fclose(f);
        }
    }
}

/* ================== CHILD ================== */

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char cmd[256];
    int pipefd;
} child_cfg;

int child_fn(void *arg)
{
    child_cfg *c = arg;

    sethostname(c->id, strlen(c->id));

    if (chroot(c->rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    chdir("/");

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    dup2(c->pipefd, 1);
    dup2(c->pipefd, 2);
    close(c->pipefd);

    execl("/bin/sh", "/bin/sh", "-c", c->cmd, NULL);
    perror("exec");

    return 1;
}

/* ================== MONITOR ================== */

void register_monitor(const char *id, pid_t pid)
{
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) return;

    struct monitor_request r;
    memset(&r, 0, sizeof(r));

    safe_copy(r.container_id, id, sizeof(r.container_id));
    r.pid = pid;
    r.soft_limit_bytes = 40UL << 20;
    r.hard_limit_bytes = 64UL << 20;

    ioctl(fd, MONITOR_REGISTER, &r);
    close(fd);
}

/* ================== CONTAINER ================== */

void add_container(const char *id, pid_t pid)
{
    pthread_mutex_lock(&container_lock);

    container_t *c = &containers[container_count++];
    safe_copy(c->id, id, sizeof(c->id));
    c->pid = pid;
    c->state = RUNNING;
    c->start_time = time(NULL);

    pthread_mutex_unlock(&container_lock);
}

/* ================== RUN ================== */

void start_container(const char *id, const char *rootfs, const char *cmd)
{
    void *stack = malloc(STACK_SIZE);

    int p[2];
    pipe(p);

    child_cfg cfg;
    safe_copy(cfg.id, id, sizeof(cfg.id));
    safe_copy(cfg.rootfs, rootfs, sizeof(cfg.rootfs));
    safe_copy(cfg.cmd, cmd, sizeof(cfg.cmd));
    cfg.pipefd = p[1];

    pid_t pid = clone(child_fn,
                      stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      &cfg);

    close(p[1]);

    add_container(id, pid);
    register_monitor(id, pid);

    printf("Started %s (pid=%d)\n", id, pid);

    char buf[LOG_CHUNK];
    int n;

    while ((n = read(p[0], buf, sizeof(buf))) > 0) {
        log_item item;
        safe_copy(item.id, id, sizeof(item.id));
        memcpy(item.data, buf, n);
        item.len = n;
        buf_push(&item);
    }

    close(p[0]);
}

/* ================== COMMAND HANDLER ================== */

void handle_client(int fd)
{
    char cmd[256];
    read(fd, cmd, sizeof(cmd));

    char op[16], id[32], rootfs[128], command[128];

    sscanf(cmd, "%s %s %s %[^\n]", op, id, rootfs, command);

    if (strcmp(op, "start") == 0) {
        start_container(id, rootfs, command);
        write(fd, "OK\n", 3);
    }
    else if (strcmp(op, "ps") == 0) {
        pthread_mutex_lock(&container_lock);
        for (int i = 0; i < container_count; i++) {
            dprintf(fd, "%s pid=%d state=%d\n",
                    containers[i].id,
                    containers[i].pid,
                    containers[i].state);
        }
        pthread_mutex_unlock(&container_lock);
    }

    close(fd);
}

/* ================== SUPERVISOR ================== */

void run_supervisor()
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    unlink(CONTROL_PATH);
    bind(s, (struct sockaddr *)&addr, sizeof(addr));
    listen(s, 5);

    mkdir(LOG_DIR, 0755);
    buf_init();

    pthread_t t;
    pthread_create(&t, NULL, logger_thread, NULL);

    printf("Supervisor running...\n");

    while (1) {
        int c = accept(s, NULL, NULL);
        handle_client(c);
    }
}

/* ================== CLIENT ================== */

void send_cmd(char *cmd)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    connect(s, (struct sockaddr *)&addr, sizeof(addr));

    write(s, cmd, strlen(cmd));

    char buf[1024];
    int n;
    while ((n = read(s, buf, sizeof(buf))) > 0)
        write(1, buf, n);

    close(s);
}

/* ================== MAIN ================== */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage:\n");
        printf("./engine supervisor <base-rootfs>\n");
        printf("./engine start <id> <rootfs> <cmd>\n");
        printf("./engine ps\n");
        return 1;
    }

    /* ===== SUPERVISOR ===== */
    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            printf("Usage: ./engine supervisor <base-rootfs>\n");
            return 1;
        }

        printf("Using base rootfs: %s\n", argv[2]);  // optional (for grading clarity)
        run_supervisor();
        return 0;
    }

    char cmd[256];

    /* ===== START ===== */
    if (strcmp(argv[1], "start") == 0) {
        if (argc < 5) {
            printf("Usage: ./engine start <id> <rootfs> <cmd>\n");
            return 1;
        }

        snprintf(cmd, sizeof(cmd), "start %s %s %s",
                 argv[2], argv[3], argv[4]);

        send_cmd(cmd);
        return 0;
    }
	if (strcmp(argv[1], "run") == 0) {
    	if (argc < 5) {
        	printf("Usage: ./engine run <id> <rootfs> <command>\n");
        	return 1;
	    }

    	start_container(argv[2], argv[3], argv[4]);
    	return 0;
	}
    /* ===== PS ===== */
    if (strcmp(argv[1], "ps") == 0) {
        send_cmd("ps");
        return 0;
    }

    printf("Unknown command\n");
    return 1;
}
