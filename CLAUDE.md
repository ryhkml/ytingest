# Ytingest

A C CLI tool that extracts YouTube video metadata and transcripts into LLM-friendly formats (txt, json, md). Optionally estimates token counts using tiktoken (OpenAI) or the Gemini API.

## Build

The project uses [nob](https://github.com/tsoding/nob.h) (a C-based build system — single header `nob.h`, build script `nob.c`).

```sh
# Bootstrap the build tool (first time only)
gcc -o nob nob.c

# Standard build
./nob

# Build with token counting support (requires cargo for tiktoken-rs)
./nob -ltokencount
```

The binary is output to `out/ytingest`.

If `nob.c` is modified, `./nob` auto-rebuilds itself on next run (`NOB_GO_REBUILD_URSELF`).

## Compiler flags

gcc with `-O2 -Wall -Wextra -Wformat -Wformat-security -std=c17 -fstack-protector-strong -D_FORTIFY_SOURCE=2`. When building with `-ltokencount`, the `USE_LIBTOKENCOUNT` preprocessor macro is defined.

## Dependencies

- **libcurl** — HTTP requests (linked via `-lcurl`)
- **cJSON** — JSON parsing (vendored in `src/cJSON/`, compiled as `src/cJSON/cJSON.c`)
- **tiktoken-c** — OpenAI token counting (vendored in `src/tiktoken-c/`, Rust crate built via cargo, optional)
- **POSIX regex** (`<regex.h>`) — extracting `ytInitialPlayerResponse` from YouTube HTML

## Architecture

Single-purpose CLI with two source files:

- `src/main.c` — Argument parsing via `getopt_long`, initializes libcurl, calls `ingest()`
- `src/ingest.c` — All core logic:
  - `fetch()` — HTTP requests via libcurl with DNS-over-HTTPS support
  - `extract()` — Regex extraction of `ytInitialPlayerResponse` JSON from raw YouTube HTML
  - `parse_transcript()` — Parses transcript JSON events into timestamped text lines
  - `write_yt()` — Serializes `Ytingest` struct to the chosen output format (txt/json/md)
  - `token_count()` — Token estimation via tiktoken (OpenAI models) or Gemini API (conditional on `USE_LIBTOKENCOUNT`)
- `src/ingest.h` — Shared types (`YtingestOpt`, version macros, ANSI color macros)

## Key patterns

- The `isinclude()` function returns `true` when a field should be **included** (i.e., it's NOT in the exclude list). The name is counterintuitive — it checks exclusion.
- Transcript fetching requires a `--transcript-url` obtained manually from browser DevTools (YouTube now requires proof-of-access for transcript endpoints).
- Output filenames follow the pattern `yt_{lang}_{videoId}.{format}`.
- Memory management: `mstrdup()` is used instead of POSIX `strdup()`. Manual `malloc`/`free` throughout — no arena or pool allocator.
