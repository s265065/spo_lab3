#pragma once

#include <termios.h>

struct termios set_keypress(void);
void reset_keypress(struct termios stored_settings);
