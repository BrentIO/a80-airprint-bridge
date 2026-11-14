// Minimal AirPrint IPP server logic (see docs/AIRPRINT.md).
#include "ipp.h"
#include "wifi.h"
#include "mdns.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "config.h"
#include "weblog.h"
#include "urfconv.h"
#include "printer.h"

// ---------------------------------------------------------------- tags
enum {
    G_OPERATION = 0x01, G_JOB = 0x02, G_END = 0x03, G_PRINTER = 0x04,
    T_INTEGER = 0x21, T_BOOLEAN = 0x22, T_ENUM = 0x23, T_DATETIME = 0x31, T_RESOLUTION = 0x32,
    T_RANGE = 0x33, T_BEGCOL = 0x34, T_ENDCOL = 0x37, T_TEXT = 0x41, T_NAME = 0x42, T_KEYWORD = 0x44,
    T_URI = 0x45, T_CHARSET = 0x47, T_LANG = 0x48, T_MIME = 0x49, T_MEMBER = 0x4A,
};

// ---------------------------------------------------------------- growable buffer
typedef struct { uint8_t *p; size_t len, cap; } buf_t;

static void put(buf_t *b, const void *d, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 8192;
        while (cap < b->len + n) cap *= 2;
        b->p = heap_caps_realloc(b->p, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        b->cap = cap;
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
}
static void u8(buf_t *b, uint8_t v) { put(b, &v, 1); }
static void u16(buf_t *b, uint16_t v) { uint8_t x[2] = {v >> 8, v}; put(b, x, 2); }
static void u32(buf_t *b, uint32_t v) { uint8_t x[4] = {v >> 24, v >> 16, v >> 8, v}; put(b, x, 4); }

static void attr(buf_t *b, uint8_t tag, const char *name, const void *val, uint16_t vlen)
{
    u8(b, tag);
    u16(b, strlen(name));
    put(b, name, strlen(name));
    u16(b, vlen);
    put(b, val, vlen);
}
static void a_str(buf_t *b, uint8_t tag, const char *name, const char *s) { attr(b, tag, name, s, strlen(s)); }
static void a_int(buf_t *b, uint8_t tag, const char *name, int32_t v)
{
    uint8_t x[4] = {v >> 24, v >> 16, v >> 8, v};
    attr(b, tag, name, x, 4);
}
static void a_bool(buf_t *b, const char *name, bool v) { uint8_t x = v; attr(b, T_BOOLEAN, name, &x, 1); }
static void a_strs(buf_t *b, uint8_t tag, const char *name, const char *const *v, int n)
{
    for (int i = 0; i < n; i++) a_str(b, tag, i ? "" : name, v[i]);
}
static void a_ints(buf_t *b, uint8_t tag, const char *name, const int *v, int n)
{
    for (int i = 0; i < n; i++) a_int(b, tag, i ? "" : name, v[i]);
}
static void a_res(buf_t *b, const char *name, int dpi)
{
    uint8_t x[9] = {0, 0, dpi >> 8, dpi, 0, 0, dpi >> 8, dpi, 3};
    attr(b, T_RESOLUTION, name, x, 9);
}
static void a_range(buf_t *b, const char *name, int lo, int hi)
{
    uint8_t x[8] = {lo >> 24, lo >> 16, lo >> 8, lo, hi >> 24, hi >> 16, hi >> 8, hi};
    attr(b, T_RANGE, name, x, 8);
}
// collections
static void col_begin(buf_t *b, const char *name) { attr(b, T_BEGCOL, name, NULL, 0); }
static void col_end(buf_t *b) { attr(b, T_ENDCOL, "", NULL, 0); }
static void member(buf_t *b, const char *m) { attr(b, T_MEMBER, "", m, strlen(m)); }
static void m_int(buf_t *b, const char *m, int v) { member(b, m); a_int(b, T_INTEGER, "", v); }
static void m_kw(buf_t *b, const char *m, const char *v) { member(b, m); a_str(b, T_KEYWORD, "", v); }

static void media_size(buf_t *b, const char *name, int w, int h)
{
    col_begin(b, name);
    m_int(b, "x-dimension", w);
    m_int(b, "y-dimension", h);
    col_end(b);
}
// One media-col value. name="" for additional values of a 1setOf.
// Paper type values (A80_MEDIA_TYPE_NAMES in config.h picks what the printer advertises):
//   0 standard PWG keywords: iOS shows its own names for these ("Continuous paper - Short Edge", "Plain Paper")
//   1 free-text names: Apple's CUPS shows an unknown name as "Other Media", not as written
//   2 our own keywords, named by the strings catalog (printer-strings-uri): the iPhone has no built-in name for them
#define MT_ROLL_KW "stationery"
#define MT_FANFOLD_KW "continuous-short"   // PWG: sheets joined along the short edge (fanfold)
#define MT_ROLL_CUSTOM "a80-paper-roll"
#define MT_FANFOLD_CUSTOM "a80-continuous-perforated-letter"
#if A80_MEDIA_TYPE_NAMES == 1
#define MT_ROLL A80_PAPER_ROLL_NAME
#define MT_FANFOLD A80_PAPER_FANFOLD_NAME
#define MT_TAG T_NAME
#elif A80_MEDIA_TYPE_NAMES == 2
#define MT_ROLL MT_ROLL_CUSTOM
#define MT_FANFOLD MT_FANFOLD_CUSTOM
#define MT_TAG T_KEYWORD
#else
#define MT_ROLL MT_ROLL_KW
#define MT_FANFOLD MT_FANFOLD_KW
#define MT_TAG T_KEYWORD
#endif

// A job asks for the roll with our name for it, or (from an iPhone that remembers the old names, URF jobs only) the
// standard keyword. Anything else, including a missing type or Android's "auto"/"stationery" for PWG raster jobs,
// prints on the default continuous paper.
static bool is_roll_type(const char *t, bool pwg)
{
    return !strcmp(t, MT_ROLL_CUSTOM) || !strcmp(t, A80_PAPER_ROLL_NAME) || (!pwg && !strcmp(t, MT_ROLL_KW));
}

static void media_col(buf_t *b, const char *name, const char *key, int w, int h, int margin, const char *type)
{
    col_begin(b, name);
    member(b, "media-size");
    media_size(b, "", w, h);
    m_kw(b, "media-size-name", key);
    m_int(b, "media-top-margin", margin);
    m_int(b, "media-bottom-margin", margin);
    m_int(b, "media-left-margin", margin);
    m_int(b, "media-right-margin", margin);
    m_kw(b, "media-source", "main");
    member(b, "media-type");
    a_str(b, MT_TAG, "", type);
    col_end(b);
}

// ---------------------------------------------------------------- printer + jobs
#define MARGIN 500   // 5 mm default margins so iOS fits photos; 0-margin variants offered too
#define LETTER "na_letter_8.5x11in", 21590, 27940
#define A4 "iso_a4_210x297mm", 21000, 29700
#define PRINTER_URI "ipp://" A80_HOSTNAME ".local:80/ipp/print"

// text/strings catalog (PWG 5100.13 section 9.1): `"attribute.keyword" = "text";` per line
size_t ipp_strings_en(char *out, size_t cap)
{
    int n = snprintf(out, cap,
                     "/* Paper types shown in the print dialog */\n"
                     "\"media-type\" = \"Paper Type\";\n"
                     "\"media-type." MT_FANFOLD_CUSTOM "\" = \"" A80_PAPER_FANFOLD_NAME "\";\n"
                     "\"media-type." MT_ROLL_CUSTOM "\" = \"" A80_PAPER_ROLL_NAME "\";\n"
                     "\"media-type." MT_FANFOLD_KW "\" = \"" A80_PAPER_FANFOLD_NAME "\";\n"
                     "\"media-type." MT_ROLL_KW "\" = \"" A80_PAPER_ROLL_NAME "\";\n");
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

static int p_state = 3;
static char p_reason[32] = "connecting-to-device", p_message[96] = "Printer is off - press its power button";

// What the iPhone can display (step28): battery as a "supply level" gauge, and a one-line status as the
// printer's location (shown under its name in the printer list; also the Bonjour TXT "note").
static int p_battery = -2;          // last battery % read from the printer; -2 = never read (IPP "unknown")
static bool p_batt_low;             // the printer's own low-battery flag (status bit 08)
static char p_note[96] = "Off - press power button";

void ipp_set_battery(int pct, bool low)
{
    p_battery = pct;
    p_batt_low = low;
}

void ipp_set_note(const char *note)
{
    if (!strcmp(p_note, note)) return;
    snprintf(p_note, sizeof p_note, "%s", note);
    mdns_service_txt_item_set("_ipp", "_tcp", "note", p_note);   // re-announced to iPhones
}

void ipp_set_printer_state(int state, const char *reason, const char *message)
{
    p_state = state;
    snprintf(p_reason, sizeof p_reason, "%s", reason);
    snprintf(p_message, sizeof p_message, "%s", message);
}

typedef struct {
    bool fanfold;
    int darkness;   // printer darkness 0 (Draft), 2 (Best), or -1 = automatic per page (Normal, the default)
    int id, state, pages;
    char reasons[32], name[64];
    uint8_t *data;   // owned HTTP body; document starts at data + off
    size_t off, len;
    int64_t created;
} job_t;

#define MAX_JOBS 8
static job_t jobs[MAX_JOBS];
static int next_id = 1;

static job_t *job_find(int id)
{
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].id == id) return &jobs[i];
    return NULL;
}
static job_t *job_new(const char *name)
{
    job_t *j = &jobs[(next_id - 1) % MAX_JOBS];
    free(j->data);
    memset(j, 0, sizeof *j);
    j->id = next_id++;
    j->state = 3;
    j->created = esp_timer_get_time();
    strcpy(j->reasons, "none");
    snprintf(j->name, sizeof j->name, "%s", name && *name ? name : "job");
    return j;
}

