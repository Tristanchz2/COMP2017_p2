# Markdown Collaborative Editor

This readme is genreated by AI tool

This project implements a **multi-user collaborative Markdown editing system** based on **FIFOs (named pipes) + multithreading + signals**. The system contains two main components:

- **Server**: Maintains the shared document, processes commands, handles versioning, permissions, and broadcasts updates.
- **Client**: Communicates with the server, sends editing commands, and receives document updates.

This README explains how to compile and run the system, describes the architecture, communication protocol, and all supported commands.

---

## 🛠️ Compilation

Compile the server and client separately:

```bash
# Compile server
gcc -o server server.c -lpthread

# Compile client
gcc -o client client.c
```

Note: The server depends on `markdown.h` and its implementation. Ensure these files are included and properly linked.

---

## 🚀 Running the Programs

### **Start the Server**

```bash
./server <refresh_interval_ms>
```

Example:
```bash
./server 100
```

- The server prints its PID.
- The client must use this PID to connect.

### **Start a Client**

```bash
./client <server_pid> <username>
```

Example:
```bash
./client 12345 alice
```

The client sends a signal + FIFO handshake request to the server to establish communication.

---

## 🔐 Permission System

The server reads user permissions from `roles.txt`:

```
username role
```

Example:
```
alice write
bob read
```

- **write** — user can edit the document
- **read** — user can only view the document

If the user does not exist, the server sends:
```
Reject UNAUTHORISED
```

---

## 📡 Communication Overview

### 1. Client sends `SIGRTMIN` to request a connection.
### 2. Server creates two FIFOs:

```
FIFO_C2S_<pid>
FIFO_S2C_<pid>
```

and sends a `SIGRTMIN+1` signal to notify the client.

### 3. After connection:
The server sends the client:

1. User role (read/write)
2. Current document version
3. Document length
4. Document content

### 4. The client sends editing commands (see below).
### 5. The server's timing thread periodically processes commands and broadcasts updates.

---

## ✏️ Supported Editing Commands

All commands are sent from client to server.

### **Insert Text**
```
INSERT <pos> <string>
```

### **Delete Text**
```
DEL <pos> <length>
```

### **Insert Newline**
```
NEWLINE <pos>
```

### **Heading**
```
HEADING <level> <pos>
```

### **Bold**
```
BOLD <start> <end>
```

### **Italic**
```
ITALIC <start> <end>
```

### **Code Block**
```
CODE <start> <end>
```

### **Blockquote**
```
BLOCKQUOTE <pos>
```

### **Ordered List**
```
ORDERED_LIST <pos>
```

### **Unordered List**
```
UNORDERED_LIST <pos>
```

### **Horizontal Rule**
```
HORIZONTAL_RULE <pos>
```

### **Insert Link**
```
LINK <start> <end> <url>
```

---

## 📖 Query Commands

### **Query Document Content**
```
DOC?
```
Returns the Markdown content.

### **Query Permission**
```
PERM?
```
Returns either `read` or `write`.

---

## 🔄 Versioning System

- The server maintains a version linked list.
- The version number increments only when actual edits occur.
- Each cycle of the timing thread:
  - Processes pending commands
  - Performs permission checks
  - Updates version if needed
  - Broadcasts updated document to all clients

---

## 🔌 Client Disconnection

Client sends:
```
DISCONNECT
```

Server then:
- Marks client as offline
- Timing thread removes it
- `client_thread` terminates
- FIFOs are deleted

---

## 🛑 Server Shutdown

Server terminal input:
```
QUIT
```

Shutdown rules:
- If clients are online → server refuses to exit
- If no clients → clean up all FIFOs and versions
- Save the final document to `doc.md`

---

## 📁 Suggested Directory Structure

```
project/
│── server.c
│── client.c
│── libs/
│   └── markdown.h
│── roles.txt
│── doc.md (generated on exit)
└── README.md
```

---

## 🧪 Testing Steps

1. Open two terminals.
2. Start server in terminal A.
3. Start multiple clients in terminal B with different usernames.
4. In client, test commands:

```
INSERT 0 Hello
HEADING 1 0
DOC?
```

5. Verify that the server broadcasts updates correctly.

---

## ✔️ Finish
If you want, I can also create example shell scripts, architecture diagrams, or add more documentation sections—just tell me!

