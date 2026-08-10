#pragma once
#include "update.h"
#include <string>
#include <vector>
#include <atomic>

using namespace std;

struct GlobalBuffer
{
    static const int MAX_BUFFERED_UPDATES = 10;
    UpdateObject updates[MAX_BUFFERED_UPDATES];
    atomic<int> count;                  
    atomic<bool> broadcast_in_progress; 
};

namespace GlobalBroadcast
{
    bool init_global_buffer();
    bool add_to_global_buffer(const UpdateObject &update);
    vector<UpdateObject> get_and_clear_buffer();
    void broadcast_merged_updates(const vector<UpdateObject> &updates, const string &exclude_user = "");
    void monitor_global_buffer(const string &user_id);
}
