/* The code is subject to Purdue University copyright policies.
 * DO NOT SHARE, DISTRIBUTE, OR POST ONLINE
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Assumption: any http request/response header will not exceed 4096 bytes.
#define MAX_BUFFER 4096

#define HTTP_RESPONSE_OK "200 OK\r\n"
#define CONTENT_LENGTH_FIELD "Content-Length:"
#define HEAD_CONTENT_SPLIT "\r\n\r\n"
#define LINE_END "\r\n"

/**
 * fill the buf like the following format:
 *      GET /path/file.html HTTP/1.0\r\n
 *      Host: <the_host_name_you_are_connecting_to>:<port_number>\r\n
 *      \r\n
 * return the header byte length
 */
int fill_get_header(char buf[MAX_BUFFER], const char *filepath, const char *host_name, short port);
/**
 * parse file name from file path
 *
 * if file_path is ' /software/make/manual/make.html', the name should be 'make.html'
 */
void parse_file_name(const char *file_path, char file_name[MAX_BUFFER]);

/**
 * read from the socket fd.
 * if first_line is enabled and '\r\n' can be found in buf, return directly.
 * if read_content_length is enabled and HEAD_CONTENT_SPLIT or CONTENT_LENGTH_FIELD can be found in buf, return
 * directly. if read_split is enabled and HEAD_CONTENT_SPLIT can be found in buf, return directly. offset is an
 * input/output parameter, the result offset will be stored in offset. read_length: new-offset - old-offset. return:
 *  false: there is no more data.
 *  true: there are possible data.
 * if there is any error during reading, exit directly.
 */
bool read_from_socket(int fd, char buf[MAX_BUFFER], int max_bytes, bool first_line, bool read_content_length,
                      bool read_split, int *offset, int *read_length);

/**
 * if there is any error during writing, exit directly.
 */
void send_to_socket(int fd, const char *buf, int buf_len);

/**
 * write to a file
 * if there is any error during writing, exit directly.
 */
void write_to_file(int fd, const char *buf, int buf_len);

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "usage: ./http_client [host] [port number] [filepath]\n");
        exit(1);
    }

    const char *file_path = argv[3];
    char file_name[MAX_BUFFER] = {0};
    parse_file_name(file_path, file_name);

    int sockfd;
    struct sockaddr_in their_addr; /* client's address information */
    struct hostent *he;

    /* get server's IP by invoking the DNS */
    if ((he = gethostbyname(argv[1])) == NULL) {
        herror("gethostbyname");
        exit(1);
    }

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }

    // parse port from argv
    short serve_port = (short)atoi(argv[2]);

    their_addr.sin_family = AF_INET;
    their_addr.sin_port = htons(serve_port);
    their_addr.sin_addr = *((struct in_addr *)he->h_addr_list[0]);
    bzero(&(their_addr.sin_zero), 8);

    if (connect(sockfd, (struct sockaddr *)&their_addr, sizeof(struct sockaddr)) < 0) {
        perror("connect");
        exit(1);
    }

    // send the request.
    char buf[MAX_BUFFER] = {0};
    int buf_len = fill_get_header(buf, file_path, argv[1], serve_port);
    send_to_socket(sockfd, buf, buf_len);

    int offset = 0;
    int length = 0;
    // reset buf
    memset(buf, 0, sizeof(buf));
    // read the first line
    bool should_continue = read_from_socket(sockfd, buf, MAX_BUFFER, true, false, false, &offset, &length);
    char *first_line_end = 0;
    if (!(first_line_end = strstr(buf, LINE_END))) {
        fprintf(stderr, "can not read the first line.\n");
        exit(1);
    } else {
        if (!strstr(buf, HTTP_RESPONSE_OK)) {
            // state is wrong
            *first_line_end = 0;
            printf("%s\n", buf);
            exit(1);

        } else if (should_continue) {
            // read the content length
            should_continue = read_from_socket(sockfd, buf, MAX_BUFFER, false, true, false, &offset, &length);
        }
    }

    char *content_length_str = strstr(buf, CONTENT_LENGTH_FIELD);
    if (!content_length_str) {
        printf("%s\n", "Error: could not download the requested file (file length unknown)");
        exit(1);
    }
    int content_length = atoi(content_length_str + strlen(CONTENT_LENGTH_FIELD));

    if (should_continue) {
        // read the split
        should_continue = read_from_socket(sockfd, buf, MAX_BUFFER, false, false, true, &offset, &length);
    }

    FILE *output = fopen(file_name, "w");
    if (!output) {
        fprintf(stderr, "can not write to %s\n", file_name);
        exit(1);
    }

    char *content_begin = strstr(buf, HEAD_CONTENT_SPLIT) + strlen(HEAD_CONTENT_SPLIT);
    int output_fd = fileno(output);
    int already_length = offset - (content_begin - (char *)buf);
    if (already_length > content_length) {
        already_length = content_length;
    }
    write_to_file(output_fd, content_begin, already_length);
    content_length -= already_length;

    while (should_continue && content_length > 0) {
        offset = 0;
        should_continue = read_from_socket(sockfd, buf, (content_length < MAX_BUFFER ? content_length : MAX_BUFFER),
                                           false, false, false, &offset, &length);
        if (offset > 0) {
            write_to_file(output_fd, buf, offset);
            content_length -= offset;
        } else {
            // possible error.
            break;
        }
    }

    fclose(output);
    close(sockfd);

    return 0;
}

