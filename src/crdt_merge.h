#pragma once
#include <vector>
#include <string>
#include <algorithm>
#include <climits>
#include <map>
#include "update.h"

namespace CRDT
{

    static inline bool a_wins_over_b(const UpdateObject &a, const UpdateObject &b)
    {
        if (a.timestamp_ms != b.timestamp_ms)
            return a.timestamp_ms > b.timestamp_ms;
        return std::strcmp(a.user_id, b.user_id) < 0;
    }

    static inline void apply_updates(std::vector<std::string> &lines, const std::vector<UpdateObject> &updates)
    {
        if (updates.empty())
            return;

        struct LineUpdate
        {
            int line;
            std::string old_content;
            std::string new_content;
            uint64_t timestamp;
            std::string user_id;
        };

        std::vector<LineUpdate> all_updates;

        for (const auto &u : updates)
        {
            LineUpdate lu;
            lu.line = u.line;
            lu.old_content = std::string(u.old_text);
            lu.new_content = std::string(u.new_text);
            lu.timestamp = u.timestamp_ms;
            lu.user_id = std::string(u.user_id);
            all_updates.push_back(lu);
        }

        std::sort(all_updates.begin(), all_updates.end(),
                  [](const LineUpdate &a, const LineUpdate &b)
                  {
                      if (a.line != b.line)
                          return a.line < b.line;
                      return a.timestamp < b.timestamp;
                  });

        std::map<int, std::pair<std::string, uint64_t>> line_states; 

        for (size_t i = 0; i < lines.size(); ++i)
        {
            line_states[i] = {lines[i], 0};
        }

        for (const auto &lu : all_updates)
        {
            int line_num = lu.line;
            auto it = line_states.find(line_num);
            bool should_apply = false;

            if (it == line_states.end())
            {
                should_apply = true;
            }
            else
            {
                if (lu.timestamp > it->second.second)
                {
                    should_apply = true;
                }
                else if (lu.timestamp == it->second.second)
                {
                    should_apply = (lu.user_id < it->second.first); 
                }
            }

            if (should_apply)
            {
                line_states[line_num] = {lu.new_content, lu.timestamp};
            }
        }

        lines.clear();

        int max_line = -1;
        for (const auto &entry : line_states)
        {
            if (entry.first > max_line)
            {
                max_line = entry.first;
            }
        }

        if (max_line < 0)
        {
            return; 
        }

        for (int i = 0; i <= max_line; ++i)
        {
            auto it = line_states.find(i);
            if (it != line_states.end())
            {
                lines.push_back(it->second.first);
            }
            else
            {
                lines.push_back(""); 
            }
        }
        while (!lines.empty() && lines.back().empty())
        {
            lines.pop_back();
        }
    }
} 
