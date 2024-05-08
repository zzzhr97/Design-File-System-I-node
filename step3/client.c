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

#define MAX_INPUT 256   // the maximum input length
#define MAX_OUTPUT 1024 // the maximum output length

static int sockfd;      // socket with fs.c

// Initialize client.
void init_client(char *argv[]) {
    int portno;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    char host_name[20] = "localhost";

    portno = atoi(argv[1]);
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        printf("Error: opening socket.\n");
        exit(-1);
    }
    server = gethostbyname(host_name);
    if (server == NULL) {
        printf("Error: no such host.\n");
        exit(-1);
    }
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy((char *) server->h_addr,
          (char *) &serv_addr.sin_addr.s_addr,
          server->h_length);
    serv_addr.sin_port = htons(portno);
    if (connect(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        printf("Error: connecting.\n");
        exit(-1);
    }
}

// Read the input from stdin.
void get_input(char buffer[MAX_OUTPUT]) {
    bzero(buffer, MAX_OUTPUT);
    fgets(buffer, MAX_OUTPUT, stdin);
}

// Write the input to server.
void client_write(char buffer[MAX_OUTPUT]) {
    int n = write(sockfd, buffer, strlen(buffer));
    if (n < 0) {
        printf("Error: writing to socket.\n");
        exit(-1);
    }
}

// Read the output from server and output it.
// Do not output '\n'
void client_read() {
    char buffer[MAX_OUTPUT];
    bzero(buffer, MAX_OUTPUT);
    int n = read(sockfd, buffer, MAX_OUTPUT);
    if (n < 0) {
        printf("Error: reading from socket.\n");
        exit(-1);
    }
    printf("%s", buffer);
}

void polling() {
    char buffer[MAX_OUTPUT];
    while (1) {
        printf("=================== Command ===================\n");

        // read from server: get current directory
        client_read();

        // input command
        get_input(buffer);

        // write to server: command
        client_write(buffer);

        printf("=================== output ====================\n");

        if (0 == strcmp("e\n", buffer)) {
            printf("Goodbye!\n");
            break;
        }

        // read from server: output
        client_read();

        // send a '\n' to fs.c to
        // act as a separation between two read
        buffer[0] = '\n';
        buffer[1] = '\0';
        client_write(buffer);
    }
}

int main(int argc, char *argv[]) {

    if (argc < 2) {
        printf("Error: no port provided.\n");
        exit(-1);
    } else if (argc > 2) {
        printf("Error: Input error.\n");
        exit(-1);
    } else {
        init_client(argv);
        polling();
        close(sockfd);
    }
    return 0;
}