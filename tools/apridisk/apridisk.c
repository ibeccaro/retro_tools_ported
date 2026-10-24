/*
 * apridisk.c -- Linux port of APRIDISK.EXE
 *               "Apricot floppy disk filing system" (DOS build of Oct 7 2004)
 *
 * Original program: (c) 1998-2001 Jonathan Marsters (per its banner).
 * This port was written from a disassembly of that binary: the .apr image
 * format, boot-sector classification, info printout, menu, prompts and
 * messages follow the original; DOS-specific pieces (INT 13h, conio, DOS
 * paths) are replaced by POSIX equivalents.
 *
 * Image format (as found in the binary)
 *   0x00  128-byte header: "ACT Apricot disk image\x1a\x04", zero padded
 *   0x80  sequence of items, each = 18-byte header + data:
 *           u32 type        E31D0001 sector, E31D0002 comment, E31D0003 creator
 *                           (E31D0000 = deleted, ignored by readers)
 *           u16 compression 9E90 raw, 3E5A "all bytes equal" (data = u16 count, u8 byte)
 *           u16 header size (0x12)
 *           u32 data size
 *           u32 ident       sectors: cyl<<16 | sector<<8 | head ; others: 0
 *           u16 pad
 *   All integers little-endian.  Items are appended; readers scan from 0x80.
 *
 * Differences from the DOS build are marked "PORT:".
 *
 * Build:  cc -O2 -Wall -o apridisk apridisk.c
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fd.h>
#endif

#define PROG_DATE   "Oct  7 2004"           /* banner text of the original build */
#define CREATOR_TAG "APRIDISK Linux port"   /* PORT: stored in the creator item */
#define SECSZ       512
#define APR_HDR_LEN 0x80
#define ITEM_HDR    0x12
#define APR_SIG     "ACT Apricot disk image\x1a\x04"
#define ITEM_DELETED 0xE31D0000u
#define ITEM_SECTOR  0xE31D0001u
#define ITEM_COMMENT 0xE31D0002u
#define ITEM_CREATOR 0xE31D0003u
#define COMP_RAW     0x9E90u
#define COMP_RLE     0x3E5Au
#define KEY_UP       0x148
#define INI_NAME     "apridisk.ini"
#define DBG_NAME     "apridisk.dbg"

/* ------------------------------------------------------------------ globals */
static char  g_dir[PATH_MAX] = "./disks";   /* ini line 1: default directory  */
static long  g_nextDisk = 1;                /* ini line 2: next disk number   */
static int   g_numbered = 0;                /* "/n" : numbered-disk mode      */
static char  g_device[PATH_MAX] = "/dev/fd0";   /* PORT: replaces BIOS drive A: */
static char  g_hist[1024];                  /* last comment, for Up-arrow     */

/* ---------------------------------------------------------------- terminal */
static struct termios g_tio;
static int g_istty, g_raw;

static void term_restore(void)
{ if (g_raw) { tcsetattr(0, TCSANOW, &g_tio); g_raw = 0; } }

static void term_raw(void)
{
    struct termios t;
    if (!g_istty || g_raw) return;
    t = g_tio; t.c_lflag &= ~(ICANON | ECHO); t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t); g_raw = 1;
}

static void on_signal(int s) { term_restore(); signal(s, SIG_DFL); raise(s); }

static int kbhit(void)
{
    struct pollfd p = { 0, POLLIN, 0 };
    return g_istty && poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}

/* DOS getch(): returns the key; ESC = 27, Up-arrow = KEY_UP; EOF acts as ESC */
static int getch(void)
{
    unsigned char c, s[2];
    ssize_t n;
    do n = read(0, &c, 1); while (n < 0 && errno == EINTR);
    if (n != 1) return 27;
    if (c == 27 && g_istty) {
        struct pollfd p = { 0, POLLIN, 0 };
        if (poll(&p, 1, 40) > 0 && read(0, &s[0], 1) == 1) {
            if (s[0] == '[' || s[0] == 'O') {
                if (read(0, &s[1], 1) == 1 && s[1] == 'A') return KEY_UP;
            }
            return 0;                         /* some other escape sequence */
        }
    }
    return c;
}

/* printf that flushes (the DOS build wrote straight to the console) */
static void say(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); fflush(stdout);
}

/* ---------------------------------------------------------------- helpers */
static unsigned u16(const uint8_t *p, int o) { return p[o] | (p[o + 1] << 8); }
static uint32_t u32(const uint8_t *p, int o)
{ return p[o] | (p[o+1] << 8) | ((uint32_t)p[o+2] << 16) | ((uint32_t)p[o+3] << 24); }
static void p16(uint8_t *p, int o, unsigned v) { p[o] = v; p[o+1] = v >> 8; }
static void p32(uint8_t *p, int o, uint32_t v)
{ p[o] = v; p[o+1] = v >> 8; p[o+2] = v >> 16; p[o+3] = v >> 24; }

typedef struct { int cyl, head, sector; } CHS;        /* sector is 1-based */
static uint32_t chs_ident(const CHS *c)
{ return ((uint32_t)c->cyl << 16) | ((uint32_t)c->sector << 8) | (uint32_t)c->head; }

/* ------------------------------------------------- disk-format detection */
typedef struct { int type, bps, cylinders, heads, spt, flag; } DiskFormat;

