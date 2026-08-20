#include "opds_app.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <inkview.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include "icons.h" 

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Font and image pointers
ifont *font48 = NULL;
ifont *font36 = NULL;
ifont *font30 = NULL;
ifont *font24 = NULL;
ibitmap *current_cover_bmp = NULL;

ibitmap *folder_icon_bmp = NULL;
ibitmap *book_fallback_bmp = NULL;
ibitmap *list_thumbs[MAX_ENTRIES] = {NULL};

// Navigation tracking
#define MAX_NAV_STACK 32
char nav_stack[MAX_NAV_STACK][MAX_STR_LEN];
int nav_stack_ptr = 0;

#define MAX_PAGE_STACK 16
char page_stack[MAX_PAGE_STACK][MAX_STR_LEN];
int page_offset_stack[MAX_PAGE_STACK];
int page_stack_ptr = 0;
int current_global_offset = 0;
int return_to_last_page = 0; 
int jump_target_page = -1;
int current_page = 0;

char last_loaded_url[MAX_STR_LEN] = {0};

extern void LogDebug(const char *msg);
extern int is_opensearch_url;
extern int ParseOpenSearch(const char *xml_data, const char *base_url, char *template_out);

static int summary_scroll_y = 0;
static int view_x = 0;
static int view_y = 0;
static int view_w = 0;
static int view_h = 0;

void SetTextFont(int size, int color);
void DrawButton(int x, int y, int w, int h, const char *text, int is_dark);
void Repaint();
void LoadCatalog(const char *url);
void GoBackInHierarchy();
void FetchThumbnailsForPage();
void ManageImageCache(); 
void JIT_DecodePageThumbnails(); 

// --- NEW: Bulletproof Aggressive Dark Mode Detection ---
int IsDarkMode() {
    // 1. Try dynamic loading of ANY firmware 6.8+ inversion check function
    static int checked_dl = 0;
    static int (*dl_funcs[6])() = {NULL};
    
    if (!checked_dl) {
        void *h = dlopen("libinkview.so", RTLD_LAZY);
        if (!h) h = dlopen(NULL, RTLD_LAZY);
        if (h) {
            dl_funcs[0] = dlsym(h, "GetScreenInversionState");
            dl_funcs[1] = dlsym(h, "IvGetScreenInversionState");
            dl_funcs[2] = dlsym(h, "GetDarkMode");
            dl_funcs[3] = dlsym(h, "IsDarkMode");
            dl_funcs[4] = dlsym(h, "GetThemeDark");
            dl_funcs[5] = dlsym(h, "IsThemeDark");
        }
        checked_dl = 1;
    }
    
    for (int i = 0; i < 6; i++) {
        if (dl_funcs[i] && dl_funcs[i]()) {
            return 1;
        }
    }

    // 2. Aggressive Config Hunt - bypass cache and look for any relevant key
    const char* config_paths[] = {
        "/mnt/ext1/system/config/global.cfg",
        "/mnt/ext1/system/profiles/default/config/global.cfg",
        NULL
    };

    for (int p = 0; config_paths[p] != NULL; p++) {
        FILE *f = fopen(config_paths[p], "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                // Look for keys set to 1 or true
                if (strstr(line, "=1\n") || strstr(line, "=1\r") || strstr(line, "=true")) {
                    char lower[256];
                    for(int i = 0; line[i]; i++) lower[i] = tolower(line[i]);
                    
                    // Does it sound like dark mode? (Ignoring "night" to avoid smartlight false positives)
                    if ((strstr(lower, "dark") || strstr(lower, "invert") || strstr(lower, "invers")) && 
                        !strstr(lower, "night")) {
                        
                        char msg[512]; snprintf(msg, sizeof(msg), "HUNT: Detected dark mode via key -> %s", line);
                        LogDebug(msg);
                        fclose(f);
                        return 1;
                    }
                }
            }
            fclose(f);
        }
    }
    
    return 0;
}

void DrawSmartBitmap(int x, int y, int w, int h, ibitmap *bmp, int flags, int preserve_color) {
    if (!bmp) return;
    
    int is_dark = IsDarkMode();
    int buffer_size = bmp->scanline * bmp->height;
    unsigned char *ptr = bmp->data;
    
    // Pre-invert the raw image data in memory so the hardware inversion flips it back to normal
    if (is_dark && preserve_color) {
        for (int i = 0; i < buffer_size; i++) {
            ptr[i] = ~ptr[i]; // Fast bitwise inversion
        }
    }
    
    DrawBitmapRect(x, y, w, h, bmp, flags);
    
    // Restore the image data immediately so the cached cover isn't permanently altered
    if (is_dark && preserve_color) {
        for (int i = 0; i < buffer_size; i++) {
            ptr[i] = ~ptr[i]; // Flip it back
        }
    }
}

// Calculates the requested rows for the active server dynamically
int GetCatalogRows() {
    if (current_server_index >= 0 && current_server_index < server_count) {
        int r = servers[current_server_index].catalog_rows;
        return (r >= 4 && r <= 10) ? r : 10;
    }
    return 10;
}

// --- NEW: Centralized Visual Feedback Helper ---
void FlashArea(int x, int y, int w, int h) {
    InvertArea(x, y, w, h);
    PartialUpdate(x, y, w, h);
    usleep(100000); 
}

void SetTextFont(int size, int color) {
    // Dynamic caching system so we can generate proportionally sized fonts on the fly!
    static int cached_sizes[16] = {0};
    static ifont* cached_fonts[16] = {NULL};
    ifont *f = NULL;
    for (int i = 0; i < 16; i++) {
        if (cached_sizes[i] == size) { f = cached_fonts[i]; break; }
        if (cached_sizes[i] == 0) {
            cached_fonts[i] = OpenFont("default", size, 1);
            cached_sizes[i] = size;
            f = cached_fonts[i];
            break;
        }
    }
    if (f) SetFont(f, color);
}

void DrawButton(int x, int y, int w, int h, const char *text, int style) {
    int bg_color = 0xDDDDDD; 
    int fg_color = 0x000000; 

    if (style == 1) { bg_color = 0x555555; fg_color = 0xFFFFFF; } 
    else if (style == 2) { bg_color = 0xCC0000; fg_color = 0xFFFFFF; }

    FillArea(x, y, w, h, bg_color); 
    DrawRect(x, y, w, h, 0x000000); 
    SetTextFont(36, fg_color); 
    DrawTextRect(x, y + (h/2) - 20, w, 40, text, ALIGN_CENTER);
}

unsigned long hash_str(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) { hash = ((hash << 5) + hash) + c; }
    return hash;
}

ibitmap* LoadCoverSTB_Memory(const unsigned char *buffer, int len) {
    int width, height, channels;
    unsigned char *img = stbi_load_from_memory(buffer, len, &width, &height, &channels, 3);
    if (!img) return NULL;

    int scanline = (width * 3 + 3) & ~3; 
    int data_size = scanline * height;

    ibitmap *bm = (ibitmap *)malloc(sizeof(ibitmap) + data_size);
    if (!bm) { stbi_image_free(img); return NULL; }

    bm->width = width; 
    bm->height = height; 
    bm->depth = 24; 
    bm->scanline = scanline;

    unsigned char *dst = (unsigned char *)bm->data;
    for (int y = 0; y < height; y++) {
        unsigned char *src_row = img + (y * width * 3);
        unsigned char *dst_row = dst + (y * scanline);
        for (int x = 0; x < width; x++) {
            dst_row[x * 3 + 0] = src_row[x * 3 + 0]; 
            dst_row[x * 3 + 1] = src_row[x * 3 + 1]; 
            dst_row[x * 3 + 2] = src_row[x * 3 + 2]; 
        }
    }
    stbi_image_free(img);
    return bm;
}

