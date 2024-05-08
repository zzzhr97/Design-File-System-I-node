#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <asm/types.h>
#include <fcntl.h>
#include <ctype.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <time.h>

// An important tip:
//      While using socket, must use interleaved read and write.
//      Such as: write - read - write - read - ...
//      Otherwise, if we use write twice in client , the server will
//      read twice. But the server may only get ONE message instead
//      of TWO. Because BEFORE the second read by server, the second
//      message may already be sent by client, which will make the
//      TWO messages into ONE message!
//      In this case, the server will infinitely wait for the second
//      message, and the client will infinitely wait for the new
//      message from server. Thus, the program will be stuck in an
//      DEADLOCK!
//      So, we need interleaving read / write operation for the
//      purpose of realizing communication synchronization.

// =================================================================
// Modify this part to get different output type.

// If OUTPUT_TEST is 1, after each command,
// output some information to screen including:
//      inode number
//      block number
//      current directory
//      inodes and blocks information (in the front)
#define OUTPUT_TEST 0

// If OUTPUT_STDOUT is 1, output information to screen and log file.
// Otherwise, only output to log file.
#define OUTPUT_STDOUT 1

// If both OUTPUT_DETAIL and OUTPUT_STOUT are 1, output details while inputting 'ls'.
// Otherwise, only output name.
#define OUTPUT_DETAIL 1

// If SOCKET_OPEN is 1, use socket.
#define SOCKET_OPEN 1

// If TELNET_TEST is 1, use telnet directly to test.
#define TELNET_TEST 0
// =================================================================

static int BLOCK_NUM;       // block number
#define BLOCK_SIZE 256      // block size: 256 Bytes = 2048 bits
static int INODE_NUM;       // inode number
#define INODE_SIZE 64       // inode size: 64 Bytes = 512 bits

#define MAX_LEN 10000       // The maximum length of input
#define MAX_DATA_LEN 8192   // The maximum length of input data
#define MAX_PATH_LEN 64     // The maximum length of input path

// The i_info of a file inode which has all allowed authority.
#define INFO_FILE_ALL_ALLOW 0x27e00
// The i_info of a directory inode which has all allowed authority.
#define INFO_DIR_ALL_ALLOW 0x27e01

// super_block
// 160 bits
struct b_super_block {
    __u32 s_count_inode;
    __u32 s_count_block;
    __u32 s_count_free_inode;
    __u32 s_count_free_block;
    __u32 s_inode_root;
};

// inode_bitmap
// 1024 bits
struct bitmap_inode {
    __u8 i_valid_bit[128];
};

// block_bitmap
// 2048 bits
struct bitmap_block {
    __u8 b_valid_bit[256];
};

// i_node
// 64 Bytes = 512 bits
struct i_node {
    __u32 i_info;               // base information
    __u8 i_name[16];            // file / dir name
    __u32 i_size_file;          // file size
    __u32 i_time_access;        // access time
    __u32 i_time_modify;        // access time
    __u32 i_time_change;        // change time
    __u16 i_num_block;          // data block number (except indirect)
    __u16 i_num_link;           // link number
    __u16 i_inode_parent_dir;   // parent dir
    __u16 i_block_direct[8];    // direct block
    __u16 i_block_single;       // single block
    __u16 i_block_double;       // double block
    __u16 i_block_triple;       // triple block
};

// b_block
// can contain 128 inode/block index
// 256 Bytes = 2048 bits
struct b_block {
    char b_data[256];
};

// index_name
// A struct of index and its name.
// Not stored in storage system.
struct index_name {
    __u16 index;
    char name[16];
};

static int disk_block_num;      // the total number of disk blocks
static int cylinders;           // the number of cylinders
static int sectors_pc;          // the number of sectors per cylinder
static int disk_sockfd;         // socket with disk.c
static int client_sockfd;       // socket with client.c
static int client_newsockfd;    // new socket with client.c
static socklen_t clilen;
struct sockaddr_in serv_addr;
struct sockaddr_in cli_addr;
static char buffer[MAX_LEN];
static char client_buffer[MAX_LEN];     // buffer read from client.c
static char client_buffer_w[MAX_LEN];   // buffer write to client.c
static char disk_buffer[MAX_LEN];       // buffer read from disk.c
static char disk_buffer_w[MAX_LEN];     // buffer write to disk.c

static struct b_super_block super_block;    // super block
static struct bitmap_inode inode_bitmap;    // inode bitmap
static struct bitmap_block block_bitmap;    // block_bitmap
static struct i_node inode[1024];   // inodes
static struct b_block block[2048];  // blocks
static FILE *fs_log;                // file id of fs.log
static __u16 cur_dir;               // current directory
static int cur_usr = 0;                 // current user

// Write to client.c.
void server_write() {
    int n = write(client_newsockfd, client_buffer_w, strlen(client_buffer_w));
    bzero(client_buffer_w, MAX_LEN);
    if (n < 0) {
        printf("Error: writing to socket.\n");
        exit(-1);
    }
}

// Read from client.c.
void server_read(char buffer[MAX_LEN]) {
    bzero(buffer, MAX_LEN);
    int n = read(client_newsockfd, buffer, MAX_LEN);
    if (n < 0) {
        printf("Error: reading from socket.\n");
        exit(-1);
    }
    printf("%s", buffer);
}

// Write to disk.c. (disk_buffer_w)
void client_write(int length) {
    int n = write(disk_sockfd, disk_buffer_w, length);
    if (n < 0) {
        printf("Error: writing to socket.\n");
        exit(-1);
    }
}

// Read from disk.c. (disk_buffer)
void client_read() {
    int n = read(disk_sockfd, disk_buffer, MAX_LEN);
    if (n < 0) {
        printf("Error: reading from socket.\n");
        exit(-1);
    }
}

// Write a block to disk.c.
// Already store the data in 'disk_buffer_w'.
//      If exceed disk capacity, return 0.
//      If write completed, return 1.
int write_to_disk(int disk_block_index) {
    if (disk_block_index >= disk_block_num)
        return 0;

    // W c s data
    int c = disk_block_index / sectors_pc;
    int s = disk_block_index % sectors_pc;
    char c_str[64], s_str[64], tmp[MAX_LEN];
    sprintf(c_str, "%d ", c);
    sprintf(s_str, "%d ", s);
    sprintf(tmp, "W ");
    int length = BLOCK_SIZE + strlen(tmp) + strlen(c_str) + strlen(s_str) + 1;

    strcat(tmp, c_str);
    strcat(tmp, s_str);
    memcpy(tmp + strlen(tmp), disk_buffer_w, BLOCK_SIZE);
    bzero(disk_buffer_w, MAX_LEN);
    memcpy(disk_buffer_w, tmp, length - 1);
    disk_buffer_w[length - 1] = '\n';
    disk_buffer_w[length] = '\0';

    // write command to disk.c
    client_write(length);
    bzero(disk_buffer_w, MAX_LEN);

    // wait for output from disk.c
    bzero(disk_buffer, MAX_LEN);
    client_read();

    return 1;
}

// Read a block from disk.c.
// Data will be stored in 'disk_buffer'.
//      If exceed disk capacity, return 0.
//      If read completed, return 1.
int read_from_disk(int disk_block_index) {
    if (disk_block_index >= disk_block_num)
        return 0;

    // R c s
    int c = disk_block_index / sectors_pc;
    int s = disk_block_index % sectors_pc;
    char c_str[64], s_str[64];
    bzero(disk_buffer_w, MAX_LEN);
    sprintf(c_str, "%d ", c);
    sprintf(s_str, "%d", s);
    sprintf(disk_buffer_w, "R ");

    strcat(disk_buffer_w, c_str);
    strcat(disk_buffer_w, s_str);

    // write command to disk.c
    client_write(strlen(disk_buffer_w));
    bzero(disk_buffer_w, MAX_LEN);

    // read from disk.c to act as a division
    // This is to avoid the case mentioned at row 19.
    bzero(disk_buffer, MAX_LEN);
    client_read();

    return 1;
}

// Write something to disk.c.
//      fg = 0: super block
//      fg = 1: inode bitmap
//      fg = 2: block bitmap
//      fg = 3: inode
//      fg = 4: block
void write_disk(int fg, int index) {
    if (!SOCKET_OPEN)
        return;
    bzero(disk_buffer_w, MAX_LEN);
    int disk_block_index;
    switch (fg) {
        case 0:     // super block
            disk_block_index = 0;
            memcpy(disk_buffer_w, &super_block, 4 * 5);
            break;
        case 1:     // inode bitmap
            disk_block_index = 1;
            memcpy(disk_buffer_w, inode_bitmap.i_valid_bit, 128);
            break;
        case 2:     // block bitmap
            disk_block_index = 2;
            memcpy(disk_buffer_w, block_bitmap.b_valid_bit, 256);
            break;
        case 3:     // inode
            index = index / 4;      // 1 block contains 4 inodes
            disk_block_index = 3 + index;
            memcpy(disk_buffer_w, &inode[index * 4], 64);
            memcpy(disk_buffer_w + 64, &inode[index * 4 + 1], 64);
            memcpy(disk_buffer_w + 128, &inode[index * 4 + 2], 64);
            memcpy(disk_buffer_w + 192, &inode[index * 4 + 3], 64);
            break;
        case 4:     // block
            disk_block_index = 3 + INODE_NUM / 4 + index;
            memcpy(disk_buffer_w, &block[index], 256);
    }

    int ret = write_to_disk(disk_block_index);
    if (ret == 0) {
        printf("Error: exceed!\n");
    }
}

