#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

#define FILE_TYPE_FILE 1
#define FILE_TYPE_DIR 2

#define MODE_FILE 0100000
#define MODE_DIR 0040000

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
    uint32_t checksum;
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
    uint64_t inode_crc;

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

int parse_args(int argc, char *argv[], char **input_file, char **output_file, char **file_to_add)
{
    *input_file = NULL;
    *output_file = NULL;
    *file_to_add = NULL;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc)
        {
            *input_file = argv[++i];
        }
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
        {
            *output_file = argv[++i];
        }
        else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc)
        {
            *file_to_add = argv[++i];
        }
        else
        {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return -1;
        }
    }

    if (!*input_file)
    {
        fprintf(stderr, "Error: --input parameter required\n");
        return -1;
    }
    if (!*output_file)
    {
        fprintf(stderr, "Error: --output parameter required\n");
        return -1;
    }
    if (!*file_to_add)
    {
        fprintf(stderr, "Error: --file parameter required\n");
        return -1;
    }

    return 0;
}

int find_free_bit(uint8_t *bitmap, uint64_t max_bits)
{
    for (uint64_t byte_idx = 0; byte_idx < (max_bits + 7) / 8; byte_idx++)
    {
        if (bitmap[byte_idx] != 0xFF)
        {
            for (int bit_idx = 0; bit_idx < 8; bit_idx++)
            {
                uint64_t bit_pos = byte_idx * 8 + bit_idx;
                if (bit_pos >= max_bits)
                    return -1;

                if (!(bitmap[byte_idx] & (1 << bit_idx)))
                {
                    return bit_pos;
                }
            }
        }
    }
    return -1;
}

void set_bit(uint8_t *bitmap, int bit_pos)
{
    int byte_idx = bit_pos / 8;
    int bit_idx = bit_pos % 8;
    bitmap[byte_idx] |= (1 << bit_idx);
}

long get_file_size(const char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (!f)
        return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    return size;
}

uint64_t blocks_needed_for_file(long file_size)
{
    if (file_size <= 0)
        return 0;
    return (file_size + BS - 1) / BS;
}

