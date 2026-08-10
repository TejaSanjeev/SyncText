#pragma once
#include <cstdint>
#include <cstring>

#define MAX_TEXT_SIZE 256
#define MAX_UID_SIZE 32

struct UpdateObject
{
    enum class Type
    {
        Insert,
        Delete,
        Replace
    };
    Type op;
    int line;
    int col_start;
    int col_end;
    char old_text[MAX_TEXT_SIZE];
    char new_text[MAX_TEXT_SIZE];
    uint64_t timestamp_ms;
    char user_id[MAX_UID_SIZE];
};
