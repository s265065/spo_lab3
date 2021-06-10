#include "terminal.h"

struct termios set_keypress() {
    struct termios stored_settings, new_settings;

    tcgetattr(0, &stored_settings);

    new_settings = stored_settings;

    new_settings.c_lflag &= (~ICANON & ~ECHO);
    new_settings.c_cc[VTIME] = 0;
    new_settings.c_cc[VMIN] = 1;

    tcsetattr(0, TCSANOW, &new_settings);
    return stored_settings;
}

void reset_keypress(struct termios stored_settings) {
    tcsetattr(0, TCSANOW, &stored_settings);
}
