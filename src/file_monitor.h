#pragma once
#include <string>
#include <vector>
#include "update.h"
#include "messaging.h"
#include "crdt_merge.h"
#include <iostream>
#include <fstream>
#include <chrono>
using namespace std;

void ensure_file_exists(const string &filename);

class FileMonitor
{
    string m_filename;
    string m_uid;
    vector<string> m_lines;
    vector<UpdateObject> m_update_buffer;
    vector<UpdateObject> *m_received_ptr; 
    time_t m_last_mtime;

public:
    FileMonitor(const string &filename, const string &uid, vector<UpdateObject> *received);
    void run();

private:
    void detect_changes();
    void display_document();
    std::vector<std::string> read_lines();
    void print_diff(const std::vector<UpdateObject> &updates);
    void find_diff(const std::string &old_line, const std::string &new_line, UpdateObject &u);
    void attempt_merge();
    void write_back_file();
};