/* classifier (orig 0xBF94): 0,1,2 Apricot native; 3 MS-DOS BPB; 6 other; 7 unknown */
static int classify(const uint8_t *s)
{
    uint8_t media = s[0x15], a = s[0x5a], c = s[0x5d];
    if (media == 0xF0 || media == 0xF9) return 3;
    if (media >= 0xF8) return 7;
    if (a == 0xFC && c == 0) return 0;
    if (a == 0xFD && c == 1) return 1;
    if (a == 0xFE && c == 2) return 2;
    if ((a == 0xF8 && c == 3) || (a == 0xF9 && c == 4) || (a == 0xFA && c == 5)) return 7;
    if (s[0x0b] == 0 || s[0x0d] != 0) return 7;
    if (u16(s, 0x18) != 0x200 || s[0x08] == 0) return 7;
    return 6;
}

/* orig 0xC0BC; returns 1 and fills *f, or 0 if the format is unknown */
static int detect_format(const uint8_t *s, DiskFormat *f)
{
    int t = classify(s);
    if (t == 7) return 0;
    f->type = t;
    if (t == 3) {
        int total = u16(s, 0x13), spt = u16(s, 0x18), heads = u16(s, 0x1a);
        if (spt == 0 || heads == 0) return 0;          /* PORT: original divides by zero */
        f->bps = u16(s, 0x0b);
        f->cylinders = (total / spt) / heads;
        f->heads = heads; f->spt = spt; f->flag = (spt != 9);
    } else {
        f->bps = 0x200;
        f->cylinders = u16(s, 0x12); f->heads = s[0x16]; f->spt = u16(s, 0x10); f->flag = 0;
    }
    return f->cylinders > 0 && f->heads > 0 && f->spt > 0;   /* PORT: sanity check */
}

/* orig 0xB963: disk information block */
static void describe_disk(const uint8_t *s, FILE *o)
{
    static const char *os[] = { "Invalid", "MS-DOS", "p-System", "CP/M 86",
                                "Concurrent CP/M", "BOS", "Unix" };
    static const char *bt[] = { "non-bootable", "Apricot and Xi RAM BIOS", "Generic ROM BIOS",
                                "Apricot and Xi ROM BIOS", "Apricot Portable ROM BIOS",
                                "Apricot F1 ROM BIOS", "Apricot Xen ROM BIOS" };
    int si = 0;

    if (classify(s) != 3) {
        fputs(s[0x0b] == 0 ? " [Apricot data disk]\n" : " [Apricot boot disk]\n", o);
        fputs(" Operating system:       ", o);
        if (s[8] <= 6) fputs(os[s[8]], o); else fprintf(o, "Unknown (0x%02x)", s[8]);
        fputs("\n", o);
        fprintf(o, " Software write protect: %s\n", s[9] ? "ON" : "OFF");
        fprintf(o, " Copy protect:           %s\n", s[10] ? "ON" : "OFF");
        fputs(" Boot disk type:         ", o);
        if (s[0x0b] <= 6) fputs(bt[s[0x0b]], o); else fprintf(o, "Unknown (0x%02x)", s[0x0b]);
        fputs("\n", o);
        fprintf(o, " Sectors per track:    %3d\n", u16(s, 0x10));
        fprintf(o, " Cylinders:            %3d\n", u16(s, 0x12));
        fprintf(o, " Number of heads:      %3d\n", s[0x16]);
        if (s[8] == 1) {
            fprintf(o, " MS-DOS Media ID byte:   0x%02x (", s[0x5a]);
            switch (s[0x5a]) {
            case 0xf8: fputs("5M Winchester", o); break;
            case 0xf9: fputs("10M Winchester", o); break;
            case 0xfa: fputs("20M Winchester", o); break;
            case 0xfc: fputs("70 track Single Sided", o); break;
            case 0xfd: fputs("80 track Single Sided", o); break;
            case 0xfe: fputs("80 track Double Sided", o); break;
            default:   fprintf(o, "Unknown (0x%02x)", s[0x5a]);
            }
            fputs(")\n", o);
            fputs(" MS-DOS Disk type:       ", o);      /* (sic: original prints a lone ")" below) */
            switch (s[0x5d]) {
            case 0: fputs("70 track Single Sided", o); break;
            case 1: fputs("80 track Single Sided", o); break;
            case 2: fputs("80 track Double Sided", o); break;
            case 3: fputs("5M Winchester", o); break;
            case 4: fputs("10M Winchester", o); break;
            case 5: fputs("20M Winchester", o); break;
            default: fprintf(o, "Unknown (0x%02x)", s[0x5d]);
            }
            fputs(")\n", o);
        }
    } else {
        char oem[9]; int spt = u16(s, 0x18), heads = u16(s, 0x1a), total = u16(s, 0x13);
        memcpy(oem, s + 3, 8); oem[8] = 0;
        fputs(" [Microsoft data disk]\n", o);
        fprintf(o, " MSDOS Media ID byte:    0x%02x\n", s[0x15]);
        fprintf(o, " OEM name and version:   %s\n", oem);
        fprintf(o, " Bytes per sector =      %d\n", u16(s, 0x0b));
        fprintf(o, " Total sectors:         %4d\n", total);
        fprintf(o, " Sectors per track:    %3d\n", spt);
        fprintf(o, " Number of heads:      %3d\n", heads);
        fprintf(o, " Hence cylinders:      %3d\n", (spt && heads) ? total / spt / heads : 0);
        fprintf(o, " Boot code jump:         0x%04X\n", u16(s, 0));
        si = spt > 10;
    }
    fputs("\n", o);
    fputs(" Suitable modern media for this disk is ", o);
    fputs(si ? "DSHD (1.44Mb formatted)" : "DSDD (720Kb formatted)", o);
    fputs("\n", o);
}

