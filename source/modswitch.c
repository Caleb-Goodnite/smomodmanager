#include "modswitch.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIB_DIR "/mods/Super Mario Odyssey"
#define ACTIVE_CONTENTS "/atmosphere/contents/0100000000010000"
#define ACTIVE_PATCH "/atmosphere/exefs_patches/StarlightBase"
#define WORK_DIR "/config/odyssey-mod-switch"
#define STAGE_DIR WORK_DIR "/stage"
#define OLD_DIR WORK_DIR "/old"
#define KEEP_CHEATS WORK_DIR "/keep-cheats"
#define MOD_CONTENTS "/atmosphere/contents/0100000000010000"
#define MOD_PATCH "/atmosphere/exefs_patches/StarlightBase"
#define STAGE_CONTENTS STAGE_DIR "/contents"
#define STAGE_PATCH STAGE_DIR "/patch"
#define OLD_CONTENTS OLD_DIR "/contents"
#define OLD_PATCH OLD_DIR "/patch"
#define CHEATS_NAME "cheats"
#define METADATA_NAME "romfs_metadata.bin"

#define PATH_BUF 1024
#define MAX_DEPTH 24
#define COPY_BUF_SIZE (64 * 1024)
#define MAX_RECOVERED 999

enum { KIND_NONE = 0, KIND_FILE, KIND_DIR, KIND_OTHER };

// Scope tells the tree walkers which extra names to ignore. Atmosphère creates
// romfs_metadata.bin, and cheats belong to the player rather than to a mod.
typedef enum { SCOPE_PLAIN = 0, SCOPE_CONTENTS_ROOT } tree_scope;

#ifdef MS_TEST
static long g_ops = 0;
static long g_crash_at = -1;
static long g_fail_at = -1;

void ms_test_set_faults(long crash_at, long fail_at) {
    g_crash_at = crash_at;
    g_fail_at = fail_at;
    g_ops = 0;
}

long ms_test_ops(void) {
    return g_ops;
}

static int fault_point(void) {
    g_ops++;
    if (g_ops == g_crash_at) {
        _exit(99);
    }
    if (g_ops == g_fail_at) {
        errno = EIO;
        return 1;
    }
    return 0;
}

static size_t x_fwrite(const void *data, size_t size, FILE *file) {
    g_ops++;
    if (g_ops == g_crash_at) {
        fwrite(data, 1, size / 2, file);
        fflush(file);
        _exit(99);
    }
    if (g_ops == g_fail_at) {
        size_t partial = fwrite(data, 1, size / 2, file);
        errno = EIO;
        return partial;
    }
    return fwrite(data, 1, size, file);
}
#else
static int fault_point(void) {
    return 0;
}

static size_t x_fwrite(const void *data, size_t size, FILE *file) {
    return fwrite(data, 1, size, file);
}
#endif

static int x_mkdir(const char *path) {
    if (fault_point()) {
        return -1;
    }
    return mkdir(path, 0777);
}

static int x_rename(const char *from, const char *to) {
    if (fault_point()) {
        return -1;
    }
    return rename(from, to);
}

static int x_unlink(const char *path) {
    if (fault_point()) {
        return -1;
    }
    return unlink(path);
}

static int x_rmdir(const char *path) {
    if (fault_point()) {
        return -1;
    }
    return rmdir(path);
}

static FILE *x_fopen_write(const char *path) {
    if (fault_point()) {
        return NULL;
    }
    return fopen(path, "wb");
}

static int path_kind(const char *path) {
    struct stat info;
#ifdef __SWITCH__
    if (stat(path, &info) != 0) {
        return KIND_NONE;
    }
#else
    if (lstat(path, &info) != 0) {
        return KIND_NONE;
    }
#endif
    if (S_ISREG(info.st_mode)) {
        return KIND_FILE;
    }
    if (S_ISDIR(info.st_mode)) {
        return KIND_DIR;
    }
    return KIND_OTHER;
}

static ms_result make_path(char *out, const char *first, const char *second) {
    int written = snprintf(out, PATH_BUF, "%s%s", first, second);
    if (written < 0 || written >= PATH_BUF) {
        return MS_ERR_PATH_TOO_LONG;
    }
    return MS_OK;
}

