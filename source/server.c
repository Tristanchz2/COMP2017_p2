// TODO: server code that manages the document and handles client instructions
#include <stdio.h>

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "../libs/markdown.h" // Assuming this library exists

#define FIFO_NAME_LEN 32
#define True 1
#define False 0
#define Reject_INVALID 10
#define Reject_FORBIDDEN 11
#define SUCCESS 0
#define REJECTED 1
#define INVALID_CURSOR_POS -1
#define DELETED_POSITION -2
#define OUTDATED_VERSION -3
#define MODIFIED 1
#define NOT_MODIFIED 0

// Structure definitions (unchanged)
typedef struct client {
    pid_t pid;
    char role[8]; // "read" or "write"
    int fd_c2s;
    int fd_s2c;
    pthread_t thread;
    struct client* next;
    int online;
    int handshake;
} client;

typedef struct command {
    char text[256]; // fixed in length
    struct command* next;
    struct client* sender;
    int is_finish;
} command;

typedef struct version {
    struct command* head;
    uint64_t num;
    struct version* next;
} version;


// === static variable ===
static int online = True;
static document* doc = NULL;
static client* clients = NULL; // the clients linked list
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static version* versions = NULL; // the version linked list
static version* current_version = NULL; // used to store the current version
static pthread_mutex_t version_lock = PTHREAD_MUTEX_INITIALIZER;

// === function declarations (For Linker) ===
int modify_authorization(client* cli);
void message(client* cli, int return_code);
const char* get_user_role(const char* username);
int create_fifos(pid_t pid, char* c2s, char* s2c);
client* init_client(pid_t pid, int fd_c2s, int fd_s2c, const char* role);
void handshake_disconnected_clients();

// Command handler declarations
void handle_doc(client *cli);
void handle_perm(client* cli);
int handle_insert(char* text);
int handle_delete(char *text);
int handle_newline(char *text);
int handle_heading(char *text);
int handle_bold(char *text);
int handle_italic(char *text);
int handle_blockquote(char *text);
int handle_ordered_list(char *text);
int handle_unordered_list(char *text);
int handle_code(char *text);
int handle_horizontal_rule(char *text);
int handle_link(char *text);

// Thread function declarations
void* console_thread(void* arg); 
void* client_thread(void* c); 
void* timing_thread(void* arg); 


// === helper function DEFINITIONS ===

/**
 * This function is used to help getting the role of the client. If client is found in the
 * roles.txt, return the role, else, return NULL
 */
const char* get_user_role(const char* username) {
    FILE* file = fopen("roles.txt", "r");
    if (!file) return NULL;

    static char role[32]; // static to return
    char line[128];

    while (fgets(line, sizeof(line), file)) {
        char* real_line = strtok(line, "\n");
        if (!real_line) continue;

        char* user_token = strtok(real_line, " ");
        char* role_token = strtok(NULL, " ");

        if (user_token && role_token && strcmp(user_token, username) == 0) {
            strncpy(role, role_token, sizeof(role));
            fclose(file);
            return role;
        }
    }

    fclose(file);
    return NULL;
}

// === Client initialization ===
/**
 * Initialize the FIFO, rename the c2s and s2c according to requirement
 */
int create_fifos(pid_t pid, char* c2s, char* s2c) {
    snprintf(c2s, FIFO_NAME_LEN, "FIFO_C2S_%d", pid);
    snprintf(s2c, FIFO_NAME_LEN, "FIFO_S2C_%d", pid);
    unlink(c2s);
    unlink(s2c);
    return mkfifo(c2s, 0666) == 0 && mkfifo(s2c, 0666) == 0;
}

/**
 * Init the client struct basically
 */