// Read something from disk.c.
//      fg = 0: super block
//      fg = 1: inode bitmap
//      fg = 2: block bitmap
//      fg = 3: inode
//      fg = 4: block
void read_disk(int fg, int index) {
    if (!SOCKET_OPEN)
        return;
    bzero(disk_buffer_w, MAX_LEN);
    int disk_block_index;

    // calculate disk block index
    switch (fg) {
        case 0:     // super block
            disk_block_index = 0;
            break;
        case 1:     // inode bitmap
            disk_block_index = 1;
            break;
        case 2:     // block bitmap
            disk_block_index = 2;
            break;
        case 3:     // inode
            index = index / 4;      // 1 block contains 4 inodes
            disk_block_index = 3 + index;
            break;
        case 4:     // block
            disk_block_index = 3 + INODE_NUM / 4 + index;
    }

    // read a block from disk
    int ret = read_from_disk(disk_block_index);

    // write data to corresponding space
    switch (fg) {
        case 0:     // super block
            memcpy(&super_block, disk_buffer, 4 * 5);
            break;
        case 1:     // inode bitmap
            memcpy(inode_bitmap.i_valid_bit, disk_buffer, 128);
            break;
        case 2:     // block bitmap
            memcpy(block_bitmap.b_valid_bit, disk_buffer, 256);
            break;
        case 3:     // inode
            // already subtract inode index by 4, do not need to subtract
            memcpy(&inode[index * 4], disk_buffer, 64);
            memcpy(&inode[index * 4 + 1], disk_buffer + 64, 64);
            memcpy(&inode[index * 4 + 2], disk_buffer + 128, 64);
            memcpy(&inode[index * 4 + 3], disk_buffer + 192, 64);
            break;
        case 4:     // block
            memcpy(&block[index], disk_buffer, 256);
    }
    if (ret == 0) {
        printf("Error: exceed!\n");
    }
}

// Get cylinders and sectors_pc.
void get_disk_org() {
    if (!SOCKET_OPEN) {
        cylinders = 64;
        sectors_pc = 128;
        disk_block_num = cylinders * sectors_pc;
        return;
    }
    bzero(disk_buffer_w, MAX_LEN);  // clear buffer
    int i = 0;

    // write 'I' to disk.c
    sprintf(disk_buffer_w, "I");
    client_write(1);

    // read data from disk.c
    client_read();
    while (disk_buffer[i] != ' ') {
        i++;
    }
    disk_buffer[i] = '\0';
    cylinders = atoi(disk_buffer);
    disk_buffer[i] = ' ';
    strcpy(disk_buffer, disk_buffer + i + 1);
    sectors_pc = atoi(disk_buffer);

    bzero(disk_buffer, MAX_LEN);    // clear buffer

    disk_block_num = cylinders * sectors_pc;

    if (disk_block_num >= 3600)
        INODE_NUM = 1024;
    else
        INODE_NUM = disk_block_num / 3 - 2;
    BLOCK_NUM = 2 * INODE_NUM;
}

// Initialize super block.
void init_super_block() {
    super_block.s_count_inode = 0;
    super_block.s_count_block = 0;
    super_block.s_count_free_inode = INODE_NUM;
    super_block.s_count_free_block = BLOCK_NUM;
    super_block.s_inode_root = 0;
    write_disk(0, 0);
}

// Initialize inode bitmap.
void init_inode_bitmap() {
    for (int i = 0; i < 128; i++)
        inode_bitmap.i_valid_bit[i] = 0;
    write_disk(1, 0);
}

// Initialize block bitmap.
void init_block_bitmap() {
    for (int i = 0; i < 256; i++)
        block_bitmap.b_valid_bit[i] = 0;
    write_disk(2, 0);
}

// Initialize block.
void init_block() {
    for (int i = 0; i < BLOCK_NUM; i++)
        for (int j = 0; j < BLOCK_SIZE; j++)
            block[i].b_data[j] = '\0';
}

// Modify super block.
// fg = 0: modify inode number.
// fg = 1: modify block number.
int modify_super_block(int fg, int modify_count) {
    read_disk(0, 0);

    if (fg == 0) {  // update inode number
        int count_free_inode = super_block.s_count_free_inode;
        count_free_inode -= modify_count;
        if (count_free_inode < 0)
            return -1;
        super_block.s_count_inode += modify_count;
        super_block.s_count_free_inode = count_free_inode;
    } else {    // update block number
        int count_free_block = super_block.s_count_free_block;
        count_free_block -= modify_count;
        if (count_free_block < 0)
            return -1;
        super_block.s_count_block += modify_count;
        super_block.s_count_free_block = count_free_block;
    }
    write_disk(0, 0);
    return 1;
}

// Modify a bit of a __u8 to 0 or 1.
// 'digit' (0 ~ 7) indicates the location of bit.
__u8 modify_bitmap(__u8 bits, int digit, int valid_bit) {
    if (valid_bit == 0) {
        switch (digit) {
            case 0:
                bits = bits & 0b11111110;
                break;
            case 1:
                bits = bits & 0b11111101;
                break;
            case 2:
                bits = bits & 0b11111011;
                break;
            case 3:
                bits = bits & 0b11110111;
                break;
            case 4:
                bits = bits & 0b11101111;
                break;
            case 5:
                bits = bits & 0b11011111;
                break;
            case 6:
                bits = bits & 0b10111111;
                break;
            case 7:
                bits = bits & 0b01111111;
        }
    }
    if (valid_bit == 1) {
        switch (digit) {
            case 0:
                bits = bits | 0b00000001;
                break;
            case 1:
                bits = bits | 0b00000010;
                break;
            case 2:
                bits = bits | 0b00000100;
                break;
            case 3:
                bits = bits | 0b00001000;
                break;
            case 4:
                bits = bits | 0b00010000;
                break;
            case 5:
                bits = bits | 0b00100000;
                break;
            case 6:
                bits = bits | 0b01000000;
                break;
            case 7:
                bits = bits | 0b10000000;
        }
    }
    return bits;
}

// Modify inode bitmap and super block.
void modify_inode_bitmap(int i_index, int i_valid_bit) {
    read_disk(1, 0);

    int i = i_index / 8;
    int digit = i_index % 8;
    if (i_valid_bit == 1)
        modify_super_block(0, 1);
    else
        modify_super_block(0, -1);
    inode_bitmap.i_valid_bit[i] = modify_bitmap(inode_bitmap.i_valid_bit[i],
                                                digit, i_valid_bit);

    write_disk(1, 0);
}

// Modify block bitmap and super block.
void modify_block_bitmap(int b_index, int b_valid_bit) {
    read_disk(2, 0);

    int i = b_index / 8;
    int digit = b_index - i;
    if (b_valid_bit == 1)
        modify_super_block(1, 1);
    else
        modify_super_block(1, -1);
    block_bitmap.b_valid_bit[i] = modify_bitmap(block_bitmap.b_valid_bit[i],
                                                digit, b_valid_bit);

    write_disk(2, 0);
}

// Convert an inode index (__u16) to a string.
void itoa_2(__u16 i_index, char i_index_ch[2]) {
    i_index_ch[0] = (char) (i_index % (1 << 8));
    i_index_ch[1] = (char) (i_index / (1 << 8));
}

// Convert a string to an inode index (__u16).
__u16 atoi_2(char i_index_ch[2]) {
    __u16 i_index;
    i_index = ((__u16) i_index_ch[0]) & 0x00ff;     // must & 0x00ff
    i_index += ((__u16) i_index_ch[1]) * (1 << 8);
    return i_index;
}

// Clear given inode's access time with 0.
// That is because function 'build_inode'
//      will set all the information to 0
//      except access time of this inode.
void clear_inode(__u16 free_inode_index) {
    read_disk(3, free_inode_index);

    inode[free_inode_index].i_time_access = 0;

    write_disk(3, free_inode_index);
}

// Clear given block with '\0'.
void clear_block(__u16 free_block_index) {
    for (int i = 0; i < 256; i++)
        block[free_block_index].b_data[i] = '\0';

    write_disk(4, free_block_index);
}

// Find the first 0 of a binary number.
// length: 128 --> inode bitmap, 256 --> block bitmap
__u16 __find_free(__u8 data[], int length) {
    __u8 detection;
    for (int i = 0; i < length; i++) {
        detection = 0b00000001;
        for (int j = 0; j < 8; j++) {
            if ((detection & data[i]) == 0)
                return (i * 8 + j);
            detection = detection << 1;
        }
    }
}

