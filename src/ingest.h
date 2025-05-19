#ifndef INGEST_H
#define INGEST_H

#include <stdbool.h>

#define ANSI_INFO "\033[1;32m"
#define ANSI_WARN "\033[1;33m"
#define ANSI_ERROR "\033[1;31m"
#define ANSI_RESET "\033[0m"

#define NOOP \
    do {     \
    } while (0)

struct YtingestOpt {
    bool lang_available;
    char *exclude, *lang, *format, *output_path, *token_count;
};

int ingest(const char *json_str, struct YtingestOpt *opt);

#endif  // INGEST_H
