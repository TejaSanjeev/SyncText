#pragma once
#include <vector>
#include <string>
#include <algorithm>
#include <climits>
#include <map>
#include <cstring>
#include "update.h"

namespace CRDT
{
    // Conflict resolution priority: latest timestamp wins; tiebreak with smaller user_id
    static inline bool a_wins_over_b(const UpdateObject &a, const UpdateObject &b)
    {
        if (a.timestamp_ms != b.timestamp_ms)
            return a.timestamp_ms > b.timestamp_ms;
        return std::strcmp(a.user_id, b.user_id) < 0;
    }

    // Two updates conflict if they have overlapping column ranges
    static inline bool has_overlap(const UpdateObject &a, const UpdateObject &b)
    {
        if (a.col_start == a.col_end && b.col_start == b.col_end)
            return a.col_start == b.col_start;
        return std::max(a.col_start, b.col_start) < std::min(a.col_end, b.col_end);
    }

    static inline void apply_updates(std::vector<std::string> &lines, const std::vector<UpdateObject> &updates)
    {
        if (updates.empty())
            return;

        // 1. Group updates by line number
        std::map<int, std::vector<UpdateObject>> line_updates;
        for (const auto &u : updates)
        {
            line_updates[u.line].push_back(u);
        }

        // 2. Process updates line by line
        for (auto &pair : line_updates)
        {
            int line_num = pair.first;
            auto &u_list = pair.second;

            // Make sure the lines vector is large enough
            if (line_num >= (int)lines.size())
            {
                lines.resize(line_num + 1, "");
            }

            // Sort updates for this line by LWW priority (highest priority first)
            std::sort(u_list.begin(), u_list.end(), a_wins_over_b);

            // Filter out updates that conflict with an already accepted higher-priority update
            std::vector<UpdateObject> winning_updates;
            for (const auto &cand : u_list)
            {
                bool conflict = false;
                for (const auto &winner : winning_updates)
                {
                    if (has_overlap(cand, winner))
                    {
                        conflict = true;
                        break;
                    }
                }
                if (!conflict)
                {
                    winning_updates.push_back(cand);
                }
            }

            // Sort winning updates from right to left (highest col_start first)
            // so that modifying text at a later column doesn't shift earlier column indices
            std::sort(winning_updates.begin(), winning_updates.end(),
                      [](const UpdateObject &a, const UpdateObject &b)
                      {
                          return a.col_start > b.col_start;
                      });

            // Apply each winning column edit to the line
            std::string &cur_line = lines[line_num];
            for (const auto &u : winning_updates)
            {
                int c_start = u.col_start;
                int c_end = u.col_end;

                if (c_start > (int)cur_line.length())
                {
                    cur_line.append(c_start - cur_line.length(), ' ');
                }

                c_start = std::min(c_start, (int)cur_line.length());
                c_end = std::min(c_end, (int)cur_line.length());
                int replace_len = std::max(0, c_end - c_start);

                cur_line.replace(c_start, replace_len, u.new_text);
            }
        }

        // 3. Trim trailing empty lines
        while (!lines.empty() && lines.back().empty())
        {
            lines.pop_back();
        }
    }
} 