void ipp_job_update(int id, int state, const char *reasons, int pages_done)
{
    job_t *j = job_find(id);
    if (!j) return;
    if (j->state == 7 && state != 7) return;   // canceled stays canceled
    j->state = state;
    snprintf(j->reasons, sizeof j->reasons, "%s", reasons);
    if (pages_done >= 0) j->pages = pages_done;
}

bool ipp_job_canceled(int id)
{
    job_t *j = job_find(id);
    return !j || j->state == 7;
}

static void printer_attributes(buf_t *b)
{
    static const char *const kw_csets[] = {"us-ascii", "utf-8"};
    static const char *const kw_jca[] = {"copies", "media", "media-col", "orientation-requested",
                                         "print-color-mode", "print-quality", "sides"};
    static const char *const kw_mcs[] = {"media-bottom-margin", "media-left-margin", "media-right-margin",
                                         "media-size", "media-size-name", "media-source", "media-top-margin",
                                         "media-type"};
    static const char *const kw_media[] = {"na_letter_8.5x11in", "iso_a4_210x297mm"};
    static const char *const kw_vers[] = {"1.1", "2.0"};
    static const char *const kw_scal[] = {"auto", "auto-fit", "fill", "fit", "none"};
    static const char *const kw_urf[] = {"CP1", "IS1", "MT1", "RS300", "V1.4", "W8"};
    static const char *const kw_which[] = {"completed", "not-completed"};
    static const char *const kw_types[] = {MT_FANFOLD, MT_ROLL};
    static const int ops[] = {0x02, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0A, 0x0B, 0x39, 0x3B, 0x3C};
    static const int margins[] = {0, MARGIN};
    static const int quals[] = {4, 5};   // iPhone Quality Normal (automatic) / Best (02); Draft dropped: too light

    a_str(b, T_CHARSET, "charset-configured", "utf-8");
    a_strs(b, T_CHARSET, "charset-supported", kw_csets, 2);
    a_bool(b, "color-supported", false);
    a_str(b, T_KEYWORD, "compression-supported", "none");
    a_int(b, T_INTEGER, "copies-default", 1);
    a_range(b, "copies-supported", 1, 1);
    a_int(b, T_ENUM, "finishings-default", 3);
    a_int(b, T_ENUM, "finishings-supported", 3);
    a_str(b, T_MIME, "document-format-default", "image/urf");
    static const char *const kw_fmts[] = {"image/urf", "image/pwg-raster"};   // iPhone/Mac use URF, Android PWG raster
    a_strs(b, T_MIME, "document-format-supported", kw_fmts, 2);
    a_str(b, T_LANG, "generated-natural-language-supported", "en");
    a_str(b, T_KEYWORD, "identify-actions-default", "sound");
    a_str(b, T_KEYWORD, "identify-actions-supported", "sound");
    a_str(b, T_KEYWORD, "ipp-features-supported", "airprint-1.4");
    a_strs(b, T_KEYWORD, "ipp-versions-supported", kw_vers, 2);
    a_strs(b, T_KEYWORD, "job-creation-attributes-supported", kw_jca, 7);
    a_bool(b, "job-ids-supported", true);
    a_ints(b, T_INTEGER, "media-bottom-margin-supported", margins, 2);
    a_ints(b, T_INTEGER, "media-left-margin-supported", margins, 2);
    a_ints(b, T_INTEGER, "media-right-margin-supported", margins, 2);
    a_ints(b, T_INTEGER, "media-top-margin-supported", margins, 2);
    // Continuous perforated Letter first and the default; the roll is the other choice
    media_col(b, "media-col-database", LETTER, MARGIN, MT_FANFOLD);   // fanfold: Letter only (sheet pitch is calibrated for it)
    media_col(b, "", LETTER, 0, MT_FANFOLD);
    media_col(b, "", LETTER, MARGIN, MT_ROLL);
    media_col(b, "", A4, MARGIN, MT_ROLL);
    media_col(b, "", LETTER, 0, MT_ROLL);
    media_col(b, "", A4, 0, MT_ROLL);
    media_col(b, "media-col-default", LETTER, MARGIN, MT_FANFOLD);
    media_col(b, "media-col-ready", LETTER, MARGIN, MT_FANFOLD);
    a_strs(b, T_KEYWORD, "media-col-supported", kw_mcs, 8);
    a_str(b, T_KEYWORD, "media-default", "na_letter_8.5x11in");
    a_str(b, T_KEYWORD, "media-ready", "na_letter_8.5x11in");
    media_size(b, "media-size-supported", 21590, 27940);
    media_size(b, "", 21000, 29700);
    a_str(b, T_KEYWORD, "media-source-supported", "main");
    a_strs(b, T_KEYWORD, "media-supported", kw_media, 2);
    a_strs(b, MT_TAG, "media-type-supported", kw_types, 2);
    a_str(b, T_KEYWORD, "multiple-document-handling-supported", "separate-documents-uncollated-copies");
    a_bool(b, "multiple-document-jobs-supported", false);
    a_int(b, T_INTEGER, "multiple-operation-time-out", 60);
    a_str(b, T_LANG, "natural-language-configured", "en");
    a_ints(b, T_ENUM, "operations-supported", ops, sizeof ops / sizeof *ops);
    a_int(b, T_ENUM, "orientation-requested-default", 3);
    a_int(b, T_ENUM, "orientation-requested-supported", 3);
    a_str(b, T_KEYWORD, "output-bin-default", "face-up");
    a_str(b, T_KEYWORD, "output-bin-supported", "face-up");
    a_int(b, T_INTEGER, "pages-per-minute", 2);
    a_str(b, T_KEYWORD, "pdl-override-supported", "attempted");
    a_str(b, T_KEYWORD, "print-color-mode-default", "monochrome");
    a_str(b, T_KEYWORD, "print-color-mode-supported", "monochrome");
    a_int(b, T_ENUM, "print-quality-default", 4);   // Normal = automatic darkness
    a_ints(b, T_ENUM, "print-quality-supported", quals, 2);
    a_str(b, T_KEYWORD, "print-scaling-default", "auto");
    a_strs(b, T_KEYWORD, "print-scaling-supported", kw_scal, 5);
    a_str(b, T_TEXT, "printer-device-id", "MFG:aixiqee;MDL:A80;CMD:URF,PWGRaster;");
    a_str(b, T_TEXT, "printer-info", A80_PRINTER_NAME);
    a_bool(b, "printer-is-accepting-jobs", true);
    a_str(b, T_TEXT, "printer-location", p_note);
    // Battery shown as a supply level (iOS: printer options > Supply Levels)
    char supply[128];
    int lvl = p_battery >= 0 ? p_battery : -2;
    snprintf(supply, sizeof supply, "index=1;class=supplyThatIsConsumed;type=toner;unit=percent;maxcapacity=100;"
             "level=%d;colorantname=black;", lvl);
    a_str(b, T_NAME, "marker-colors", "#000000");
    a_int(b, T_INTEGER, "marker-high-levels", 100);
    a_int(b, T_INTEGER, "marker-levels", lvl);
    a_int(b, T_INTEGER, "marker-low-levels", 0);   // "low" comes only from the printer's own flag (below)
    a_str(b, T_NAME, "marker-names", "Battery");
    a_str(b, T_KEYWORD, "marker-types", "toner");
    attr(b, 0x30, "printer-supply", supply, strlen(supply));   // octetString
    a_str(b, T_TEXT, "printer-supply-description", "Battery");
    a_str(b, T_TEXT, "printer-make-and-model", "aixiqee A80");
    a_str(b, T_NAME, "printer-name", A80_PRINTER_NAME);
    char more[48], addr[16];
    wifi_address(addr, sizeof addr);
    snprintf(more, sizeof more, "http://%s/", addr);
    a_str(b, T_URI, "printer-more-info", more);
    // Names for the two paper types (PWG 5100.13 message catalog); the iPhone fetches it from this printer
    char strings[56];
    snprintf(strings, sizeof strings, "http://%s/strings/en.strings", addr);
    static const char *const langs[] = {"en", "en-us"};   // the iPhone may ask for "en-US"; both get the same file
    a_strs(b, T_LANG, "printer-strings-languages-supported", langs, 2);
    a_str(b, T_URI, "printer-strings-uri", strings);
    a_res(b, "printer-resolution-default", 300);
    a_res(b, "printer-resolution-supported", 300);
    a_res(b, "pwg-raster-document-resolution-supported", 300);   // PWG 5102.4: one value, at most 360 dpi
    a_str(b, T_KEYWORD, "pwg-raster-document-sheet-back", "normal");
    a_str(b, T_KEYWORD, "pwg-raster-document-type-supported", "sgray_8");   // 8-bit gray, what Android sends to mono printers
    a_int(b, T_ENUM, "printer-state", p_state);
    if (p_batt_low && strcmp(p_reason, "none")) {
        a_str(b, T_KEYWORD, "printer-state-reasons", p_reason);
        a_str(b, T_KEYWORD, "", "marker-supply-low-warning");
    } else a_str(b, T_KEYWORD, "printer-state-reasons", p_batt_low ? "marker-supply-low-warning" : p_reason);
    a_str(b, T_TEXT, "printer-state-message", p_message);
    a_int(b, T_INTEGER, "printer-up-time", (int)(esp_timer_get_time() / 1000000) + 1);
    a_str(b, T_URI, "printer-uri-supported", PRINTER_URI);
    a_str(b, T_URI, "printer-uuid", "urn:uuid:" A80_UUID);
    a_int(b, T_INTEGER, "queued-job-count", 0);
    a_str(b, T_KEYWORD, "sides-default", "one-sided");
    a_str(b, T_KEYWORD, "sides-supported", "one-sided");
    a_str(b, T_KEYWORD, "uri-authentication-supported", "none");
    a_str(b, T_KEYWORD, "uri-security-supported", "none");
    a_strs(b, T_KEYWORD, "urf-supported", kw_urf, 6);
    a_strs(b, T_KEYWORD, "which-jobs-supported", kw_which, 2);
}