client* init_client(pid_t pid, int fd_c2s, int fd_s2c, const char* role) {
    client* new_client = malloc(sizeof(client));
    if (!new_client) return NULL;

    // set the fifos
    new_client->fd_c2s = fd_c2s;
    new_client->fd_s2c = fd_s2c;

    // init other attributes
    new_client->pid = pid;
    new_client->online = True;
    new_client->handshake = False;
    strncpy(new_client->role, role, sizeof(new_client->role));
    new_client->role[sizeof(new_client->role) - 1] = '\0';
    new_client->next = NULL;
    return new_client;
}

/**
 * This function is used to traverse the clients list and remove all the offline client
 */
void handshake_disconnected_clients() {
    pthread_mutex_lock(&clients_lock);

    client* cur = clients;
    client* prev = NULL;

    while (cur) {
        if (cur->online == False) {
            cur->handshake = True;

            // remove from the list
            if (prev) {
                prev->next = cur->next;
            } else {
                clients = cur->next;
            }
            
            client* to_free = cur;
            cur = cur->next;
            free(to_free); // FIX: Free the client structure memory here.
        } else {
            prev = cur;
            cur = cur->next;
        }
    }

    pthread_mutex_unlock(&clients_lock);
}

// === handle command line function ===
void handle_doc(client *cli) {
    char *content = markdown_flatten(doc);
    dprintf(cli->fd_s2c, "%s\n", content);
    free(content);
}

void handle_perm(client* cli) {
    dprintf(cli->fd_s2c, "%s\n", cli->role);
}

int handle_insert(char* text) {
    size_t pos;
    char content[256];
    if (sscanf(text, "INSERT %lu %[^\n]", &pos, content) != 2){
        return INVALID_CURSOR_POS;
    }
    return markdown_insert(doc, current_version->num, pos, content);
}

int handle_delete(char *text) {
    size_t pos, len;
    if (sscanf(text, "DEL %lu %lu", &pos, &len) != 2) {
        return INVALID_CURSOR_POS;
    }
    return markdown_delete(doc, current_version->num, pos, len);
}

int handle_newline(char *text) {
    size_t pos;
    if (sscanf(text, "NEWLINE %lu", &pos) != 1) {
        return INVALID_CURSOR_POS;
    }
    return markdown_newline(doc, current_version->num, pos);
}

int handle_heading(char *text) {
    int level;
    size_t pos;
    if (sscanf(text, "HEADING %d %lu", &level, &pos) != 2) {
        return INVALID_CURSOR_POS;
    }
    return markdown_heading(doc, current_version->num, level, pos);
}

int handle_bold(char *text) {
    size_t start, end;
    if (sscanf(text, "BOLD %lu %lu", &start, &end) != 2) {
        return INVALID_CURSOR_POS;
    }
    return markdown_bold(doc, current_version->num, start, end);
}

int handle_italic(char *text) {
    size_t start, end;
    if (sscanf(text, "ITALIC %lu %lu", &start, &end) != 2) {
        return INVALID_CURSOR_POS;
    }
    return markdown_italic(doc, current_version->num, start, end);
}

int handle_blockquote(char *text) {
    size_t pos;
    if (sscanf(text, "BLOCKQUOTE %lu", &pos) != 1) {
        return INVALID_CURSOR_POS;
    }
    return markdown_blockquote(doc, current_version->num, pos);
}

int handle_ordered_list(char *text) {
    size_t pos;
    if (sscanf(text, "ORDERED_LIST %lu", &pos) != 1) {
        return INVALID_CURSOR_POS;
    }
    return markdown_ordered_list(doc, current_version->num, pos);
}

int handle_unordered_list(char *text) {
    size_t pos;
    if (sscanf(text, "UNORDERED_LIST %lu", &pos) != 1) {
        return INVALID_CURSOR_POS;
    }
    return markdown_unordered_list(doc, current_version->num, pos);
}

int handle_code(char *text) {
    size_t start, end;
    if (sscanf(text, "CODE %lu %lu", &start, &end) != 2) {
        return INVALID_CURSOR_POS;
    }
    return markdown_code(doc, current_version->num, start, end);
}

