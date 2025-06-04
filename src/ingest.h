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

#define YTINGEST_MAJOR_SEMVER 0
#define YTINGEST_MINOR_SEMVER 3
#define YTINGEST_PATCH_SEMVER 0
#define STRINGIFY(v) #v
#define TOSTRING(v) STRINGIFY(v)
#define YTINGEST_VERSION \
    TOSTRING(YTINGEST_MAJOR_SEMVER) "." TOSTRING(YTINGEST_MINOR_SEMVER) "." TOSTRING(YTINGEST_PATCH_SEMVER)

#define ISO8601_BUFFER_SIZE 26

struct YtingestOpt {
    bool lang_available;
    char *doh, *exclude, *lang, *format, *output_path;
#ifdef USE_LIBTOKENCOUNT
    char *token_count;
#endif
};

int ingest(const char *json_str, struct YtingestOpt *opt);

#endif  // INGEST_H