ibitmap* LoadCoverSTB(const char *path) {
    int width, height, channels;
    unsigned char *img = stbi_load(path, &width, &height, &channels, 3);
    if (!img) return NULL;

    int scanline = (width * 3 + 3) & ~3; 
    int data_size = scanline * height;

    ibitmap *bm = (ibitmap *)malloc(sizeof(ibitmap) + data_size);
    if (!bm) { stbi_image_free(img); return NULL; }

    bm->width = width; 
    bm->height = height; 
    bm->depth = 24; 
    bm->scanline = scanline;

    unsigned char *dst = (unsigned char *)bm->data;
    for (int y = 0; y < height; y++) {
        unsigned char *src_row = img + (y * width * 3);
        unsigned char *dst_row = dst + (y * scanline);
        for (int x = 0; x < width; x++) {
            dst_row[x * 3 + 0] = src_row[x * 3 + 0]; 
            dst_row[x * 3 + 1] = src_row[x * 3 + 1]; 
            dst_row[x * 3 + 2] = src_row[x * 3 + 2]; 
        }
    }
    stbi_image_free(img);
    return bm;
}

void ShowDownloadProgress(long long total, long long current) {
    static int last_percent = -1;
    int percent = (total > 0) ? (int)((current * 100) / total) : 0;
    if (percent == last_percent) return;
    last_percent = percent;

    int bw = sys_width * 0.8;
    int bh = 60;
    int bx = (sys_width - bw) / 2;
    int by = (sys_height / 2) - 60;

    FillArea(bx - 20, by - 80, bw + 40, 200, 0xFFFFFF);
    DrawRect(bx - 20, by - 80, bw + 40, 200, 0x000000);

    SetTextFont(36, 0x000000);
    char buf[64]; snprintf(buf, sizeof(buf), "Downloading: %d%%", percent);
    DrawTextRect(bx, by - 50, bw, 40, buf, ALIGN_CENTER);
    
    DrawRect(bx, by + 10, bw, bh, 0x000000);
    int fill_w = (int)((bw - 4) * percent / 100);
    if (fill_w > 0) FillArea(bx + 2, by + 12, fill_w, bh - 4, 0x555555);

    PartialUpdate(bx - 30, by - 90, bw + 60, 220);
}

int CheckDownloadCancel() { return 0; }

int EnsureNetwork() { 
    if (NetConnect(NULL) != 0) {
        Message(ICON_ERROR, "No Network", "Please turn on Wi-Fi to connect.", 3000);
        return -1; 
    }
    return 0; 
}

void EnsureDirectories() {
    mkdir(APP_ROOT_DIR, 0777);
    mkdir(IMAGES_DIR, 0777);
    mkdir(BOOKS_DIR, 0777); 
}

void FreeListThumbs() {
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (list_thumbs[i]) { free(list_thumbs[i]); list_thumbs[i] = NULL; }
    }
}

// --- Image Cache Management ---
#define MAX_CACHE_BYTES 20971520 
#define TARGET_CACHE_BYTES 15728640 

typedef struct {
    char path[256];
    time_t mtime;
    off_t size;
} CacheEntry;

int compare_cache_entries(const void *a, const void *b) {
    CacheEntry *ea = (CacheEntry *)a;
    CacheEntry *eb = (CacheEntry *)b;
    if (ea->mtime < eb->mtime) return -1;
    if (ea->mtime > eb->mtime) return 1;
    return 0;
}

void ManageImageCache() {
    DIR *dir = opendir(IMAGES_DIR);
    if (!dir) return;

    struct dirent *ent;
    int capacity = 100;
    int count = 0;
    CacheEntry *entries = malloc(capacity * sizeof(CacheEntry));
    if (!entries) { closedir(dir); return; }

    long long total_size = 0;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "cache_", 6) == 0) {
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s%s", IMAGES_DIR, ent->d_name);
            
            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
                if (count >= capacity) {
                    capacity *= 2;
                    CacheEntry *tmp = realloc(entries, capacity * sizeof(CacheEntry));
                    if (!tmp) break; 
                    entries = tmp;
                }
                strncpy(entries[count].path, full_path, 255);
                entries[count].path[255] = '\0';
                entries[count].mtime = st.st_mtime;
                entries[count].size = st.st_size;
                total_size += st.st_size;
                count++;
            }
        }
    }
    closedir(dir);

    if (total_size > MAX_CACHE_BYTES) {
        LogDebug("Cache Janitor: 20MB Threshold Reached. Cleaning oldest images...");
        qsort(entries, count, sizeof(CacheEntry), compare_cache_entries);
        
        for (int i = 0; i < count; i++) {
            if (remove(entries[i].path) == 0) {
                total_size -= entries[i].size;
                if (total_size <= TARGET_CACHE_BYTES) {
                    break;
                }
            }
        }
    }
    free(entries);
}

void LoadStockIcons() {
    if (!folder_icon_bmp) folder_icon_bmp = LoadCoverSTB_Memory(folder_icon_png, folder_icon_png_len);
    if (!book_fallback_bmp) book_fallback_bmp = LoadCoverSTB_Memory(book_icon_png, book_icon_png_len);
}

void UnloadGlobalCover() { 
    if (current_cover_bmp) { free(current_cover_bmp); current_cover_bmp = NULL; } 
}

void GoBackInHierarchy() {
    if (nav_stack_ptr > 0) {
        nav_stack_ptr--; 
        page_stack_ptr = 0;      
        current_global_offset = 0;
        return_to_last_page = 0;  
        LoadCatalog(nav_stack[nav_stack_ptr]); 
    } else { 
        current_state = STATE_SERVER_OPTIONS; 
        Repaint(); 
    }
}

