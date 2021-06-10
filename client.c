#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <ctype.h>
#include "terminal.h"

#define CSI "\x1B["

struct message {
    struct message * next;

    long long id;
    char * author;
    char * text;
    bool read;
    bool collapsed;

    struct message * children;
};

struct context {
    struct sockaddr_in server_address;
    char * username;
    int socket;

    struct {
        int top;
        int left;
        int width;
        int height;
        long long reply_id;
        long long selected_id;
        bool writing;
        int move;

        struct {
            size_t capacity;
            size_t length;
            char * buffer;
        } input;
    } ui;

    struct message * messages;
    bool stopping;
};

static int do_connect(struct context * context) {
    context->socket = socket(AF_INET, SOCK_STREAM, 0);
    return connect(context->socket, (struct sockaddr *) &context->server_address, sizeof(context->server_address));
}

static void reconnect(struct context * context) {
    for (int i = 0; i < 10; ++i) {
        close(context->socket);

        if (do_connect(context) == 0) {
            return;
        }
    }

    printf("Cannot connect to remote host.\n");
    context->stopping = true;
}

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

static void context_draw_buffer_bounds(struct message * messages, size_t * width, size_t * height) {
    for (struct message * msg = messages; msg; msg = msg->next) {
        ++(*height);

        size_t message_width = 2 + strlen(msg->author) + 2 + strlen(msg->text);

        if (!msg->collapsed) {
            size_t children_width = 0, children_height = 0;
            context_draw_buffer_bounds(msg->children, &children_width, &children_height);

            children_width += 1;
            if (children_width > message_width) {
                message_width = children_width;
            }

            *height += children_height;
        }

        if (message_width > *width) {
            *width = message_width;
        }
    }
}

static size_t context_draw_buffer_line(char * buffer, const char * line, size_t top, size_t left, size_t width) {
    size_t length = strlen(line);

    memcpy(buffer + top * width + left, line, length);

    return left + length;
}

static bool message_is_branch_read(struct message * message) {
    for (struct message * msg = message; msg; msg = msg->next) {
        if (!msg->read) {
            return false;
        }

        if (!message_is_branch_read(message->children)) {
            return false;
        }
    }

    return true;
}

static int context_draw_buffer_messages(char * buffer, struct context * context, struct message * messages, int top, int left, size_t width, int * selected_top, struct message ** buffer_messages) {
    for (struct message * msg = messages; msg; msg = msg->next) {
        for (int i = 0; i < left; ++i) {
            buffer[top * width + i] = ' ';
        }

        size_t local_left = left;

        if (context->ui.selected_id == msg->id) {
            *selected_top = top;
        }

        if (context->ui.reply_id == msg->id) {
            buffer[top * width + local_left] = '>';
        } else if (!msg->read || !message_is_branch_read(msg->children)) {
            buffer[top * width + local_left] = '!';
        } else {
            buffer[top * width + local_left] = ' ';
        }

        if (msg->collapsed) {
            buffer[top * width + local_left + 1] = '+';
        } else {
            buffer[top * width + local_left + 1] = ' ';
        }

        local_left = context_draw_buffer_line(buffer, msg->author, top, local_left + 2, width);
        local_left = context_draw_buffer_line(buffer, ": ", top, local_left, width);
        local_left = context_draw_buffer_line(buffer, msg->text, top, local_left, width);
        memset(buffer + top * width + local_left, ' ', width - local_left);

        buffer_messages[top] = msg;
        ++top;

        if (!msg->collapsed) {
            top = context_draw_buffer_messages(buffer, context, msg->children, top, left + 1, width, selected_top, buffer_messages);
        }
    }

    return top;
}

/*
 *   a: test
 *  > b: hello
 *    d: mmm
 * !+c: oaoaoa
 */
static char * context_draw_buffer(struct context * context, size_t * width, size_t * height, int * selected_top, struct message *** buffer_messages) {
    char * buffer;

    context_draw_buffer_bounds(context->messages, width, height);

    buffer = malloc(*width * *height);
    *buffer_messages = malloc(sizeof(struct message *) * *height);
    context_draw_buffer_messages(buffer, context, context->messages, 0, 0, *width, selected_top, *buffer_messages);

    return buffer;
}

