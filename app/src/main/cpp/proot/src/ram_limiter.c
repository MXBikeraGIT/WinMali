#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdatomic.h>
#include <malloc.h>

/*
 * ramlimiter.c
 * - Detects Wine/Game process environments.
 * - Tracks process RSS & anonymous mmap allocations.
 * - Forces memory release (malloc_trim / madvise) when limits are hit.
 * - Redirects overflow memory to $TMPDIR as file-backed virtual RAM when RAML_FALLBACK=1.
 */

static long (*real_sysconf)(int name) = NULL;
static int (*real_sysinfo)(struct sysinfo *info) = NULL;
static void* (*real_mmap)(void *addr, size_t length, int prot, int flags, int fd, off_t offset) = NULL;
static int (*real_munmap)(void *addr, size_t length) = NULL;

static atomic_size_t current_allocated_bytes = 0;
static atomic_size_t temp_backed_bytes      = 0;

static size_t fake_ram_bytes     = 0; // FAKERAM_AMOUNT
static size_t max_ram_bytes      = 0; // RAM_LIMIT_MB
static int enable_temp_fallback  = 0; // RAML_FALLBACK (0 or 1)
static int is_target_process     = 0; // Wine/Game flag

// Identifies if current process is running under Wine, Box64, PRoot, or a Game executable
static int detect_wine_or_game_process(void) {
    char cmdline[512] = {0};
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) {
        read(fd, cmdline, sizeof(cmdline) - 1);
        close(fd);
    }

    if (strstr(cmdline, "wine") || strstr(cmdline, ".exe") || 
        strstr(cmdline, "box64") || strstr(cmdline, "proot") ||
        getenv("WINEPREFIX") != NULL) {
        return 1;
    }
    return 0;
}

// Queries current Resident Set Size (RSS) directly from kernel status
static size_t get_current_rss_bytes(void) {
    long rss_pages = 0;
    FILE *fp = fopen("/proc/self/statm", "r");
    if (fp) {
        if (fscanf(fp, "%*s %ld", &rss_pages) != 1) {
            rss_pages = 0;
        }
        fclose(fp);
    }
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    return (size_t)rss_pages * (size_t)page_size;
}

// Forcefully flushes unmapped heap memory back to the system kernel
static void force_release_ram(void) {
#if defined(__GLIBC__) || defined(__BIONIC__)
    malloc_trim(0);
#endif
}

// Creates file-backed memory in temp folder disguised as physical RAM
static void* allocate_temp_backed_memory(size_t length, int prot) {
    const char *tmp_dir = getenv("TMPDIR");
    if (!tmp_dir || tmp_dir[0] == '\0') {
        tmp_dir = "/tmp";
    }

    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s/fakeram_swap_XXXXXX", tmp_dir);

    int fd = mkstemp(temp_path);
    if (fd < 0) return MAP_FAILED;

    // Immediate unlink guarantees auto-cleanup on close or crash
    unlink(temp_path);

    if (ftruncate(fd, length) != 0) {
        close(fd);
        return MAP_FAILED;
    }

    // MAP_SHARED maps disk sectors into virtual memory address space transparently
    void *ptr = real_mmap(NULL, length, prot, MAP_SHARED, fd, 0);
    close(fd);

    if (ptr != MAP_FAILED) {
        // Advise kernel that this region acts as standard active memory
        madvise(ptr, length, MADV_WILLNEED);
        atomic_fetch_add(&temp_backed_bytes, length);
    }

    return ptr;
}

__attribute__((constructor))
static void init_ram_limiter(void) {
    real_sysconf = dlsym(RTLD_NEXT, "sysconf");
    real_sysinfo = dlsym(RTLD_NEXT, "sysinfo");
    real_mmap    = dlsym(RTLD_NEXT, "mmap");
    real_munmap  = dlsym(RTLD_NEXT, "munmap");

    is_target_process = detect_wine_or_game_process();

    // 1. RAM_LIMIT_MB (Hardware threshold before trim / fallback)
    const char *limit_env = getenv("RAM_LIMIT_MB");
    if (limit_env) {
        size_t parsed = strtoul(limit_env, NULL, 10);
        if (parsed > 0) max_ram_bytes = parsed * 1024ULL * 1024ULL;
    }
    if (max_ram_bytes == 0) {
        max_ram_bytes = 2048ULL * 1024ULL * 1024ULL; // 2GB Default
    }

    // 2. FAKERAM_AMOUNT (Reported virtual RAM to sysinfo/sysconf)
    const char *fakeram_env = getenv("FAKERAM_AMOUNT");
    if (fakeram_env) {
        size_t parsed = strtoul(fakeram_env, NULL, 10);
        if (parsed > 0) fake_ram_bytes = parsed * 1024ULL * 1024ULL;
    }
    if (fake_ram_bytes == 0) {
        fake_ram_bytes = max_ram_bytes;
    }

    // 3. RAML_FALLBACK (0 = Reject on breach, 1 = Convert temp folder into RAM)
    const char *fallback_env = getenv("RAML_FALLBACK");
    if (fallback_env) {
        enable_temp_fallback = (atoi(fallback_env) != 0) ? 1 : 0;
    }

    if (!enable_temp_fallback && is_target_process) {
        struct rlimit rl;
        rl.rlim_cur = max_ram_bytes;
        rl.rlim_max = max_ram_bytes;
        setrlimit(RLIMIT_AS, &rl);
    }
}

