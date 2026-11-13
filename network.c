#define _GNU_SOURCE  // strcasestr
#include <ctype.h>
#include "opds_app.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

extern void LogDebug(const char *msg);
extern void ShowDownloadProgress(long long total, long long current);
extern int CheckDownloadCancel();
extern char current_host[MAX_STR_LEN]; // Populated by ui.c

// RELATIVE URL SAFETY NET
void EnsureAbsoluteURL(const char *in_url, char *out_url) {
    if (strncmp(in_url, "http://", 7) == 0 || strncmp(in_url, "https://", 8) == 0) {
        strncpy(out_url, in_url, MAX_STR_LEN * 2 - 1);
        return;
    }
    
    char base_domain[256] = {0};
    const char *proto_end = strstr(current_host, "://");
    if (proto_end) {
        const char *path_start = strchr(proto_end + 3, '/');
        if (path_start) {
            strncpy(base_domain, current_host, path_start - current_host);
        } else {
            strncpy(base_domain, current_host, 255);
        }
    } else {
        strncpy(base_domain, current_host, 255);
    }
    
    if (in_url[0] == '/') {
        snprintf(out_url, MAX_STR_LEN * 2, "%s%s", base_domain, in_url);
    } else {
        snprintf(out_url, MAX_STR_LEN * 2, "%s/%s", base_domain, in_url);
    }
}

// Memory write callback for catalog XML
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr) return 0; 
    
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    
    return realsize;
}

// File write callback for downloading Books and Images
static size_t WriteFileCallback(void *ptr, size_t size, size_t nmemb, void *stream) {
    return fwrite(ptr, size, nmemb, (FILE *)stream);
}

// Debug callback to log network headers
static int DebugCallback(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr) {
    if (type == CURLINFO_HEADER_OUT) {
        char buf[MAX_STR_LEN];
        char *token = strtok(data, "\r\n");
        while (token != NULL) {
            snprintf(buf, sizeof(buf), ">>> SEND: %s", token);
            LogDebug(buf);
            token = strtok(NULL, "\r\n");
        }
    }
    return 0;
}

// Progress callback to update the UI
static int ProgressCallback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow) {
    if (dltotal > 0) {
        ShowDownloadProgress((long long)dltotal, (long long)dlnow);
    }
    return CheckDownloadCancel();
}

// QUOTE-AWARE HEADER PARSER
static size_t HeaderCallback(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t numbytes = size * nitems;
    char *filename = (char *)userdata;
    
    char log_buf[MAX_STR_LEN];
    char clean_buf[512] = {0};
    size_t copy_len = numbytes < 511 ? numbytes : 511;
    strncpy(clean_buf, buffer, copy_len);
    
    while(copy_len > 0 && (clean_buf[copy_len-1] == '\n' || clean_buf[copy_len-1] == '\r')) {
        clean_buf[copy_len-1] = '\0'; copy_len--;
    }
    
    if (copy_len > 0) {
        snprintf(log_buf, sizeof(log_buf), "<<< RECV: %s", clean_buf);
        LogDebug(log_buf);
    }

    // Parse the NUL-terminated copy: curl's header buffer is not terminated.
    if (filename && strncasecmp(clean_buf, "Content-Disposition:", 20) == 0) {
        // Prefer filename*=UTF-8''<percent-encoded> (RFC 6266/5987): some servers
        // send only this form, and it carries the exact name.
        char *ext = strcasestr(clean_buf, "filename*=");
        if (ext) {
            char *p = ext + 10;
            // Skip charset'language' (language may be empty: UTF-8''name).
            char *q1 = strchr(p, '\'');
            char *q = q1 ? strchr(q1 + 1, '\'') : NULL;
            if (q) {
                p = q + 1;
                int i = 0;
                while (*p && *p != ';' && *p != ' ' && i < 255) {
                    if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                        char hex[3] = { p[1], p[2], 0 };
                        filename[i++] = (char)strtol(hex, NULL, 16);
                        p += 3;
                    } else {
                        filename[i++] = *p++;
                    }
                }
                filename[i] = '\0';
                if (i > 0) return numbytes;
            }
        }
        char *ptr = strcasestr(clean_buf, "filename=");
        if (ptr) {
            ptr += 9; 
            if (*ptr == '"') {
                ptr++; 
                char *end = strchr(ptr, '"');
                if (end) {
                    int len = end - ptr;
                    if (len > 255) len = 255;
                    strncpy(filename, ptr, len);
                    filename[len] = '\0';
                }
            } 
            else {
                int i = 0;
                while (ptr[i] && ptr[i] != ' ' && ptr[i] != ';' && ptr[i] != '\r' && ptr[i] != '\n' && i < 255) {
                    filename[i] = ptr[i]; 
                    i++;
                }
                filename[i] = '\0';
            }
        }
    }
    return numbytes;
}