/* ------------------------------------------- PORT: Linux disk/image device */
/* Replaces the INT 13h layer.  The device may be a floppy (/dev/fdN), another
 * block device (USB floppy) or a plain raw image file; sectors are addressed
 * by CHS and mapped to a linear offset using the format's geometry. */
typedef struct { int fd; int is_floppy; int warned; } Dev;

static int dev_open(Dev *d, int writable)
{
    d->fd = open(g_device, (writable ? O_RDWR : O_RDONLY) | O_NONBLOCK);
    d->is_floppy = 0; d->warned = 0;
    if (d->fd < 0) { say("Unable to open %s: %s\n", g_device, strerror(errno)); return 0; }
#ifdef FDGETPRM
    { struct floppy_struct fs;
      if (ioctl(d->fd, FDGETPRM, &fs) == 0) { d->is_floppy = 1; ioctl(d->fd, FDFLUSH); } }
#endif
    return 1;
}

static void dev_close(Dev *d) { if (d->fd >= 0) { fsync(d->fd); close(d->fd); d->fd = -1; } }

/* tell the kernel floppy driver the geometry of the disk we are about to use */
static void dev_set_geometry(Dev *d, const DiskFormat *f)
{
#ifdef FDGETPRM
    struct floppy_struct fs;
    if (!d->is_floppy) return;
    if (ioctl(d->fd, FDGETPRM, &fs) != 0) return;
    if ((int)fs.sect == f->spt && (int)fs.head == f->heads && (int)fs.track == f->cylinders) return;
    fs.sect = f->spt; fs.head = f->heads; fs.track = f->cylinders;
    fs.size = f->spt * f->heads * f->cylinders;
    if (ioctl(d->fd, FDSETPRM, &fs) != 0 && !d->warned) {
        d->warned = 1;
        say("(warning: could not set floppy geometry: %s)\n", strerror(errno));
    }
#else
    (void)d; (void)f;
#endif
}

static off_t dev_off(const DiskFormat *f, const CHS *c)
{ return ((off_t)(c->cyl * f->heads + c->head) * f->spt + (c->sector - 1)) * SECSZ; }

static int dev_io(Dev *d, const DiskFormat *f, const CHS *c, void *buf, int n, int wr)
{
    size_t len = (size_t)n * SECSZ;
    ssize_t r = wr ? pwrite(d->fd, buf, len, dev_off(f, c)) : pread(d->fd, buf, len, dev_off(f, c));
    return r == (ssize_t)len;
}

/* boot sector is always cyl 0, head 0, sector 1 == offset 0 */
static int dev_read_boot(Dev *d, uint8_t *buf)
{ return pread(d->fd, buf, SECSZ, 0) == SECSZ; }

/* ------------------------------------------------------- .apr image file */
typedef struct { FILE *fp; long cached; } AprFile;

typedef struct { uint32_t type, ident, dsize; unsigned comp, hsize; } ItemHdr;

static int read_item_hdr(AprFile *f, long pos, ItemHdr *h)
{
    uint8_t b[ITEM_HDR];
    if (fseek(f->fp, pos, SEEK_SET) != 0 || fread(b, 1, ITEM_HDR, f->fp) != ITEM_HDR) return 0;
    h->type = u32(b, 0); h->comp = u16(b, 4); h->hsize = u16(b, 6);
    h->dsize = u32(b, 8); h->ident = u32(b, 12);
    return 1;
}

static int write_item(AprFile *f, uint32_t type, uint32_t ident, const uint8_t *data, unsigned len)
{
    uint8_t h[ITEM_HDR] = { 0 }, rle[3];
    unsigned comp = COMP_RAW, outlen = len, i;
    const uint8_t *out = data;
    if (len > 3 && len <= 0xFFFF) {                    /* orig 0xAF74 */
        for (i = 1; i < len && data[i] == data[0]; i++) ;
        if (i == len) { rle[0] = len; rle[1] = len >> 8; rle[2] = data[0];
                        out = rle; outlen = 3; comp = COMP_RLE; }
    }
    p32(h, 0, type); p16(h, 4, comp); p16(h, 6, ITEM_HDR); p32(h, 8, outlen); p32(h, 12, ident);
    if (fseek(f->fp, 0, SEEK_END) != 0) return 0;
    return fwrite(h, 1, ITEM_HDR, f->fp) == ITEM_HDR && fwrite(out, 1, outlen, f->fp) == outlen;
}

/* orig 0xB73D + 0xAB9C: create image, write header and creator item */
static AprFile *apr_create(const char *path)
{
    AprFile *f = calloc(1, sizeof *f);
    uint8_t hdr[APR_HDR_LEN] = { 0 };
    if (!f) return NULL;
    f->fp = fopen(path, "w+b");
    if (!f->fp) { free(f); return NULL; }
    memcpy(hdr, APR_SIG, sizeof APR_SIG - 1);
    if (fwrite(hdr, 1, APR_HDR_LEN, f->fp) != APR_HDR_LEN ||
        !write_item(f, ITEM_CREATOR, 0, (const uint8_t *)CREATOR_TAG, sizeof CREATOR_TAG)) {
        fclose(f->fp); free(f); return NULL;
    }
    return f;
}

