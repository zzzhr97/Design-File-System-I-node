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

#define BLOCK_SIZE 256
#define MAX_LEN 1024     // the maximum length of socket message

// =================================================================
// You can modify this part to get different output type.

// If TELNET_TEST is 1, use telnet directly to test.
#define TELNET_TEST 0

// If SOCKET_OPEN is 1, use socket.
#define SOCKET_OPEN 1
// =================================================================

static int FILE_SIZE;
static int CYLINDERS;
static int SECTORS_PC;
static int MOVE_DELAY;
static int cur_cylinder;    // current access cylinder
static char *file_name;     // storage file name
static int fd;              // file id of storage file name
static FILE *disk_log;      // file id of disk.log
static char *disk_file;     // pointer of memory map

static int sockfd;          // socket with fs.c
static int newsockfd;       // new socket with fs.c
static socklen_t clilen;
struct sockaddr_in serv_addr;
struct sockaddr_in cli_addr;
static char buffer[MAX_LEN];// I/O buffer

// Write the input to client.
void server_write(char buffer[MAX_LEN], int length) {
    int n = write(newsockfd, buffer, length);
    if (n < 0) {
        printf("Error: writing to socket.\n");
        exit(-1);
    }
}

// Read the output from client.
void server_read(char buffer[MAX_LEN]) {
    bzero(buffer, MAX_LEN);
    int n = read(newsockfd, buffer, MAX_LEN);
    if (n < 0) {
        printf("Error: reading from socket.\n");
        exit(-1);
    }
    printf("read: %s\n", buffer);
}

// print the
void print_time(int time) {
    printf("track-to-track time: %d\n", time);
}

void input_detect(int argc) {
    int number;
    if (SOCKET_OPEN) {
        number = 6;
    } else {
        number = 5;
    }
    if (argc < number) {
        printf("Error: Missing parameter.\n");
        exit(-1);
    }
    if (argc > number) {
        printf("Error: Extra parameter.\n");
        exit(-1);
    }
}

void check_file() {
    int result = write(fd, "", 1);
    if (result != 1) {
        perror("Error writing last byte of the file");
        close(fd);
        exit(-1);
    }
}

void storage_init(char *argv[]) {
    // get parameters
    CYLINDERS = atoi(argv[1]);
    SECTORS_PC = atoi(argv[2]);
    MOVE_DELAY = atoi(argv[3]);
    file_name = argv[4];
    FILE_SIZE = CYLINDERS * SECTORS_PC * BLOCK_SIZE;

    // open storage file
    fd = open(file_name, O_RDWR | O_CREAT, S_IRWXU);
    if (fd < 0) {
        printf("Error: Could not open file '%s'.\n", file_name);
        exit(-1);
    }

    // check the size of file
    int result = lseek(fd, FILE_SIZE - 1, SEEK_SET);
    if (result == -1) {
        perror("Error calling lseek() to 'stretch' the file");
        close(fd);
        exit(-1);
    }

    // memory map
    disk_file = (char *) mmap(NULL, FILE_SIZE,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, 0);
    if (disk_file == MAP_FAILED) {
        close(fd);
        printf("Error: Could not map file.\n");
        exit(-1);
    }

    // open disk.log
    disk_log = fopen("disk.log", "w");
    if (disk_log == NULL) {
        close(fd);
        printf("Error: Could not open file 'disk.log'.\n");
        exit(-1);
    }
}

// Initialize server.
void init_server(char *argv[]) {
    // server
    int portno;

    // create socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        printf("Error: opening socket.\n");
        exit(-1);
    }
    bzero((char *) &serv_addr, sizeof(serv_addr));
    portno = atoi(argv[5]);
    serv_addr.sin_family = AF_INET;            // IPv4 address
    serv_addr.sin_addr.s_addr = INADDR_ANY;    // bind the socket
    serv_addr.sin_port = htons(portno);
    // htons() converts the port number from host byte order to network byte order

    // bind
    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        printf("Error: on binding.\n");
        exit(-1);
    }

    // listen
    if (listen(sockfd, 5) == -1) {
        printf("Error: on listening.\n");
        close(sockfd);
        exit(-1);
    }
    clilen = sizeof(cli_addr);
    printf("Accepting connections ...\n");

    // wait for accept
    newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
    if (newsockfd < 0) {
        printf("Error: on accept.\n");
        exit(-1);
    }
}

// read c and s from buffer.
void read_c_s(int *c, int *s) {
    int i = 0, j = 0;
    char c_str[64], s_str[64];
    while (buffer[i + 2] != ' ') {
        c_str[i] = buffer[i + 2];       // before the first ' ' is c
        i++;
    }
    c_str[i] = '\0';
    while (buffer[j + i + 3] != ' ' && buffer[j + i + 3] != '\0') {
        s_str[j] = buffer[j + i + 3];   // before the second ' ' or '\0' is s
        j++;
    };
    s_str[j] = '\0';
    *c = atoi(c_str);
    *s = atoi(s_str);

    // truncate buffer
    memcpy(buffer, buffer + i + j + 3, BLOCK_SIZE + 1);

    //printf("c: %d, s: %d, len: %d\n", *c, *s, (int) strlen(buffer));
}

// read c, s and buf from buffer.
void read_c_s_buf(int *c, int *s, char buf[BLOCK_SIZE + 1]) {
    read_c_s(c, s);
    memcpy(buffer, buffer + 1, BLOCK_SIZE);
    memcpy(buf, buffer, BLOCK_SIZE);
}

