/*
 * bcurl.c - small persistent HTTP/1.1 calculator client
 *
 * Build: cc -std=c11 -Wall -Wextra -O2 -o bcurl bcurl.c
 * Usage: ./bcurl [-v] host:port /add?a=2&b=3 [target ...]
 */

#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_HEADER_SIZE 8192
#define MAX_BODY_SIZE (1024 * 1024)

typedef struct {
    char *host;
    char *port;
} host_port_t;

static ssize_t write_full(int fd, const void *buf, size_t length) {
    const char *cursor = buf;
    size_t written = 0;
    while (written < length) {
        ssize_t result = write(fd, cursor + written, length - written);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (result == 0) return -1;
        written += (size_t)result;
    }
    return (ssize_t)written;
}

static int parse_host_port(const char *value, host_port_t *result) {
    const char *colon = strrchr(value, ':');
    if (!colon || colon == value || colon[1] == '\0') return -1;
    size_t host_length = (size_t)(colon - value);
    result->host = malloc(host_length + 1);
    result->port = strdup(colon + 1);
    if (!result->host || !result->port) {
        free(result->host);
        free(result->port);
        return -1;
    }
    memcpy(result->host, value, host_length);
    result->host[host_length] = '\0';
    return 0;
}

static int connect_to(const host_port_t *host_port) {
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    int result = getaddrinfo(host_port->host, host_port->port, &hints, &addresses);
    if (result != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(result));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd >= 0 && connect(fd, address->ai_addr, address->ai_addrlen) == 0) break;
        if (fd >= 0) close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

static int read_until_headers(int fd, char *buffer, size_t capacity, size_t *length) {
    *length = 0;
    while (*length < capacity - 1) {
        ssize_t result = read(fd, buffer + *length, 1);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (result == 0) return -1;
        (*length)++;
        buffer[*length] = '\0';
        if (*length >= 4 && strstr(buffer, "\r\n\r\n")) return 0;
    }
    return -1;
}

static int header_value(const char *headers, const char *wanted, long *value) {
    const char *line = strstr(headers, "\r\n");
    if (!line) return -1;
    line += 2;
    while (*line && !(line[0] == '\r' && line[1] == '\n')) {
        const char *end = strstr(line, "\r\n");
        if (!end) return -1;
        size_t name_length = strlen(wanted);
        if (strncasecmp(line, wanted, name_length) == 0 && line[name_length] == ':') {
            char *number_end;
            errno = 0;
            long parsed = strtol(line + name_length + 1, &number_end, 10);
            if (errno || number_end == line + name_length + 1 || parsed < 0) return -1;
            *value = parsed;
            return 0;
        }
        line = end + 2;
    }
    return -1;
}

static int read_full(int fd, char *buffer, size_t length) {
    size_t received = 0;
    while (received < length) {
        ssize_t result = read(fd, buffer + received, length - received);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (result == 0) return -1;
        received += (size_t)result;
    }
    return 0;
}

static int request(int fd, const char *host, const char *target, int verbose) {
    char request_text[8192];
    int request_length = snprintf(
        request_text, sizeof(request_text),
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n",
        target, host);
    if (request_length < 0 || (size_t)request_length >= sizeof(request_text)) return -1;
    if (write_full(fd, request_text, (size_t)request_length) < 0) return -1;

    char headers[MAX_HEADER_SIZE];
    size_t header_length;
    if (read_until_headers(fd, headers, sizeof(headers), &header_length) != 0) return -1;

    char status_line[128];
    if (sscanf(headers, "%127[^\r\n]", status_line) != 1) return -1;
    int status = 0;
    if (sscanf(status_line, "HTTP/1.1 %d", &status) != 1) return -1;

    long body_length;
    if (header_value(headers, "Content-Length", &body_length) != 0 ||
        body_length < 0 || body_length > MAX_BODY_SIZE) return -1;
    char *body = malloc((size_t)body_length + 1);
    if (!body) return -1;
    int result = read_full(fd, body, (size_t)body_length);
    body[body_length] = '\0';
    if (result == 0) {
        if (verbose) printf("Request: %s\n", target);
        printf("Status: %d\n%s", status, body);
        if (body_length == 0 || body[body_length - 1] != '\n') putchar('\n');
    }
    free(body);
    return result;
}

static void usage(const char *program) {
    fprintf(stderr, "usage: %s [-v] <host:port> <target> [target ...]\n", program);
}

int main(int argc, char **argv) {
    int verbose = 0;
    int first_argument = 1;
    if (first_argument < argc && strcmp(argv[first_argument], "-v") == 0) {
        verbose = 1;
        first_argument++;
    }
    if (argc - first_argument < 2) {
        usage(argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    host_port_t host_port = {0};
    if (parse_host_port(argv[first_argument], &host_port) != 0) {
        fprintf(stderr, "invalid host:port\n");
        return 1;
    }
    int fd = connect_to(&host_port);
    if (fd < 0) {
        fprintf(stderr, "failed to connect to %s:%s\n", host_port.host, host_port.port);
        free(host_port.host);
        free(host_port.port);
        return 1;
    }

    int exit_code = 0;
    for (int i = first_argument + 1; i < argc; i++) {
        if (request(fd, host_port.host, argv[i], verbose) != 0) {
            exit_code = 1;
            break;
        }
    }
    close(fd);
    free(host_port.host);
    free(host_port.port);
    return exit_code;
}
