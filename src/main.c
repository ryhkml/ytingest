#include <curl/curl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include "ingest.h"

static void print_help() {
    printf("\n");
    printf("Extract YouTube video, feed it to any LLM as knowledge\n");
    printf("Usage   : ytingest <options?> <url>\n");
    printf("Options :\n");
    printf("  --doh <str>                   Specify DNS over HTTPS\n");
    printf("                                Available: cloudflare, google, quad9, or use URL\n");
    printf("  -e, --exclude <str,str,...>   Specify YouTube metadata fields to exclude (comma-separated)\n");
    printf("                                Available: author, description, keywords, owner_profile_url,\n");
    printf("                                           video_url, video_thumbnail, video_length, category,\n");
    printf("                                           publish_date, view_count, transcript, and timestamp_url\n");
    printf("  --format <str>                Specify the output file format (Default: txt)\n");
    printf("                                Available: json, md, and txt\n");
    printf("  --lang <str>                  Specify language code for transcript translation (Default: en)\n");
    printf("  --lang-available              Display available transcript translation languages\n");
    printf("  -O, --output-path <str>       Specify the directory to save the output file (Default: $pwd)\n");
    printf("  --transcript-url <str>		Specify transcript url\n");
#ifdef USE_LIBTOKENCOUNT
    printf(
        "  -T, --token-count <str>       Specify Google Gemini or OpenAI model name to estimate the token count of the "
        "output\n");
#endif
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
    printf("\n");
}

enum {
    OPT_DOH = 1000,
    OPT_LANG,
    OPT_LANG_AVAILABLE,
    OPT_FORMAT,
    OPT_TRANSCRIPT_URL,
};

int main(int argc, char *argv[]) {
    struct option init_opt[] = {
        {"doh",            required_argument, NULL, OPT_DOH           },
        {"exclude",        required_argument, NULL, 'e'               },
        {"lang",           required_argument, NULL, OPT_LANG          },
        {"lang-available", no_argument,       NULL, OPT_LANG_AVAILABLE},
        {"format",         required_argument, NULL, OPT_FORMAT        },
        {"output-path",    required_argument, NULL, 'O'               },
        {"transcript-url", required_argument, NULL, OPT_TRANSCRIPT_URL},
#ifdef USE_LIBTOKENCOUNT
        {"token-count",    required_argument, NULL, 'T'               },
#endif
        {"help",           no_argument,       NULL, 'h'               },
        {"version",        no_argument,       NULL, 'v'               },
        {0,                0,                 0,    0                 }
    };

    struct YtingestOpt ytingest = {
        .lang = "en",
        .lang_available = false,
        .format = "txt",
    };

    const char *short_opt =
#ifdef USE_LIBTOKENCOUNT
        "e:O:T:hv";
#else
        "e:O:hv";
#endif

    int opt;
    while ((opt = getopt_long(argc, argv, short_opt, init_opt, NULL)) != -1) {
        switch (opt) {
            case 'e':
                ytingest.exclude = optarg;
                break;
            case 'O':
                ytingest.output_path = optarg;
                break;
#ifdef USE_LIBTOKENCOUNT
            case 'T':
                ytingest.token_count = optarg;
                break;
#endif
            case 'h':
                print_help();
                return EXIT_SUCCESS;
            case 'v':
                printf("%s\n", YTINGEST_VERSION);
                return EXIT_SUCCESS;
            case OPT_DOH:
                ytingest.doh = optarg;
                break;
            case OPT_LANG:
                ytingest.lang = optarg;
                break;
            case OPT_LANG_AVAILABLE:
                ytingest.lang_available = true;
                break;
            case OPT_FORMAT:
                ytingest.format = optarg;
                break;
            case OPT_TRANSCRIPT_URL:
                ytingest.transcript_url = optarg;
                break;
            case '?':
                printf("Invalid option. Use -h or --help to display help message\n");
                return EXIT_FAILURE;
            default:
                return EXIT_FAILURE;
        }
    }

    const char *url = NULL;
    if (optind < argc) url = argv[optind];
    if (!url) {
        printf("Invalid option. Use -h or --help to display help message\n");
        return EXIT_FAILURE;
    }

    curl_global_init(CURL_GLOBAL_ALL);

    printf("Please wait!\n");
    int res = ingest(url, &ytingest);

    curl_global_cleanup();

    return res;
}
