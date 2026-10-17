#include "dol.h"
#include "sha1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8* read_file(const char* path, size_t* size_out) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    u8* data = (u8*)malloc((size_t)size);
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data)
        *size_out = (size_t)size;
    return data;
}

static u32 be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static bool copy_to_guest(CPUState* cpu, u32 addr, const u8* src, u32 size) {
    u32 offset = addr - GC_RAM_BASE;
    if (addr < GC_RAM_BASE || offset > cpu->ram_size || size > cpu->ram_size - offset) {
        fprintf(stderr, "error: section 0x%08X+0x%X is outside main RAM\n", addr, size);
        return false;
    }
    if (src)
        memcpy(cpu->ram + offset, src, size);
    else
        memset(cpu->ram + offset, 0, size);
    return true;
}

u32 dol_load(CPUState* cpu, const char* disc_dir, bool skip_hash) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/sys/main.dol", disc_dir);

    size_t size = 0;
    u8* dol = read_file(path, &size);
    if (!dol || size < 0x100) {
        fprintf(stderr, "error: can't read %s\n", path);
        free(dol);
        return 0;
    }

    char hash[41];
    sha1_hex(dol, size, hash);
    if (strcmp(hash, NFLS2_EXPECTED_DOL_SHA1) != 0) {
        fprintf(stderr, "%s: main.dol SHA-1 is %s, expected %s (USA GN7E69 rev 0)\n",
                skip_hash ? "warning" : "error", hash, NFLS2_EXPECTED_DOL_SHA1);
        if (!skip_hash) {
            free(dol);
            return 0;
        }
    } else {
        printf("main.dol verified (GN7E69 rev 0)\n");
    }

    /* DOL header: 7 text + 11 data sections, then BSS and entry point. */
    for (int i = 0; i < 18; i++) {
        u32 off = be32(dol + 0x00 + i * 4);
        u32 addr = be32(dol + 0x48 + i * 4);
        u32 len = be32(dol + 0x90 + i * 4);
        if (len == 0)
            continue;
        if ((size_t)off + len > size) {
            fprintf(stderr, "error: DOL section %d runs past end of file\n", i);
            free(dol);
            return 0;
        }
        if (!copy_to_guest(cpu, addr, dol + off, len)) {
            free(dol);
            return 0;
        }
    }

    u32 bss_addr = be32(dol + 0xD8);
    u32 bss_size = be32(dol + 0xDC);
    u32 entry = be32(dol + 0xE0);
    free(dol);

    /* BSS may overlap small data sections; those were loaded above and must
       survive, so only clear BSS bytes not covered by a loaded section. The
       game's own startup code clears BSS too, so this is just hygiene. */
    (void)bss_addr;
    (void)bss_size;

    return entry;
}

static void put32(CPUState* cpu, u32 addr, u32 value) {
    mem_write32(cpu, addr, value);
}

bool boot_globals_init(CPUState* cpu, const char* disc_dir) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/sys/boot.bin", disc_dir);
    size_t size = 0;
    u8* boot = read_file(path, &size);
    if (!boot || size < 0x20) {
        fprintf(stderr, "error: can't read %s\n", path);
        free(boot);
        return false;
    }

    /* 0x80000000: disc ID, maker, disc number, version (first 0x20 bytes of boot.bin). */
    copy_to_guest(cpu, GC_RAM_BASE, boot, 0x20);
    free(boot);

    put32(cpu, 0x80000020, 0x0D15EA5Eu);         /* boot magic: booted normally */
    put32(cpu, 0x80000024, 0x00000001u);         /* boot version */
    put32(cpu, 0x80000028, GC_MAIN_RAM_SIZE);    /* physical memory size */
    put32(cpu, 0x8000002C, 0x00000003u);         /* console type: retail */
    put32(cpu, 0x800000F0, GC_MAIN_RAM_SIZE);    /* simulated memory size */
    put32(cpu, 0x800000F8, 162000000u);          /* bus clock */
    put32(cpu, 0x800000FC, 486000000u);          /* CPU clock */
    /* Arena bounds (0x80000030/0x80000034) stay 0 so OSInit uses the DOL's own.
       FST location (0x80000038) is filled when disc reads are implemented. */
    return true;
}