int handle_horizontal_rule(char *text) {
    size_t pos;
    if (sscanf(text, "HORIZONTAL_RULE %lu", &pos) != 1) {
        return INVALID_CURSOR_POS;
    }
    return markdown_horizontal_rule(doc, current_version->num, pos);
}

int handle_link(char *text) {
    size_t start, end;
    char url[256];
    if (sscanf(text, "LINK %lu %lu %255s", &start, &end, url) != 3) {
        return INVALID_CURSOR_POS;
    }
    return markdown_link(doc, current_version->num, start, end, url);
}

/**
 * FIX: Restored modify_authorization definition
 */
int modify_authorization(client* cli){
    if (strncmp(cli->role, "write", 5) != 0){
        char msg[50];
        strcpy(msg, "UNAUTHORISED <INSERT> <write> <read>");
        write(cli->fd_s2c, msg, strlen(msg));
        return REJECTED;
    }
    return SUCCESS;
}

/**
 * FIX: Restored message definition
 */
void message(client* cli, int return_code){
    char msg[50];
    
    if (return_code == INVALID_CURSOR_POS) {
        strcpy(msg, "INVALID_POSITION\n");
    } else if (return_code == DELETED_POSITION) {
        strcpy(msg, "DELETED_POSITION\n");
    } else if (return_code == OUTDATED_VERSION) {
        strcpy(msg, "OUTDATED_VERSION\n");
    } else if (return_code == SUCCESS) {
        strcpy(msg, "SUCCESS\n");
    }

    write(cli->fd_s2c, msg, strlen(msg));
}


// === Thread function DEFINITIONS ===
/**
 * This is a client thread fucntion, used to recieve meassgae from client and write to the command list
 */
void* client_thread(void* c) {
    client* cli = (client*)c; // get the client struct

    // get the current content from doc and send message to client as required
    char* content = markdown_flatten(doc);
    size_t len = strlen(content);
    dprintf(cli->fd_s2c, "%s\n", cli->role); // role
    dprintf(cli->fd_s2c, "%lu\n", doc->version); // version
    dprintf(cli->fd_s2c, "%lu\n", len); // len
    write(cli->fd_s2c, content, len); // content
    
    // FIX: Send a newline separator to handle client fread/fgets transition
    write(cli->fd_s2c, "\n", 1); 
    
    free(content);

    // FIX: Add a short delay to ensure client receives initial document (WSL workaround)
    usleep(100000); // Wait 100ms 

    FILE* in = fdopen(cli->fd_c2s, "r");
    char line[256];
    while (fgets(line, sizeof(line), in)) {
        line[strcspn(line, "\n")] = '\0';

        if (strcmp(line, "DISCONNECT") == 0) {
            pthread_mutex_lock(&clients_lock);
            cli->online = False; // Set flag for timing thread to clean up
            pthread_mutex_unlock(&clients_lock);
            
            // continue waiting until timing thread deal with all message and give us a handshake
            while (!cli->handshake){
                usleep(10);
            }

            // close stream/fds
            fclose(in); // Closes cli->fd_c2s
            close(cli->fd_s2c);

            // unlink fifos
            char fifo_c2s[FIFO_NAME_LEN], fifo_s2c[FIFO_NAME_LEN];
            snprintf(fifo_c2s, FIFO_NAME_LEN, "FIFO_C2S_%d", cli->pid);
            snprintf(fifo_s2c, FIFO_NAME_LEN, "FIFO_S2C_%d", cli->pid);
            unlink(fifo_c2s);
            unlink(fifo_s2c);

            return NULL;
        }
        
        // create a new command
        command* com = malloc(sizeof(command));
        if (!com) continue; // handle malloc failure
        
        strncpy(com->text, line, sizeof(com->text));
        com->text[sizeof(com->text) - 1] = '\0';
        com->sender = cli;
        com->next = NULL;
        com->is_finish = False;

        // get the lock for version and add one command at the end
        pthread_mutex_lock(&version_lock);
        if (!current_version->head) {
            current_version->head = com;
        } else {
            command* cur = current_version->head;
            while (cur->next) {
                cur = cur->next;
            }
            cur->next = com;
        }
        pthread_mutex_unlock(&version_lock);
    }

    // Handle case where client pipe is closed unexpectedly (e.g. client close(fd_c2s))
    pthread_mutex_lock(&clients_lock);
    cli->online = False;
    pthread_mutex_unlock(&clients_lock);
    // Wait for cleanup by timing thread
    while (!cli->handshake){
        usleep(10);
    }
    // close and unlink
    if (in) fclose(in); 
    if (cli->fd_s2c >= 0) close(cli->fd_s2c);
    
    char fifo_c2s[FIFO_NAME_LEN], fifo_s2c[FIFO_NAME_LEN];
    snprintf(fifo_c2s, FIFO_NAME_LEN, "FIFO_C2S_%d", cli->pid);
    snprintf(fifo_s2c, FIFO_NAME_LEN, "FIFO_S2C_%d", cli->pid);
    unlink(fifo_c2s);
    unlink(fifo_s2c);
    
    return NULL;
}

