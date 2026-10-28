/* Compile the real stdio implementation with its own FILE definition. */
#define AX_CONFIG_FS
#define malloc injected_malloc
#define free injected_free
#define open injected_open
#define close injected_close
#define write injected_write
#define fopen arceos_fopen
#define fclose arceos_fclose
#define fflush arceos_fflush
#define fputc arceos_fputc
#ifndef AXLIBC_STDIO_SOURCE
#define AXLIBC_STDIO_SOURCE "../../c/stdio.c"
#endif
#include AXLIBC_STDIO_SOURCE
#undef fopen
#undef fclose
#undef fflush
#undef fputc
#undef open
#undef close
#undef write
#undef malloc
#undef free

#define CHECK(condition)      \
    do {                      \
        if (!(condition))     \
            __builtin_trap(); \
    } while (0)

static unsigned int malloc_calls, open_calls, free_calls, close_calls, write_calls;
static FILE allocated_file;
static int allow_allocation, allocation_live, fd_open;
static int open_error, close_error, free_error;
static ssize_t write_results[8];
static int write_errors[8];
static unsigned int write_steps;
static char written_bytes[4096];
static size_t written_count;

void *injected_malloc(size_t size)
{
    ++malloc_calls;
    CHECK(size == sizeof(FILE));
    if (allow_allocation && !allocation_live) {
        allocation_live = 1;
        /* malloc does not promise the zeroed FILE used by the old fixture. */
        memset(&allocated_file, 0xa5, sizeof(allocated_file));
        return &allocated_file;
    }
    errno = ENOMEM;
    return NULL;
}

void injected_free(void *ptr)
{
    CHECK(ptr == &allocated_file && allocation_live && !fd_open);
    allocation_live = 0;
    ++free_calls;
    if (free_error)
        errno = free_error;
}

int injected_open(const char *filename, int flags, ...)
{
    (void)filename;
    (void)flags;
    ++open_calls;
    if (open_error) {
        errno = open_error;
        return -1;
    }
    CHECK(!fd_open);
    fd_open = 1;
    return 7;
}

int injected_close(int fd)
{
    CHECK(fd == 7 || (fd >= 0 && fd <= 2));
    ++close_calls;
    fd_open = 0;
    if (close_error) {
        errno = close_error;
        return -1;
    }
    return 0;
}

ssize_t injected_write(int fd, const void *ptr, size_t size)
{
    CHECK((fd == 7 && fd_open) || (fd >= 0 && fd <= 2));
    ssize_t result = size;
    if (write_calls < write_steps) {
        result = write_results[write_calls];
        if (result < 0)
            errno = write_errors[write_calls];
    }
    ++write_calls;
    if (result > 0) {
        CHECK((size_t)result <= size);
        CHECK(written_count + result <= sizeof(written_bytes));
        memcpy(written_bytes + written_count, ptr, result);
        written_count += result;
    }
    return result;
}

static void reset_state(void)
{
    CHECK(!allocation_live && !fd_open);
    malloc_calls = open_calls = free_calls = close_calls = write_calls = 0;
    allow_allocation = 1;
    open_error = close_error = free_error = 0;
    write_steps = 0;
    written_count = 0;
    __stdin_FILE.buffer_len = __stdout_FILE.buffer_len = __stderr_FILE.buffer_len = 0;
    errno = 0;
}

static FILE *open_file(void)
{
    FILE *file = arceos_fopen("exists", "w");
    CHECK(file == &allocated_file && file->fd == 7 && file->buffer_len == 0);
    return file;
}

static void set_buffer(FILE *file, const char *text)
{
    file->buffer_len = strlen(text);
    memcpy(file->buf, text, file->buffer_len);
}

static void test_open_failures_and_recovery(void)
{
    reset_state();
    allow_allocation = 0;
    CHECK(arceos_fopen("unused", "w") == NULL);
    CHECK(errno == ENOMEM && malloc_calls == 1 && open_calls == 0 && free_calls == 0);

    const char *invalid_modes[] = { "invalid", "" };
    for (size_t i = 0; i < sizeof(invalid_modes) / sizeof(invalid_modes[0]); ++i) {
        errno = 0;
        CHECK(arceos_fopen("unused", invalid_modes[i]) == NULL);
        CHECK(errno == EINVAL && malloc_calls == 1 && open_calls == 0 && free_calls == 0);
    }

    allow_allocation = 1;
    open_error = ENOENT;
    free_error = EBUSY;
    for (unsigned int i = 0; i < 100; ++i) {
        errno = 0;
        CHECK(arceos_fopen("missing-file", "r") == NULL);
        CHECK(errno == ENOENT && !allocation_live && malloc_calls == i + 2 &&
              open_calls == i + 1 && free_calls == i + 1 && close_calls == 0);
    }
    open_error = free_error = 0;
    FILE *file = open_file();
    CHECK(arceos_fputc('Q', file) != EOF && file->buffer_len == 1);
    CHECK(arceos_fclose(file) == 0);
    CHECK(!allocation_live && !fd_open && free_calls == 101 && close_calls == 1 &&
          written_count == 1 && written_bytes[0] == 'Q');
}

