/*
 * STMP3770 Development Board (HP 39gII Calculator)
 *
 * Mimics the real hardware environment including Boot ROM initialization.
 *
 * Hardware configuration based on ExistOS-For-HP39GII BSP:
 * - 512KB on-chip SRAM (no external DRAM)
 * - NAND Flash (Samsung K9F1G08U0D: 128MB, 2KB page, 64 pages/block)
 * - 131×64 monochrome LCD (grayscale capable)
 * - Matrix keyboard (6×9)
 * - USB 2.0 OTG
 * - Audio DAC/ADC
 *
 * Memory architecture:
 * - Physical SRAM: 512KB @ 0x00000000
 * - Virtual memory: ExistOS uses NAND Flash as swap space
 *   - VM ROM: 0x00100000-0x006FFFFF (6MB virtual, mapped to Flash)
 *   - VM RAM: 0x02000000-0x022FFFFF (3MB virtual, mapped to Flash)
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/stmp3770.h"
#include "hw/boards.h"
#include "hw/qdev-properties.h"
#include "qemu/error-report.h"
#include "hw/loader.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "qemu/units.h"
#include "chardev/char.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/block-backend-global-state.h"
#include "block/block-common.h"
#include "qobject/qdict.h"
#include "qemu/cutils.h"
#include "target/arm/cpu.h"
#include "exec/cputlb.h"
#include "block/aio.h"
#include "crypto/aes.h"

/* HP 39gII hardware: 512KB SRAM only, no external DRAM */
#define STMP3770_BOARD_RAM_DEFAULT  (0)
#define STMP3770_DEFAULT_ROM_NAME   "rom.bin"
#define STMP3770_DEFAULT_FLASH_NAME "flash.bin"

#define TYPE_STMP3770_BOARD MACHINE_TYPE_NAME("stmp3770")
OBJECT_DECLARE_SIMPLE_TYPE(STMP3770BoardState, STMP3770_BOARD)

struct STMP3770BoardState {
    MachineState parent_obj;
    STMP3770State soc;

    /* Boot strap pins (modeled as board-level properties). */
    uint8_t boot_lcd_rs;
    uint8_t boot_lcd_data;

    /* Optional SB image file for direct boot (bypasses boot media search). */
    char *sb_image;

    /* Boot state machine state, exposed for introspection. */
    uint32_t boot_state;
    uint32_t boot_mode;
};

static char *stmp3770_exec_dir_file(const char *name)
{
    g_autofree char *dir = get_relocated_path(".");

    return g_build_filename(dir, name, NULL);
}

static char *stmp3770_default_file(const char *name)
{
    g_autofree char *direct = stmp3770_exec_dir_file(name);

    if (g_file_test(direct, G_FILE_TEST_IS_REGULAR)) {
        return g_steal_pointer(&direct);
    }

    g_autofree char *parent = stmp3770_exec_dir_file("..");
    g_autofree char *parent_firmware =
        g_build_filename(parent, "firmware", name, NULL);

    if (g_file_test(parent_firmware, G_FILE_TEST_IS_REGULAR)) {
        return g_steal_pointer(&parent_firmware);
    }

    return g_build_filename(parent, name, NULL);
}

static bool stmp3770_file_exists(const char *path)
{
    return g_file_test(path, G_FILE_TEST_IS_REGULAR);
}

/* ------------------------------------------------------------------------- */
/* SB (Safe Boot) image parser — STMP3770 boot image format v1.x             */
/* ------------------------------------------------------------------------- */

#define SB_BLOCK_SIZE          16
#define SB_SIGNATURE_STMP      0x504D5453  /* 'STMP' */
#define SB_SIGNATURE_SGTL      0x7367746C  /* 'sgtl' */

/* SB boot image header (see elftosb / u-boot mxsimage.h) */
typedef struct QEMU_PACKED SBBootImageHeader {
    union {
        uint8_t digest[20];     /* SHA1 of header (also CBC-MAC IV) */
        struct {
            uint8_t iv[16];     /* CBC-MAC initialization vector */
            uint8_t extra[4];
        };
    };
    uint8_t  signature1[4];     /* 'STMP' */
    uint8_t  major_version;
    uint8_t  minor_version;
    uint16_t flags;
    uint32_t image_blocks;      /* total size in 16-byte blocks */
    uint32_t first_boot_tag_block;
    uint32_t first_boot_section_id;
    uint16_t key_count;
    uint16_t key_dictionary_block;
    uint16_t header_blocks;     /* header size in 16-byte blocks */
    uint16_t section_count;
    uint16_t section_header_size;
    uint8_t  padding0[2];
    uint8_t  signature2[4];     /* 'sgtl' (v1.1+) */
    uint64_t timestamp_us;
    uint16_t product_version_major;
    uint16_t product_version_pad0;
    uint16_t product_version_minor;
    uint16_t product_version_pad1;
    uint16_t product_version_revision;
    uint16_t product_version_pad2;
    uint16_t component_version_major;
    uint16_t component_version_pad0;
    uint16_t component_version_minor;
    uint16_t component_version_pad1;
    uint16_t component_version_revision;
    uint16_t component_version_pad2;
    uint16_t drive_tag;
    uint8_t  padding1[6];
} SBBootImageHeader;

QEMU_BUILD_BUG_MSG(sizeof(SBBootImageHeader) != 96, "SB header size mismatch");

/* Key dictionary entry */
typedef struct QEMU_PACKED SBKeyDictionaryKey {
    uint8_t cbc_mac[SB_BLOCK_SIZE];  /* CBC-MAC of section headers */
    uint8_t key[SB_BLOCK_SIZE];      /* AES key encrypted by image key */
} SBKeyDictionaryKey;

/* Section header */
typedef struct QEMU_PACKED SBSectionsHeader {
    uint32_t section_number;
    uint32_t section_offset;   /* offset in 16-byte blocks after TAG */
    uint32_t section_size;     /* size in 16-byte blocks */
    uint32_t section_flags;
} SBSectionsHeader;

#define SB_SECTION_FLAG_BOOTABLE  (1 << 0)

/* Boot command tags */
enum {
    ROM_NOP_CMD  = 0x00,
    ROM_TAG_CMD  = 0x01,
    ROM_LOAD_CMD = 0x02,
    ROM_FILL_CMD = 0x03,
    ROM_JUMP_CMD = 0x04,
    ROM_CALL_CMD = 0x05,
    ROM_MODE_CMD = 0x06,
};

#define ROM_TAG_CMD_FLAG_LAST_TAG  0x1

/* Boot command (16 bytes) */
typedef struct QEMU_PACKED SBCommand {
    uint8_t  checksum;
    uint8_t  tag;
    uint16_t flags;
    uint32_t args[3];
} SBCommand;

QEMU_BUILD_BUG_MSG(sizeof(SBCommand) != SB_BLOCK_SIZE, "SB command size mismatch");

/* AES-128 CBC decrypt one block in-place */
static void sb_aes_cbc_decrypt_block(const AES_KEY *key, const uint8_t *iv,
                                     uint8_t *block)
{
    uint8_t tmp[SB_BLOCK_SIZE];
    AES_decrypt(block, tmp, key);
    for (int i = 0; i < SB_BLOCK_SIZE; i++) {
        tmp[i] ^= iv[i];
    }
    memcpy(block, tmp, SB_BLOCK_SIZE);
}

