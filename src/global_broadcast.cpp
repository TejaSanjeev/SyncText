#include "global_broadcast.h"
#include "crdt_merge.h"
#include "messaging.h"
#include "registry.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <climits>

using namespace std;

static const char *GLOBAL_BUFFER_SHM_NAME = "/synctext_global_buffer";
static GlobalBuffer *g_buffer = nullptr;

bool GlobalBroadcast::init_global_buffer()
{
    int fd = shm_open(GLOBAL_BUFFER_SHM_NAME, O_RDWR | O_CREAT, 0666);
    if (fd < 0)
    {
        perror("shm_open (global buffer)");
        return false;
    }

    struct stat shm_stat;
    fstat(fd, &shm_stat);
    bool is_new = shm_stat.st_size == 0;

    ftruncate(fd, sizeof(GlobalBuffer));
    void *addr = mmap(nullptr, sizeof(GlobalBuffer), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
    {
        perror("mmap (global buffer)");
        close(fd);
        return false;
    }

    g_buffer = static_cast<GlobalBuffer *>(addr);

    if (is_new)
    {
        memset(g_buffer, 0, sizeof(GlobalBuffer));
        g_buffer->count.store(0);
        g_buffer->broadcast_in_progress.store(false);
        cout << "[GlobalBuffer] Initialized new global buffer." << endl;
    }

    close(fd);
    return true;
}

bool GlobalBroadcast::add_to_global_buffer(const UpdateObject &update)
{
    if (!g_buffer)
    {
        cerr << "[GlobalBuffer] Buffer not initialized!" << endl;
        return false;
    }

    int wait_count = 0;
    while (g_buffer->broadcast_in_progress.load())
    {
        this_thread::sleep_for(chrono::milliseconds(10));
        wait_count++;
        if (wait_count > 100)
        { 
            cerr << "[GlobalBuffer] Timeout waiting for broadcast to complete!" << endl;
            return false;
        }
    }

    int current_count = g_buffer->count.fetch_add(1);

    if (current_count >= GlobalBuffer::MAX_BUFFERED_UPDATES)
    {
        g_buffer->count.fetch_sub(1);
        cout << "[GlobalBuffer] Buffer full, waiting..." << endl;
        this_thread::sleep_for(chrono::milliseconds(100));
        return false;
    }

    memcpy(&g_buffer->updates[current_count], &update, sizeof(UpdateObject));

    atomic_thread_fence(std::memory_order_release);

    int new_count = current_count + 1;
    cout << "[GlobalBuffer] Added update from " << update.user_id
         << " (buffer count: " << new_count << "/5)" << endl;

    return new_count >= 5;
}

vector<UpdateObject> GlobalBroadcast::get_and_clear_buffer()
{
    vector<UpdateObject> updates;

    if (!g_buffer)
    {
        return updates;
    }

    bool expected = false;
    if (!g_buffer->broadcast_in_progress.compare_exchange_strong(expected, true))
    {
        return updates;
    }

    atomic_thread_fence(std::memory_order_acquire);

    int count = g_buffer->count.load();

    if (count < 0 || count > GlobalBuffer::MAX_BUFFERED_UPDATES)
    {
        cerr << "[GlobalBuffer] Invalid buffer count: " << count << endl;
        g_buffer->broadcast_in_progress.store(false);
        return updates;
    }

    for (int i = 0; i < count; ++i)
    {
        updates.push_back(g_buffer->updates[i]);
    }

    memset(g_buffer->updates, 0, sizeof(g_buffer->updates));
    g_buffer->count.store(0);

    atomic_thread_fence(std::memory_order_release);

    g_buffer->broadcast_in_progress.store(false);

    return updates;
}

void GlobalBroadcast::broadcast_merged_updates(const vector<UpdateObject> &updates, const string &exclude_user)
{
    if (updates.empty())
        return;

    vector<string> users = Registry::get_all_users();

    cout << "\n========================================" << endl;
    cout << "[GlobalBroadcast] Broadcasting " << updates.size() << " merged updates to all users" << endl;
    cout << "========================================\n"
         << endl;

    for (const auto &update : updates)
    {
        for (const string &user_id : users)
        {
            if (user_id != exclude_user)
            {
                send_update(user_id, update);
            }
        }
    }

    cout << "[GlobalBroadcast] Broadcast complete to " << users.size() << " users." << endl;
}

void GlobalBroadcast::monitor_global_buffer(const string &user_id)
{
    while (true)
    {
        this_thread::sleep_for(chrono::milliseconds(500));

        if (!g_buffer)
            continue;

        int count = g_buffer->count.load();

        if (count >= 5 && !g_buffer->broadcast_in_progress.load())
        {
            cout << "\n[GlobalBroadcast] Threshold reached! Processing " << count << " updates..." << endl;

            vector<UpdateObject> all_updates = get_and_clear_buffer();

            if (all_updates.empty())
            {
                continue; 
            }

            cout << "[GlobalBroadcast] Resolving conflicts..." << endl;

            sort(all_updates.begin(), all_updates.end(),
                 [](const UpdateObject &a, const UpdateObject &b)
                 {
                     if (a.timestamp_ms != b.timestamp_ms)
                         return a.timestamp_ms < b.timestamp_ms;
                     return strcmp(a.user_id, b.user_id) < 0;
                 });

            map<pair<int, int>, vector<int>> conflicts; 
            for (size_t i = 0; i < all_updates.size(); ++i)
            {
                auto key = make_pair(all_updates[i].line, all_updates[i].col_start);
                conflicts[key].push_back(i);
            }

            for (const auto &conflict : conflicts)
            {
                if (conflict.second.size() > 1)
                {
                    cout << "[Conflict] Line " << conflict.first.first
                         << ", Col " << conflict.first.second << ": ";
                    for (int idx : conflict.second)
                    {
                        cout << all_updates[idx].user_id << "(ts:" << all_updates[idx].timestamp_ms << ") ";
                    }
                    int winner_idx = conflict.second.back();
                    cout << " -> Winner: " << all_updates[winner_idx].user_id << endl;
                }
            }

            broadcast_merged_updates(all_updates);

            cout << "[GlobalBroadcast] All users should now be in sync!\n"
                 << endl;
        }
    }
}
