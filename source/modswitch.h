// Core logic for turning Super Mario Odyssey mods on and off.
//
// Design rules:
//  - The library in /mods/Super Mario Odyssey is the source of truth and is only
//    ever read, except for adding _Recovered-N folders.
//  - Nothing is deleted outside this tool's own work folder. Active files are
//    moved there first, so a bug can't delete anything else on the card.
//  - Files that don't match a library mod are never deleted. They move to a new
//    _Recovered-N folder in the library.
//  - What's active is worked out by comparing files, so there's no state file
//    that can go stale after a crash or a manual swap.
#ifndef MODSWITCH_H
#define MODSWITCH_H

#include <stddef.h>

#define MS_MAX_MODS 64
#define MS_NAME_MAX 128

typedef enum {
    MS_OK = 0,
    MS_ERR_BAD_NAME,
    MS_ERR_NO_MOD,
    MS_ERR_BAD_LAYOUT,
    MS_ERR_IO,
    MS_ERR_VERIFY,
    MS_ERR_PATH_TOO_LONG,
    MS_ERR_GUARD,
} ms_result;

typedef enum {
    MS_ACTIVE_NONE = 0,
    MS_ACTIVE_MOD,
    MS_ACTIVE_UNKNOWN,
} ms_active_kind;

typedef struct {
    ms_active_kind kind;
    char name[MS_NAME_MAX];
} ms_active;

typedef struct {
    char name[MS_NAME_MAX];
    int usable;
} ms_mod;

typedef struct {
    ms_mod mods[MS_MAX_MODS];
    int count;
    int truncated;
} ms_list;

// root is the card root without a trailing slash, such as "sdmc:".
ms_result ms_cleanup(const char *root);
ms_result ms_list_mods(const char *root, ms_list *out);
ms_result ms_detect_active(const char *root, ms_active *out);

// recovered, if not NULL, receives the _Recovered-N folder name when unknown
// files had to be moved out of the way. It's left empty otherwise.
ms_result ms_enable(const char *root, const char *name, char *recovered, size_t recovered_size);
ms_result ms_disable(const char *root, char *recovered, size_t recovered_size);

const char *ms_strerror(ms_result result);

#ifdef MS_TEST
// Simulates a power cut (crash_at) or a one-time I/O error (fail_at) at the Nth
// file change. Pass -1 to turn either one off.
void ms_test_set_faults(long crash_at, long fail_at);
long ms_test_ops(void);
#endif

#endif
