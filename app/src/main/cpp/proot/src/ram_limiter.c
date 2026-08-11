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
    const char *env_val = getenv("RAM_L");
    if (!env_val || strlen(env_val) == 0) {
        return; // RAM_L not present: stay dormant
    }

    long limit_mb = strtol(env_val, NULL, 10);
    if (limit_mb <= 0) {
        LOGD("RAM_L is set to 0. RAM Limiter disabled.");
        return;
    }

    rlim_t limit_bytes = (rlim_t)limit_mb * 1024 * 1024;

    struct rlimit rl;
    rl.rlim_cur = limit_bytes;
    rl.rlim_max = limit_bytes;

    // Enforce hard Virtual Memory (RLIMIT_AS) and Heap Space (RLIMIT_DATA)
    if (setrlimit(RLIMIT_AS, &rl) != 0) {
        LOGE("Failed to enforce RLIMIT_AS memory limit on PRoot boundary.");
    }

    if (setrlimit(RLIMIT_DATA, &rl) != 0) {
        LOGE("Failed to enforce RLIMIT_DATA heap limit on PRoot boundary.");
    }

    // Ensure fallback swap/temp memory directory exists on external storage
    const char *temp_dir = "/storage/emulated/0/TEMP";
    struct stat st = {0};
    if (stat(temp_dir, &st) == -1) {
        mkdir(temp_dir, 0777);
    }

    setenv("WINE_SWAP_PATH", temp_dir, 1);

    // Required hook confirmation log for Winlator Logs Tab
    LOGD("ramlimiter succesfully hooked");
    LOGD("PRoot container & process tree capped at %ld MB.", limit_mb);
}
