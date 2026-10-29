/*
 * UB820 external-SRT engage program (staged on the USB stick, run by the M1
 * helper after it is copied to /tmp).
 *
 * It does not patch code.  It uses the firmware's own engage path:
 *
 *   1. find apl_meda_bdplay in /proc
 *   2. write 1 to bdplay's ENABLE_BD_SRT_TEXT cache (VA 0x43504)
 *   3. set byte 5 ("External Subtitle on") in the shared play-data file
 *      /tmp/apl_play3dlib_play_dat.dat, preserving every other byte
 *   4. post the vendor event 0xb233 to bdplay's EV queue 0x90c44c00 with the
 *      stock /usr/dtvrec/bin/evtSend; bdplay calls FUN_00018b84, which checks
 *      the gate and media type, then starts the SRT worker
 *   5. the worker opens /mnt/usb/sub/sub.srt; because play-data byte 5 is 1,
 *      FUN_000189a0(1) calls FUN_00014370(1) (display on + draw)
 *
 * If the worker was already loaded with byte 5 == 0 (display off), it uses the
 * real UI toggle event 0x8eb5 (sender 0xc3b5, ctl 0x39, arg 1) instead; the
 * handler FUN_00018544 toggles byte 5 on and draws.
 *
 * All paths are absolute.  No flash writes, no code patching, no signals.
 *
 * Build (device, static ARM):
 *   arm-linux-gnueabihf-gcc -static -Os -s -o engage engage.c
 *
 * Test build (x86, fake evtSend):
 *   gcc -O2 -DEVT_PATH='"/tmp/evtSend_fake"' -o engage_test engage.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#ifndef EVT_PATH
#define EVT_PATH "/usr/dtvrec/bin/evtSend"
#endif

/* linux_dirent64 from getdents64(2).  Using the raw syscall (instead of
 * opendir/readdir) avoids glibc's statx(), which is absent on the 4.4 kernel. */