// Show cylinders and sectors per cylinder.
int show_org() {
    if (strlen(buffer) == 1) {
        printf("=================== output ====================\n");
        fprintf(disk_log, "%d %d\n", CYLINDERS, SECTORS_PC);
        printf("%d %d\n", CYLINDERS, SECTORS_PC);
    } else {
        return -1;
    }

    if (SOCKET_OPEN) {
        char cylinders[8], sectors_pc[8], buf[32];
        sprintf(cylinders, "%d ", CYLINDERS);
        sprintf(sectors_pc, "%d", SECTORS_PC);
        strcat(buf, cylinders);
        strcat(buf, sectors_pc);
        server_write(buf, strlen(buf));
    }

    return 1;
}

int read_block() {
    int c, s;
    read_c_s(&c, &s);

    char buf[BLOCK_SIZE + 1];

    // check location
    if (c >= CYLINDERS || s >= SECTORS_PC) {
        printf("=================== output ====================\n");
        fprintf(disk_log, "No\n");
        printf("Read: Location exceed\n");
        return 1;
    }

    check_file();

    printf("=================== output ====================\n");
    print_time(MOVE_DELAY * (abs(c - cur_cylinder)));   // track-to-track time
    cur_cylinder = c;

    int loc;
    loc = BLOCK_SIZE * (c * SECTORS_PC + s);
    memcpy(buf, &disk_file[loc], BLOCK_SIZE);

    // print and send message
    fprintf(disk_log, "Yes %s\n", buf);
    printf("Read completed: %s\n", buf);
    if (SOCKET_OPEN) {
        server_write(buf, BLOCK_SIZE);
    }

    return 1;
}

int write_block() {
    int c, s;
    char buf[BLOCK_SIZE + 1];
    buf[0] = '\0';

    read_c_s_buf(&c, &s, buf);

    if (strlen(buf) == 0 && !SOCKET_OPEN) {
        fprintf(disk_log, "No\n");
        return -1;
    }

    // check location
    if (c >= CYLINDERS || s >= SECTORS_PC) {
        printf("=================== output ====================\n");
        fprintf(disk_log, "No\n");
        printf("Write: Location exceed.\n");
        return 1;
    }

    check_file();

    printf("=================== output ====================\n");
    print_time(MOVE_DELAY * (abs(c - cur_cylinder)));
    cur_cylinder = c;

    int loc;
    loc = BLOCK_SIZE * (c * SECTORS_PC + s);
    memcpy(&disk_file[loc], buf, BLOCK_SIZE);

    // print and send message
    fprintf(disk_log, "Yes\n");
    printf("Write completed.\n");

    if (SOCKET_OPEN) {
        server_write(buf, BLOCK_SIZE);
    }

    return 1;
}

int set_block() {
    int c, s, ch;
    char buf[BLOCK_SIZE + 1];
    read_c_s_buf(&c, &s, buf);
    buf[3] = '\0';
    ch = atoi(buf);

    // check location
    if (c >= CYLINDERS || s >= SECTORS_PC) {
        fprintf(disk_log, "No\n");
        printf("Write: Location exceed.\n");
        return 1;
    }

    check_file();

    printf("=================== output ====================\n");
    print_time(MOVE_DELAY * (abs(c - cur_cylinder)));
    cur_cylinder = c;

    int loc;
    loc = BLOCK_SIZE * (c * SECTORS_PC + s);

    memset(&disk_file[loc], ch, BLOCK_SIZE);

    fprintf(disk_log, "Yes\n");
    printf("Memory set completed.\n");

    return 1;
}

int exit_sys() {
    if (strlen(buffer) == 1)
        return 0;
    else {
        return -1;
    }
}

int exe_command(char ch) {
    switch (ch) {
        case 'I':
            return show_org();
        case 'R':
            return read_block();
        case 'W':
            return write_block();
        case 'E':
            return exit_sys();
        case 'S':
            return set_block();
        default:
            return -1;
    }
}

void storage_polling() {
    // polling
    char ch;
    int state = 1;
    cur_cylinder = 0;
    // state = 1: resume
    // state = 0: exit and say Goodbye.
    // state = -1: instruction error
    while (1) {
        printf("=================== Command ===================\n");
        state = 1;
        bzero(buffer, MAX_LEN);
        if (SOCKET_OPEN) {
            server_read(buffer);  // read the output of fs
            if (TELNET_TEST) {
                buffer[strlen(buffer) - 2] = '\0';
            }
        } else {
            fgets(buffer, MAX_LEN, stdin);

            // delete '\n'
            buffer[strlen(buffer) - 1] = '\0';

            // delete ' '
            while (buffer[strlen(buffer) - 1] == ' ')
                buffer[strlen(buffer) - 1] = '\0';
        }

        // execute
        ch = buffer[0];
        state = exe_command(ch);

        if (state == 0) {   // exit
            printf("=================== output ====================\n");
            printf("Goodbye!\n");
            fprintf(disk_log, "Goodbye\n");
            break;
        }
        if (state == -1) {   // error
            printf("=================== output ====================\n");
            printf("Instruction error!\n");
        }
    }

    fclose(disk_log);
    if (SOCKET_OPEN)
        close(fd);
}

int main(int argc, char *argv[]) {

    input_detect(argc);

    storage_init(argv);

    if (SOCKET_OPEN) {
        init_server(argv);
    }

    storage_polling();

    if (SOCKET_OPEN) {
        close(sockfd);
        close(newsockfd);
    }

    return 0;
}