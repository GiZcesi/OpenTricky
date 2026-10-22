/* ISO-backed file layer test (MACOS_PORT_PLAN.md, Phase 1): the POSIX file backend serving the game
 * disc out of an XDVDFS image through the same Nt* entry points the translated game calls.
 *   iso_test <image.iso>        (macos/iso_test/make_xdvdfs.py builds a small one) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include "kernel/kernel.h"
#include "kernel/xbox_xdvdfs.h"

int xbox_iso_handles_in_use(void);
static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static NTSTATUS open_path(const char *path, ULONG disposition, ULONG options, HANDLE *h)
{
    static char buf[512];
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK iosb;
    snprintf(buf, sizeof buf, "%s", path);
    name.Buffer = buf; name.Length = (USHORT)strlen(buf); name.MaximumLength = (USHORT)sizeof buf;
    oa.RootDirectory = NULL; oa.ObjectName = &name; oa.Attributes = 0;
    *h = INVALID_HANDLE_VALUE;
    return xbox_NtCreateFile(h, XBOX_GENERIC_READ, &oa, &iosb, NULL, 0, 0, disposition, options);
}

static uint8_t big_byte(unsigned i) { return (uint8_t)((i * 7 + 3) & 0xFF); }


/* ---- risk probes: things a real title does that the basic cases above do not ---- */
typedef struct { int ok; int id; } reader_arg;

static void *reader_thread(void *p)
{
    reader_arg *a = (reader_arg *)p;
    HANDLE h;
    unsigned seed = 12345u + (unsigned)a->id * 7919u;
    int i;
    a->ok = 1;
    for (i = 0; i < 300 && a->ok; i++) {
        uint8_t buf[512];
        XBOX_IO_STATUS_BLOCK iosb;
        LARGE_INTEGER off;
        unsigned o, n, k;
        if (!NT_SUCCESS(open_path("D:\\data\\big.bin", XBOX_FILE_OPEN, 0, &h))) { a->ok = 0; break; }
        seed = seed * 1103515245u + 12345u;
        o = (seed >> 8) % 4900u;
        n = 1 + (seed >> 4) % 100u;
        off.QuadPart = o;
        if (!NT_SUCCESS(xbox_NtReadFile(h, NULL, NULL, NULL, &iosb, buf, n, &off)) || iosb.Information != n) a->ok = 0;
        for (k = 0; a->ok && k < n; k++) if (buf[k] != big_byte(o + k)) a->ok = 0;
        xbox_NtClose(h);
    }
    return NULL;
}

