#pragma once
#include <string>
#include <vector>
using namespace std;

namespace Registry
{
    bool register_user(const string &user_id);
    void unregister_user(const string &user_id);
    void list_users();
    vector<string> get_all_users();
}