static AprFile *apr_open(const char *path, int writable)
{
    AprFile *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->fp = fopen(path, writable ? "r+b" : "rb");
    if (!f->fp) { free(f); return NULL; }
    return f;
}

static void apr_close(AprFile *f) { if (f) { if (f->fp) fclose(f->fp); free(f); } }

/* orig 0xAC60: find an item; try the cached "next item" position first, then
 * scan from the start.  Returns malloc'ed, decompressed data. */
static uint8_t *apr_find(AprFile *f, uint32_t type, uint32_t ident, unsigned *len, long *start)
{
    ItemHdr h; long pos = 0, next = 0; int found = 0; uint8_t *out;

    if (f->cached && read_item_hdr(f, f->cached, &h) && h.type == type && h.ident == ident &&
        h.hsize >= ITEM_HDR) { pos = f->cached; found = 1; }
    if (!found) {
        pos = APR_HDR_LEN;
        for (;;) {
            if (!read_item_hdr(f, pos, &h) || h.hsize < ITEM_HDR) { f->cached = 0; return NULL; }
            if (h.type == type && h.ident == ident) break;
            pos += h.hsize + (long)h.dsize;
        }
    }
    next = pos + h.hsize + (long)h.dsize;
    if (h.dsize > 0x100000) { f->cached = 0; return NULL; }       /* PORT: corruption guard */
    {
        uint8_t *raw = malloc(h.dsize ? h.dsize : 1);
        if (!raw) return NULL;
        if (h.dsize && (fseek(f->fp, pos + h.hsize, SEEK_SET) != 0 ||
                        fread(raw, 1, h.dsize, f->fp) != h.dsize)) {
            say("Unable to read record!\n"); free(raw); f->cached = 0; return NULL;
        }
        if (h.comp == COMP_RAW) { out = raw; *len = h.dsize; }
        else if (h.comp == COMP_RLE && h.dsize >= 3) {                /* orig 0xB0AF */
            unsigned n = raw[0] | (raw[1] << 8);
            out = malloc(n ? n : 1);
            if (!out) { free(raw); return NULL; }
            memset(out, raw[2], n); *len = n; free(raw);
        } else {
            say("\nError - unable to decompress compression type 0x%X\n", h.comp);
            free(raw); f->cached = 0; return NULL;
        }
    }
    f->cached = next;
    if (start) *start = pos;
    return out;
}

/* sector read (orig 0xB443): short items are zero padded, long ones truncated */
static int apr_read_sector(AprFile *f, const CHS *c, uint8_t *buf)
{
    unsigned len; uint8_t *d = apr_find(f, ITEM_SECTOR, chs_ident(c), &len, NULL);
    if (!d) return 0;
    memset(buf, 0, SECSZ); memcpy(buf, d, len < SECSZ ? len : SECSZ); free(d);
    return 1;
}

/* track read (orig 0xB57E): every sector must be present */
static int apr_read_track(AprFile *f, const CHS *c, uint8_t *buf, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        CHS t = *c; t.sector += i;
        if (!apr_read_sector(f, &t, buf + (size_t)i * SECSZ)) {
            say("Error - unable to read sector with mark %08X\n", chs_ident(&t));
            return 0;
        }
    }
    return 1;
}

static int apr_write_sector(AprFile *f, const CHS *c, const uint8_t *buf)
{ return write_item(f, ITEM_SECTOR, chs_ident(c), buf, SECSZ); }

static int apr_write_track(AprFile *f, const CHS *c, const uint8_t *buf, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        CHS t = *c; t.sector += i;
        if (!apr_write_sector(f, &t, buf + (size_t)i * SECSZ)) return 0;
    }
    return 1;
}

/* orig 0xB7C7 / 0xB82C: comment item (text incl. terminating NUL; lines end in CR) */
static int apr_set_comment(AprFile *f, const char *text)
{ return write_item(f, ITEM_COMMENT, 0, (const uint8_t *)text, strlen(text) + 1); }

static void apr_get_comment(AprFile *f, char *out, size_t max)
{
    unsigned len; uint8_t *d = apr_find(f, ITEM_COMMENT, 0, &len, NULL);
    if (!d) { out[0] = 0; return; }
    if (len >= max) len = max - 1;
    memcpy(out, d, len); out[len] = 0; free(d);
}

/* PORT (E command): replace the comment: mark the old item deleted, append new */
static int apr_replace_comment(AprFile *f, const char *text)
{
    unsigned len; long start; uint8_t *d;
    f->cached = 0;
    d = apr_find(f, ITEM_COMMENT, 0, &len, &start);
    if (d) {
        uint8_t t[4]; free(d);
        p32(t, 0, ITEM_DELETED);
        if (fseek(f->fp, start, SEEK_SET) != 0 || fwrite(t, 1, 4, f->fp) != 4) return 0;
        fflush(f->fp);
    }
    f->cached = 0;
    return apr_set_comment(f, text);
}