static ms_result make_child(char *out, const char *dir, const char *name) {
    int written = snprintf(out, PATH_BUF, "%s/%s", dir, name);
    if (written < 0 || written >= PATH_BUF) {
        return MS_ERR_PATH_TOO_LONG;
    }
    return MS_OK;
}

static ms_result make_mod_path(char *out, const char *root, const char *name, const char *suffix) {
    int written = snprintf(out, PATH_BUF, "%s" LIB_DIR "/%s%s", root, name, suffix);
    if (written < 0 || written >= PATH_BUF) {
        return MS_ERR_PATH_TOO_LONG;
    }
    return MS_OK;
}

static int is_junk_name(const char *name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strcmp(name, ".DS_Store") == 0 ||
           strncmp(name, "._", 2) == 0;
}

static int is_ignored(const char *name, tree_scope scope) {
    if (is_junk_name(name)) {
        return 1;
    }
    if (scope == SCOPE_CONTENTS_ROOT) {
        return strcmp(name, METADATA_NAME) == 0 || strcmp(name, CHEATS_NAME) == 0;
    }
    return 0;
}

static ms_result ensure_dirs(const char *root, const char *relative) {
    char path[PATH_BUF];
    ms_result result = make_path(path, root, relative);
    if (result != MS_OK) {
        return result;
    }
    size_t start = strlen(root) + 1;
    size_t length = strlen(path);
    for (size_t i = start; i <= length; i++) {
        if (path[i] != '/' && path[i] != '\0') {
            continue;
        }
        char saved = path[i];
        path[i] = '\0';
        int kind = path_kind(path);
        if (kind == KIND_NONE) {
            if (x_mkdir(path) != 0 && errno != EEXIST) {
                path[i] = saved;
                return MS_ERR_IO;
            }
        } else if (kind != KIND_DIR) {
            path[i] = saved;
            return MS_ERR_IO;
        }
        path[i] = saved;
    }
    return MS_OK;
}

// Deleting is only allowed inside the work folder, so no bug elsewhere in this
// file can remove anything else on the card.
static int inside_work_dir(const char *root, const char *path) {
    char prefix[PATH_BUF];
    if (make_path(prefix, root, WORK_DIR "/") != MS_OK) {
        return 0;
    }
    if (strstr(path, "/../") != NULL) {
        return 0;
    }
    size_t length = strlen(prefix);
    return strncmp(path, prefix, length) == 0 && path[length] != '\0';
}

static ms_result remove_tree_inner(const char *path, int depth) {
    if (depth > MAX_DEPTH) {
        return MS_ERR_PATH_TOO_LONG;
    }
    int kind = path_kind(path);
    if (kind == KIND_NONE) {
        return MS_OK;
    }
    if (kind != KIND_DIR) {
        return x_unlink(path) == 0 ? MS_OK : MS_ERR_IO;
    }
    // The folder is closed before each delete. Not every file system can keep
    // reading a folder reliably while entries are being removed from it.
    for (;;) {
        DIR *dir = opendir(path);
        if (dir == NULL) {
            return MS_ERR_IO;
        }
        char child[PATH_BUF];
        int found = 0;
        ms_result result = MS_OK;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                result = make_child(child, path, entry->d_name);
                found = 1;
                break;
            }
        }
        closedir(dir);
        if (result != MS_OK) {
            return result;
        }
        if (!found) {
            break;
        }
        result = remove_tree_inner(child, depth + 1);
        if (result != MS_OK) {
            return result;
        }
    }
    return x_rmdir(path) == 0 ? MS_OK : MS_ERR_IO;
}

static ms_result remove_work_tree(const char *root, const char *path) {
    if (!inside_work_dir(root, path)) {
        return MS_ERR_GUARD;
    }
    return remove_tree_inner(path, 0);
}

static ms_result copy_file(const char *from, const char *to) {
    static char buffer[COPY_BUF_SIZE];
    FILE *input = fopen(from, "rb");
    if (input == NULL) {
        return MS_ERR_IO;
    }
    FILE *output = x_fopen_write(to);
    if (output == NULL) {
        fclose(input);
        return MS_ERR_IO;
    }
    ms_result result = MS_OK;
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), input);
        if (count == 0) {
            if (ferror(input)) {
                result = MS_ERR_IO;
            }
            break;
        }
        if (x_fwrite(buffer, count, output) != count) {
            result = MS_ERR_IO;
            break;
        }
    }
    fclose(input);
    if (fclose(output) != 0) {
        result = MS_ERR_IO;
    }
    return result;
}