/* AES-128 CBC decrypt a buffer in-place */
static void sb_aes_cbc_decrypt(const AES_KEY *key, uint8_t *buf,
                               uint32_t len, const uint8_t *iv_in)
{
    uint8_t iv[SB_BLOCK_SIZE];
    uint8_t prev_ct[SB_BLOCK_SIZE];

    memcpy(iv, iv_in, SB_BLOCK_SIZE);
    g_assert(len % SB_BLOCK_SIZE == 0);

    for (uint32_t off = 0; off < len; off += SB_BLOCK_SIZE) {
        memcpy(prev_ct, buf + off, SB_BLOCK_SIZE);
        sb_aes_cbc_decrypt_block(key, iv, buf + off);
        memcpy(iv, prev_ct, SB_BLOCK_SIZE);
    }
}

/* Get the image key from OCOTP CRYPTO_KEY or zeros for unencrypted boot */
static void sb_get_image_key(STMP3770BoardState *s, uint8_t *key)
{
    STMP3770State *soc = &s->soc;
    uint32_t enable_unencrypted = soc->ocotp->rom[0] & (1U << 4);

    if (enable_unencrypted || soc->ocotp->crypto[0] == 0) {
        memset(key, 0, SB_BLOCK_SIZE);
        return;
    }

    /* OCOTP CRYPTO0-3 provide the 128-bit AES key (4 × 32-bit LE words) */
    for (int i = 0; i < 4; i++) {
        key[i * 4 + 0] = soc->ocotp->crypto[i] & 0xFF;
        key[i * 4 + 1] = (soc->ocotp->crypto[i] >> 8) & 0xFF;
        key[i * 4 + 2] = (soc->ocotp->crypto[i] >> 16) & 0xFF;
        key[i * 4 + 3] = (soc->ocotp->crypto[i] >> 24) & 0xFF;
    }
}

/* ---- NCB/LDLB/DBBT NAND boot control block parsing ---- */

#define NCB_FINGERPRINT1    0x504D5453  /* 'STMP' */
#define NCB_FINGERPRINT2    0x2042434E  /* 'NCB ' */
#define NCB_FINGERPRINT3    0x4E494252  /* 'RBIN' */
#define LDLB_FINGERPRINT2   0x424C444C  /* 'LDLB' */
#define LDLB_FINGERPRINT3   0x4C494252  /* 'RBIL' */
#define DBBT_FINGERPRINT2   0x54424244  /* 'DBBT' */
#define DBBT_FINGERPRINT3   0x44494252  /* 'RBID' */

#define NCB_BOOT_SEARCH_BLOCKS  4   /* ROM searches blocks 0-3 for NCB */
#define NCB_BOOT_READ_SIZE      2048 /* ROM reads first 2K of each page */

/*
 * NCB/LDLB/DBBT share a common 3-fingerprint layout (NCB_BootBlockStruct_t
 * from NXP imx-kobs BootControlBlocks.h).  The structure is 144 bytes but
 * only the first ~100 bytes contain fields used by the ROM boot loader.
 *
 * Layout:
 *   [0]   FingerPrint1 ('STMP')
 *   [4]   Block1 union (40 bytes): NCB timing+geometry / LDLB version+bitmap / DBBT bad-block counts
 *   [44]  FingerPrint2 ('NCB '/'LDLB'/'DBBT')
 *   [48]  Block2 union (80 bytes): NCB ECC config / LDLB firmware location / DBBT reserved
 *   [128] FingerPrint3 ('RBIN'/'RBIL'/'RBID')
 */
typedef struct QEMU_PACKED NCBBootBlock {
    uint32_t fingerprint1;
    union {
        struct QEMU_PACKED {
            uint8_t  data_setup;
            uint8_t  data_hold;
            uint8_t  address_setup;
            uint8_t  dsample_time;
            uint32_t data_page_size;
            uint32_t total_page_size;
            uint32_t sectors_per_block;
            uint32_t sector_in_page_mask;
            uint32_t sector_to_page_shift;
            uint32_t number_of_nands;
        } ncb1;
        struct QEMU_PACKED {
            uint16_t ldlb_version_major;
            uint16_t ldlb_version_minor;
            uint16_t ldlb_version_sub;
            uint16_t ldlb_version_reserved;
            uint32_t nand_bitmap;
        } ldlb1;
        struct QEMU_PACKED {
            uint32_t number_bb_nand0;
            uint32_t number_bb_nand1;
            uint32_t number_bb_nand2;
            uint32_t number_bb_nand3;
            uint32_t num_2k_pages_bb_nand0;
            uint32_t num_2k_pages_bb_nand1;
            uint32_t num_2k_pages_bb_nand2;
            uint32_t num_2k_pages_bb_nand3;
        } dbbt1;
        uint32_t reserved1[10];
    } block1;
    uint32_t fingerprint2;
    union {
        struct QEMU_PACKED {
            uint32_t num_row_bytes;
            uint32_t num_column_bytes;
            uint32_t total_internal_die;
            uint32_t internal_planes_per_die;
            uint32_t cell_type;
            uint32_t ecc_type;
            uint32_t ecc_block0_size;
            uint32_t ecc_block_n_size;
            uint32_t ecc_block0_ecc_level;
            uint32_t num_ecc_blocks_per_page;
            uint32_t metadata_bytes;
            uint32_t erase_threshold;
            uint32_t read1st_code;
            uint32_t read2nd_code;
            uint32_t boot_patch;
            uint32_t patch_sectors;
            uint32_t firmware_starting_nand2;
        } ncb2;
        struct QEMU_PACKED {
            uint32_t firmware_starting_nand;
            uint32_t firmware_starting_sector;
            uint32_t firmware_sector_stride;
            uint32_t sectors_in_firmware;
            uint32_t firmware_starting_nand2;
            uint32_t firmware_starting_sector2;
            uint32_t firmware_sector_stride2;
            uint32_t sectors_in_firmware2;
            uint16_t firmware_version_major;
            uint16_t firmware_version_minor;
            uint16_t firmware_version_sub;
            uint16_t firmware_version_reserved;
            uint32_t discovered_bb_table_sector;
            uint32_t discovered_bb_table_sector2;
        } ldlb2;
        uint32_t reserved2[20];
    } block2;
    uint32_t fingerprint3;
} NCBBootBlock;

QEMU_BUILD_BUG_MSG(sizeof(NCBBootBlock) != 132, "NCB boot block size mismatch");

/*
 * Read the data portion of a NAND page from the GPMI storage backend.
 * Returns true on success, false if the page is out of range or no storage.
 */
static bool stmp3770_nand_read_page(STMP3770GPMIState *gpmi,
                                    uint32_t page_row,
                                    uint8_t *buf, uint32_t max_len)
{
    uint64_t data_offset;
    uint32_t page_size;

    if (!gpmi || !gpmi->storage) {
        return false;
    }

    page_size = gpmi->page_size;
    if (page_row >= (uint64_t)gpmi->pages_per_block * gpmi->num_blocks) {
        return false;
    }

    if (gpmi->storage_layout == GPMI_STORAGE_LAYOUT_INTERLEAVED_OOB) {
        data_offset = (uint64_t)page_row * (page_size + gpmi->oob_size);
    } else {
        data_offset = (uint64_t)page_row * page_size;
    }

    if (data_offset + page_size > gpmi->storage_size) {
        return false;
    }

    uint32_t to_copy = MIN(page_size, max_len);
    memcpy(buf, gpmi->storage + data_offset, to_copy);
    return true;
}

