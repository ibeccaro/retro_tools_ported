// ============================================================================
// apridisk_reconstructed.cpp
//
// Hand-reconstructed C++ for APRIDISK.EXE ("Apricot DISK utility",
// build string "Oct  7 2004", (c) 1998-2001 Jonathan Marsters per the banner).
//
// Original toolchain: Borland C++ 3.x/4.x (1994 runtime), 16-bit DOS, large
// model (far code + far data), Borland <cstring.h> `string`, BIOS INT 13h I/O.
//
// This is NOT the original source. It is rebuilt from the disassembly:
//   * Names are invented. Control flow, constants, offsets and every message
//     string are taken from the binary.
//   * Each routine is tagged with the address it came from and a confidence:
//        [TRACED]   read instruction by instruction, logic is faithful
//        [OUTLINE]  structure and strings known, inner details approximated
//   * Original vtable layouts differ between the two I/O classes (see below);
//     here they are ordinary virtual methods.
//
// Written for study/interoperability of a program you already have.
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#ifdef __BORLANDC__
#  include <conio.h>
#  include <dos.h>
#endif

#ifndef __BORLANDC__        // lets g++ -fsyntax-only chew on this file
#  define far
#  define stricmp strcasecmp
#  include <strings.h>
   static int kbhit(){return 0;} static int getch(){return 0;}
   union REGS { struct { int ax; } x; struct { unsigned char al, ah, dl; } h; };
   static void int86(int, REGS*, REGS*) {}
#endif

#define CREATOR_TAG "APRIDISK " __DATE__          // "APRIDISK Oct  7 2004"

// ----------------------------------------------------------------------------
// Globals (data segment offsets from the binary)
// ----------------------------------------------------------------------------
static char  g_dir[80];           // ds:27EE  default directory for image files
static long  g_nextDisk = 1;      // ds:283E  next apricot disk number
static int   g_numbered = 0;      // ds:0AF4  set by "/n" on command line

// ----------------------------------------------------------------------------
// Basic types
// ----------------------------------------------------------------------------
struct CHS { int cyl, head, sector; };          // lives at [bp-6],[bp-4],[bp-2]
                                                // sectors are 1-based

// 8-byte helper object built by the ctor at 0xC249 / freed by dtor at 0xC2E8.
// Offset +2 holds the far data pointer (accessed as es:[bx+2] everywhere).
class Buffer {
public:
    Buffer(unsigned bytes)            // 0xC249  (always called with 0x200)
        : size(bytes), data((unsigned char far *)farmalloc_(bytes)) {}
    ~Buffer()                         // 0xC2E8
    { if (data) farfree_(data); }
    unsigned       size;
    unsigned char far *data;
private:
    static void far *farmalloc_(unsigned n) { return malloc(n); }
    static void      farfree_(void far *p)  { free(p);          }
};

// Multi-sector buffer: 0xC295 is called as (NULL, 0x200, sectorsPerTrack)
// and returns NULL on allocation failure -> caller falls back to 1 sector
// at a time.
class TrackBuffer {
public:
    static TrackBuffer *make(unsigned secSize, unsigned nSecs)
    { TrackBuffer *t = new TrackBuffer; t->mem = malloc((size_t)secSize*nSecs);
      if (!t->mem) { delete t; return 0; } return t; }
    ~TrackBuffer() { free(mem); }
    void *mem;
private:
    TrackBuffer() : mem(0) {}
};

// Disk geometry record. The classifier fills a single static instance at
// ds:28B2 and returns a pointer to it (or NULL).
struct DiskFormat {                 // ds:28B2 ..
    int type;                       // +0  0,1,2 = Apricot formats, 3 = DOS/BPB
    int bytesPerSector;             // +2
    int cylinders;                  // +4
    int heads;                      // +6
    int sectorsPerTrack;            // +8
    int flag;                       // +A  (see classify: 0 if 9 spt / Apricot)
};
static DiskFormat g_fmt;