static void job_attributes(buf_t *b, job_t *j)
{
    char uri[96];
    snprintf(uri, sizeof uri, "%s/%d", PRINTER_URI, j->id);
    a_int(b, T_INTEGER, "job-id", j->id);
    a_str(b, T_URI, "job-uri", uri);
    a_int(b, T_ENUM, "job-state", j->state);
    a_str(b, T_KEYWORD, "job-state-reasons", j->reasons);
    a_str(b, T_NAME, "job-name", j->name);
    a_str(b, T_URI, "job-printer-uri", PRINTER_URI);
    a_int(b, T_INTEGER, "job-impressions-completed", j->pages);
    a_int(b, T_INTEGER, "job-media-sheets-completed", j->pages);
    a_int(b, T_INTEGER, "time-at-creation", (int)(j->created / 1000000) + 1);
}

// ---------------------------------------------------------------- request parsing
typedef struct {
    int ver_major, ver_minor, op, req_id;
    bool first_ok, has_uri, last_document;
    int job_id, quality;
    char format[48], job_name[64], which[24], media_type[32];
    // seen in job requests, only to log what each kind of client sends
    char color_mode[24], sides[24], size_name[32], source[24], user[40];
    int copies, orient, size_x, size_y, margin_top;
    size_t doc_off;
} req_t;

