#include "registry.h"
#include "file_monitor.h"
#include "messaging.h"
#include "global_broadcast.h"
#include <iostream>
#include <csignal>
#include <vector>
#include <thread>

using namespace std;

string g_uid;
vector<UpdateObject> g_received_updates;

void on_exit(int)
{
    Registry::unregister_user(g_uid);
    exit(0);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        cerr << "Usage: ./editor <user_id>\n";
        return 1;
    }
    g_uid = argv[1];
    string filename = g_uid + "_doc.txt";

    if (!Registry::register_user(g_uid))
        return 1;
    signal(SIGINT, on_exit);
    if (!GlobalBroadcast::init_global_buffer()) {
        cerr << "Failed to initialize global buffer!" << endl;
        return 1;
    }

    thread global_monitor_thread(GlobalBroadcast::monitor_global_buffer, g_uid);
    global_monitor_thread.detach();

    thread listener_thread(listen_for_updates, g_uid, ref(g_received_updates));
    listener_thread.detach(); 

    ensure_file_exists(filename);

    Registry::list_users();

    FileMonitor monitor(filename, g_uid, &g_received_updates);
    monitor.run(); 
}