/**
 * This is a timing thread fucntion. Each time interval, deal with all the command and boardcast lastest version
 */
void* timing_thread(void* arg) {
    int interval = *(int*)arg;
    free(arg);

    while (True) {
        usleep(interval * 1000);
        
        pthread_mutex_lock(&version_lock); // acquire the lock for the command line

        command* head = current_version->head;
        command* cur = head;
        
        // Deal with all the command
        while(cur){
            if (cur->is_finish == True){
                cur = cur->next;
                continue;
            }
            
            // --- Command Processing Block ---
            int result = SUCCESS;
            
            if (strncmp(cur->text, "INSERT", 6) == 0) {
                if (modify_authorization(cur->sender) == REJECTED){
                    result = REJECTED;
                } else {
                    result = handle_insert(cur->text);
                }
            } else if (strncmp(cur->text, "DEL", 3) == 0) {
                if (modify_authorization(cur->sender) == REJECTED) {
                    result = REJECTED;
                } else {
                    result = handle_delete(cur->text);
                }
            } else if (strncmp(cur->text, "NEWLINE", 7) == 0) {
                if (modify_authorization(cur->sender) == REJECTED) {
                    result = REJECTED;
                } else {
                    result = handle_newline(cur->text);
                }
            } else if (strncmp(cur->text, "HEADING", 7) == 0) {
                if (modify_authorization(cur->sender) == REJECTED) {
                    result = REJECTED;
                } else {
                    result = handle_heading(cur->text);
                }
            } else if (strncmp(cur->text, "BOLD", 4) == 0) {
                if (modify_authorization(cur->sender) == REJECTED) {
                    result = REJECTED;
                } else {
                    result = handle_bold(cur->text);
                }
            } else if (strncmp(cur->text, "ITALIC", 6) == 0) {
                if (modify_authorization(cur->sender) == REJECTED) {
                    result = REJECTED;
                } else {
                    result = handle_italic(cur->text);
                }
            } else if (strncmp(cur->text, "BLOCKQUOTE", 10) == 0) {
                if (modify_authorization(cur->sender) == REJECTED) {
                    result = REJECTED;
                } else {
                    result = handle_blockquote(cur->text);
                }
            } else if (strncmp(cur->text, "ORDERED_LIST", 12) == 0) {
                if (modify_authorization(cur->sender) == REJECTED) {
                    result = REJECTED;
                } else {
                    result = handle_ordered_list(cur->text);
                }
            } else if (strncmp(cur->text, "UNORDERED_LIST", 14) == 0) {
                if (modify_authorization(cur->sender) == REJECTED) {
                    result = REJECTED;
                } else {
                    result = handle_unordered_list(cur->text);
                }
            } else if (strncmp(cur->text, "CODE", 4) == 0) {
                if (modify_authorization(cur->sender) == REJECTED) {
                    result = REJECTED;
                } else {
                    result = handle_code(cur->text);
                }
            } else if (strncmp(cur->text, "HORIZONTAL_RULE", 15) == 0) {
                if (modify_authorization(cur->sender) == REJECTED) {
                    result = REJECTED;
                } else {
                    result = handle_horizontal_rule(cur->text);
                }
            } else if (strncmp(cur->text, "LINK", 4) == 0) {
                if (modify_authorization(cur->sender) == REJECTED) {
                    result = REJECTED;
                } else {
                    result = handle_link(cur->text);
                }
            } else if (strncmp(cur->text, "DOC?", 4) == 0){
                handle_doc(cur->sender);
                result = SUCCESS; // Handled, not an error
            } else if (strcmp(cur->text, "PERM?") == 0) {
                handle_perm(cur->sender);
                result = SUCCESS; // Handled, not an error
            }

            // Send standard response for modification commands
            if (result != SUCCESS && result != REJECTED && 
                strncmp(cur->text, "DOC?", 4) != 0 && strcmp(cur->text, "PERM?") != 0) {
                message(cur->sender, result); 
            }
            
            // Mark as finish
            cur->is_finish = True;
            cur = cur->next; 
        } // End of command processing loop

        // --- Memory Cleanup for Commands ---
        cur = head;
        command* next_com = NULL;
        command* new_head = NULL;
        
        // Find the new head (the first command that hasn't finished yet)
        while (cur && cur->is_finish == True) {
            next_com = cur->next;
            free(cur);
            cur = next_com;
        }
        new_head = cur;
        
        // Re-link the remaining commands
        if (new_head) {
            command* prev_clean = new_head;
            cur = new_head->next;
            while (cur) {
                if (cur->is_finish == True) {
                    next_com = cur->next;
                    prev_clean->next = next_com; // Unlink and bypass
                    free(cur);
                    cur = next_com;
                } else {
                    prev_clean = cur;
                    cur = cur->next;
                }
            }
        }
        current_version->head = new_head;
        // --- End of Cleanup ---
        
        // set the handshake for offline client
        handshake_disconnected_clients();

        // increment the version
        if (doc->is_modify == MODIFIED) {
            markdown_increment_version(doc);
            version* ver = malloc(sizeof(version));
            if (!ver) { /* Handle malloc error for version */ }
            ver->head = NULL;
            ver->next = NULL;
            ver->num = current_version->num + 1;
            current_version->next = ver;
            current_version = ver;
        }

        pthread_mutex_unlock(&version_lock);
    }

    return NULL;
}


