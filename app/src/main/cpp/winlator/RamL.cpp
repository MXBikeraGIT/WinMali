#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <android/log.h>

#define LOG_TAG "WinMaliRAMLimiter"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" {

void apply_ram_limit_from_env() {
    const char *env_val = getenv("RAM_L");
    if (!env_val || strlen(env_val) == 0) {
        LOGD("RAM_L environment variable not set. Limiter inactive.");
        return;
    }

    long limit_mb = std::strtol(env_val, nullptr, 10);
    if (limit_mb <= 0) {
        LOGD("RAM_L value is <= 0 (%ld). No memory restriction applied.", limit_mb);
        return;
    }

    rlim_t limit_bytes = (rlim_t)limit_mb * 1024 * 1024;
    LOGD("RAM_L active: Enforcing strict hard memory ceiling of %ld MB.", limit_mb);

    struct rlimit rl;
    rl.rlim_cur = limit_bytes;
    rl.rlim_max = limit_bytes; // Hard limit enforced instantly to trigger immediate release/OOM constraints

    if (setrlimit(RLIMIT_AS, &rl) != 0) {
        LOGE("Failed to set RLIMIT_AS resource limit.");
    } else {
        LOGD("Strict hard memory limit of %ld MB applied successfully.", limit_mb);
    }

    // Ensure fallback swap/temp memory directory exists on external storage for spillover handling
    const char *temp_dir = "/storage/emulated/0/TEMP";
    struct stat st = {0};
    if (stat(temp_dir, &st) == -1) {
        if (mkdir(temp_dir, 0777) == 0) {
            LOGD("Created fallback swap/spillover directory at %s", temp_dir);
        } else {
            LOGE("Failed to create fallback directory at %s. Check storage permissions.", temp_dir);
        }
    } else {
        LOGD("Fallback swap directory verified at %s", temp_dir);
    }

    // Configure environment hint for Wine/memory mapping layer to recognize storage fallback space
    setenv("WINEDEBUG", "-all", 0);
    setenv("WINE_SWAP_PATH", temp_dir, 1);
    LOGD("Configured runtime spillover path pointing to external TEMP storage workaround.");
}

}