void StripHTML(char *src) {
    if (!src || strlen(src) == 0) return;

    char *read = src;
    char *write = src;
    while (*read) {
        if (*read == '\n' || *read == '\r' || *read == '\t') {
            *write++ = ' ';
            read++;
            continue;
        }

        if ((unsigned char)read[0] == 0xE2 && (unsigned char)read[1] == 0x98 && (unsigned char)read[2] == 0x85) {
            *write++ = '*'; read += 3;
        } else if ((unsigned char)read[0] == 0xE2 && (unsigned char)read[1] == 0x98 && (unsigned char)read[2] == 0x86) {
            *write++ = '-'; read += 3;
        } 
        else if (*read == '&') {
            if (strncmp(read, "&lt;", 4) == 0) { *write++ = '<'; read += 4; }
            else if (strncmp(read, "&gt;", 4) == 0) { *write++ = '>'; read += 4; }
            else if (strncmp(read, "&amp;", 5) == 0) { *write++ = '&'; read += 5; }
            else if (strncmp(read, "&quot;", 6) == 0) { *write++ = '"'; read += 6; }
            else if (strncmp(read, "&nbsp;", 6) == 0) { *write++ = ' '; read += 6; }
            else if (strncmp(read, "&mdash;", 7) == 0) { *write++ = '-'; *write++ = '-'; read += 7; }
            else if (strncmp(read, "&ndash;", 7) == 0) { *write++ = '-'; read += 7; }
            else if (strncmp(read, "&ldquo;", 7) == 0) { *write++ = '"'; read += 7; }
            else if (strncmp(read, "&rdquo;", 7) == 0) { *write++ = '"'; read += 7; }
            else if (strncmp(read, "&lsquo;", 7) == 0) { *write++ = '\''; read += 7; }
            else if (strncmp(read, "&rsquo;", 7) == 0) { *write++ = '\''; read += 7; }
            else if (strncmp(read, "&hellip;", 8) == 0) { *write++ = '.'; *write++ = '.'; *write++ = '.'; read += 8; }
            else if (strncmp(read, "&#9733;", 7) == 0) { *write++ = '*'; read += 7; } 
            else if (strncmp(read, "&#9734;", 7) == 0) { *write++ = '-'; read += 7; } 
            else if (strncmp(read, "&#", 2) == 0) {
                int num = 0; char *temp = read + 2; int digits = 0;
                while (*temp >= '0' && *temp <= '9' && digits < 5) { num = num * 10 + (*temp - '0'); temp++; digits++; }
                if (digits > 0) {
                    if (num == 9733) { *write++ = '*'; }
                    else if (num == 9734) { *write++ = '-'; }
                    else if (num == 8212) { *write++ = '-'; *write++ = '-'; }
                    else if (num == 8211) { *write++ = '-'; }
                    else if (num == 8220 || num == 8221) { *write++ = '"'; }
                    else if (num == 8216 || num == 8217) { *write++ = '\''; }
                    else if (num == 8230) { *write++ = '.'; *write++ = '.'; *write++ = '.'; }
                    else if (num == 160) { *write++ = ' '; }
                    else if (num >= 32 && num <= 126) { *write++ = (char)num; }
                    if (*temp == ';') read = temp + 1; else read = temp; 
                } else { *write++ = *read++; }
            } else { *write++ = *read++; }
        } else { *write++ = *read++; }
    }
    *write = '\0';
    
    int in_tag = 0; char *dest = src; char *orig = src; 
    while (*src) {
        if (*src == '<') {
            in_tag = 1;
            if (strncasecmp(src, "<br", 3) == 0 || strncasecmp(src, "<p", 2) == 0 || strncasecmp(src, "</p", 3) == 0) {
                *dest++ = '\n';
            }
        } 
        else if (*src == '>') { in_tag = 0; } 
        else if (!in_tag) { *dest++ = *src; }
        src++;
    }
    *dest = '\0';
    
    char *d = orig; int space = 0; int newlines = 0;
    for (char *s = orig; *s; s++) {
        if (*s == ' ') {
            if (!space && newlines == 0) { *d++ = ' '; space = 1; }
        } else if (*s == '\n') {
            if (newlines < 2) { *d++ = '\n'; newlines++; }
            space = 0;
        } else {
            *d++ = *s; space = 0; newlines = 0;
        }
    }
    *d = '\0';

    char *r_star = orig;
    char *w_star = orig;
    int in_stars = 0;
    while (*r_star) {
        if (*r_star == '*' || *r_star == '-') {
            in_stars = 1;
            *w_star++ = *r_star++;
        } else if (in_stars && (*r_star == ' ' || *r_star == '\n' || *r_star == '\r')) {
            char *peek = r_star;
            while (*peek == ' ' || *peek == '\n' || *peek == '\r') peek++;
            if (*peek == '*' || *peek == '-') {
                r_star = peek; 
            } else {
                in_stars = 0;
                *w_star++ = *r_star++;
            }
        } else {
            in_stars = 0;
            *w_star++ = *r_star++;
        }
    }
    *w_star = '\0';

    int len = strlen(orig);
    while (len > 0 && (orig[len - 1] == ' ' || orig[len - 1] == '\n')) { orig[len - 1] = '\0'; len--; }
    char *start = orig;
    while (*start == ' ' || *start == '\n') start++;
    if (start != orig) memmove(orig, start, strlen(start) + 1);
}

void SeparateTags(char *text) {
    if (!text || strlen(text) == 0) return;
    
    const char *keywords[] = {"series:", "summary:", "tags:"};
    const char *separator = "\n\n";
    int sep_len = strlen(separator);

    for (int k = 0; k < 3; k++) {
        char temp[1024] = {0};
        strncpy(temp, text, 1023);
        
        for (int i = 0; temp[i]; i++) {
            temp[i] = tolower(temp[i]);
        }

        char *found = strstr(temp, keywords[k]);

        if (found) {
            char *target = text + (found - temp);
            
            if (target && target != text) {
                int current_len = strlen(text);

                if (current_len + sep_len < 1020) {
                    memmove(target + sep_len, target, strlen(target) + 1);
                    memcpy(target, separator, sep_len);
                }
            }
        }

    }
}

void KbdCallback(char *text) { Repaint(); }

void JumpToPageCallback(char *text) {
    if (!text || strlen(text) == 0) { Repaint(); return; }
    
    int target_absolute = atoi(text);
    if (target_absolute <= 0) { Repaint(); return; }

    int entries_per_page = GetCatalogRows();
    int current_batch_start = current_global_offset + 1;
    int max_local_pages = (entry_count + entries_per_page - 1) / entries_per_page;
    int current_batch_end = current_global_offset + max_local_pages;

    if (target_absolute >= current_batch_start && target_absolute <= current_batch_end) {
        current_page = target_absolute - current_global_offset - 1;
        JIT_DecodePageThumbnails();
        Repaint();
        FetchThumbnailsForPage();
    } 
    else if (target_absolute < current_batch_start) {
        int found_index = -1;
        
        for (int i = page_stack_ptr - 1; i >= 0; i--) {
            if (target_absolute > page_offset_stack[i]) {
                found_index = i;
                break;
            }
        }
        
        if (found_index >= 0) {
            int new_local_page = target_absolute - page_offset_stack[found_index] - 1;
            
            page_stack_ptr = found_index; 
            current_global_offset = page_offset_stack[found_index];
            
            jump_target_page = new_local_page;
            LoadCatalog(page_stack[found_index]);
        } else {
            Message(ICON_INFORMATION, "Jump Failed", "Page could not be found in history.", 2000);
            Repaint();
        }
    } 
    else {
        Message(ICON_INFORMATION, "Unknown Territory", "Cannot jump to future pages without loading them first.", 3000);
        Repaint();
    }
}

void TriggerLibraryRefresh() {
    // EVT_CONFIGCHANGED (154) is only a config-reload notice and never indexes anything, which is why downloads used to need a reboot.
    // EVT_STARTSCAN and scanner.app were each verified to index a new file on their own (Era Lite, 6.8); both are kept because neither reports success, so there is no way to detect which one a given firmware honours.
    SendEventTo(ALLTASKS, EVT_STARTSCAN, 0, 0);
    const char *scanner = (access(SYSTEMSCANNER, F_OK) == 0) ? SYSTEMSCANNER : ((access(USERSCANNER, F_OK) == 0) ? USERSCANNER : NULL);
    int started = 0;
    if (scanner) { char *const args[] = { (char *)scanner, NULL }; started = (NewTask(scanner, args, "scanner", "Scanner", NULL, TASK_BACKGROUND | TASK_SINGLEINSTANCE) >= 0); }
    if (!started) {
        // Legacy path for firmwares that ship no scanner.app.
        if (access("/usr/bin/iv2sh", F_OK) == 0) system("/usr/bin/iv2sh SendEventTo -1 154 &");
        else if (access("/ebrmain/bin/iv2sh", F_OK) == 0) system("/ebrmain/bin/iv2sh SendEventTo -1 154 &");
    }
}

