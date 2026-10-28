/* Compile the real stdio implementation with its own FILE definition. */
#define AX_CONFIG_FS
#define malloc injected_malloc
#define free injected_free
#define open injected_open
#define fopen arceos_fopen
#ifndef AXLIBC_STDIO_SOURCE
#define AXLIBC_STDIO_SOURCE "../../c/stdio.c"
#endif
#include AXLIBC_STDIO_SOURCE
#undef fopen
#undef open
#undef malloc
#undef free

static unsigned int malloc_calls;
static unsigned int open_calls;
static unsigned int free_calls;
static FILE allocated_file;
static int allow_allocation;
static int allocation_live;

void *injected_malloc(size_t size)
{
    ++malloc_calls;
    if (size != sizeof(FILE))
        __builtin_trap();
    if (allow_allocation && !allocation_live) {
        allocation_live = 1;
        return &allocated_file;
    }
    errno = ENOMEM;
    return NULL;
}

void injected_free(void *ptr)
{
    if (ptr != &allocated_file || !allocation_live)
        __builtin_trap();
    allocation_live = 0;
    ++free_calls;
}

int injected_open(const char *filename, int flags, ...)
{
    (void)filename;
    (void)flags;
    ++open_calls;
    errno = ENOENT;
    return -1;
}

__attribute__((visibility("default"))) int main(void)
{
    errno = 0;
    FILE *file = arceos_fopen("unused", "w");
    if (file != NULL || errno != ENOMEM || malloc_calls != 1 || open_calls != 0 ||
        free_calls != 0)
        return 1;

    /* Invalid modes must still fail before allocating or opening a file. */
    errno = 0;
    file = arceos_fopen("unused", "invalid");
    if (file != NULL || errno != EINVAL || malloc_calls != 1 || open_calls != 0 ||
        free_calls != 0)
        return 1;

    /* Every failed open must release its FILE and preserve the open error. */
    allow_allocation = 1;
    for (unsigned int i = 0; i < 100; ++i) {
        errno = 0;
        file = arceos_fopen("missing-file", "r");
        if (file != NULL || errno != ENOENT || allocation_live || malloc_calls != i + 2 ||
            open_calls != i + 1 || free_calls != i + 1)
            return 1;
    }
    return 0;
}
