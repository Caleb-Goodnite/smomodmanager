// Runs the core logic on a Mac so the tests can exercise it against a fake card.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/modswitch.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: cli ROOT list|detect|cleanup|disable|enable NAME\n");
        return 2;
    }
    const char *root = argv[1];
    const char *command = argv[2];
    const char *crash = getenv("MS_CRASH_AT");
    const char *fail = getenv("MS_FAIL_AT");
    ms_test_set_faults(crash ? atol(crash) : -1, fail ? atol(fail) : -1);

    ms_result result = MS_OK;
    char recovered[MS_NAME_MAX] = "";
    if (strcmp(command, "list") == 0) {
        static ms_list list;
        result = ms_list_mods(root, &list);
        for (int i = 0; i < list.count; i++) {
            printf("MOD\t%d\t%s\n", list.mods[i].usable, list.mods[i].name);
        }
        printf("TRUNCATED\t%d\n", list.truncated);
    } else if (strcmp(command, "detect") == 0) {
        ms_active active;
        result = ms_detect_active(root, &active);
        const char *kinds[] = {"NONE", "MOD", "UNKNOWN"};
        printf("ACTIVE\t%s\t%s\n", kinds[active.kind], active.name);
    } else if (strcmp(command, "cleanup") == 0) {
        result = ms_cleanup(root);
    } else if (strcmp(command, "disable") == 0) {
        result = ms_disable(root, recovered, sizeof(recovered));
    } else if (strcmp(command, "enable") == 0 && argc >= 4) {
        result = ms_enable(root, argv[3], recovered, sizeof(recovered));
    } else {
        return 2;
    }
    printf("RECOVERED\t%s\n", recovered);
    printf("OPS\t%ld\n", ms_test_ops());
    printf("RESULT\t%d\t%s\n", (int)result, ms_strerror(result));
    return result == MS_OK ? 0 : 1;
}
