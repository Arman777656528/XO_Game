#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>

#define BUFFER_SIZE 1024
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 8080

static int connect_to_server(const char *host, int port) {
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("Socket failed");
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid address: %s\n", host);
        close(server_socket);
        return -1;
    }

    if (connect(server_socket, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) == -1) {
        perror("Connect failed");
        close(server_socket);
        return -1;
    }

    return server_socket;
}

static void send_line(int socket_fd, const char *text) {
    send(socket_fd, text, strlen(text), MSG_NOSIGNAL);
}

static void print_board(const char *cells) {
    int cell_count = strlen(cells);
    int board_size = 0;
    while (board_size * board_size < cell_count) {
        board_size++;
    }

    printf("\n    ");
    for (int col = 0; col < board_size; col++) {
        printf(" %-3d", col + 1);
    }
    printf("\n");

    for (int row = 0; row < board_size; row++) {
        printf("    ");
        for (int col = 0; col < board_size; col++) {
            printf("+---");
        }
        printf("+\n");

        printf(" %2d ", row + 1);
        for (int col = 0; col < board_size; col++) {
            char cell = cells[row * board_size + col];
            printf("| %c ", cell == '.' ? ' ' : cell);
        }
        printf("|\n");
    }

    printf("    ");
    for (int col = 0; col < board_size; col++) {
        printf("+---");
    }
    printf("+\n\n");
}
static void handle_server_line(const char *line) {
    if (strncmp(line, "WELCOME ", 8) == 0) {
        printf("Connected. You are playing as %s.\n", line + 8);
    }
    else if (strcmp(line, "START") == 0) {
        printf("The game has started.\n");
    }
    else if (strncmp(line, "BOARD ", 6) == 0) {
        print_board(line + 6);
    }
    else if (strcmp(line, "YOUR_TURN") == 0) {
        printf("Your turn. Enter \"row column\" (e.g. 2 2)\n> ");
        fflush(stdout);
    }
    else if (strcmp(line, "OK") == 0) {
    }
    else if (strncmp(line, "WIN ", 4) == 0) {
        printf("Game over. %s wins!\n", line + 4);
    }
    else if (strcmp(line, "DRAW") == 0) {
        printf("Game over. It's a draw.\n");
    }
    else if (strcmp(line, "OPPONENT_LEFT") == 0) {
        printf("Your opponent left. Waiting for a new player.\n");
    }
    else if (strncmp(line, "ERR ", 4) == 0) {
        printf("Error: %s\n> ", line + 4);
        fflush(stdout);
    }
    else {
        printf("Server: %s\n", line);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <name> [host] [port]\n", argv[0]);
        return 1;
    }

    const char *player_name = argv[1];
    const char *host = (argc > 2) ? argv[2] : DEFAULT_HOST;
    int port = (argc > 3) ? atoi(argv[3]) : DEFAULT_PORT;

    signal(SIGPIPE, SIG_IGN);

    int server_socket = connect_to_server(host, port);
    if (server_socket == -1) {
        return 1;
    }

    char name_command[64];
    snprintf(name_command, sizeof(name_command), "NAME %s\n", player_name);
    send_line(server_socket, name_command);

    struct pollfd poll_fds[2];
    poll_fds[0].fd = STDIN_FILENO;
    poll_fds[0].events = POLLIN;
    poll_fds[1].fd = server_socket;
    poll_fds[1].events = POLLIN;

    char receive_buffer[BUFFER_SIZE];
    int receive_length = 0;

    while (1) {
        int ready_count = poll(poll_fds, 2, -1);
        if (ready_count == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("Poll failed");
            break;
        }

        if (poll_fds[0].revents & POLLIN) {
            char typed_line[128];
            if (fgets(typed_line, sizeof(typed_line), stdin) == NULL) {
                break; 
            }

            typed_line[strcspn(typed_line, "\r\n")] = '\0';

            if (strcmp(typed_line, "quit") == 0) {
                break;
            }
            if (typed_line[0] != '\0') {
                char move_command[160];
                snprintf(move_command, sizeof(move_command),
                         "MOVE %s\n", typed_line);
                send_line(server_socket, move_command);
            }
        }

        if (poll_fds[1].revents & (POLLIN | POLLHUP)) {
            char receive_chunk[BUFFER_SIZE];
            ssize_t bytes_received = recv(server_socket, receive_chunk,
                                          sizeof(receive_chunk), 0);

            if (bytes_received == 0) {
                printf("Server closed the connection.\n");
                break;
            }
            if (bytes_received == -1) {
                if (errno == EINTR) {
                    continue;
                }
                perror("Recv failed");
                break;
            }

            if (receive_length + bytes_received > BUFFER_SIZE) {
                receive_length = 0; 
                continue;
            }

            memcpy(receive_buffer + receive_length, receive_chunk,
                   bytes_received);
            receive_length += bytes_received;

            char *newline_pos;
            while ((newline_pos = memchr(receive_buffer, '\n',
                                         receive_length)) != NULL) {
                *newline_pos = '\0';
                handle_server_line(receive_buffer);

                int consumed = (newline_pos - receive_buffer) + 1;
                memmove(receive_buffer, receive_buffer + consumed,
                        receive_length - consumed);
                receive_length -= consumed;
            }
        }
    }

    close(server_socket);
    return 0;
}