static ms_result copy_tree(const char *from, const char *to, tree_scope scope, int depth) {
    if (depth > MAX_DEPTH) {
        return MS_ERR_PATH_TOO_LONG;
    }
    int kind = path_kind(from);
    if (kind == KIND_FILE) {
        return copy_file(from, to);
    }
    if (kind != KIND_DIR) {
        return MS_ERR_BAD_LAYOUT;
    }
    if (x_mkdir(to) != 0 && errno != EEXIST) {
        return MS_ERR_IO;
    }
    DIR *dir = opendir(from);
    if (dir == NULL) {
        return MS_ERR_IO;
    }
    ms_result result = MS_OK;
    struct dirent *entry;
    while (result == MS_OK && (entry = readdir(dir)) != NULL) {
        if (is_ignored(entry->d_name, scope)) {
            continue;
        }
        char child_from[PATH_BUF];
        char child_to[PATH_BUF];
        result = make_child(child_from, from, entry->d_name);
        if (result == MS_OK) {
            result = make_child(child_to, to, entry->d_name);
        }
        if (result == MS_OK) {
            result = copy_tree(child_from, child_to, SCOPE_PLAIN, depth + 1);
        }
    }
    closedir(dir);
    return result;
}

static int files_equal(const char *first, const char *second) {
    static char buffer_a[COPY_BUF_SIZE];
    static char buffer_b[COPY_BUF_SIZE];
    FILE *a = fopen(first, "rb");
    if (a == NULL) {
        return 0;
    }
    FILE *b = fopen(second, "rb");
    if (b == NULL) {
        fclose(a);
        return 0;
    }
    int equal = 1;
    for (;;) {
        size_t count_a = fread(buffer_a, 1, sizeof(buffer_a), a);
        size_t count_b = fread(buffer_b, 1, sizeof(buffer_b), b);
        if (count_a != count_b || memcmp(buffer_a, buffer_b, count_a) != 0 || ferror(a) || ferror(b)) {
            equal = 0;
            break;
        }
        if (count_a == 0) {
            break;
        }
    }
    fclose(a);
    fclose(b);
    return equal;
}

// Checks that every entry in `first` exists with the same contents in `second`.
static int tree_contains(const char *first, const char *second, tree_scope scope, int depth) {
    if (depth > MAX_DEPTH) {
        return 0;
    }
    DIR *dir = opendir(first);
    if (dir == NULL) {
        return 0;
    }
    int same = 1;
    struct dirent *entry;
    while (same && (entry = readdir(dir)) != NULL) {
        if (is_ignored(entry->d_name, scope)) {
            continue;
        }
        char child_a[PATH_BUF];
        char child_b[PATH_BUF];
        if (make_child(child_a, first, entry->d_name) != MS_OK ||
            make_child(child_b, second, entry->d_name) != MS_OK) {
            same = 0;
            break;
        }
        int kind_a = path_kind(child_a);
        int kind_b = path_kind(child_b);
        if (kind_a != kind_b) {
            same = 0;
        } else if (kind_a == KIND_FILE) {
            same = files_equal(child_a, child_b);
        } else if (kind_a == KIND_DIR) {
            same = tree_contains(child_a, child_b, SCOPE_PLAIN, depth + 1);
        } else {
            same = 0;
        }
    }
    closedir(dir);
    return same;
}

static int trees_equal(const char *first, const char *second, tree_scope scope) {
    return tree_contains(first, second, scope, 0) && tree_contains(second, first, scope, 0);
}

// A directory holding only ignored files counts as empty. A missing path does too.
static int has_real_content(const char *path, tree_scope scope) {
    int kind = path_kind(path);
    if (kind == KIND_NONE) {
        return 0;
    }
    if (kind != KIND_DIR) {
        return 1;
    }
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return 1;
    }
    int found = 0;
    struct dirent *entry;
    while (!found && (entry = readdir(dir)) != NULL) {
        if (!is_ignored(entry->d_name, scope)) {
            found = 1;
        }
    }
    closedir(dir);
    return found;
}