// Find a free inode index by inode bitmap.
__u16 find_free_inode() {
    read_disk(1, 0);

    __u16 free_inode_index = __find_free(inode_bitmap.i_valid_bit, 128);

    modify_inode_bitmap(free_inode_index, 1);

    clear_inode(free_inode_index);
    return free_inode_index;
}

// Find a free block index by block bitmap.
// Clear this block with '\0'.
__u16 find_free_block() {
    read_disk(2, 0);

    __u16 free_block_index = __find_free(block_bitmap.b_valid_bit, 256);
    modify_block_bitmap(free_block_index, 1);
    clear_block(free_block_index);
    return free_block_index;
}

// Check the digit-th bit of 'bits' is 1 or 0.
// If 1, return 1, 2, 4, 8, 16, 32, 64, 128.
// If 0, return 0.
int __check_valid(__u8 bits, int digit) {
    switch (digit) {
        case 0:
            return bits & 0b00000001;
        case 1:
            return bits & 0b00000010;
        case 2:
            return bits & 0b00000100;
        case 3:
            return bits & 0b00001000;
        case 4:
            return bits & 0b00010000;
        case 5:
            return bits & 0b00100000;
        case 6:
            return bits & 0b01000000;
        case 7:
            return bits & 0b10000000;
    }
}

// If the inode is valid, return value > 0.
// Otherwise, return 0.
int check_valid_inode(__u16 i_index) {
    read_disk(1, 0);

    int digit = i_index % 8;
    __u8 bits = inode_bitmap.i_valid_bit[i_index / 8];
    return __check_valid(bits, digit);
}

// If the block is valid, return value > 0.
// Otherwise, return 0.
int check_valid_block(__u16 b_index) {
    read_disk(2, 0);

    int digit = b_index % 8;
    __u8 bits = block_bitmap.b_valid_bit[b_index / 8];
    return __check_valid(bits, digit);
}

// Update time. (Internal function)
void __update_time(__u32 *_time) {
    time_t cur_timer;
    time(&cur_timer);
    struct tm *cur_time = localtime(&cur_timer);
    int year = cur_time->tm_year + 1900;
    int mon = cur_time->tm_mon + 1;
    int mday = cur_time->tm_mday;
    int hour = (cur_time->tm_hour + 15) % 24;   // Correct hour.
    int min = cur_time->tm_min;
    int sec = cur_time->tm_sec;

    mon = mon << 6;
    mday = mday << 10;
    hour = hour << 15;
    min = min << 20;
    sec = sec << 26;
    *_time = year - 2000 + mon + mday + hour + min + sec;
}

// Update time.
// fg = 1: access time
// fg = 2: modify time
// fg = 3: change time
void update_time(int fg, __u16 i_index) {
    read_disk(3, i_index);

    switch (fg) {
        case 0:     // access time
            __update_time(&inode[i_index].i_time_access);
            break;
        case 1:     // modify time
            __update_time(&inode[i_index].i_time_modify);
            break;
        case 2:     // inode change time
            __update_time(&inode[i_index].i_time_change);
    }

    write_disk(3, i_index);
}

// Update time. This will modify all the parent inode.
void update_time_total(int fg, __u16 i_index) {
    while (i_index != 0) {
        update_time(fg, i_index);
        // do not need to read disk because update_time read already
        i_index = inode[i_index].i_inode_parent_dir;
    }
    update_time(fg, i_index);
}

// Find a free block.
// Store the free block index to loc-th in given block 'b_index'.
// loc: 0, 2, 4, ..., 254
// Return: the newly found free block index.
__u16 find_and_set(__u16 b_index, int loc) {
    read_disk(4, b_index);

    __u16 b_index_new = find_free_block();
    char b_index_ch[2];
    itoa_2(b_index_new, b_index_ch);
    block[b_index].b_data[loc] = b_index_ch[0];
    block[b_index].b_data[loc + 1] = b_index_ch[1];

    write_disk(4, b_index);
    return b_index_new;
}

/// Find the index at the location 'loc' in given block.
// Convert this string index to a __u16 index.
// loc: 0, 2, 4, ..., 254
// Return: the index in the 'loc' in given block.
__u16 find_and_convert(__u16 b_index, int loc) {
    read_disk(4, b_index);

    char b_index_ch[2];
    b_index_ch[0] = block[b_index].b_data[loc];
    b_index_ch[1] = block[b_index].b_data[loc + 1];
    return atoi_2(b_index_ch);
}

// Convert virtual block index to physical block index.
// Virtual block index: 0, 1, 2, ...
__u16 find_block_index(__u16 i_index, __u16 b_index_v) {
    read_disk(3, i_index);

    if (b_index_v < 8) {    // direct block
        return inode[i_index].i_block_direct[b_index_v];
    } else if (b_index_v < 8 + 128) {   // single indirect block
        int single_index_v = b_index_v - 8;   // 0 ~ 127
        return find_and_convert(inode[i_index].i_block_single, single_index_v * 2);
    } else if (b_index_v < 8 + 128 + 128 * 128) {   // double indirect block
        int double_index_v = (b_index_v - 8 - 128) / 128;   // 0 ~ 127
        int single_index_v = (b_index_v - 8 - 128) % 128;   // 0 ~ 127
        __u16 single_index = find_and_convert(inode[i_index].i_block_double, double_index_v * 2);
        return find_and_convert(single_index, single_index_v * 2);
    } else if (b_index_v < 8 + 128 + 128 * 128 + 128 * 128 * 128) { // triple indirect block
        int triple_index_v = (b_index_v - 8 - 128 - 128 * 128) / (128 * 128);   // 0 ~ 127
        int double_index_v = (b_index_v - 8 - 128 - 128 * 128) / 128 % 128;     // 0 ~ 127
        int single_index_v = (b_index_v - 8 - 128 - 128 * 128) % 128;           // 0 ~ 127
        __u16 double_index = find_and_convert(inode[i_index].i_block_triple, triple_index_v * 2);
        __u16 single_index = find_and_convert(double_index, double_index_v * 2);
        return find_and_convert(single_index, single_index_v * 2);
    }
}

// Convert virtual inode index to physical inode index. (directory file)
// Virtual inode index: 0, 1, 2, ...
__u16 find_inode_index(__u16 i_index, __u16 i_index_v) {
    __u16 b_index_v = i_index_v / 128;
    __u16 b_index = find_block_index(i_index, b_index_v);
    return find_and_convert(b_index, (i_index_v % 128) * 2);
}

// Check the name is repeated or not in directory.
// If find it, return virtual inode index.
// Otherwise, return -1.
int check_repeat(__u16 i_index, char name[16]) {
    read_disk(3, i_index);

    int size = inode[i_index].i_size_file;
    __u16 i_index_p;    // physical inode index
    for (int i_index_v = 0; i_index_v < size / 2; i_index_v++) {
        // i_index_v: virtual inode index. (0-indexed)
        i_index_p = find_inode_index(i_index, i_index_v);
        if (strcmp(name, inode[i_index_p].i_name) == 0)
            return i_index_v;
    }
    return -1;
}

// go to directory and update access time.
void goto_dir(__u16 i_index) {
    cur_dir = i_index;
    update_time_total(0, cur_dir);  // update access time
}

// change directory
// If not found or is file, return 0.
// Otherwise, return 1.
int change_dir(char name[16]) {
    read_disk(3, cur_dir);

    // handle '.'
    if (strcmp(name, ".") == 0)
        return 1;

    // handle '..'
    if (strcmp(name, "..") == 0) {
        goto_dir(inode[cur_dir].i_inode_parent_dir);
        return 1;
    }

    // get virtual index
    int i_index_v = check_repeat(cur_dir, name);
    if (i_index_v < 0)
        return 0;

    // get physical index
    int i_index = find_inode_index(cur_dir, i_index_v);

    // check directory or file
    read_disk(3, i_index);
    if (inode[i_index].i_info % 2 == 0)
        return 0;

    // go to directory
    goto_dir(i_index);

    return 1;
}

// Insert data in virtual block 'b_index_v'.
// Range: from 'pos' to the end OR insert all the data.
// Data will be truncated.
// pos: 0, 1, ..., 255
void insert_data_to_block(__u16 i_index, __u16 b_index_v, int pos, int *l, char data[MAX_DATA_LEN]) {
    __u16 b_index = find_block_index(i_index, b_index_v);

    read_disk(4, b_index);

    int length = *l;                // 0 ~ length - 1
    int len = length;               // store length
    if (pos + length > 256) {       // data cannot be totally stored in this block
        length = 256 - pos;
        memcpy(block[b_index].b_data + pos, data, length);  // insert data to block
        memcpy(data, data + length, len - length); // truncate data
        *l = *l - length;
    } else {    // remaining data can be totally stored in this block
        memcpy(block[b_index].b_data + pos, data, length);  // insert data to block
    }

    write_disk(4, b_index);
}

