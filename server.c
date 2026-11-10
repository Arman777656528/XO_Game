#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/select.h>
#include <signal.h>

#define MAX_CLIENTS 2
#define PORT 8080
#define BUFFER_SIZE 1024
#define BOARD_N 3
#define WIN_K 3

typedef struct {
    char username[32];
    int client_fd;
    char symbol;
    int is_connected;
    char inbuf[BUFFER_SIZE];
    int inlen;
} User;

typedef enum { WAITING, PLAYING, FINISHED } GameState;

typedef struct {
    char cells[BOARD_N * BOARD_N];
    int turn;
    int moves;
    GameState state;
} Game;

int create_server(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Socket failed");
        return -1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) == -1) {
        perror("Setsockopt failed");
        close(server_fd);
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) == -1) {
        perror("Bind failed");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, MAX_CLIENTS) == -1) {
        perror("Listen failed");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

int accept_client(int server_fd) {
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd == -1) {
        perror("Accept failed");
        return -1;
    }

    printf("Accept successfully\n");
    return client_fd;
}

static void send_line(int fd, const char *symbol) {
    send(fd, symbol, strlen(symbol), MSG_NOSIGNAL);
}

static void broadcast(User users[], const char *symbol) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (users[i].is_connected) {
            send_line(users[i].client_fd, symbol);
        }
    }
}

static void game_reset(Game *game) {
    memset(game->cells, '.', sizeof(game->cells));
    game->turn = 0;
    game->moves = 0;
    game->state = WAITING;
}

static void send_board(User users[], const Game *game) {
    char msg[BOARD_N * BOARD_N + 16];
    snprintf(msg, sizeof(msg), "BOARD %.*s\n", BOARD_N * BOARD_N, game->cells);
    broadcast(users, msg);
}

static int count_dir(const Game *game, int row, int col, int row_step, int col_step, char symbol) {
    int same_count = 0;
    row += row_step;
    col += col_step;
    while (row >= 0 && row < BOARD_N && col >= 0 && col < BOARD_N
           && game->cells[row * BOARD_N + col] == symbol) {
        same_count++;
        row += row_step;
        col += col_step;
    }
    return same_count;
}

static int check_win(const Game *game, int row, int col) {
    char symbol = game->cells[row * BOARD_N + col];
    int directions[4][2] = { {0, 1}, {1, 0}, {1, 1}, {1, -1} };

    for (int dir_index = 0; dir_index < 4; dir_index++) {
        int total = 1
                  + count_dir(game, row, col,  directions[dir_index][0],  directions[dir_index][1], symbol)
                  + count_dir(game, row, col, -directions[dir_index][0], -directions[dir_index][1], symbol);
        if (total >= WIN_K) {
            return 1;
        }
    }
    return 0;
}

static void try_start_game(User users[], Game *game) {
    if (game->state != WAITING) {
        return;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!users[i].is_connected || users[i].username[0] == '\0') {
            return;
        }
    }

    game->state = PLAYING;
    game->turn = 0;

    printf("Game started: %s (X) vs %s (O)\n",
           users[0].username, users[1].username);

    broadcast(users, "START\n");
    send_board(users, game);
    send_line(users[game->turn].client_fd, "YOUR_TURN\n");
}

static void handle_move(User users[], int player_index, Game *game, const char *args) {
    User *user = &users[player_index];

    if (game->state != PLAYING) {
        send_line(user->client_fd, "ERR NOT_PLAYING\n");
        return;
    }
    if (game->turn != player_index) {
        send_line(user->client_fd, "ERR NOT_YOUR_TURN\n");
        return;
    }

    int row, col;
    if (sscanf(args, "%d %d", &row, &col) != 2) {
        send_line(user->client_fd, "ERR BAD_FORMAT\n");
        return;
    }
    if (row < 1 || row > BOARD_N || col < 1 || col > BOARD_N) {
        send_line(user->client_fd, "ERR BAD_MOVE\n");
        return;
    }

    row--;
    col--;

    char *cell = &game->cells[row * BOARD_N + col];
    if (*cell != '.') {
        send_line(user->client_fd, "ERR BAD_MOVE\n");
        return;
    }

    *cell = user->symbol;
    game->moves++;

    send_line(user->client_fd, "OK\n");
    send_board(users, game);

    if (check_win(game, row, col)) {
        char msg[16];
        snprintf(msg, sizeof(msg), "WIN %c\n", user->symbol);
        broadcast(users, msg);
        game->state = FINISHED;
    }
    else if (game->moves == BOARD_N * BOARD_N) {
        broadcast(users, "DRAW\n");
        game->state = FINISHED;
    }
    else {
        game->turn = 1 - game->turn;
        send_line(users[game->turn].client_fd, "YOUR_TURN\n");
    }
}

