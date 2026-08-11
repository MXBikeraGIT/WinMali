#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <android/log.h>

#define LOG_TAG "Winlator"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

void enforce_proot_ram_limit(void) {
    // --- 1. Read RAM Limit ---
    const char *ram_l_env = getenv("RAM_L");
    if (ram_l_env && strlen(ram_l_env) > 0) {
        long limit_mb = strtol(ram_l_env, NULL, 10);
        if (limit_mb > 0) {
            rlim_t limit_bytes = (rlim_t)limit_mb * 1024 * 1024;
            struct rlimit rl;
            rl.rlim_cur = limit_bytes;
            rl.rlim_max = limit_bytes;

            // Enforce hard kernel limits on PRoot boundary
            if (setrlimit(RLIMIT_AS, &rl) != 0) {
                LOGE("Failed to enforce hard RLIMIT_AS memory ceiling.");
            }
            if (setrlimit(RLIMIT_DATA, &rl) != 0) {
                LOGE("Failed to enforce hard RLIMIT_DATA memory ceiling.");
            }

            LOGD("ramlimiter succesfully hooked");
            LOGD("Hard RAM ceiling locked at %ld MB.", limit_mb);
        }
    }

    // --- 2. Read Fallback Swap Option (RAML_FALLBACK) ---
    const char *fallback_env = getenv("RAML_FALLBACK");
    int allow_fallback = (fallback_env != NULL && strcmp(fallback_env, "1") == 0);

    if (allow_fallback) {
        const char *temp_dir = "/storage/emulated/0/TEMP";
        struct stat st = {0};
        if (stat(temp_dir, &st) == -1) {
            mkdir(temp_dir, 0777);
        }
        setenv("WINE_SWAP_PATH", temp_dir, 1);
        LOGD("using fallback option %s", temp_dir);
    } else {
        unsetenv("WINE_SWAP_PATH");
        LOGD("RAML_FALLBACK disabled (0). Running strict zero-swap hard limit mode.");
    }

    // --- 3. Read Fake RAM Amount (FAKERAM_AMOUNT) ---
    const char *fakeram_env = getenv("FAKERAM_AMOUNT");
    if (fakeram_env && strlen(fakeram_env) > 0) {
        long fake_mb = strtol(fakeram_env, NULL, 10);
        if (fake_mb > 0) {
            // Keep env set for PRoot /proc/meminfo and sysconf interception layers
            LOGD("FAKERAM_AMOUNT set: Reporting %ld MB system RAM to guest applications.", fake_mb);
        }
    }
}
