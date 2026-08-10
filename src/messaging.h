#pragma once
#include "update.h"
#include <mqueue.h>
#include <string>
#include <vector>
#include <thread>

using namespace std;

#define QUEUE_PERMISSIONS 0660
#define MAX_MESSAGES 10
#define MAX_MSG_SIZE sizeof(UpdateObject)
#define MSG_BUFFER_SIZE (MAX_MSG_SIZE + 10)

string get_mq_name(const string &user_id);
mqd_t create_mq(const string &user_id);
void send_update(const string &target_user_id, const UpdateObject &update);
void broadcast_updates(const vector<UpdateObject> &updates, const string &self_id);
void listen_for_updates(const string &user_id, vector<UpdateObject> &received_updates);