static void test_success_and_short_writes(void)
{
    reset_state();
    for (unsigned int i = 0; i < 100; ++i) {
        FILE *file = open_file();
        CHECK(arceos_fclose(file) == 0);
        CHECK(!allocation_live && free_calls == i + 1 && close_calls == i + 1);
    }
    CHECK(write_calls == 0);

    reset_state();
    FILE *file = open_file();
    set_buffer(file, "ABCD");
    write_steps = 4;
    write_results[0] = 1;
    write_results[1] = -1;
    write_errors[1] = EINTR;
    write_results[2] = 1;
    write_results[3] = 2;
    CHECK(arceos_fclose(file) == 0);
    CHECK(!allocation_live && !fd_open && free_calls == 1 && close_calls == 1 &&
          write_calls == 4 && written_count == 4 && !memcmp(written_bytes, "ABCD", 4));
}

static void test_close_errors(void)
{
    /* fclose must close/free even if flushing fails, keeping the first error. */
    for (unsigned int both_fail = 0; both_fail < 2; ++both_fail) {
        reset_state();
        FILE *file = open_file();
        set_buffer(file, "ABC");
        write_steps = 1;
        write_results[0] = -1;
        write_errors[0] = EIO;
        close_error = both_fail ? EBADF : 0;
        free_error = ENOMEM;
        CHECK(arceos_fclose(file) == EOF && errno == EIO);
        CHECK(!allocation_live && !fd_open && free_calls == 1 && close_calls == 1 &&
              write_calls == 1);
    }

    reset_state();
    FILE *file = open_file();
    set_buffer(file, "ABC");
    close_error = EBADF;
    free_error = ENOMEM;
    CHECK(arceos_fclose(file) == EOF && errno == EBADF);
    CHECK(!allocation_live && free_calls == 1 && close_calls == 1 &&
          written_count == 3 && !memcmp(written_bytes, "ABC", 3));

    reset_state();
    file = open_file();
    set_buffer(file, "ABC");
    write_steps = 1;
    write_results[0] = 0;
    CHECK(arceos_fclose(file) == EOF && errno == EIO);
    CHECK(!allocation_live && free_calls == 1 && close_calls == 1 && write_calls == 1);
}

static void test_flush_recovery(void)
{
    reset_state();
    FILE *file = open_file();
    set_buffer(file, "ABCD");
    write_steps = 2;
    write_results[0] = 2;
    write_results[1] = -1;
    write_errors[1] = EAGAIN;
    CHECK(arceos_fflush(file) == EOF && errno == EAGAIN);
    CHECK(file->buffer_len == 2 && !memcmp(file->buf, "CD", 2));
    CHECK(written_count == 2 && !memcmp(written_bytes, "AB", 2));
    write_steps = 0;
    CHECK(arceos_fflush(file) == 0 && file->buffer_len == 0);
    CHECK(written_count == 4 && !memcmp(written_bytes, "ABCD", 4));
    CHECK(arceos_fclose(file) == 0);

    /* A retained full buffer must be retried before appending another byte. */
    reset_state();
    file = open_file();
    memset(file->buf, 'A', FILE_BUF_SIZE - 1);
    file->buffer_len = FILE_BUF_SIZE - 1;
    write_steps = 2;
    write_results[0] = write_results[1] = -1;
    write_errors[0] = write_errors[1] = EAGAIN;
    CHECK(arceos_fputc('B', file) == EOF && file->buffer_len == FILE_BUF_SIZE);
    CHECK(arceos_fputc('C', file) == EOF && file->buffer_len == FILE_BUF_SIZE);
    write_steps = 0;
    CHECK(arceos_fputc('C', file) != EOF && file->buffer_len == 1);
    CHECK(arceos_fclose(file) == 0);
    CHECK(written_count == FILE_BUF_SIZE + 1 && written_bytes[FILE_BUF_SIZE - 1] == 'B' &&
          written_bytes[FILE_BUF_SIZE] == 'C');
}

static void test_static_streams(void)
{
    reset_state();
    CHECK(arceos_fclose(stdin) == 0);
    set_buffer(stdout, "O");
    CHECK(arceos_fclose(stdout) == 0);
    set_buffer(stderr, "E");
    CHECK(arceos_fclose(stderr) == 0);
    CHECK(close_calls == 3 && free_calls == 0 && written_count == 2 &&
          !memcmp(written_bytes, "OE", 2));

    reset_state();
    set_buffer(stdout, "O");
    write_steps = 1;
    write_results[0] = -1;
    write_errors[0] = EIO;
    close_error = EBADF;
    CHECK(arceos_fclose(stdout) == EOF && errno == EIO);
    CHECK(close_calls == 1 && free_calls == 0 && stdout->buffer_len == 1);

    reset_state();
    close_error = EBADF;
    CHECK(arceos_fclose(stderr) == EOF && errno == EBADF);
    CHECK(close_calls == 1 && free_calls == 0);
}

__attribute__((visibility("default"))) int main(void)
{
    test_open_failures_and_recovery();
    test_success_and_short_writes();
    test_close_errors();
    test_flush_recovery();
    test_static_streams();
    return 0;
}