// Routines whose bodies are outside the traced set (see OUTLINE notes below)
static void selectDrive(const char *spec);   // 0xA832 (purpose inferred)
static void initRuntime();                   // 0x976:7
static void shutdownRuntime();               // 0x976:0x7F
static int  diskExists(long n);              // 0x9D6D
static void multipleToFiles(), readOne(), writeOne(), editComment(),
            listFiles(), infoOne(), checkFile();

// ----------------------------------------------------------------------------
// Console helpers
// ----------------------------------------------------------------------------
// 0x8ECE [TRACED]: printf into a 1000-byte stack buffer, then puts-style out.
static int say(const char *fmt, ...);               // wraps vsprintf + 0x266E

// 0x9B7F [TRACED]
static int escPressed()
{
    if (kbhit() && getch() == 0x1B) return 1;
    return 0;
}

// 0xA077 [TRACED]
static void rule() { puts("------------------------------------------------------------"); }

// 0x8C81 [OUTLINE]: line editor used for "Enter filename" / "disk comment".
// Arg selects multi-line mode, which first prints
// "(Press Enter twice to complete)\n". Key switch handles Enter, Backspace,
// ESC (returns NULL) and an extended-key (0x48 = Up) recall path.
static char *getLine(int multiLine);

// ----------------------------------------------------------------------------
// Disk-format detection
// ----------------------------------------------------------------------------

// 0xBF94 [TRACED]: classify a 512-byte boot sector.
//   0,1,2 -> Apricot native formats
//   3     -> MS-DOS BPB media (descriptor 0xF0 / 0xF9)
//   6     -> "other, looks like a valid BPB-less sector"
//   7     -> unknown / unsupported
static int classifyBootSector(const Buffer &b)
{
    const unsigned char far *s = b.data;
    unsigned char media = s[0x15];          // BPB media descriptor
    unsigned char a = s[0x5A];
    unsigned char c = s[0x5D];

    if (media == 0xF0 || media == 0xF9) return 3;
    if (media >= 0xF8 && media <= 0xFF) return 7;       // other DOS media: no

    if (a == 0xFC && c == 0) return 0;
    if (a == 0xFD && c == 1) return 1;
    if (a == 0xFE && c == 2) return 2;
    if (a == 0xF8 && c == 3) return 7;
    if (a == 0xF9 && c == 4) return 7;
    if (a == 0xFA && c == 5) return 7;

    if (s[0x0B] == 0)                    return 7;
    if (s[0x0D] != 0)                    return 7;
    if ((s[0x18] | (s[0x19] << 8)) != 0x200) return 7;
    if (s[0x08] == 0)                    return 7;
    return 6;
}

// 0xC0BC [TRACED]: fills g_fmt from the boot sector, returns &g_fmt or NULL.
static DiskFormat *detectFormat(const Buffer &b)
{
    const unsigned char far *s = b.data;
    int t = classifyBootSector(b);
    if (t == 7) return 0;

    g_fmt.type = t;
    if (t == 3) {                                       // DOS BIOS parameter block
        int bps   = s[0x0B] | (s[0x0C] << 8);
        int total = s[0x13] | (s[0x14] << 8);
        int spt   = s[0x18] | (s[0x19] << 8);
        int heads = s[0x1A] | (s[0x1B] << 8);
        g_fmt.bytesPerSector  = bps;
        g_fmt.cylinders       = (total / spt) / heads;
        g_fmt.heads           = heads;
        g_fmt.sectorsPerTrack = spt;
        g_fmt.flag            = (spt == 9) ? 0 : 1;
    } else {                                            // Apricot native
        g_fmt.bytesPerSector  = 0x200;
        g_fmt.cylinders       = s[0x12] | (s[0x13] << 8);
        g_fmt.heads           = s[0x16];
        g_fmt.sectorsPerTrack = s[0x10] | (s[0x11] << 8);
        g_fmt.flag            = 0;
    }
    return &g_fmt;
}

