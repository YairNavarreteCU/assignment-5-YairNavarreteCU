#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h> 
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
 
#define PORT        "9000"
#define DATAFILE    "/var/tmp/aesdsocketdata"
#define BACKLOG     10
#define RECV_BUF    1024
 
static volatile sig_atomic_t g_shutdown = 0;
 
static void signal_handler(int signo)
{
    (void)signo;
    g_shutdown = 1;
}
 
static int setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
 
    if (sigaction(SIGINT,  &sa, NULL) == -1) return -1;
    if (sigaction(SIGTERM, &sa, NULL) == -1) return -1;
    return 0;
}
 
static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0)  return -1;  
    if (pid > 0)  exit(0);     
 
    if (setsid() == -1) return -1;
 
    int devnull = open("/dev/null", O_RDWR);
    if (devnull == -1) return -1;
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO) close(devnull);
 
    chdir("/");
    return 0;
}
 
static void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
        return &((struct sockaddr_in *)sa)->sin_addr;
    return &((struct sockaddr_in6 *)sa)->sin6_addr;
}
 
static void handle_client(int client_fd, const char *client_ip)
{
    syslog(LOG_INFO, "Accepted connection from %s", client_ip);
 
    /* ── Receive loop: accumulate data until newline ── */
    char   recv_buf[RECV_BUF];
    char  *packet   = NULL;
    size_t pkt_len  = 0;
    int    complete = 0;
 
    while (!complete && !g_shutdown) {
        ssize_t n = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
        if (n <= 0) break;  
 
        char *tmp = realloc(packet, pkt_len + (size_t)n + 1);
        if (!tmp) {
            syslog(LOG_ERR, "realloc failed: %m");
            free(packet);
            packet  = NULL;
            pkt_len = 0;
            break;
        }
        packet = tmp;
        memcpy(packet + pkt_len, recv_buf, (size_t)n);
        pkt_len += (size_t)n;
        packet[pkt_len] = '\0';
 
        if (memchr(packet, '\n', pkt_len) != NULL)
            complete = 1;
    }
 
    if (packet && pkt_len > 0) {
        int fd = open(DATAFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd == -1) {
            syslog(LOG_ERR, "open(%s) failed: %m", DATAFILE);
        } else {
            size_t written = 0;
            while (written < pkt_len) {
                ssize_t w = write(fd, packet + written, pkt_len - written);
                if (w == -1) { syslog(LOG_ERR, "write failed: %m"); break; }
                written += (size_t)w;
            }
            close(fd);
        }
    }
    free(packet);
 
    int fd = open(DATAFILE, O_RDONLY);
    if (fd == -1) {
        syslog(LOG_ERR, "open(%s) for read failed: %m", DATAFILE);
    } else {
        char send_buf[RECV_BUF];
        ssize_t r;
        while ((r = read(fd, send_buf, sizeof(send_buf))) > 0) {
            size_t sent = 0;
            while (sent < (size_t)r) {
                ssize_t s = send(client_fd, send_buf + sent, (size_t)r - sent, 0);
                if (s == -1) { syslog(LOG_ERR, "send failed: %m"); break; }
                sent += (size_t)s;
            }
        }
        close(fd);
    }
 
    syslog(LOG_INFO, "Closed connection from %s", client_ip);
    close(client_fd);
}
 

int main(int argc, char *argv[])
{
    int daemon_mode = 0;
 
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0)
            daemon_mode = 1;
    }
 
    openlog("aesdsocket", LOG_PID, LOG_USER);
 
    if (setup_signals() == -1) {
        syslog(LOG_ERR, "sigaction failed: %m");
        return -1;
    }
 
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;  
 
    int rc = getaddrinfo(NULL, PORT, &hints, &res);
    if (rc != 0) {
        syslog(LOG_ERR, "getaddrinfo: %s", gai_strerror(rc));
        return -1;
    }

    mkdir("/var/tmp", 0755);

    int server_fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        server_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (server_fd == -1) continue;
 
        int yes = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
 
        if (bind(server_fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
 
        close(server_fd);
        server_fd = -1;
    }
    freeaddrinfo(res);
 
    if (server_fd == -1) {
        syslog(LOG_ERR, "Failed to bind socket on port %s", PORT);
        return -1;
    }
 
    if (daemon_mode) {
        if (daemonize() == -1) {
            syslog(LOG_ERR, "daemonize failed: %m");
            close(server_fd);
            return -1;
        }
    }
 
    if (listen(server_fd, BACKLOG) == -1) {
        syslog(LOG_ERR, "listen failed: %m");
        close(server_fd);
        return -1;
    }


    while (!g_shutdown) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);
 
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd == -1) {
            if (errno == EINTR) break;  
            syslog(LOG_ERR, "accept failed: %m");
            continue;
        }
 
        char client_ip[INET6_ADDRSTRLEN];
        inet_ntop(client_addr.ss_family,
                  get_in_addr((struct sockaddr *)&client_addr),
                  client_ip, sizeof(client_ip));
 
        handle_client(client_fd, client_ip);
    }
 
    syslog(LOG_INFO, "Caught signal, exiting");
    close(server_fd);
    remove(DATAFILE);
    closelog();
    return 0;
}
 