/*
 * Search for an NCB (NAND Control Block) in the first page of blocks 0-3.
 * On success, fills in *ncb and returns the block index where it was found.
 * Returns -1 if not found.
 */
static int stmp3770_boot_search_ncb(STMP3770GPMIState *gpmi,
                                    NCBBootBlock *ncb)
{
    uint8_t page_data[NCB_BOOT_READ_SIZE];

    for (int block = 0; block < NCB_BOOT_SEARCH_BLOCKS; block++) {
        uint32_t page_row = (uint32_t)block * gpmi->pages_per_block;

        if (!stmp3770_nand_read_page(gpmi, page_row, page_data,
                                     sizeof(page_data))) {
            continue;
        }

        memcpy(ncb, page_data, sizeof(*ncb));

        if (ncb->fingerprint1 == NCB_FINGERPRINT1 &&
            ncb->fingerprint2 == NCB_FINGERPRINT2 &&
            ncb->fingerprint3 == NCB_FINGERPRINT3) {
            return block;
        }
    }

    return -1;
}

/*
 * Search for an LDLB (Logical Drive Layout Block) in the first page of
 * the block after the NCB block.  On success, fills in *ldlb and returns
 * the block index.  Returns -1 if not found.
 */
static int stmp3770_boot_search_ldlb(STMP3770GPMIState *gpmi,
                                     int ncb_block,
                                     NCBBootBlock *ldlb)
{
    uint8_t page_data[NCB_BOOT_READ_SIZE];
    int ldlb_block = ncb_block + 1;
    uint32_t page_row;

    if (ldlb_block >= gpmi->num_blocks) {
        return -1;
    }

    page_row = (uint32_t)ldlb_block * gpmi->pages_per_block;

    if (!stmp3770_nand_read_page(gpmi, page_row, page_data,
                                 sizeof(page_data))) {
        return -1;
    }

    memcpy(ldlb, page_data, sizeof(*ldlb));

    if (ldlb->fingerprint1 == NCB_FINGERPRINT1 &&
        ldlb->fingerprint2 == LDLB_FINGERPRINT2 &&
        ldlb->fingerprint3 == LDLB_FINGERPRINT3) {
        return ldlb_block;
    }

    return -1;
}

/*
 * Parse and execute an SB boot image.
 *
 * The SB image format (v1.x) is used by STMP3770/i.MX23/i.MX28 ROM boot
 * loaders.  The image is divided into 16-byte blocks and may be
 * AES-128 CBC encrypted.  Unencrypted images use a zero key with CBC
 * pass-through (the data is still XORed through CBC-MAC but the key is
 * all zeros, so the "decryption" is effectively a no-op on the plaintext).
 *
 * Layout:
 *   [header (header_blocks × 16 bytes)]
 *   [section headers (section_count × section_header_size × 16 bytes)]
 *   [key dictionary (key_count × 32 bytes)]
 *   [section data (tag + commands + load data)]
 *   [SHA1 digest (16 bytes, encrypted)]
 *
 * Returns true if a JUMP/CALL command set the entry point.
 */