void JIT_DecodePageThumbnails() {
    if (!servers[current_server_index].fetch_thumbs) return;
    int entries_per_page = GetCatalogRows();
    int start = current_page * entries_per_page;
    int end = start + entries_per_page;
    if (end > entry_count) end = entry_count;

    for (int i = start; i < end; i++) {
        if (current_entries[i].is_book && !list_thumbs[i] && strlen(current_entries[i].thumb_url) > 10) {
            char t_path[MAX_STR_LEN];
            snprintf(t_path, sizeof(t_path), "%scache_%lu.jpg", IMAGES_DIR, hash_str(current_entries[i].thumb_url));
            struct stat st;
            if (stat(t_path, &st) == 0) list_thumbs[i] = LoadCoverSTB(t_path);
        }
    }
}

void FetchThumbnailsForPage() {
    if (!servers[current_server_index].fetch_thumbs) return;
    int entries_per_page = GetCatalogRows();
    int start = current_page * entries_per_page;
    int end = start + entries_per_page;
    if (end > entry_count) end = entry_count;

    int hh = sys_height / 14, gap = sys_height / 60;
    int row_h = sys_height / 13;
    int footer_y = sys_height - row_h - 10;
    int list_start_y = hh + gap;
    int list_avail_h = (footer_y - 55) - list_start_y;
    int dynamic_row_h = list_avail_h / entries_per_page;
    int icon_h = dynamic_row_h - 10, book_w = (int)(icon_h / 1.4);

    for (int i = start; i < end; i++) {
        if (list_thumbs[i]) continue;
        if (current_entries[i].is_book && strlen(current_entries[i].thumb_url) > 10) {
            char t_path[MAX_STR_LEN];
            snprintf(t_path, sizeof(t_path), "%scache_%lu.jpg", IMAGES_DIR, hash_str(current_entries[i].thumb_url));

            struct stat st;
            if (stat(t_path, &st) == 0 || DownloadImage(current_entries[i].thumb_url, t_path, servers[current_server_index].user, servers[current_server_index].pass) == 0) {
                list_thumbs[i] = LoadCoverSTB(t_path);
                if (list_thumbs[i]) {
                    int row_index = i - start;
                    int y_pos = list_start_y + (row_index * dynamic_row_h);
                    int icon_y = y_pos + 5, b_x = 20 + ((icon_h - book_w) / 2);
                    DrawSmartBitmap(b_x, icon_y, book_w, icon_h, list_thumbs[i], STRETCH, 1);
                    PartialUpdate(b_x, icon_y, book_w, icon_h);
                }
            }
        }
    }
}

void LoadCatalog(const char *url) {
    if (EnsureNetwork() != 0) {
        if (current_state != STATE_BROWSING) {
            current_state = STATE_SERVER_OPTIONS;
        }
        Repaint();
        return; 
    }

    EnsureDirectories(); LoadStockIcons(); FreeListThumbs(); 

    int is_dark = IsDarkMode();
    int top_bg = is_dark ? 0xDDDDDD : 0x000000;
    int top_fg = is_dark ? 0x000000 : 0xFFFFFF;

    int hh = sys_height / 14; 
    FillArea(0, 0, sys_width, hh, top_bg);
    SetTextFont(36, top_fg);
    DrawTextRect(0, (hh/2)-20, sys_width, 40, "Connecting...", ALIGN_CENTER);
    FullUpdate(); 
    
    strncpy(last_loaded_url, url, MAX_STR_LEN - 1); 
    last_loaded_url[MAX_STR_LEN - 1] = '\0';
    
    struct MemoryStruct chunk;
    if (FetchFeed(last_loaded_url, servers[current_server_index].user, servers[current_server_index].pass, &chunk) == 0) {
        memset(current_entries, 0, sizeof(OPDSEntry) * MAX_ENTRIES);
        entry_count = 0; 
 
        ParseOPDSFeed(chunk.memory, last_loaded_url);
        free(chunk.memory);
 
        StripHTML(current_feed_title);
        for (int i = 0; i < entry_count; i++) {
            StripHTML(current_entries[i].title);
            StripHTML(current_entries[i].author);
            StripHTML(current_entries[i].summary);
            SeparateTags(current_entries[i].summary); 
        }

        current_state = STATE_BROWSING;         
        
        if (jump_target_page >= 0) {
            current_page = jump_target_page;
            int entries_per_page = GetCatalogRows();
            int max_p = (entry_count > 0) ? ((entry_count - 1) / entries_per_page) : 0;
            if (current_page > max_p) current_page = max_p;
            jump_target_page = -1;
        } else if (return_to_last_page) {
            int entries_per_page = GetCatalogRows();
            current_page = (entry_count > 0) ? ((entry_count - 1) / entries_per_page) : 0;
            return_to_last_page = 0;
        } else { 
            current_page = 0; 
        }

        JIT_DecodePageThumbnails();
        Repaint(); 
        FetchThumbnailsForPage();
    } else { 
        entry_count = 0; 
        current_state = STATE_BROWSING;
        Repaint();
    }

    ManageImageCache();
}

void URLEncode(const char *src, char *dest) {
    const char *hex = "0123456789ABCDEF";
    while (*src) {
        if ((*src >= 'a' && *src <= 'z') || (*src >= 'A' && *src <= 'Z') || 
            (*src >= '0' && *src <= '9') || *src == '-' || *src == '_' || *src == '.') {
            *dest++ = *src;
        } else if (*src == ' ') { 
            *dest++ = '%'; *dest++ = '2'; *dest++ = '0'; 
        } else {
            *dest++ = '%'; *dest++ = hex[(*src >> 4) & 15]; *dest++ = hex[*src & 15];
        }
        src++;
    }
    *dest = '\0';
}

static char search_input_buffer[256] = {0};

void PerformSearch(char *text) {
    if (!text || strlen(text) == 0) { Repaint(); return; }

    if (strlen(current_search_url) == 0) {
        Message(ICON_ERROR, "Not Supported", "This OPDS server does not provide a search link.", 2000);
        Repaint(); return;
    }

    if (is_opensearch_url) {
        struct MemoryStruct osd_chunk;
        if (FetchFeed(current_search_url, servers[current_server_index].user, servers[current_server_index].pass, &osd_chunk) == 0) {
            char new_template[MAX_STR_LEN] = {0};
            if (ParseOpenSearch(osd_chunk.memory, current_search_url, new_template) == 0) {
                strncpy(current_search_url, new_template, MAX_STR_LEN - 1);
                current_search_url[MAX_STR_LEN - 1] = '\0';
                is_opensearch_url = 0; 
            }
            free(osd_chunk.memory);
        }
    }

    char encoded_query[512] = {0};
    URLEncode(text, encoded_query);
    char final_url[MAX_STR_LEN * 2] = {0};

    char *template_ptr = strstr(current_search_url, "{searchTerms}");
    if (template_ptr) {
        int prefix_len = template_ptr - current_search_url;
        strncpy(final_url, current_search_url, prefix_len);
        final_url[prefix_len] = '\0';
        strcat(final_url, encoded_query);
        strcat(final_url, template_ptr + 13); 
    } else {
        snprintf(final_url, sizeof(final_url), "%s%squery=%s", 
                 current_search_url, strstr(current_search_url, "?") ? "&" : "?", encoded_query);
    }

    if (strncmp(final_url, "http://", 7) != 0 && strncmp(final_url, "https://", 8) != 0) {
        char absolute_url[MAX_STR_LEN * 2] = {0};
        char base_domain[256] = {0};
        
        const char *proto_end = strstr(servers[current_server_index].url, "://");
        if (proto_end) {
            const char *path_start = strchr(proto_end + 3, '/');
            if (path_start) {
                strncpy(base_domain, servers[current_server_index].url, path_start - servers[current_server_index].url);
            } else {
                strcpy(base_domain, servers[current_server_index].url);
            }
        } else {
            strcpy(base_domain, servers[current_server_index].url);
        }
        
        if (final_url[0] == '/') {
            snprintf(absolute_url, sizeof(absolute_url), "%s%s", base_domain, final_url);
        } else {
            snprintf(absolute_url, sizeof(absolute_url), "%s/%s", base_domain, final_url);
        }
        strcpy(final_url, absolute_url);
    }

    if (nav_stack_ptr < MAX_NAV_STACK) strncpy(nav_stack[nav_stack_ptr++], last_loaded_url, MAX_STR_LEN - 1);
    page_stack_ptr = 0;
    current_global_offset = 0;
    return_to_last_page = 0;

    LoadCatalog(final_url);
}

