#include "ingest.h"

#include <ctype.h>
#include <curl/curl.h>
#include <regex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON/cJSON.h"
#ifdef USE_LIBTOKENCOUNT
#include "tiktoken-c/tiktoken.h"
#endif

typedef enum {
    RF_PLAINTEXT,
    RF_JSON,
} RawFormat;

typedef struct {
    char *response;
    size_t size;
} Memory;

typedef struct {
    const char **items;
    size_t size;
} Keyword;

typedef struct {
    bool video_live;
    char *title, *author, *description, *video_id, *video_url, *video_thumbnail, *video_length, *owner_profile_url,
        *category, *ingest_date, *publish_date, *transcript;
    Keyword keyword;
    unsigned long long view_count;
} Ytingest;

/**
 * Finds the first occurrence of a substring within a string and ignoring case.
 *
 * `mstrcasestr()` locates the first occurrence of the null-terminated string `needle`
 * within the null-terminated string `haystack`. Unlike the standard `strstr` function,
 * the comparison is performed case-insensitively.
 */
static char *mstrcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return (char *)haystack;

    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && (tolower((unsigned char)*h) == tolower((unsigned char)*n))) {
            h++;
            n++;
        }
        if (!*n) return (char *)haystack;
        haystack++;
    }

    return NULL;
}

static char *mstrdup(const char *value) {
    if (!value) return NULL;
    size_t size = strlen(value);
    void *new_value = malloc(size + 1);
    if (!new_value) {
        printf("Failed to allocate memory - %d\n", __LINE__);
        return NULL;
    }
    return (char *)memcpy(new_value, value, size + 1);
}

/**
 * Checks if a pattern string is present within a comma-separated list of exclusion strings.
 *
 * `isinclude()` takes a string `excludes` which contains zero or more tokens
 * separated by commas (e.g., "item1,item2,another_item"). It iterates through
 * each token in the `excludes` list and compares it against the provided `pattern`
 * string.
 *
 * The comparison is case-sensitive and requires an exact match between the
 * `pattern` and a token from the `excludes` list. If an exact match is found,
 * it indicates the pattern *is* in the exclusion list.
 */
static bool isinclude(const char *exclude, const char *pattern) {
    if (!exclude || !pattern) return true;

    const char *start = exclude;
    const char *end = NULL;
    size_t pattern_len = strlen(pattern);

    while (*start != '\0') {
        end = start;
        while (*end != ',' && *end != '\0') end++;
        size_t token_len = end - start;
        if (token_len == pattern_len && strncmp(start, pattern, token_len) == 0) return false;
        if (*end == '\0') break;
        start = end + 1;
    }

    return true;
}

static bool isempty(const char *value) {
    if (!value) return true;
    while (*value != '\0') {
        if (!isspace((unsigned char)*value)) return false;
        value++;
    }
    return true;
}

/**
 * Callback function to write response data into memory.
 *
 * For more context, see https://curl.se/libcurl/c/CURLOPT_WRITEFUNCTION.html
 */