static int valid_name(const char *name) {
    if (name == NULL || name[0] == '\0' || name[0] == '.') {
        return 0;
    }
    if (strlen(name) >= MS_NAME_MAX) {
        return 0;
    }
    return strpbrk(name, "/\\:") == NULL;
}

// Only lists the exact folders the tool knows how to swap, so a mod can't reach
// other games, sysmodules or anything else on the card.
static int only_allowed_child(const char *dir_path, const char *allowed) {
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        return 0;
    }
    int ok = 1;
    struct dirent *entry;
    while (ok && (entry = readdir(dir)) != NULL) {
        if (is_junk_name(entry->d_name)) {
            continue;
        }
        if (strcmp(entry->d_name, allowed) != 0) {
            ok = 0;
        }
    }
    closedir(dir);
    return ok;
}

static int mod_usable(const char *root, const char *name) {
    char path[PATH_BUF];
    char child[PATH_BUF];
    if (!valid_name(name) || make_mod_path(path, root, name, "") != MS_OK) {
        return 0;
    }
    if (path_kind(path) != KIND_DIR) {
        return 0;
    }
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return 0;
    }
    int ok = 1;
    struct dirent *entry;
    while (ok && (entry = readdir(dir)) != NULL) {
        if (is_junk_name(entry->d_name)) {
            continue;
        }
        if (make_child(child, path, entry->d_name) != MS_OK) {
            ok = 0;
        } else if (strcmp(entry->d_name, "atmosphere") != 0 && path_kind(child) != KIND_FILE) {
            ok = 0;
        }
    }
    closedir(dir);
    if (!ok) {
        return 0;
    }

    char atmosphere[PATH_BUF];
    if (make_mod_path(atmosphere, root, name, "/atmosphere") != MS_OK || path_kind(atmosphere) != KIND_DIR) {
        return 0;
    }
    dir = opendir(atmosphere);
    if (dir == NULL) {
        return 0;
    }
    while (ok && (entry = readdir(dir)) != NULL) {
        if (is_junk_name(entry->d_name)) {
            continue;
        }
        if (strcmp(entry->d_name, "contents") != 0 && strcmp(entry->d_name, "exefs_patches") != 0) {
            ok = 0;
        }
    }
    closedir(dir);
    if (!ok) {
        return 0;
    }

    char contents_parent[PATH_BUF];
    char patch_parent[PATH_BUF];
    if (make_mod_path(contents_parent, root, name, "/atmosphere/contents") != MS_OK ||
        make_mod_path(patch_parent, root, name, "/atmosphere/exefs_patches") != MS_OK) {
        return 0;
    }
    int kind = path_kind(contents_parent);
    if (kind != KIND_NONE && (kind != KIND_DIR || !only_allowed_child(contents_parent, "0100000000010000"))) {
        return 0;
    }
    kind = path_kind(patch_parent);
    if (kind != KIND_NONE && (kind != KIND_DIR || !only_allowed_child(patch_parent, "StarlightBase"))) {
        return 0;
    }

    char mod_contents[PATH_BUF];
    char mod_patch[PATH_BUF];
    if (make_mod_path(mod_contents, root, name, MOD_CONTENTS) != MS_OK ||
        make_mod_path(mod_patch, root, name, MOD_PATCH) != MS_OK) {
        return 0;
    }
    int contents_kind = path_kind(mod_contents);
    int patch_kind = path_kind(mod_patch);
    if ((contents_kind != KIND_NONE && contents_kind != KIND_DIR) ||
        (patch_kind != KIND_NONE && patch_kind != KIND_DIR)) {
        return 0;
    }
    return has_real_content(mod_contents, SCOPE_CONTENTS_ROOT) || has_real_content(mod_patch, SCOPE_PLAIN);
}

static int compare_names(const void *a, const void *b) {
    return strcmp(((const ms_mod *)a)->name, ((const ms_mod *)b)->name);
}