static bool stmp3770_sb_parse_execute(STMP3770BoardState *s,
                                      uint8_t *data, uint32_t size,
                                      hwaddr *entry)
{
    SBBootImageHeader *hdr;
    AES_KEY aes_key;
    uint8_t image_key[SB_BLOCK_SIZE];
    uint8_t saved_iv[SB_BLOCK_SIZE];
    uint32_t hdr_blocks, total_blocks;
    uint32_t off;

    if (size < sizeof(SBBootImageHeader)) {
        error_report("STMP3770 SB: image too small (%u bytes)", size);
        return false;
    }

    hdr = (SBBootImageHeader *)data;
    total_blocks = size / SB_BLOCK_SIZE;

    /* Save the first 16 bytes (ciphertext) as the IV for section data
     * decryption (per u-boot mxsimage.c: sb_aes_reinit uses header.iv
     * = first 16 bytes of encrypted header). */
    memcpy(saved_iv, data, SB_BLOCK_SIZE);

    /* Get the image key (zero for unencrypted, OCOTP CRYPTO_KEY for encrypted) */
    sb_get_image_key(s, image_key);

    /* Save the last ciphertext block of the header area — it is the IV
     * for decrypting the section headers (the CBC chain continues from
     * the header into the section headers during encryption). */
    uint32_t hdr_bytes = sizeof(SBBootImageHeader);
    uint8_t sect_hdr_iv[SB_BLOCK_SIZE];
    memcpy(sect_hdr_iv, data + hdr_bytes - SB_BLOCK_SIZE, SB_BLOCK_SIZE);

    /* Initialize AES-128 CBC decrypt with image key and IV = image key */
    AES_set_decrypt_key(image_key, 128, &aes_key);
    sb_aes_cbc_decrypt(&aes_key, data, hdr_bytes, image_key);

    /* Verify signature */
    if (ldl_le_p(hdr->signature1) != SB_SIGNATURE_STMP) {
        error_report("STMP3770 SB: bad signature 0x%08x (expected 0x%08x)",
                     ldl_le_p(hdr->signature1), SB_SIGNATURE_STMP);
        return false;
    }

    hdr_blocks = hdr->header_blocks;
    if (hdr_blocks == 0 || hdr_blocks > total_blocks) {
        error_report("STMP3770 SB: invalid header_blocks %u", hdr_blocks);
        return false;
    }

    if (hdr->image_blocks > total_blocks) {
        error_report("STMP3770 SB: image_blocks %u > total %u",
                     hdr->image_blocks, total_blocks);
        return false;
    }

    info_report("STMP3770 SB: v%u.%u, %u blocks, %u sections, %u keys",
                hdr->major_version, hdr->minor_version,
                hdr->image_blocks, hdr->section_count, hdr->key_count);

    /* Decrypt section headers (immediately after the header) */
    off = hdr_blocks * SB_BLOCK_SIZE;
    uint32_t sect_hdr_size = hdr->section_count * hdr->section_header_size
                             * SB_BLOCK_SIZE;
    if (off + sect_hdr_size > size) {
        error_report("STMP3770 SB: section headers exceed image size");
        return false;
    }
    sb_aes_cbc_decrypt(&aes_key, data + off, sect_hdr_size, sect_hdr_iv);
    SBSectionsHeader *sects = (SBSectionsHeader *)(data + off);

    /* Decrypt key dictionary (if present) */
    if (hdr->key_count > 0) {
        uint32_t key_off = hdr->key_dictionary_block * SB_BLOCK_SIZE;
        uint32_t key_size = hdr->key_count * sizeof(SBKeyDictionaryKey);
        if (key_off + key_size > size) {
            error_report("STMP3770 SB: key dictionary exceeds image size");
            return false;
        }
        /* Reinit AES with saved IV for key dictionary decryption */
        sb_aes_cbc_decrypt(&aes_key, data + key_off, key_size, saved_iv);
    }

    /* Find the bootable section and execute its commands */
    bool found_boot = false;
    bool jumped = false;

    for (uint16_t si = 0; si < hdr->section_count && !jumped; si++) {
        SBSectionsHeader *sh = &sects[si];
        if (!(sh->section_flags & SB_SECTION_FLAG_BOOTABLE)) {
            continue;
        }
        if (sh->section_number != hdr->first_boot_section_id && found_boot) {
            continue;
        }
        found_boot = true;

        /* Section data starts at first_boot_tag_block for the boot section */
        uint32_t tag_block = hdr->first_boot_tag_block;
        if (tag_block >= total_blocks) {
            error_report("STMP3770 SB: tag block %u out of range", tag_block);
            break;
        }

        /* Decrypt and execute commands in this section */
        uint32_t sect_end = tag_block + sh->section_size;
        if (sect_end > total_blocks) {
            sect_end = total_blocks;
        }

        /* Reinit AES for each section (IV = saved_iv) */
        uint8_t cmd_iv[SB_BLOCK_SIZE];
        memcpy(cmd_iv, saved_iv, SB_BLOCK_SIZE);

        uint32_t cur_block = tag_block;
        while (cur_block < sect_end && !jumped) {
            uint8_t *cmd_ptr = data + cur_block * SB_BLOCK_SIZE;

            /* Decrypt one command block */
            uint8_t prev_ct[SB_BLOCK_SIZE];
            memcpy(prev_ct, cmd_ptr, SB_BLOCK_SIZE);
            sb_aes_cbc_decrypt_block(&aes_key, cmd_iv, cmd_ptr);
            memcpy(cmd_iv, prev_ct, SB_BLOCK_SIZE);

            SBCommand *cmd = (SBCommand *)cmd_ptr;

            switch (cmd->tag) {
            case ROM_NOP_CMD:
                cur_block++;
                break;

            case ROM_TAG_CMD:
                /* TAG marks the start of a section; reinit IV */
                memcpy(cmd_iv, saved_iv, SB_BLOCK_SIZE);
                cur_block++;
                if (cmd->flags & ROM_TAG_CMD_FLAG_LAST_TAG) {
                    jumped = true; /* stop after last tag */
                }
                break;

            case ROM_LOAD_CMD: {
                uint32_t addr = cmd->args[0];
                uint32_t count = cmd->args[1]; /* in bytes */
                uint32_t num_blocks = (count + SB_BLOCK_SIZE - 1) / SB_BLOCK_SIZE;
                cur_block++;

                if (cur_block + num_blocks > sect_end) {
                    error_report("STMP3770 SB: LOAD data exceeds section");
                    jumped = true;
                    break;
                }

                /* Decrypt load data in-place */
                uint8_t *load_data = data + cur_block * SB_BLOCK_SIZE;
                /* Save last ciphertext block before in-place decrypt
                 * overwrites it — it becomes the IV for the next command. */
                uint8_t last_ct[SB_BLOCK_SIZE];
                memcpy(last_ct, load_data + (num_blocks - 1) * SB_BLOCK_SIZE,
                       SB_BLOCK_SIZE);
                sb_aes_cbc_decrypt(&aes_key, load_data,
                                   num_blocks * SB_BLOCK_SIZE, cmd_iv);
                memcpy(cmd_iv, last_ct, SB_BLOCK_SIZE);

                /* Write data to target address */
                address_space_write(&address_space_memory, addr,
                                    MEMTXATTRS_UNSPECIFIED,
                                    load_data, count);

                cur_block += num_blocks;
                break;
            }

            case ROM_FILL_CMD: {
                uint32_t addr = cmd->args[0];
                uint32_t count = cmd->args[1]; /* in bytes */
                uint32_t pattern = cmd->args[2];

                /* Fill memory with pattern (32-bit pattern, count bytes) */
                uint32_t pat_le = cpu_to_le32(pattern);
                for (uint32_t i = 0; i < count; i += 4) {
                    uint32_t sz = (count - i < 4) ? (count - i) : 4;
                    address_space_write(&address_space_memory, addr + i,
                                        MEMTXATTRS_UNSPECIFIED,
                                        &pat_le, sz);
                }
                cur_block++;
                break;
            }

            case ROM_JUMP_CMD: {
                uint32_t addr = cmd->args[0];
                uint32_t arg = cmd->args[2];
                info_report("STMP3770 SB: JUMP to 0x%08x (r0=0x%08x)",
                            addr, arg);
                s->soc.cpu.env.regs[0] = arg;
                *entry = addr;
                jumped = true;
                break;
            }

            case ROM_CALL_CMD: {
                uint32_t addr = cmd->args[0];
                uint32_t arg = cmd->args[2];
                info_report("STMP3770 SB: CALL 0x%08x (r0=0x%08x)",
                            addr, arg);
                /* For CALL, set r0 and jump — the called function is
                 * expected to return to the boot loader, but in QEMU
                 * we treat it like JUMP for simplicity. */
                s->soc.cpu.env.regs[0] = arg;
                *entry = addr;
                jumped = true;
                break;
            }

            case ROM_MODE_CMD:
                /* MODE command changes boot mode — not applicable after
                 * the image is already loaded.  Skip. */
                cur_block++;
                break;

            default:
                error_report("STMP3770 SB: unknown command tag 0x%02x at "
                             "block %u", cmd->tag, cur_block);
                jumped = true;
                break;
            }
        }
    }

    if (!found_boot) {
        error_report("STMP3770 SB: no bootable section found");
    }

    return jumped;
}

/*
 * Simulate a peripheral register write as the boot ROM would do.
 * Uses address_space_write to go through the system address space,
 * ensuring all side effects (state transitions, IRQ updates) are
 * properly handled.
 */
static void stmp3770_rom_write(MemoryRegion *mr, hwaddr offset,
                               uint64_t value)
{
    memory_region_dispatch_write(mr, offset, value,
                                 MO_32, MEMTXATTRS_UNSPECIFIED);
}

/*
 * Simulate the STMP3770 mask ROM boot initialization sequence.
 *
 * This replicates the observable side effects of the real SigmaTel mask ROM
 * (extracted from a 64 KiB OCROM dump) reset handler and early init functions:
 *
 *  1. Reset handler (0xFFFF2338, ARM mode):
 *     - Ungate RTC, PINCTRL (SFTRST + CLKGATE clear)
 *     - Ungate POWER (CLKGATE clear only; SFTRST is not asserted at reset)
 *
 *  2. init_func_1 (0xFFFF15A4): OCOTP bank open/read/close cycle
 *
 *  3. init_func_2 (0xFFFF1858): Debug UART configuration
 *     - IBRD=0x0D, FBRD=0x01 (24 MHz / (16 * 115200) ≈ 13.02)
 *     - LCR_H=0x70 (8-bit, FIFO enabled)
 *     - CR=0x301 (UARTEN | TXE | RXE)
 *
 *  4. init_func_3/4 (0xFFFF1518/0xFFFF1538): CP15 TLB + I-cache invalidate
 *  5. init_func_5/6 (0xFFFF154C/0xFFFF1554): CP15 SCTLR configuration
 *     - Enable alignment checking (SCTLR_A, bit 1)
 *     - Configure ARM926EJ-S control bits
 */
