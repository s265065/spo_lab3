#include <string.h>

#include "main.h"

int main(int argc, char * argv[]) {
    if (argc < 2 || strlen(argv[1]) != 1) {
        return 0;
    }

    switch (argv[1][0]) {
        case 'c':
            if (argc != 4) {
                return 0;
            }

            return client_main(argv[2], argv[3]);

        case 's':
            if (argc != 2) {
                return 0;
            }

            return server_main();
    }

    return 0;
}
