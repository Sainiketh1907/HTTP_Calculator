#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_REQUEST_SIZE 8192
#define MAX_TARGET_SIZE 4096

typedef struct {
    char method[16];
    char target[MAX_TARGET_SIZE];
    char version[16];
    int has_host;
    int connection_close;
} http_request_t;

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

static int find_header_end(const char *buffer, size_t length) {
    if (length < 4) return -1;
    for (size_t i = 0; i + 3 < length; i++) {
        if (memcmp(buffer + i, "\r\n\r\n", 4) == 0) return (int)i;
    }
    return -1;
}

static int read_request(int fd, http_request_t *request) {
    char buffer[MAX_REQUEST_SIZE + 1];
    size_t length = 0;
    int header_end = -1;

    while (length < MAX_REQUEST_SIZE) {
        ssize_t result = read(fd, buffer + length, 1);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (result == 0) return length == 0 ? 0 : -1;
        length++;
        header_end = find_header_end(buffer, length);
        if (header_end >= 0) break;
    }
    if (header_end < 0) return -1;
    buffer[header_end + 2] = '\0';

    char *line_end = strstr(buffer, "\r\n");
    if (!line_end) return -1;
    *line_end = '\0';

    char extra[2];
    if (sscanf(buffer, "%15s %4095s %15s %1s", request->method,
               request->target, request->version, extra) != 3) {
        return -1;
    }
    if (strcmp(request->version, "HTTP/1.1") != 0) return -1;

    request->has_host = 0;
    request->connection_close = 0;
    char *line = line_end + 2;
    while (line < buffer + header_end) {
        char *next = strstr(line, "\r\n");
        if (!next) return -1;
        *next = '\0';
        char *colon = strchr(line, ':');
        if (!colon) return -1;
        *colon = '\0';
        char *name = line;
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t') value++;
        if (*name == '\0' || *value == '\0') return -1;

        if (strcasecmp(name, "Host") == 0) {
            request->has_host = 1;
        } else if (strcasecmp(name, "Connection") == 0 &&
                   strcasecmp(value, "close") == 0) {
            request->connection_close = 1;
        }
        line = next + 2;
    }

    return request->has_host ? 1 : -1;
}

static const char *reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        default: return "Internal Server Error";
    }
}

static int send_response(int fd, int status, const char *body, int close_connection) {
    size_t body_length = strlen(body);
    char header[512];
    int header_length = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "%s\r\n",
        status, reason_phrase(status), body_length,
        close_connection ? "close" : "keep-alive",
        status == 405 ? "Allow: GET\r\n" : "");
    if (header_length < 0 || (size_t)header_length >= sizeof(header)) return -1;
    if (write_full(fd, header, (size_t)header_length) < 0) return -1;
    return write_full(fd, body, body_length) < 0 ? -1 : 0;
}

static int parse_number(const char *text, long long *value) {
    char *end = NULL;
    errno = 0;
    long long parsed = strtoll(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0') return -1;
    *value = parsed;
    return 0;
}

static int calculate(const char *target, char *body, size_t body_size, int *status) {
    char target_copy[MAX_TARGET_SIZE];
    char *query;
    char *separator;
    char *part;
    char *save_pointer = NULL;
    long long a = 0;
    long long b = 0;
    int found_a = 0;
    int found_b = 0;
    char operation;

    if (strlen(target) >= sizeof(target_copy)) {
        *status = 400;
        return -1;
    }
    strcpy(target_copy, target);
    query = strchr(target_copy, '?');
    if (!query) {
        *status = 400;
        return -1;
    }
    *query++ = '\0';

    if (strcmp(target_copy, "/add") == 0) operation = '+';
    else if (strcmp(target_copy, "/sub") == 0) operation = '-';
    else if (strcmp(target_copy, "/mul") == 0) operation = '*';
    else if (strcmp(target_copy, "/div") == 0) operation = '/';
    else {
        *status = 404;
        return -1;
    }

    for (part = strtok_r(query, "&", &save_pointer);
         part != NULL;
         part = strtok_r(NULL, "&", &save_pointer)) {
        separator = strchr(part, '=');
        if (!separator || separator == part || separator[1] == '\0') {
            *status = 400;
            return -1;
        }
        *separator = '\0';
        if (strcmp(part, "a") == 0) {
            if (found_a || parse_number(separator + 1, &a) != 0) {
                *status = 400;
                return -1;
            }
            found_a = 1;
        } else if (strcmp(part, "b") == 0) {
            if (found_b || parse_number(separator + 1, &b) != 0) {
                *status = 400;
                return -1;
            }
            found_b = 1;
        } else {
            *status = 400;
            return -1;
        }
    }
    if (!found_a || !found_b || (operation == '/' && b == 0)) {
        *status = 400;
        return -1;
    }

    __int128 result;
    if (operation == '+') result = (__int128)a + b;
    else if (operation == '-') result = (__int128)a - b;
    else if (operation == '*') result = (__int128)a * b;
    else result = a / b;
    if (result > LLONG_MAX || result < LLONG_MIN) {
        *status = 400;
        return -1;
    }
    snprintf(body, body_size, "%lld\n", (long long)result);
    *status = 200;
    return 0;
}

static void handle_connection(int fd) {
    for (;;) {
        http_request_t request;
        int read_status = read_request(fd, &request);
        if (read_status == 0) return;

        int close_connection = read_status < 0;
        int status = 400;
        char body[128];
        strcpy(body, "Bad Request\n");

        if (read_status > 0) {
            if (strcmp(request.method, "GET") != 0) {
                status = 405;
                strcpy(body, "Method Not Allowed\n");
            } else if (calculate(request.target, body, sizeof(body), &status) != 0) {
                snprintf(body, sizeof(body), "%s\n", reason_phrase(status));
            }
            close_connection = request.connection_close;
        }

        if (send_response(fd, status, body, close_connection) != 0 || close_connection) return;
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <document-root> <port>\n", argv[0]);
        return 1;
    }

    char *end = NULL;
    long port = strtol(argv[2], &end, 10);
    if (argv[2][0] == '\0' || *end != '\0' || port <= 0 || port > 65535) {
        fprintf(stderr, "invalid port\n");
        return 1;
    }
    (void)argv[1];

    signal(SIGPIPE, SIG_IGN);
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listen_fd, 16) != 0) {
        perror("bind/listen");
        close(listen_fd);
        return 1;
    }

    printf("bserve listening on port %ld\n", port);
    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        handle_connection(client_fd);
        close(client_fd);
    }
}