int main(int argc, char *argv[])
{
    crc32_init();

    char *input_file, *output_file, *file_to_add;

    if (parse_args(argc, argv, &input_file, &output_file, &file_to_add) != 0)
    {
        fprintf(stderr, "Usage: %s --input <file> --output <file> --file <file>\n", argv[0]);
        return 1;
    }

    if (access(file_to_add, F_OK) != 0)
    {
        fprintf(stderr, "Error: File '%s' not found\n", file_to_add);
        return 1;
    }

    long file_size = get_file_size(file_to_add);
    if (file_size < 0)
    {
        fprintf(stderr, "Error: Cannot read file '%s'\n", file_to_add);
        return 1;
    }

    uint64_t blocks_needed = blocks_needed_for_file(file_size);
    if (blocks_needed > DIRECT_MAX)
    {
        fprintf(stderr, "Error: File too large (needs %lu blocks, max %d)\n",
                blocks_needed, DIRECT_MAX);
        return 1;
    }

    FILE *input_img = fopen(input_file, "rb");
    if (!input_img)
    {
        perror("Error opening input image");
        return 1;
    }

    superblock_t superblock;
    if (fread(&superblock, sizeof(superblock_t), 1, input_img) != 1)
    {
        fprintf(stderr, "Error reading superblock\n");
        fclose(input_img);
        return 1;
    }

    if (superblock.magic != 0x4D565346)
    {
        fprintf(stderr, "Error: Invalid file system magic number\n");
        fclose(input_img);
        return 1;
    }

    fseek(input_img, 0, SEEK_END);
    long img_size = ftell(input_img);
    fseek(input_img, 0, SEEK_SET);

    uint8_t *image_data = malloc(img_size);
    if (!image_data)
    {
        fprintf(stderr, "Error: Cannot allocate memory for image\n");
        fclose(input_img);
        return 1;
    }

    if (fread(image_data, 1, img_size, input_img) != (size_t)img_size)
    {
        fprintf(stderr, "Error reading image data\n");
        free(image_data);
        fclose(input_img);
        return 1;
    }
    fclose(input_img);

    uint8_t *inode_bitmap = image_data + (superblock.inode_bitmap_start * BS);
    uint8_t *data_bitmap = image_data + (superblock.data_bitmap_start * BS);
    inode_t *inode_table = (inode_t *)(image_data + (superblock.inode_table_start * BS));
    uint8_t *data_region = image_data + (superblock.data_region_start * BS);

    int free_inode = find_free_bit(inode_bitmap, superblock.inode_count);
    if (free_inode < 0)
    {
        fprintf(stderr, "Error: No free inodes available\n");
        free(image_data);
        return 1;
    }

    uint32_t free_blocks[DIRECT_MAX];
    int blocks_found = 0;
    for (uint64_t i = 0; i < superblock.data_region_blocks && blocks_found < (int)blocks_needed; i++)
    {
        if (!(data_bitmap[i / 8] & (1 << (i % 8))))
        {
            free_blocks[blocks_found++] = superblock.data_region_start + i;
        }
    }

    if (blocks_found < (int)blocks_needed)
    {
        fprintf(stderr, "Error: Not enough free data blocks (need %lu, found %d)\n",
                blocks_needed, blocks_found);
        free(image_data);
        return 1;
    }

    FILE *file_fp = fopen(file_to_add, "rb");
    if (!file_fp)
    {
        perror("Error opening file to add");
        free(image_data);
        return 1;
    }

    inode_t *new_inode = &inode_table[free_inode];
    memset(new_inode, 0, sizeof(inode_t));

    time_t now = time(NULL);
    new_inode->mode = MODE_FILE;
    new_inode->links = 1;
    new_inode->uid = 0;
    new_inode->gid = 0;
    new_inode->size_bytes = file_size;
    new_inode->atime = now;
    new_inode->mtime = now;
    new_inode->ctime = now;

    for (int i = 0; i < blocks_found; i++)
    {
        new_inode->direct[i] = free_blocks[i];
    }
    for (int i = blocks_found; i < DIRECT_MAX; i++)
    {
        new_inode->direct[i] = 0;
    }

    new_inode->reserved_0 = 0;
    new_inode->reserved_1 = 0;
    new_inode->reserved_2 = 0;
    new_inode->proj_id = 0;
    new_inode->uid16_gid16 = 0;
    new_inode->xattr_ptr = 0;

    for (int i = 0; i < blocks_found; i++)
    {
        uint64_t block_offset = (free_blocks[i] - superblock.data_region_start) * BS;
        uint8_t *block_ptr = data_region + block_offset;

        size_t bytes_to_read = BS;
        if (i == blocks_found - 1)
        {
            bytes_to_read = file_size - (i * BS);
        }

        size_t bytes_read = fread(block_ptr, 1, bytes_to_read, file_fp);
        if (bytes_read != bytes_to_read)
        {
            fprintf(stderr, "Error reading file data\n");
            fclose(file_fp);
            free(image_data);
            return 1;
        }

        if (bytes_read < BS)
        {
            memset(block_ptr + bytes_read, 0, BS - bytes_read);
        }
    }
    fclose(file_fp);

    set_bit(inode_bitmap, free_inode);
    for (int i = 0; i < blocks_found; i++)
    {
        int data_block_idx = free_blocks[i] - superblock.data_region_start;
        set_bit(data_bitmap, data_block_idx);
    }

    inode_t *root_inode = &inode_table[0];
    uint8_t *root_data = data_region;

    dirent64_t *root_entries = (dirent64_t *)root_data;
    int entries_per_block = BS / sizeof(dirent64_t);
    int free_entry = -1;

    for (int i = 0; i < entries_per_block; i++)
    {
        if (root_entries[i].inode_no == 0)
        {
            free_entry = i;
            break;
        }
    }

    if (free_entry < 0)
    {
        fprintf(stderr, "Error: Root directory is full\n");
        free(image_data);
        return 1;
    }

    dirent64_t *new_entry = &root_entries[free_entry];
    memset(new_entry, 0, sizeof(dirent64_t));
    new_entry->inode_no = free_inode + 1;
    new_entry->type = FILE_TYPE_FILE;

    char *filename = basename(file_to_add);
    if (strlen(filename) >= 58)
    {
        fprintf(stderr, "Error: Filename too long (max 57 characters)\n");
        free(image_data);
        return 1;
    }
    strcpy(new_entry->name, filename);

    root_inode->size_bytes += sizeof(dirent64_t);
    root_inode->links++;
    root_inode->mtime = now;
    root_inode->ctime = now;

    inode_crc_finalize(new_inode);
    inode_crc_finalize(root_inode);
    dirent_checksum_finalize(new_entry);

    superblock.mtime_epoch = now;
    superblock_crc_finalize((superblock_t *)image_data);

    FILE *output_img = fopen(output_file, "wb");
    if (!output_img)
    {
        perror("Error creating output image");
        free(image_data);
        return 1;
    }

    if (fwrite(image_data, 1, img_size, output_img) != (size_t)img_size)
    {
        fprintf(stderr, "Error writing output image\n");
        fclose(output_img);
        free(image_data);
        return 1;
    }

    fclose(output_img);
    free(image_data);

    printf("File '%s' added to MiniVSFS image '%s' successfully\n", file_to_add, output_file);
    printf("Allocated inode: %d\n", free_inode + 1);
    printf("Allocated %lu data blocks\n", blocks_needed);

    return 0;
}