/* orig 0xB174 ("C - check file integrity"): item listing to screen and apridisk.dbg */
static void apr_dump(AprFile *f)
{
    uint8_t hdr[APR_HDR_LEN + 1], b[ITEM_HDR];
    FILE *o;
    say("\nAprifile debug\n");
    o = fopen(DBG_NAME, "wt");
    memset(hdr, 0, sizeof hdr);
    if (fseek(f->fp, 0, SEEK_SET) != 0 || fread(hdr, 1, APR_HDR_LEN, f->fp) != APR_HDR_LEN)
        say("Unable to read header!\n");
    hdr[APR_HDR_LEN] = 0;
    say("Header is [%s]\n", (char *)hdr);
    if (o) fprintf(o, "Header is [%s]\n", (char *)hdr);
    say("\n");
    if (o) fprintf(o, "\n");
    while (fread(b, 1, ITEM_HDR, f->fp) == ITEM_HDR) {
        unsigned hs = u16(b, 6); uint32_t ds = u32(b, 8);
        say("Item type %08lx  header size %d  size %3ld  compress %04x  ident %08lx\n",
            (unsigned long)u32(b, 0), hs, (long)ds, u16(b, 4), (unsigned long)u32(b, 12));
        if (o) fprintf(o, "Item type %08lx  header size %d  size %3ld  compress %04x  ident %08lx\n",
            (unsigned long)u32(b, 0), hs, (long)ds, u16(b, 4), (unsigned long)u32(b, 12));
        if (hs < ITEM_HDR || fseek(f->fp, (long)(hs + ds) - ITEM_HDR, SEEK_CUR) != 0) break;
    }
    say("End of debug\n");
    if (o) fclose(o);
}

/* ------------------------------------------------------- the two copy loops */
static int abort_key(void)             /* any keypress aborts, as in the DOS build */
{ if (kbhit()) { getch(); return 1; } return 0; }

/* R: floppy -> image (orig 0x8F0F) */
static int read_disk_to_file(Dev *dev, const char *name, const char *comment)
{
    uint8_t boot[SECSZ], sec[SECSZ], *trk;
    DiskFormat fmt; CHS c; AprFile *file; int unreadable = 0;

    say("Transferring floppy disk to file...\n");
    say("Checking boot sector for disk format...\n");
    if (!dev_read_boot(dev, boot)) { say("Unable to read boot sector\n"); return 0; }
    say("\n");
    if (!detect_format(boot, &fmt)) {
        say("Unable to determine Apricot disk format from boot sector\n"); return 0;
    }
    say("Copying this type:\n");
    describe_disk(boot, stdout);
    say("\n");
    dev_set_geometry(dev, &fmt);

    file = apr_create(name);
    if (!file) { say("\n[2] Failed to start new file %s\n", name); return 0; }

    say("Will copy %d heads, %d cylinders, %d sectors\n", fmt.heads, fmt.cylinders, fmt.spt);
    trk = malloc((size_t)fmt.spt * SECSZ);          /* NULL => sector-by-sector only */

    for (c.cyl = 0; c.cyl != fmt.cylinders; c.cyl++) {
        if (abort_key()) { apr_close(file); free(trk); return 0; }
        for (c.head = 0; c.head != fmt.heads; c.head++) {
            say("Copying Track %2d Head %1d\r", c.cyl, c.head);
            c.sector = 1;
            if (trk && dev_io(dev, &fmt, &c, trk, fmt.spt, 0)) {       /* fast path */
                c.sector = 1;
                if (!apr_write_track(file, &c, trk, fmt.spt)) {
                    say("\nFailed to write sectors to file %s\n", name);
                    apr_close(file); free(trk); return 0;
                }
                continue;
            }
            for (c.sector = 1; c.sector != fmt.spt + 1; c.sector++) {  /* slow path */
                say("Copying Track %2d Head %1d Sector %02d\r", c.cyl, c.head, c.sector);
                if (!dev_io(dev, &fmt, &c, sec, 1, 0)) {
                    say("\nFailed to read sector - ignoring and continuing\n");
                    unreadable++; continue;
                }
                if (!apr_write_sector(file, &c, sec)) {
                    say("\nFailed to write sector to file %s\n", name);
                    apr_close(file); free(trk); return 0;
                }
            }
        }
    }
    free(trk);
    apr_set_comment(file, comment ? comment : "");
    say("\n");
    apr_close(file);
    say("Done transferring floppy disk to file.\n");
    if (unreadable > 0)
        say("*** WARNING - %d sectors were unreadable and were therefore not written to the file ***\n",
            unreadable);
    return 1;
}