ms_result ms_list_mods(const char *root, ms_list *out) {
    memset(out, 0, sizeof(*out));
    char library[PATH_BUF];
    ms_result result = make_path(library, root, LIB_DIR);
    if (result != MS_OK) {
        return result;
    }
    if (path_kind(library) == KIND_NONE) {
        return MS_OK;
    }
    DIR *dir = opendir(library);
    if (dir == NULL) {
        return MS_ERR_IO;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || !valid_name(entry->d_name)) {
            continue;
        }
        char path[PATH_BUF];
        if (make_child(path, library, entry->d_name) != MS_OK || path_kind(path) != KIND_DIR) {
            continue;
        }
        if (out->count == MS_MAX_MODS) {
            out->truncated = 1;
            break;
        }
        snprintf(out->mods[out->count].name, MS_NAME_MAX, "%s", entry->d_name);
        out->mods[out->count].usable = mod_usable(root, entry->d_name);
        out->count++;
    }
    closedir(dir);
    qsort(out->mods, (size_t)out->count, sizeof(out->mods[0]), compare_names);
    return MS_OK;
}

static int mod_matches_active(const char *root, const char *name, int has_contents, int has_patch) {
    char mod_contents[PATH_BUF];
    char mod_patch[PATH_BUF];
    char active_contents[PATH_BUF];
    char active_patch[PATH_BUF];
    if (make_mod_path(mod_contents, root, name, MOD_CONTENTS) != MS_OK ||
        make_mod_path(mod_patch, root, name, MOD_PATCH) != MS_OK ||
        make_path(active_contents, root, ACTIVE_CONTENTS) != MS_OK ||
        make_path(active_patch, root, ACTIVE_PATCH) != MS_OK) {
        return 0;
    }
    int mod_has_contents = has_real_content(mod_contents, SCOPE_CONTENTS_ROOT);
    int mod_has_patch = has_real_content(mod_patch, SCOPE_PLAIN);
    if (mod_has_contents != has_contents || mod_has_patch != has_patch) {
        return 0;
    }
    if (mod_has_contents) {
        if (path_kind(active_contents) != KIND_DIR ||
            !trees_equal(mod_contents, active_contents, SCOPE_CONTENTS_ROOT)) {
            return 0;
        }
    }
    if (mod_has_patch) {
        if (path_kind(active_patch) != KIND_DIR || !trees_equal(mod_patch, active_patch, SCOPE_PLAIN)) {
            return 0;
        }
    }
    return 1;
}

// Asks about one specific mod, so two mods with identical files can't be confused.
static int mod_is_active(const char *root, const char *name) {
    char active_contents[PATH_BUF];
    char active_patch[PATH_BUF];
    if (make_path(active_contents, root, ACTIVE_CONTENTS) != MS_OK ||
        make_path(active_patch, root, ACTIVE_PATCH) != MS_OK) {
        return 0;
    }
    int has_contents = has_real_content(active_contents, SCOPE_CONTENTS_ROOT);
    int has_patch = has_real_content(active_patch, SCOPE_PLAIN);
    return mod_matches_active(root, name, has_contents, has_patch);
}

ms_result ms_detect_active(const char *root, ms_active *out) {
    memset(out, 0, sizeof(*out));
    char active_contents[PATH_BUF];
    char active_patch[PATH_BUF];
    ms_result result = make_path(active_contents, root, ACTIVE_CONTENTS);
    if (result == MS_OK) {
        result = make_path(active_patch, root, ACTIVE_PATCH);
    }
    if (result != MS_OK) {
        return result;
    }
    int has_contents = has_real_content(active_contents, SCOPE_CONTENTS_ROOT);
    int has_patch = has_real_content(active_patch, SCOPE_PLAIN);
    if (!has_contents && !has_patch) {
        out->kind = MS_ACTIVE_NONE;
        return MS_OK;
    }
    static ms_list list;
    result = ms_list_mods(root, &list);
    if (result != MS_OK) {
        return result;
    }
    for (int i = 0; i < list.count; i++) {
        if (list.mods[i].usable && mod_matches_active(root, list.mods[i].name, has_contents, has_patch)) {
            out->kind = MS_ACTIVE_MOD;
            snprintf(out->name, MS_NAME_MAX, "%s", list.mods[i].name);
            return MS_OK;
        }
    }
    out->kind = MS_ACTIVE_UNKNOWN;
    return MS_OK;
}

