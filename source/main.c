// Odyssey Mod Switch: a two-column mod picker, like Minecraft's resource pack
// screen. Press A or tap a mod to move it across. Only one mod can be selected,
// because they all replace the same game files.
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "modswitch.h"

#define ROOT "sdmc:"
#define EMUMMC_CONFIG_ITEM 65007
#define COLUMN_RIGHT_X 42
#define HEADER_ROW 5
#define FIRST_ROW 7
#define VISIBLE_ROWS 26
#define NAME_WIDTH 34
#define CELL_SIZE 16

#define COLOR_RESET "\x1b[0m"
#define COLOR_TITLE "\x1b[36;1m"
#define COLOR_GREEN "\x1b[32;1m"
#define COLOR_RED "\x1b[31;1m"
#define COLOR_YELLOW "\x1b[33;1m"
#define COLOR_DIM "\x1b[30;1m"
#define COLOR_CURSOR "\x1b[47;30m"

typedef enum { SYSTEM_UNKNOWN = 0, SYSTEM_EMUMMC, SYSTEM_SYSMMC } system_kind;

static ms_list g_mods;
static ms_active g_active;
static int g_available[MS_MAX_MODS];
static int g_available_count;
static system_kind g_system;
static int g_column;
static int g_row;
static int g_scroll;
static char g_message[160];
static const char *g_message_color = COLOR_RESET;

static system_kind detect_system(void) {
    if (R_FAILED(splInitialize())) {
        return SYSTEM_UNKNOWN;
    }
    u64 value = 0;
    Result result = splGetConfig((SplConfigItem)EMUMMC_CONFIG_ITEM, &value);
    splExit();
    if (R_FAILED(result)) {
        return SYSTEM_UNKNOWN;
    }
    return value != 0 ? SYSTEM_EMUMMC : SYSTEM_SYSMMC;
}

// In title override mode the running application is this app, so there's nothing
// to check. In applet mode, any running application could be using the mod files.
static bool game_is_running(void) {
    AppletType type = appletGetAppletType();
    if (type == AppletType_Application || type == AppletType_SystemApplication) {
        return false;
    }
    if (R_FAILED(pmdmntInitialize())) {
        return false;
    }
    u64 pid = 0;
    Result result = pmdmntGetApplicationProcessId(&pid);
    pmdmntExit();
    return R_SUCCEEDED(result) && pid != 0;
}

static void set_message(const char *color, const char *text) {
    g_message_color = color;
    snprintf(g_message, sizeof(g_message), "%s", text);
}

static void refresh(void) {
    if (ms_list_mods(ROOT, &g_mods) != MS_OK) {
        g_mods.count = 0;
        set_message(COLOR_RED, "Couldn't read the mods folder.");
    }
    if (ms_detect_active(ROOT, &g_active) != MS_OK) {
        g_active.kind = MS_ACTIVE_UNKNOWN;
        g_active.name[0] = '\0';
    }
    g_available_count = 0;
    for (int i = 0; i < g_mods.count; i++) {
        if (g_active.kind == MS_ACTIVE_MOD && strcmp(g_mods.mods[i].name, g_active.name) == 0) {
            continue;
        }
        g_available[g_available_count++] = i;
    }
    if (g_row >= g_available_count) {
        g_row = g_available_count > 0 ? g_available_count - 1 : 0;
    }
    if (g_scroll > g_row) {
        g_scroll = g_row;
    }
    if (g_row >= g_scroll + VISIBLE_ROWS) {
        g_scroll = g_row - VISIBLE_ROWS + 1;
    }
    if (g_active.kind == MS_ACTIVE_NONE && g_column == 1) {
        g_column = 0;
    }
}

static void draw_cell(int row, int x, const char *text, const char *color, bool selected) {
    printf("\x1b[%d;%dH%s%s%-*.*s" COLOR_RESET, row, x, selected ? COLOR_CURSOR : "", selected ? "" : color,
           NAME_WIDTH, NAME_WIDTH, text);
}

