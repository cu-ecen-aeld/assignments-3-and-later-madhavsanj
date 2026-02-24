//  Author: Madhav Appanaboyina
//  file: server/aesdsocket.c - revised
//  AI attribution: https://chatgpt.com/share/699bd5fa-fafc-8012-b7d0-7a66f53b2fc1

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#define AESD_PORT 9000
#define DATAFILE_PATH "/var/tmp/aesdsocketdata"

/*
 * Global shutdown flag
 */
static volatile sig_atomic_t exit_requested = 0;

/*
 * Global listen socket so signal handler can unblock accept()
 */
static int listen_fd_g = -1;

/*
 * Mutex protecting ALL interactions with /var/tmp/aesdsocketdata
 * - ensures appends are atomic across client threads
 * - ensures timestamp writes are atomic w.r.t. client writes
 * - ensures snapshot reads are coherent
 */
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Thread list mutex:
 * Protects shared fields in thread nodes (client_fd, complete),
 * and protects list traversal during shutdown requests.
 */
static pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Singly linked list node to track each client handler thread.
 * Memory allocation and deallocation is done ONLY in main thread.
 */
struct thread_node {
    pthread_t tid;
    int client_fd;                 // active fd, or -1 once thread is done/closed
    bool complete;                 // set true by worker thread on exit
    struct sockaddr_in client_addr;
    struct thread_node *next;
};

static void handle_signal(int sig)
{
    (void)sig;
    exit_requested = 1;

    /* Unblock accept() in main thread */
    if (listen_fd_g >= 0) {
        shutdown(listen_fd_g, SHUT_RDWR);
    }
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-d]\n", prog);
}

static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) exit(0);

    if (setsid() < 0) return -1;
    if (chdir("/") < 0) return -1;

    int devnull = open("/dev/null", O_RDWR);
    if (devnull < 0) return -1;

    (void)dup2(devnull, STDIN_FILENO);
    (void)dup2(devnull, STDOUT_FILENO);
    (void)dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO) close(devnull);

    return 0;
}

/*
 * File helpers (caller must hold file_mutex)
 */
static int append_bytes_to_file_locked(const char *buf, size_t len)
{
    int fd = open(DATAFILE_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return -1;

    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        off += (size_t)w;
    }

    close(fd);
    return 0;
}

/*
 * Read entire file into a snapshot buffer (caller must hold file_mutex).
 * Returns malloc'd buffer in *out_buf and length in *out_len.
 */
static int read_file_snapshot_locked(char **out_buf, size_t *out_len)
{
    *out_buf = NULL;
    *out_len = 0;

    int fd = open(DATAFILE_PATH, O_RDONLY);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }

    size_t len = (size_t)st.st_size;
    char *buf = malloc(len ? len : 1);
    if (!buf) {
        close(fd);
        return -1;
    }

    size_t off = 0;
    while (off < len) {
        ssize_t r = read(fd, buf + off, len - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf);
            close(fd);
            return -1;
        }
        if (r == 0) break;
        off += (size_t)r;
    }

    close(fd);
    *out_buf = buf;
    *out_len = off;
    return 0;
}

static int send_all(int client_fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t s = send(client_fd, buf + off, len - off, 0);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)s;
    }
    return 0;
}

/*
 * Sleep in 1-second chunks so shutdown is responsive
 */
static void sleep_interruptible_seconds(int total_seconds)
{
    for (int i = 0; i < total_seconds && !exit_requested; i++) {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
    }
}

/*
 * Timestamp thread: every 10 seconds append RFC 2822 timestamp line
 * Format: "timestamp:time\n"
 */
static void *timestamp_thread_fn(void *arg)
{
    (void)arg;

    while (!exit_requested) {
        sleep_interruptible_seconds(10);
        if (exit_requested) break;

        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        /* RFC 2822 compliant format */
        char timebuf[128];
        if (strftime(timebuf, sizeof(timebuf), "%a, %d %b %Y %H:%M:%S %z", &tm_now) == 0) {
            continue;
        }

        char line[256];
        int n = snprintf(line, sizeof(line), "timestamp:%s\n", timebuf);
        if (n < 0) continue;

        pthread_mutex_lock(&file_mutex);
        (void)append_bytes_to_file_locked(line, (size_t)n);
        pthread_mutex_unlock(&file_mutex);
    }

    return NULL;
}