/* W: image -> floppy (orig 0x92E3) */
static int write_file_to_disk(const char *name, Dev *dev)
{
    uint8_t boot[SECSZ], sec[SECSZ], *trk;
    DiskFormat fmt; CHS c; AprFile *file; int missing = 0;

    say("Checking boot sector for disk format...\n");
    file = apr_open(name, 0);
    if (!file) { say("Unable to start opening file %s\n", name); return 0; }

    c.cyl = 0; c.head = 0; c.sector = 1;
    if (!apr_read_sector(file, &c, boot)) {
        say("Unable to read boot sector from file %s\n", name); apr_close(file); return 0;
    }
    say("\n");
    if (!detect_format(boot, &fmt)) {
        say("Unable to determine Apricot disk format from boot sector\n"); apr_close(file); return 0;
    }
    say("Copying this type:\n");
    describe_disk(boot, stdout);
    say("\n");
    dev_set_geometry(dev, &fmt);
    say("Will copy %d heads, %d cylinders, %d sectors\n", fmt.heads, fmt.cylinders, fmt.spt);
    trk = malloc((size_t)fmt.spt * SECSZ);

    for (c.cyl = 0; c.cyl != fmt.cylinders; c.cyl++) {
        if (abort_key()) { apr_close(file); free(trk); return 0; }
        for (c.head = 0; c.head != fmt.heads; c.head++) {
            say("Copying Track %2d Head %1d\r", c.cyl, c.head);
            c.sector = 1;
            if (trk && apr_read_track(file, &c, trk, fmt.spt)) {
                c.sector = 1;
                if (!dev_io(dev, &fmt, &c, trk, fmt.spt, 1)) {
                    say("\nFailed to write sectors to disk\n");
                    apr_close(file); free(trk); return 0;
                }
                continue;
            }
            for (c.sector = 1; c.sector != fmt.spt + 1; c.sector++) {
                say("Copying Track %2d Head %1d Sector %02d\r", c.cyl, c.head, c.sector);
                if (!apr_read_sector(file, &c, sec)) {
                    say("\nFailed to read sector from file\n"); missing++; continue;
                }
                if (!dev_io(dev, &fmt, &c, sec, 1, 1)) {
                    say("\nFailed to write sector to disk\n");
                    apr_close(file); free(trk); return 0;
                }
            }
        }
    }
    free(trk); apr_close(file);
    say("\n");
    if (missing > 0)
        say("** Warning, %d sectors were not available in file and were not written to disk.\n", missing);
    return 1;
}

/* I: information on the disk in the drive (orig 0x9A93). Returns 1 if copyable. */
static int show_drive_info(Dev *dev)
{
    uint8_t boot[SECSZ]; DiskFormat fmt;
    say("Checking boot sector for disk format...\n");
    if (!dev_read_boot(dev, boot)) { say("Unable to read boot sector\n"); return 0; }
    say("\n");
    if (!detect_format(boot, &fmt)) {
        say("Unable to determine Apricot disk format from boot sector\n");
        return 0;           /* PORT: original carried on and printed garbage */
    }
    say("Disk type is as follows:\n");
    describe_disk(boot, stdout);
    say("\n");
    return 1;
}

/* ------------------------------------------------------------- prompts */
/* orig 0x8C81: line editor.  multi!=0 => several lines, Enter twice ends.
 * Lines are separated by CR (0x0D) exactly as stored in the DOS images.
 * Returns NULL if ESC is pressed. */
static char *get_line(int multi)
{
    static char buf[1024];
    int lines = 1; size_t n;

    buf[0] = 0;
    if (multi) say("(Press Enter twice to complete)\n");
    say("-:");
    for (;;) {
        int c = getch();
        n = strlen(buf);
        if (c == 27) return NULL;
        if (c == '\r' || c == '\n') {
            if (!multi) { say("\n"); return buf; }
            if (n > 0) {
                if (buf[n - 1] == '\r') {                 /* second Enter: done */
                    say("\n");
                    snprintf(g_hist, sizeof g_hist, "%s", buf);
                    return buf;
                }
                lines++;
            }
            say("\n :");
            if (n + 1 < sizeof buf) { buf[n] = '\r'; buf[n + 1] = 0; }
        } else if (c == 8 || c == 127) {
            if (n > 0 && buf[n - 1] != '\r') { say("\b \b"); buf[n - 1] = 0; }
            else say("\a");
        } else if (c == KEY_UP) {                          /* recall same line of last comment */
            char *p = g_hist, *q; int k;
            for (k = 0; p && k < lines - 1; k++) { p = strchr(p, '\r'); if (p) p++; }
            if (!p) continue;
            q = strchr(p, '\r');
            { size_t add = q ? (size_t)(q - p) : strlen(p);
              if (n + add < sizeof buf - 1) {
                  fwrite(p, 1, add, stdout); fflush(stdout);
                  memcpy(buf + n, p, add); buf[n + add] = 0;
              } }
        } else if (c >= 1 && c < 256) {
            if (n + 1 < sizeof buf - 1) { say("%c", c); buf[n] = (char)c; buf[n + 1] = 0; }
            else say("\a");
        }
    }
}

/* orig 0x9BA6: build the image path from what the user typed */
static char *make_filename(const char *in)
{
    char name[PATH_MAX], tmp[PATH_MAX];
    if (!in) return NULL;
    snprintf(name, sizeof name, "%s", in);
    if (g_numbered && !name[0]) snprintf(name, sizeof name, "%ld", g_nextDisk);
    /* Orig quirk kept: strchr("0123456789", c) also matches c == 0, so an empty
     * name becomes "APR00000" in non-numbered mode. */
    if (!name[0] || isdigit((unsigned char)name[0])) {
        int z = strlen(name) < 5 ? 5 - (int)strlen(name) : 0;
        snprintf(tmp, sizeof tmp, "APR%.*s%s", z, "00000", name);
        snprintf(name, sizeof name, "%s", tmp);
    }
    if (!strchr(name, '.')) strncat(name, ".DSK", sizeof name - strlen(name) - 1);
    if (!strchr(name, '/')) {
        snprintf(tmp, sizeof tmp, "%s/%s", g_dir, name);
        snprintf(name, sizeof name, "%s", tmp);
    }
    return strdup(name);
}

static char *prompt_filename(void)             /* orig 0x9D33 */
{
    char *l;
    say("\nEnter filename:\n");
    l = get_line(0);
    return l ? make_filename(l) : NULL;
}