void DrawMainMenu() {
    SetPanelType(0); nav_stack_ptr = 0; page_stack_ptr = 0; current_global_offset = 0;
    int margin = sys_width / 20, gap = sys_height / 60, hh = sys_height / 14, row_h = sys_height / 12;
    SetTextFont(48, 0x000000); 
    DrawTextRect(0, gap, sys_width, 60, "OPDS Library", ALIGN_CENTER);
    DrawLine(margin, hh, sys_width - margin, hh, 0x000000);
    int y = hh + gap; SetTextFont(36, 0x000000);
    for (int i = 0; i < server_count; i++) {
        char buf[300]; snprintf(buf, sizeof(buf), "%d. %s", i + 1, servers[i].name);
        DrawTextRect(margin * 2, y + (row_h / 4), sys_width - (margin * 4), 50, buf, ALIGN_LEFT);
        DrawLine(margin * 2, y + row_h, sys_width - (margin * 2), y + row_h, 0xAAAAAA);
        y += row_h; 
    }
    y += gap; 
    DrawButton(margin * 2, y, sys_width - (margin * 4), row_h, "+ Add New Server", 0);
    DrawButton(margin * 2, y + row_h + gap, sys_width - (margin * 4), row_h, "Exit Application", 2);

    SetTextFont(24, 0x555555); 
    DrawTextRect(0, sys_height - 40, sys_width, 30, "v1.0", ALIGN_CENTER);
}

void HandleMainMenuTouch(int x, int y) {
    int margin = sys_width / 20, gap = sys_height / 60, hh = sys_height / 14, row_h = sys_height / 12, list_y = hh + gap;
    for (int i = 0; i < server_count; i++) {
        if (y >= list_y && y <= list_y + row_h) {
            FlashArea(margin * 2, list_y, sys_width - (margin * 4), row_h);
            current_server_index = i; 
            strncpy(current_host, servers[i].url, MAX_STR_LEN - 1);
            current_state = STATE_SERVER_OPTIONS; Repaint(); return; 
        } 
        list_y += row_h;
    }
    if (y >= list_y + gap && y <= list_y + row_h + gap) {
        FlashArea(margin * 2, list_y + gap, sys_width - (margin * 4), row_h);
        memset(&temp_server, 0, sizeof(OPDSServer)); 
        temp_server.fetch_thumbs = 1; 
        temp_server.catalog_rows = 10;
        editing_server_index = -1;
        current_state = STATE_SERVER_FORM; Repaint(); return; 
    } 
    if (y >= list_y + row_h + (gap * 2)) {
        FlashArea(margin * 2, list_y + row_h + (gap * 2), sys_width - (margin * 4), row_h);

        Message(ICON_INFORMATION, "Exiting", "Cleaning up...", 0); Repaint();
        ManageImageCache(); TriggerLibraryRefresh(); CloseApp(); 
    }
}

void DrawServerOptions() {
    int gap = sys_height / 40;
    int row_h = sys_height / 12;
    int bw = sys_width - (sys_width / 4);
    int bx = sys_width / 8;
    int m = sys_width / 20;

    SetTextFont(48, 0x000000); 
    DrawTextRect(0, gap, sys_width, 60, servers[current_server_index].name, ALIGN_CENTER);

    int top_y = sys_height / 5;
    DrawButton(bx, top_y, bw, row_h, "Browse Catalog", 0);
    DrawButton(bx, top_y + row_h + gap, bw, row_h, "Edit Details", 0);

    int mid_y = sys_height / 2;
    DrawButton(bx, mid_y, bw, row_h, "Delete Server", 2);

    DrawButton(m, sys_height - 110, sys_width - (m * 2), 90, "Main Menu", 1);
}

void HandleServerOptionsTouch(int x, int y) {
    int gap = sys_height / 40;
    int row_h = sys_height / 12;
    int top_y = sys_height / 5;
    int mid_y = sys_height / 2;
    int bw = sys_width - (sys_width / 4);
    int bx = sys_width / 8;
    int m = sys_width / 20;

    if (y >= top_y && y <= top_y + row_h) {
        FlashArea(bx, top_y, bw, row_h);
        nav_stack_ptr = 0; page_stack_ptr = 0; current_global_offset = 0; current_page = 0; 
        LoadCatalog(servers[current_server_index].url); return; 
    }
    if (y >= top_y + row_h + gap && y <= top_y + (row_h * 2) + gap) {
        FlashArea(bx, top_y + row_h + gap, bw, row_h);
        editing_server_index = current_server_index; memcpy(&temp_server, &servers[current_server_index], sizeof(OPDSServer));
        current_state = STATE_SERVER_FORM; Repaint(); return;
    }
    if (y >= mid_y && y <= mid_y + row_h) {
        FlashArea(bx, mid_y, bw, row_h);
        for (int i = current_server_index; i < server_count - 1; i++) servers[i] = servers[i + 1];
        server_count--; SaveServers(); current_state = STATE_MAIN_MENU; Repaint(); return;
    }
    if (y >= sys_height - 110 && y <= sys_height - 20) {
        FlashArea(m, sys_height - 110, sys_width - (m * 2), 90);
        current_state = STATE_MAIN_MENU; Repaint(); return; 
    }
}

void DrawServerForm() {
    int gap = sys_height / 40, row_h = sys_height / 12, box_x = sys_width / 4, box_w = sys_width - box_x - (sys_width / 10), y = (sys_height / 10);
    SetTextFont(48, 0x000000); 
    DrawTextRect(0, gap, sys_width, 60, (editing_server_index >= 0) ? "Edit Server" : "Add Server", ALIGN_CENTER);
    SetTextFont(36, 0x000000); 
    
    const char *labels[] = {"Name:", "URL:", "User:", "Pass:", "Thumbs:", "Rows:"};
    char *vals[] = {temp_server.name, temp_server.url, temp_server.user, temp_server.pass};
    
    for(int i=0; i<6; i++) {
        DrawTextRect(20, y + (gap / 2), box_x - 30, 40, labels[i], ALIGN_LEFT); 
        if (i < 4) {
            DrawRect(box_x, y, box_w, row_h, 0x000000);
            if (i == 3) {
                char m[256] = {0}; for(size_t j=0; j<strlen(vals[i]); j++) m[j] = '*';
                DrawTextRect(box_x + 15, y + (gap / 2), box_w - 30, 40, m, ALIGN_LEFT);
            } else { DrawTextRect(box_x + 15, y + (gap / 2), box_w - 30, 40, vals[i], ALIGN_LEFT); }
        } else if (i == 4) {
            DrawRect(box_x, y + (row_h / 2) - 20, 40, 40, 0x000000);
            if (temp_server.fetch_thumbs) FillArea(box_x + 5, y + (row_h / 2) - 15, 30, 30, 0x555555); 
        } else if (i == 5) {
            DrawRect(box_x, y, 60, row_h, 0x000000);
            SetTextFont(36, 0x000000);
            DrawTextRect(box_x, y + (row_h / 2) - 20, 60, 40, "-", ALIGN_CENTER);
            
            char r_buf[16]; snprintf(r_buf, sizeof(r_buf), "%d", temp_server.catalog_rows);
            DrawTextRect(box_x + 70, y + (row_h / 2) - 20, box_w - 140, 40, r_buf, ALIGN_CENTER);
            
            DrawRect(box_x + box_w - 60, y, 60, row_h, 0x000000);
            DrawTextRect(box_x + box_w - 60, y + (row_h / 2) - 20, 60, 40, "+", ALIGN_CENTER);
        }
        y += row_h + gap;
    }
    y += gap; 
    DrawButton(20, y, (sys_width / 2) - 40, row_h, "CANCEL", 0);
    DrawButton(sys_width / 2 + 20, y, (sys_width / 2) - 40, row_h, "SAVE", 1);
}