static void draw(void) {
    consoleClear();
    printf("\x1b[2;3H" COLOR_TITLE "Odyssey Mod Switch" COLOR_RESET);
    const char *system_text = "Unknown system: mods can't be turned on";
    const char *system_color = COLOR_RED;
    if (g_system == SYSTEM_EMUMMC) {
        system_text = "emuMMC";
        system_color = COLOR_GREEN;
    } else if (g_system == SYSTEM_SYSMMC) {
        system_text = "sysCFW: mods can't be turned on here";
        system_color = COLOR_YELLOW;
    }
    printf("\x1b[3;3HSystem: %s%s" COLOR_RESET, system_color, system_text);
    printf("\x1b[%d;3H" COLOR_TITLE "AVAILABLE" COLOR_RESET, HEADER_ROW);
    printf("\x1b[%d;%dH" COLOR_TITLE "SELECTED (one at a time)" COLOR_RESET, HEADER_ROW, COLUMN_RIGHT_X);

    if (g_available_count == 0) {
        printf("\x1b[%d;3H" COLOR_DIM "(nothing else available)" COLOR_RESET, FIRST_ROW);
    }
    for (int i = 0; i < VISIBLE_ROWS && g_scroll + i < g_available_count; i++) {
        const ms_mod *mod = &g_mods.mods[g_available[g_scroll + i]];
        char label[MS_NAME_MAX + 16];
        snprintf(label, sizeof(label), "%s%s", mod->name, mod->usable ? "" : " (unsupported)");
        bool selected = g_column == 0 && g_row == g_scroll + i;
        draw_cell(FIRST_ROW + i, 3, label, mod->usable ? COLOR_RESET : COLOR_DIM, selected);
    }

    if (g_active.kind == MS_ACTIVE_MOD) {
        draw_cell(FIRST_ROW, COLUMN_RIGHT_X, g_active.name, COLOR_GREEN, g_column == 1);
    } else if (g_active.kind == MS_ACTIVE_UNKNOWN) {
        draw_cell(FIRST_ROW, COLUMN_RIGHT_X, "Unknown files", COLOR_YELLOW, g_column == 1);
    } else {
        printf("\x1b[%d;%dH" COLOR_DIM "(no mods: normal game)" COLOR_RESET, FIRST_ROW, COLUMN_RIGHT_X);
    }

    printf("\x1b[%d;3H%s%.76s" COLOR_RESET, FIRST_ROW + VISIBLE_ROWS + 1, g_message_color, g_message);
    printf("\x1b[%d;3HA or tap: move across    D-pad: choose    +: exit", FIRST_ROW + VISIBLE_ROWS + 3);
    printf("\x1b[%d;3H" COLOR_DIM "Changes apply the next time you launch Odyssey." COLOR_RESET,
           FIRST_ROW + VISIBLE_ROWS + 4);
    consoleUpdate(NULL);
}

static void show_working(const char *text) {
    set_message(COLOR_YELLOW, text);
    draw();
}

static void report(ms_result result, const char *recovered, const char *success) {
    char text[sizeof(g_message)];
    if (result != MS_OK) {
        snprintf(text, sizeof(text), "%s. Your mods folder is unchanged.", ms_strerror(result));
        set_message(COLOR_RED, text);
    } else if (recovered[0] != '\0') {
        snprintf(text, sizeof(text), "%s Unknown files were kept in %.40s.", success, recovered);
        set_message(COLOR_YELLOW, text);
    } else {
        set_message(COLOR_GREEN, success);
    }
}

static ms_result guarded_begin(void) {
    appletLockExit();
    appletSetAutoSleepDisabled(true);
    return MS_OK;
}

static void guarded_end(void) {
    appletSetAutoSleepDisabled(false);
    appletUnlockExit();
}