ms_result ms_cleanup(const char *root) {
    char work[PATH_BUF];
    char keep[PATH_BUF];
    char stage[PATH_BUF];
    char old[PATH_BUF];
    char active_contents[PATH_BUF];
    char active_cheats[PATH_BUF];
    ms_result result = make_path(work, root, WORK_DIR);
    if (result == MS_OK) result = make_path(keep, root, KEEP_CHEATS);
    if (result == MS_OK) result = make_path(stage, root, STAGE_DIR);
    if (result == MS_OK) result = make_path(old, root, OLD_DIR);
    if (result == MS_OK) result = make_path(active_contents, root, ACTIVE_CONTENTS);
    if (result == MS_OK) result = make_child(active_cheats, active_contents, CHEATS_NAME);
    if (result != MS_OK) {
        return result;
    }

    // Cheats parked by an interrupted swap go back first, and are never deleted.
    if (path_kind(keep) != KIND_NONE) {
        result = ensure_dirs(root, ACTIVE_CONTENTS);
        if (result != MS_OK) {
            return result;
        }
        if (path_kind(active_cheats) == KIND_NONE) {
            if (x_rename(keep, active_cheats) != 0) {
                return MS_ERR_IO;
            }
        } else {
            char conflict[PATH_BUF];
            int moved = 0;
            for (int i = 1; i <= MAX_RECOVERED && !moved; i++) {
                int written = snprintf(conflict, PATH_BUF, "%s/cheats-conflict-%d", work, i);
                if (written < 0 || written >= PATH_BUF) {
                    return MS_ERR_PATH_TOO_LONG;
                }
                if (path_kind(conflict) == KIND_NONE) {
                    if (x_rename(keep, conflict) != 0) {
                        return MS_ERR_IO;
                    }
                    moved = 1;
                }
            }
            if (!moved) {
                return MS_ERR_IO;
            }
        }
    }
    result = remove_work_tree(root, stage);
    if (result != MS_OK) {
        return result;
    }
    return remove_work_tree(root, old);
}

static ms_result park_cheats(const char *root) {
    char active_contents[PATH_BUF];
    char active_cheats[PATH_BUF];
    char keep[PATH_BUF];
    ms_result result = make_path(active_contents, root, ACTIVE_CONTENTS);
    if (result == MS_OK) result = make_child(active_cheats, active_contents, CHEATS_NAME);
    if (result == MS_OK) result = make_path(keep, root, KEEP_CHEATS);
    if (result != MS_OK) {
        return result;
    }
    if (path_kind(active_contents) != KIND_DIR || path_kind(active_cheats) == KIND_NONE) {
        return MS_OK;
    }
    return x_rename(active_cheats, keep) == 0 ? MS_OK : MS_ERR_IO;
}

static ms_result restore_cheats(const char *root) {
    char keep[PATH_BUF];
    char active_contents[PATH_BUF];
    char active_cheats[PATH_BUF];
    ms_result result = make_path(keep, root, KEEP_CHEATS);
    if (result == MS_OK) result = make_path(active_contents, root, ACTIVE_CONTENTS);
    if (result == MS_OK) result = make_child(active_cheats, active_contents, CHEATS_NAME);
    if (result != MS_OK) {
        return result;
    }
    if (path_kind(keep) == KIND_NONE) {
        return MS_OK;
    }
    result = ensure_dirs(root, ACTIVE_CONTENTS);
    if (result != MS_OK) {
        return result;
    }
    return x_rename(keep, active_cheats) == 0 ? MS_OK : MS_ERR_IO;
}

static ms_result next_recovered(const char *root, char *name, size_t name_size) {
    for (int i = 1; i <= MAX_RECOVERED; i++) {
        int written = snprintf(name, name_size, "_Recovered-%d", i);
        if (written < 0 || (size_t)written >= name_size) {
            return MS_ERR_PATH_TOO_LONG;
        }
        char path[PATH_BUF];
        ms_result result = make_mod_path(path, root, name, "");
        if (result != MS_OK) {
            return result;
        }
        if (path_kind(path) == KIND_NONE) {
            return MS_OK;
        }
    }
    return MS_ERR_IO;
}

