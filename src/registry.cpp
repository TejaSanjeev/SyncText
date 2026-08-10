#include "registry.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <atomic>

using namespace std;

struct RegistryEntry
{
    atomic<bool> active;
    char user_id[32];
};

struct SharedRegistry
{
    static const int MAX_USERS = 5;
    RegistryEntry entries[MAX_USERS];
};

static const char *SHM_NAME = "/synctext_registry4";

bool Registry::register_user(const string &uid)
{
    int fd = shm_open(SHM_NAME, O_RDWR | O_CREAT, 0666);
    if (fd < 0)
    {
        perror("shm_open");
        return false;
    }

    struct stat shm_stat;
    fstat(fd, &shm_stat);
    bool is_new = shm_stat.st_size == 0;

    ftruncate(fd, sizeof(SharedRegistry));
    void *addr = mmap(nullptr, sizeof(SharedRegistry), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
    {
        perror("mmap");
        close(fd);
        return false;
    }

    SharedRegistry *reg = static_cast<SharedRegistry *>(addr);

    if (is_new)
    {
        memset(reg, 0, sizeof(SharedRegistry));
        cout << "[Registry] Initialized new shared memory." << endl;
    }

    for (int i = 0; i < SharedRegistry::MAX_USERS; ++i)
    {
        bool expected = false;
        if (reg->entries[i].active.compare_exchange_strong(expected, true))
        {
            strncpy(reg->entries[i].user_id, uid.c_str(), sizeof(reg->entries[i].user_id) - 1);
            cout << "[Registry] Registered user: " << uid << " in slot " << i << endl;
            munmap(addr, sizeof(SharedRegistry));
            close(fd);
            return true;
        }
    }

    cerr << "[Registry] No free slots for new user!" << endl;
    munmap(addr, sizeof(SharedRegistry));
    close(fd);
    return false;
}

void Registry::unregister_user(const string &uid)
{
    int fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (fd < 0)
        return;
    void *addr = mmap(nullptr, sizeof(SharedRegistry), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
    {
        close(fd);
        return;
    }
    SharedRegistry *reg = static_cast<SharedRegistry *>(addr);

    for (int i = 0; i < SharedRegistry::MAX_USERS; ++i)
    {
        if (reg->entries[i].active &&
            uid == reg->entries[i].user_id)
        {
            reg->entries[i].active = false;
            memset(reg->entries[i].user_id, 0, sizeof(reg->entries[i].user_id));
            cout << "[Registry] Unregistered user: " << uid << endl;
            break; // Found the user, no need to continue
        }
    }
    munmap(addr, sizeof(SharedRegistry));
    close(fd);
}

void Registry::list_users()
{
    int fd = shm_open(SHM_NAME, O_RDONLY, 0666);
    if (fd < 0)
    {
        perror("shm_open");
        return;
    }

    void *addr = mmap(nullptr, sizeof(SharedRegistry), PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
    {
        close(fd);
        return;
    }
    SharedRegistry *reg = static_cast<SharedRegistry *>(addr);

    cout << "Active users:" << endl;
    for (int i = 0; i < SharedRegistry::MAX_USERS; ++i)
    {
        if (reg->entries[i].active)
            cout << " - " << reg->entries[i].user_id << endl;
    }
    munmap(addr, sizeof(SharedRegistry));
    close(fd);
}

vector<string> Registry::get_all_users()
{
    vector<string> users;
    int fd = shm_open(SHM_NAME, O_RDONLY, 0666);
    if (fd < 0)
    {
        return users;
    }

    void *addr = mmap(nullptr, sizeof(SharedRegistry), PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
    {
        close(fd);
        return users;
    }
    SharedRegistry *reg = static_cast<SharedRegistry *>(addr);

    for (int i = 0; i < SharedRegistry::MAX_USERS; ++i)
    {
        if (reg->entries[i].active)
            users.push_back(reg->entries[i].user_id);
    }
    munmap(addr, sizeof(SharedRegistry));
    close(fd);
    return users;
}
