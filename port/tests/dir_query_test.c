/*
 * dir_query_test.c -- host test of what NtQueryDirectoryFile writes into the
 * caller's buffer (xboxrecomp/src/kernel/kernel_file.c).
 *
 *   gcc -std=gnu11 -O3 -Wall -Wextra -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
 *       -I xboxrecomp/src/kernel -I xboxrecomp/src port/tests/dir_query_test.c \
 *       -o dir_query_test.exe && ./dir_query_test.exe
 *
 * The real kernel_file.c is built into the test. A directory of the disc
 * image is one of its virtual handles, with a listing the test makes up
 * (xdvdfs_list below); a host directory is a real one, made in the temporary
 * folder. Each query writes into a buffer filled with a guard byte, so that
 * anything written past the length passed shows. Checked:
 *   - a buffer shorter than the fixed part of an entry (72 bytes):
 *     STATUS_INFO_LENGTH_MISMATCH, nothing written, the scan where it was;
 *   - a disc name longer than the buffer holds: cut at the buffer's end,
 *     STATUS_BUFFER_OVERFLOW, FileNameLength the length copied;
 *   - a name that fits, in the buffers the title passes (0x146 and 0x148
 *     bytes) or just big enough: every byte as expected, guard intact.
 * Built against kernel sources that copied the name whatever the length,
 * the short-buffer and long-name cases fail and the others pass.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kernel_file.c"

#ifndef STATUS_INFO_LENGTH_MISMATCH     /* kernel.h before the length check */
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif

/* ---- What kernel_file.c needs from the rest of the kernel ---------------- */

void xbox_log(int level, const char *subsystem, const char *fmt, ...)
{
    (void)level; (void)subsystem; (void)fmt;
}

BOOL xbox_path_split_game_disc(const char *xbox_path, const char **remainder)
{
    (void)xbox_path; (void)remainder;
    return FALSE;
}

BOOL xbox_translate_path(const char *xbox_path, xbox_host_char *host_path_buf, DWORD buf_size)
{
    (void)xbox_path; (void)host_path_buf; (void)buf_size;
    return FALSE;
}

BOOL xdvdfs_is_mounted(void) { return TRUE; }

BOOL xdvdfs_find(const char *rel_path, uint32_t *out_sector, uint32_t *out_size,
                 uint8_t *out_attrs)
{
    (void)rel_path; (void)out_sector; (void)out_size; (void)out_attrs;
    return FALSE;
}

uint32_t xdvdfs_read(uint32_t sector, uint32_t size, uint32_t offset, void *buf,
                     uint32_t len)
{
    (void)sector; (void)size; (void)offset; (void)buf; (void)len;
    return 0;
}

/* The disc directory's listing: two entries the test sets */
static xdvdfs_entry s_disc[2];

uint32_t xdvdfs_list(uint32_t sector, uint32_t size, xdvdfs_entry *out, uint32_t max)
{
    uint32_t i, n = sizeof(s_disc) / sizeof(s_disc[0]);
    (void)sector; (void)size;
    for (i = 0; i < n && i < max; i++)
        out[i] = s_disc[i];
    return i;
}

/* ---- Checks --------------------------------------------------------------- */

#define GUARD   0xA5
#define BUFSZ   1024
#define HEADER  ((ULONG)offsetof(XBOX_FILE_DIRECTORY_INFORMATION, FileName))

static unsigned char s_buf[BUFSZ];
static int s_checks, s_fail;

static void check(int ok, const char *what, const char *detail)
{
    s_checks++;
    if (!ok) {
        s_fail++;
        printf("FAIL %-46s %s\n", what, detail);
    }
}

/* First byte past `from` that is not the guard, or -1 */
static long first_written(ULONG from)
{
    ULONG i;
    for (i = from; i < BUFSZ; i++)
        if (s_buf[i] != GUARD)
            return (long)i;
    return -1;
}

static NTSTATUS query(HANDLE h, ULONG length, BOOLEAN restart, XBOX_IO_STATUS_BLOCK *ios)
{
    memset(s_buf, GUARD, sizeof(s_buf));
    ios->Status = 0x12345678;
    ios->Information = 0x9ABC;
    return xbox_NtQueryDirectoryFile(h, NULL, NULL, NULL, ios, s_buf, length, NULL, restart);
}

/* A disc entry as it should come back: the fixed part (72 bytes) zeroed then
 * filled in, the name at offset 64 cut at the buffer's end, a terminator if
 * there is room for it, the guard everywhere else. */
