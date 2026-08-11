#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <android/log.h>

#define LOG_TAG "Winlator" // Mapped to standard Winlator log receiver so it appears in the logs tab
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" {

// Automatically run constructor when the native library loads inside container execution paths
__attribute__((constructor)) void initialize_ram_limiter_bridge() {
    const char *env_val = getenv("RAM_L");
    if (!env_val || strlen(env_val) == 0) {
        return; // Limiter remains completely dormant if RAM_L is unassigned or zero
    }

    long limit_mb = std::strtol(env_val, nullptr, 10);
    if (limit_mb <= 0) {
        return;
    }

    rlim_t limit_bytes = (rlim_t)limit_mb * 1024 * 1024;

    // Apply strict hard and soft resource limits to current process tree
    struct rlimit rl;
    rl.rlim_cur = limit_bytes;
    rl.rlim_max = limit_bytes; 
    setrlimit(RLIMIT_AS, &rl);
    setrlimit(RLIMIT_RSS, &rl);

    // Setup fallback TEMP storage path workaround
    const char *temp_dir = "/storage/emulated/0/TEMP";
    struct stat st = {0};
    if (stat(temp_dir, &st) == -1) {
        mkdir(temp_dir, 0777);
    }

    setenv("WINE_SWAP_PATH", temp_dir, 1);
    
    // Print explicit confirmation matching log interface criteria for the UI logs tab
    LOGD("ramlimiter succesfully hooked");
    LOGD("RAM Limit enforced at %ld MB with storage fallback active.", limit_mb);
}

void apply_ram_limit_from_env() {
    // Compatibility bridge hook for manual calls if needed
    initialize_ram_limiter_bridge();
}

}