// While inserting data, calculate the new block number,
// and update new file size.
// Return: number of added data blocks. (except indirect)
__u16 cal_block_insert(__u16 i_index, int pos, int l) {
    read_disk(3, i_index);

    int num_block_before = inode[i_index].i_num_block;
    int size_before = inode[i_index].i_size_file;
    if (pos > size_before)
        pos = size_before;
    int size_after = pos + l;
    if (size_after < size_before)
        size_after = size_before;

    // update file size
    inode[i_index].i_size_file = size_after;
    write_disk(3, i_index);

    int num_block_after = size_after / BLOCK_SIZE;
    if (size_after % BLOCK_SIZE > 0)
        num_block_after++;

    return (__u16)(num_block_after - num_block_before);
}

// Add block and update data block number in inode.
void add_block(__u16 i_index, int num_block_add) {
    read_disk(3, i_index);

    int b_before = inode[i_index].i_num_block;
    int b_after = b_before + num_block_add;
    char b_index_ch[2];

    if (b_after > (8 + 128 + 128 * 128 + 128 * 128 * 128)) {
        printf("Error: exceed file maximum size.\n");
        return;
    }

    inode[i_index].i_num_block = (__u16) b_after;

    write_disk(3, i_index);

    for (int i = b_before + 1; i <= b_after; i++) {
        // the index of direct (data) block in block table
        __u16 b_index_data;
        if (1 <= i && i <= 8) {
            // find direct (data) block
            b_index_data = find_free_block();
            inode[i_index].i_block_direct[i - 1] = b_index_data;

            write_disk(3, i_index);
        } else {
            // the index of indirect block in block table
            __u16 b_index_single;
            __u16 b_index_double;
            __u16 b_index_triple;
            // the location of data block index in single indirect block (0, 2, ..., 254)
            int b_loc_data;
            if (i == (8 + 1)) {
                // find single indirect block
                num_block_add++;
                b_index_single = find_free_block();
                inode[i_index].i_block_single = b_index_single;

                write_disk(3, i_index);
                b_loc_data = 0;
            } else if (i > (8 + 1) && i <= (8 + 128)) {
                b_index_single = inode[i_index].i_block_single;
                b_loc_data = (i - 1 - 8) * 2;
            } else if (i == (8 + 128 + 1)) {
                num_block_add += 2;
                b_index_double = find_free_block();
                inode[i_index].i_block_double = b_index_double;

                write_disk(3, i_index);
                b_index_single = find_and_set(b_index_double, 0);
                b_loc_data = 0;
            } else if (i > (8 + 128 + 1) && i <= (8 + 128 + 128 * 128)) {
                // the j-th double indirect block
                // j: [0, 128 * 128 - 1]
                int j = i - 8 - 128 - 1;
                int double_index = j / 128;
                int single_index = j % 128;
                b_index_double = inode[i_index].i_block_double;
                if (single_index == 0) {
                    num_block_add++;
                    b_index_single = find_and_set(b_index_double, double_index * 2);
                } else {
                    b_index_single = find_and_convert(b_index_double, double_index * 2);
                }
                b_loc_data = single_index * 2;
            } else if (i == (8 + 128 + 128 * 128 + 1)) {
                num_block_add += 3;
                b_index_triple = find_free_block();
                inode[i_index].i_block_triple = b_index_triple;

                write_disk(3, i_index);
                b_index_double = find_and_set(b_index_triple, 0);
                b_index_single = find_and_set(b_index_double, 0);
                b_loc_data = 0;
            } else if (i > (8 + 128 + 128 * 128 + 1) && i <= (8 + 128 + 128 * 128 + 128 * 128 * 128)) {
                // the k-th triple indirect block
                // k: [0, 128 * 128 * 128 - 1]
                int k = i - 8 - 128 - 128 * 128 - 1;
                int triple_index = k / (128 * 128);
                int double_index = k / 128 % 128;
                int single_index = k % 128;
                b_index_triple = inode[i_index].i_block_triple;
                if (double_index == 0) {
                    num_block_add++;
                    b_index_double = find_and_set(b_index_triple, triple_index * 2);
                } else {
                    b_index_double = find_and_convert(b_index_triple, triple_index * 2);
                }
                if (single_index == 0) {
                    num_block_add++;
                    b_index_single = find_and_set(b_index_double, double_index * 2);
                } else {
                    b_index_single = find_and_convert(b_index_double, double_index * 2);
                }
                b_loc_data = single_index * 2;
            }
            // create direct (data) block
            find_and_set(b_index_single, b_loc_data);
        }
    }
}

// Read block.
// If fg == 1, delete block.
// If 'info' is not NULL, record the accessed data to 'info'.
// Range: pos_block ~ pos_block + num_block - 1 (virtually)
// Need to delete indirect block.
// Do not modify file size.
void read_del_block(__u16 i_index, int pos_block, int num_block, char *info, int fg) {
    read_disk(3, i_index);

    // the block index that will be accessed
    int b_index;
    int b_index_single;
    int b_index_double;
    int b_index_triple;

    // update inode
    update_time_total(0, i_index);
    if (fg) {
        update_time_total(1, i_index);
        update_time(2, i_index);
        inode[i_index].i_num_block -= num_block;

        write_disk(3, i_index);
    }

    // i is 0-indexed
    for (int i = pos_block; i < pos_block + num_block; i++) {
        if (i < 8) {
            b_index = inode[i_index].i_block_direct[i];
        } else if (i < 8 + 128) {
            int single_index = i - 8;  // 0 ~ 127
            b_index_single = inode[i_index].i_block_single;
            b_index = find_and_convert(b_index_single, single_index * 2);
            if (single_index == 0 && fg)
                modify_block_bitmap(b_index_single, 0);
        } else if (i < 8 + 128 + 128 * 128) {
            int double_index = (i - 8 - 128) / 128; // 0 ~ 127
            int single_index = (i - 8 - 128) % 128; // 0 ~ 127
            b_index_double = inode[i_index].i_block_double;
            b_index_single = find_and_convert(b_index_double, double_index * 2);
            b_index = find_and_convert(b_index_single, single_index * 2);
            if (double_index == 0 && fg)
                modify_block_bitmap(b_index_double, 0);
            if (single_index == 0 && fg)
                modify_block_bitmap(b_index_single, 0);
        } else if (i < 8 + 128 + 128 * 128 + 128 * 128 * 128) {
            int triple_index = (i - 8 - 128 - 128 * 128) / (128 * 128); // 0 ~ 127
            int double_index = (i - 8 - 128 - 128 * 128) / 128 % 128;   // 0 ~ 127
            int single_index = (i - 8 - 128 - 128 * 128) % 128;         // 0 ~ 127
            b_index_triple = inode[i_index].i_block_triple;
            b_index_double = find_and_convert(b_index_triple, triple_index * 2);
            b_index_single = find_and_convert(b_index_double, double_index * 2);
            b_index = find_and_convert(b_index_single, single_index * 2);
            if (triple_index == 0 && fg)
                modify_block_bitmap(b_index_triple, 0);
            if (double_index == 0 && fg)
                modify_block_bitmap(b_index_double, 0);
            if (single_index == 0 && fg)
                modify_block_bitmap(b_index_single, 0);
        }
        if (info != NULL) {
            read_disk(4, b_index);
            memcpy(info, block[b_index].b_data, 256);
            info += 256;
        }
        // delete data block
        if (fg)
            modify_block_bitmap(b_index, 0);
    }
}

// File has been added necessary blocks but those blocks are empty.
// This function is to add data to those empty blocks.
void insert_data(__u16 i_index, int pos, int l, char data[MAX_DATA_LEN]) {
    read_disk(3, i_index);

    int size = inode[i_index].i_size_file;
    if (pos >= size)
        pos = size;

    // find the last virtual block index of this file.
    __u16 last_block_index_v = (size - 1) / BLOCK_SIZE;

    // find the virtual block index of pos.
    __u16 cur_block_index_v = pos / BLOCK_SIZE;

    // insert data to block
    insert_data_to_block(i_index, cur_block_index_v, pos - cur_block_index_v * BLOCK_SIZE, &l, data);
    while (cur_block_index_v < last_block_index_v) {
        cur_block_index_v++;
        insert_data_to_block(i_index, cur_block_index_v, 0, &l, data);
    }
}

// Build inode, update time.
// Set the corresponding bits of the target inode.
// i_index: The index of the target inode.
void build_inode(__u16 i_index, __u32 i_info, const char i_name[16], __u32 i_size_file,
                 __u16 i_num_block, __u16 i_num_link, __u16 i_inode_parent_dir) {
    read_disk(3, i_index);

    inode[i_index].i_info = i_info;
    strcpy(inode[i_index].i_name, i_name);
    inode[i_index].i_size_file = i_size_file;
    inode[i_index].i_num_block = i_num_block;
    inode[i_index].i_num_link = i_num_link;
    inode[i_index].i_inode_parent_dir = i_inode_parent_dir;

    write_disk(3, i_index);

    update_time_total(0, inode[i_index].i_inode_parent_dir);    // not access this file
    update_time_total(1, i_index);
    update_time(2, i_index);
}