// 0xB963 [OUTLINE]: prints the disk-information block. Strings recovered:
//   " [Apricot data disk]" / " [Apricot boot disk]" / " [Microsoft data disk]"
//   " Operating system:       " Invalid | MS-DOS | p-System | CP/M 86 |
//                               Concurrent CP/M | Unknown (0x%02x)
//   " Software write protect: ", " Copy protect:           "
//   " Boot disk type:         " non-bootable | Apricot and Xi RAM BIOS |
//        Apricot and Xi ROM BIOS | Apricot Portable ROM BIOS |
//        Apricot F1 ROM BIOS | Apricot Xen ROM BIOS
//   " Number of heads:      %3d", " Bytes per sector =      %d",
//   " Total sectors:         %4d",
//   " Suitable modern media for this disk is "
static void describeDisk(const Buffer &boot, const char *indent);

// ----------------------------------------------------------------------------
// BIOS INT 13h layer
// ----------------------------------------------------------------------------

// 0xA51A [TRACED]: builds a register block from a transfer request and calls
// int86x(0x13,...). AX=(cmd<<8)|count, CX=(cyl<<8)|sector, DX=(head<<8)|drive,
// BX/ES = buffer far pointer. Returns 1 when carry is clear, and stores AH
// (status) and AL back into the request.
struct BiosReq { int cyl, head, sector, count, off, seg; unsigned char ah; int al; };
static int biosDisk(int cmd, int drive, BiosReq far *r);

// 0xA5A1 [TRACED]: INT 13h AH=01 (get last status) and, if non-zero, prints
// "\n** PC BIOS Error: %02X\n".
static void reportBiosError(int drive)
{
    union REGS r;  r.x.ax = 0x0100;  r.h.dl = (unsigned char)drive;
    int86(0x13, &r, &r);
    if (r.h.al) say("\n** PC BIOS Error: %02X\n", r.h.al);   // original keeps AX&0xFF
}

// ----------------------------------------------------------------------------
// Device abstractions used by the copy loops
//
// In the binary the floppy object has its vptr at +0 and the file object at
// +2, and the two vtables are laid out differently:
//
//    FloppyDrive vtable:  +0 prepare()  +4 readSector  +8 writeSector
//                         +C readTrack  +10 writeTrack
//    AprFile    vtable:   +0 readSector +4 writeSector +8 readTrack
//                         +C writeTrack +10 beginWrite
//
// All I/O methods return 1 on success.
// ----------------------------------------------------------------------------
class FloppyDrive {
public:
    virtual void prepare() = 0;
    virtual int  readSector (CHS far *, Buffer far *)               = 0;
    virtual int  writeSector(CHS far *, Buffer far *)               = 0;
    virtual int  readTrack  (CHS far *, void far *, int nSectors)   = 0;
    virtual int  writeTrack (CHS far *, void far *, int nSectors)   = 0;
};

// .APR image file. On disk (confirmed from the binary's strings/constants):
//   header   "ACT Apricot disk image\x1A\x04"
//   items    { type:32, compression:16, headerSize:16, dataSize:32, ... }
//            type constants of the form 0xE31D000n
//            compression tag 0x9E90 is checked/written for packed sectors
// The parser lives at 0xAB4D (debug dump: "Item type %08lx  header size %d
// size %3ld  compress %04x  ident %08lx", "Header is [%s]").
class AprFile {
public:
    AprFile(int mode, const char *name, const char *creator);  // 0xB2EC
    virtual ~AprFile();                                         // 0xB3D2
    virtual int  readSector (CHS far *, Buffer far *)            ;
    virtual int  writeSector(CHS far *, Buffer far *)            ;
    virtual int  readTrack  (CHS far *, void far *, int nSectors);
    virtual int  writeTrack (CHS far *, void far *, int nSectors);
    virtual void beginWrite()                                    ;
    int  isOpen() const;                                        // 0xB3BB
    void setComment(const char *text);                          // 0xB7E7
    // error text on a bad sector record: "Error - unable to read sector with
    // mark %08X"; "Unable to read record!"; "Error - unable to decompress"
};