static size_t write_cb(void *contents, size_t size, size_t nmemb, void *clientp) {
    size_t realsize = size * nmemb;
    Memory *mem = (Memory *)clientp;

    char *ptr = realloc(mem->response, mem->size + realsize + 1);
    if (!ptr) return 0;

    mem->response = ptr;
    memcpy(&(mem->response[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->response[mem->size] = 0;

    return realsize;
}

static char *fetch(const char *url, const RawFormat raw_format, const char *body, const char *doh) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Failed to initialize curl\n");
        return NULL;
    }

    Memory chunk = {0};
    CURLcode res;

    struct curl_slist *headers = NULL;

    if (raw_format == RF_JSON) {
        headers = curl_slist_append(headers, "Accept: application/json");
        headers = curl_slist_append(headers, "Cache-Control: no-cache, no-store, must-revalidate");
        headers = curl_slist_append(headers, "Content-Type: application/json");
    } else {
        headers = curl_slist_append(
            headers,
            "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8");
        headers = curl_slist_append(headers, "Accept-Language: en;q=0.8");
        headers = curl_slist_append(headers, "Cache-Control: no-cache, no-store, must-revalidate");
        headers = curl_slist_append(headers, "Sec-Fetch-Dest: document");
        headers = curl_slist_append(headers, "Sec-Fetch-Mode: navigate");
        headers = curl_slist_append(headers, "Sec-Fetch-Site: same-origin");
        headers = curl_slist_append(headers, "Priority: u=0, i");
        headers = curl_slist_append(headers, "Upgrade-Insecure-Requests: 1");
    }

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (raw_format == RF_PLAINTEXT)
        curl_easy_setopt(
            curl, CURLOPT_USERAGENT,
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/136.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);

    // NOTE!
    // Use DNS over HTTPS if transcripts cannot be obtained. This may be due to inconsistent responses.
    // However, there is a trade-off as DoH requests may take slightly longer compared to requests without DoH.
    if (doh) {
        if (strcmp(doh, "cloudflare") == 0) {
            curl_easy_setopt(curl, CURLOPT_DOH_URL, "https://cloudflare-dns.com/dns-query");
        }
        if (strcmp(doh, "google") == 0) {
            curl_easy_setopt(curl, CURLOPT_DOH_URL, "https://dns.google/dns-query");
        }
        if (strcmp(doh, "quad9") == 0) {
            curl_easy_setopt(curl, CURLOPT_DOH_URL, "https://dns.quad9.net/dns-query");
        }
        if (mstrcasestr(doh, "https://")) {
            // Or custom DoH URL
            curl_easy_setopt(curl, CURLOPT_DOH_URL, doh);
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "%sERROR:%s %s\n", ANSI_ERROR, ANSI_RESET, curl_easy_strerror(res));
        free(chunk.response);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return NULL;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return chunk.response;
}

static char *extract(const char *raw_content) {
    regex_t regex;
    regmatch_t matches[2];

    if (regcomp(&regex, "var ytInitialPlayerResponse = (\\{.*?\\});", REG_EXTENDED)) return NULL;
    if (regexec(&regex, raw_content, 2, matches, 0) == 0) {
        size_t player_size = matches[1].rm_eo - matches[1].rm_so;
        char *player_buff = malloc(player_size + 1);
        if (!player_buff) {
            printf("Failed to allocate memory - %d\n", __LINE__);
            return NULL;
        }
        snprintf(player_buff, player_size + 1, "%s", raw_content + matches[1].rm_so);
        regfree(&regex);
        return player_buff;
    }

    regfree(&regex);
    return NULL;
}

/**
 * Normalize characters newlines to \n and escapes double quote characters
 * to \" within a given string.
 *
 * `normalize()` takes a constant input string and produces a new string
 * where specific characters are escaped for safe inclusion in formats like
 * JSON.
 */
static char *normalize(const char *text) {
    if (!text) return NULL;

    size_t output_size = 0;
    const char *cursor = text;
    while (*cursor) {
        if (*cursor == '"') {
            // \"
            output_size += 2;
        } else {
            output_size += 1;
        }
        cursor++;
    }

    output_size++;
    char *output_buff = malloc(output_size);
    if (!output_buff) {
        printf("Failed to allocate memory - %d\n", __LINE__);
        return NULL;
    }

    const char *src = text;
    char *dst = output_buff;
    while (*src) {
        if (*src == '"') {
            *dst++ = '\"';
        } else {
            *dst++ = *src;
        }
        src++;
    }

    *dst = '\0';
    return output_buff;
}

static void rtrim(char *text) {
    if (!text) return;

    char *end = text + strlen(text) - 1;
    while (end >= text && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
}

static void trim(char *text) {
    if (!text) return;

    char *start = text;
    while (isspace((unsigned char)*start)) start++;
    if (*start == '\0') {
        text[0] = '\0';
        return;
    }

    memmove(text, start, strlen(start) + 1);
    rtrim(text);
}

/**
 * WARNING: `tohttps()` strictly assumes the input URL always begins with http://.
 * It does not handle https://, relative URLs, or URLs without a scheme.
 * This assumption is based on the current format of YouTube response
 * for the relevant field. If the response changes, this may fail💀
 */
static char *tohttps(const char *url) {
    if (!url) return NULL;
    // 7 length of http://
    // 8 length of https://
    size_t url_size = strlen(url) - 7 + 8;
    char *url_buff = malloc(url_size + 1);
    if (!url_buff) {
        printf("Failed to allocate memory - %d\n", __LINE__);
        return NULL;
    }
    snprintf(url_buff, url_size + 1, "https://%s", url + 7);
    return url_buff;
}

static void tolowercase(char *text) {
    if (!text) return;
    for (; *text; text++) *text = tolower((unsigned char)*text);
}

static char *ingest_date(char *buff, size_t size) {
    time_t now = time(NULL);
    if (!now) return NULL;
    struct tm *timeinfo = localtime(&now);
    size_t len_written = strftime(buff, size, "%Y-%m-%dT%H:%M:%S", timeinfo);
    if (len_written == 0) return NULL;
    // For +HHMM\0 or -HHMM\0
    char tz_offset_str[6];
    strftime(tz_offset_str, sizeof(tz_offset_str), "%z", timeinfo);
    // The characters to be added 1 (sign) + 2 (HH) + 1 (:) + 2 (MM) = 6
    // The sign (+ or -)
    buff[len_written++] = tz_offset_str[0];
    // One-digit hour
    buff[len_written++] = tz_offset_str[1];
    // Two-digit hour
    buff[len_written++] = tz_offset_str[2];
    buff[len_written++] = ':';
    // One-digit minute
    buff[len_written++] = tz_offset_str[3];
    // Two-digit minute
    buff[len_written++] = tz_offset_str[4];
    buff[len_written] = '\0';

    return buff;
}

static char *time_yt(uint16_t video_len, char *buff, size_t size) {
    if (video_len == 0) {
        snprintf(buff, size, "0:00");
    } else if (video_len < 3600) {
        int minutes = video_len / 60;
        int seconds = video_len % 60;
        snprintf(buff, size, "%d:%02d", minutes, seconds);
    } else {
        int hours = video_len / 3600;
        int minutes = (video_len / 60) % 60;
        int seconds = video_len % 60;
        snprintf(buff, size, "%d:%02d:%02d", hours, minutes, seconds);
    }
    return buff;
}

static char *join(const cJSON *root) {
    if (!root) return NULL;
    size_t total_calculated_len = 0;
    int lines_to_render_count = 0;
    int outer_arr_size = cJSON_GetArraySize(root);

    // Phase 1, calculate total length needed
    for (int i = 0; i < outer_arr_size; i++) {
        cJSON *inner_arr = cJSON_GetArrayItem(root, i);
        int inner_items_size = cJSON_GetArraySize(inner_arr);
        for (int j = 0; j < inner_items_size; j++) {
            cJSON *item = cJSON_GetArrayItem(inner_arr, j);
            total_calculated_len += strlen(item->valuestring);
            lines_to_render_count++;
        }
    }

    if (lines_to_render_count == 0) return NULL;
    if (lines_to_render_count > 1) total_calculated_len += (lines_to_render_count - 1);

    char *result_buff = malloc(total_calculated_len + 1);
    if (!result_buff) return NULL;
    char *current_pos = result_buff;

    // Phase 2, construct the final string
    int parts_rendered = 0;
    for (int i = 0; i < outer_arr_size; i++) {
        cJSON *inner_arr = cJSON_GetArrayItem(root, i);
        int inner_items_size = cJSON_GetArraySize(inner_arr);
        for (int j = 0; j < inner_items_size; j++) {
            cJSON *item = cJSON_GetArrayItem(inner_arr, j);
            size_t part_len = strlen(item->valuestring);
            if (part_len > 0) {
                memcpy(current_pos, item->valuestring, part_len);
                current_pos += part_len;
            }
            parts_rendered++;
        }
    }

    *current_pos = '\0';
    return result_buff;
}

static void replace_newline(char *text) {
    if (!text) return;
    for (int i = 0; text[i] != '\0'; i++) {
        if (text[i] == '\n' || text[i] == '\r') text[i] = ' ';
    }
}

static char *parse_transcript(char *json_str, const Ytingest *yt, const struct YtingestOpt *opt) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        printf("%sERROR:%s Invalid language code\n", ANSI_ERROR, ANSI_RESET);
        return mstrdup("INVALID_LANG");
    }

    cJSON *events = cJSON_GetObjectItem(root, "events");
    if (!cJSON_IsArray(events)) {
        printf("%sERROR:%s Failed to get root event\n", ANSI_ERROR, ANSI_RESET);
        cJSON_Delete(root);
        return NULL;
    }

    const bool ismd = strcmp(opt->format, "md") == 0;

    bool invalid_text = false;
    char *result = NULL;
    int index = 0;

    cJSON *new_arr = cJSON_CreateArray();
    cJSON *segment = NULL;
    cJSON_ArrayForEach(segment, events) {
        cJSON *segments = cJSON_GetObjectItem(segment, "segs");
        int segment_size = cJSON_GetArraySize(segments);
        if (segment_size == 1) {
            cJSON *first = cJSON_GetArrayItem(segments, 0);
            cJSON *text = cJSON_GetObjectItem(first, "utf8");
            if (strcmp(text->valuestring, "\n") == 0) {
                index++;
                continue;
            }
        }

        cJSON *tstartms = cJSON_GetObjectItem(segment, "tStartMs");
        uint16_t seconds = (uint16_t)(tstartms->valuedouble / 1000.0);
        char time_tmp[9];
        char *time = time_yt(seconds, time_tmp, sizeof(time_tmp));

        cJSON *new_sec_arr = cJSON_CreateArray();

        if (ismd) {
            cJSON *prefix = cJSON_CreateString("-");
            cJSON_AddItemToArray(new_sec_arr, prefix);
        }

        const char *timestamp_fmt = NULL;
        char *timestamp_buff;

        // NOTE!
        // Some YouTube videos may use a different transcript format.
        // English transcripts typically start at 0 seconds, with no further transcript available.
        // The effect of this format on all YouTube videos is unclear.
        if (isinclude(opt->exclude, "timestamp_url") && mstrcasestr(yt->video_url, "https://www.youtube.com/watch")) {
            timestamp_fmt = "%s[[%s]](https://www.youtube.com/watch?v=%s&t=%ds) ";
            size_t timestamp_size = snprintf(NULL, 0, timestamp_fmt, ismd ? " " : "", time, yt->video_id, seconds);
            timestamp_buff = malloc(timestamp_size + 1);
            if (!timestamp_buff) {
                printf("Failed to allocate memory - %d\n", __LINE__);
                goto done;
            }
            snprintf(timestamp_buff, timestamp_size + 1, timestamp_fmt, ismd ? " " : "", time, yt->video_id, seconds);
        } else {
            timestamp_fmt = "%s[%s] ";
            size_t timestamp_size = snprintf(NULL, 0, timestamp_fmt, ismd ? " " : "", time);
            timestamp_buff = malloc(timestamp_size + 1);
            if (!timestamp_buff) {
                printf("Failed to allocate memory - %d\n", __LINE__);
                goto done;
            }
            snprintf(timestamp_buff, timestamp_size + 1, timestamp_fmt, ismd ? " " : "", time);
        }

        cJSON *timestamp = cJSON_CreateString(timestamp_buff);
        cJSON_AddItemToArray(new_sec_arr, timestamp);

        free(timestamp_buff);

        cJSON *utf8 = NULL;
        cJSON_ArrayForEach(utf8, segments) {
            cJSON *text = cJSON_GetObjectItem(utf8, "utf8");
            replace_newline(text->valuestring);
            if (strlen(text->valuestring) == 1 || isempty(text->valuestring)) {
                invalid_text = true;
            } else {
                cJSON *text_str = cJSON_CreateString(text->valuestring);
                cJSON_AddItemToArray(new_sec_arr, text_str);
            }
        }

        if (invalid_text) {
            cJSON_DeleteItemFromArray(new_sec_arr, index);
            invalid_text = false;
        } else {
            cJSON *newline = cJSON_CreateString("\n");
            cJSON_AddItemToArray(new_sec_arr, newline);
            cJSON_AddItemToArray(new_arr, new_sec_arr);
        }

        index++;
    }

    result = join(new_arr);
    rtrim(result);

done:
    cJSON_Delete(new_arr);
    cJSON_Delete(root);
    return result;
}

/**
 * Calculates the approximate token count using a tiktoken via libtiktoken-c and
 * via Google Gemini API.
 *
 * `token_count()` utilizes the libtiktoken-c library to determine the token count
 * for a given text buffer (assumed to be read from a file) based on the
 * specified model name.
 *
 * For Google Gemini model, you need an API KEY. You can find it at https://aistudio.google.com
 *
 * @param file Pointer to a FILE stream. The content of this file will be used
 * as input for tiktoken.
 *
 * @param model A null-terminated C string representing the model name.
 *
 * @see For more context on the different OpenAI models, see
 * https://github.com/openai/openai-cookbook/blob/main/examples/How_to_count_tokens_with_tiktoken.ipynb
 *
 * @see For more context on the different Google Gemini models, see
 * https://ai.google.dev/gemini-api/docs/tokens
 */
#ifdef USE_LIBTOKENCOUNT
static void token_count(FILE *file, const char *model) {
    if (!model) return;

    rewind(file);
    fseek(file, 0, SEEK_END);
    long file_buff_len = ftell(file);
    rewind(file);

    char *file_buff = malloc(file_buff_len + 1);
    if (!file_buff) {
        printf("Failed to allocate memory\n");
        return;
    }

    size_t bytes_read = fread(file_buff, 1, file_buff_len, file);
    if (bytes_read > 0) {
        file_buff[bytes_read] = '\0';
    } else {
        printf("%sERROR:%s Failed to read file\n", ANSI_ERROR, ANSI_RESET);
        goto done;
    }

    if (mstrcasestr(model, "gemini-")) {
        const char *GEMINI_API_KEY = getenv("GEMINI_API_KEY");
        if (!GEMINI_API_KEY) {
            printf("%sWARNING:%s GEMINI API KEY is empty\n", ANSI_WARN, ANSI_RESET);
            goto done;
        };

        const char *payload_fmt = "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"%s\"}]}]}";
        size_t payload_size = snprintf(NULL, 0, payload_fmt, file_buff);
        char *payload_buff = malloc(payload_size + 1);
        if (!payload_buff) goto done;
        snprintf(payload_buff, payload_size + 1, payload_fmt, file_buff);

        const char *endpoint_fmt = "https://generativelanguage.googleapis.com/v1beta/models/%s:countTokens?key=%s";
        size_t endpoint_size = snprintf(NULL, 0, endpoint_fmt, model, GEMINI_API_KEY);
        char *endpoint_buff = malloc(endpoint_size + 1);
        if (!endpoint_buff) {
            free(payload_buff);
            goto done;
        }
        snprintf(endpoint_buff, endpoint_size + 1, endpoint_fmt, model, GEMINI_API_KEY);

        char *res = fetch(endpoint_buff, RF_JSON, payload_buff, NULL);
        if (!res) {
            free(endpoint_buff);
            free(payload_buff);
            goto done;
        }

        cJSON *root = cJSON_Parse(res);
        if (!root) {
            free(res);
            free(endpoint_buff);
            free(payload_buff);
            printf("%sERROR:%s Failed to parse JSON\n", ANSI_ERROR, ANSI_RESET);
            goto done;
        }
        cJSON *total_tokens = cJSON_GetObjectItem(root, "totalTokens");
        if (cJSON_IsNumber(total_tokens) && total_tokens->valuedouble > 0) {
            printf("%sOK:%s Approximately %zu tokens for the %s model\n", ANSI_INFO, ANSI_RESET,
                   (size_t)total_tokens->valuedouble, model);
        } else {
            printf("%sERROR:%s No encoding for model %s\n", ANSI_ERROR, ANSI_RESET, model);
        }

        cJSON_Delete(root);
        free(res);
        free(endpoint_buff);
        free(payload_buff);
        goto done;
    }

    size_t token = 0;
    /**
     * Get the Core BPE tokenizer configuration specific to the provided language model.
     * This object contains the rules needed for tokenization.
     *
     * Reference BPE: https://en.wikipedia.org/wiki/Byte_pair_encoding
     */
    CoreBPE *bpe = tiktoken_get_bpe_from_model(model);
    /**
     * Encode the text content stored in `file_buff` into a sequence of token IDs.
     * This function uses the previously obtained BPE configuration and ensures
     * hat any special tokens (like <|endoftext|>, etc.) present in the text
     * are handled according to the model's definition.
     */
    tiktoken_corebpe_encode_with_special_tokens(bpe, file_buff, &token);
    if (token > 0) {
        printf("%sOK:%s Approximately %zu tokens for the %s model\n", ANSI_INFO, ANSI_RESET, token, model);
    } else {
        printf("%sERROR:%s No encoding for model %s\n", ANSI_ERROR, ANSI_RESET, model);
    }

    tiktoken_destroy_corebpe(bpe);

done:
    free(file_buff);
}
#endif

static void write_yt(FILE *file, const Ytingest *yt, const char *format) {
    if (strcmp(format, "json") == 0) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "id", yt->video_id);
        cJSON_AddStringToObject(root, "title", yt->title);
        if (yt->author) cJSON_AddStringToObject(root, "author", yt->author);
        if (yt->description) cJSON_AddStringToObject(root, "description", yt->description);
        if (yt->keyword.items) {
            cJSON *keywords = cJSON_AddArrayToObject(root, "keywords");
            cJSON *keyword;
            for (size_t i = 0; i < yt->keyword.size; i++) {
                cJSON_AddItemToArray(keywords, keyword = cJSON_CreateString(yt->keyword.items[i]));
            }
        }
        if (yt->owner_profile_url) cJSON_AddStringToObject(root, "owner_profile_url", yt->owner_profile_url);
        if (yt->video_url) cJSON_AddStringToObject(root, "video_url", yt->video_url);
        if (yt->video_thumbnail) cJSON_AddStringToObject(root, "video_thumbnail", yt->video_thumbnail);
        if (yt->video_length) cJSON_AddStringToObject(root, "video_length", yt->video_length);
        if (yt->category) cJSON_AddStringToObject(root, "category", yt->category);
        if (yt->publish_date) cJSON_AddStringToObject(root, "publish_date", yt->publish_date);
        cJSON_AddStringToObject(root, "ingest_date", yt->ingest_date);
        if (yt->view_count) cJSON_AddNumberToObject(root, "view_count", yt->view_count);
        if (yt->transcript) cJSON_AddStringToObject(root, "transcript", yt->transcript);
        char *result = cJSON_Print(root);
        fprintf(file, "%s", result);
        free(result);
        cJSON_Delete(root);
    } else if (strcmp(format, "md") == 0) {
        fprintf(file, "# [%s](%s)", yt->title, yt->video_url);
        if (yt->video_thumbnail) fprintf(file, "\n![Thumbnail](%s)", yt->video_thumbnail);
        if (yt->author) fprintf(file, "\n\n**Author:** %s", yt->author);
        if (yt->owner_profile_url) fprintf(file, "\n\n**Owner Profile URL:** %s", yt->owner_profile_url);
        if (yt->category) fprintf(file, "\n\n**Category:** %s", yt->category);
        if (yt->publish_date) fprintf(file, "\n\n**Publish Date:** %s", yt->publish_date);
        fprintf(file, "\n\n**Ingest Date:** %s", yt->ingest_date);
        if (yt->video_length) fprintf(file, "\n\n**Video Length:** %s", yt->video_length);
        if (yt->view_count) fprintf(file, "\n\n**View Count:** %llu", yt->view_count);
        if (yt->description) {
            fprintf(file, "\n\n## Description");
            fprintf(file, "\n```");
            fprintf(file, "\n%s", yt->description);
            fprintf(file, "\n```");
        }
        if (yt->keyword.items) {
            fprintf(file, "\n\n## Keywords");
            for (size_t i = 0; i < yt->keyword.size; i++) {
                fprintf(file, "\n- %s", yt->keyword.items[i]);
            }
        }
        if (yt->transcript) {
            fprintf(file, "\n\n## Transcript");
            fprintf(file, "\n%s", yt->transcript);
        }
        fprintf(file, "\n");
        fprintf(file, "\n---");
        fprintf(file, "\n\n#### ID");
        fprintf(file, "\n`%s`", yt->video_id);
    } else {
        fprintf(file, "ID: %s", yt->video_id);
        fprintf(file, "\n\nTitle: %s", yt->title);
        if (yt->author) fprintf(file, "\n\nAuthor: %s", yt->author);
        if (yt->description) {
            fprintf(file, "\n\nDescription:");
            fprintf(file, "\n\"\"\"");
            fprintf(file, "\n%s", yt->description);
            fprintf(file, "\n\"\"\"");
        }
        if (yt->keyword.items) {
            fprintf(file, "\n\nKeywords:");
            for (size_t i = 0; i < yt->keyword.size; i++) {
                fprintf(file, "\n- %s", yt->keyword.items[i]);
            }
        }
        if (yt->owner_profile_url) fprintf(file, "\n\nOwner Profile URL: %s", yt->owner_profile_url);
        if (yt->video_url) fprintf(file, "\n\nVideo URL: %s", yt->video_url);
        if (yt->video_thumbnail) fprintf(file, "\n\nVideo Thumbnail: %s", yt->video_thumbnail);
        if (yt->video_length) fprintf(file, "\n\nVideo Length: %s", yt->video_length);
        if (yt->category) fprintf(file, "\n\nCategory: %s", yt->category);
        if (yt->publish_date) fprintf(file, "\n\nPublish Date: %s", yt->publish_date);
        fprintf(file, "\n\nIngest Date: %s", yt->ingest_date);
        if (yt->view_count) fprintf(file, "\n\nView Count: %llu", yt->view_count);
        if (yt->transcript) {
            fprintf(file, "\n\nTranscript:");
            fprintf(file, "\n\"\"\"");
            fprintf(file, "\n%s", yt->transcript);
            fprintf(file, "\n\"\"\"");
        }
    }
}