// Delete inode after update time.
// Not delete block so that must be called after 'read_del_block'.
void del_inode(__u16 i_index) {
    // do not need to read disk, because the function caller
    //      has already read disk.
    update_time_total(0, inode[i_index].i_inode_parent_dir);
    update_time_total(1, i_index);
    update_time(2, i_index);
    modify_inode_bitmap(i_index, 0);
}

// Modify inode and add data.
// From pos, add l bytes of data.
// Update file size and block number.
void modify_inode_add(__u16 i_index, int pos, int l, char data[MAX_DATA_LEN]) {
    update_time_total(0, i_index);   // update access time totally
    update_time_total(1, i_index);   // update modify time totally
    update_time(2, i_index);         // update change time

    // Calculate added block number. (Only contain data block)
    // And update file size.
    int num_block_add = cal_block_insert(i_index, pos, l);

    // Add block physically.
    // And update block number.
    if (num_block_add > 0)
        add_block(i_index, num_block_add);

    // Insert data.
    insert_data(i_index, pos, l, data);
}

// Modify current directory after delete a file or a subdirectory.
// i_index_v: the virtual index that will be deleted in current dir.
void modify_cur_dir(__u16 i_index_v) {
    read_disk(3, cur_dir);

    char index_info[8000];
    int block_num = inode[cur_dir].i_num_block;
    int file_size = inode[cur_dir].i_size_file;

    // delete all the index information in current directory
    inode[cur_dir].i_size_file = 0;
    write_disk(3, cur_dir);
    read_del_block(cur_dir, 0, block_num, index_info, 1);

    // modify index information
    memcpy(index_info + i_index_v * 2,
           index_info + (i_index_v + 1) * 2,
           block_num * 256 - 2);

    // insert index information
    modify_inode_add(cur_dir, 0, file_size - 2, index_info);
}

// Delete the given directory or file recursively.
void del_subdir(__u16 i_index) {
    read_disk(3, i_index);

    // If this inode is directory,
    // recursively delete subdir/file.
    if (inode[i_index].i_info % 2 == 1) {
        int size = inode[i_index].i_size_file;
        __u16 i_index_p;    // physical subdir/file index

        // traverse all the index in this inode
        for (int i_index_v = 0; i_index_v < size / 2; i_index_v++) {
            // find the physical index
            i_index_p = find_inode_index(i_index, i_index_v);

            // recursively delete the subdir/file
            del_subdir(i_index_p);
        }
    }

    // delete block
    read_del_block(i_index, 0, inode[i_index].i_num_block, NULL, 1);

    // delete inode
    del_inode(i_index);
}

// Compare a and b by lexicography order.
// If left <= right, return 0.
// If left > right, return 1.
int lex_cmp(char left[16], char right[16]) {
    char a[16], b[16];
    strcpy(a, left);
    strcpy(b, right);
    for (int i = 0; i < 16; i++) {
        if (a[i] == '\0')
            return 0;
        if (b[i] == '\0')
            return 1;
        if (a[i] >= 'A' && a[i] <= 'Z')
            a[i] = a[i] - 'A' + 'a';
        if (b[i] >= 'A' && b[i] <= 'Z')
            b[i] = b[i] - 'A' + 'a';
        if (a[i] < b[i])
            return 0;
        if (a[i] > b[i])
            return 1;

        // (int)'a' > (int)'A'
        // a < A (lex)
        if (left[i] > right[i])
            return 0;
        if (left[i] < right[i])
            return 1;
    }
}

// Sort array by lexicography name.
// Use insertion sort.
// From 0 ~ size - 1.
void sort_by_lex(struct index_name array[4000], int size) {
    struct index_name tmp;
    for (int i = 0; i < size - 1; i++) {
        for (int j = size - 1; j > i; j--) {
            if (lex_cmp(array[j - 1].name, array[j].name)) {
                tmp = array[j];
                array[j] = array[j - 1];
                array[j - 1] = tmp;
            }
        }
    }
}

void print_usr(int id) {
    printf("%d", id);
    sprintf(buffer, "%d", id);
    strcat(client_buffer_w, buffer);
}

// If OUTPUT_STDOUT == 1, output current directory in stdout.
void print_cur_dir() {
    char name[64][16];
    int i = 0;
    __u16 dir_index = cur_dir;

    while (dir_index != 0) {
        read_disk(3, dir_index);

        strcpy(name[i], inode[dir_index].i_name);
        dir_index = inode[dir_index].i_inode_parent_dir;
        i++;
    }

    print_usr(cur_usr);
    printf(":");
    sprintf(buffer, ":");
    strcat(client_buffer_w, buffer);

    read_disk(1, 0);

    // the system has been formatted
    if ((inode_bitmap.i_valid_bit[0] & 0b00000001) == 1) {
        if (i == 0) {
            printf("/");
            sprintf(buffer, "/");
            strcat(client_buffer_w, buffer);
        }
        for (int j = i - 1; j >= 0; j--) {
            printf("/%s", name[j]);
            sprintf(buffer, "/%s", name[j]);
            strcat(client_buffer_w, buffer);
        }
    }

    printf("$ ");
    sprintf(buffer, "$ ");
    strcat(client_buffer_w, buffer);
}

// Print authority.
void print_authority(__u32 info) {
    if ((info & 0b100) == 4) {
        printf("r");
        sprintf(buffer, "r");
        strcat(client_buffer_w, buffer);
    } else {
        printf("-");
        sprintf(buffer, "-");
        strcat(client_buffer_w, buffer);
    }
    if ((info & 0b010) == 2) {
        printf("w");
        sprintf(buffer, "w");
        strcat(client_buffer_w, buffer);
    } else {
        printf("-");
        sprintf(buffer, "-");
        strcat(client_buffer_w, buffer);
    }
    if ((info & 0b001) == 1) {
        printf("x");
        sprintf(buffer, "x");
        strcat(client_buffer_w, buffer);
    } else {
        printf("-");
        sprintf(buffer, "-");
        strcat(client_buffer_w, buffer);
    }
}

// Print time.
void print_time(__u32 time) {
    int year = time % (1 << 6) + 2000;
    int mon = (time >> 6) % (1 << 4);
    int mday = (time >> 10) % (1 << 5);
    int hour = (time >> 15) % (1 << 5);
    int min = (time >> 20) % (1 << 6);
    int sec = (time >> 26);

    if (year == 2000) {
        printf("---------- --------");
        sprintf(buffer, "---------- --------");
        strcat(client_buffer_w, buffer);
    } else {
        printf("%d-%02d-%02d %02d:%02d:%02d", year, mon, mday, hour, min, sec);
        sprintf(buffer, "%d-%02d-%02d %02d:%02d:%02d", year, mon, mday, hour, min, sec);
        strcat(client_buffer_w, buffer);
    }
}

// Print inode details.
void print_inode(__u16 i_index) {
    read_disk(3, i_index);

    __u32 i_info = inode[i_index].i_info;
    __u16 i_num_link = inode[i_index].i_num_link;
    __u32 i_size_file = inode[i_index].i_size_file;
    __u32 i_time_access = inode[i_index].i_time_access;
    __u32 i_time_modify = inode[i_index].i_time_modify;
    __u32 i_time_change = inode[i_index].i_time_change;
    __u8 i_name[16];
    memcpy(i_name, inode[i_index].i_name, 16);

    // file or directory
    if (i_info % 2 == 0) {  // file
        printf("-");
        sprintf(buffer, "-");
        strcat(client_buffer_w, buffer);
    } else {    // directory
        printf("d");
        sprintf(buffer, "d");
        strcat(client_buffer_w, buffer);
    }

    // authority
    print_authority(i_info >> 9);   // owner
    print_authority(i_info >> 12);  // group
    print_authority(i_info >> 15);  // public

    printf(" ");
    sprintf(buffer, " ");
    strcat(client_buffer_w, buffer);

    // link number
    printf("%d ", i_num_link);
    sprintf(buffer, "%d ", i_num_link);
    strcat(client_buffer_w, buffer);

    // owner id
    print_usr((i_info >> 1) % 16);
    printf(" ");
    sprintf(buffer, " ");
    strcat(client_buffer_w, buffer);

    // group id
    print_usr((i_info >> 5) % 16);
    printf(" ");
    sprintf(buffer, " ");
    strcat(client_buffer_w, buffer);

    // file size
    printf("%d ", i_size_file);
    sprintf(buffer, "%d ", i_size_file);
    strcat(client_buffer_w, buffer);

    // access time
    printf("Access: ");
    sprintf(buffer, "Access: ");
    strcat(client_buffer_w, buffer);
    print_time(i_time_access);

    // modify time
    printf(" Modify: ");
    sprintf(buffer, " Modify: ");
    strcat(client_buffer_w, buffer);
    print_time(i_time_modify);

    // change time
    printf(" Change: ");
    sprintf(buffer, " Change: ");
    strcat(client_buffer_w, buffer);
    print_time(i_time_change);

    // name
    printf(" %s\n", i_name);
    sprintf(buffer, " %s\n", i_name);
    strcat(client_buffer_w, buffer);
}