// ----------------------------------------------------------------------------
// R: Read one floppy to file                      0x8F0F  [TRACED]
// ----------------------------------------------------------------------------
int readDiskToFile(FloppyDrive far *drive, const char far *fileName,
                   const char far *comment)
{
    Buffer boot(0x200), sec(0x200);
    CHS     chs;
    AprFile *file = 0;
    int     unreadable = 0;

    say("Transferring floppy disk to file...\n");
    selectDrive("");                                     // 0xA832

    say("Checking boot sector for disk format...\n");
    chs.cyl = 0; chs.head = 0; chs.sector = 1;

    if (!drive->readSector(&chs, &boot)) {
        say("Unable to read boot sector\n");
        return 0;
    }
    say("");                                             // 0x51D, blank
    DiskFormat *fmt = detectFormat(boot);                // 0xC0BC
    if (!fmt) {
        say("Unable to determine Apricot disk format from boot sector\n");
        return 0;
    }

    say("Copying this type:\n");
    describeDisk(boot, "");                              // 0xB963

    say("");
    file = new AprFile(0, fileName, CREATOR_TAG);        // 0xB2EC
    if (!file) {
        say("Null!\n");
        say("\n[1] Failed to start new file %s\n", fileName);
        return 0;
    }
    file->beginWrite();                                  // vt+0x10
    if (!file->isOpen()) {                               // 0xB3BB
        say("\n[2] Failed to start new file %s\n", fileName);
        return 0;
    }

    say("Will copy %d heads, %d cylinders, %d sectors\n",
        fmt->heads, fmt->cylinders, fmt->sectorsPerTrack);

    for (chs.cyl = 0; chs.cyl != fmt->cylinders; chs.cyl++) {
        if (kbhit()) {                                   // any key aborts
            getch();
            delete file;                                 // partial file closed
            return 0;
        }
        for (chs.head = 0; chs.head != fmt->heads; chs.head++) {
            say("\rCopying Track %2d Head %1d", chs.cyl, chs.head);

            TrackBuffer *trk = TrackBuffer::make(0x200, fmt->sectorsPerTrack);
            chs.sector = 1;

            if (trk) {
                // fast path: whole track in one BIOS call
                if (drive->readTrack(&chs, trk->mem, fmt->sectorsPerTrack) == 1) {
                    chs.sector = 1;
                    if (file->writeTrack(&chs, trk->mem, fmt->sectorsPerTrack) != 1) {
                        say("\nFailed to write sectors to file %s\n", fileName);
                        delete file; delete trk;
                        return 0;
                    }
                    delete trk;
                    continue;                            // next head
                }
                delete trk;                              // track read failed ->
                trk = 0;                                 // retry sector by sector
            }

            // slow path: one sector at a time, tolerate bad sectors
            for (chs.sector = 1; chs.sector != fmt->sectorsPerTrack + 1; chs.sector++) {
                say("\rCopying Track %2d Head %1d Sector %02d",
                    chs.cyl, chs.head, chs.sector);
                if (drive->readSector(&chs, &sec) != 1) {
                    say("\nFailed to read sector - ignoring and continuing\n");
                    unreadable++;
                    continue;
                }
                if (file->writeSector(&chs, &sec) != 1) {
                    say("\nFailed to write sector to file %s\n", fileName);
                    delete file;
                    return 0;
                }
            }
        }
    }

    file->setComment(comment);                           // 0xB7E7 (via 0xB2E:4E7)
    say("\n");
    delete file;                                         // flush + close
    say("Done transferring floppy disk to file.\n");
    if (unreadable > 0)
        say("*** WARNING - %d sectors were unreadable and were therefore "
            "not written to the file ***\n", unreadable);
    return 1;
}