static const char *opname(int op)
{
    switch (op) {
    case 0x02: return "Print-Job";
    case 0x04: return "Validate-Job";
    case 0x05: return "Create-Job";
    case 0x06: return "Send-Document";
    case 0x08: return "Cancel-Job";
    case 0x09: return "Get-Job-Attributes";
    case 0x0A: return "Get-Jobs";
    case 0x0B: return "Get-Printer-Attributes";
    case 0x39: return "Cancel-My-Jobs";
    case 0x3B: return "Close-Job";
    case 0x3C: return "Identify-Printer";
    default: return "?";
    }
}

static int parse(const uint8_t *d, size_t n, req_t *r)
{
    memset(r, 0, sizeof *r);
    if (n < 9) return 0;
    r->ver_major = d[0];
    r->ver_minor = d[1];
    r->op = d[2] << 8 | d[3];
    r->req_id = d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7];
    strcpy(r->which, "not-completed");
    size_t i = 8;
    int group = 0, idx = 0, depth = 0;
    bool ended = false;
    char mname[24] = "";
    while (i < n) {
        uint8_t t = d[i++];
        if (t == G_END) { ended = true; break; }
        if (t < 0x10) { group = t; continue; }
        if (i + 2 > n) return 0;
        uint16_t nl = d[i] << 8 | d[i + 1];
        const char *name = (const char *)d + i + 2;
        i += 2 + nl;
        if (i + 2 > n) return 0;
        uint16_t vl = d[i] << 8 | d[i + 1];
        const uint8_t *v = d + i + 2;
        i += 2 + vl;
        if (i > n) return 0;
        if (depth && t == T_MEMBER) {   // member name inside a collection (e.g. media-col)
            snprintf(mname, sizeof mname, "%.*s", vl, (const char *)v);
            continue;
        }
        if (depth && mname[0]) {
            bool txt = t == T_KEYWORD || t == T_NAME || t == T_TEXT;
            int iv = t == T_INTEGER && vl == 4 ? v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3] : 0;
            if (txt && !strcmp(mname, "media-type")) snprintf(r->media_type, sizeof r->media_type, "%.*s", vl, (const char *)v);
            else if (txt && !strcmp(mname, "media-size-name")) snprintf(r->size_name, sizeof r->size_name, "%.*s", vl, (const char *)v);
            else if (txt && !strcmp(mname, "media-source")) snprintf(r->source, sizeof r->source, "%.*s", vl, (const char *)v);
            else if (t == T_INTEGER && !strcmp(mname, "x-dimension")) r->size_x = iv;
            else if (t == T_INTEGER && !strcmp(mname, "y-dimension")) r->size_y = iv;
            else if (t == T_INTEGER && !strcmp(mname, "media-top-margin")) r->margin_top = iv;
            if (t != T_BEGCOL) mname[0] = 0;
        }
        if (t == T_BEGCOL) { depth++; continue; }
        if (t == T_ENDCOL) { depth--; continue; }
        if (depth || !nl) continue;   // only top-level named attributes matter here