void write_to_file(int fd, const char *buf, int buf_len) {
    int offset = 0;
    while (buf_len > 0) {
        // multiple writes may be needed.
        ssize_t tmp = write(fd, buf + offset, buf_len);
        if (tmp == -1) {
            // write error
            perror("write");
            exit(1);
        }

        offset += (int)tmp;
        buf_len -= (int)tmp;
    }
}

void send_to_socket(int fd, const char *buf, int buf_len) {
    int offset = 0;
    while (buf_len > 0) {
        // multiple writes may be needed.
        ssize_t tmp = send(fd, buf + offset, buf_len, 0);
        if (tmp == -1) {
            // send error
            perror("send");
            exit(1);
        }

        offset += (int)tmp;
        buf_len -= (int)tmp;
    }
}

bool read_from_socket(int fd, char buf[MAX_BUFFER], int max_bytes, bool first_line, bool read_content_length,
                      bool read_split, int *poffset, int *pread_length) {
    int offset = *poffset;
    int read_length = 0;
    // do not overflow the buffer
    if (offset >= max_bytes) {
        return true;
    }
    bool should_continue = true;
    // read at most left_bytes bytes.
    int left_bytes = max_bytes - offset;
    do {
        //check some flags
        if (first_line && strstr(buf, LINE_END)) {
            break;
        } else if (read_content_length && (strstr(buf, HEAD_CONTENT_SPLIT) || strstr(buf, CONTENT_LENGTH_FIELD))) {
            break;
        } else if (read_split && strstr(buf, HEAD_CONTENT_SPLIT)) {
            break;
        }
        ssize_t tmp = recv(fd, &(buf[offset]), left_bytes, 0);
        if (tmp == -1) {
            // recv error
            perror("recv");
            exit(1);
        } else if (tmp == 0) {
            should_continue = false;
            break;
        } else {
            offset += tmp;
            left_bytes -= tmp;
            read_length += tmp;
        }
    } while (left_bytes > 0);

    *pread_length = read_length;
    *poffset = offset;

    return should_continue;
}

int fill_get_header(char buf[MAX_BUFFER], const char *filepath, const char *host_name, short port) {
    return sprintf(buf, "GET %s HTTP/1.0\r\nHost: %s:%d\r\n\r\n", filepath, host_name, port);
}

// the name should be splited by '/'
void parse_file_name(const char *file_path, char file_name[MAX_BUFFER]) {
    int len = strlen(file_path);
    int result_len = 0;
    while (len > 0 && file_path[len - 1] != '/') {
        file_name[result_len++] = file_path[len - 1];
        len--;
    }
    for (int i = 0; i < result_len / 2; i++) {
        char tmp = file_name[i];
        file_name[i] = file_name[result_len - 1 - i];
        file_name[result_len - 1 - i] = tmp;
    }
}