#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "textinput.h"

int textinput_prompt(const char *header, const char *initial, int zh_only,
                     int password, char *out, size_t outsz) {
    if (!out || outsz == 0) return -1;
    out[0] = '\0';

    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) return -1;

    if (password) swkbdConfigMakePresetPassword(&kbd);
    else swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetType(&kbd, zh_only ? SwkbdType_ZhHans : SwkbdType_Normal);
    if (header && header[0]) swkbdConfigSetHeaderText(&kbd, header);
    if (initial && initial[0]) swkbdConfigSetInitialText(&kbd, initial);
    swkbdConfigSetStringLenMax(&kbd, (u32)(outsz - 1));

    rc = swkbdShow(&kbd, out, outsz);
    swkbdClose(&kbd);

    if (R_SUCCEEDED(rc) && out[0] != '\0') return 1;
    if (R_FAILED(rc)) printf("textinput: swkbdShow rc=0x%x (视为取消)\n", rc);
    return 0;
}