static void check_disc_entry(const char *what, ULONG length, NTSTATUS st,
                             const XBOX_IO_STATUS_BLOCK *ios, const xdvdfs_entry *e)
{
    unsigned char want[BUFSZ];
    PXBOX_FILE_DIRECTORY_INFORMATION w = (PXBOX_FILE_DIRECTORY_INFORMATION)want;
    ULONG room = length - HEADER;
    ULONG n = (ULONG)strlen(e->name);
    ULONG copy = n < room ? n : room;
    NTSTATUS want_st = copy < n ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    char detail[160];
    long diff = -1;
    ULONG i;

    memset(want, GUARD, sizeof(want));
    memset(want, 0, sizeof(XBOX_FILE_DIRECTORY_INFORMATION));
    w->FileNameLength = copy;
    memcpy(w->FileName, e->name, copy);
    if (copy < room)
        w->FileName[copy] = '\0';
    w->EndOfFile.QuadPart = e->size;
    w->AllocationSize.QuadPart = (e->size + 2047) & ~2047LL;
    w->FileAttributes = FILE_ATTRIBUTE_READONLY |
        ((e->attrs & XDVDFS_ATTR_DIRECTORY) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
    for (i = 0; i < BUFSZ && diff < 0; i++)
        if (s_buf[i] != want[i])
            diff = (long)i;

    snprintf(detail, sizeof(detail), "status want 0x%08lX got 0x%08lX",
             (unsigned long)want_st, (unsigned long)st);
    check(st == want_st && ios->Status == want_st, what, detail);
    snprintf(detail, sizeof(detail), "nothing past %lu bytes: first written byte %ld",
             (unsigned long)length, first_written(length));
    check(first_written(length) < 0, what, detail);
    snprintf(detail, sizeof(detail), "entry bytes: first difference at %ld", diff);
    check(diff < 0, what, detail);
    snprintf(detail, sizeof(detail), "Information want %lu got %lu",
             (unsigned long)(want_st == STATUS_SUCCESS ? sizeof(XBOX_FILE_DIRECTORY_INFORMATION)
                                                       : length),
             (unsigned long)ios->Information);
    check(ios->Information == (want_st == STATUS_SUCCESS
                                   ? sizeof(XBOX_FILE_DIRECTORY_INFORMATION) : length),
          what, detail);
}

/* A buffer too short for the fixed part: refused, nothing written, the
 * I/O status untouched */
static void check_refused(const char *what, NTSTATUS st, const XBOX_IO_STATUS_BLOCK *ios)
{
    char detail[160];
    snprintf(detail, sizeof(detail), "status want 0x%08lX got 0x%08lX",
             (unsigned long)STATUS_INFO_LENGTH_MISMATCH, (unsigned long)st);
    check(st == STATUS_INFO_LENGTH_MISMATCH, what, detail);
    snprintf(detail, sizeof(detail), "nothing written: first written byte %ld",
             first_written(0));
    check(first_written(0) < 0, what, detail);
    check(ios->Status == 0x12345678 && ios->Information == 0x9ABC, what,
          "I/O status block written");
}

static void set_disc_name(int i, char c, size_t n)
{
    memset(s_disc[i].name, c, n);
    s_disc[i].name[n] = '\0';
}

static void test_disc(void)
{
    static const struct { const char *what; ULONG length; size_t name; } fit[] = {
        { "disc: 11-byte name, 0x148 bytes",       0x148, 11 },
        { "disc: 255-byte name, 0x148 bytes",      0x148, 255 },
        { "disc: 255-byte name, 0x146 bytes",      0x146, 255 },
        { "disc: 23-byte name, 88 bytes",          88,    23 },
        { "disc: 24-byte name, 88 bytes (no NUL)", 88,    24 },
        { "disc: 7-byte name, 72 bytes",           72,    7 },
        { "disc: 8-byte name, 72 bytes (no NUL)",  72,    8 },
        { "disc: 255-byte name, 88 bytes",         88,    255 },
        { "disc: 255-byte name, 72 bytes",         72,    255 },
    };
    XBOX_IO_STATUS_BLOCK ios;
    HANDLE h = iso_handle_alloc(100, 2048, TRUE);
    NTSTATUS st;
    size_t i;

    s_disc[0].sector = 200; s_disc[0].size = 5000; s_disc[0].attrs = 0;
    s_disc[1].sector = 300; s_disc[1].size = 0;    s_disc[1].attrs = XDVDFS_ATTR_DIRECTORY;
    set_disc_name(1, 'b', 9);

    for (i = 0; i < sizeof(fit) / sizeof(fit[0]); i++) {
        set_disc_name(0, 'a', fit[i].name);
        st = query(h, fit[i].length, TRUE, &ios);
        check_disc_entry(fit[i].what, fit[i].length, st, &ios, &s_disc[0]);
    }

    /* Too short for the fixed part: refused, and the scan goes on from
     * where it was */
    set_disc_name(0, 'a', 40);
    st = query(h, 0x148, TRUE, &ios);
    check_disc_entry("disc: first entry, 0x148 bytes", 0x148, st, &ios, &s_disc[0]);
    st = query(h, sizeof(XBOX_FILE_DIRECTORY_INFORMATION) - 1, FALSE, &ios);
    check_refused("disc: 71 bytes", st, &ios);
    st = query(h, HEADER, FALSE, &ios);
    check_refused("disc: 64 bytes (header only)", st, &ios);
    st = query(h, 0, FALSE, &ios);
    check_refused("disc: 0 bytes", st, &ios);
    st = query(h, 0x148, FALSE, &ios);
    check_disc_entry("disc: then 0x148 bytes gives the second entry", 0x148, st, &ios,
                     &s_disc[1]);
    st = query(h, 0x148, FALSE, &ios);
    check(st == STATUS_NO_MORE_FILES, "disc: then no more files", "status");
}

/* ---- A host directory, the path the title's saves take ------------------- */

static void test_host(void)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    static const WCHAR *const names[] = { L"a_first.ssx", L"b_second.ssx" };
    XBOX_IO_STATUS_BLOCK ios;
    PXBOX_FILE_DIRECTORY_INFORMATION e = (PXBOX_FILE_DIRECTORY_INFORMATION)s_buf;
    HANDLE h;
    NTSTATUS st;
    char detail[160];
    size_t i;

    GetTempPathW(MAX_PATH, dir);
    swprintf_s(path, MAX_PATH, L"%lsdir_query_test_%lu", dir,
               (unsigned long)GetCurrentProcessId());
    wcscpy_s(dir, MAX_PATH, path);
    if (!CreateDirectoryW(dir, NULL)) {
        check(0, "host: temporary folder", "could not be made");
        return;
    }
    for (i = 0; i < 2; i++) {
        HANDLE f;
        swprintf_s(path, MAX_PATH, L"%ls\\%ls", dir, names[i]);
        f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE)
            CloseHandle(f);
    }
    h = CreateFileW(dir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE |
                    FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    check(h != INVALID_HANDLE_VALUE, "host: directory handle", "could not be opened");

    if (h != INVALID_HANDLE_VALUE) {
        /* Too short for the fixed part, twice: refused, nothing consumed */
        st = query(h, 40, TRUE, &ios);
        check_refused("host: 40 bytes", st, &ios);
        st = query(h, HEADER, FALSE, &ios);
        check_refused("host: 64 bytes (header only)", st, &ios);

        /* Then the title's buffer: the first file, nothing past the length */
        st = query(h, 0x148, FALSE, &ios);
        snprintf(detail, sizeof(detail), "status 0x%08lX, name length %lu",
                 (unsigned long)st, (unsigned long)e->FileNameLength);
        check(st == STATUS_SUCCESS && e->FileNameLength == 11 &&
              memcmp(e->FileName, "a_first.ssx", 11) == 0 &&
              ios.Information == HEADER + 11, "host: then 0x148 bytes gives the first file",
              detail);
        snprintf(detail, sizeof(detail), "first written byte %ld", first_written(0x148));
        check(first_written(0x148) < 0, "host: nothing past 0x148 bytes", detail);

        st = query(h, 0x146, FALSE, &ios);
        check(st == STATUS_SUCCESS && e->FileNameLength == 12 &&
              memcmp(e->FileName, "b_second.ssx", 12) == 0 && first_written(0x146) < 0,
              "host: then the second file, in 0x146 bytes", "entry");
        st = query(h, 0x148, FALSE, &ios);
        check(st == STATUS_NO_MORE_FILES, "host: then no more files", "status");

        xbox_dir_context_release(h);
        CloseHandle(h);
    }

    for (i = 0; i < 2; i++) {
        swprintf_s(path, MAX_PATH, L"%ls\\%ls", dir, names[i]);
        DeleteFileW(path);
    }
    RemoveDirectoryW(dir);
}

int main(void)
{
    test_disc();
    test_host();
    printf("%d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
