#!/usr/bin/env python3
"""Find PsyQ BIOS call stubs (li t2,0xA0|0xB0|0xC0 ; jr t2 ; li t1,N) in main.bin and print splat symbols."""
import os, struct
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VRAM = 0x801F0000
# psx-spx BIOS function names (PsyQ wrapper names where they differ in common use)
A = {0x00:"open",0x01:"lseek",0x02:"read",0x03:"write",0x04:"close",0x05:"ioctl",0x06:"exit",0x08:"getc",0x09:"putc",
     0x0E:"abs",0x0F:"labs",0x10:"atoi",0x11:"atol",0x13:"setjmp",0x14:"longjmp",0x15:"strcat",0x16:"strncat",
     0x17:"strcmp",0x18:"strncmp",0x19:"strcpy",0x1A:"strncpy",0x1B:"strlen",0x1C:"index",0x1D:"rindex",
     0x1E:"strchr",0x1F:"strrchr",0x20:"strpbrk",0x21:"strspn",0x22:"strcspn",0x23:"strtok",0x24:"strstr",
     0x25:"toupper",0x26:"tolower",0x27:"bcopy",0x28:"bzero",0x29:"bcmp",0x2A:"memcpy",0x2B:"memset",
     0x2C:"memmove",0x2D:"memcmp",0x2E:"memchr",0x2F:"rand",0x30:"srand",0x31:"qsort",0x33:"malloc",
     0x34:"free",0x36:"bsearch",0x37:"calloc",0x38:"realloc",0x39:"InitHeap",0x3A:"_exit",0x3B:"getchar",
     0x3C:"putchar",0x3D:"gets",0x3E:"puts",0x3F:"printf",0x44:"FlushCache",0x49:"GPU_cw",
     0x72:"CdRemove"}
B = {0x07:"DeliverEvent",0x08:"OpenEvent",0x09:"CloseEvent",0x0A:"WaitEvent",0x0B:"TestEvent",0x0C:"EnableEvent",
     0x0D:"DisableEvent",0x0E:"OpenTh",0x0F:"CloseTh",0x10:"ChangeTh",0x12:"InitPAD",0x13:"StartPAD",0x14:"StopPAD",
     0x15:"PAD_init",0x16:"PAD_dr",0x17:"ReturnFromException",0x18:"ResetEntryInt",0x19:"HookEntryInt",
     0x20:"UnDeliverEvent",0x32:"open_B",0x33:"lseek_B",0x34:"read_B",0x35:"write_B",0x36:"close_B",
     0x37:"ioctl_B",0x38:"exit_B",0x3F:"puts_B",0x40:"cd",0x41:"format",0x42:"firstfile",0x43:"nextfile",
     0x44:"rename",0x45:"erase",0x4A:"InitCARD",0x4B:"StartCARD",0x4C:"StopCARD",0x4E:"_card_write",
     0x4F:"_card_read",0x50:"_new_card",0x51:"Krom2RawAdd",0x56:"GetC0Table",0x57:"GetB0Table",
     0x5B:"ChangeClearPad"}
C = {0x02:"SysEnqIntRP",0x03:"SysDeqIntRP",0x0A:"ChangeClearRCnt"}
TABLES = {0xA0:("A",A),0xB0:("B",B),0xC0:("C",C)}

d = open(os.path.join(ROOT, "baserom/main.bin"), "rb").read()
W = lambda i: struct.unpack_from("<I", d, i)[0]
for i in range(0, len(d) - 8, 4):
    w0, w1, w2 = W(i), W(i + 4), W(i + 8)
    if (w0 >> 16) == 0x240A and (w0 & 0xFFFF) in TABLES and w1 == 0x01400008 and (w2 >> 16) == 0x2409:
        letter, tbl = TABLES[w0 & 0xFFFF]
        fn = w2 & 0xFFFF
        name = tbl.get(fn, f"BIOS_{letter}{fn:02X}")
        print(f"{name} = 0x{VRAM + i:08X}; // type:func size:0xC  BIOS {letter}({fn:#04x})")