static void handle_command(User users[], int player_index, Game *game, char *line) {
    User *user = &users[player_index];

    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\r') {
        line[len - 1] = '\0';
    }

    if (strncmp(line, "NAME ", 5) == 0) {
        const char *name = line + 5;

        if (name[0] == '\0' || strlen(name) >= sizeof(user->username)
            || strchr(name, ' ') != NULL) {
            send_line(user->client_fd, "ERR BAD_NAME\n");
            return;
        }

        strcpy(user->username, name);
        printf("Player %c is now called %s\n", user->symbol, user->username);
        send_line(user->client_fd, "OK\n");
        try_start_game(users, game);
    }
    else if (strncmp(line, "MOVE ", 5) == 0) {
        handle_move(users, player_index, game, line + 5);
    }
    else {
        send_line(user->client_fd, "ERR UNKNOWN_COMMAND\n");
    }
}

static void handle_disconnect(User users[], int player_index, Game *game) {
    printf("Client disconnected! FD = %d\n", users[player_index].client_fd);

    close(users[player_index].client_fd);
    memset(&users[player_index], 0, sizeof(User));
    users[player_index].client_fd = -1;

    if (game->state == PLAYING) {
        broadcast(users, "OPPONENT_LEFT\n");
    }
    game_reset(game);
}

void run_server(int server_fd) {
    User users[MAX_CLIENTS];
    memset(users, 0, sizeof(users));
    for (int i = 0; i < MAX_CLIENTS; i++) {
        users[i].client_fd = -1;
    }

    Game game;
    game_reset(&game);

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);

        int max_fd = server_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (users[i].is_connected) {
                FD_SET(users[i].client_fd, &readfds);
                if (users[i].client_fd > max_fd) {
                    max_fd = users[i].client_fd;
                }
            }
        }

        int ready = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("Select failed");
            break;
        }

        if (FD_ISSET(server_fd, &readfds)) {
            int new_client = accept_client(server_fd);

            if (new_client != -1) {
                int added = 0;

                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (!users[i].is_connected) {
                        memset(&users[i], 0, sizeof(User));
                        users[i].client_fd = new_client;
                        users[i].symbol = (i == 0) ? 'X' : 'O';
                        users[i].is_connected = 1;
                        added = 1;

                        printf("New client connected! FD = %d\n", new_client);

                        char msg[16];
                        snprintf(msg, sizeof(msg), "WELCOME %c\n",
                                 users[i].symbol);
                        send_line(new_client, msg);
                        break;
                    }
                }

                if (!added) {
                    printf("Server is full!\n");
                    send_line(new_client, "ERR SERVER_FULL\n");
                    close(new_client);
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!users[i].is_connected) {
                continue;
            }

            if (FD_ISSET(users[i].client_fd, &readfds)) {
                char buffer[BUFFER_SIZE];

                ssize_t bytes_received = recv(users[i].client_fd,
                                              buffer, sizeof(buffer), 0);

                if (bytes_received == 0) {
                    handle_disconnect(users, i, &game);
                }
                else if (bytes_received == -1) {
                    if (errno == EINTR) {
                        continue;
                    }
                    perror("Recv failed");
                    handle_disconnect(users, i, &game);
                }
                else {
                    User *u = &users[i];

                    if (u->inlen + bytes_received > BUFFER_SIZE) {
                        send_line(u->client_fd, "ERR LINE_TOO_LONG\n");
                        u->inlen = 0;
                    }
                    else {
                        memcpy(u->inbuf + u->inlen, buffer, bytes_received);
                        u->inlen += bytes_received;

                        char *nl;
                        while ((nl = memchr(u->inbuf, '\n', u->inlen)) != NULL) {
                            *nl = '\0';
                            handle_command(users, i, &game, u->inbuf);

                            int used = (nl - u->inbuf) + 1;
                            memmove(u->inbuf, u->inbuf + used, u->inlen - used);
                            u->inlen -= used;
                        }
                    }
                }
            }
        }
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (users[i].is_connected) {
            close(users[i].client_fd);
        }
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN); 

    int server_fd = create_server(PORT);
    if (server_fd == -1) {
        return 1;
    }

    printf("Server started successfully\n");

    run_server(server_fd);

    close(server_fd);
    return 0;
}