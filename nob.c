#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX

#include "nob.h"

int main(int argc, char **argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);

    Cmd cmd = {0};
    Procs procs = {0};

    remove("out/ytingest");

    bool use_ltokencount = (argc > 1 && strcmp(argv[1], "-ltokencount") == 0);
    if (use_ltokencount) {
        cmd_append(&cmd, "cargo", "build", "--manifest-path", "src/tiktoken-c/Cargo.toml", "--release");
        da_append(&procs, cmd_run_async_and_reset(&cmd));
        if (!procs_wait_and_reset(&procs)) return 1;
    }

    cmd_append(&cmd, "gcc", "-O2", "-Wall", "-Wextra", "-Wformat", "-Wformat-security", "-std=c17",
               "-fstack-protector-strong", "-D_FORTIFY_SOURCE=2", "-Isrc/cJSON", "-o", "out/ytingest", "src/main.c",
               "src/ingest.c", "src/cJSON/cJSON.c", "-lcurl");
    if (use_ltokencount)
        cmd_append(&cmd, "-Isrc/tiktoken-c", "-Lsrc/tiktoken-c/target/release", "-ltiktoken_c",
                   "-Wl,-rpath=src/tiktoken-c/target/release", "-DUSE_LIBTOKENCOUNT");
    da_append(&procs, cmd_run_async_and_reset(&cmd));

    if (!procs_wait_and_reset(&procs)) return 1;

    remove("nob.old");

    return 0;
}
