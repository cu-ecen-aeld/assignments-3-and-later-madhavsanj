//  Author: Madhav Appanaboyina
//  file: server/aesdsocket.c - assignment 9
//  AI attribution: https://chatgpt.com/share/699bd5fa-fafc-8012-b7d0-7a66f53b2fc1

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/ioctl.h>

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

#include "../aesd-char-driver/aesd_ioctl.h"

#define AESD_PORT 9000

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#if USE_AESD_CHAR_DEVICE
#define DATAFILE_PATH "/dev/aesdchar"
#else
#define DATAFILE_PATH "/var/tmp/aesdsocketdata"
#endif

#define IOCTL_PREFIX "AESDCHAR_IOCSEEKTO:"

static volatile sig_atomic_t exit_requested = 0;
static int listen_fd_g = -1;

static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;

struct thread_node {
    pthread_t tid;
    int client_fd;
    bool complete;
    struct sockaddr_in client_addr;
    struct thread_node *next;
};

static void handle_signal(int sig)
{
    (void)sig;
    exit_requested = 1;

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

static int open_backing_fd_locked(void)
{
#if USE_AESD_CHAR_DEVICE
    return open(DATAFILE_PATH, O_RDWR);
#else
    return open(DATAFILE_PATH, O_RDWR | O_CREAT, 0644);
#endif
}

static int write_all_fd_locked(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }

    return 0;
}

static int read_remaining_fd_locked(int fd, char **out_buf, size_t *out_len)
{
    *out_buf = NULL;
    *out_len = 0;

    size_t capacity = 1024;
    size_t total = 0;
    char *buf = malloc(capacity);
    if (!buf) {
        return -1;
    }

    while (1) {
        if (total == capacity) {
            size_t new_capacity = capacity * 2;
            char *new_buf = realloc(buf, new_capacity);
            if (!new_buf) {
                free(buf);
                return -1;
            }
            buf = new_buf;
            capacity = new_capacity;
        }

        ssize_t r = read(fd, buf + total, capacity - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return -1;
        }
        if (r == 0) {
            break;
        }
        total += (size_t)r;
    }

    *out_buf = buf;
    *out_len = total;
    return 0;
}

static bool parse_ioctl_command(const char *packet, size_t pkt_size, struct aesd_seekto *seekto)
{
    char *tmp;
    char *comma;
    char *endptr;
    unsigned long x;
    unsigned long y;
    size_t prefix_len = strlen(IOCTL_PREFIX);

    if (!packet || !seekto || pkt_size == 0) {
        return false;
    }

    if (packet[pkt_size - 1] != '\n') {
        return false;
    }

    tmp = malloc(pkt_size);
    if (!tmp) {
        return false;
    }

    memcpy(tmp, packet, pkt_size - 1);
    tmp[pkt_size - 1] = '\0';

    if (strncmp(tmp, IOCTL_PREFIX, prefix_len) != 0) {
        free(tmp);
        return false;
    }

    comma = strchr(tmp + prefix_len, ',');
    if (!comma) {
        free(tmp);
        return false;
    }

    *comma = '\0';

    errno = 0;
    x = strtoul(tmp + prefix_len, &endptr, 10);
    if ((errno != 0) || (*endptr != '\0')) {
        free(tmp);
        return false;
    }

    errno = 0;
    y = strtoul(comma + 1, &endptr, 10);
    if ((errno != 0) || (*endptr != '\0')) {
        free(tmp);
        return false;
    }

    seekto->write_cmd = (uint32_t)x;
    seekto->write_cmd_offset = (uint32_t)y;

    free(tmp);
    return true;
}

