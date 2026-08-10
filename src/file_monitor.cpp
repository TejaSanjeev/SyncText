#include "file_monitor.h"
#include "messaging.h"
#include "registry.h"
#include "global_broadcast.h"
#include <sys/stat.h>
#include <fstream>
#include <thread>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <map>
#include <cstring>

// 0 = Broadcast every update immediately.
// 1 = Buffer updates and broadcast after 5 changes.
const int BROADCAST_MODE = 1;

using namespace std;

static time_t get_mtime(const string &file)
{
    struct stat sb;
    if (stat(file.c_str(), &sb) == 0)
        return sb.st_mtime;
    return 0;
}

void ensure_file_exists(const string &filename)
{
    ifstream in(filename);
    if (!in.good())
    {
        ofstream out(filename);
        out << "Initial text\n";
    }
}

FileMonitor::FileMonitor(const string &f, const string &uid, vector<UpdateObject> *received) : m_filename(f), m_uid(uid), m_received_ptr(received)
{
    m_lines = read_lines();
    m_last_mtime = get_mtime(m_filename);
    display_document();
}

vector<string> FileMonitor::read_lines()
{
    ifstream in(m_filename);
    vector<string> lines;
    string line;
    while (getline(in, line))
        lines.push_back(line);
    return lines;
}

void FileMonitor::run()
{
    while (true)
    {
        this_thread::sleep_for(chrono::milliseconds(400));
        attempt_merge();
        time_t m = get_mtime(m_filename);
        if (m != m_last_mtime)
        {
            detect_changes();
            m_last_mtime = m;
            display_document();
        }
    }
}

void FileMonitor::find_diff(const string &old_line, const string &new_line, UpdateObject &u)
{
    int start = 0;
    while (start < old_line.length() && start < new_line.length() && old_line[start] == new_line[start])
    {
        start++;
    }

    int end_old = old_line.length();
    int end_new = new_line.length();
    while (end_old > start && end_new > start && old_line[end_old - 1] == new_line[end_new - 1])
    {
        end_old--;
        end_new--;
    }

    u.col_start = start;
    u.col_end = end_old;

    string old_text = (start >= end_old) ? "" : old_line.substr(start, end_old - start);
    string new_text = (start >= end_new) ? "" : new_line.substr(start, end_new - start);

    strncpy(u.old_text, old_text.c_str(), MAX_TEXT_SIZE);
    strncpy(u.new_text, new_text.c_str(), MAX_TEXT_SIZE);

    if (old_text.empty() && !new_text.empty())
    {
        u.op = UpdateObject::Type::Insert;
    }
    else if (!old_text.empty() && new_text.empty())
    {
        u.op = UpdateObject::Type::Delete;
    }
    else
    {
        u.op = UpdateObject::Type::Replace;
    }
}

void FileMonitor::detect_changes()
{
    vector<string> new_lines = read_lines();
    vector<UpdateObject> updates;

    size_t max_lines = max(m_lines.size(), new_lines.size());

    static uint64_t last_timestamp = 0;
    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();

    if (now <= last_timestamp)
    {
        now = last_timestamp + 1;
    }
    last_timestamp = now;

    for (size_t i = 0; i < max_lines; ++i)
    {
        string old_line = (i < m_lines.size()) ? m_lines[i] : "";
        string new_line = (i < new_lines.size()) ? new_lines[i] : "";

        if (old_line != new_line)
        {
            UpdateObject u;
            memset(&u, 0, sizeof(UpdateObject)); 

            u.line = i;
            u.col_start = 0;
            u.col_end = max(old_line.length(), new_line.length());
            u.timestamp_ms = now++; 

            strncpy(u.user_id, m_uid.c_str(), MAX_UID_SIZE - 1);
            u.user_id[MAX_UID_SIZE - 1] = '\0';

            strncpy(u.new_text, new_line.c_str(), MAX_TEXT_SIZE - 1);
            u.new_text[MAX_TEXT_SIZE - 1] = '\0';

            strncpy(u.old_text, old_line.c_str(), MAX_TEXT_SIZE - 1);
            u.old_text[MAX_TEXT_SIZE - 1] = '\0';

            if (old_line.empty() && !new_line.empty())
            {
                u.op = UpdateObject::Type::Insert;
            }
            else if (!old_line.empty() && new_line.empty())
            {
                u.op = UpdateObject::Type::Delete;
            }
            else
            {
                u.op = UpdateObject::Type::Replace;
            }

            updates.push_back(u);
        }
    }

    if (!updates.empty())
    {
        print_diff(updates);

        if (BROADCAST_MODE == 0)
        {
            broadcast_updates(updates, m_uid);
        }
        else
        {
            for (const auto &u : updates)
            {
                bool threshold_reached = GlobalBroadcast::add_to_global_buffer(u);
                if (threshold_reached)
                {
                    cout << "[FileMonitor] Global buffer threshold reached!" << endl;
                }
            }
        }
    }

    m_lines = new_lines;
}

void FileMonitor::display_document()
{
    cout << "\033[2J\033[1;1H"; 
    cout << "--- Document: " << m_filename << " ---" << endl;

    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);

    cout << "--- Last updated: " << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %X") << " ---" << endl;

    cout << "----------------------------------------" << endl;
    for (int i = 0; i < m_lines.size(); ++i)
    {
        cout << "Line " << i << ": " << m_lines[i] << endl;
    }
    cout << "----------------------------------------" << endl;
    cout << "Active users: ";
    vector<string> users = Registry::get_all_users();
    for (const auto &user : users)
    {
        cout << user << " ";
    }
    cout << "\nMonitoring for changes..." << endl;
}

void FileMonitor::print_diff(const vector<UpdateObject> &updates)
{
    cout << "\n--- Change detected by " << m_uid << " ---\n";
    for (auto &u : updates)
    {
        string op;
        if (u.op == UpdateObject::Type::Insert)
            op = "Insert";
        else if (u.op == UpdateObject::Type::Delete)
            op = "Delete";
        else
            op = "Replace";
        cout << "[" << op << "] line " << u.line
             << " old: \"" << u.old_text << "\" new: \"" << u.new_text << "\"\n";
    }
}

void FileMonitor::attempt_merge()
{
    if (!m_received_ptr)
        return;
    if (m_received_ptr->empty())
        return;

    vector<UpdateObject> updates_to_merge;

    if (BROADCAST_MODE == 0)
    {
        updates_to_merge.insert(updates_to_merge.end(), m_received_ptr->begin(), m_received_ptr->end());
        m_received_ptr->clear();
    }
    else
    {
        updates_to_merge.insert(updates_to_merge.end(), m_received_ptr->begin(), m_received_ptr->end());
        m_received_ptr->clear();
    }

    if (updates_to_merge.empty())
        return;

    time_t old_mtime = m_last_mtime;

    CRDT::apply_updates(m_lines, updates_to_merge);
    write_back_file();
    m_last_mtime = get_mtime(m_filename);

    display_document();

    cout << "\n[Merge] Applied " << updates_to_merge.size() << " updates from global broadcast." << endl;
}

void FileMonitor::write_back_file()
{
    ofstream out(m_filename, ios::trunc);
    if (!out.is_open())
    {
        cerr << "[Error] Failed to open file for writing: " << m_filename << endl;
        return;
    }

    for (size_t i = 0; i < m_lines.size(); ++i)
    {
        out << m_lines[i];
        if (i + 1 < m_lines.size())
            out << "\n";
    }

    out.flush();
    out.close();
    this_thread::sleep_for(chrono::milliseconds(50));
}
