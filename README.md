# MiniVSFS: A C-based VSFS Image Generator  

This project implements a **miniature, inode-based file system** called **MiniVSFS** along with two utilities:

- **mkfs_builder** — creates a raw MiniVSFS disk image.
- **mkfs_adder** — adds a file to an existing MiniVSFS disk image.

MiniVSFS is a simplified version of VSFS. It is block-based, uses a single root directory, and omits indirect blocks to keep the design minimal and educational.

---

## Features  

### mkfs_builder  
- Parses command-line parameters.  
- Creates a MiniVSFS file system image with a configurable size and inode count.  
- Outputs a byte-exact binary `.img` file.  

### mkfs_adder  
- Parses command-line parameters.  
- Opens an existing MiniVSFS image.  
- Adds a file from the current working directory to the root (`/`) directory of the image.  
- Outputs an updated binary image.  

---

## MiniVSFS Specifications  

- **Block Size:** 4096 bytes  
- **Inode Size:** 128 bytes  
- **Supported Directories:** Root (`/`) only  
- **Bitmaps:** One block each for inode and data bitmaps  
- **Direct Pointers:** 12 direct data blocks per inode  
- **Allocation Policy:** First-fit allocation  

### Disk Layout  

| Block | Contents      |
|-------|---------------|
| 0     | Superblock    |
| 1     | Inode bitmap  |
| 2     | Data bitmap   |
| 3…n   | Inode table   |
| …     | Data region   |

All on-disk structures are **little endian**.

---

## Command-Line Usage  

### mkfs_builder  

```bash
./mkfs_builder \
  --image out.img \
  --size-kib <180..4096> \
  --inodes <128..512>
```
--image : Name of the output image file.
--size-kib : Total size of the image in KiB (must be a multiple of 4).
--inodes : Number of inodes.

### mkfs_adder

```bash
./mkfs_adder \
  --input out.img \
  --output out2.img \
  --file <file>
```
--input : Input image file.
--output : Output image file.
--file : File (from current directory) to add to the file system.