void HandleServerFormTouch(int x, int y) {
    int gap = sys_height / 40, row_h = sys_height / 12, ly = (sys_height / 10);
    int box_x = sys_width / 4, box_w = sys_width - box_x - (sys_width / 10);
    
    if (y >= ly && y <= ly + row_h) { FlashArea(box_x, ly, box_w, row_h); OpenKeyboard("Name", temp_server.name, MAX_STR_LEN - 1, 0, KbdCallback); return; } ly += row_h + gap;
    if (y >= ly && y <= ly + row_h) { FlashArea(box_x, ly, box_w, row_h); OpenKeyboard("URL", temp_server.url, MAX_STR_LEN - 1, 0, KbdCallback); return; } ly += row_h + gap;
    if (y >= ly && y <= ly + row_h) { FlashArea(box_x, ly, box_w, row_h); OpenKeyboard("User", temp_server.user, MAX_STR_LEN - 1, 0, KbdCallback); return; } ly += row_h + gap;
    if (y >= ly && y <= ly + row_h) { FlashArea(box_x, ly, box_w, row_h); OpenKeyboard("Pass", temp_server.pass, MAX_STR_LEN - 1, 0, KbdCallback); return; } ly += row_h + gap;
    
    if (y >= ly && y <= ly + row_h) { FlashArea(box_x, ly + (row_h / 2) - 20, 40, 40); temp_server.fetch_thumbs = !temp_server.fetch_thumbs; Repaint(); return; } ly += row_h + gap;
    
    if (y >= ly && y <= ly + row_h) { 
        if (x >= box_x && x <= box_x + 60) {
            FlashArea(box_x, ly, 60, row_h);
            if (temp_server.catalog_rows > 4) temp_server.catalog_rows--; // Lower limit is now 4
            Repaint(); return;
        } else if (x >= box_x + box_w - 60 && x <= box_x + box_w) {
            FlashArea(box_x + box_w - 60, ly, 60, row_h);
            if (temp_server.catalog_rows < 10) temp_server.catalog_rows++;
            Repaint(); return;
        }
    }
    ly += row_h + (gap * 2);

    if (y >= ly && y <= ly + row_h) {
        if (x < sys_width / 2) {
            FlashArea(20, ly, (sys_width / 2) - 40, row_h);
        } else {
            FlashArea(sys_width / 2 + 20, ly, (sys_width / 2) - 40, row_h);
            if (strlen(temp_server.name) > 0) {
                if (editing_server_index >= 0) servers[editing_server_index] = temp_server;
                else if (server_count < MAX_SERVERS) servers[server_count++] = temp_server;
                SaveServers();
            }
        }
        current_state = STATE_MAIN_MENU; Repaint();
    }
}

void DrawBrowsingView() {
    int is_dark = IsDarkMode();
    int top_bg = is_dark ? 0xDDDDDD : 0x000000;
    int top_fg = is_dark ? 0x000000 : 0xFFFFFF;
    int top_sub_fg = is_dark ? 0x555555 : 0xCCCCCC;

    int entries_per_page = GetCatalogRows();
    int hh = sys_height / 14, row_h = sys_height / 13, gap = sys_height / 60;
    int footer_y = sys_height - row_h - 10; 

    FillArea(0, 0, sys_width, hh, top_bg);
    
    int btn_w = sys_width / 9;
    if (btn_w < 100) btn_w = 100; 
    int btn_h = hh / 2, btn_y = hh / 4;

    int back_x = sys_width - btn_w - 10;
    int home_x = back_x - btn_w - 10;
    int search_x = home_x - btn_w - 10;
    int exit_x = 10;
    int title_x = exit_x + btn_w + 20, title_w = search_x - title_x - 20;

    SetTextFont(24, top_sub_fg);
    const char *display_title = (strlen(current_feed_title) > 0) ? current_feed_title : last_loaded_url;
    DrawTextRect(title_x, (hh / 2) + 5, title_w, 30, display_title, ALIGN_LEFT | DOTS);

    SetTextFont(36, top_fg); 
    DrawTextRect(title_x, hh / 6, title_w, 40, "Catalog", ALIGN_LEFT | DOTS);

    DrawButton(search_x, btn_y, btn_w, btn_h, "SEARCH", 0);
    DrawButton(home_x, btn_y, btn_w, btn_h, "HOME", 0); 
    DrawButton(back_x, btn_y, btn_w, btn_h, "BACK", 0); 
    DrawButton(exit_x, btn_y, btn_w, btn_h, "EXIT", 2);

    int list_start_y = hh + gap;
    int list_avail_h = (footer_y - 55) - list_start_y;
    int dynamic_row_h = list_avail_h / entries_per_page;
    int y = list_start_y; 
    
    if (entry_count == 0) { 
        SetTextFont(36, 0x000000);
        DrawTextRect(0, sys_height / 3, sys_width, 60, "No items found.", ALIGN_CENTER); 
    } else {
        int start = current_page * entries_per_page, end = start + entries_per_page;
        if (end > entry_count) end = entry_count;
        
        // Calculate font sizes using a dampened scale so they don't blow up at low rows
        float scale = (float)dynamic_row_h / (float)row_h;
        float font_scale = 1.0f + ((scale - 1.0f) * 0.4f);
        
        int title_font_size = (int)(36 * font_scale);
        if (title_font_size > 44) title_font_size = 44; // Cap the maximum font size
        
        int author_font_size = (int)(24 * font_scale);
        if (author_font_size > 32) author_font_size = 32; // Cap the maximum font size

        int icon_h = dynamic_row_h - 10, book_w = (int)(icon_h / 1.4); 
        int text_x = 20 + icon_h + 15, text_w = sys_width - text_x - 20;

        for (int i = start; i < end; i++) {
            if (y + dynamic_row_h > footer_y - 55) break;
            int icon_y = y + 5;

            if (current_entries[i].is_book) {
                int b_x = 20 + ((icon_h - book_w) / 2);
                if (servers[current_server_index].fetch_thumbs && list_thumbs[i]) DrawSmartBitmap(b_x, icon_y, book_w, icon_h, list_thumbs[i], STRETCH, 1);
                else if (book_fallback_bmp) DrawSmartBitmap(b_x, icon_y, book_w, icon_h, book_fallback_bmp, STRETCH, 0);
                else { FillArea(b_x, icon_y, book_w, icon_h, 0xDDDDDD); DrawRect(b_x, icon_y, book_w, icon_h, 0x000000); }
            } else {
                if (folder_icon_bmp) {
                    int folder_w = (icon_h * folder_icon_bmp->width) / folder_icon_bmp->height;
                    int f_x = 20 + ((icon_h - folder_w) / 2); 
                    DrawSmartBitmap(f_x, icon_y, folder_w, icon_h, folder_icon_bmp, STRETCH, 0);
                } else { 
                    FillArea(20, icon_y, icon_h, icon_h, 0x999999); DrawRect(20, icon_y, icon_h, icon_h, 0x000000); 
                }
            }

            if (current_entries[i].is_book) {
                SetTextFont(title_font_size, 0x000000); 
                
                // Allow the title to dynamically wrap up to a maximum height
                int max_title_h = dynamic_row_h - author_font_size - 25; // Reserve room at the bottom for the author
                int actual_title_h = TextRectHeight(text_w, current_entries[i].title, ALIGN_LEFT);
                if (actual_title_h > max_title_h) actual_title_h = max_title_h;
                
                // Draw wrapped title, adding DOTS only if it hits the maximum allowed height
                DrawTextRect(text_x, y + 10, text_w, actual_title_h, current_entries[i].title, ALIGN_LEFT | DOTS);
                
                if (strlen(current_entries[i].author) > 0) { 
                    SetTextFont(author_font_size, 0x555555); 
                    // Pin author right below the title text
                    DrawTextRect(text_x, y + 10 + actual_title_h + 5, text_w, author_font_size + 10, current_entries[i].author, ALIGN_LEFT | DOTS); 
                }
            } else {
                SetTextFont(title_font_size, 0x000000); 
                
                // For folders, use the entire height to allow deep wrapping
                int max_title_h = dynamic_row_h - 20;
                int actual_title_h = TextRectHeight(text_w, current_entries[i].title, ALIGN_LEFT);
                if (actual_title_h > max_title_h) actual_title_h = max_title_h;
                
                // Vertically center the folder text
                int text_y = y + (dynamic_row_h - actual_title_h) / 2;
                DrawTextRect(text_x, text_y, text_w, actual_title_h, current_entries[i].title, ALIGN_LEFT | DOTS);
            }

            DrawLine(20, y + dynamic_row_h, sys_width - 20, y + dynamic_row_h, 0xAAAAAA);
            y += dynamic_row_h;
        }

        char page_txt[128]; 
        int absolute_page = current_global_offset + current_page + 1;
        int local_max_pages = (entry_count + entries_per_page - 1) / entries_per_page;
        
        if (total_results > 0) {
            int absolute_max_pages = (total_results + entries_per_page - 1) / entries_per_page;
            snprintf(page_txt, sizeof(page_txt), "Page %d / %d   |   Total Catalog: %d", absolute_page, absolute_max_pages, total_results);
        } else {
            int known_max = current_global_offset + local_max_pages;
            if (strlen(next_page_url) == 0) {
                snprintf(page_txt, sizeof(page_txt), "Page %d / %d", absolute_page, known_max);
            } else {
                snprintf(page_txt, sizeof(page_txt), "Page %d / %d+", absolute_page, known_max);
            }
        }
        
        SetTextFont(24, 0x555555); 
        DrawTextRect(0, footer_y - 45, sys_width, 30, page_txt, ALIGN_CENTER);
        
        if (current_page > 0 || page_stack_ptr > 0) DrawButton(20, footer_y, sys_width/2 - 30, row_h, "<< PREV", 0);
        if (end < entry_count) DrawButton(sys_width/2 + 10, footer_y, sys_width/2 - 30, row_h, "NEXT >>", 0);
        else if (strlen(next_page_url) > 0) DrawButton(sys_width/2 + 10, footer_y, sys_width/2 - 30, row_h, "MORE >>", 0);
    }
}