static void stmp3770_rom_boot_init(void *opaque)
{
    STMP3770State *soc = opaque;

    /* Get memory regions for each device we need to write to */
    MemoryRegion *rtc_mr = sysbus_mmio_get_region(
        SYS_BUS_DEVICE(soc->rtc), 0);
    MemoryRegion *pinctrl_mr = sysbus_mmio_get_region(
        SYS_BUS_DEVICE(soc->pinctrl), 0);
    MemoryRegion *power_mr = sysbus_mmio_get_region(
        SYS_BUS_DEVICE(soc->power), 0);
    MemoryRegion *ocotp_mr = sysbus_mmio_get_region(
        SYS_BUS_DEVICE(soc->ocotp), 0);

    /*
     * 1. Reset handler: release peripheral soft-reset and clock gates.
     *
     * RTC CTRL_CLR (0x8005C008) = 0xC0000000  (SFTRST | CLKGATE)
     * PINCTRL CTRL_CLR (0x80018008) = 0xC0000000  (SFTRST | CLKGATE)
     * POWER CTRL_CLR (0x80044008) = 0x40000000  (CLKGATE only)
     */
    stmp3770_rom_write(rtc_mr, 0x008, 0xC0000000);
    stmp3770_rom_write(pinctrl_mr, 0x008, 0xC0000000);
    stmp3770_rom_write(power_mr, 0x008, 0x40000000);

    /*
     * 2. OCOTP init: open bank for shadow read, then close.
     *
     * OCOTP CTRL_SET (0x8002C004) = 0x00001000  (RD_BANK_OPEN, bit 12)
     * OCOTP CTRL_CLR (0x8002C008) = 0x00001000  (close bank)
     *
     * The real ROM waits for the bank-ready status between set and close,
     * but QEMU's OCOTP shadow read is synchronous so no polling is needed.
     */
    stmp3770_rom_write(ocotp_mr, 0x004, 0x00001000);
    stmp3770_rom_write(ocotp_mr, 0x008, 0x00001000);

    /*
     * 3. Debug UART configuration (init_func_2).
     *
     * The ROM configures the debug UART for 115200 baud at 24 MHz XTAL:
     *   IBRD = 13, FBRD = 1  →  24e6 / (16 * (13 + 1/64)) = 115384 Hz
     *   LCR_H = 0x70         →  8-bit word, FIFO enabled
     *   CR = 0x301           →  UARTEN | TXE | RXE
     *
     * Directly set the UART state registers because the UART device
     * reset must run first (to initialize FIFOs and timers) and our
     * bottom-half callback runs after reset completes.
     */
    {
        STMP3770UARTDebugState *uart = soc->uartdbg;
        uart->ibrd = 0x00D;
        uart->fbrd = 0x001;
        uart->lcr = 0x070;
        uart->cr = 0x301;
    }

    /*
     * 4. CP15 initialization (init_func_3/4/5/6).
     *
     * The ROM performs:
     *   MCR p15, r0, c8, c7, #0  — invalidate entire TLB
     *   MCR p15, r0, c7, c5, #0  — invalidate entire I-cache
     *   MRC p15, r0, c1, c0, #0  — read SCTLR
     *   ORR r0, r0, #2            — set SCTLR_A (alignment check enable)
     *   AND r0, r0, mask          — clear unused/reserved bits
     *   ORR r0, r0, value         — set ARM926 control bits
     *   MCR p15, r0, c1, c0, #0  — write SCTLR
     *
     * In QEMU, TLB invalidation maps to tlb_flush().  I-cache invalidation
     * is a no-op (QEMU manages translation caching internally).  The SCTLR
     * is modified in-place and hflags are rebuilt.
     */
    {
        CPUARMState *env = &soc->cpu.env;
        uint64_t sctlr = env->cp15.sctlr_ns;

        /* Flush TLB (MCR p15, c8, c7, #0) */
        tlb_flush(CPU(&soc->cpu));

        /* Modify SCTLR: enable alignment checking, configure control bits */
        sctlr |= SCTLR_A;          /* bit 1: alignment check enable */
        sctlr &= 0x0005F3FFULL;    /* clear upper bits and reserved fields */
        sctlr |= 0x00050078ULL;    /* set ARM926 control bits */
        env->cp15.sctlr_ns = sctlr;
        arm_rebuild_hflags(env);
    }
}

static BlockBackend *stmp3770_open_default_flash(const char *path)
{
    Error *local_err = NULL;
    QDict *options = qdict_new();
    BlockBackend *blk;

    qdict_put_str(options, "driver", "raw");
    blk = blk_new_open(path, NULL, options, BDRV_O_RDWR | BDRV_O_RESIZE,
                       &local_err);
    if (!blk) {
        warn_report("Could not open default flash '%s': %s",
                    path, error_get_pretty(local_err));
        error_free(local_err);
        return NULL;
    }

    return blk;
}

typedef enum {
    STMP3770_BOOT_RESULT_WAIT,
    STMP3770_BOOT_RESULT_LOADED,
    STMP3770_BOOT_RESULT_FAILED,
} STMP3770BootResult;

typedef enum {
    STMP3770_BOOT_MODE_USB,
    STMP3770_BOOT_MODE_I2C,
    STMP3770_BOOT_MODE_SPI1_FLASH,
    STMP3770_BOOT_MODE_SPI2_FLASH,
    STMP3770_BOOT_MODE_GPMI_ECC4,
    STMP3770_BOOT_MODE_JTAG_WAIT,
    STMP3770_BOOT_MODE_SPI2_EEPROM,
    STMP3770_BOOT_MODE_SSP1_MMC,
    STMP3770_BOOT_MODE_SSP2_MMC,
    STMP3770_BOOT_MODE_GPMI_ECC8,
    STMP3770_BOOT_MODE_RECOVERY,
    STMP3770_BOOT_MODE_UNKNOWN,
    STMP3770_BOOT_MODE_MAX,
} STMP3770BootMode;

typedef enum {
    STMP3770_BOOT_STATE_INIT,
    STMP3770_BOOT_STATE_SELECT,
    STMP3770_BOOT_STATE_PORT_INIT,
    STMP3770_BOOT_STATE_LOAD,
    STMP3770_BOOT_STATE_EXEC,
    STMP3770_BOOT_STATE_JTAG_WAIT,
    STMP3770_BOOT_STATE_FAILED,
} STMP3770BootState;

static const char * const stmp3770_boot_mode_name[STMP3770_BOOT_MODE_MAX] = {
    [STMP3770_BOOT_MODE_USB]         = "USB",
    [STMP3770_BOOT_MODE_I2C]         = "I2C",
    [STMP3770_BOOT_MODE_SPI1_FLASH]  = "SPI1 Flash",
    [STMP3770_BOOT_MODE_SPI2_FLASH]  = "SPI2 Flash",
    [STMP3770_BOOT_MODE_GPMI_ECC4]   = "GPMI ECC4",
    [STMP3770_BOOT_MODE_JTAG_WAIT]   = "JTAG_WAIT",
    [STMP3770_BOOT_MODE_SPI2_EEPROM] = "SPI2 EEPROM",
    [STMP3770_BOOT_MODE_SSP1_MMC]    = "SSP1 MMC",
    [STMP3770_BOOT_MODE_SSP2_MMC]    = "SSP2 MMC",
    [STMP3770_BOOT_MODE_GPMI_ECC8]   = "GPMI ECC8",
    [STMP3770_BOOT_MODE_RECOVERY]    = "Recovery",
    [STMP3770_BOOT_MODE_UNKNOWN]     = "Unknown",
};