static int process_packet_and_snapshot_locked(const char *packet,
                                              size_t pkt_size,
                                              char **snapshot,
                                              size_t *snapshot_len)
{
    struct aesd_seekto seekto;
    bool is_ioctl_cmd = false;

    *snapshot = NULL;
    *snapshot_len = 0;

    is_ioctl_cmd = parse_ioctl_command(packet, pkt_size, &seekto);

    if (is_ioctl_cmd) {
#if USE_AESD_CHAR_DEVICE
        int fd = open_backing_fd_locked();
        if (fd < 0) {
            return -1;
        }

        if (ioctl(fd, AESDCHAR_IOCSEEKTO, &seekto) != 0) {
            close(fd);
            return -1;
        }

        if (read_remaining_fd_locked(fd, snapshot, snapshot_len) != 0) {
            close(fd);
            return -1;
        }

        close(fd);
        return 0;
#else
        return -1;
#endif
    } else {
#if USE_AESD_CHAR_DEVICE
        int wfd = open(DATAFILE_PATH, O_WRONLY);
        int rfd;

        if (wfd < 0) {
            return -1;
        }

        if (write_all_fd_locked(wfd, packet, pkt_size) != 0) {
            close(wfd);
            return -1;
        }

        close(wfd);

        rfd = open(DATAFILE_PATH, O_RDONLY);
        if (rfd < 0) {
            return -1;
        }

        if (read_remaining_fd_locked(rfd, snapshot, snapshot_len) != 0) {
            close(rfd);
            return -1;
        }

        close(rfd);
        return 0;
#else
        int fd = open_backing_fd_locked();
        if (fd < 0) {
            return -1;
        }

        if (lseek(fd, 0, SEEK_END) < 0) {
            close(fd);
            return -1;
        }

        if (write_all_fd_locked(fd, packet, pkt_size) != 0) {
            close(fd);
            return -1;
        }

        if (lseek(fd, 0, SEEK_SET) < 0) {
            close(fd);
            return -1;
        }

        if (read_remaining_fd_locked(fd, snapshot, snapshot_len) != 0) {
            close(fd);
            return -1;
        }

        close(fd);
        return 0;
#endif
    }
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
            if ((errno == EINTR) && exit_requested) break;
            break;
        }
        if (n == 0) {
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

        while (1) {
            void *nl_ptr = memchr(packet, '\n', packet_len);
            if (!nl_ptr) break;

            size_t pkt_size = (size_t)((char *)nl_ptr - packet) + 1;
            char *snapshot = NULL;
            size_t snapshot_len = 0;

            pthread_mutex_lock(&file_mutex);

            if (process_packet_and_snapshot_locked(packet,
                                                   pkt_size,
                                                   &snapshot,
                                                   &snapshot_len) != 0) {
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
                if (shrink) {
                    packet = shrink;
                }
            }
        }
    }

done:
    free(packet);

    shutdown(client_fd, SHUT_RDWR);
    close(client_fd);

    pthread_mutex_lock(&thread_list_mutex);
    node->client_fd = -1;
    node->complete = true;
    pthread_mutex_unlock(&thread_list_mutex);

    syslog(LOG_INFO, "Closed connection from %s", ipstr);
    return NULL;
}

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
    } else if ((argc == 2) && (strcmp(argv[1], "-d") == 0)) {
        run_as_daemon = 1;
    } else {
        usage(argv[0]);
        return 1;
    }

    openlog("aesdsocket", 0, LOG_USER);

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
    addr.sin_family = AF_INET;
    addr.sin_port = htons(AESD_PORT);
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

            if (exit_requested) break;

            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            exit_requested = 1;
            break;
        }

        reap_completed_threads(&thread_head);

        if (exit_requested) {
            if (client_fd >= 0) {
                close(client_fd);
            }
            break;
        }

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

        node->next = thread_head;
        thread_head = node;
    }

    syslog(LOG_INFO, "Exiting: requesting client threads shutdown");

    if (listen_fd >= 0) {
        shutdown(listen_fd, SHUT_RDWR);
        close(listen_fd);
    }
    listen_fd_g = -1;

    request_client_threads_exit(thread_head);
    join_all_threads_and_free(&thread_head);

#if !USE_AESD_CHAR_DEVICE
    unlink(DATAFILE_PATH);
#endif

    closelog();
    return 0;
}