void HandleHardwareButtons(int key) {
    if (current_state == STATE_BOOK_DETAILS) {
        int scroll_amount = 150, moved = 0;
        if (key == IV_KEY_NEXT || key == 0x207) { summary_scroll_y += scroll_amount; moved = 1; } 
        else if (key == IV_KEY_PREV || key == 0x206) { summary_scroll_y -= scroll_amount; if (summary_scroll_y < 0) summary_scroll_y = 0; moved = 1; }
        
        if (moved) {
            FillArea(view_x, view_y, view_w, view_h, 0xFFFFFF);
            SetTextFont(30, 0x000000); SetClip(view_x, view_y, view_w, view_h);
            DrawTextRect(view_x, view_y - summary_scroll_y, view_w, 2000, current_entries[selected_entry_index].summary, ALIGN_LEFT);
            SetClip(0, 0, sys_width, sys_height); PartialUpdate(view_x, view_y, view_w, view_h);
        }
        return; 
    }

    if (current_state != STATE_BROWSING) return;
    
    int entries_per_page = GetCatalogRows();
    
    if (key == IV_KEY_NEXT || key == 0x207) {
        if ((current_page + 1) * entries_per_page < entry_count) {
            current_page++; JIT_DecodePageThumbnails(); Repaint(); FetchThumbnailsForPage();   
        } else if (strlen(next_page_url) > 0) {
            if (page_stack_ptr < MAX_PAGE_STACK) {
                strncpy(page_stack[page_stack_ptr], last_loaded_url, MAX_STR_LEN - 1);
                page_offset_stack[page_stack_ptr] = current_global_offset;
                page_stack_ptr++;
            }
            current_global_offset += (entry_count + entries_per_page - 1) / entries_per_page;
            LoadCatalog(next_page_url);
        }
    } else if (key == IV_KEY_PREV || key == 0x206) {
        if (current_page > 0) {
            current_page--; JIT_DecodePageThumbnails(); Repaint(); FetchThumbnailsForPage();
        } else if (page_stack_ptr > 0) {
            page_stack_ptr--; 
            return_to_last_page = 1; 
            current_global_offset = page_offset_stack[page_stack_ptr];
            LoadCatalog(page_stack[page_stack_ptr]);
        }
    } else if (key == IV_KEY_MENU) {
        static char page_input_buffer[16] = {0};
        memset(page_input_buffer, 0, sizeof(page_input_buffer));
        OpenKeyboard("Jump to page:", page_input_buffer, 15, 0, JumpToPageCallback);
    }
}