/*
 * Worker thread: handles one client connection
 * - receives data
 * - when newline-terminated packet complete: append packet + send whole file back
 * - exits on client close or error
 * - sets node->complete = true before returning
 */
static void *client_thread_fn(void *arg)
{
    struct thread_node *node = (struct thread_node *)arg;
    int client_fd = node->client_fd;

    char ipstr[INET_ADDRSTRLEN];
    ipstr[0] = '\0';
    inet_ntop(AF_INET, &node->client_addr.sin_addr, ipstr, sizeof(ipstr));
    syslog(LOG_INFO, "Accepted connection from %s", ipstr);

    char recvbuf[1024];
    char *packet = NULL;
    size_t packet_len = 0;

    while (!exit_requested) {
        ssize_t n = recv(client_fd, recvbuf, sizeof(recvbuf), 0);
        if (n < 0) {
            if (errno == EINTR && exit_requested) break;
            break;
        }
        if (n == 0) {
            /* client closed */
            break;
        }

        char *newbuf = realloc(packet, packet_len + (size_t)n);
        if (!newbuf) {
            free(packet);
            packet = NULL;
            packet_len = 0;
            break;
        }
        packet = newbuf;
        memcpy(packet + packet_len, recvbuf, (size_t)n);
        packet_len += (size_t)n;

        /* process all complete packets (newline-terminated) */
        while (1) {
            void *nl_ptr = memchr(packet, '\n', packet_len);
            if (!nl_ptr) break;

            size_t pkt_size = (size_t)((char *)nl_ptr - packet) + 1;

            /* Snapshot file while holding the file mutex, but SEND after unlocking. */
            char *snapshot = NULL;
            size_t snapshot_len = 0;

            pthread_mutex_lock(&file_mutex);

            if (append_bytes_to_file_locked(packet, pkt_size) != 0) {
                pthread_mutex_unlock(&file_mutex);
                free(packet);
                packet = NULL;
                packet_len = 0;
                goto done;
            }

            if (read_file_snapshot_locked(&snapshot, &snapshot_len) != 0) {
                pthread_mutex_unlock(&file_mutex);
                free(packet);
                packet = NULL;
                packet_len = 0;
                goto done;
            }

            pthread_mutex_unlock(&file_mutex);

            if (send_all(client_fd, snapshot, snapshot_len) != 0) {
                free(snapshot);
                free(packet);
                packet = NULL;
                packet_len = 0;
                goto done;
            }
            free(snapshot);

            /* remove processed packet from buffer */
            size_t remain = packet_len - pkt_size;
            if (remain > 0) {
                memmove(packet, packet + pkt_size, remain);
            }
            packet_len = remain;

            if (packet_len == 0) {
                free(packet);
                packet = NULL;
            } else {
                char *shrink = realloc(packet, packet_len);
                if (shrink) packet = shrink;
            }
        }
    }

done:
    free(packet);

    shutdown(client_fd, SHUT_RDWR);
    close(client_fd);

    /* Mark thread completion. Main thread will join + free node. */
    pthread_mutex_lock(&thread_list_mutex);
    node->client_fd = -1;
    node->complete = true;
    pthread_mutex_unlock(&thread_list_mutex);

    syslog(LOG_INFO, "Closed connection from %s", ipstr);
    return NULL;
}

/*
 * Join and remove completed threads from singly linked list.
 * Frees nodes ONLY here (one place).
 */
static void reap_completed_threads(struct thread_node **head)
{
    struct thread_node *prev = NULL;
    struct thread_node *cur = *head;

    while (cur) {
        bool done = false;

        pthread_mutex_lock(&thread_list_mutex);
        done = cur->complete;
        pthread_mutex_unlock(&thread_list_mutex);

        if (done) {
            pthread_join(cur->tid, NULL);

            struct thread_node *to_free = cur;
            if (prev) {
                prev->next = cur->next;
            } else {
                *head = cur->next;
            }
            cur = cur->next;

            free(to_free);
            continue;
        }

        prev = cur;
        cur = cur->next;
    }
}