struct linux_dirent64 {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

#define GATE_ADDR   0x43504UL   /* DAT_00043504: ENABLE_BD_SRT_TEXT cache */
#define LOADED_ADDR 0x434e8UL   /* DAT_000434e8: ESL loaded */
#define STATE_ADDR  0x43540UL   /* DAT_00043540: FUN_0001ac28 media state */
#define PLAY_DAT    "/tmp/apl_play3dlib_play_dat.dat"
#define PLAY_LOCK   "/tmp/apl_play3dlib_3ddynamic_lock"

/* Real media states for which FUN_00012fe8 rejects ds commands. */
static int state_ok(unsigned int st) {
    return st != 0 && st != 9 && st != 10;
}

static int mem_read(int pid, unsigned long addr, void *buf, size_t len) {
    char path[32];
    int fd;
    ssize_t n;
    snprintf(path, sizeof path, "/proc/%d/mem", pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    n = pread(fd, buf, len, (off_t)addr);
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

static int mem_write_u32(int pid, unsigned long addr, unsigned int v) {
    char path[32];
    int fd;
    ssize_t n;
    snprintf(path, sizeof path, "/proc/%d/mem", pid);
    fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    n = pwrite(fd, &v, 4, (off_t)addr);
    close(fd);
    return n == 4 ? 0 : -1;
}

static int mem_read_buf(int pid, unsigned long addr, void *buf, size_t len) {
    char path[32];
    int fd;
    ssize_t n;
    snprintf(path, sizeof path, "/proc/%d/mem", pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    n = pread(fd, buf, len, (off_t)addr);
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

static int mem_write_buf(int pid, unsigned long addr, const void *buf, size_t len) {
    char path[32];
    int fd;
    ssize_t n;
    snprintf(path, sizeof path, "/proc/%d/mem", pid);
    fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    n = pwrite(fd, buf, len, (off_t)addr);
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

/* Native-matched style, applied before the 0xb233 event: the font/colours are
 * latched once inside ESL_Open (FUN_0001e254), and libtiwin's dormant 4-pass
 * outline is enabled for TTF with one byte.  0x3148d is the font filename;
 * 0x1eb76/0x1eb88/0x1ed3e are Thumb-2 immediates.  The kernel's
 * /proc/pid/mem write path flushes the I-cache for VM_EXEC. */
#define STYLE_FONT_ADDR 0x3148dUL
#define STYLE_FG_ADDR   0x1eb76UL
#define STYLE_TEXT_BG   0x1eb88UL
#define STYLE_BAND      0x1ed3eUL
#define LIBTIWIN_BASE_DEF 0x293a0000UL
#define LIBTIWIN_OUTLINE  0x15462UL

static const unsigned char style_font[22] = {
    'f','0','1','5','0','0','3','t','.','t','t','f', 0,0,0,0,0,0,0,0,0,0
};
static const unsigned char style_fg[4] = { 0x4f, 0xf0, 0xff, 0x32 };
/* MOVS low half must be zero: ff 22 encoded opaque BLUE 0xff0000ff. */
static const unsigned char style_text_bg[6] = { 0x00, 0x22, 0xcf, 0xf6, 0x00, 0x72 };
static const unsigned char style_band[6] = { 0x00, 0x22, 0xc0, 0xf2, 0x00, 0x02 };
static const unsigned char style_outline = 0x04;

static unsigned long libtiwin_base(int pid) {
    static char buf[0x10000];
    char path[32];
    int fd;
    ssize_t n;
    char *p;
    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    p = buf;
    while ((p = strstr(p, "libtiwin.so.0.0.0")) != NULL) {
        char *ls = p;
        char *perm;
        while (ls > buf && ls[-1] != '\n') ls--;
        perm = strstr(ls, " r-xp ");
        if (perm && perm < p) {
            unsigned long start = strtoul(ls, NULL, 16);
            unsigned long off = strtoul(perm + 6, NULL, 16);
            if (off == 0) return start;
        }
        p++;
    }
    return 0;
}

static void font_size_set(void) {
    int fd = open("/tmp/ti_font_size", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        if (write(fd, "122\n", 4) != 4) { }
        close(fd);
    }
}

static int style_site(int pid, unsigned long addr, const unsigned char *want,
                      size_t len) {
    unsigned char cur[32];
    int i, same = 1;
    if (mem_read_buf(pid, addr, cur, len) != 0) return -1;
    for (i = 0; i < (int)len; i++)
        if (cur[i] != want[i]) { same = 0; break; }
    if (same) return 0;
    if (mem_write_buf(pid, addr, want, len) != 0) return -1;
    if (mem_read_buf(pid, addr, cur, len) != 0) return -1;
    for (i = 0; i < (int)len; i++)
        if (cur[i] != want[i]) return -1;
    return 0;
}

static int style_patch(int pid) {
    unsigned long ti = libtiwin_base(pid);
    int ok = 1;
    if (!ti) ti = LIBTIWIN_BASE_DEF;
    if (style_site(pid, STYLE_FONT_ADDR, style_font, sizeof style_font) != 0) ok = 0;
    if (style_site(pid, STYLE_FG_ADDR, style_fg, sizeof style_fg) != 0) ok = 0;
    if (style_site(pid, STYLE_TEXT_BG, style_text_bg, sizeof style_text_bg) != 0) ok = 0;
    if (style_site(pid, STYLE_BAND, style_band, sizeof style_band) != 0) ok = 0;
    if (style_site(pid, ti + LIBTIWIN_OUTLINE, &style_outline, 1) != 0) ok = 0;
    return ok ? 0 : -1;
}

static int find_bdplay(void) {
    char buf[4096] __attribute__((aligned(8)));
    int fd, n, found = 0;
    fd = open("/proc", O_RDONLY | O_DIRECTORY);
    if (fd < 0) return 0;
    while (!found && (n = (int)syscall(SYS_getdents64, fd, buf, sizeof buf)) > 0) {
        int off = 0;
        while (off < n) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            char path[64], comm[64];
            int cfd;
            ssize_t r;
            off += d->d_reclen;
            if (d->d_name[0] < '0' || d->d_name[0] > '9') continue;
            snprintf(path, sizeof path, "/proc/%s/comm", d->d_name);
            cfd = open(path, O_RDONLY);
            if (cfd < 0) continue;
            r = read(cfd, comm, sizeof comm - 1);
            close(cfd);
            if (r <= 0) continue;
            comm[r] = 0;
            if (strncmp(comm, "apl_meda_bdplay", 15) == 0) {
                found = atoi(d->d_name);
                break;
            }
        }
    }
    close(fd);
    return found;
}

/* Read/write the 8-byte play-data record under the library's own flock. */
static int playdat_byte5(int *out) {
    int lf, fd, rc = -1;
    unsigned char b[8];
    lf = open(PLAY_LOCK, O_CREAT | O_RDWR, 0666);
    if (lf < 0) return -1;
    if (flock(lf, LOCK_EX) != 0) { close(lf); return -1; }
    memset(b, 0, sizeof b);
    fd = open(PLAY_DAT, O_RDONLY);
    if (fd >= 0) {
        if (read(fd, b, sizeof b) == (ssize_t)sizeof b) rc = 0;
        close(fd);
    } else if (errno == ENOENT) {
        rc = 0;
    }
    if (rc == 0) *out = b[5];
    flock(lf, LOCK_UN);
    close(lf);
    return rc;
}

static int playdat_set_on(void) {
    int lf, fd, rc = -1;
    unsigned char b[8];
    lf = open(PLAY_LOCK, O_CREAT | O_RDWR, 0666);
    if (lf < 0) return -1;
    if (flock(lf, LOCK_EX) != 0) { close(lf); return -1; }
    memset(b, 0, sizeof b);
    fd = open(PLAY_DAT, O_RDONLY);
    if (fd >= 0) {
        if (read(fd, b, sizeof b) != (ssize_t)sizeof b) { close(fd); flock(lf, LOCK_UN); close(lf); return -1; }
        close(fd);
    }
    /* Native-matched look: byte0=-8 moves the overlay down ~198 photo px
     * (native baseline ~2526 vs predicted 2532); size index 0 = 52 px scaled
     * by /tmp/ti_font_size=122 to ~63 units (cap ~116 px, native 116).
     * No sync, UTF-8, external on.  Preserve bytes 1, 6, 7. */
    b[0] = (unsigned char)-8;
    b[2] = 0;
    b[3] = 0;
    b[4] = 0;
    b[5] = 1;
    fd = open(PLAY_DAT, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        if (write(fd, b, sizeof b) == (ssize_t)sizeof b) rc = 0;
        close(fd);
    }
    flock(lf, LOCK_UN);
    close(lf);
    return rc;
}

static int run_evt(char *const argv[]) {
    pid_t p = fork();
    int st;
    if (p < 0) return -1;
    if (p == 0) {
        execve(argv[0], argv, NULL);
        _exit(127);
    }
    if (waitpid(p, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* 0xb233: start the SRT worker (sender 0xc352, ctl 0x3b, kind 1, type 1). */
static int evt_load(void) {
    char *const argv[] = {
        EVT_PATH, "0xc352", "0x44", "0xc44c", "0x1", "0x0", "0x1",
        "0x3b", "0xb233", NULL
    };
    return run_evt(argv);
}

/* 0x8eb5: External Subtitle on/off toggle (sender 0xc3b5, ctl 0x39, kind 1,
 * type 1, arg 1).  Only used when the engine is already loaded and the
 * play-data flag is still 0. */
static int evt_toggle(void) {
    char *const argv[] = {
        EVT_PATH, "0xc3b5", "0x44", "0xc44c", "0x1", "0x0", "0x1",
        "0x39", "0x8eb5", "0x0", "0x0", "0x1", NULL
    };
    return run_evt(argv);
}

int main(void) {
    int pid = 0;
    int style_fail = 0;

    /* ~5-10 minutes depending on the continue path; every path sleeps. */
    for (int loops = 0; loops < 1200; loops++) {
        unsigned int gate = 0, state = 0;
        unsigned char loaded = 0;
        int b5 = -1;

        if (pid && kill(pid, 0) != 0) pid = 0;
        if (!pid) {
            pid = find_bdplay();
            if (!pid) { usleep(500000); continue; }
            style_fail = 0;
        }

        /* Read the engine flag first: if it is still 0, set the display flag
         * before enabling the gate, so a concurrent startup launcher's worker
         * also sees byte 5 == 1. */
        if (mem_read(pid, LOADED_ADDR, &loaded, 1) != 0) { pid = 0; continue; }

        if (!loaded) {
            /* ESL_Open latches the font and both GC colours once; the event
             * below creates the worker that calls it.  Patch first. */
            font_size_set();               /* 122% of the 52 px Normal step */
            if (style_patch(pid) != 0) {
                if (++style_fail < 25) { usleep(200000); continue; }
                /* After ~5 s let the event through with whatever landed
                 * (the font write is data-only and normally succeeds). */
            } else {
                style_fail = 0;
            }
            playdat_set_on();               /* force Normal / UTF-8 / position 0 */
            if (mem_read(pid, GATE_ADDR, &gate, 4) != 0) { pid = 0; continue; }
            if (gate == 0) mem_write_u32(pid, GATE_ADDR, 1);
            evt_load();
            usleep(800000);
            continue;
        }

        /* Engine loaded.  The font was created during ESL_Open; remove the
         * global size hook so other Ti apps are not affected.  Keep the gate
         * set.  If the display flag is still 0, use the real UI toggle.
         * FUN_00012fe8 rejects it for media state 0/9/10. */
        unlink("/tmp/ti_font_size");
        if (mem_read(pid, GATE_ADDR, &gate, 4) != 0) { pid = 0; continue; }
        if (gate == 0) mem_write_u32(pid, GATE_ADDR, 1);
        if (playdat_byte5(&b5) == 0 && b5 == 0) {
            if (mem_read(pid, STATE_ADDR, &state, 4) == 0 && state_ok(state)) {
                evt_toggle();
                usleep(400000);
            }
        }
        usleep(600000);
    }
    return 0;
}