static void risk_probes(void)
{
    HANDLE h;
    NTSTATUS st;
    XBOX_IO_STATUS_BLOCK iosb;
    union { XBOX_FILE_DIRECTORY_INFORMATION d; uint8_t raw[512]; } u;

    /* the disc's root, as a title opens it to list the disc: with and without separators */
    st = open_path("\\Device\\CdRom0\\", XBOX_FILE_OPEN, XBOX_FILE_DIRECTORY_FILE, &h);
    CHECK(NT_SUCCESS(st), "open the disc root (0x%08X)", (unsigned)st);
    if (NT_SUCCESS(st)) {
        int n = 0;
        while (NT_SUCCESS(xbox_NtQueryDirectoryFile(h, NULL, NULL, NULL, &iosb, &u, sizeof u, NULL, n == 0)) && ++n < 20) {}
        CHECK(n == 3, "the root lists 3 entries (%d)", n);
        xbox_NtClose(h);
    }
    st = open_path("D:\\data\\", XBOX_FILE_OPEN, XBOX_FILE_DIRECTORY_FILE, &h);
    CHECK(NT_SUCCESS(st), "open a directory with a trailing backslash (0x%08X)", (unsigned)st);
    if (NT_SUCCESS(st)) xbox_NtClose(h);
    /* Xbox paths use backslashes: the drive rules match on them, as in the Windows build. Forward
     * slashes are not translated to the disc (they are passed through to the host unchanged). */
    st = open_path("D:/data/hello.txt", XBOX_FILE_OPEN, 0, &h);
    CHECK(!NT_SUCCESS(st), "forward slashes are not a disc path (0x%08X)", (unsigned)st);
    if (NT_SUCCESS(st)) xbox_NtClose(h);
    st = open_path("D:\\data\\hello.txt\\x", XBOX_FILE_OPEN, 0, &h);
    CHECK(!NT_SUCCESS(st), "a file used as a directory is refused (0x%08X)", (unsigned)st);
    if (NT_SUCCESS(st)) xbox_NtClose(h);

    /* a directory query into a buffer that cannot hold the entry must not write past it */
    st = open_path("D:\\data", XBOX_FILE_OPEN, XBOX_FILE_DIRECTORY_FILE, &h);
    if (NT_SUCCESS(st)) {
        static uint8_t guard[256];
        memset(guard, 0xAB, sizeof guard);
        st = xbox_NtQueryDirectoryFile(h, NULL, NULL, NULL, &iosb, guard + 16, 8, NULL, 1);   /* 8 bytes: far too small */
        {
            int clean = 1, i;
            for (i = 24; i < 256; i++) if (guard[i] != 0xAB) clean = 0;
            CHECK(clean, "a too-small directory buffer is not overrun (status 0x%08X)", (unsigned)st);
        }
        xbox_NtClose(h);
    }

    /* handle table exhaustion: refused cleanly, and the slots come back */
    {
        HANDLE hs[300];
        int i, opened = 0, refused = 0;
        for (i = 0; i < 300; i++) {
            st = open_path("D:\\data\\hello.txt", XBOX_FILE_OPEN, 0, &hs[i]);
            if (NT_SUCCESS(st)) opened++; else { refused++; hs[i] = NULL; }
        }
        CHECK(opened == 256 && refused == 44, "the 256-slot table fills and then refuses (%d opened, %d refused)", opened, refused);
        for (i = 0; i < 300; i++) if (hs[i]) xbox_NtClose(hs[i]);
        CHECK(xbox_iso_handles_in_use() == 0, "all slots released (%d)", xbox_iso_handles_in_use());
    }

    /* host-handle APIs handed a virtual handle (a title waiting on, duplicating or closing a file
     * handle through the generic object calls): in a child, so a crash is a result, not the end */
    {
        pid_t pid;
        fflush(stdout);
        pid = fork();
        if (pid == 0) {
            HANDLE ih, dup = NULL;
            if (!NT_SUCCESS(open_path("D:\\data\\hello.txt", XBOX_FILE_OPEN, 0, &ih))) _exit(3);
            WaitForSingleObject(ih, 0);
            DuplicateHandle(GetCurrentProcess(), ih, GetCurrentProcess(), &dup, 0, FALSE, 0);
            SetEvent(ih);
            CloseHandle(ih);
            _exit(0);
        } else {
            int status = 0;
            waitpid(pid, &status, 0);
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "generic handle APIs on a virtual ISO handle do not crash (status 0x%x)", status);
        }
    }

    /* several threads, each opening, reading and closing: no torn slot state */
    {
        pthread_t th[4];
        reader_arg ra[4];
        int i, ok = 1;
        for (i = 0; i < 4; i++) { ra[i].id = i; pthread_create(&th[i], NULL, reader_thread, &ra[i]); }
        for (i = 0; i < 4; i++) { pthread_join(th[i], NULL); if (!ra[i].ok) ok = 0; }
        CHECK(ok && xbox_iso_handles_in_use() == 0, "4 threads reading concurrently (%d slots left)", xbox_iso_handles_in_use());
    }
}

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/ot_iso_test_XXXXXX";
    char *save = mkdtemp(tmpl);
    HANDLE h;
    NTSTATUS st;
    XBOX_IO_STATUS_BLOCK iosb;
    uint8_t buf[8192];
    if (argc < 2) { printf("usage: iso_test <image.iso>\n"); return 2; }
    CHECK(xdvdfs_mount(argv[1]), "mount %s", argv[1]);
    xbox_path_init("/nonexistent-game-dir", save);          /* the disc must come from the image, not from here */

    /* a small file, by two spellings of the disc */
    st = open_path("\\Device\\CdRom0\\data\\hello.txt", XBOX_FILE_OPEN, 0, &h);
    CHECK(NT_SUCCESS(st), "open hello.txt (0x%08X)", (unsigned)st);
    CHECK(xbox_iso_handles_in_use() == 1, "one virtual handle in use (%d)", xbox_iso_handles_in_use());
    st = xbox_NtReadFile(h, NULL, NULL, NULL, &iosb, buf, sizeof buf, NULL);
    CHECK(NT_SUCCESS(st) && iosb.Information == 20 && !memcmp(buf, "hello from the disc\n", 20), "read hello.txt");
    st = xbox_NtReadFile(h, NULL, NULL, NULL, &iosb, buf, 16, NULL);
    CHECK(st == STATUS_END_OF_FILE, "sequential read at the end is EOF (0x%08X)", (unsigned)st);
    xbox_NtClose(h);
    CHECK(xbox_iso_handles_in_use() == 0, "closing releases the slot (%d)", xbox_iso_handles_in_use());

    /* case-insensitive, drive-letter form; a file spanning sectors; positioned and sequential reads */
    st = open_path("d:\\DATA\\Big.BIN", XBOX_FILE_OPEN, 0, &h);
    CHECK(NT_SUCCESS(st), "open big.bin as d:\\DATA\\Big.BIN (0x%08X)", (unsigned)st);
    {
        XBOX_FILE_STANDARD_INFORMATION si;
        XBOX_FILE_NETWORK_OPEN_INFORMATION ni;
        XBOX_FILE_POSITION_INFORMATION pi;
        LARGE_INTEGER off;
        int ok = 1, i;
        st = xbox_NtQueryInformationFile(h, &iosb, &si, sizeof si, XboxFileStandardInformation);
        CHECK(NT_SUCCESS(st) && si.EndOfFile.QuadPart == 5000 && si.AllocationSize.QuadPart == 6144 && !si.Directory,
              "standard info: size %lld alloc %lld", (long long)si.EndOfFile.QuadPart, (long long)si.AllocationSize.QuadPart);
        st = xbox_NtQueryInformationFile(h, &iosb, &ni, sizeof ni, XboxFileNetworkOpenInformation);
        CHECK(NT_SUCCESS(st) && ni.EndOfFile.QuadPart == 5000 && (ni.FileAttributes & FILE_ATTRIBUTE_READONLY),
              "network-open info (the size call FILE_size makes)");
        off.QuadPart = 2040;                                    /* crosses the 2048-byte sector boundary */
        st = xbox_NtReadFile(h, NULL, NULL, NULL, &iosb, buf, 100, &off);
        for (i = 0; i < 100; i++) if (buf[i] != big_byte(2040 + i)) ok = 0;
        CHECK(NT_SUCCESS(st) && iosb.Information == 100 && ok, "positioned read across a sector boundary");
        st = xbox_NtQueryInformationFile(h, &iosb, &pi, sizeof pi, XboxFilePositionInformation);
        CHECK(NT_SUCCESS(st) && pi.CurrentByteOffset.QuadPart == 0, "a positioned read leaves the position alone");
        st = xbox_NtReadFile(h, NULL, NULL, NULL, &iosb, buf, 3000, NULL);
        st = xbox_NtReadFile(h, NULL, NULL, NULL, &iosb, buf + 3000, 3000, NULL);
        ok = NT_SUCCESS(st) && iosb.Information == 2000;
        for (i = 0; i < 5000; i++) if (buf[i] != big_byte((unsigned)i)) ok = 0;
        CHECK(ok, "two sequential reads cover the whole file (second returned %u)", (unsigned)iosb.Information);
        pi.CurrentByteOffset.QuadPart = 4990;
        st = xbox_NtSetInformationFile(h, &iosb, &pi, sizeof pi, XboxFilePositionInformation);
        st = xbox_NtReadFile(h, NULL, NULL, NULL, &iosb, buf, 100, NULL);
        CHECK(NT_SUCCESS(st) && iosb.Information == 10 && buf[0] == big_byte(4990), "seek, then a short read at the end");
        {
            uint8_t w = 1;
            st = xbox_NtWriteFile(h, NULL, NULL, NULL, &iosb, &w, 1, NULL);
            CHECK(st == STATUS_MEDIA_WRITE_PROTECTED, "the disc refuses writes (0x%08X)", (unsigned)st);
        }
    }
    xbox_NtClose(h);

    /* a nested path, a missing file, and an overwrite of a disc file */
    st = open_path("D:\\data\\sub\\deep.txt", XBOX_FILE_OPEN, 0, &h);
    CHECK(NT_SUCCESS(st), "open a file two directories down (0x%08X)", (unsigned)st);
    xbox_NtClose(h);
    st = open_path("D:\\data\\nope.txt", XBOX_FILE_OPEN, 0, &h);
    CHECK(st == STATUS_OBJECT_NAME_NOT_FOUND, "missing file (0x%08X)", (unsigned)st);
    st = open_path("D:\\data\\hello.txt", XBOX_FILE_SUPERSEDE, 0, &h);
    CHECK(st == STATUS_MEDIA_WRITE_PROTECTED, "creating over a disc file is refused (0x%08X)", (unsigned)st);
    CHECK(xbox_iso_handles_in_use() == 0, "no slot leaked by the refused open (%d)", xbox_iso_handles_in_use());

    /* attributes without opening */
    {
        XBOX_ANSI_STRING name; XBOX_OBJECT_ATTRIBUTES oa; XBOX_FILE_NETWORK_OPEN_INFORMATION ni;
        char p[] = "D:\\data\\big.bin";
        name.Buffer = p; name.Length = (USHORT)strlen(p); name.MaximumLength = sizeof p;
        oa.RootDirectory = NULL; oa.ObjectName = &name; oa.Attributes = 0;
        st = xbox_NtQueryFullAttributesFile(&oa, &ni);
        CHECK(NT_SUCCESS(st) && ni.EndOfFile.QuadPart == 5000, "NtQueryFullAttributesFile on the disc");
        strcpy(p, "D:\\data");
        name.Length = (USHORT)strlen(p);
        st = xbox_NtQueryFullAttributesFile(&oa, &ni);
        CHECK(NT_SUCCESS(st) && (ni.FileAttributes & FILE_ATTRIBUTE_DIRECTORY), "a directory reports as one");
    }

    /* directory enumeration: one entry per call until NO_MORE_FILES */
    st = open_path("D:\\data", XBOX_FILE_OPEN, XBOX_FILE_DIRECTORY_FILE, &h);
    CHECK(NT_SUCCESS(st), "open the directory (0x%08X)", (unsigned)st);
    {
        union { XBOX_FILE_DIRECTORY_INFORMATION d; uint8_t raw[512]; } u;
        int seen_hello = 0, seen_big = 0, seen_sub = 0, subdir = 0, n = 0;
        for (;;) {
            st = xbox_NtQueryDirectoryFile(h, NULL, NULL, NULL, &iosb, &u, sizeof u, NULL, n == 0);
            if (st == STATUS_NO_MORE_FILES) break;
            if (!NT_SUCCESS(st) || ++n > 20) break;
            if (!strcasecmp(u.d.FileName, "hello.txt")) seen_hello = 1;
            if (!strcasecmp(u.d.FileName, "big.bin")) seen_big = 1;
            if (!strcasecmp(u.d.FileName, "sub")) { seen_sub = 1; subdir = (u.d.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0; }
        }
        CHECK(n == 3 && seen_hello && seen_big && seen_sub && subdir, "directory lists its 3 entries (%d)", n);
    }
    xbox_NtClose(h);

    /* saves are not the disc: U: goes to the host folder, and works while the disc is mounted */
    st = open_path("U:\\save.dat", XBOX_FILE_CREATE, 0, &h);
    CHECK(NT_SUCCESS(st), "create a save file on U: (0x%08X)", (unsigned)st);
    if (NT_SUCCESS(st)) {
        uint8_t w[4] = { 1, 2, 3, 4 };
        st = xbox_NtWriteFile(h, NULL, NULL, NULL, &iosb, w, 4, NULL);
        CHECK(NT_SUCCESS(st) && iosb.Information == 4, "write it");
        xbox_NtClose(h);
    }
    xbox_NtClose(INVALID_HANDLE_VALUE);                         /* must not crash on a tagged or invalid handle */
    xbox_NtClose((HANDLE)(uintptr_t)0x1500D15C00000042ull);     /* a stale virtual handle */

    risk_probes();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