int FetchFeed(const char *url, const char *user, const char *pass, struct MemoryStruct *chunk) {
    CURL *curl;
    CURLcode res;
    
    char safe_url[MAX_STR_LEN * 2] = {0};
    EnsureAbsoluteURL(url, safe_url);

    chunk->memory = malloc(1);
    chunk->size = 0;

    curl = curl_easy_init();
    if(!curl) return -1;
    
    LogDebug("================ NETWORK REQUEST START ================");
    char msg[MAX_STR_LEN]; snprintf(msg, sizeof(msg), "TARGET: [%s]", safe_url); LogDebug(msg);

    // Relaxed Network Timeouts (30 seconds)
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);

    curl_easy_setopt(curl, CURLOPT_URL, safe_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, APP_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, DebugCallback);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);

    if (user && pass && strlen(user) > 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        char auth[512]; snprintf(auth, sizeof(auth), "%s:%s", user, pass);
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth);
    }

    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    LogDebug("================ NETWORK REQUEST END ================");

    return (res == CURLE_OK) ? 0 : -1;
}

int DownloadBook(const char *url, const char *filepath, char *server_fname, const char *user, const char *pass) {
    CURL *curl;
    CURLcode res;
    FILE *fp;

    char safe_url[MAX_STR_LEN * 2] = {0};
    EnsureAbsoluteURL(url, safe_url);

    fp = fopen(filepath, "wb");
    if (!fp) return -1;

    if (server_fname) server_fname[0] = '\0'; 

    curl = curl_easy_init();
    if (!curl) { fclose(fp); return -1; }

    LogDebug("================ DOWNLOAD REQUEST START ================");
    char msg[MAX_STR_LEN]; snprintf(msg, sizeof(msg), "DOWNLOAD TARGET: [%s]", safe_url); LogDebug(msg);

    // Relaxed Network Timeouts (30 seconds)
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);

    curl_easy_setopt(curl, CURLOPT_URL, safe_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, APP_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, DebugCallback);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, ProgressCallback);
    
    if (server_fname) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, server_fname);
    }

    if (user && pass && strlen(user) > 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        char auth[512]; snprintf(auth, sizeof(auth), "%s:%s", user, pass);
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth);
    }

    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    fclose(fp);
    LogDebug("================ DOWNLOAD REQUEST END ================");

    if (res != CURLE_OK) { remove(filepath); return -1; }
    return 0;
}

int DownloadImage(const char *url, const char *filepath, const char *user, const char *pass) {
    CURL *curl;
    CURLcode res;
    FILE *fp;

    char safe_url[MAX_STR_LEN * 2] = {0};
    EnsureAbsoluteURL(url, safe_url);

    fp = fopen(filepath, "wb");
    if (!fp) return -1;

    curl = curl_easy_init();
    if (!curl) { fclose(fp); return -1; }

    // Relaxed Network Timeouts (30 seconds)
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);

    curl_easy_setopt(curl, CURLOPT_URL, safe_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, APP_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    if (user && pass && strlen(user) > 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        char auth[512]; snprintf(auth, sizeof(auth), "%s:%s", user, pass);
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth);
    }

    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (res != CURLE_OK) { remove(filepath); return -1; }
    return 0;
}