static cJSON *get_captions(cJSON *root, int field) {
    cJSON *captions = cJSON_GetObjectItem(root, "captions");
    if (!cJSON_IsObject(captions)) {
        printf("%sWARNING:%s Captions not available\n", ANSI_WARN, ANSI_RESET);
        return NULL;
    }
    cJSON *tracklist = cJSON_GetObjectItem(captions, "playerCaptionsTracklistRenderer");
    if (!cJSON_IsObject(tracklist)) {
        printf("%sWARNING:%s Tracklist not available\n", ANSI_WARN, ANSI_RESET);
        return NULL;
    }
    cJSON *caption_tracks = cJSON_GetObjectItem(tracklist, "captionTracks");
    if (!cJSON_IsArray(caption_tracks)) {
        printf("%sWARNING:%s Caption tracks not available\n", ANSI_WARN, ANSI_RESET);
        return NULL;
    }
    cJSON *track = cJSON_GetArrayItem(caption_tracks, 0);
    if (!cJSON_IsObject(track)) {
        printf("%sWARNING:%s Track not available\n", ANSI_WARN, ANSI_RESET);
        return NULL;
    }
    cJSON *translation_langs = cJSON_GetObjectItem(tracklist, "translationLanguages");
    if (cJSON_IsArray(translation_langs) && cJSON_GetArraySize(translation_langs) > 0) {
        if (field == 't') {
            return track;
        }
        return translation_langs;
    }
    return NULL;
}