static void turn_on(const ms_mod *mod) {
    if (!mod->usable) {
        set_message(COLOR_YELLOW, "That mod's folders aren't in a supported layout.");
        return;
    }
    if (g_system != SYSTEM_EMUMMC) {
        set_message(COLOR_YELLOW, "Mods can only be turned on in emuMMC.");
        return;
    }
    if (game_is_running()) {
        set_message(COLOR_YELLOW, "Close the running game first (Home, then X).");
        return;
    }
    char recovered[MS_NAME_MAX] = "";
    char text[sizeof(g_message)];
    snprintf(text, sizeof(text), "Turning on %.60s...", mod->name);
    show_working(text);
    guarded_begin();
    ms_result result = ms_enable(ROOT, mod->name, recovered, sizeof(recovered));
    guarded_end();
    snprintf(text, sizeof(text), "%.60s is on.", mod->name);
    report(result, recovered, text);
    refresh();
    if (result == MS_OK) {
        g_column = 1;
    }
}

static void turn_off(void) {
    if (game_is_running()) {
        set_message(COLOR_YELLOW, "Close the running game first (Home, then X).");
        return;
    }
    char recovered[MS_NAME_MAX] = "";
    show_working("Turning off mods...");
    guarded_begin();
    ms_result result = ms_disable(ROOT, recovered, sizeof(recovered));
    guarded_end();
    report(result, recovered, "Mods are off.");
    refresh();
    if (result == MS_OK) {
        g_column = 0;
    }
}

static void activate(void) {
    if (g_column == 1) {
        if (g_active.kind != MS_ACTIVE_NONE) {
            turn_off();
        }
        return;
    }
    if (g_row >= 0 && g_row < g_available_count) {
        turn_on(&g_mods.mods[g_available[g_row]]);
    }
}

static void handle_touch(int x, int y) {
    int cell_row = y / CELL_SIZE + 1;
    int cell_column = x / CELL_SIZE + 1;
    int index = cell_row - FIRST_ROW;
    if (index < 0 || index >= VISIBLE_ROWS) {
        return;
    }
    if (cell_column >= COLUMN_RIGHT_X) {
        if (index == 0 && g_active.kind != MS_ACTIVE_NONE) {
            g_column = 1;
            activate();
        }
        return;
    }
    if (g_scroll + index < g_available_count) {
        g_column = 0;
        g_row = g_scroll + index;
        activate();
    }
}

int main(void) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

    g_system = detect_system();
    set_message(COLOR_RESET, "");
    ms_result recovery = ms_cleanup(ROOT);
    refresh();
    if (recovery != MS_OK) {
        set_message(COLOR_RED, ms_strerror(recovery));
    }
    if (g_mods.truncated) {
        set_message(COLOR_YELLOW, "Showing the first 64 mods only.");
    }
    draw();

    s32 previous_touches = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        bool changed = false;

        if (down & HidNpadButton_Plus) {
            break;
        }
        if (down & HidNpadButton_AnyDown) {
            if (g_column == 0 && g_row + 1 < g_available_count) {
                g_row++;
                if (g_row >= g_scroll + VISIBLE_ROWS) {
                    g_scroll++;
                }
                changed = true;
            }
        }
        if (down & HidNpadButton_AnyUp) {
            if (g_column == 0 && g_row > 0) {
                g_row--;
                if (g_row < g_scroll) {
                    g_scroll = g_row;
                }
                changed = true;
            }
        }
        if ((down & HidNpadButton_AnyRight) && g_column == 0 && g_active.kind != MS_ACTIVE_NONE) {
            g_column = 1;
            changed = true;
        }
        if ((down & HidNpadButton_AnyLeft) && g_column == 1) {
            g_column = 0;
            changed = true;
        }
        if (down & HidNpadButton_A) {
            activate();
            changed = true;
        }

        HidTouchScreenState touch = {0};
        if (hidGetTouchScreenStates(&touch, 1) > 0) {
            if (touch.count > 0 && previous_touches == 0) {
                handle_touch((int)touch.touches[0].x, (int)touch.touches[0].y);
                changed = true;
            }
            previous_touches = touch.count;
        }

        if (changed) {
            draw();
        } else {
            consoleUpdate(NULL);
        }
    }
    consoleExit(NULL);
    return 0;
}
