#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <ctype.h>

#include "terminal.h"

struct message {
    struct message * next;

    long long id;
    char * author;
    char * text;

    struct message * children;
};

struct server_context {
    struct {
        int capacity;
        int amount;
        int * sockets;
    } clients;

    long long prev_id;
    struct message * messages;

    bool closing;
};

struct client_context {
    struct server_context * server_context;
    int socket;
};

static struct message * message_find_by_id(struct message * message, long long id) {
    for (struct message * msg = message; msg; msg = msg->next) {
        if (msg->id == id) {
            return msg;
        }

        struct message * child = message_find_by_id(msg->children, id);
        if (child) {
            return child;
        }
    }

    return NULL;
}

static void server_context_add_message(struct server_context * context, long long reply_id, char * username, char * message) {
    struct message * parent = NULL;

    if (reply_id) {
        parent = message_find_by_id(context->messages, reply_id);
    }

    struct message ** list = parent ? &parent->children : &context->messages;
    struct message * new_message = malloc(sizeof(struct message));

    new_message->next = *list;
    new_message->id = ++context->prev_id;
    new_message->author = strdup(username);
    new_message->text = strdup(message);
    new_message->children = NULL;
    *list = new_message;

    size_t username_length = strlen(username), message_length = strlen(message);
    for (int i = 0; i < context->clients.amount; ++i) {
        write(context->clients.sockets[i], &new_message->id, sizeof(new_message->id));
        write(context->clients.sockets[i], &reply_id, sizeof(reply_id));
        write(context->clients.sockets[i], &username_length, sizeof(username_length));
        write(context->clients.sockets[i], username, username_length);
        write(context->clients.sockets[i], &message_length, sizeof(message_length));
        write(context->clients.sockets[i], message, message_length);
    }

    if (reply_id) {
        printf("Message from %s as a reply to %lld: %s\n", username, reply_id, message);
    } else {
        printf("Message from %s: %s\n", username, message);
    }
}

// packet: <reply_id or 0><strlen(username)><username><strlen(message)><message>
static void * listen_to_client(void * param) {
    struct client_context * context = param;

    while (!context->server_context->closing) {
        long long reply_id;
        if (read(context->socket, &reply_id, sizeof(reply_id)) <= 0) {
            for (int i = 0; i < context->server_context->clients.amount; ++i) {
                if (context->server_context->clients.sockets[i] == context->socket) {
                    context->server_context->clients.sockets[i] = context->server_context->clients.sockets[--context->server_context->clients.amount];
                    break;
                }
            }

            close(context->socket);
            break;
        }

        size_t username_length, message_length;
        read(context->socket, &username_length, sizeof(username_length));

        char username[username_length + 1];
        read(context->socket, username, username_length);
        username[username_length] = '\0';

        read(context->socket, &message_length, sizeof(message_length));

        char message[message_length + 1];
        read(context->socket, message, message_length);
        message[message_length] = '\0';

        server_context_add_message(context->server_context, reply_id, username, message);
    }

    pthread_exit(0);
}

static void server_context_add_client(struct server_context * context, int socket) {
    if (context->clients.capacity == context->clients.amount) {
        context->clients.capacity *= 2;
        context->clients.sockets = realloc(context->clients.sockets, sizeof(int) * context->clients.capacity);
    }

    context->clients.sockets[context->clients.amount++] = socket;
}

static void handle_client_send_messages(int socket, struct message * messages, long long reply_id) {
    for (struct message * msg = messages; msg; msg = msg->next) {
        size_t author_length = strlen(msg->author), text_length = strlen(msg->text);

        write(socket, &msg->id, sizeof(msg->id));
        write(socket, &reply_id, sizeof(reply_id));
        write(socket, &author_length, sizeof(author_length));
        write(socket, msg->author, author_length);
        write(socket, &text_length, sizeof(text_length));
        write(socket, msg->text, text_length);

        handle_client_send_messages(socket, msg->children, msg->id);
    }
}

static void handle_client(int socket, struct server_context * server_context) {
    pthread_t tid; /* идентификатор потока */
    pthread_attr_t attr; /* атрибуты потока */

/* получаем дефолтные значения атрибутов */
    pthread_attr_init(&attr);

    struct client_context * client_context = malloc(sizeof(struct client_context));
    client_context->server_context = server_context;
    client_context->socket = socket;

    handle_client_send_messages(socket, server_context->messages, 0);

    server_context_add_client(server_context, socket);

/* создаем новый поток */
    pthread_create(&tid, &attr, listen_to_client, client_context);
}

static void * handle_console(void * param) {
    struct server_context * context = param;

    while (!context->closing) {
        int c = getc(stdin);

        switch (c) {
            case 'h':
                printf("Available commands: h - help, q - quit\n");

            case 'q':
                context->closing = true;
                break;

            default:
                if (isprint(c) && !isspace(c)) {
                    printf("Unrecognized command: %c\n", c);
                }
        }
    }

    pthread_exit(0);
}

static void run_console_handler(struct server_context * context) {
    pthread_t tid; /* идентификатор потока */
    pthread_attr_t attr; /* атрибуты потока */

/* получаем дефолтные значения атрибутов */
    pthread_attr_init(&attr);

/* создаем новый поток */
    pthread_create(&tid, &attr, handle_console, context);
}

int server_main() {
    // create the server socket
    int server_socket;
    server_socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

    // define the server address
    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(9002);
    server_address.sin_addr.s_addr = INADDR_ANY;

    // bind the socket to our specified IP and port
    bind(server_socket, (struct sockaddr*) &server_address, sizeof(server_address));

    // second argument is a backlog - how many connections can be waiting for this socket simultaneously
    listen(server_socket, 255);

    struct server_context * context = malloc(sizeof(struct server_context));
    context->clients.amount = 0;
    context->clients.capacity = 2;
    context->clients.sockets = malloc(sizeof(int) * 2);
    context->prev_id = 0;
    context->messages = NULL;
    context->closing = false;

    struct termios stored_settings = set_keypress();

    run_console_handler(context);

    {
        struct sigaction sa;
        sa.sa_handler = SIG_IGN;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;

        sigaction(SIGPIPE, &sa, NULL);
    }

    while (!context->closing) {
        int ret = accept(server_socket, NULL, NULL);

        if (ret >= 0) {
            handle_client(ret, context);
        }

        sched_yield();
    }

    for (int i = 0; i < context->clients.amount; ++i) {
        close(context->clients.sockets[i]);
    }

    close(server_socket);
    reset_keypress(stored_settings);

    printf("Bye!\n");
    return 0;
}