static void context_redraw_screen(struct context * context) {
    size_t width = 0, height = 0;
    int selected_top = 0;

    if (context->ui.selected_id == 0 && context->messages) {
        context->ui.selected_id = context->messages->id;
    }

    struct message ** buffer_messages;
    char * buffer = context_draw_buffer(context, &width, &height, &selected_top, &buffer_messages);

    if (context->ui.move != 0) {
        selected_top += context->ui.move;

        if (selected_top < 0) {
            selected_top = 0;
        } else if (selected_top >= height) {
            if (height == 0) {
                selected_top = 0;
            } else {
                selected_top = (int) height - 1;
            }
        }

        if (selected_top > 0 && selected_top < height) {
            context->ui.selected_id = buffer_messages[selected_top]->id;
        }

        context->ui.move = 0;
    }

    if (selected_top < context->ui.top) {
        context->ui.top = selected_top;
    } else if (selected_top >= context->ui.top + context->ui.height - 2) {
        context->ui.top = selected_top - (context->ui.height - 2) + 1;
    }

    printf(CSI"2J");

    size_t top;
    for (top = context->ui.top; top < height && top - context->ui.top < context->ui.height - 2; ++top) {
        printf(CSI"%zu;1H", top - context->ui.top + 1);
        fflush(stdout);

        size_t buffer_line = width - context->ui.left;

        write(STDOUT_FILENO, buffer + top * width + context->ui.left,
              buffer_line < context->ui.width ? buffer_line : context->ui.width);

        buffer_messages[top]->read = true;
    }

    top = context->ui.height - 1;

    const char * help = " q - quit, r - reply, n - new, c - collapse (fold), wasd - moving";
    size_t help_length = strlen(help);
    size_t help_left = context->ui.left % help_length;
    help_length -= help_left;

    printf(CSI"%zu;1H", top);
    fflush(stdout);

    write(STDOUT_FILENO, help + help_left, help_length < context->ui.width ? help_length : context->ui.width);
    ++top;

    printf(CSI"%zu;1HYour message: ", top);
    fflush(stdout);

    char * input_start = context->ui.input.buffer;
    size_t input_length = context->ui.input.length;

    if (context->ui.input.length > context->ui.width - 15) {
        input_start += input_length - (context->ui.width - 15);
        input_length = context->ui.width - 15;
    }

    write(STDOUT_FILENO, input_start, input_length);

    if (context->ui.writing) {
        printf(CSI"%zu;%zuH", top, input_length + 15);
    } else {
        printf(CSI"%d;1H", selected_top - context->ui.top + 1);
    }

    fflush(stdout);
    free(buffer_messages);
    free(buffer);
}

static void context_add_message(struct context * context, long long id, long long reply_id, const char * username, const char * message) {
    struct message * parent = NULL;

    if (reply_id) {
        parent = message_find_by_id(context->messages, reply_id);
    }

    struct message ** list = parent ? &parent->children : &context->messages;
    for (struct message * msg = *list; msg; msg = msg->next) {
        if (msg->id > id) {
            break;
        }

        list = &msg->next;
    }

    struct message * new_message = malloc(sizeof(struct message));

    new_message->next = *list;
    new_message->id = id;
    new_message->author = strdup(username);
    new_message->text = strdup(message);
    new_message->read = false;
    new_message->collapsed = false;
    new_message->children = NULL;
    *list = new_message;

    context_redraw_screen(context);
}

// packet: <id><reply_id or 0><strlen(username)><username><strlen(message)><message>
static void * listen_to_server(void * param) {
    struct context * context = param;

    while (!context->stopping) {
        long long id, reply_id;

        if (read(context->socket, &id, sizeof(id)) < 0) {
            if (!context->stopping) {
                reconnect(context);
            }

            continue;
        }

        read(context->socket, &reply_id, sizeof(reply_id));

        size_t username_length, message_length;
        read(context->socket, &username_length, sizeof(username_length));

        char username[username_length + 1];
        read(context->socket, username, username_length);
        username[username_length] = '\0';

        read(context->socket, &message_length, sizeof(message_length));

        char message[message_length + 1];
        read(context->socket, message, message_length);
        message[message_length] = '\0';

        context_add_message(context, id, reply_id, username, message);
    }

    pthread_exit(0);
}