static char *prompt_comment(void)              /* orig 0x9DD0 */
{
    char *l;
    say("\nEnter disk label or comment:\n");
    l = get_line(1);
    return l ? strdup(l) : NULL;
}

static int file_exists(const char *p) { return access(p, F_OK) == 0; }

static void ensure_dir_of(const char *path)    /* PORT: create ./disks on first use */
{
    char d[PATH_MAX], *s;
    snprintf(d, sizeof d, "%s", path);
    s = strrchr(d, '/');
    if (s && s != d) { *s = 0; mkdir(d, 0777); }
}

/* ------------------------------------------------------------ ini file */
static void load_ini(void)                     /* orig 0x991E */
{
    FILE *f = fopen(INI_NAME, "r");
    char dir[PATH_MAX]; long n; char *p;
    if (f) {
        if (fscanf(f, "%4095s\n%ld\n", dir, &n) == 2) {
            for (p = dir; *p; p++) if (*p == '\\') *p = '/';     /* PORT: DOS ini files */
            snprintf(g_dir, sizeof g_dir, "%s", dir); g_nextDisk = n;
        }
        say("\nDefault directory is <%s>\nand next apricot disk is numbered <%ld>\n",
            g_dir, g_nextDisk);
        fclose(f);
    } else {
        say("No initialisation file - using defaults\n");
        snprintf(g_dir, sizeof g_dir, "./disks");                /* orig: .\disks */
        g_nextDisk = 1;
    }
}

static void save_ini(void)                     /* orig 0x99A5 */
{
    FILE *f = fopen(INI_NAME, "w");
    if (!f) { say("Unable to write ini file\n"); return; }
    fprintf(f, "%s\n%ld\n", g_dir, g_nextDisk);
    fclose(f);
}

/* ------------------------------------------------------- menu actions */
static void do_read(Dev *dev, char *file, char *comment, int *ok)
{
    ensure_dir_of(file);
    say("Destination file is %s\n", file);
    *ok = read_disk_to_file(dev, file, comment);
    if (*ok == 1) say("\a");
    else if (remove(file) != 0 && file_exists(file))
        say("Aborted/failed but unable to delete file %s\n", file);
}

static int confirm_overwrite(const char *file)
{
    int c;
    if (!file_exists(file)) return 1;
    say("%s\n already exists. Do you want to overwrite it? (Y/N)\n", file);
    say("-:");
    do c = getch(); while (!strchr("YyNn", c) && c != 27);
    say("%c\n", c);
    return strchr("Nn", c) == NULL && c != 27;
}

static void cmd_read(Dev *dev)                 /* R: orig 0x9E38 */
{
    char *file = prompt_filename(), *comment; int ok;
    if (!file) return;
    if (!confirm_overwrite(file)) { free(file); return; }
    comment = prompt_comment();
    if (!comment) { free(file); return; }
    if (dev_open(dev, 0)) { do_read(dev, file, comment, &ok); dev_close(dev); }
    free(comment); free(file);
}

static void cmd_write(Dev *dev)                /* W: orig 0x9F75 */
{
    char *file = prompt_filename();
    if (!file) return;
    say("Transfer from file %s to disk...\n", file);
    if (dev_open(dev, 1)) { write_file_to_disk(file, dev); dev_close(dev); }
    free(file);
}

static void cmd_info(Dev *dev)                 /* I: orig 0x9FD9 */
{
    int ok = 0;
    if (dev_open(dev, 0)) { ok = show_drive_info(dev); dev_close(dev); }
    say(ok ? "(Should be able to copy this disk)\n" : "(Will be unable to copy this disk)\n");
}

static void cmd_check(void)                    /* C: orig 0xA007 */
{
    char *file = prompt_filename(); AprFile *f;
    if (!file) return;
    f = apr_open(file, 0);
    if (!f) say("File doesn't exists or could not be opened\n");
    else { apr_dump(f); apr_close(f); }
    free(file);
}

static void print_comment(const char *c)       /* CR-separated lines -> real lines */
{
    for (; *c; c++) putchar(*c == '\r' ? '\n' : *c);
    putchar('\n');
}

/* PORT: E, L and M are empty stubs in the 2004 DOS build; small versions here. */
static void cmd_edit(void)
{
    char *file = prompt_filename(), *c; AprFile *f; char old[1024];
    if (!file) return;
    f = apr_open(file, 1);
    if (!f) { say("File doesn't exists or could not be opened\n"); free(file); return; }
    apr_get_comment(f, old, sizeof old);
    say("Current comment:\n"); print_comment(old);
    c = prompt_comment();
    if (c) { if (!apr_replace_comment(f, c)) say("Unable to update comment\n"); free(c); }
    apr_close(f); free(file);
}

static int is_image_name(const struct dirent *e)
{
    const char *x = strrchr(e->d_name, '.');
    return x && (!strcasecmp(x, ".dsk") || !strcasecmp(x, ".apr"));
}

static void cmd_list(void)
{
    struct dirent **l; int n, i;
    n = scandir(g_dir, &l, is_image_name, alphasort);
    if (n < 0) { say("Unable to read directory %s\n", g_dir); return; }
    for (i = 0; i < n; i++) {
        char path[PATH_MAX], cm[256]; AprFile *f; char *r;
        snprintf(path, sizeof path, "%s/%s", g_dir, l[i]->d_name);
        cm[0] = 0;
        if ((f = apr_open(path, 0))) { apr_get_comment(f, cm, sizeof cm); apr_close(f); }
        if ((r = strchr(cm, '\r'))) *r = 0;
        say("%-16s %s\n", l[i]->d_name, cm);
        free(l[i]);
    }
    free(l);
}