// Initialize client with disk.c.
void init_client(char *argv[]) {
    int disk_portno;
    struct sockaddr_in disk_serv_addr;
    struct hostent *disk_server;
    char host_name[20] = "localhost";   // localhost

    // port number
    disk_portno = atoi(argv[1]);
    disk_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (disk_sockfd < 0) {
        printf("Error: opening socket.\n");
        exit(-1);
    }

    // localhost
    disk_server = gethostbyname(host_name);
    if (disk_server == NULL) {
        printf("Error: no such host.\n");
        exit(-1);
    }
    bzero((char *) &disk_serv_addr, sizeof(disk_serv_addr));
    disk_serv_addr.sin_family = AF_INET;
    bcopy((char *) disk_server->h_addr,
          (char *) &disk_serv_addr.sin_addr.s_addr,
          disk_server->h_length);
    disk_serv_addr.sin_port = htons(disk_portno);

    // connect
    if (connect(disk_sockfd, (struct sockaddr *) &disk_serv_addr, sizeof(disk_serv_addr)) < 0) {
        printf("Error: connecting.\n");
        exit(-1);
    }
}


// Initialize server with client.c.
void init_server(char *argv[]) {
    // server
    int portno;

    // create socket
    client_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_sockfd < 0) {
        printf("Error: opening socket.\n");
        exit(-1);
    }
    bzero((char *) &serv_addr, sizeof(serv_addr));
    portno = atoi(argv[2]);
    serv_addr.sin_family = AF_INET;            // IPv4 address
    serv_addr.sin_addr.s_addr = INADDR_ANY;    // bind the socket
    serv_addr.sin_port = htons(portno);
    // htons() converts the port number from host byte order to network byte order

    // bind
    if (bind(client_sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        printf("Error: on binding.\n");
        exit(-1);
    }

    // listen
    if (listen(client_sockfd, 5) == -1) {
        printf("Error: on listening.\n");
        close(client_sockfd);
        exit(-1);
    }
    clilen = sizeof(cli_addr);
    printf("Accepting connections ...\n");

    // wait for accept
    client_newsockfd = accept(client_sockfd, (struct sockaddr *) &cli_addr, &clilen);
    if (client_newsockfd < 0) {
        printf("Error: on accept.\n");
        exit(-1);
    }
}

// Read a string from client_buffer.
// Terminate at ' ' or '\0'.
void read_str_to_space(char str[]) {
    int i = 0, size = strlen(client_buffer);

    // find the first ' ' and copy command
    while (client_buffer[i] != ' ' && client_buffer[i] != '\0')
        i++;
    memcpy(str, client_buffer, i);
    str[i] = '\0';

    if (client_buffer[i] == '\0')
        return;

    // truncate client_buffer
    memcpy(client_buffer,
           client_buffer + i + 1,
           size - i - 1);
    client_buffer[size - i - 1] = '\0';
}

// Read a number from client_buffer.
// Terminate at ' ' or '\0'.
void read_num_to_space(int *num) {
    char length_str[32];

    // first convert to separate string
    read_str_to_space(length_str);

    // second convert to number
    *num = atoi(length_str);
}

void read_command(char command[16]) {
    read_str_to_space(command);
}

void read_name(char name[16]) {
    read_str_to_space(name);
}

void read_path(char path[MAX_PATH_LEN]) {
    read_str_to_space(path);
}

void read_data(char data[MAX_DATA_LEN]) {
    strcpy(data, client_buffer);
}

void read_length(int *length) {
    read_num_to_space(length);
}

void read_pos(int *pos) {
    read_num_to_space(pos);
}

// Format file system. Construct a directory named '/'.
void f_sys_f() {
    get_disk_org();

    init_super_block();
    init_inode_bitmap();
    init_block_bitmap();
    init_block();

    cur_dir = 0;

    __u16 i_index = find_free_inode();

    build_inode(i_index, INFO_DIR_ALL_ALLOW, "/", 0, 0, 0, 0);  // only update modify/change time

    fprintf(fs_log, "Done\n");
    if (OUTPUT_STDOUT) {
        printf("=================== output ====================\n");
        printf("Done\n");
        sprintf(buffer, "Done\n");
        strcat(client_buffer_w, buffer);
    }
}

// Create file.
void f_sys_mk() {
    char f[16]; // file name
    read_name(f);

    // check for repetition
    if (check_repeat(cur_dir, f) >= 0) {
        fprintf(fs_log, "No\n");
        if (OUTPUT_STDOUT) {
            printf("=================== output ====================\n");
            printf("No\n");
            sprintf(buffer, "No\n");
            strcat(client_buffer_w, buffer);
        }
        return;
    }

    __u16 i_index = find_free_inode();  // find free inode index
    char i_index_ch[2];                 // inode index (string form)
    itoa_2(i_index, i_index_ch);        // convert index from __u16 to string

    read_disk(3, cur_dir);

    // modify current inode, add block, insert data
    // update current directory time
    modify_inode_add(cur_dir, inode[cur_dir].i_size_file, 2, i_index_ch);

    // build new inode for file
    // update file time
    build_inode(i_index, INFO_FILE_ALL_ALLOW, f, 0, 0, 1, cur_dir);

    fprintf(fs_log, "Yes\n");
    if (OUTPUT_STDOUT) {
        printf("=================== output ====================\n");
        printf("Yes\n");
        sprintf(buffer, "Yes\n");
        strcat(client_buffer_w, buffer);
    }
}

// Create directory.
void f_sys_mkdir() {
    char d[16]; // directory name
    read_name(d);

    // check for repetition
    if (check_repeat(cur_dir, d) >= 0) {
        fprintf(fs_log, "No\n");
        if (OUTPUT_STDOUT) {
            printf("=================== output ====================\n");
            printf("No\n");
            sprintf(buffer, "No\n");
            strcat(client_buffer_w, buffer);
        }
        return;
    }

    __u16 i_index = find_free_inode();  // find free inode index
    char i_index_ch[2];                 // inode index (string form)
    itoa_2(i_index, i_index_ch);        // convert index from __u16 to string

    read_disk(3, cur_dir);

    // modify current inode, add block, insert data
    // update current directory time
    modify_inode_add(cur_dir, inode[cur_dir].i_size_file, 2, i_index_ch);

    // build new inode for directory
    // update file time
    build_inode(i_index, INFO_DIR_ALL_ALLOW, d, 0, 0, 1, cur_dir);

    fprintf(fs_log, "Yes\n");
    if (OUTPUT_STDOUT) {
        printf("=================== output ====================\n");
        printf("Yes\n");
        sprintf(buffer, "Yes\n");
        strcat(client_buffer_w, buffer);
    }
}

void f_sys_rm() {
    char f[16]; // file name
    read_name(f);

    // check for existence and get inode index
    int i_index_v = check_repeat(cur_dir, f);           // virtual index
    if (i_index_v < 0) {
        fprintf(fs_log, "No\n");
        if (OUTPUT_STDOUT) {
            printf("=================== output ====================\n");
            printf("No\n");
            printf("Error: '%s' is not found.\n", f);
            sprintf(buffer, "No\n");
            strcat(client_buffer_w, buffer);
            sprintf(buffer, "Error: '%s' is not found.\n", f);
            strcat(client_buffer_w, buffer);
        }
        return;
    }
    int i_index = find_inode_index(cur_dir, i_index_v); // physical index

    read_disk(3, i_index);

    if (inode[i_index].i_info % 2 == 1) {
        fprintf(fs_log, "No\n");
        if (OUTPUT_STDOUT) {
            printf("=================== output ====================\n");
            printf("No\n");
            printf("Error: '%s' is a directory.\n", f);
            sprintf(buffer, "No\n");
            strcat(client_buffer_w, buffer);
            sprintf(buffer, "Error: '%s' is a directory.\n", f);
            strcat(client_buffer_w, buffer);
        }
        return;
    }

    // delete file.
    del_subdir(i_index);

    // modify current directory
    modify_cur_dir(i_index_v);

    fprintf(fs_log, "Yes\n");
    if (OUTPUT_STDOUT) {
        printf("=================== output ====================\n");
        printf("Yes\n");
        sprintf(buffer, "Yes\n");
        strcat(client_buffer_w, buffer);
    }
}

void f_sys_cd() {
    char path[MAX_PATH_LEN];
    read_path(path);

    read_disk(3, 0);

    // handle the first '/'
    if (path[0] == '/') {
        goto_dir(0);
        strcpy(path, path + 1);
    }

    int i = 0, state = 1;
    char name[16];
    while (strlen(path) != 0) {
        // get name
        while (path[i] != '/' && path[i] != '\0') {
            name[i] = path[i];
            i++;
        }
        name[i] = '\0';

        // truncate path
        if (path[i] == '/')
            i++;
        strcpy(path, path + i);
        i = 0;

        // change directory
        if (change_dir(name) == 0) {
            state = 0;
            break;
        }
    }

    if (state) {
        fprintf(fs_log, "Yes\n");
        if (OUTPUT_STDOUT) {
            printf("=================== output ====================\n");
            printf("Yes\n");
            sprintf(buffer, "Yes\n");
            strcat(client_buffer_w, buffer);
        }
    } else {
        fprintf(fs_log, "No\n");
        if (OUTPUT_STDOUT) {
            printf("=================== output ====================\n");
            printf("No\n");
            printf("Error: directory '%s' is not found.\n", name);
            sprintf(buffer, "No\n");
            strcat(client_buffer_w, buffer);
            sprintf(buffer, "Error: directory '%s' is not found.\n", name);
            strcat(client_buffer_w, buffer);
        }
    }
}