// === console thread ===
/**
 * This function is used to keep listen the quit command and exit the program gracefully
 */
void* console_thread(void* arg) {
    char line[64];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = '\0';

        if (strcmp(line, "QUIT") == 0) {
            // determine if there is any client online
            pthread_mutex_lock(&clients_lock);
            int count = 0;
            client* c = clients;
            while (c) {
                count++;
                c = c->next;
            }

            // stop the server in the critical section
            if (count == 0){
                online = False;
            }

            pthread_mutex_unlock(&clients_lock);
            
            if (count > 0) {
                printf("QUIT rejected, %d clients still connected.\n", count);
                continue;
            }

            // clean all pipes
            system("rm -f FIFO_C2S_* FIFO_S2C_*");

            // iterate to free all version and commands
            pthread_mutex_lock(&version_lock);
            version* ver = versions;
            while (ver) {
                command* cmd = ver->head;
                while (cmd) {
                    command* to_free_cmd = cmd;
                    cmd = cmd->next;
                    free(to_free_cmd);
                }
                version* to_free_ver = ver;
                ver = ver->next;
                free(to_free_ver);
            }
            pthread_mutex_unlock(&version_lock);

            // save the doc.md
            char* content = markdown_flatten(doc);
            FILE* f = fopen("doc.md", "w");
            if (f) {
                fwrite(content, 1, strlen(content), f);
                fclose(f);
            }
            free(content);

            markdown_free(doc);
            exit(0);
        }
    }
    return NULL;
}


