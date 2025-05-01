#include "ingest.h"

#include <ctype.h>
#include <curl/curl.h>
#include <regex.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON/cJSON.h"
#include "tiktoken-c/tiktoken.h"

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
        *category, *publish_date, *transcript;
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

static char *fetch(const char *url, const RawFormat raw_format, const char *body) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Failed to initialize curl\n");
        return NULL;
    }

    Memory chunk = {0};
    CURLcode res;

    const char *HEADER_LIST[] = {
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8",
        "Accept-Language: en-US,en;q=0.5",
        "Cache-Control: no-cache, no-store, must-revalidate",
        "Connection: keep-alive",
        NULL,
    };
    struct curl_slist *headers = NULL;
    if (raw_format == RF_JSON) {
        headers = curl_slist_append(headers, "Accept: application/json");
        headers = curl_slist_append(headers, HEADER_LIST[2]);
        headers = curl_slist_append(headers, "Content-Type: application/json");
    } else {
        for (int i = 0; HEADER_LIST[i] != NULL; i++) {
            headers = curl_slist_append(headers, HEADER_LIST[i]);
        }
    }

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (raw_format != RF_JSON)
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                         "Chrome/135.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
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

static void trim(char *text) {
    if (!text) return;

    char *start = text;
    char *end;
    while (isspace((unsigned char)*start)) start++;
    if (*start == '\0') {
        text[0] = '\0';
        return;
    }

    end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;
    int new_len = end - start + 1;
    memmove(text, start, new_len);
    text[new_len] = '\0';
}

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
    const char diff = 'a' - 'A';
    while (*text) {
        // Check if the character is an ASCII uppercase letter
        if (*text >= 'A' && *text <= 'Z') {
            // Convert to lowercase by adding the ASCII difference
            *text = *text + diff;
        }
        text++;
    }
}