void f_sys_rmdir() {
    char d[16]; // directory name
    read_name(d);

    // check for existence and get inode index
    int i_index_v = check_repeat(cur_dir, d);           // virtual index
    if (i_index_v < 0) {
        fprintf(fs_log, "No\n");
        if (OUTPUT_STDOUT) {
            printf("=================== output ====================\n");
            printf("No\n");
            printf("Error: '%s' is not found.\n", d);
            sprintf(buffer, "No\n");
            strcat(client_buffer_w, buffer);
            sprintf(buffer, "Error: '%s' is not found.\n", d);
            strcat(client_buffer_w, buffer);
        }
        return;
    }
    int i_index = find_inode_index(cur_dir, i_index_v); // physical index

    read_disk(3, i_index);

    if (inode[i_index].i_info % 2 == 0) {
        fprintf(fs_log, "No\n");
        if (OUTPUT_STDOUT) {
            printf("=================== output ====================\n");
            printf("No\n");
            printf("Error: '%s' is a file.\n", d);
            sprintf(buffer, "No\n");
            strcat(client_buffer_w, buffer);
            sprintf(buffer, "Error: '%s' is a file.\n", d);
            strcat(client_buffer_w, buffer);
        }
        return;
    }

    // delete all the subdirectory and file.
    del_subdir(i_index);

    // modify current directory
    modify_cur_dir(i_index_v);

    fprintf(fs_log, "Yes\n");
    if (OUTPUT_STDOUT) {
        printf("=================== output ====================\n");
        printf("Yes\n");
        sprintf(buffer, "Yes\n");
        strcat(client_buffer_w, buffer);
    }
}

void f_sys_ls() {
    read_disk(3, cur_dir);

    struct index_name file[4000];
    struct index_name dir[4000];
    int size = inode[cur_dir].i_size_file;
    __u16 i_index_p;  //physical index
    int i = 0, j = 0;

    // traverse all the index in this node
    for (int i_index_v = 0; i_index_v < size / 2; i_index_v++) {
        // find the physical index
        i_index_p = find_inode_index(cur_dir, i_index_v);

        read_disk(3, i_index_p);

        if (inode[i_index_p].i_info % 2 == 0) {
            // file
            strcpy(file[i].name, inode[i_index_p].i_name);
            file[i].index = i_index_p;
            i++;
        } else {
            // directory
            strcpy(dir[j].name, inode[i_index_p].i_name);
            dir[j].index = i_index_p;
            j++;
        }
    }

    // sort by lexicography order
    sort_by_lex(file, i);
    sort_by_lex(dir, j);

    if (OUTPUT_STDOUT) {
        printf("=================== output ====================\n");
    }

    // output all the file/dir names
    for (int ii = 0; ii < i; ii++) {
        fprintf(fs_log, "%s ", file[ii].name);
        if (OUTPUT_STDOUT) {
            if (OUTPUT_DETAIL) {
                print_inode(file[ii].index);
            } else {
                printf("%s ", file[ii].name);
                sprintf(buffer, "%s ", file[ii].name);
                strcat(client_buffer_w, buffer);
            }
        }
    }
    fprintf(fs_log, "& ");
    if (OUTPUT_STDOUT && !OUTPUT_DETAIL) {
        printf("& ");
        sprintf(buffer, "& ");
        strcat(client_buffer_w, buffer);
    }
    for (int jj = 0; jj < j; jj++) {
        fprintf(fs_log, "%s ", dir[jj].name);
        if (OUTPUT_STDOUT) {
            if (OUTPUT_DETAIL) {
                print_inode(dir[jj].index);
            } else {
                printf("%s ", dir[jj].name);
                sprintf(buffer, "%s ", dir[jj].name);
                strcat(client_buffer_w, buffer);
            }
        }
    }
    fprintf(fs_log, "\n");

    // while i = j = 0, it must send a '\n'
    if ((OUTPUT_STDOUT && !OUTPUT_DETAIL) || (i + j == 0)) {
        printf("\n");
        sprintf(buffer, "\n");
        strcat(client_buffer_w, buffer);
    }
}

void f_sys_cat() {
    char f[16]; // file name
    read_name(f);

    // check for existence and get inode index
    int i_index_v = check_repeat(cur_dir, f);           // virtual index
    if (i_index_v < 0) {
        fprintf(fs_log, "No\n");
        if (OUTPUT_STDOUT) {
            printf("=================== output ====================\n");
            printf("No\n");
            printf("Error: '%s' is not found.\n", f);
            sprintf(buffer, "No\n");
            strcat(client_buffer_w, buffer);
            sprintf(buffer, "Error: '%s' is not found.\n", f);
            strcat(client_buffer_w, buffer);
        }
        return;
    }
    int i_index = find_inode_index(cur_dir, i_index_v); // physical index

    read_disk(3, i_index);

    if (inode[i_index].i_info % 2 == 1) {
        fprintf(fs_log, "No\n");
        if (OUTPUT_STDOUT) {
            printf("=================== output ====================\n");
            printf("No\n");
            printf("Error: '%s' is a directory.\n", f);
            sprintf(buffer, "No\n");
            strcat(client_buffer_w, buffer);
            sprintf(buffer, "Error: '%s' is a directory.\n", f);
            strcat(client_buffer_w, buffer);
        }
        return;
    }

    // read block
    char data[BLOCK_SIZE * 1024];
    data[inode[i_index].i_num_block * BLOCK_SIZE] = '\0';
    read_del_block(i_index, 0, inode[i_index].i_num_block, data, 0);

    // output data
    fprintf(fs_log, "%s\n", data);
    if (OUTPUT_STDOUT || (strlen(data) == 0)) {
        printf("=================== output ====================\n");
        printf("%s\n", data);
        sprintf(buffer, "%s\n", data);
        strcat(client_buffer_w, buffer);
    }
}

void f_sys_w() {
    char f[16]; // file name
    read_name(f);

    // check for existence and get inode index
    int i_index_v = check_repeat(cur_dir, f);           // virtual index
    if (i_index_v < 0) {
        fprintf(fs_log, "No\n");
        if (OUTPUT_STDOUT) {
            printf("=================== output ====================\n");
            printf("No\n");
            printf("Error: '%s' is not found.\n", f);
            sprintf(buffer, "No\n");
            strcat(client_buffer_w, buffer);
            sprintf(buffer, "Error: '%s' is not found.\n", f);
            strcat(client_buffer_w, buffer);
        }
        return;
    }

    int i_index = find_inode_index(cur_dir, i_index_v); // physical index

    read_disk(3, i_index);

    if (inode[i_index].i_info % 2 == 1) {
        fprintf(fs_log, "No\n");
        if (OUTPUT_STDOUT) {
            printf("=================== output ====================\n");
            printf("No\n");
            printf("Error: '%s' is a directory.\n", f);
            sprintf(buffer, "No\n");
            strcat(client_buffer_w, buffer);
            sprintf(buffer, "Error: '%s' is a directory.\n", f);
            strcat(client_buffer_w, buffer);
        }
        return;
    }

    int l;
    char data[MAX_DATA_LEN];
    read_length(&l);
    read_data(data);

    // delete block and modify block number
    read_del_block(i_index, 0, inode[i_index].i_num_block, NULL, 1);

    // modify file size
    inode[i_index].i_size_file = 0;

    write_disk(3, i_index);

    // insert at 0
    modify_inode_add(i_index, 0, l, data);

    fprintf(fs_log, "Yes\n");
    if (OUTPUT_STDOUT) {
        printf("=================== output ====================\n");
        printf("Yes\n");
        sprintf(buffer, "Yes\n");
        strcat(client_buffer_w, buffer);
    }
}