// Intercept sysconf to feed FAKERAM_AMOUNT to Wine / Game queries
long sysconf(int name) {
    if (!real_sysconf) real_sysconf = dlsym(RTLD_NEXT, "sysconf");

    long page_size = real_sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;

    if (name == _SC_PHYS_PAGES) {
        return (long)(fake_ram_bytes / page_size);
    }
    
    if (name == _SC_AVPHYS_PAGES) {
        size_t allocated = atomic_load(&current_allocated_bytes);
        size_t free_bytes = (allocated < fake_ram_bytes) ? (fake_ram_bytes - allocated) : 0;
        return (long)(free_bytes / page_size);
    }

    return real_sysconf(name);
}

// Intercept sysinfo to feed FAKERAM_AMOUNT to GlobalMemoryStatus / Wine
int sysinfo(struct sysinfo *info) {
    if (!real_sysinfo) real_sysinfo = dlsym(RTLD_NEXT, "sysinfo");

    int result = real_sysinfo(info);
    if (result == 0 && info != NULL) {
        size_t mem_unit = info->mem_unit ? info->mem_unit : 1;
        size_t clamped_total = fake_ram_bytes / mem_unit;
        
        size_t allocated = atomic_load(&current_allocated_bytes);
        size_t free_bytes = (allocated < fake_ram_bytes) ? (fake_ram_bytes - allocated) : 0;

        info->totalram = clamped_total;
        info->freeram  = free_bytes / mem_unit;
    }
    return result;
}

// Intercept mmap allocations
void* mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap");

    if (flags & MAP_ANONYMOUS) {
        size_t current_tracked = atomic_load(&current_allocated_bytes);
        size_t actual_rss = get_current_rss_bytes();
        size_t effective_usage = (actual_rss > current_tracked) ? actual_rss : current_tracked;

        // Check if allocation breaches RAM_LIMIT_MB
        if (effective_usage + length > max_ram_bytes) {
            
            // Stage 1: Force immediate RAM release
            force_release_ram();

            // Re-check after memory trim
            actual_rss = get_current_rss_bytes();
            current_tracked = atomic_load(&current_allocated_bytes);
            effective_usage = (actual_rss > current_tracked) ? actual_rss : current_tracked;

            if (effective_usage + length > max_ram_bytes) {
                if (enable_temp_fallback) {
                    // Stage 2: Divert excess allocation to Temp Folder as fake RAM
                    void *temp_ptr = allocate_temp_backed_memory(length, prot);
                    if (temp_ptr != MAP_FAILED) {
                        atomic_fetch_add(&current_allocated_bytes, length);
                        return temp_ptr;
                    }
                }

                // Stage 3: Reject allocation if fallback is disabled or failed
                errno = ENOMEM;
                return MAP_FAILED;
            }
        }
        atomic_fetch_add(&current_allocated_bytes, length);
    }

    void *ptr = real_mmap(addr, length, prot, flags, fd, offset);
    if (ptr == MAP_FAILED && (flags & MAP_ANONYMOUS)) {
        atomic_fetch_sub(&current_allocated_bytes, length);
    }

    return ptr;
}

// Intercept munmap to update live tracking
int munmap(void *addr, size_t length) {
    if (!real_munmap) real_munmap = dlsym(RTLD_NEXT, "munmap");

    int res = real_munmap(addr, length);
    if (res == 0) {
        size_t current = atomic_load(&current_allocated_bytes);
        if (current >= length) {
            atomic_fetch_sub(&current_allocated_bytes, length);
        } else {
            atomic_store(&current_allocated_bytes, 0);
        }
    }
    return res;
}