static void cmd_multi(Dev *dev)
{
    for (;;) {
        char *file, *comment; int ok, c;
        say("\nInsert disk %05ld and press a key (ESC to stop)\n", g_nextDisk);
        c = getch(); if (c == 27) return;
        file = make_filename("");
        if (!confirm_overwrite(file)) { free(file); return; }
        comment = prompt_comment();
        if (!comment) { free(file); return; }
        ok = 0;
        if (dev_open(dev, 0)) { do_read(dev, file, comment, &ok); dev_close(dev); }
        free(comment); free(file);
        if (!ok) return;
        g_nextDisk++; save_ini();
    }
}

/* ------------------------------------------------------------- main */
static void rule(void)
{ say("------------------------------------------------------------\n"); }

static int disk_exists(long n)                 /* orig 0x9D6D */
{
    char num[32], *p; int r;
    snprintf(num, sizeof num, "%ld", n);
    p = make_filename(num);
    r = p && file_exists(p); free(p); return r;
}

static void usage(const char *a0)
{
    fprintf(stderr,
      "usage: %s [-d DEVICE] [-n] [COMMAND]\n"
      "  -d DEVICE   floppy/raw device or image file (default /dev/fd0)\n"
      "  -n | /n     numbered-disk mode (enables E L M B F)\n"
      "No COMMAND: interactive menu.  Commands (non-interactive):\n"
      "  read  FILE [COMMENT]   floppy -> image\n"
      "  write FILE             image  -> floppy (disk must already be formatted)\n"
      "  info                   describe the disk in the drive\n"
      "  check FILE             list the items in an image (also writes %s)\n", a0, DBG_NAME);
}

static int run_command(Dev *dev, int argc, char **argv)
{
    int ok = 0;
    if (!strcmp(argv[0], "read") && argc >= 2) {
        if (dev_open(dev, 0)) {
            ensure_dir_of(argv[1]);
            ok = read_disk_to_file(dev, argv[1], argc > 2 ? argv[2] : "");
            if (!ok) remove(argv[1]);
            dev_close(dev);
        }
    } else if (!strcmp(argv[0], "write") && argc == 2) {
        if (dev_open(dev, 1)) { ok = write_file_to_disk(argv[1], dev); dev_close(dev); }
    } else if (!strcmp(argv[0], "info") && argc == 1) {
        if (dev_open(dev, 0)) { ok = show_drive_info(dev); dev_close(dev); }
    } else if (!strcmp(argv[0], "check") && argc == 2) {
        AprFile *f = apr_open(argv[1], 0);
        if (!f) say("File doesn't exists or could not be opened\n");
        else { apr_dump(f); apr_close(f); ok = 1; }
    } else { usage("apridisk"); return 2; }
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    Dev dev = { -1, 0, 0 };
    int i, rc = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) snprintf(g_device, sizeof g_device, "%s", argv[++i]);
        else if (!strcmp(argv[i], "-n") || !strcasecmp(argv[i], "/n")) g_numbered = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else break;
    }

    g_istty = isatty(0);
    if (g_istty) {
        tcgetattr(0, &g_tio); atexit(term_restore);
        signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
        term_raw();
    }

    if (i < argc) { rc = run_command(&dev, argc - i, argv + i); return rc; }

    say("\n** APRIDISK Apricot DISK utility (this version built on %s)\n", PROG_DATE);
    say("** (c) Copyright 1998-2001 Jonathan Marsters. All rights reserved.\n");
    say("** Linux port; device %s\n\n", g_device);
    load_ini();
    say("\n\n\n");

    for (;;) {
        if (g_numbered)
            say("========================================next=%05ld==%s===\n", g_nextDisk,
                disk_exists(g_nextDisk) ? "EXISTS" : "======");
        else
            say("============================================================\n");
        say("Apricot floppy disk filing system\n\n");
        say("  R Read one floppy to file\n");
        say("  W Write one floppy from file\n");
        say("  I Information on disk in drive\n\n");
        if (g_numbered) {
            say("  E Edit disk comment\n");
            say("  L List disk files\n");
            say("  M Multiple floppies to files\n");
            say("  B go Back one disk number\n");
            say("  F go Forward one disk number\n\n");
        }
        say("  C Check file integrity\n");
        say("  Q Quit\n");
        say("?:");
        {
            int c = getch();
            say("%c\n", c >= 32 && c < 127 ? c : ' ');
            rule();
            switch (toupper(c)) {
            case 'M': if (g_numbered) cmd_multi(&dev);  break;
            case 'R': cmd_read(&dev);                   break;
            case 'W': cmd_write(&dev);                  break;
            case 'E': if (g_numbered) cmd_edit();       break;
            case 'L': if (g_numbered) cmd_list();       break;
            case 'B': if (g_numbered) { g_nextDisk--; save_ini(); } break;
            case 'F': if (g_numbered) { g_nextDisk++; save_ini(); } break;
            case 'I': cmd_info(&dev);                   break;
            case 'C': cmd_check();                      break;
            case 27: case 'Q': return 0;
            default:  say("\a");
            }
        }
    }
}