#define IS(s) (nl == strlen(s) && !memcmp(name, s, nl))
#define COPY(dst) snprintf(dst, sizeof dst, "%.*s", vl, (const char *)v)
        if (idx == 0) r->first_ok = IS("attributes-charset");
        if (idx == 1) r->first_ok = r->first_ok && IS("attributes-natural-language");
        idx++;
        if (group == G_OPERATION && (IS("printer-uri") || IS("job-uri"))) r->has_uri = true;
        else if (IS("document-format")) COPY(r->format);
        else if (IS("job-name")) COPY(r->job_name);
        else if (IS("which-jobs")) COPY(r->which);
        else if (IS("job-id") && vl == 4) r->job_id = v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3];
        else if (IS("last-document") && vl == 1) r->last_document = v[0];
        else if (IS("print-quality") && vl == 4) r->quality = v[3];
        else if (IS("print-color-mode")) COPY(r->color_mode);
        else if (IS("sides")) COPY(r->sides);
        else if (IS("copies") && vl == 4) r->copies = v[3];
        else if (IS("orientation-requested") && vl == 4) r->orient = v[3];
        else if (IS("requesting-user-name")) COPY(r->user);
    }
    if (!ended) return 0;
    r->doc_off = i;
    return 1;
}

int ipp_header_len(const uint8_t *d, size_t n)
{
    req_t r;
    int k = parse(d, n, &r);
    return k > 0 ? (int)r.doc_off : k;
}