void HandleBrowsingTouch(int x, int y) {
    int entries_per_page = GetCatalogRows();
    int hh = sys_height / 14, row_h = sys_height / 13, gap = sys_height / 60, footer_y = sys_height - row_h - 10;

    if (y <= hh) {
        int btn_w = sys_width / 9; if (btn_w < 100) btn_w = 100;
        int btn_h = hh / 2, btn_y = hh / 4;
        int back_x = sys_width - btn_w - 10, home_x = back_x - btn_w - 10, search_x = home_x - btn_w - 10, exit_x = 10;
        int title_x = exit_x + btn_w + 20, title_w = search_x - title_x - 20;

        if (x >= search_x && x <= search_x + btn_w) {
            FlashArea(search_x, btn_y, btn_w, btn_h);
            memset(search_input_buffer, 0, sizeof(search_input_buffer));
            OpenKeyboard("Search Catalog:", search_input_buffer, 255, 0, PerformSearch);
        } else if (x >= home_x && x <= home_x + btn_w) { 
            FlashArea(home_x, btn_y, btn_w, btn_h);
            nav_stack_ptr = 0; page_stack_ptr = 0; current_global_offset = 0; current_page = 0; LoadCatalog(servers[current_server_index].url); 
        } else if (x >= back_x && x <= back_x + btn_w) { 
            FlashArea(back_x, btn_y, btn_w, btn_h);
            GoBackInHierarchy(); 
        } else if (x >= exit_x && x <= exit_x + btn_w) { 
            FlashArea(exit_x, btn_y, btn_w, btn_h);
            Message(ICON_INFORMATION, "Exiting", "Cleaning up...", 0); Repaint(); ManageImageCache(); TriggerLibraryRefresh(); CloseApp(); 
        } else if (x >= title_x && x <= title_x + title_w) {
            FlashArea(title_x, hh / 6, title_w, 40); 
            nav_stack_ptr = 0; page_stack_ptr = 0; current_global_offset = 0; current_state = STATE_SERVER_OPTIONS; Repaint();
        }
        return;
    }
    
    if (y >= footer_y) {
        if (x < sys_width / 2) { 
            FlashArea(20, footer_y, sys_width/2 - 30, row_h);
            if (current_page > 0) { 
                current_page--; JIT_DecodePageThumbnails(); Repaint(); FetchThumbnailsForPage(); 
            } else if (page_stack_ptr > 0) { 
                page_stack_ptr--; return_to_last_page = 1; current_global_offset = page_offset_stack[page_stack_ptr]; LoadCatalog(page_stack[page_stack_ptr]); 
            }
        } else if (x >= sys_width / 2) {
            FlashArea(sys_width/2 + 10, footer_y, sys_width/2 - 30, row_h);
            if ((current_page + 1) * entries_per_page < entry_count) { 
                current_page++; JIT_DecodePageThumbnails(); Repaint(); FetchThumbnailsForPage(); 
            } else if (strlen(next_page_url) > 0) {
                if (page_stack_ptr < MAX_PAGE_STACK) {
                    strncpy(page_stack[page_stack_ptr], last_loaded_url, MAX_STR_LEN - 1);
                    page_offset_stack[page_stack_ptr] = current_global_offset;
                    page_stack_ptr++;
                }
                current_global_offset += (entry_count + entries_per_page - 1) / entries_per_page;
                LoadCatalog(next_page_url);
            }
        } 
        return;
    }
    
    int list_start_y = hh + gap;
    int list_avail_h = (footer_y - 55) - list_start_y;
    int dynamic_row_h = list_avail_h / entries_per_page;
    int start = current_page * entries_per_page, ly = list_start_y;
    for (int i = start; i < start + entries_per_page && i < entry_count; i++) {
        if (y >= ly && y <= ly + dynamic_row_h) {
            OPDSEntry *e = &current_entries[i];

            // Visual e-ink feedback flash
            FlashArea(0, ly, sys_width, dynamic_row_h);

            if (strlen(e->nav_url) > 0 && e->is_book == 0) {
                if (nav_stack_ptr < MAX_NAV_STACK) strncpy(nav_stack[nav_stack_ptr++], last_loaded_url, MAX_STR_LEN - 1);
                page_stack_ptr = 0; current_global_offset = 0; return_to_last_page = 0; LoadCatalog(e->nav_url);
            } else if (e->is_book) {
                selected_entry_index = i; UnloadGlobalCover();
                if (strlen(e->cover_url) > 10) {
                    char c_path[MAX_STR_LEN]; snprintf(c_path, sizeof(c_path), "%scache_%lu.jpg", IMAGES_DIR, hash_str(e->cover_url));
                    if (EnsureNetwork() == 0) {
                        struct stat st; 
                        if (stat(c_path, &st) == 0 || DownloadImage(e->cover_url, c_path, servers[current_server_index].user, servers[current_server_index].pass) == 0) {
                            current_cover_bmp = LoadCoverSTB(c_path);
                        }
                    }
                }
                summary_scroll_y = 0; current_state = STATE_BOOK_DETAILS; Repaint();
            } 
            return;
        } 
        ly += dynamic_row_h; if (ly + dynamic_row_h > footer_y - 55) break;
    }
}

void DrawBookDetails() {
    OPDSEntry *e = &current_entries[selected_entry_index];
    int m = sys_width / 20, thumb_w = sys_width / 3, thumb_h = (int)(thumb_w * 1.4);
    
    SetTextFont(48, 0x000000); DrawTextRect(m, m, sys_width - (m * 2), 100, e->title, ALIGN_LEFT);
    SetTextFont(36, 0x555555); DrawTextRect(m, m + 110, sys_width - (m * 2), 50, e->author, ALIGN_LEFT);
    
    int thumb_y = m + 180; FillArea(m, thumb_y, thumb_w, thumb_h, 0xFFFFFF); 
    if (current_cover_bmp) { DrawSmartBitmap(m, thumb_y, thumb_w, thumb_h, current_cover_bmp, STRETCH, 1); } 
    else { FillArea(m, thumb_y, thumb_w, thumb_h, 0xDDDDDD); SetTextFont(24, 0x000000); DrawTextRect(m, thumb_y + (thumb_h / 2) - 20, thumb_w, 40, "[NO COVER]", ALIGN_CENTER); }
    
    SetTextFont(30, 0x000000); 
    view_x = m * 2 + thumb_w; view_y = thumb_y; view_w = sys_width - (m * 3 + thumb_w); view_h = thumb_h;
    SetClip(view_x, view_y, view_w, view_h);
    DrawTextRect(view_x, view_y - summary_scroll_y, view_w, 2000, e->summary, ALIGN_LEFT);
    SetClip(0, 0, sys_width, sys_height);
    
    int by = thumb_y + thumb_h + 40;
    for (int i = 0; i < e->format_count; i++) {
        char b[64]; snprintf(b, sizeof(b), "Download %s", e->formats[i].label);
        DrawButton(m, by, sys_width - (m * 2), 90, b, 0); by += 110;
    }
    DrawButton(m, sys_height - 110, sys_width - (m * 2), 90, "Back", 1); 
    SoftUpdate();
}

void HandleBookDetailsTouch(int x, int y) {
    OPDSEntry *e = &current_entries[selected_entry_index];
    int m = sys_width / 20, thumb_w = sys_width / 3, by = m + 180 + (int)(thumb_w * 1.4) + 40; 

    for (int i = 0; i < e->format_count; i++) {
        if (y >= by && y <= by + 90) {
            FlashArea(m, by, sys_width - (m * 2), 90);
            if (EnsureNetwork() != 0) { Repaint(); return; }

            char tmp_fname[MAX_STR_LEN], server_fname[256] = {0}; 
            snprintf(tmp_fname, sizeof(tmp_fname), "dl_%ld.tmp", (long)time(NULL));
            char tmp_path[MAX_STR_LEN]; snprintf(tmp_path, sizeof(tmp_path), "%s%s", BOOKS_DIR, tmp_fname);
            Repaint(); 
            
            int res = DownloadBook(e->formats[i].url, tmp_path, server_fname, servers[current_server_index].user, servers[current_server_index].pass);
            if (res == 0) { 
                if (strlen(server_fname) == 0) snprintf(server_fname, sizeof(server_fname), "%s.%s", e->title, e->formats[i].label);
                for(size_t j=0; server_fname[j]; j++) { if(server_fname[j] == '/' || server_fname[j] == '\\' || server_fname[j] == ':') server_fname[j] = '-'; }
                char final_path[MAX_STR_LEN]; snprintf(final_path, sizeof(final_path), "%s%s", BOOKS_DIR, server_fname);
                rename(tmp_path, final_path); BookReady(final_path); TriggerLibraryRefresh(); Message(ICON_INFORMATION, "Success", "Book saved to library.", 2000); 
            } else { remove(tmp_path); Message(ICON_ERROR, "Failed", "Download error.", 3000); }
            Repaint(); return;
        } by += 110;
    }
    if (y >= sys_height - 110) { 
        FlashArea(m, sys_height - 110, sys_width - (m * 2), 90);
        UnloadGlobalCover(); current_state = STATE_BROWSING; Repaint(); 
    }
}
