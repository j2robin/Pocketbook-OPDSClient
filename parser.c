#include "opds_app.h"
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/uri.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

char next_page_url[MAX_STR_LEN] = {0};
char current_search_url[MAX_STR_LEN] = {0};
char current_feed_title[MAX_STR_LEN] = {0};
int total_results = 0; 
int is_opensearch_url = 0; 

extern void LogDebug(const char *msg);

// Resolves relative links against the base URL provided by the feed
static void ResolveURL(xmlDocPtr doc, xmlNodePtr node, const char *base_url, const char *href, char *out, int max_len) {
    if (!href || strlen(href) == 0) return;
    
    xmlChar *xml_base = xmlNodeGetBase(doc, node);
    const xmlChar *base_to_use = xml_base ? xml_base : (const xmlChar *)base_url;

    xmlChar *resolved = xmlBuildURI((const xmlChar *)href, base_to_use);
    
    if (resolved) {
        strncpy(out, (char *)resolved, max_len - 1);
        out[max_len - 1] = '\0';
        xmlFree(resolved);
    } else {
        strncpy(out, href, max_len - 1);
        out[max_len - 1] = '\0';
    }
    
    if (xml_base) xmlFree(xml_base);
}

// Parses an OpenSearch Description document to extract the dynamic search template
int ParseOpenSearch(const char *xml_data, const char *base_url, char *template_out) {
    if (!xml_data) return -1;
    xmlDocPtr doc = xmlReadMemory(xml_data, strlen(xml_data), base_url, NULL, XML_PARSE_NOERROR | XML_PARSE_RECOVER);
    if (!doc) return -1;
    xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
    
    xmlXPathObjectPtr obj = xmlXPathEvalExpression((xmlChar*)"//*[local-name()='Url']", ctx);
    int found = 0;
    if (obj && obj->nodesetval) {
        for (int i = 0; i < obj->nodesetval->nodeNr; i++) {
            xmlNodePtr node = obj->nodesetval->nodeTab[i];
            char *type = (char*)xmlGetProp(node, (xmlChar*)"type");
            
            int is_atom = 0;
            if (type) {
                char check[256] = {0};
                strncpy(check, type, 255);
                for(int k = 0; check[k]; k++) check[k] = tolower(check[k]);
                if (strstr(check, "atom+xml")) is_atom = 1;
            }

            if (is_atom) {
                char *tmpl = (char*)xmlGetProp(node, (xmlChar*)"template");
                if (tmpl) {
                    xmlChar *resolved = xmlBuildURI((const xmlChar *)tmpl, (const xmlChar *)base_url);
                    if (resolved) {
                        strncpy(template_out, (char *)resolved, MAX_STR_LEN - 1);
                        xmlFree(resolved);
                    } else {
                        strncpy(template_out, tmpl, MAX_STR_LEN - 1);
                    }
                    template_out[MAX_STR_LEN - 1] = '\0';
                    xmlFree(tmpl);
                    found = 1;
                }
            }
            if (type) xmlFree(type);
            if (found) break;
        }
    }
    if (obj) xmlXPathFreeObject(obj);
    xmlXPathFreeContext(ctx);
    xmlFreeDoc(doc);
    return found ? 0 : -1;
}

// Determines if a parsed link points to a supported eBook format
static int IsSupportedBook(const char *type, const char *href) {
    if (!type && !href) return 0;
    
    char check[MAX_STR_LEN * 2] = {0};
    snprintf(check, sizeof(check), "%s %s", type ? type : "", href ? href : "");
    for(int i=0; check[i]; i++) check[i] = tolower(check[i]);
    
    // Explicitly reject web pages, images, and atom navigation links
    if (strstr(check, "image/")) return 0;
    if (strstr(check, "atom+xml")) return 0;

    // Accept all formats natively supported by PocketBook OS
    if (strstr(check, "epub") || strstr(check, "pdf") || strstr(check, "fb2") || 
        strstr(check, "djvu") || strstr(check, "txt") || strstr(check, "rtf") || 
        strstr(check, "doc") || strstr(check, "mobi") || strstr(check, "prc") || 
        strstr(check, "chm") || strstr(check, "tcr") || strstr(check, "cbz") || 
        strstr(check, "cbr") || strstr(check, "acsm") || strstr(check, "kepub")) {
        return 1;
    }
    return 0;
}

static xmlChar* GetXPathVal(xmlXPathContextPtr ctx, const char *expr) {
    xmlXPathObjectPtr obj = xmlXPathEvalExpression((xmlChar*)expr, ctx);
    if (!obj || !obj->nodesetval || obj->nodesetval->nodeNr == 0) {
        if (obj) xmlXPathFreeObject(obj);
        return NULL;
    }
    xmlNodePtr node = obj->nodesetval->nodeTab[0];
    xmlChar *res = xmlNodeListGetString(node->doc, node->xmlChildrenNode, 1);
    xmlXPathFreeObject(obj);
    return res;
}