static STMP3770BootMode stmp3770_boot_mode_from_bm(uint8_t bm)
{
    switch (bm) {
    case 0x0:
        return STMP3770_BOOT_MODE_USB;
    case 0x1:
        return STMP3770_BOOT_MODE_I2C;
    case 0x2:
        return STMP3770_BOOT_MODE_SPI1_FLASH;
    case 0x3:
        return STMP3770_BOOT_MODE_SPI2_FLASH;
    case 0x4:
        return STMP3770_BOOT_MODE_GPMI_ECC4;
    case 0x6:
        return STMP3770_BOOT_MODE_JTAG_WAIT;
    case 0x8:
        return STMP3770_BOOT_MODE_SPI2_EEPROM;
    case 0x9:
        return STMP3770_BOOT_MODE_SSP1_MMC;
    case 0xA:
        return STMP3770_BOOT_MODE_SSP2_MMC;
    case 0xC:
        return STMP3770_BOOT_MODE_GPMI_ECC8;
    default:
        return STMP3770_BOOT_MODE_UNKNOWN;
    }
}

static STMP3770BootMode stmp3770_boot_select(STMP3770BoardState *s)
{
    STMP3770State *soc = &s->soc;
    uint32_t ocotp_rom0 = soc->ocotp->rom[0];
    uint32_t rtc_persistent1 = soc->rtc->persistent[1];
    bool force_recovery = rtc_persistent1 & 0x1;
    bool disable_recovery = ocotp_rom0 & (1U << 2);
    uint8_t bm;

    if (force_recovery) {
        /* ROM consumes the recovery latch once it has read it. */
        soc->rtc->persistent[1] &= ~1U;
    }

    if (force_recovery && !disable_recovery) {
        return STMP3770_BOOT_MODE_RECOVERY;
    }

    if (s->boot_lcd_rs & 0x1) {
        bm = s->boot_lcd_data & 0xF;
    } else {
        bm = (ocotp_rom0 >> 24) & 0xF;
    }

    return stmp3770_boot_mode_from_bm(bm);
}

static STMP3770BootResult stmp3770_boot_load_usb(STMP3770BoardState *s,
                                                 hwaddr *entry)
{
    return STMP3770_BOOT_RESULT_FAILED;
}

static STMP3770BootResult stmp3770_boot_load_i2c(STMP3770BoardState *s,
                                                 hwaddr *entry)
{
    return STMP3770_BOOT_RESULT_FAILED;
}

static STMP3770BootResult stmp3770_boot_load_spi(STMP3770BoardState *s,
                                                 hwaddr *entry, int port)
{
    return STMP3770_BOOT_RESULT_FAILED;
}

static STMP3770BootResult stmp3770_boot_load_ssp(STMP3770BoardState *s,
                                                 hwaddr *entry, int port)
{
    return STMP3770_BOOT_RESULT_FAILED;
}

static STMP3770BootResult stmp3770_boot_load_gpmi(STMP3770BoardState *s,
                                                  hwaddr *entry, bool ecc8)
{
    STMP3770GPMIState *gpmi = s->soc.gpmi;
    NCBBootBlock ncb, ldlb;
    int ncb_block, ldlb_block;
    uint32_t fw_start_sector, fw_sectors;
    uint32_t page_size, pages_per_block;
    uint8_t *fw_data;
    uint32_t fw_size;
    uint32_t page_row;
    uint32_t copied;

    if (!gpmi || !gpmi->storage) {
        info_report("STMP3770 boot: GPMI has no NAND backing, skipping NAND boot");
        return STMP3770_BOOT_RESULT_FAILED;
    }

    page_size = gpmi->page_size;
    pages_per_block = gpmi->pages_per_block;

    /* Step 1: Search for NCB in the first page of blocks 0-3. */
    ncb_block = stmp3770_boot_search_ncb(gpmi, &ncb);
    if (ncb_block < 0) {
        info_report("STMP3770 boot: NCB not found in NAND blocks 0-%d",
                    NCB_BOOT_SEARCH_BLOCKS - 1);
        return STMP3770_BOOT_RESULT_FAILED;
    }

    info_report("STMP3770 boot: NCB found in block %d "
                "(page_size=%u, sectors_per_block=%u, nands=%u)",
                ncb_block, ncb.block1.ncb1.data_page_size,
                ncb.block1.ncb1.sectors_per_block,
                ncb.block1.ncb1.number_of_nands);

    /* Step 2: Search for LDLB in the block after NCB. */
    ldlb_block = stmp3770_boot_search_ldlb(gpmi, ncb_block, &ldlb);
    if (ldlb_block < 0) {
        info_report("STMP3770 boot: LDLB not found after NCB block %d",
                    ncb_block);
        return STMP3770_BOOT_RESULT_FAILED;
    }

    fw_start_sector = ldlb.block2.ldlb2.firmware_starting_sector;
    fw_sectors = ldlb.block2.ldlb2.sectors_in_firmware;

    info_report("STMP3770 boot: LDLB found in block %d "
                "(fw_start_sector=%u, fw_sectors=%u)",
                ldlb_block, fw_start_sector, fw_sectors);

    if (fw_sectors == 0) {
        warn_report("STMP3770 boot: LDLB reports zero firmware sectors");
        return STMP3770_BOOT_RESULT_FAILED;
    }

    /*
     * Step 3: Load firmware pages from NAND.
     *
     * The ROM reads firmware starting at the LDLB-specified sector,
     * reading one NAND page per sector.  We assemble the pages into
     * a contiguous buffer and then feed it to the SB image parser.
     *
     * For simplicity (and matching the ROM behavior for small images),
     * we read fw_sectors pages sequentially, starting from the page
     * at (fw_start_sector * pages_per_block / sectors_per_block) if
     * sectors are block-aligned, or more commonly from page
     * fw_start_sector directly when sector == page.
     *
     * In the STMP3770 ROM, "sector" means "2K page", so
     * firmware_starting_sector is a page index.  For 2K pages this
     * is a 1:1 mapping; for 4K pages the sector_to_page_shift applies.
     */
    uint32_t sector_to_page_shift = 0;
    if (ncb.block1.ncb1.data_page_size > 0 &&
        ncb.block1.ncb1.data_page_size != NCB_BOOT_READ_SIZE) {
        sector_to_page_shift = ncb.block1.ncb1.sector_to_page_shift;
    }

    fw_size = fw_sectors * NCB_BOOT_READ_SIZE;
    fw_data = g_malloc(fw_size);
    copied = 0;

    for (uint32_t i = 0; i < fw_sectors; i++) {
        uint32_t sector_idx = fw_start_sector + i;
        page_row = sector_idx << sector_to_page_shift;

        if (!stmp3770_nand_read_page(gpmi, page_row,
                                     fw_data + copied,
                                     fw_size - copied)) {
            warn_report("STMP3770 boot: failed to read firmware page %u "
                        "(sector %u)", page_row, sector_idx);
            g_free(fw_data);
            return STMP3770_BOOT_RESULT_FAILED;
        }

        copied += MIN(page_size, fw_size - copied);
        if (copied >= fw_size) {
            break;
        }
    }

    info_report("STMP3770 boot: loaded %u bytes of firmware from NAND",
                copied);

    /* Step 4: Parse and execute the SB image. */
    bool ok = stmp3770_sb_parse_execute(s, fw_data, copied, entry);
    g_free(fw_data);

    return ok ? STMP3770_BOOT_RESULT_LOADED : STMP3770_BOOT_RESULT_FAILED;
}