// === Main ===
int main(int argc, char* argv[]) {
    // FIX: Ensure correct parameter checking for the server
    if (argc < 2) { 
        fprintf(stderr, "Usage: %s <time_interval_ms>\n", argv[0]); 
        return 1;
    }
    
    int time_interval = atoi(argv[1]); // get the time interval
    if (time_interval <= 0) time_interval = 100; // Sanity check
    
    printf("Server PID: %d\n", getpid()); // send pid

    doc = markdown_init();

    // create the first version;
    versions = malloc(sizeof(version));
    if (!versions) return 1; // Handle malloc failure
    current_version = versions;
    versions->num = 1;
    versions->next = NULL;
    versions->head = NULL;

    // start the console thread
    pthread_t console_thread_id;
    pthread_create(&console_thread_id, NULL, console_thread, NULL);

    // start the timing thread
    pthread_t timing_thread_id;
    int* buffer = malloc(sizeof(int));
    if (!buffer) return 1; // Handle malloc failure
    *buffer = time_interval;
    pthread_create(&timing_thread_id, NULL, timing_thread, buffer);

    // siginal
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGRTMIN);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    while (True) {
        siginfo_t info;
        sigwaitinfo(&mask, &info); // block the server unitll someone is trying to connect

        if (online == False) continue; // don't create new client when quit is set, stay until exit(0)

        // init
        pid_t pid = info.si_pid; // get pid
        char fifo_c2s[FIFO_NAME_LEN], fifo_s2c[FIFO_NAME_LEN]; // create fd
        // FIX: Check if FIFO creation succeeded
        if (!create_fifos(pid, fifo_c2s, fifo_s2c)) { 
            perror("mkfifo failed");
            continue;
        }

        kill(pid, SIGRTMIN + 1); // send signal to client

        // FIX: Check for open errors
        int fd_c2s = open(fifo_c2s, O_RDONLY);
        if (fd_c2s < 0) {
            perror("open C2S failed");
            unlink(fifo_c2s); unlink(fifo_s2c);
            continue;
        }
        int fd_s2c = open(fifo_s2c, O_WRONLY); // open two fds
        if (fd_s2c < 0) {
            perror("open S2C failed");
            close(fd_c2s);
            unlink(fifo_c2s); unlink(fifo_s2c);
            continue;
        }

        char temp[64];
        // Read client username (first message sent to server)
        ssize_t len = read(fd_c2s, temp, sizeof(temp) - 1);
        if (len <= 0) {
            close(fd_c2s); close(fd_s2c);
            unlink(fifo_c2s); unlink(fifo_s2c);
            continue;
        }

        temp[len] = '\0'; // make it a string to use the strcspn function below
        temp[strcspn(temp, "\n")] = '\0'; // remove all the newline

        const char* role = get_user_role(temp); // find aceess authority of the user 
        
        // not found in the document
        if (!role) {
            dprintf(fd_s2c, "Reject UNAUTHORISED\n");
            // Give time for client to receive and read
            sleep(1); 
            
            // close fd and unlink fifo
            close(fd_c2s); close(fd_s2c);
            unlink(fifo_c2s); unlink(fifo_s2c);
            continue;
        }

        // found in the document, init a client server
        client* cli = init_client(pid, fd_c2s, fd_s2c, role);
        if (!cli) {
            close(fd_c2s); close(fd_s2c);
            unlink(fifo_c2s); unlink(fifo_s2c);
            continue;
        }

        // FIX: Must protect the clients linked list modification
        pthread_mutex_lock(&clients_lock);
        cli->next = clients;
        clients = cli;
        pthread_mutex_unlock(&clients_lock);

        pthread_create(&cli->thread, NULL, client_thread, cli);
        pthread_detach(cli->thread); // auto detect and end of the thread and clean it
    }
    return 0;
}