static void GetChildNodeText(xmlNodePtr parent, const char *node_name, char *out, int max_len) {
    out[0] = '\0';
    xmlNodePtr child = parent->children;
    while (child) {
        if (child->type == XML_ELEMENT_NODE && strcmp((char*)child->name, node_name) == 0) {
            xmlChar *content = xmlNodeGetContent(child);
            if (content) {
                strncpy(out, (char*)content, max_len - 1); out[max_len - 1] = '\0';
                xmlFree(content); return;
            }
        }
        child = child->next;
    }
}

static void GetAuthorName(xmlNodePtr parent, char *out, int max_len) {
    out[0] = '\0';
    xmlNodePtr child = parent->children;
    while (child) {
        if (child->type == XML_ELEMENT_NODE && (strcmp((char*)child->name, "author") == 0 || strcmp((char*)child->name, "creator") == 0)) {
            xmlNodePtr sub = child->children;
            while (sub) {
                if (sub->type == XML_ELEMENT_NODE && strcmp((char*)sub->name, "name") == 0) {
                    xmlChar *content = xmlNodeGetContent(sub);
                    if (content) {
                        strncpy(out, (char*)content, max_len - 1); out[max_len - 1] = '\0';
                        xmlFree(content); return;
                    }
                }
                sub = sub->next;
            }
            xmlChar *content = xmlNodeGetContent(child);
            if (content) {
                strncpy(out, (char*)content, max_len - 1); out[max_len - 1] = '\0';
                xmlFree(content); return;
            }
        }
        child = child->next;
    }
}