static void run_server_handler(struct context * context) {
    pthread_t tid; /* идентификатор потока */
    pthread_attr_t attr; /* атрибуты потока */

/* получаем дефолтные значения атрибутов */
    pthread_attr_init(&attr);

/* создаем новый поток */
    pthread_create(&tid, &attr, listen_to_server, context);
}

static void context_send_message(struct context * context, const char * message) {
    size_t username_length = strlen(context->username), message_length = strlen(message);

    write(context->socket, &context->ui.reply_id, sizeof(context->ui.reply_id));
    write(context->socket, &username_length, sizeof(username_length));
    write(context->socket, context->username, username_length);
    write(context->socket, &message_length, sizeof(message_length));
    write(context->socket, message, message_length);
}

static void handle_console(struct context * context) {
    while (!context->stopping) {
        int c = getc(stdin);

        if (context->ui.writing) {
            if (c == '\n') {
                if (context->ui.input.length == 0) {
                    context->ui.writing = false;
                    context->ui.reply_id = 0;
                    context_redraw_screen(context);
                    continue;
                }

                char * buf = malloc(context->ui.input.length + 1);
                memcpy(buf, context->ui.input.buffer, context->ui.input.length);
                buf[context->ui.input.length] = '\0';

                context->ui.input.length = 0;
                context->ui.writing = false;
                context_redraw_screen(context);

                context_send_message(context, buf);
                free(buf);

                context->ui.reply_id = 0;
                continue;
            }

            if (c == '\177') {
                if (context->ui.input.length > 0) {
                    --context->ui.input.length;
                    context_redraw_screen(context);
                }

                continue;
            }

            if (isprint(c) || c == ' ') {
                if (context->ui.input.capacity == context->ui.input.length) {
                    context->ui.input.capacity *= 2;
                    context->ui.input.buffer = realloc(context->ui.input.buffer, context->ui.input.capacity);
                }

                context->ui.input.buffer[context->ui.input.length++] = (char) c;
                context_redraw_screen(context);
            }

            continue;
        }

        switch (c) {
            case 'q':
                context->stopping = true;
                break;

            case 'r':
                context->ui.reply_id = context->ui.selected_id;
                context->ui.writing = true;
                context_redraw_screen(context);
                break;

            case 'n':
                context->ui.reply_id = 0;
                context->ui.writing = true;
                context_redraw_screen(context);
                break;

            case 'c':
            {
                struct message * msg = message_find_by_id(context->messages, context->ui.selected_id);
                if (msg) {
                    msg->collapsed = !msg->collapsed;
                    context_redraw_screen(context);
                }

                break;
            }

            case 'w':
                context->ui.move = -1;
                context_redraw_screen(context);
                break;

            case 's':
                context->ui.move = 1;
                context_redraw_screen(context);
                break;

            case 'a':
                if (context->ui.left > 0) {
                    --context->ui.left;
                    context_redraw_screen(context);
                }

                break;

            case 'd':
                ++context->ui.left;
                context_redraw_screen(context);
                break;
        }
    }
}

int client_main(const char * username, const char * host) {
    // specify an address for the socket
    struct context * context = malloc(sizeof(struct context));

    context->server_address.sin_family = AF_INET;
    context->server_address.sin_port = htons(9002);
    if (inet_aton(host, &context->server_address.sin_addr) == 0) {
        printf("Bad hostname format: %s\n", host);
        return 1;
    }

    // check for error with the connection
    if (do_connect(context)) {
        perror("There was an error making a connection to the remote socket");
        return 2;
    }

    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

    context->username = strdup(username);

    context->ui.top = 0;
    context->ui.left = 0;
    context->ui.width = w.ws_col;
    context->ui.height = w.ws_row;
    context->ui.reply_id = 0;
    context->ui.selected_id = 0;
    context->ui.writing = false;
    context->ui.move = 0;

    context->ui.input.capacity = 256;
    context->ui.input.length = 0;
    context->ui.input.buffer = malloc(256);

    context->messages = NULL;
    context->stopping = false;

    struct termios stored_settings = set_keypress();

    context_redraw_screen(context);

    run_server_handler(context);
    handle_console(context);

    // and then close the socket
    close(context->socket);

    reset_keypress(stored_settings);
    printf(CSI"2J");
    fflush(stdout);

    return 0;
}
