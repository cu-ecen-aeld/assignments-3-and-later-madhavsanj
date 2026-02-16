// server/aesdsocket.c

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>

#define AESD_PORT 9000
#define DATAFILE_PATH "/var/tmp/aesdsocketdata"

static volatile sig_atomic_t exit_requested = 0;

static void handle_signal(int sig)
{
    (void)sig;
    exit_requested = 1; // FIX 1: handler only sets flag
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-d]\n", prog);
}

static int append_packet_to_file(const char *buf, size_t len)
{
    int fd = open(DATAFILE_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return -1;

    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0) {
            close(fd);
            return -1;
        }
        off += (size_t)w;
    }

    close(fd);
    return 0;
}

static int send_file_to_client(int client_fd)
{
    int fd = open(DATAFILE_PATH, O_RDONLY);
    if (fd < 0) return -1;

    char filebuf[4096];
    for (;;) {
        ssize_t r = read(fd, filebuf, sizeof(filebuf));
        if (r < 0) {
            close(fd);
            return -1;
        }
        if (r == 0) break;

        size_t off = 0;
        while (off < (size_t)r) {
            ssize_t s = send(client_fd, filebuf + off, (size_t)r - off, 0);
            if (s < 0) {
                close(fd);
                return -1;
            }
            off += (size_t)s;
        }
    }

    close(fd);
    return 0;
}

static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        // Parent exits
        exit(0);
    }

    if (setsid() < 0) {
        return -1;
    }

    if (chdir("/") < 0) {
        return -1;
    }

    // Redirect stdin/out/err to /dev/null
    int devnull = open("/dev/null", O_RDWR);
    if (devnull < 0) {
        return -1;
    }
    (void)dup2(devnull, STDIN_FILENO);
    (void)dup2(devnull, STDOUT_FILENO);
    (void)dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO) {
        close(devnull);
    }

    return 0;
}

int main(int argc, char *argv[])
{
    // FIX 3: strict argument parsing
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

    // FIX 4: SO_REUSEADDR
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(listen_fd);
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
        closelog();
        return -1;
    }

    if (listen(listen_fd, 10) < 0) {
        close(listen_fd);
        closelog();
        return -1;
    }

    // Requirement: fork after ensuring it can bind to port 9000
    if (run_as_daemon) {
        if (daemonize() != 0) {
            close(listen_fd);
            closelog();
            return -1;
        }
    }

    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd;

        // FIX 2: accept() EINTR handling
        for (;;) {
            client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd >= 0) break;

            if (errno == EINTR) {
                if (exit_requested) {
                    break; // exit requested -> break out to cleanup
                }
                continue; // interrupted but no exit requested -> retry
            }

            // other accept error -> log and exit loop to cleanup
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            exit_requested = 1;
            break;
        }

        if (exit_requested) {
            if (client_fd >= 0) close(client_fd);
            break;
        }

        char ipstr[INET_ADDRSTRLEN];
        ipstr[0] = '\0';
        inet_ntop(AF_INET, &client_addr.sin_addr, ipstr, sizeof(ipstr));
        syslog(LOG_INFO, "Accepted connection from %s", ipstr);

        char recvbuf[1024];
        char *packet = NULL;
        size_t packet_len = 0;

        while (!exit_requested) {
            ssize_t n = recv(client_fd, recvbuf, sizeof(recvbuf), 0);
            if (n < 0) {
                if (errno == EINTR && exit_requested) {
                    break;
                }
                break;
            }

            if (n == 0) {
                // FIX 5: drop incomplete packet on EOF; do NOT flush partial data
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

                if (append_packet_to_file(packet, pkt_size) != 0) {
                    free(packet);
                    packet = NULL;
                    packet_len = 0;
                    goto done_with_client;
                }

                if (send_file_to_client(client_fd) != 0) {
                    free(packet);
                    packet = NULL;
                    packet_len = 0;
                    goto done_with_client;
                }

                size_t remain = packet_len - pkt_size;
                if (remain > 0) memmove(packet, packet + pkt_size, remain);
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

done_with_client:
        free(packet);
        close(client_fd);
        syslog(LOG_INFO, "Closed connection from %s", ipstr);
    }

    syslog(LOG_INFO, "Caught signal, exiting");

    close(listen_fd);
    unlink(DATAFILE_PATH);
    closelog();
    return 0;
}