void f_sys_i() {
    char f[16]; // file name
    read_name(f);

    // check for existence and get inode index
    int i_index_v = check_repeat(cur_dir, f);           // virtual index
    if (i_index_v < 0) {
        fprintf(fs_log, "No\n");
        if (OUTPUT_STDOUT) {
            printf("=================== output ====================\n");
            printf("No\n");
            printf("Error: '%s' is not found.\n", f);
            sprintf(buffer, "No\n");
            strcat(client_buffer_w, buffer);
            sprintf(buffer, "Error: '%s' is not found.\n", f);
            strcat(client_buffer_w, buffer);
        }
        return;
    }
    int i_index = find_inode_index(cur_dir, i_index_v); // physical index

    read_disk(3, i_index);

    if (inode[i_index].i_info % 2 == 1) {
        fprintf(fs_log, "No\n");
        if (OUTPUT_STDOUT) {
            printf("=================== output ====================\n");
            printf("No\n");
            printf("Error: '%s' is a directory.\n", f);
            sprintf(buffer, "No\n");
            strcat(client_buffer_w, buffer);
            sprintf(buffer, "Error: '%s' is a directory.\n", f);
            strcat(client_buffer_w, buffer);
        }
        return;
    }

    int pos, l, size;
    char data[MAX_DATA_LEN], init_data[BLOCK_SIZE * 1024];
    read_pos(&pos);
    read_length(&l);
    read_data(data);

    size = inode[i_index].i_size_file;
    if (pos > size)
        pos = size;

    // delete block and modify block number
    // record the initial data
    int remain_block = pos / BLOCK_SIZE;
    int remain_size = remain_block * BLOCK_SIZE;
    read_del_block(i_index, remain_block, inode[i_index].i_num_block - remain_block, init_data, 1);

    // modify file size
    inode[i_index].i_size_file = remain_size;

    write_disk(3, i_index);

    // modify init_data by data
    memcpy(init_data + pos + l - remain_size,
           init_data + pos - remain_size,
           size - pos);
    memcpy(init_data + pos - remain_size,
           data,
           l);
    init_data[size + l - remain_size] = '\0';

    // insert at remain size
    modify_inode_add(i_index, remain_size, size + l - remain_size, init_data);

    fprintf(fs_log, "Yes\n");
    if (OUTPUT_STDOUT) {
        printf("=================== output ====================\n");
        printf("Yes\n");
        sprintf(buffer, "Yes\n");
        strcat(client_buffer_w, buffer);
    }
}

void f_sys_d() {
    char f[16]; // file name
    read_name(f);

    // check for existence and get inode index
    int i_index_v = check_repeat(cur_dir, f);           // virtual index
    if (i_index_v < 0) {
        fprintf(fs_log, "No\n");
        if (OUTPUT_STDOUT) {
            printf("=================== output ====================\n");
            printf("No\n");
            printf("Error: '%s' is not found.\n", f);
            sprintf(buffer, "No\n");
            strcat(client_buffer_w, buffer);
            sprintf(buffer, "Error: '%s' is not found.\n", f);
            strcat(client_buffer_w, buffer);
        }
        return;
    }
    int i_index = find_inode_index(cur_dir, i_index_v); // physical index

    read_disk(3, i_index);

    if (inode[i_index].i_info % 2 == 1) {
        fprintf(fs_log, "No\n");
        if (OUTPUT_STDOUT) {
            printf("=================== output ====================\n");
            printf("No\n");
            printf("Error: '%s' is a directory.\n", f);
            sprintf(buffer, "No\n");
            strcat(client_buffer_w, buffer);
            sprintf(buffer, "Error: '%s' is a directory.\n", f);
            strcat(client_buffer_w, buffer);
        }
        return;
    }

    int pos, l, size;
    char init_data[BLOCK_SIZE * 1024];
    read_pos(&pos);
    read_length(&l);

    size = inode[i_index].i_size_file;
    if (pos + l > size)
        l = size - pos;
    if (l > 0) {

        // delete block and modify block number
        // record the initial data
        int remain_block = pos / BLOCK_SIZE;
        int remain_size = remain_block * BLOCK_SIZE;
        read_del_block(i_index, remain_block, inode[i_index].i_num_block - remain_block, init_data, 1);

        // modify file size
        inode[i_index].i_size_file = remain_size;

        write_disk(3, i_index);

        // modify init_data
        memcpy(init_data + pos - remain_size, init_data + pos + l - remain_size, size - pos - l);
        init_data[size - l - remain_size] = '\0';

        // insert at remain size
        modify_inode_add(i_index, remain_size, size - l - remain_size, init_data);
    }

    fprintf(fs_log, "Yes\n");
    if (OUTPUT_STDOUT) {
        printf("=================== output ====================\n");
        printf("Yes\n");
        sprintf(buffer, "Yes\n");
        strcat(client_buffer_w, buffer);
    }
}

// =========================================================
// Test function.
void test() {
    // This function will output:
    //      inode[0] ~ inode[num * 8 - 1]
    //      block[0] ~ block[num * 8 - 1]
    int num = 2;

    read_disk(0, 0);
    read_disk(1, 0);
    read_disk(2, 0);
    printf("=================== Test ======================\n");
    printf("inode num: %d\n", super_block.s_count_inode);
    printf("block num: %d\n", super_block.s_count_block);
    printf("current directory: %d\n", cur_dir);
    for (int i = 0; i < num; i++) {
        for (int j = i * 8; j < i * 8 + 8; j++) {
            printf("inode %d: bit = %d", j, inode_bitmap.i_valid_bit[i]);
            read_disk(3, j);
            printf(", size: %d", inode[j].i_size_file);
            printf(", name: %s", inode[j].i_name);
            printf("\n");
        }
    }
    for (int i = 0; i < num; i++) {
        for (int j = i * 8; j < i * 8 + 8; j++) {
            printf("block %d: bit = %d", j, block_bitmap.b_valid_bit[i]);
            read_disk(4, j);
            printf(", data: %s", block[j].b_data);
            printf("\n");
        }
    }
}
// =========================================================

void memory_polling() {
    // open disk.log
    fs_log = fopen("fs.log", "w");
    if (fs_log == NULL) {
        printf("Error: Could not open file 'fs.log'.\n");
        exit(-1);
    }

    char command[16];
    int state = 1;

    if (SOCKET_OPEN) {
        // read information from disk.
        get_disk_org();
        read_disk(0, 0);
        read_disk(1, 0);
        read_disk(2, 0);
        for (int i = 0; i < 16; i++)
            read_disk(3, i);
        if ((inode_bitmap.i_valid_bit[0] & 0b00000001) == 1)
            cur_dir = 0;
    }

    // state = 1: resume
    // state = 0: exit
    while (1) {
        if (SOCKET_OPEN) {
            // test
            if (OUTPUT_TEST)
                test();
            bzero(client_buffer, MAX_LEN);
            bzero(client_buffer_w, MAX_LEN);
        }

        printf("=================== Command ===================\n");

        if (OUTPUT_STDOUT) {    // print current directory
            print_cur_dir();
            if (SOCKET_OPEN) {
                server_write();
                bzero(client_buffer_w, MAX_LEN);
            }
        }

        if (SOCKET_OPEN) {  // read the output of fs
            server_read(client_buffer);

            printf("command: %s\n", client_buffer);

            if (TELNET_TEST) {  // use telnet directly to test
                client_buffer[strlen(client_buffer) - 2] = '\0';
            } else {
                client_buffer[strlen(client_buffer) - 1] = '\0';
            }
        } else {
            fgets(client_buffer, MAX_LEN, stdin);

            // delete '\n'
            client_buffer[strlen(client_buffer) - 1] = '\0';
        }

        // read command from client_buffer
        read_command(command);

        // execute command
        if (0 == strcmp("f", command)) {
            f_sys_f();
        } else if (0 == strcmp("mk", command)) {
            f_sys_mk();
        } else if (0 == strcmp("mkdir", command)) {
            f_sys_mkdir();
        } else if (0 == strcmp("rm", command)) {
            f_sys_rm();
        } else if (0 == strcmp("cd", command)) {
            f_sys_cd();
        } else if (0 == strcmp("rmdir", command)) {
            f_sys_rmdir();
        } else if (0 == strcmp("ls", command)) {
            f_sys_ls();
        } else if (0 == strcmp("cat", command)) {
            f_sys_cat();
        } else if (0 == strcmp("w", command)) {
            f_sys_w();
        } else if (0 == strcmp("i", command)) {
            f_sys_i();
        } else if (0 == strcmp("d", command)) {
            f_sys_d();
        } else if (0 == strcmp("e", command)) {
            fprintf(fs_log, "Goodbye!\n");
            if (OUTPUT_STDOUT) {
                printf("=================== output ====================\n");
                printf("Goodbye!\n");
                if (SOCKET_OPEN) {
                    disk_buffer_w[0] = 'E';
                    disk_buffer_w[1] = '\0';
                    client_write(1);
                }
            }
            state = 0;
        } else {
            if (OUTPUT_STDOUT) {
                printf("=================== output ====================\n");
                printf("Error: command '%s' error.\n", command);
                sprintf(buffer, "Error: command '%s' error.\n", command);
                strcat(client_buffer_w, buffer);
            }
        }

        if (SOCKET_OPEN) {
            server_write();

            // act as a separation between two write
            server_read(client_buffer);
        }

        if (state == 0)
            break;
    }

    fclose(fs_log);
}

int main(int argc, char *argv[]) {

    if (SOCKET_OPEN) {
        init_client(argv);
        init_server(argv);
    }

    memory_polling();

    if (SOCKET_OPEN) {
        close(client_sockfd);
        close(client_newsockfd);
        close(disk_sockfd);
    }

    return 0;
}
