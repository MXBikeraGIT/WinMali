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

/**
 * Generate a synthetic /proc/meminfo file so Wine, Box64, and guest
 * applications see FAKERAM_AMOUNT instead of physical hardware RAM.
 */
static void generate_fake_meminfo(long fake_ram_mb) {
    if (fake_ram_mb <= 0) {
        return;
    }

    unsigned long long fake_ram_kb = (unsigned long long)fake_ram_mb * 1024;
    unsigned long long fake_free_kb = (fake_ram_kb * 75) / 100;    // 75% free
    unsigned long long fake_avail_kb = (fake_ram_kb * 80) / 100;   // 80% available

    const char *temp_dir = "/storage/emulated/0/TEMP";
    struct stat st = {0};
    if (stat(temp_dir, &st) == -1) {
        mkdir(temp_dir, 0777);
    }

    char meminfo_path[256];
    snprintf(meminfo_path, sizeof(meminfo_path), "%s/fake_meminfo", temp_dir);

    FILE *f = fopen(meminfo_path, "w");
    if (f != NULL) {
        fprintf(f,
            "MemTotal:       %llu kB\n"
            "MemFree:        %llu kB\n"
            "MemAvailable:   %llu kB\n"
            "Buffers:          102400 kB\n"
            "Cached:           204800 kB\n"
            "SwapTotal:             0 kB\n"
            "SwapFree:              0 kB\n"
            "Dirty:                 0 kB\n"
            "Writeback:             0 kB\n"
            "AnonPages:         51200 kB\n"
            "Mapped:            20480 kB\n"
            "Shmem:             10240 kB\n"
            "Slab:              30720 kB\n"
            "SReclaimable:      20480 kB\n"
            "SUnreclaim:        10240 kB\n",
            fake_ram_kb, fake_free_kb, fake_avail_kb);
        fclose(f);

        // Export path so container startup scripts can bind it over /proc/meminfo
        setenv("PROOT_FAKE_MEMINFO_PATH", meminfo_path, 1);

        // Export memory limits directly to translation layers
        char fake_str[32];
        snprintf(fake_str, sizeof(fake_str), "%ld", fake_ram_mb);
        setenv("BOX64_TOTAL_RAM", fake_str, 1);

        LOGD("FAKERAM_AMOUNT set: Reporting %ld MB RAM to guest environment.", fake_ram_mb);
        LOGD("Synthetic meminfo written to %s", meminfo_path);
    } else {
        LOGE("Failed to write synthetic meminfo file at %s", meminfo_path);
    }
}

void enforce_proot_ram_limit(void) {
    // --- 1. Enforce Hard Kernel RAM Limit (RAM_L) ---
    const char *ram_l_env = getenv("RAM_L");
    if (ram_l_env && strlen(ram_l_env) > 0) {
        long limit_mb = strtol(ram_l_env, NULL, 10);
        if (limit_mb > 0) {
            rlim_t limit_bytes = (rlim_t)limit_mb * 1024 * 1024;

            struct rlimit rl;
            rl.rlim_cur = limit_bytes;
            rl.rlim_max = limit_bytes;

            if (setrlimit(RLIMIT_AS, &rl) != 0) {
                LOGE("Failed to enforce RLIMIT_AS memory limit on PRoot boundary.");
            }

            if (setrlimit(RLIMIT_DATA, &rl) != 0) {
                LOGE("Failed to enforce RLIMIT_DATA heap limit on PRoot boundary.");
            }

            LOGD("ramlimiter succesfully hooked");
            LOGD("PRoot process tree capped at hard limit of %ld MB.", limit_mb);
        }
    }

    // --- 2. Fallback Swap Control (RAML_FALLBACK) ---
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
        LOGD("RAML_FALLBACK disabled (0). Strict zero-swap hard limit active.");
    }

    // --- 3. Fake RAM Reporting (FAKERAM_AMOUNT) ---
    const char *fakeram_env = getenv("FAKERAM_AMOUNT");
    if (fakeram_env && strlen(fakeram_env) > 0) {
        long fake_mb = strtol(fakeram_env, NULL, 10);
        if (fake_mb > 0) {
            generate_fake_meminfo(fake_mb);
        }
    }
}
