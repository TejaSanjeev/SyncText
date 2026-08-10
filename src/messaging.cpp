#include "messaging.h"
#include "registry.h"
#include <iostream>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <thread>
#include <chrono>

using namespace std;

string get_mq_name(const string &user_id)
{
    return "/queue_" + user_id;
}

mqd_t create_mq(const string &user_id)
{
    string mq_name = get_mq_name(user_id);
    struct mq_attr attr;
    attr.mq_flags = O_NONBLOCK; 
    attr.mq_maxmsg = MAX_MESSAGES;
    attr.mq_msgsize = MAX_MSG_SIZE;
    attr.mq_curmsgs = 0;

    mq_unlink(mq_name.c_str());

    mqd_t mq = mq_open(mq_name.c_str(), O_CREAT | O_RDWR, QUEUE_PERMISSIONS, &attr);
    if (mq == (mqd_t)-1)
    {
        perror("mq_open");
        cerr << "Failed to create message queue for " << user_id << endl;
    }
    else
    {
        cout << "Message queue created: " << mq_name << endl;
    }
    return mq;
}

void send_update(const string &target_user_id, const UpdateObject &update)
{
    string mq_name = get_mq_name(target_user_id);
    mqd_t mq = mq_open(mq_name.c_str(), O_WRONLY);
    if (mq == (mqd_t)-1)
    {
        return;
    }

    if (mq_send(mq, (const char *)&update, sizeof(UpdateObject), 0) == -1)
    {
        // perror("mq_send");
    }

    mq_close(mq);
}

void broadcast_updates(const vector<UpdateObject> &updates, const string &self_id)
{
    vector<string> users = Registry::get_all_users();
    for (const auto &update : updates)
    {
        for (const string &user_id : users)
        {
            if (user_id != self_id)
            {
                send_update(user_id, update);
            }
        }
    }
    cout << "Broadcasted " << updates.size() << " updates to " << (users.size() - 1) << " users." << endl;
}

void listen_for_updates(const string &user_id, vector<UpdateObject> &received_updates)
{
    mqd_t mq = create_mq(user_id);
    if (mq == (mqd_t)-1)
    {
        return;
    }

    char buffer[MSG_BUFFER_SIZE];
    while (true)
    {
        ssize_t bytes_read = mq_receive(mq, buffer, MSG_BUFFER_SIZE, NULL);
        if (bytes_read >= 0)
        {
            UpdateObject update;
            memcpy(&update, buffer, sizeof(UpdateObject));
            received_updates.push_back(update);

            // Print the received update details
            cout << "\n--- Received Update ---" << endl;
            cout << "From User: " << update.user_id << endl;
            cout << "Timestamp: " << update.timestamp_ms << endl;
            string op_type;
            switch (update.op)
            {
            case UpdateObject::Type::Insert:
                op_type = "Insert";
                break;
            case UpdateObject::Type::Delete:
                op_type = "Delete";
                break;
            case UpdateObject::Type::Replace:
                op_type = "Replace";
                break;
            }
            cout << "Operation: " << op_type << endl;
            cout << "Line: " << update.line << endl;
            cout << "Old Text: \"" << update.old_text << "\"" << endl;
            cout << "New Text: \"" << update.new_text << "\"" << endl;
            cout << "-----------------------\n"
                 << endl;
        }
        this_thread::sleep_for(chrono::milliseconds(100));
    }
}