// Moves the active files out of the way, leaving no active mod. Unknown files
// go to a _Recovered folder. Anything else is safe to discard later because the
// library holds the same bytes.
static ms_result clear_active(const char *root, ms_active_kind kind, char *recovered, size_t recovered_size) {
    char active_contents[PATH_BUF];
    char active_patch[PATH_BUF];
    ms_result result = make_path(active_contents, root, ACTIVE_CONTENTS);
    if (result == MS_OK) result = make_path(active_patch, root, ACTIVE_PATCH);
    if (result != MS_OK) {
        return result;
    }
    result = ensure_dirs(root, OLD_DIR);
    if (result != MS_OK) {
        return result;
    }
    result = park_cheats(root);
    if (result != MS_OK) {
        return result;
    }

    int has_contents = has_real_content(active_contents, SCOPE_CONTENTS_ROOT);
    int has_patch = has_real_content(active_patch, SCOPE_PLAIN);
    char destination_contents[PATH_BUF];
    char destination_patch[PATH_BUF];
    result = make_path(destination_contents, root, OLD_CONTENTS);
    if (result == MS_OK) result = make_path(destination_patch, root, OLD_PATCH);
    if (result != MS_OK) {
        return result;
    }

    if (kind == MS_ACTIVE_UNKNOWN && (has_contents || has_patch)) {
        char name[MS_NAME_MAX];
        result = next_recovered(root, name, sizeof(name));
        if (result != MS_OK) {
            return result;
        }
        char relative[PATH_BUF];
        if (has_contents) {
            int written = snprintf(relative, PATH_BUF, LIB_DIR "/%s/atmosphere/contents", name);
            if (written < 0 || written >= PATH_BUF) {
                return MS_ERR_PATH_TOO_LONG;
            }
            result = ensure_dirs(root, relative);
            if (result == MS_OK) result = make_mod_path(destination_contents, root, name, MOD_CONTENTS);
            if (result != MS_OK) {
                return result;
            }
        }
        if (has_patch) {
            int written = snprintf(relative, PATH_BUF, LIB_DIR "/%s/atmosphere/exefs_patches", name);
            if (written < 0 || written >= PATH_BUF) {
                return MS_ERR_PATH_TOO_LONG;
            }
            result = ensure_dirs(root, relative);
            if (result == MS_OK) result = make_mod_path(destination_patch, root, name, MOD_PATCH);
            if (result != MS_OK) {
                return result;
            }
        }
        if (recovered != NULL && recovered_size > 0) {
            snprintf(recovered, recovered_size, "%s", name);
        }
    }

    if (path_kind(active_contents) != KIND_NONE && x_rename(active_contents, destination_contents) != 0) {
        return MS_ERR_IO;
    }
    if (path_kind(active_patch) != KIND_NONE && x_rename(active_patch, destination_patch) != 0) {
        return MS_ERR_IO;
    }
    return MS_OK;
}

ms_result ms_disable(const char *root, char *recovered, size_t recovered_size) {
    if (recovered != NULL && recovered_size > 0) {
        recovered[0] = '\0';
    }
    ms_result result = ms_cleanup(root);
    if (result != MS_OK) {
        return result;
    }
    ms_active active;
    result = ms_detect_active(root, &active);
    if (result != MS_OK) {
        return result;
    }
    result = clear_active(root, active.kind, recovered, recovered_size);
    if (result != MS_OK) {
        return result;
    }
    result = restore_cheats(root);
    if (result != MS_OK) {
        return result;
    }
    result = ms_cleanup(root);
    if (result != MS_OK) {
        return result;
    }
    result = ms_detect_active(root, &active);
    if (result != MS_OK) {
        return result;
    }
    return active.kind == MS_ACTIVE_NONE ? MS_OK : MS_ERR_VERIFY;
}