int ParseOPDSFeed(const char *xml_data, const char *base_url) {
    if (!xml_data) return -1;
    
    entry_count = 0;
    next_page_url[0] = '\0';
    current_search_url[0] = '\0';
    current_feed_title[0] = '\0';
    is_opensearch_url = 0; 
    
    size_t size = strlen(xml_data);
    
    xmlDocPtr doc = xmlReadMemory(xml_data, size, base_url, NULL, XML_PARSE_NOERROR | XML_PARSE_RECOVER);
    if (!doc) return -1;
    
    xmlXPathContextPtr ctx = xmlXPathNewContext(doc);

    xmlChar *title = GetXPathVal(ctx, "//*[local-name()='feed']/*[local-name()='title']");
    if (title) { strncpy(current_feed_title, (char*)title, MAX_STR_LEN-1); xmlFree(title); }

    xmlChar *total_res = GetXPathVal(ctx, "//*[local-name()='totalResults']");
    if (total_res) { 
        total_results = atoi((char*)total_res); 
        xmlFree(total_res); 
    } else { 
        total_results = 0; 
    }

    // Parse navigation and pagination links
    xmlXPathObjectPtr link_objs = xmlXPathEvalExpression((xmlChar*)"//*[local-name()='feed']/*[local-name()='link']", ctx);
    if (link_objs && link_objs->nodesetval) {
        for (int i = 0; i < link_objs->nodesetval->nodeNr; i++) {
            xmlNodePtr node = link_objs->nodesetval->nodeTab[i];
            char *rel = (char*)xmlGetProp(node, (xmlChar*)"rel");
            char *href = (char*)xmlGetProp(node, (xmlChar*)"href");
            char *type = (char*)xmlGetProp(node, (xmlChar*)"type");

            if (rel && href) {
                if (strstr(rel, "next")) {
                    ResolveURL(doc, node, base_url, href, next_page_url, MAX_STR_LEN);
                } else if (strstr(rel, "search")) {
                    int is_atom = 0;
                    int is_osd = 0;

                    if (type) {
                        char check[256] = {0};
                        strncpy(check, type, 255);
                        for(int k=0; check[k]; k++) check[k] = tolower(check[k]);
                        
                        if (strstr(check, "atom+xml")) is_atom = 1;
                        if (strstr(check, "opensearch")) is_osd = 1;
                    }

                    if (!current_search_url[0] || is_atom) {
                        ResolveURL(doc, node, base_url, href, current_search_url, MAX_STR_LEN);
                        if (strstr(current_search_url, "osd")) is_osd = 1;
                        is_opensearch_url = is_osd;
                    }
                }
            }
            if (rel) xmlFree(rel); 
            if (href) xmlFree(href);
            if (type) xmlFree(type);
        }
        xmlXPathFreeObject(link_objs);
    }

    // Parse catalog entries (books or folders)
    xmlXPathObjectPtr entries = xmlXPathEvalExpression((xmlChar*)"//*[local-name()='entry']", ctx);
    if (entries && entries->nodesetval) {
        for (int i = 0; i < entries->nodesetval->nodeNr && entry_count < MAX_ENTRIES; i++) {
            xmlNodePtr entry_node = entries->nodesetval->nodeTab[i];
            OPDSEntry *e = &current_entries[entry_count];
            memset(e, 0, sizeof(OPDSEntry));

            GetChildNodeText(entry_node, "title", e->title, MAX_STR_LEN);
            GetAuthorName(entry_node, e->author, MAX_STR_LEN);
            GetChildNodeText(entry_node, "summary", e->summary, 1023);
            if (strlen(e->summary) == 0) {
                GetChildNodeText(entry_node, "content", e->summary, 1023);
            }

            xmlNodePtr child = entry_node->children;
            while (child) {
                if (child->type == XML_ELEMENT_NODE && strcmp((char*)child->name, "link") == 0) {
                    char *rel = (char*)xmlGetProp(child, (xmlChar*)"rel");
                    char *href = (char*)xmlGetProp(child, (xmlChar*)"href");
                    char *type = (char*)xmlGetProp(child, (xmlChar*)"type");
                    char *link_title = (char*)xmlGetProp(child, (xmlChar*)"title");
                    
                    if (href) {
                        if (IsSupportedBook(type, href) && e->format_count < MAX_FORMATS) {
                            ResolveURL(doc, child, base_url, href, e->formats[e->format_count].url, MAX_STR_LEN);
                            
                            // Prioritize the human-readable title provided by the server
                            if (link_title && strlen(link_title) > 0) {
                                int max_len = sizeof(e->formats[e->format_count].label) - 1;
                                strncpy(e->formats[e->format_count].label, link_title, max_len);
                                e->formats[e->format_count].label[max_len] = '\0';
                            } 
                            // Fallback: Infer the format from the file extension or MIME type
                            else {
                                char check[MAX_STR_LEN * 2];
                                snprintf(check, sizeof(check), "%s %s", type ? type : "", href);
                                for(int k=0; check[k]; k++) check[k] = tolower(check[k]);

                                if (strstr(check, "kepub")) strcpy(e->formats[e->format_count].label, "KEPUB");
                                else if (strstr(check, "epub")) strcpy(e->formats[e->format_count].label, "EPUB");
                                else if (strstr(check, "pdf")) strcpy(e->formats[e->format_count].label, "PDF");
                                else if (strstr(check, "mobi")) strcpy(e->formats[e->format_count].label, "MOBI");
                                else if (strstr(check, "fb2+zip")) strcpy(e->formats[e->format_count].label, "FB2 ZIP");
                                else if (strstr(check, "fb2")) strcpy(e->formats[e->format_count].label, "FB2");
                                else if (strstr(check, "djvu")) strcpy(e->formats[e->format_count].label, "DJVU");
                                else if (strstr(check, "txt")) strcpy(e->formats[e->format_count].label, "TXT");
                                else if (strstr(check, "rtf")) strcpy(e->formats[e->format_count].label, "RTF");
                                else if (strstr(check, "doc")) strcpy(e->formats[e->format_count].label, "DOC");
                                else if (strstr(check, "cbr")) strcpy(e->formats[e->format_count].label, "CBR");
                                else if (strstr(check, "cbz")) strcpy(e->formats[e->format_count].label, "CBZ");
                                else if (strstr(check, "acsm")) strcpy(e->formats[e->format_count].label, "ACSM");
                                else strcpy(e->formats[e->format_count].label, "BOOK");
                            }
                            
                            e->format_count++;
                            e->is_book = 1;
                        } 
                        else if ((type && strstr(type, "atom+xml")) || 
                                 (rel && (strstr(rel, "subsection") || strstr(rel, "start") || strstr(rel, "next")))) {
                            if (strlen(e->nav_url) == 0) {
                                ResolveURL(doc, child, base_url, href, e->nav_url, MAX_STR_LEN);
                            }
                        } 
                        if (rel && strstr(rel, "thumbnail")) {
                            ResolveURL(doc, child, base_url, href, e->thumb_url, MAX_STR_LEN);
                        } 
                        if (rel && strstr(rel, "image")) {
                            ResolveURL(doc, child, base_url, href, e->cover_url, MAX_STR_LEN);
                        }
                    }
                    if (rel) xmlFree(rel); 
                    if (href) xmlFree(href); 
                    if (type) xmlFree(type);
                    if (link_title) xmlFree(link_title); 
                }
                child = child->next;
            }
            
            if (strlen(e->thumb_url) == 0 && strlen(e->cover_url) > 0) strcpy(e->thumb_url, e->cover_url);
            if (strlen(e->cover_url) == 0 && strlen(e->thumb_url) > 0) strcpy(e->cover_url, e->thumb_url);
            
            if (strlen(e->title) > 0) {
                entry_count++;
            }
        }
        xmlXPathFreeObject(entries);
    }

    xmlXPathFreeContext(ctx);
    xmlFreeDoc(doc);
    return 0;
}