/*
 * On shutdown, request all client threads exit by shutting down their sockets.
 */
static void request_client_threads_exit(struct thread_node *head)
{
    pthread_mutex_lock(&thread_list_mutex);
    for (struct thread_node *cur = head; cur; cur = cur->next) {
        if (cur->client_fd >= 0) {
            shutdown(cur->client_fd, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&thread_list_mutex);
}

/*
 * Join all remaining threads and free all nodes (one place).
 */
static void join_all_threads_and_free(struct thread_node **head)
{
    struct thread_node *cur = *head;
    while (cur) {
        pthread_join(cur->tid, NULL);
        struct thread_node *next = cur->next;
        free(cur);
        cur = next;
    }
    *head = NULL;
}

int main(int argc, char *argv[])
{
    int run_as_daemon = 0;
    if (argc == 1) {
        run_as_daemon = 0;
    } else if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        run_as_daemon = 1;
    } else {
        usage(argv[0]);
        return 1;
    }

    openlog("aesdsocket", 0, LOG_USER);

    /* Prevent SIGPIPE from killing process on send() to closed socket */
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        closelog();
        return -1;
    }
    listen_fd_g = listen_fd;

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(listen_fd);
        listen_fd_g = -1;
        closelog();
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(AESD_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(listen_fd);
        listen_fd_g = -1;
        closelog();
        return -1;
    }

    if (listen(listen_fd, 10) < 0) {
        close(listen_fd);
        listen_fd_g = -1;
        closelog();
        return -1;
    }

    if (run_as_daemon) {
        if (daemonize() != 0) {
            close(listen_fd);
            listen_fd_g = -1;
            closelog();
            return -1;
        }
    }

    /* Start timestamp thread */
    pthread_t ts_tid;
    if (pthread_create(&ts_tid, NULL, timestamp_thread_fn, NULL) != 0) {
        syslog(LOG_ERR, "Failed to create timestamp thread");
        close(listen_fd);
        listen_fd_g = -1;
        closelog();
        return -1;
    }

    /* Thread list head */
    struct thread_node *thread_head = NULL;

    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = -1;

        for (;;) {
            client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd >= 0) break;

            if (errno == EINTR) {
                if (exit_requested) break;
                continue;
            }

            /* accept may fail when listen socket is shut down during exit */
            if (exit_requested) break;

            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            exit_requested = 1;
            break;
        }

        /* Reap finished threads regularly (prevents leaks) */
        reap_completed_threads(&thread_head);

        if (exit_requested) {
            if (client_fd >= 0) close(client_fd);
            break;
        }

        /* Create node for new thread (allocated in main, freed in main) */
        struct thread_node *node = calloc(1, sizeof(*node));
        if (!node) {
            close(client_fd);
            exit_requested = 1;
            break;
        }

        node->client_fd = client_fd;
        node->complete = false;
        node->client_addr = client_addr;

        if (pthread_create(&node->tid, NULL, client_thread_fn, node) != 0) {
            syslog(LOG_ERR, "pthread_create failed");
            close(client_fd);
            free(node);
            exit_requested = 1;
            break;
        }

        /* push node onto singly linked list head */
        node->next = thread_head;
        thread_head = node;
    }

    syslog(LOG_INFO, "Exiting: requesting client threads shutdown");

    /* stop accepting */
    if (listen_fd >= 0) {
        shutdown(listen_fd, SHUT_RDWR);
        close(listen_fd);
    }
    listen_fd_g = -1;

    /* request client threads exit and join/free all */
    request_client_threads_exit(thread_head);
    join_all_threads_and_free(&thread_head);

    /* stop timestamp thread */
    pthread_join(ts_tid, NULL);

    syslog(LOG_INFO, "Cleanup and exit");

    /* Cleanup required */
    unlink(DATAFILE_PATH);

    closelog();
    return 0;
}