/*
 * Load and parse an SB image from a file specified by the `sb-image`
 * machine property.  This bypasses the boot media search (NCB/LDLB/DBBT
 * for NAND, config block for SPI, etc.) and directly parses the SB image,
 * which is useful for testing the boot loader path without a full boot
 * media image.
 */
static STMP3770BootResult stmp3770_boot_load_sb_file(STMP3770BoardState *s,
                                                     hwaddr *entry)
{
    g_autofree char *path = NULL;
    gsize size;
    GError *err = NULL;
    uint8_t *data;

    if (!s->sb_image) {
        return STMP3770_BOOT_RESULT_FAILED;
    }

    path = g_strdup(s->sb_image);
    if (!g_file_get_contents(path, (gchar **)&data, &size, &err)) {
        warn_report("STMP3770 boot: could not read SB image '%s': %s",
                    path, err->message);
        g_error_free(err);
        return STMP3770_BOOT_RESULT_FAILED;
    }

    info_report("STMP3770 boot: loading SB image '%s' (%lu bytes)",
                path, (unsigned long)size);

    bool ok = stmp3770_sb_parse_execute(s, data, size, entry);
    g_free(data);

    return ok ? STMP3770_BOOT_RESULT_LOADED : STMP3770_BOOT_RESULT_FAILED;
}

static STMP3770BootResult stmp3770_boot_load(STMP3770BoardState *s,
                                             hwaddr *entry)
{
    STMP3770BootMode mode = s->boot_mode;

    /* Direct SB image loading takes priority over boot media search */
    if (s->sb_image) {
        return stmp3770_boot_load_sb_file(s, entry);
    }

    switch (mode) {
    case STMP3770_BOOT_MODE_USB:
    case STMP3770_BOOT_MODE_RECOVERY:
        return stmp3770_boot_load_usb(s, entry);
    case STMP3770_BOOT_MODE_I2C:
        return stmp3770_boot_load_i2c(s, entry);
    case STMP3770_BOOT_MODE_SPI1_FLASH:
        return stmp3770_boot_load_spi(s, entry, 1);
    case STMP3770_BOOT_MODE_SPI2_FLASH:
    case STMP3770_BOOT_MODE_SPI2_EEPROM:
        return stmp3770_boot_load_spi(s, entry, 2);
    case STMP3770_BOOT_MODE_SSP1_MMC:
        return stmp3770_boot_load_ssp(s, entry, 1);
    case STMP3770_BOOT_MODE_SSP2_MMC:
        return stmp3770_boot_load_ssp(s, entry, 2);
    case STMP3770_BOOT_MODE_GPMI_ECC4:
        return stmp3770_boot_load_gpmi(s, entry, false);
    case STMP3770_BOOT_MODE_GPMI_ECC8:
        return stmp3770_boot_load_gpmi(s, entry, true);
    case STMP3770_BOOT_MODE_JTAG_WAIT:
        info_report("STMP3770 boot: JTAG_WAIT mode, waiting for debugger");
        return STMP3770_BOOT_RESULT_WAIT;
    case STMP3770_BOOT_MODE_UNKNOWN:
    default:
        return STMP3770_BOOT_RESULT_FAILED;
    }
}

static STMP3770BootResult stmp3770_boot_run(STMP3770BoardState *s)
{
    hwaddr entry = STMP3770_SRAM_ADDR;

    s->boot_state = STMP3770_BOOT_STATE_INIT;
    s->boot_state = STMP3770_BOOT_STATE_SELECT;
    s->boot_mode = stmp3770_boot_select(s);
    info_report("STMP3770 boot: selected mode %s",
                stmp3770_boot_mode_name[s->boot_mode]);
    s->boot_state = STMP3770_BOOT_STATE_PORT_INIT;
    s->boot_state = STMP3770_BOOT_STATE_LOAD;

    STMP3770BootResult result = stmp3770_boot_load(s, &entry);
    if (result == STMP3770_BOOT_RESULT_LOADED) {
        s->boot_state = STMP3770_BOOT_STATE_EXEC;
        s->soc.cpu.env.regs[15] = entry;
    } else if (result == STMP3770_BOOT_RESULT_WAIT) {
        s->boot_state = STMP3770_BOOT_STATE_JTAG_WAIT;
    } else {
        s->boot_state = STMP3770_BOOT_STATE_FAILED;
    }

    return result;
}

static void stmp3770_board_get_boot_lcd_rs(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    STMP3770BoardState *s = STMP3770_BOARD(obj);
    uint8_t value = s->boot_lcd_rs;

    visit_type_uint8(v, name, &value, errp);
}

static void stmp3770_board_set_boot_lcd_rs(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    STMP3770BoardState *s = STMP3770_BOARD(obj);

    visit_type_uint8(v, name, &s->boot_lcd_rs, errp);
}

static void stmp3770_board_get_boot_lcd_data(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    STMP3770BoardState *s = STMP3770_BOARD(obj);
    uint8_t value = s->boot_lcd_data;

    visit_type_uint8(v, name, &value, errp);
}

static void stmp3770_board_set_boot_lcd_data(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    STMP3770BoardState *s = STMP3770_BOARD(obj);

    visit_type_uint8(v, name, &s->boot_lcd_data, errp);
}

static char *stmp3770_board_get_sb_image(Object *obj, Error **errp)
{
    STMP3770BoardState *s = STMP3770_BOARD(obj);

    return g_strdup(s->sb_image);
}

static void stmp3770_board_set_sb_image(Object *obj, const char *value,
                                        Error **errp)
{
    STMP3770BoardState *s = STMP3770_BOARD(obj);

    g_free(s->sb_image);
    s->sb_image = g_strdup(value);
}