// ---------------------------------------------------------------- transactions
struct ipp_txn {
    req_t r;
    int status;
    job_t *job;
    urfconv_t *conv;
    size_t doc_bytes;
    buf_t groups;   // response groups after the operation group
};

ipp_txn_t *ipp_begin(const uint8_t *hdr, size_t len)
{
    ipp_txn_t *t = heap_caps_calloc(1, sizeof *t, MALLOC_CAP_SPIRAM);
    req_t *r = &t->r;
    parse(hdr, len, r);
    {
        // The iPhone polls printer and job status every 1-3 s: note those once per 30 s with a count, log the rest.
        static int64_t last_poll[2];
        static int polls[2];
        int k = r->op == 0x0B ? 0 : r->op == 0x09 ? 1 : -1;
        int64_t now = esp_timer_get_time();
        static const char *const what[] = {
            [0x02] = "Client sent a print job", [0x04] = "Client asked whether a job would be accepted",
            [0x05] = "Client started a print job", [0x06] = "Client is sending the document",
            [0x08] = "Client cancelled a job", [0x09] = "Client is checking job progress",
            [0x0A] = "Client asked for the job list", [0x0B] = "Client is checking printer status and settings",
            [0x39] = "Client cancelled its jobs", [0x3B] = "Client closed the job",
            [0x3C] = "Client asked the printer to identify itself",
        };
        const char *w = r->op < (int)(sizeof what / sizeof *what) && what[r->op] ? what[r->op] : "Client sent an unknown request";
        if (k < 0) weblog("%s [IPP %s #%d]", w, opname(r->op), r->req_id);
        else if (now - last_poll[k] >= 30000000LL) {
            if (polls[k]) weblog("%s [IPP %s #%d; %d more in the last 30 s not logged]", w, opname(r->op), r->req_id, polls[k]);
            else weblog("%s [IPP %s #%d]", w, opname(r->op), r->req_id);
            last_poll[k] = now;
            polls[k] = 0;
        } else polls[k]++;
    }
    if (r->op == 0x02 || r->op == 0x04 || r->op == 0x05)   // what this client asks for (reference for new kinds of client)
        weblog("Job request from \"%s\": format %s, paper type '%s', size %s (%dx%d), source %s, top margin %d, color %s, "
               "sides %s, quality %d, copies %d, orientation %d", r->user, r->format[0] ? r->format : "(none)", r->media_type,
               r->size_name, r->size_x, r->size_y, r->source, r->margin_top, r->color_mode, r->sides, r->quality, r->copies, r->orient);
    if (r->ver_major < 1 || r->ver_major > 2) t->status = 0x0503;
    else if (r->req_id == 0 || !r->first_ok || !r->has_uri) t->status = 0x0400;
    if (t->status) return t;

    buf_t *g = &t->groups;
    switch (r->op) {
    case 0x0B:   // Get-Printer-Attributes
        u8(g, G_PRINTER);
        printer_attributes(g);
        break;
    case 0x04:   // Validate-Job
        if (r->format[0] && strcmp(r->format, "image/urf") && strcmp(r->format, "image/pwg-raster") &&
            strcmp(r->format, "application/octet-stream"))
            t->status = 0x040A;
        break;
    case 0x02:   // Print-Job
    case 0x05:   // Create-Job
        t->job = job_new(r->job_name);
        // a job that names a paper type gets it; one that doesn't prints as continuous perforated Letter
        // either spelling counts: the name we advertise, or the standard keyword (an iPhone may remember the old one)
        t->job->fanfold = !is_roll_type(r->media_type, !strcmp(r->format, "image/pwg-raster"));
        t->job->darkness = r->quality == 5 ? 2 : -1;   // Best = darkest; Normal = automatic
        // iOS may show Draft even though only Normal and Best are advertised; Draft prints as Normal (user: too light)
        if (r->quality == 3) weblog("Job %d: Draft isn't offered by this printer; printing as Normal", t->job->id);
        weblog("Job %d \"%s\": %s, quality %s [media-type '%s']", t->job->id, t->job->name,
               t->job->fanfold ? A80_PAPER_FANFOLD_NAME : A80_PAPER_ROLL_NAME,
               t->job->darkness == 0 ? "Draft" : t->job->darkness == 2 ? "Best" : "Normal (automatic darkness)", r->media_type);
        if (r->op == 0x02) t->conv = urfconv_new(A80_HEAD_DOTS);
        break;
    case 0x06:   // Send-Document
        t->job = job_find(r->job_id);
        if (!t->job) t->status = 0x0406;
        else t->conv = urfconv_new(A80_HEAD_DOTS);
        break;
    case 0x08:   // Cancel-Job
    case 0x39:   // Cancel-My-Jobs
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].id && jobs[i].state < 7 && (r->op == 0x39 || jobs[i].id == r->job_id)) {
                jobs[i].state = 7;
                strcpy(jobs[i].reasons, "job-canceled-by-user");
            }
        break;
    case 0x09:   // Get-Job-Attributes
        t->job = job_find(r->job_id);
        if (!t->job) t->status = 0x0406;
        break;
    case 0x0A: { // Get-Jobs
        bool completed = !strcmp(r->which, "completed");
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].id && completed == (jobs[i].state >= 7)) {
                u8(g, G_JOB);
                job_attributes(g, &jobs[i]);
            }
        break;
    }
    case 0x3B:   // Close-Job
    case 0x3C:   // Identify-Printer
        break;
    default:
        t->status = 0x0501;
    }
    return t;
}

