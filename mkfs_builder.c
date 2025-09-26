// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_minivsfs.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u

uint64_t g_random_seed = 0; // This should be replaced by seed value from the CLI.

// below contains some basic structures you need for your project
// you are free to create more structures as you require

#pragma pack(push, 1)
typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;

    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint32_t checksum; // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push, 1)
typedef struct
{
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[12];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;

    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint64_t inode_crc; // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0

} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t) == INODE_SIZE, "inode size mismatch");

#pragma pack(push, 1)
typedef struct
{
    uint32_t inode_no;
    uint8_t type;
    char name[58];
    uint8_t checksum; // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t) == 64, "dirent size mismatch");

// File type
#define FILE_TYPE_FILE 1
#define FILE_TYPE_DIR 2

// Mode
#define MODE_FILE 0100000
#define MODE_DIR 0040000

// ==========================DO NOT CHANGE THIS PORTION=========================
// These functions are there for your help. You should refer to the specifications to see how you can use them.
// ====================================CRC32====================================
uint32_t CRC32_TAB[256];
void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        CRC32_TAB[i] = c;
    }
}
uint32_t crc32(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = CRC32_TAB[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
// ====================================CRC32====================================

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb)
{
    sb->checksum = 0;
    uint32_t s = crc32((void *)sb, BS - 4);
    sb->checksum = s;
    return s;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t *ino)
{
    uint8_t tmp[INODE_SIZE];
    memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t *de)
{
    const uint8_t *p = (const uint8_t *)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++)
        x ^= p[i]; // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}

int parse_args(int argc, char *argv[], char **image_file, uint64_t *size_kib, uint64_t *inodes)
{
    *image_file = NULL;
    *size_kib = 0;
    *inodes = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc)
        {
            *image_file = argv[++i];
        }
        else if (strcmp(argv[i], "--size-kib") == 0 && i + 1 < argc)
        {
            *size_kib = strtoull(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--inodes") == 0 && i + 1 < argc)
        {
            *inodes = strtoull(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
        {
            g_random_seed = strtoull(argv[++i], NULL, 10);
        }

        else
        {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return -1;
        }
    }

    if (!*image_file)
    {
        fprintf(stderr, "Error: --image parameter required\n");
        return -1;
    }
    if (*size_kib < 180 || *size_kib > 4096)
    {
        fprintf(stderr, "Error: --size-kib must be between 180 and 4096\n");
        return -1;
    }
    if (*size_kib % 4 != 0)
    {
        fprintf(stderr, "Error: --size-kib must be a multiple of 4\n");
        return -1;
    }
    if (*inodes < 128 || *inodes > 512)
    {
        fprintf(stderr, "Error: --inodes must be between 128 and 512\n");
        return -1;
    }

    return 0;
}

// superblk create
void create_superblock(superblock_t *sb, uint64_t size_kib, uint64_t inode_count)
{
    memset(sb, 0, sizeof(superblock_t));

    uint64_t total_blocks = (size_kib * 1024) / BS;
    uint64_t inode_table_blocks = (inode_count * INODE_SIZE + BS - 1) / BS;

    // setting val for superblk
    sb->magic = 0x4D565346;
    sb->version = 1;
    sb->block_size = BS;
    sb->total_blocks = total_blocks;
    sb->inode_count = inode_count;
    sb->inode_bitmap_start = 1;
    sb->inode_bitmap_blocks = 1;
    sb->data_bitmap_start = 2;
    sb->data_bitmap_blocks = 1;
    sb->inode_table_start = 3;
    sb->inode_table_blocks = inode_table_blocks;
    sb->data_region_start = 3 + inode_table_blocks;
    sb->data_region_blocks = total_blocks - (3 + inode_table_blocks);
    sb->root_inode = ROOT_INO;
    sb->mtime_epoch = time(NULL);
    sb->flags = (uint32_t)rand();
}

// root dir inode create
void create_root_inode(inode_t *root_ino, uint64_t data_block)
{
    memset(root_ino, 0, sizeof(inode_t));
    // setting val for inode
    time_t now = time(NULL);
    root_ino->mode = MODE_DIR;
    root_ino->links = 2;
    root_ino->uid = 0;
    root_ino->gid = 0;
    root_ino->size_bytes = 2 * sizeof(dirent64_t); // . and .. entry
    root_ino->atime = now;
    root_ino->mtime = now;
    root_ino->ctime = now;
    root_ino->direct[0] = data_block; // First data block point

    for (int i = 1; i < 12; i++)
    {
        root_ino->direct[i] = 0;
    }
    root_ino->reserved_0 = 0;
    root_ino->reserved_1 = 0;
    root_ino->reserved_2 = 0;
    root_ino->proj_id = 1;
    root_ino->uid16_gid16 = 0;
    root_ino->xattr_ptr = 0;
}

void create_root_directory_entries(dirent64_t *entries)
{
    //  "." entry
    memset(&entries[0], 0, sizeof(dirent64_t));
    entries[0].inode_no = ROOT_INO;
    entries[0].type = FILE_TYPE_DIR;
    strcpy(entries[0].name, ".");

    // ".." entry
    memset(&entries[1], 0, sizeof(dirent64_t));
    entries[1].inode_no = ROOT_INO;
    entries[1].type = FILE_TYPE_DIR;
    strcpy(entries[1].name, "..");

    dirent_checksum_finalize(&entries[0]);
    dirent_checksum_finalize(&entries[1]);
}

int main(int argc, char *argv[])
{
    crc32_init();

    char *image_file;
    uint64_t size_kib, inode_count;

    // command line argument  Parsing
    if (parse_args(argc, argv, &image_file, &size_kib, &inode_count) != 0)
    {
        fprintf(stderr, "Usage: %s --image <file> --size-kib <180..4096> --inodes <128..512>\n", argv[0]);
        return 1;
    }
    // 🔹 Initialize random seed
    if (g_random_seed == 0)
        g_random_seed = (uint64_t)time(NULL);

    srand((unsigned)g_random_seed);

    uint64_t total_blocks = (size_kib * 1024) / BS;
    uint64_t inode_table_blocks = (inode_count * INODE_SIZE + BS - 1) / BS;
    uint64_t data_region_start = 3 + inode_table_blocks;
    uint64_t data_region_blocks = total_blocks - data_region_start;

    // storage chck
    if (data_region_blocks < 1)
    {
        fprintf(stderr, "Error: Not enough space for data region\n");
        return 1;
    }

    // creating superblk, inode, root_dict
    superblock_t superblock;
    create_superblock(&superblock, size_kib, inode_count);
    superblock_crc_finalize(&superblock);

    inode_t root_inode;
    create_root_inode(&root_inode, data_region_start);
    inode_crc_finalize(&root_inode);

    dirent64_t root_entries[2];
    create_root_directory_entries(root_entries);

    FILE *img_file = fopen(image_file, "wb");
    if (!img_file)
    {
        perror("Error opening output file");
        return 1;
    }

    uint8_t block_buffer[BS];
    memset(block_buffer, 0, BS);
    memcpy(block_buffer, &superblock, sizeof(superblock_t));
    if (fwrite(block_buffer, 1, BS, img_file) != BS)
    {
        perror("Error writing superblock");
        fclose(img_file);
        return 1;
    }

    memset(block_buffer, 0, BS);

    block_buffer[0] = 0x01;
    if (fwrite(block_buffer, 1, BS, img_file) != BS)
    {
        perror("Error writing inode bitmap");
        fclose(img_file);
        return 1;
    }

    memset(block_buffer, 0, BS);

    block_buffer[0] = 0x01; // First bit set
    if (fwrite(block_buffer, 1, BS, img_file) != BS)
    {
        perror("Error writing data bitmap");
        fclose(img_file);
        return 1;
    }

    for (uint64_t i = 0; i < inode_table_blocks; i++)
    {
        memset(block_buffer, 0, BS);

        if (i == 0)
        {
            memcpy(block_buffer, &root_inode, sizeof(inode_t));
        }

        if (fwrite(block_buffer, 1, BS, img_file) != BS)
        {
            perror("Error writing inode table");
            fclose(img_file);
            return 1;
        }
    }

    //  data regiont
    for (uint64_t i = 0; i < data_region_blocks; i++)
    {
        memset(block_buffer, 0, BS);

        if (i == 0)
        {
            memcpy(block_buffer, root_entries, 2 * sizeof(dirent64_t));
        }

        if (fwrite(block_buffer, 1, BS, img_file) != BS)
        {
            perror("Error writing data region");
            fclose(img_file);
            return 1;
        }
    }

    fclose(img_file);

    printf("MiniVSFS image '%s' created successfully\n", image_file);
    printf("Total size: %lu KB (%lu blocks)\n", size_kib, total_blocks);
    printf("Inodes: %lu\n", inode_count);
    printf("Data blocks available: %lu\n", data_region_blocks - 1);

    return 0;
}
