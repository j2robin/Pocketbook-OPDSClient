#ifndef OPDS_APP_H
#define OPDS_APP_H

#include <inkview.h>
#include <stddef.h> 

// libxml2 headers for OPDS parsing
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

// --- Constants ---
#define MAX_SERVERS 10
#define MAX_STR_LEN 512
#define MAX_ENTRIES 1500
#define MAX_FORMATS 5

#define APP_ROOT_DIR "/mnt/ext1/applications/OPDSClient/"
#define IMAGES_DIR APP_ROOT_DIR "images/"
#define BOOKS_DIR "/mnt/ext1/Downloads/"

// New and Legacy Config File Locations
#define NEW_CFG_FILE APP_ROOT_DIR "opds_client.cfg"
#define LEGACY_CFG_FILE "/mnt/ext1/system/config/opds_client.cfg"

#define FOLDER_ICON IMAGES_DIR "folder.png"
#define BOOK_ICON IMAGES_DIR "book.png"

#define APP_USER_AGENT "PocketBook-OPDS-Expert/1.0 (PocketBook Era; Linux)"

typedef enum AppState {
    STATE_MAIN_MENU,
    STATE_SERVER_OPTIONS,
    STATE_SERVER_FORM,
    STATE_BROWSING,
    STATE_BOOK_DETAILS
} AppState;

typedef struct {
    char name[MAX_STR_LEN];
    char url[MAX_STR_LEN];
    char user[MAX_STR_LEN];
    char pass[MAX_STR_LEN];
    int fetch_thumbs; 
    int catalog_rows;
} OPDSServer;

typedef struct {
    char label[64];
    char url[MAX_STR_LEN];
} BookFormat;

typedef struct {
    char title[MAX_STR_LEN];
    char author[MAX_STR_LEN];
    char summary[1024];
    char cover_url[MAX_STR_LEN];
    char thumb_url[MAX_STR_LEN];
    char nav_url[MAX_STR_LEN];
    char publisher[128];
    char published[64];
    char language[32];
    char category[256];
    BookFormat formats[MAX_FORMATS];
    int format_count;
    int is_book;
} OPDSEntry;

// --- Global Variables (Externs) ---
extern AppState current_state;
extern int server_count;
extern int current_server_index;
extern int editing_server_index;
extern int selected_entry_index;
extern OPDSServer servers[MAX_SERVERS];
extern OPDSServer temp_server;
extern OPDSEntry current_entries[MAX_ENTRIES];
extern int entry_count;
extern char current_host[MAX_STR_LEN];
extern int sys_width, sys_height;
extern char current_search_url[MAX_STR_LEN];
extern char next_page_url[MAX_STR_LEN];
extern char current_feed_title[MAX_STR_LEN];
extern int total_results;

// --- Function Prototypes ---
struct MemoryStruct { char *memory; size_t size; };
int FetchFeed(const char *url, const char *user, const char *pass, struct MemoryStruct *chunk);
int DownloadImage(const char *url, const char *filepath, const char *user, const char *pass);
int DownloadBook(const char *url, const char *tmp_path, char *out_filename, const char *user, const char *pass);

int ParseOPDSFeed(const char *xml_data, const char *base_url);

void DrawMainMenu();
void HandleMainMenuTouch(int x, int y);
void DrawServerOptions();
void HandleServerOptionsTouch(int x, int y);
void DrawServerForm();
void HandleServerFormTouch(int x, int y);
void DrawBrowsingView();
void HandleBrowsingTouch(int x, int y);
void DrawBookDetails();
void HandleBookDetailsTouch(int x, int y);
void HandleHardwareButtons(int key);
void LoadCatalog(const char *url);
void Repaint();
void SaveServers();
void LoadServers();

#endif
