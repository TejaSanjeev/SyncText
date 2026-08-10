# SyncText - CRDT-Based Collaborative Text Editor

A lock-free, real-time collaborative text editor using Conflict-Free Replicated Data Types (CRDT) with Last-Writer-Wins (LWW) conflict resolution.

## Platform 

- **Operating System**: Linux (Ubuntu 24.04.2 LTS)
- **Compiler**: g++ (Ubuntu 13.3.0-6ubuntu2~24.04) 13.3.0
- **Architecture**: x86_64

## Dependencies

The following libraries are required:

- **pthread** - POSIX threads for multi-threading support
- **rt** - Real-time extensions for message queues and shared memory

These libraries are typically pre-installed on Linux systems. No additional installation required.



## Compilation Instructions

```bash
g++ -std=c++17 src/*.cpp -lpthread -lrt -o editor
```

**Terminal 1 (User 1):**
```bash
./editor user_1
```

**Terminal 2 (User 2):**
```bash
./editor user_2
```

**Terminal 3 (User 3):**
```bash
./editor user_3
```

## How to Test

### Test 1: Basic Synchronization

1. Start all 3 users in separate terminals
2. Open `user_1_doc.txt` in any text editor (vim, nano, gedit, etc.)
3. Make changes to the file (add, modify, or delete text)
4. Save the file
5. Observe all 3 terminals update automatically

**Expected Result**: All users see the same changes after the global buffer reaches 5 combined updates.

### Test 2: Concurrent Editing

1. Start all 3 users
2. Open `user_1_doc.txt` in one editor - make 2 changes
3. Open `user_2_doc.txt` in another editor - make 2 changes
4. Open `user_3_doc.txt` in another editor - make 1 change
5. Save all files

**Expected Result**: After 5 total changes (threshold reached), all users synchronize and show merged content based on LWW conflict resolution.