static void show_lang_available(cJSON *root) {
    cJSON *translation_langs = get_captions(root, 0);
    if (translation_langs) {
        printf("%sOK:%s Available transcript translation languages:\n", ANSI_INFO, ANSI_RESET);
        cJSON *translation_lang;
        cJSON_ArrayForEach(translation_lang, translation_langs) {
            cJSON *lang_code = cJSON_GetObjectItem(translation_lang, "languageCode");
            cJSON *lang_name = cJSON_GetObjectItem(translation_lang, "languageName");
            cJSON *text = cJSON_GetObjectItem(lang_name, "simpleText");
            printf("- %s -> %s\n", text->valuestring, lang_code->valuestring);
        }
    } else {
        printf("%sWARNING:%s Transcript not available\n", ANSI_WARN, ANSI_RESET);
    }
}

int ingest(const char *url, struct YtingestOpt *opt) {
    char ingest_tmp[ISO8601_BUFFER_SIZE];
    ingest_date(ingest_tmp, sizeof(ingest_tmp));

    tolowercase(opt->doh);

    char *raw_content = fetch(url, RF_PLAINTEXT, NULL, opt->doh);
    if (!raw_content) return 1;

    char *json_str = extract(raw_content);
    if (!json_str) {
        printf("%sERROR:%s Failed to extract JSON\n", ANSI_ERROR, ANSI_RESET);
        free(raw_content);
        return 1;
    }

    free(raw_content);

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        printf("%sERROR:%s Failed to parse JSON\n", ANSI_ERROR, ANSI_RESET);
        free(json_str);
        return 1;
    }

    free(json_str);

    cJSON *video_details = cJSON_GetObjectItem(root, "videoDetails");
    if (!cJSON_IsObject(video_details)) {
        printf("%sERROR:%s Video details not found\n", ANSI_ERROR, ANSI_RESET);
        cJSON_Delete(root);
        return 1;
    }

    int exit_status = 0;

    if (opt->lang_available) {
        show_lang_available(root);
        cJSON_Delete(root);
        return exit_status;
    }

    Ytingest yt = {0};

    yt.ingest_date = ingest_tmp;

    if (strcmp(opt->format, "txt") != 0) {
        tolowercase(opt->format);
        if (strcmp("json", opt->format) != 0 && strcmp("md", opt->format) != 0) opt->format = "txt";
    }

    cJSON *video_id = cJSON_GetObjectItem(video_details, "videoId");

    FILE *file;
    char *filename;
    if (opt->output_path) {
        trim(opt->output_path);
        const char *output_path_fmt = NULL;
        size_t output_path_len = strlen(opt->output_path);
        if ((output_path_len == 1 && strcmp(opt->output_path, "/") == 0) ||
            (output_path_len > 1 && opt->output_path[output_path_len - 1] == '/')) {
            output_path_fmt = "%syt_%s_%s.%s";
        } else {
            output_path_fmt = "%s/yt_%s_%s.%s";
        }
        size_t output_path_size =
            snprintf(NULL, 0, output_path_fmt, opt->output_path, opt->lang, video_id->valuestring, opt->format);
        filename = malloc(output_path_size + 1);
        if (!filename) {
            printf("Failed to allocate memory - %d\n", __LINE__);
            cJSON_Delete(root);
            return 1;
        }
        snprintf(filename, output_path_size + 1, output_path_fmt, opt->output_path, opt->lang, video_id->valuestring,
                 opt->format);
        file = fopen(filename, "w+");
    } else {
        const char *output_path_fmt = "yt_%s_%s.%s";
        size_t output_path_size = snprintf(NULL, 0, output_path_fmt, opt->lang, video_id->valuestring, opt->format);
        filename = malloc(output_path_size + 1);
        if (!filename) {
            printf("Failed to allocate memory - %d\n", __LINE__);
            cJSON_Delete(root);
            return 1;
        }
        snprintf(filename, output_path_size + 1, output_path_fmt, opt->lang, video_id->valuestring, opt->format);
        file = fopen(filename, "w+");
    }
    if (!file) {
        printf("Failed to create file\n");
        free(filename);
        cJSON_Delete(root);
        return 1;
    }

    yt.video_id = video_id->valuestring;

    cJSON *title = cJSON_GetObjectItem(video_details, "title");
    yt.title = title->valuestring;

    if (isinclude(opt->exclude, "author")) {
        cJSON *author = cJSON_GetObjectItem(video_details, "author");
        yt.author = author->valuestring;
    }

    if (isinclude(opt->exclude, "description")) {
        cJSON *description = cJSON_GetObjectItem(video_details, "shortDescription");
        if (cJSON_IsString(description) && !isempty(description->valuestring)) {
            if (strcmp(opt->format, "json") == 0) {
                char *tmp_buff = normalize(description->valuestring);
                yt.description = mstrdup(tmp_buff);
                free(tmp_buff);
            } else {
                yt.description = description->valuestring;
            }
        }
    }

    if (isinclude(opt->exclude, "video_length")) {
        cJSON *video_length = cJSON_GetObjectItem(video_details, "lengthSeconds");
        if (cJSON_IsString(video_length) && !isempty(video_length->valuestring)) {
            char tmp[9];
            yt.video_length = time_yt((uint16_t)atoi(video_length->valuestring), tmp, sizeof(tmp));
        }
    }

    cJSON *islive = cJSON_GetObjectItem(video_details, "isLiveContent");
    if (cJSON_IsTrue(islive)) {
        printf("The video is live!\n");
        if (isinclude(opt->exclude, "video_live")) yt.video_live = true;
    }

    if (isinclude(opt->exclude, "keywords")) {
        cJSON *keywords = cJSON_GetObjectItem(video_details, "keywords");
        if (cJSON_IsArray(keywords) && cJSON_GetArraySize(keywords) > 0) {
            yt.keyword.size = cJSON_GetArraySize(keywords);
            yt.keyword.items = malloc(yt.keyword.size * sizeof(char *));
            if (!yt.keyword.items) {
                exit_status = 1;
                goto done;
            }
            int index = 0;
            cJSON *keyword;
            cJSON_ArrayForEach(keyword, keywords) {
                if (cJSON_IsString(keyword) && !isempty(keyword->valuestring)) {
                    yt.keyword.items[index++] = keyword->valuestring;
                }
            }
        }
    }

    cJSON *microformat = cJSON_GetObjectItem(root, "microformat");
    cJSON *player_microformat_renderer = cJSON_GetObjectItem(microformat, "playerMicroformatRenderer");

    if (isinclude(opt->exclude, "video_url")) {
        if (mstrcasestr(url, "youtube.com/shorts/")) {
            const char *video_url_fmt = "https://www.youtube.com/shorts/%s";
            size_t video_url_size = snprintf(NULL, 0, video_url_fmt, video_id->valuestring);
            yt.video_url = malloc(video_url_size + 1);
            if (!yt.video_url) {
                exit_status = 1;
                printf("Failed to allocate memory\n");
                goto done;
            }
            snprintf(yt.video_url, video_url_size + 1, video_url_fmt, video_id->valuestring);
        } else {
            const char *video_url_fmt = "https://www.youtube.com/watch?v=%s";
            size_t video_url_size = snprintf(NULL, 0, video_url_fmt, video_id->valuestring);
            yt.video_url = malloc(video_url_size + 1);
            if (!yt.video_url) {
                exit_status = 1;
                printf("Failed to allocate memory\n");
                goto done;
            }
            snprintf(yt.video_url, video_url_size + 1, video_url_fmt, video_id->valuestring);
        }
    }

    if (isinclude(opt->exclude, "video_thumbnail")) {
        cJSON *thumbnail = cJSON_GetObjectItem(player_microformat_renderer, "thumbnail");
        if (!cJSON_IsObject(thumbnail)) NOOP;
        cJSON *thumbnails = cJSON_GetObjectItem(thumbnail, "thumbnails");
        if (!cJSON_IsArray(thumbnails)) NOOP;
        cJSON *thumbnail_index = cJSON_GetArrayItem(thumbnails, 0);
        if (!cJSON_IsObject(thumbnail_index)) NOOP;
        cJSON *thumbnail_url = cJSON_GetObjectItem(thumbnail_index, "url");
        if (cJSON_IsString(thumbnail_url)) yt.video_thumbnail = thumbnail_url->valuestring;
    }

    if (isinclude(opt->exclude, "owner_profile_url")) {
        cJSON *owner_profile_url = cJSON_GetObjectItem(player_microformat_renderer, "ownerProfileUrl");
        yt.owner_profile_url = tohttps(owner_profile_url->valuestring);
    }

    if (isinclude(opt->exclude, "category")) {
        cJSON *category = cJSON_GetObjectItem(player_microformat_renderer, "category");
        if (cJSON_IsString(category) && !isempty(category->valuestring)) {
            yt.category = category->valuestring;
        }
    }

    if (isinclude(opt->exclude, "publish_date")) {
        cJSON *publish_date = cJSON_GetObjectItem(player_microformat_renderer, "publishDate");
        yt.publish_date = publish_date->valuestring;
    }

    if (isinclude(opt->exclude, "view_count")) {
        cJSON *view_count = cJSON_GetObjectItem(player_microformat_renderer, "viewCount");
        if (cJSON_IsString(view_count) && !isempty(view_count->valuestring)) {
            yt.view_count = strtoull(view_count->valuestring, NULL, 10);
        } else {
            printf("%sWARNING:%s Indication of member-only video content\n", ANSI_WARN, ANSI_RESET);
        }
    }

    if (isinclude(opt->exclude, "transcript")) {
        cJSON *track = get_captions(root, 't');
        if (track) {
            cJSON *base_url = cJSON_GetObjectItem(track, "baseUrl");
            const char *endpoint_fmt = "%s&fmt=json3&tlang=%s";
            size_t endpoint_size = snprintf(NULL, 0, endpoint_fmt, base_url->valuestring, opt->lang);
            char *endpoint_buff = malloc(endpoint_size + 1);
            if (!endpoint_buff) {
                exit_status = 1;
                printf("Failed to allocate memory\n");
                goto done;
            }
            snprintf(endpoint_buff, endpoint_size + 1, endpoint_fmt, base_url->valuestring, opt->lang);
            char *raw_transcript = fetch(endpoint_buff, RF_JSON, NULL, NULL);
            if (raw_transcript) {
                char *transcript = parse_transcript(raw_transcript, &yt, opt);
                if (transcript) {
                    if (strcmp(transcript, "INVALID_LANG") != 0) {
                        if (strcmp(opt->format, "json") == 0) {
                            char *tmp_buff = normalize(transcript);
                            yt.transcript = mstrdup(tmp_buff);
                            free(tmp_buff);
                        } else {
                            yt.transcript = mstrdup(transcript);
                        }
                    }
                    free(transcript);
                }
                free(raw_transcript);
            }
            free(endpoint_buff);
        } else {
            printf("%sWARNING:%s Transcript not available\n", ANSI_WARN, ANSI_RESET);
        }
    }

done:
    if (exit_status) {
        fclose(file);
        remove(filename);
    } else {
        write_yt(file, &yt, opt->format);
#ifdef USE_LIBTOKENCOUNT
        if (opt->token_count) token_count(file, opt->token_count);
#endif
        printf("%sOK:%s Output file has been created in %s\n", ANSI_INFO, ANSI_RESET, filename);
        fclose(file);
    }

    if (strcmp(opt->format, "json") == 0) {
        if (yt.transcript) free(yt.transcript);
        if (yt.description) free(yt.description);
    } else {
        if (yt.transcript) free(yt.transcript);
    }
    if (yt.video_url) free(yt.video_url);
    if (yt.owner_profile_url) free(yt.owner_profile_url);
    if (yt.keyword.items) {
        free(yt.keyword.items);
        yt.keyword.size = 0;
    }
    free(filename);

    cJSON_Delete(root);

    return exit_status;
}
