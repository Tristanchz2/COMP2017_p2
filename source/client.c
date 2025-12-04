// TODO: client code that can send instructions to server.
#include <stdio.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>

#define True 1
#define False 0

#define SUCCESS 0
#define UNSUCCESS 1

#define FIFO_NAME_LEN 32
#define command_number 12

volatile sig_atomic_t handshake = 0; // volatile type

// argv
pid_t server_pid;
char* username;

// doc
uint64_t version;
static const char *valid_cmds[] = {
    "INSERT", "DEL", "NEWLINE", "HEADING", "BOLD", "ITALIC", "BLOCKQUOTE",
    "ORDERED_LIST", "UNORDERED_LIST", "CODE", "HORIZONTAL_RULE", "LINK"
};

// === helper function ===
int is_valid_output(const char *line) {
    if (strncmp(line, "EDIT ", 5) != 0){
        return False;
    }

    char buf[256];
    strncpy(buf, line, sizeof(buf));
    buf[sizeof(buf)-1] = '\0';

    // divided the message into 3 parts name/command/message
    char *p = buf + 5;
    char *name_end = strchr(p, ' ');
    if (!name_end) return False;
    *name_end = '\0';
    char *command = name_end + 1;
    char *cmd_end = strchr(command, ' ');
    if (!cmd_end) return False;
    *cmd_end = '\0';
    char *message = cmd_end + 1;
    if (*message == '\0') return False;
    
    for (size_t i = 0; i < command_number; i++) {
        if (strcmp(command, valid_cmds[i]) == 0) {
            return True;
        }
    }
    return False;
}

/**
 * This thread is used to listen the message from the server
 * FIX: Accepts a FILE* stream, handles all subsequent reads, and closes the stream on exit.
 */
void* listener_thread(void* stream) {
    FILE* in = (FILE*)stream; 
    char line[256];

    while (fgets(line, sizeof(line), in)) {
        line[strcspn(line, "\n")] = '\0';

        if (strncmp(line, "VERSION", 7) == 0) {
            printf("%s\n", line); // print the version line

            while (fgets(line, sizeof(line), in)) {
                line[strcspn(line, "\n")] = '\0';
                if (strcmp(line, "END") == 0){
                    break;;
                }
                
                if (is_valid_output(line)) {
                    printf("%s\n", line);
                }  
            }
        } else {
            printf("%s\n", line); 
        }
    }
    
    // FIX: Close the stream when the server closes the pipe.
    fclose(in); 
    
    return NULL;
}

/**
 * This thread is used to handle stdin input
 */
void* stdin_thread(void* fd_c2s) {
    int fd = *(int*)fd_c2s;

    char input[256];
    while (fgets(input, sizeof(input), stdin)) {
        if (strncmp(input, "DISCONNECT", 10) == 0) {
            dprintf(fd, "DISCONNECT\n");
            break;
        }
        dprintf(fd, "%s\n", input);
    }
    return NULL;
}


/**
 * signal 
 */
void handle_sig(int sig) {
    if (sig == SIGRTMIN + 1) {
        handshake = True;
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <server_pid> <username>\n", argv[0]);
        return UNSUCCESS;
    }
    
    server_pid = atoi(argv[1]);
    username = argv[2];

    // make our handle_sig function work
    struct sigaction sa;
    sa.sa_handler = handle_sig;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGRTMIN + 1, &sa, NULL);

    // send signal to server
    kill(server_pid, SIGRTMIN);

    while (handshake == False) {
        usleep(20);
    }

    // open fifos
    pid_t client_pid = getpid(); 
    char fifo_c2s[FIFO_NAME_LEN], fifo_s2c[FIFO_NAME_LEN];
    snprintf(fifo_c2s, sizeof(fifo_c2s), "FIFO_C2S_%d", client_pid);
    snprintf(fifo_s2c, sizeof(fifo_s2c), "FIFO_S2C_%d", client_pid);

    int fd_c2s = open(fifo_c2s, O_WRONLY);
    if (fd_c2s < 0) {
        perror("Error opening C2S FIFO");
        return UNSUCCESS;
    }
    
    int fd_s2c = open(fifo_s2c, O_RDONLY);
    if (fd_s2c < 0) {
        perror("Error opening S2C FIFO");
        close(fd_c2s);
        return UNSUCCESS;
    }


    dprintf(fd_c2s, "%s\n", username); // send user name to server

    // handle return message from server
    FILE* in = fdopen(fd_s2c, "r");
    char line[256];
    
    if (fgets(line, sizeof(line), in) == NULL) {
        fclose(in);
        close(fd_c2s);
        return UNSUCCESS;
    }
    line[strcspn(line, "\n")] = '\0';

    if (strcmp(line, "Reject UNAUTHORISED") == 0) {
        fprintf(stderr, "Rejected: UNAUTHORISED\n"); 
        fclose(in);
        close(fd_c2s);
        return UNSUCCESS;
    }

    // Read Permission
    char permission[16];
    strncpy(permission, line, sizeof(permission));
    permission[sizeof(permission) - 1] = '\0';
    printf("Permission received: %s\n", permission);

    if (fgets(line, sizeof(line), in) == NULL) { // get version
        fclose(in); close(fd_c2s); return UNSUCCESS;
    }
    uint64_t version = strtoull(line, NULL, 10); 
    printf("Initial Version: %lu\n", version);

    if (fgets(line, sizeof(line), in) == NULL) { // get document len
        fclose(in); close(fd_c2s); return UNSUCCESS;
    }
    size_t doc_len = strtoull(line, NULL, 10); 
    printf("Initial Document Length: %zu\n", doc_len);

    char* doc = malloc(doc_len + 1);
    if (doc == NULL) {
        perror("malloc failed");
        fclose(in); close(fd_c2s); return UNSUCCESS;
    }
    
    // Read the document content
    size_t bytes_read = fread(doc, 1, doc_len, in); 
    doc[bytes_read] = '\0';
    printf("Initial Document Content:\n---\n%s\n---\n", doc);

    // FIX: Consume the extra newline separator sent by the server
    char newline_char;
    fread(&newline_char, 1, 1, in); 

    pthread_t listener_thread_tid;
    pthread_create(&listener_thread_tid, NULL, listener_thread, in); 

    pthread_t stdin_thread_tid;
    pthread_create(&stdin_thread_tid, NULL, stdin_thread, &fd_c2s);

    pthread_join(stdin_thread_tid, NULL);
    
    // FIX: Clean up fds opened by main thread
    close(fd_c2s); // Close the write pipe to signal end of input to server
    pthread_detach(listener_thread_tid); // Allow listener thread to clean up stream (fclose(in)) in background
    
    free(doc);
    
    return SUCCESS;
}