static char *toparagraph(char *json_str) {
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

    size_t current_len = 0;
    size_t transcript_size = 1;
    char *transcript_buff = malloc(transcript_size);
    if (!transcript_buff) {
        printf("Failed to allocate memory - %d\n", __LINE__);
        cJSON_Delete(root);
        return NULL;
    }
    transcript_buff[0] = '\0';

    cJSON *event;
    cJSON_ArrayForEach(event, events) {
        cJSON *segments = cJSON_GetObjectItem(event, "segs");
        if (cJSON_IsArray(segments)) {
            cJSON *segment;
            cJSON_ArrayForEach(segment, segments) {
                cJSON *utf8 = cJSON_GetObjectItem(segment, "utf8");
                // TODO: Add timestamp to the transcript
                // And then transform as query parameter => youtube.com/watch?v=OeYnV9zp7Dk&t=<TIMESTAMP>
                if (cJSON_IsString(utf8) && !isempty(utf8->valuestring)) {
                    size_t text_len = strlen(utf8->valuestring);
                    int ws_extra = (current_len > 0 && transcript_buff[current_len - 1] != ' ');
                    size_t ws_len = ws_extra ? 1 : 0;
                    size_t realloc_size = current_len + ws_len + text_len + 1;
                    if (realloc_size > transcript_size) {
                        char *tmp = realloc(transcript_buff, realloc_size);
                        if (!tmp) {
                            printf("Failed to reallocate memory - %d\n", __LINE__);
                            free(transcript_buff);
                            cJSON_Delete(root);
                            return NULL;
                        }
                        transcript_buff = tmp;
                        transcript_size = realloc_size;
                    }
                    if (ws_extra) {
                        transcript_buff[current_len] = ' ';
                        current_len++;
                    }
                    memcpy(transcript_buff + current_len, utf8->valuestring, text_len);
                    current_len += text_len;
                    transcript_buff[current_len] = '\0';
                }
            }
        }
    }

    cJSON_Delete(root);

    // Trim the buffer to the right size
    // If this fails, the original buffer is still valid
    char *final_buff = realloc(transcript_buff, current_len + 1);
    if (final_buff) transcript_buff = final_buff;

    return transcript_buff;
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

        char *res = fetch(endpoint_buff, RF_JSON, payload_buff);
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

static char *time_yt(uint16_t video_len, char *buff, size_t size) {
    if (video_len == 0) return NULL;
    if (video_len < 3600) {
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
            fprintf(file, "\n```");
            fprintf(file, "\n%s", yt->transcript);
            fprintf(file, "\n```");
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
        if (yt->view_count) fprintf(file, "\n\nView Count: %llu", yt->view_count);
        if (yt->transcript) {
            fprintf(file, "\n\nTranscript:");
            fprintf(file, "\n\"\"\"");
            fprintf(file, "\n%s", yt->transcript);
            fprintf(file, "\n\"\"\"");
        }
    }
}

static void show_lang_available(cJSON *root) {
    cJSON *captions = cJSON_GetObjectItem(root, "captions");
    if (cJSON_IsObject(captions)) {
        cJSON *tracklist = cJSON_GetObjectItem(captions, "playerCaptionsTracklistRenderer");
        if (cJSON_IsObject(tracklist)) {
            cJSON *caption_tracks = cJSON_GetObjectItem(tracklist, "captionTracks");
            if (cJSON_IsArray(caption_tracks)) {
                cJSON *track = cJSON_GetArrayItem(caption_tracks, 0);
                if (cJSON_IsObject(track)) {
                    cJSON *translation_langs = cJSON_GetObjectItem(tracklist, "translationLanguages");
                    if (cJSON_IsArray(translation_langs) && cJSON_GetArraySize(translation_langs) > 0) {
                        printf("%sOK:%s Available transcript translation languages:\n", ANSI_INFO, ANSI_RESET);
                        cJSON *translation_lang;
                        cJSON_ArrayForEach(translation_lang, translation_langs) {
                            cJSON *lang_code = cJSON_GetObjectItem(translation_lang, "languageCode");
                            cJSON *lang_name = cJSON_GetObjectItem(translation_lang, "languageName");
                            cJSON *text = cJSON_GetObjectItem(lang_name, "simpleText");
                            printf("- %s -> %s\n", text->valuestring, lang_code->valuestring);
                        }
                    }
                }
            }
        }
    } else {
        printf("%sWARNING:%s Transcript not available\n", ANSI_WARN, ANSI_RESET);
    }
}

int ingest(const char *url, struct YtingestOpt *opt) {
    char *raw_content = fetch(url, RF_PLAINTEXT, NULL);
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

    tolowercase(opt->format);

    const char *FORMAT_LIST[] = {"json", "txt", "md", NULL};
    int fallback_format = 0;
    for (int i = 0; FORMAT_LIST[i] != NULL; i++) {
        if (strcmp(FORMAT_LIST[i], opt->format) == 0) {
            fallback_format = 1;
            break;
        }
    }
    if (fallback_format == 0) opt->format = "txt";

    cJSON *video_id = cJSON_GetObjectItem(video_details, "videoId");

    FILE *file;
    char *filename;
    if (opt->output_path) {
        trim(opt->output_path);
        const char *output_path_fmt = NULL;
        size_t output_path_len = strlen(opt->output_path);
        if ((output_path_len == 1 && strcmp(opt->output_path, "/") == 0) ||
            (output_path_len > 1 && opt->output_path[output_path_len - 1] == '/')) {
            output_path_fmt = "%syt_%s.%s";
        } else {
            output_path_fmt = "%s/yt_%s.%s";
        }
        size_t output_path_size =
            snprintf(NULL, 0, output_path_fmt, opt->output_path, video_id->valuestring, opt->format);
        filename = malloc(output_path_size + 1);
        if (!filename) {
            printf("Failed to allocate memory - %d\n", __LINE__);
            cJSON_Delete(root);
            return 1;
        }
        snprintf(filename, output_path_size + 1, output_path_fmt, opt->output_path, video_id->valuestring, opt->format);
        file = fopen(filename, "w+");
    } else {
        const char *output_path_fmt = "yt_%s.%s";
        size_t output_path_size = snprintf(NULL, 0, output_path_fmt, video_id->valuestring, opt->format);
        filename = malloc(output_path_size + 1);
        if (!filename) {
            printf("Failed to allocate memory - %d\n", __LINE__);
            cJSON_Delete(root);
            return 1;
        }
        snprintf(filename, output_path_size + 1, "yt_%s.%s", video_id->valuestring, opt->format);
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
            char time[9];
            yt.video_length = time_yt(atoi(video_length->valuestring), time, sizeof(time));
        }
    }

    cJSON *islive = cJSON_GetObjectItem(video_details, "isLiveContent");
    if (cJSON_IsTrue(islive)) {
        printf("The video is live!\n");
        if (isinclude(opt->exclude, "video_live")) {
            yt.video_live = true;
        }
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
        if (cJSON_IsObject(thumbnail)) {
            cJSON *thumbnails = cJSON_GetObjectItem(thumbnail, "thumbnails");
            if (cJSON_IsArray(thumbnails) && cJSON_GetArraySize(thumbnails) > 0) {
                cJSON *thumbnail_index = cJSON_GetArrayItem(thumbnails, 0);
                if (cJSON_IsObject(thumbnail_index)) {
                    cJSON *thumbnail_url = cJSON_GetObjectItem(thumbnail_index, "url");
                    if (cJSON_IsString(thumbnail_url)) {
                        yt.video_thumbnail = thumbnail_url->valuestring;
                    }
                }
            }
        }
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
        cJSON *captions = cJSON_GetObjectItem(root, "captions");
        if (cJSON_IsObject(captions)) {
            cJSON *tracklist = cJSON_GetObjectItem(captions, "playerCaptionsTracklistRenderer");
            if (cJSON_IsObject(tracklist)) {
                cJSON *caption_tracks = cJSON_GetObjectItem(tracklist, "captionTracks");
                if (cJSON_IsArray(caption_tracks)) {
                    cJSON *track = cJSON_GetArrayItem(caption_tracks, 0);
                    if (cJSON_IsObject(track)) {
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
                        char *raw_transcript = fetch(endpoint_buff, RF_JSON, NULL);
                        if (raw_transcript) {
                            char *transcript = toparagraph(raw_transcript);
                            if (transcript) {
                                if (strcmp(transcript, "INVALID_LANG") != 0) {
                                    if (strcmp(opt->format, "json") == 0) {
                                        char *tmp_buff = normalize(transcript);
                                        yt.transcript = mstrdup(tmp_buff);
                                        free(tmp_buff);
                                    } else {
                                        yt.transcript = transcript;
                                    }
                                }
                                free(transcript);
                            }
                            free(raw_transcript);
                        }
                        free(endpoint_buff);
                    }
                }
            }
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
        if (opt->token_count) token_count(file, opt->token_count);
        printf("%sOK:%s Output file has been created in %s\n", ANSI_INFO, ANSI_RESET, filename);
        fclose(file);
    }

    if (strcmp(opt->format, "json") == 0) {
        free(yt.transcript);
        free(yt.description);
    }
    if (yt.video_url) free(yt.video_url);
    if (yt.owner_profile_url) free(yt.owner_profile_url);
    if (yt.keyword.items) free(yt.keyword.items);
    free(filename);

    cJSON_Delete(root);

    return exit_status;
}