ms_result ms_enable(const char *root, const char *name, char *recovered, size_t recovered_size) {
    if (recovered != NULL && recovered_size > 0) {
        recovered[0] = '\0';
    }
    if (!valid_name(name)) {
        return MS_ERR_BAD_NAME;
    }
    char mod_dir[PATH_BUF];
    ms_result result = make_mod_path(mod_dir, root, name, "");
    if (result != MS_OK) {
        return result;
    }
    if (path_kind(mod_dir) != KIND_DIR) {
        return MS_ERR_NO_MOD;
    }
    if (!mod_usable(root, name)) {
        return MS_ERR_BAD_LAYOUT;
    }
    result = ms_cleanup(root);
    if (result != MS_OK) {
        return result;
    }
    if (mod_is_active(root, name)) {
        return MS_OK;
    }
    ms_active active;
    result = ms_detect_active(root, &active);
    if (result != MS_OK) {
        return result;
    }

    char mod_contents[PATH_BUF];
    char mod_patch[PATH_BUF];
    char stage_contents[PATH_BUF];
    char stage_patch[PATH_BUF];
    char active_contents[PATH_BUF];
    char active_patch[PATH_BUF];
    result = make_mod_path(mod_contents, root, name, MOD_CONTENTS);
    if (result == MS_OK) result = make_mod_path(mod_patch, root, name, MOD_PATCH);
    if (result == MS_OK) result = make_path(stage_contents, root, STAGE_CONTENTS);
    if (result == MS_OK) result = make_path(stage_patch, root, STAGE_PATCH);
    if (result == MS_OK) result = make_path(active_contents, root, ACTIVE_CONTENTS);
    if (result == MS_OK) result = make_path(active_patch, root, ACTIVE_PATCH);
    if (result != MS_OK) {
        return result;
    }

    // Copy and verify the whole mod before touching anything active, so a full
    // card or a bad read leaves the current mod running.
    result = ensure_dirs(root, STAGE_DIR);
    if (result != MS_OK) {
        return result;
    }
    int mod_has_contents = has_real_content(mod_contents, SCOPE_CONTENTS_ROOT);
    int mod_has_patch = has_real_content(mod_patch, SCOPE_PLAIN);
    if (mod_has_contents) {
        result = copy_tree(mod_contents, stage_contents, SCOPE_CONTENTS_ROOT, 0);
        if (result == MS_OK && !trees_equal(mod_contents, stage_contents, SCOPE_CONTENTS_ROOT)) {
            result = MS_ERR_VERIFY;
        }
    }
    if (result == MS_OK && mod_has_patch) {
        result = copy_tree(mod_patch, stage_patch, SCOPE_PLAIN, 0);
        if (result == MS_OK && !trees_equal(mod_patch, stage_patch, SCOPE_PLAIN)) {
            result = MS_ERR_VERIFY;
        }
    }
    if (result != MS_OK) {
        ms_cleanup(root);
        return result;
    }

    result = clear_active(root, active.kind, recovered, recovered_size);
    if (result == MS_OK && mod_has_contents) {
        result = ensure_dirs(root, "/atmosphere/contents");
        if (result == MS_OK && x_rename(stage_contents, active_contents) != 0) {
            result = MS_ERR_IO;
        }
    }
    if (result == MS_OK && mod_has_patch) {
        result = ensure_dirs(root, "/atmosphere/exefs_patches");
        if (result == MS_OK && x_rename(stage_patch, active_patch) != 0) {
            result = MS_ERR_IO;
        }
    }
    if (result == MS_OK) {
        result = restore_cheats(root);
    }
    if (result != MS_OK) {
        ms_cleanup(root);
        return result;
    }
    result = ms_cleanup(root);
    if (result != MS_OK) {
        return result;
    }
    return mod_is_active(root, name) ? MS_OK : MS_ERR_VERIFY;
}

const char *ms_strerror(ms_result result) {
    switch (result) {
        case MS_OK: return "Done";
        case MS_ERR_BAD_NAME: return "That mod name isn't allowed";
        case MS_ERR_NO_MOD: return "That mod wasn't found";
        case MS_ERR_BAD_LAYOUT: return "That mod's folders aren't in a supported layout";
        case MS_ERR_IO: return "An SD card read or write failed";
        case MS_ERR_VERIFY: return "The files didn't match after copying";
        case MS_ERR_PATH_TOO_LONG: return "A file path is too long";
        case MS_ERR_GUARD: return "Safety check blocked a delete";
    }
    return "Unknown error";
}