// ----------------------------------------------------------------------------
// W: Write one floppy from file                   0x92E3  [TRACED]
// Mirror image of the above; note the argument order is (file, drive).
// ----------------------------------------------------------------------------
int writeFileToDisk(const char far *fileName, FloppyDrive far *drive)
{
    Buffer boot(0x200), sec(0x200);
    CHS     chs;
    int     missing = 0;

    say("Checking boot sector for disk format...\n");
    AprFile *file = new AprFile(0, fileName, CREATOR_TAG);
    if (!file) say("Unable to start opening file %s\n", fileName);   // original
                                                                      // falls through
    drive->prepare();                                    // vt+0

    chs.cyl = 0; chs.head = 0; chs.sector = 1;
    if (!file->readSector(&chs, &boot)) {                // boot sector from file
        say("Unable to read boot sector from file %s\n", fileName);
        return 0;
    }
    say("");
    DiskFormat *fmt = detectFormat(boot);
    if (!fmt) {
        say("Unable to determine Apricot disk format from boot sector\n");
        return 0;
    }
    describeDisk(boot, "");
    say("");
    say("Will copy %d heads, %d cylinders, %d sectors\n",
        fmt->heads, fmt->cylinders, fmt->sectorsPerTrack);

    for (chs.cyl = 0; chs.cyl != fmt->cylinders; chs.cyl++) {
        if (kbhit()) { getch(); return 0; }
        for (chs.head = 0; chs.head != fmt->heads; chs.head++) {
            chs.sector = 1;
            say("\rCopying Track %2d Head %1d", chs.cyl, chs.head);

            TrackBuffer *trk = TrackBuffer::make(0x200, fmt->sectorsPerTrack);
            if (trk) {
                chs.sector = 1;
                if (file->readTrack(&chs, trk->mem, fmt->sectorsPerTrack) == 1) {
                    chs.sector = 1;
                    if (drive->writeTrack(&chs, trk->mem, fmt->sectorsPerTrack) != 1) {
                        say("\nFailed to write sectors to disk\n");
                        delete file; delete trk;
                        return 0;
                    }
                    delete trk;
                    continue;
                }
                delete trk;                              // not in file as a run:
            }                                            // go sector by sector

            for (chs.sector = 1; chs.sector != fmt->sectorsPerTrack + 1; chs.sector++) {
                say("\rCopying Track %2d Head %1d Sector %02d",
                    chs.cyl, chs.head, chs.sector);
                if (file->readSector(&chs, &sec) != 1) {
                    say("\nFailed to read sector from file\n");
                    missing++;                           // sector absent in image
                    continue;
                }
                if (drive->writeSector(&chs, &sec) != 1) {
                    say("\nFailed to write sector to disk\n");
                    return 0;
                }
            }
        }
    }
    say("");
    if (missing > 0)
        say("** Warning, %d sectors were not available in file and were not "
            "written to disk.\n", missing);
    return 1;
}

// ----------------------------------------------------------------------------
// apridisk.ini                                    0x991E / 0x99A5  [TRACED]
// Two lines: default directory, then next disk number.
// ----------------------------------------------------------------------------
static void loadIni()
{
    FILE *f = fopen("apridisk.ini", "r");
    if (f) {
        fscanf(f, "%s\n%ld\n", g_dir, &g_nextDisk);
        say("\nDefault directory is <%s>\nand next apricot disk is "
            "numbered <%ld>\n", g_dir, g_nextDisk);
        fclose(f);
    } else {
        say("No initialisation file - using defaults\n");
        strcpy(g_dir, ".\\disks");
        g_nextDisk = 1;
    }
}

static void saveIni()
{
    FILE *f = fopen("apridisk.ini", "w");
    if (f) { fprintf(f, "%s\n%ld\n", g_dir, g_nextDisk); fclose(f); }
    else   say("Unable to write ini file\n");
}

// 0x99FC [TRACED]: debugging hex dump, 10 rows x 16 bytes: "%04x " offset,
// then "%02x " per byte, then the bytes as "%c" (control chars -> space).
static void hexDump(const unsigned char far *p);