static void stmp3770_board_init(MachineState *machine)
{
    STMP3770BoardState *s = STMP3770_BOARD(machine);
    MemoryRegion *sysmem = get_system_memory();
    Chardev *chr;
    g_autofree char *default_rom = stmp3770_default_file(STMP3770_DEFAULT_ROM_NAME);
    g_autofree char *default_flash =
        stmp3770_default_file(STMP3770_DEFAULT_FLASH_NAME);
    const char *firmware = machine->firmware;
    BlockBackend *default_flash_blk = NULL;

    /* Initialize the SoC */
    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_STMP3770);

    /* Connect debug UART to the first serial chardev if provided */
    chr = serial_hd(0);
    if (!chr) {
        /* In -nographic mode qemu_chr_new may not be wired to serial_hd(0) */
        chr = qemu_chr_new("stdio", "stdio", NULL);
    }
    if (chr) {
        qdev_prop_set_chr(DEVICE(s->soc.uartdbg), "chardev", chr);
    }

    /* Connect application UART to the second serial chardev if provided */
    chr = serial_hd(1);
    if (chr) {
        qdev_prop_set_chr(DEVICE(s->soc.uartapp), "chardev", chr);
    }

    /* Connect NAND drive to GPMI if one was provided with -drive if=none */
    {
        DriveInfo *dinfo = drive_get(IF_NONE, 0, 0);
        if (dinfo) {
            qdev_prop_set_drive(DEVICE(s->soc.gpmi), "drive",
                                blk_by_legacy_dinfo(dinfo));
        } else if (stmp3770_file_exists(default_flash)) {
            default_flash_blk = stmp3770_open_default_flash(default_flash);
            if (default_flash_blk) {
                qdev_prop_set_drive(DEVICE(s->soc.gpmi), "drive",
                                    default_flash_blk);
                fprintf(stderr, "stmp3770: using default flash %s\n",
                        default_flash);
            }
        }
    }

    if (!qdev_realize(DEVICE(&s->soc), NULL, &error_fatal)) {
        error_report("Failed to realize STMP3770 SoC");
        exit(1);
    }

    /*
     * Simulate Boot ROM initialization.
     *
     * The real STMP3770 mask ROM (SigmaTel OCROM, 64 KiB @ 0xFFFF0000)
     * performs a sequence of peripheral initialization before handing
     * control to the boot loader.  The sequence below replicates the
     * observable side effects extracted from the actual ROM dump:
     *
     *   - Release soft-reset/clock-gate on RTC, PINCTRL, POWER
     *   - Open/close OCOTP bank for shadow read
     *   - Configure Debug UART (115200 8N1 at 24 MHz XTAL)
     *   - Invalidate TLB and configure CP15 SCTLR (alignment check, etc.)
     *
     * This is registered as a reset handler so it runs AFTER all device
     * reset handlers, matching the real hardware where the ROM runs after
     * the reset deassertion sequence.
     */
    /* Schedule the ROM boot init to run after all device reset handlers
     * complete.  Using a bottom half ensures the init runs on the next
     * main loop iteration, which is after the reset phase.  This matches
     * real hardware where the mask ROM runs after reset deassertion. */
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            stmp3770_rom_boot_init, &s->soc);

    /*
     * CPU starts at 24MHz XTAL (not PLL)
     *
     * Real hardware: CPU runs at 24MHz until firmware enables PLL and switches
     * to high-frequency domain. CLKCTRL reset values:
     *   - CLKCTRL_CLKSEQ.BYPASS_CPU = 1 (use 24MHz XTAL, not PLL/480MHz)
     *   - CLKCTRL_PLLCTRL0.POWER = 0 (PLL disabled)
     *   - CLKCTRL_FRAC.CLKGATECPU = 1 (CPU clock gated by default)
     *
     * ExistOS Clk::init() sequence:
     *   1. Enable PLL (PLLCTRL0.POWER=1)
     *   2. Ungate CPU clock (FRAC.CLKGATECPU=0)
     *   3. Set temporary dividers (CPU=5, HBUS=4 → 96MHz/24MHz)
     *   4. Switch to PLL domain (CLKSEQ.BYPASS_CPU=0)
     *   5. Set final dividers (CPU frac=22, HBUS=2 → 392.7MHz/240MHz)
     *
     * Note: CLKCTRL reset() should handle these values. This comment documents
     * expected Boot ROM / reset state for reference.
     */

    /*
     * Run the ROM boot state machine.  It consumes LCD_RS/LCD_DATA[5:0],
     * OCOTP_ROM0 and RTC_PERSISTENT1.FORCE_RECOVERY to select a boot port
     * and, when a port loader succeeds, sets the CPU entry point.
     */
    {
        STMP3770BootResult result = stmp3770_boot_run(s);
        if (result == STMP3770_BOOT_RESULT_LOADED) {
            return;
        }
        if (result == STMP3770_BOOT_RESULT_WAIT) {
            return;
        }
    }

    /*
     * 3. No external DRAM on HP 39gII
     *
     * HP 39gII has NO external DRAM - only 512KB on-chip SRAM.
     * STMP3770 SoC has a DRAM controller (EMI) but it's unused on this board.
     *
     * ExistOS virtual memory (VM_RAM_BASE @ 0x02000000, 3MB) is mapped to
     * NAND Flash via MMU, not to physical DRAM. The DRAM address range
     * (0x40000000+) is unmapped and will cause data abort if accessed.
     *
     * For generic firmware that expects DRAM, machine->ram can be optionally
     * provided and mapped to 0x40000000, but this doesn't match HP 39gII
     * hardware.
     */
    if (machine->ram_size > 0) {
        /* Optional external DRAM for non-HP39gII firmware */
        memory_region_add_subregion(sysmem, STMP3770_DRAM_ADDR, machine->ram);
    }

    if (!firmware && !machine->kernel_filename) {
        if (stmp3770_file_exists(default_rom)) {
            firmware = default_rom;
            fprintf(stderr, "stmp3770: using default ROM %s\n", default_rom);
        } else {
            warn_report("No kernel specified and default ROM '%s' was not found",
                        default_rom);
        }
    }

    /* Load kernel or firmware if provided */
    if (firmware) {
        /* Firmware (e.g. HP39GII Hypervisor rom.bin) is linked to run from SRAM */
        if (load_image_targphys(firmware,
                                STMP3770_SRAM_ADDR,
                                STMP3770_SRAM_SIZE) < 0) {
            error_report("Failed to load firmware '%s'", firmware);
            exit(1);
        }

        /* Set entry point */
        s->soc.cpu.env.regs[15] = STMP3770_SRAM_ADDR;
    } else if (machine->kernel_filename) {
        /* Load kernel to SRAM (HP 39gII has no DRAM) */
        if (load_image_targphys(machine->kernel_filename,
                                STMP3770_SRAM_ADDR,
                                STMP3770_SRAM_SIZE) < 0) {
            error_report("Failed to load kernel '%s'", machine->kernel_filename);
            exit(1);
        }

        /* Set entry point */
        s->soc.cpu.env.regs[15] = STMP3770_SRAM_ADDR;
    } else {
        /* No kernel - CPU will try to boot from ROM/SRAM */
        warn_report("No kernel specified, CPU will execute from 0x0 (SRAM)");
    }
}

static void stmp3770_board_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "STMP3770 Development Board (HP 39gII Calculator)";
    mc->init = stmp3770_board_init;
    mc->max_cpus = 1;
    mc->min_cpus = 1;
    mc->default_cpus = 1;
    mc->is_default = true;
    mc->default_ram_size = STMP3770_BOARD_RAM_DEFAULT;
    mc->default_ram_id = "stmp3770.dram";

    object_class_property_add(oc, "boot-lcd-rs", "uint8",
                              stmp3770_board_get_boot_lcd_rs,
                              stmp3770_board_set_boot_lcd_rs,
                              NULL, NULL);
    object_class_property_set_description(oc, "boot-lcd-rs",
        "LCD_RS boot strap pin (0 = use OCOTP, 1 = use LCD_DATA[5:0])");

    object_class_property_add(oc, "boot-lcd-data", "uint8",
                              stmp3770_board_get_boot_lcd_data,
                              stmp3770_board_set_boot_lcd_data,
                              NULL, NULL);
    object_class_property_set_description(oc, "boot-lcd-data",
        "LCD_DATA[5:0] boot strap vector when boot-lcd-rs is 1");

    object_class_property_add_str(oc, "sb-image",
                                  stmp3770_board_get_sb_image,
                                  stmp3770_board_set_sb_image);
    object_class_property_set_description(oc, "sb-image",
        "Path to an SB (Safe Boot) image file for direct boot loader parsing");
}

static const TypeInfo stmp3770_board_type = {
    .name = TYPE_STMP3770_BOARD,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(STMP3770BoardState),
    .class_init = stmp3770_board_class_init,
};

static void stmp3770_board_register_types(void)
{
    type_register_static(&stmp3770_board_type);
}

type_init(stmp3770_board_register_types)
