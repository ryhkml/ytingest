#include <curl/curl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include "ingest.h"

static void print_help() {
    printf("---\n");
    printf("Extract YouTube video, feed it to any LLM as knowledge\n");
    printf("Usage   : ytingest <options?> <url>\n");
    printf("Options :\n");
    printf("  -e, --exclude <str,str,...>   Specify YouTube metadata fields to exclude (comma-separated)\n");
    printf("                                Available: author, description, keywords, owner_profile_url,\n");
    printf("                                           video_url, video_thumbnail, video_length, category,\n");
    printf("                                           publish_date, view_count, and transcript\n");
    printf("  --format <str>                Specify the output file format (Default: txt)\n");
    printf("                                Available: json, md, and txt\n");
    printf("  --lang <str>                  Specify language code for transcript translation (Default: en)\n");
    printf("  --lang-available              Display available transcript translation languages\n");
    printf("  -O, --output-path <str>       Specify the directory to save the output file (Default: $pwd)\n");
    printf(
        "  -T, --token-count <str>       Specify Google Gemini or OpenAI model name to estimate the token count of the "
        "output\n");
    printf("\n");
    printf("  -h, --help                    Display help message and exit\n");
    printf("  -v, --version                 Display version and exit\n");
    printf("\n");
    printf("Example :\n");
    printf(" 1. out/ytingest \"https://www.youtube.com/watch?v=OeYnV9zp7Dk\"\n");
    printf(" 2. Using a shareable URL, but takes some time due to the redirection process\n");
    printf("    out/ytingest \"https://youtu.be/OeYnV9zp7Dk?si=_rCGAoV59TIR5L1r\"\n");
    printf(" 3. Or using YouTube Shorts\n");
    printf("    out/ytingest \"https://www.youtube.com/shorts/olm7ie2YUhY\"\n");
    printf("---\n");
}

int main(int argc, char *argv[]) {
    struct option init_opt[] = {
        {"excludes",       required_argument, NULL, 'e'},
        {"lang",           required_argument, NULL, 0  },
        {"lang-available", no_argument,       NULL, 0  },
        {"format",         required_argument, NULL, 0  },
        {"output-path",    required_argument, NULL, 'O'},
        {"token-count",    required_argument, NULL, 'T'},
        {"help",           no_argument,       NULL, 'h'},
        {"version",        no_argument,       NULL, 'v'},
        {0,                0,                 0,    0  }
    };

    struct YtingestOpt ytingest = {
        .lang = "en",
        .lang_available = false,
        .format = "txt",
    };

    int opt;
    int opt_index = 0;
    while ((opt = getopt_long(argc, argv, "e:O:T:hv", init_opt, &opt_index)) != -1) {
        switch (opt) {
            case 'e':
                ytingest.exclude = optarg;
                break;
            case 'O':
                ytingest.output_path = optarg;
                break;
            case 'T':
                ytingest.token_count = optarg;
                break;
            case 'h':
                print_help();
                return EXIT_SUCCESS;
                break;
            case 'v':
                printf("0.1.2\n");
                return EXIT_SUCCESS;
            case 0:
                if (opt_index == 1) ytingest.lang = optarg;
                if (opt_index == 2) ytingest.lang_available = true;
                if (opt_index == 3) ytingest.format = optarg;
                break;
            case '?':
                printf("Invalid option. Use -h or --help to display help meesage\n");
                return EXIT_FAILURE;
            default:
                printf("Error parsing options\n");
                return EXIT_FAILURE;
        }
    }

    const char *url = NULL;
    if (optind < argc) url = argv[optind];
    if (!url) {
        printf("Invalid option. Use -h or --help to display help meesage\n");
        return EXIT_FAILURE;
    }

    curl_global_init(CURL_GLOBAL_ALL);

    printf("Please wait!\n");
    int res = ingest(url, &ytingest);

    curl_global_cleanup();

    return res;
}