// 0x9A93 [TRACED]: "I - Information on disk in drive".
// Reads cyl0/head0/sector1 through the BIOS layer into a Buffer, then:
//   "Checking boot sector for disk format..."  / "Unable to read boot sector"
//   detectFormat() or "Unable to determine Apricot disk format..."
//   "Disk type is as follows:\n" + describeDisk()
static int showDriveInfo();

// ----------------------------------------------------------------------------
// Remaining menu actions [OUTLINE] -- strings and call structure known
// ----------------------------------------------------------------------------
// 0x9E38  R: prompts "\nEnter filename:\n" and "\nEnter disk comment:\n"
//         (or auto-numbers when /n), "Destination file is %s\n", then calls
//         readDiskToFile(). On failure tries remove() and, if that fails,
//         "Aborted/failed but unable to delete file %s\n".
// 0x9F75  W: "Transfer from file %s to disk...\n", then writeFileToDisk().
//         Also checks for existing target: "%s\n already exists. Do you want
//         to ..." (0x9E33 family).
// 0x9FD9  I: showDriveInfo(); with /n also prints "(Will be unable to copy
//         this disk)" or "(Should be able to copy this disk)".
// 0x9FBB / 0x9FCA  B / F: decrement / increment g_nextDisk (/n mode only).
// 0x9FF8  E: edit disk comment      0x9FFD  L: list disk files (/n only)
// 0xA007  C: check file integrity (runs the AprFile parser, debug output goes
//         to "apridisk.dbg": "\nAprifile debug\n" ... "End of debug\n")
// 0x9E33  M: multiple floppies -> files, loop over R with auto numbering.

// ----------------------------------------------------------------------------
// main menu                                       0xA088  [TRACED]
// Key table (21 entries, upper+lower case): R W I E L M B F C Q and ESC.
// E, L, M, B, F are only live when "/n" was given (g_numbered != 0).
// ----------------------------------------------------------------------------
int main(int argc, char **argv)
{
    initRuntime();                                       // 0x976:7
    if (argc == 2 && stricmp(argv[1], "/n") == 0)
        g_numbered = 1;

    printf("\n");
    printf("** APRIDISK Apricot DISK utility (this version built on %s)\n",
           __DATE__);
    printf("** (c) Copyright 1998-2001 Jonathan Marsters. All rights reserved.\n");
    printf("\n");
    loadIni();                                           // 0x991:0xE -> 0x991E
    printf("\n\n\n");

    for (;;) {
        if (g_numbered)
            printf("========================================next=%05ld==%s===\n",
                   g_nextDisk, diskExists(g_nextDisk) ? "EXISTS" : "======");
        else
            printf("============================================================\n");

        printf("Apricot floppy disk filing system\n\n");
        printf("  R Read one floppy to file\n");
        printf("  W Write one floppy from file\n");
        printf("  I Information on disk in drive\n\n");
        if (g_numbered) {
            printf("  E Edit disk comment\n");
            printf("  L List disk files\n");
            printf("  M Multiple floppies to files\n");
            printf("  B go Back one disk number\n");
            printf("  F go Forward one disk number\n\n");
        }
        printf("  C Check file integrity\n");
        printf("  Q Quit\n");
        printf("?:");

        int c = getch();
        printf("%c\n", c);
        rule();

        switch (toupper(c)) {
        case 'M': if (g_numbered) multipleToFiles();  break;   // 0x9E33
        case 'R': readOne();                           break;   // 0x9E38
        case 'W': writeOne();                          break;   // 0x9F75
        case 'E': if (g_numbered) editComment();       break;   // 0x9FF8
        case 'L': if (g_numbered) listFiles();         break;   // 0x9FFD
        case 'B': if (g_numbered) g_nextDisk--;        break;   // 0x9FBB
        case 'F': if (g_numbered) g_nextDisk++;        break;   // 0x9FCA
        case 'I': infoOne();                           break;   // 0x9FD9
        case 'C': checkFile();                         break;   // 0xA007
        case 27:
        case 'Q': shutdownRuntime(); exit(0);                   // 0x976:0x7F
        default:  putchar('\a');                                // bell on junk
        }
    }
}