int ipp_doc(ipp_txn_t *t, const uint8_t *d, size_t n)
{
    if (!t->conv) return 0;   // no document expected: discard
    size_t before = t->doc_bytes;
    t->doc_bytes += n;
    if (t->doc_bytes / (1024 * 1024) != before / (1024 * 1024))
        weblog("Job %d: %u KB of the document received so far", t->job->id, (unsigned)(t->doc_bytes / 1024));
    if (urfconv_feed(t->conv, d, n)) {
        weblog("Job %d: could not read the document: %s", t->job->id, urfconv_error(t->conv));
        return -1;
    }
    return 0;
}

size_t ipp_finish(ipp_txn_t *t, uint8_t **resp)
{
    req_t *r = &t->r;
    job_t *j = t->job;
    if (t->conv) {
        int err = urfconv_finish(t->conv);
        int np = urfconv_page_count(t->conv);
        for (int i = 0; i < np; i++) {
            a80_page_t *p = urfconv_page(t->conv, i);
            size_t black = 0, nb = (size_t)p->bytes_per_row * p->rows;
            for (size_t k = 0; k < nb; k++) black += __builtin_popcount(p->bits[k]);
            weblog("Job %d page %d converted: %u%% black [client image %dx%d at %d dpi, %d-bit -> %dx%d printer dots]",
                   j->id, i + 1, (unsigned)(black * 100 / (nb ? nb * 8 : 1)), p->src_w,
                   p->src_h, p->src_dpi, p->src_bpp, p->width, p->rows);
        }
        weblog("Job %d: document received, %d page(s)%s%s [%u KB]", j->id, np, err ? ", ERROR: " : "",
               err ? urfconv_error(t->conv) : "", (unsigned)(t->doc_bytes / 1024));
        if (err || np == 0) {
            j->state = 8;   // aborted
            strcpy(j->reasons, "document-format-error");
        } else {
            a80_page_t **pages = malloc(np * sizeof *pages);
            for (int i = 0; i < np; i++) pages[i] = urfconv_take_page(t->conv, i);
            printer_enqueue(j->id, pages, np, j->fanfold, j->darkness);
        }
        urfconv_free(t->conv);
    }
    if (j && !t->status) {
        u8(&t->groups, G_JOB);
        job_attributes(&t->groups, j);
    }

    buf_t b = {0};
    bool v_ok = r->ver_major == 1 || r->ver_major == 2;
    u8(&b, v_ok ? r->ver_major : 2);
    u8(&b, v_ok ? r->ver_minor : 0);
    u16(&b, t->status);
    u32(&b, r->req_id);
    u8(&b, G_OPERATION);
    a_str(&b, T_CHARSET, "attributes-charset", "utf-8");
    a_str(&b, T_LANG, "attributes-natural-language", "en");
    if (t->groups.len) put(&b, t->groups.p, t->groups.len);
    u8(&b, G_END);
    if (t->status) weblog("Request refused: %s [IPP status 0x%04x]",
                          t->status == 0x0400 ? "malformed request" : t->status == 0x0406 ? "no such job"
                          : t->status == 0x040A ? "document format not supported" : t->status == 0x0501 ? "operation not supported"
                          : t->status == 0x0503 ? "IPP version not supported" : "error", t->status);
    free(t->groups.p);
    free(t);
    *resp = b.p;
    return b.len;
}
