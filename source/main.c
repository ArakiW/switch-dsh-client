#include <stdio.h>
#include <switch.h>

#include "app.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* 网络(curl/mbedtls)需要 */
    socketInitializeDefault();

    if (app_init() != 0) {
        printf("main: app_init failed\n");
        socketExit();
        return 1;
    }

    while (appletMainLoop()) {
        if (app_frame() != 0) break;
    }

    app_exit();
    socketExit();
    return 0;
}
