#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/resource.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>

/*
 * ramlimiter.c
 * LD_PRELOAD hook to spoof available system RAM and enforce memory allocation limits.
 */

// Function pointers to real libc symbols
static long (*real_sysconf)(int name) = NULL;
static int (*real_sysinfo)(struct sysinfo *info) = NULL;
static void* (*real_mmap)(void *addr, size_t length, int prot, int flags, int fd, off_t offset) = NULL;
static int (*real_munmap)(void *addr, size_t length) = NULL;

static atomic_size_t current_allocated_bytes = 0;
static size_t max_ram_bytes = 0;

__attribute__((constructor))
static void init_ram_limiter(void) {
    // Resolve original function symbols
    real_sysconf = dlsym(RTLD_NEXT, "sysconf");
    real_sysinfo = dlsym(RTLD_NEXT, "sysinfo");
    real_mmap    = dlsym(RTLD_NEXT, "mmap");
    real_munmap  = dlsym(RTLD_NEXT, "munmap");

    // Read custom memory cap from environment (in Megabytes)
    const char *limit_env = getenv("RAM_LIMIT_MB");
    if (limit_env != NULL) {
        size_t limit_mb = strtoul(limit_env, NULL, 10);
        if (limit_mb > 0) {
            max_ram_bytes = limit_mb * 1024ULL * 1024ULL;
        }
    }

    // Default fallback: 2048 MB (safest limit for legacy 32-bit apps and Wine memory maps)
    if (max_ram_bytes == 0) {
        max_ram_bytes = 2048ULL * 1024ULL * 1024ULL;
    }

    // Set process hard virtual memory boundary (RLIMIT_AS) as a kernel-level backstop
    struct rlimit rl;
    rl.rlim_cur = max_ram_bytes;
    rl.rlim_max = max_ram_bytes;
    setrlimit(RLIMIT_AS, &rl);
}

// Intercept sysconf() to report restricted physical page counts
long sysconf(int name) {
    if (!real_sysconf) real_sysconf = dlsym(RTLD_NEXT, "sysconf");

    long page_size = real_sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;

    if (name == _SC_PHYS_PAGES) {
        return (long)(max_ram_bytes / page_size);
    }
    
    if (name == _SC_AVPHYS_PAGES) {
        size_t allocated = atomic_load(&current_allocated_bytes);
        size_t free_bytes = (allocated < max_ram_bytes) ? (max_ram_bytes - allocated) : 0;
        return (long)(free_bytes / page_size);
    }

    return real_sysconf(name);
}

// Intercept sysinfo() to spoof global system RAM numbers
int sysinfo(struct sysinfo *info) {
    if (!real_sysinfo) real_sysinfo = dlsym(RTLD_NEXT, "sysinfo");

    int result = real_sysinfo(info);
    if (result == 0 && info != NULL) {
        size_t mem_unit = info->mem_unit ? info->mem_unit : 1;
        size_t clamped_total = max_ram_bytes / mem_unit;
        
        size_t allocated = atomic_load(&current_allocated_bytes);
        size_t free_bytes = (allocated < max_ram_bytes) ? (max_ram_bytes - allocated) : 0;

        info->totalram = clamped_total;
        info->freeram  = free_bytes / mem_unit;
    }
    return result;
}

// Intercept mmap() anonymous memory requests to restrict process expansion
void* mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap");

    if (flags & MAP_ANONYMOUS) {
        size_t allocated = atomic_load(&current_allocated_bytes);
        if (allocated + length > max_ram_bytes) {
            errno = ENOMEM;
            return MAP_FAILED;
        }
        atomic_fetch_add(&current_allocated_bytes, length);
    }

    void *ptr = real_mmap(addr, length, prot, flags, fd, offset);
    
    if (ptr == MAP_FAILED && (flags & MAP_ANONYMOUS)) {
        atomic_fetch_sub(&current_allocated_bytes, length);
    }

    return ptr;
}

// Intercept munmap() to decrement allocated RAM counter
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
