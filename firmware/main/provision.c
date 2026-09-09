/* Setup mode. See provision.h for the contract and what stage 3 does not do.
 *
 * THE IDEA IN ONE PARAGRAPH. A WiFi chip can be a *station* -- a client that
 * joins somebody else's network, which is what this gadget does all day -- or
 * an *access point*, a network of its own that other devices join. It cannot
 * usefully be both here, so setup mode is a deliberate swap: stop being a
 * client, become a two-device network with a phone, serve that phone one HTML
 * form, and reboot back into being a client. Everything in this file is in
 * service of that swap.
 *
 * ---------------------------------------------------------------------------
 * HOW THIS FILE IS ORGANIZED, top to bottom:
 *
 *   1. module state          -- the three file-scope variables, and why they
 *                               need no lock
 *   2. the hotspot's identity -- a recognisable name, and a random password
 *                               that the display's font can actually draw
 *   3. the form              -- the HTML, the escaping, and why the pages are
 *                               streamed rather than built in a buffer
 *   4. reading the submission -- url-decoding, validation, and the reply that
 *                               says plainly that nothing was saved
 *   5. provision_start()     -- the swap itself, in dependency order
 * ---------------------------------------------------------------------------
 *
 * WHICH TASK IS RUNNING WHAT. provision_start() runs on usage_task, the
 * caller. Everything after it -- form_get, save_post, favicon_get -- runs on a
 * task that esp_http_server creates and owns, one request at a time. That is
 * the reason the state below can be shared without a mutex, and the reason a
 * handler must not block for long: while one is running, no other request is
 * being served.
 *
 * What is deliberately absent: any write to NVS (stage 4), any DNS responder
 * (decided against for v1 -- this gadget has a screen and can simply tell you
 * the URL), and any call into gc9a01. The panel belongs to usage_task; this
 * module reports, and lets the caller draw.
 */

#include <string.h>                  /* memcpy, strlen, strncmp, strchr      */
#include <stdio.h>                   /* snprintf, for every bounded copy     */
#include <stdlib.h>                  /* strtol -- see the port check below   */

#include "esp_wifi.h"                /* the radio driver: mode, config, start */
#include "esp_netif.h"               /* the AP interface and its DHCP server  */
#include "esp_event.h"               /* to hear about devices joining the AP  */
#include "esp_mac.h"                 /* esp_read_mac -- the AP's own MAC      */
#include "esp_random.h"              /* the hardware RNG behind the password  */
#include "esp_http_server.h"         /* the web server serving the form; new
                                      * to REQUIRES at this stage, and what
                                      * the stage-0 partition bump paid for  */
#include "esp_log.h"                 /* ESP_LOGI / ESP_LOGW / ESP_LOGE       */

#include "provision.h"

static const char *TAG = "provision";

/* --- 1. module state --------------------------------------------------------
 *
 * All three are written once by provision_start, on the caller's task, before
 * any HTTP handler can run -- the server does not exist until the last step.
 * After that they are read-only, which is what makes them safe to touch from
 * the server's task without a lock. */

static httpd_handle_t s_server;
static esp_netif_t   *s_ap_netif;

/* The values the form is pre-filled with. A COPY, not a pointer to the
 * caller's config: the handlers outlive provision_start's stack frame, and
 * this is the kind of dangling pointer that works perfectly in testing.
 *
 * Note what is copied -- ssid, host, port -- and what is not. The stored WiFi
 * password is not here at all, so no bug in a handler can render it back over
 * the network. That is a structural guarantee rather than a careful habit,
 * which is the only kind worth having with a credential. */
static struct {
    char     ssid[CFG_SSID_CAP];
    char     host[CFG_HOST_CAP];
    uint16_t port;
} s_form;

/* --- 2. the hotspot's identity ------------------------------------------- */

/* The alphabet the random password is drawn from.
 *
 * Two constraints shaped it, and both are real:
 *
 *   - The panel's font covers ASCII 32..90 only, and gc9a01_draw_text maps
 *     lowercase to uppercase. A password containing 'k' would display as 'K'
 *     and be untypeable -- the screen would be lying about a credential, which
 *     is a particularly bad thing for it to lie about.
 *   - Ambiguous glyphs are removed: no O or 0, no I or 1, no S or 5, no B or
 *     8, no Z or 2. Someone is reading this off a small round screen at a
 *     desk, possibly at a bad angle, and a password that fails once is worse
 *     than a password that is two characters shorter.
 */
static const char PASS_ALPHABET[] = "ACDEFGHJKLMNPQRTUVWXY34679";

#define PASS_ALPHABET_LEN  (sizeof(PASS_ALPHABET) - 1)

/* The one thing that could silently break if someone edits the line above.
 *
 * make_password draws five bits at a time, so it can only ever produce indices
 * 0..31. Adding a 33rd character would not be an error, a warning, or a crash
 * -- every character past the 32nd would simply never be chosen, and the
 * password would quietly come from a smaller alphabet than the one written
 * here. That is exactly the kind of weakening nobody notices, so the compiler
 * is asked to notice it instead. */
_Static_assert(PASS_ALPHABET_LEN <= 32,
               "make_password draws 5 bits, so it cannot reach past 32 characters");

static void make_password(char *out, size_t cap)
{
    /* 26 characters is not a power of two, so `random % 26` would make the
     * first six letters slightly likelier than the rest. Rejection sampling
     * avoids that bias without any arithmetic to get wrong: take five bits,
     * discard anything past the end of the alphabet, take five more. About
     * 1.2 draws per character, which is free at this size.
     *
     * esp_random() is the hardware RNG, and is only properly random once a
     * radio has been started. At this call site one always has been -- setup
     * mode is entered from a chip that has been running WiFi since boot. */
    size_t i = 0;
    while (i + 1 < cap) {
        uint32_t bits = esp_random();

        /* Each 32-bit draw carries six usable 5-bit candidates. Using all of
         * them keeps the RNG calls down for no extra complexity. */
        for (int shift = 0; shift <= 25 && i + 1 < cap; shift += 5) {
            uint32_t idx = (bits >> shift) & 0x1F;
            if (idx < PASS_ALPHABET_LEN) {
                out[i++] = PASS_ALPHABET[idx];
            }
        }
    }
    out[i] = '\0';
}

/* "TOKEN-MON-A3F2": a name someone scanning a phone's WiFi list will recognise
 * as this gadget, with the last two bytes of the AP's MAC so that two of them
 * on one desk stay distinguishable.
 *
 * Deriving the NAME from the MAC is fine -- it is a public identifier either
 * way, since the AP broadcasts it in every beacon frame. Deriving the PASSWORD
 * from it would not be, which is the distinction provision.h spells out. */
static void make_ssid(char *out, size_t cap)
{
    uint8_t mac[6] = { 0 };

    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (err != ESP_OK) {
        /* Not fatal: an unsuffixed name is still a usable name. Worth a line,
         * because two gadgets would then be indistinguishable. */
        ESP_LOGW(TAG, "could not read the softap mac (%s) -- unsuffixed ssid",
                 esp_err_to_name(err));
        snprintf(out, cap, "TOKEN-MON");
        return;
    }

    /* Uppercase hex, because the panel's font has no lowercase. */
    snprintf(out, cap, "TOKEN-MON-%02X%02X", mac[4], mac[5]);
}

/* --- 3. the form ---------------------------------------------------------- */

/* Everything is inline: no external stylesheet, no web font, no favicon. A
 * phone joined to this AP has no route to the internet, so every reference to
 * one would be a spinner and a timeout before the form appeared.
 *
 * One rule in here is functional rather than decorative: the inputs are
 * 16px. Mobile Safari zooms the whole page in when you focus a text field
 * whose font is smaller than that, and then does not zoom back out -- so a
 * 14px field turns a four-field form into a panning exercise. Any change to
 * the input font size has to stay at 16 or above.
 *
 * The rest is a dark card on a dark ground, which is only taste, though it
 * does mean the phone and the panel look like the same device. */
static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Token monitor setup</title><style>"
    "body{font:16px system-ui,sans-serif;margin:0;padding:24px 18px;"
    "background:#14161c;color:#e6e8ef}"
    "h1{font-size:20px;margin:0 0 4px}"
    "p.sub{margin:0 0 24px;color:#9aa0b0;font-size:14px}"
    "label{display:block;margin:0 0 16px}"
    "span{display:block;margin-bottom:6px;font-size:13px;color:#9aa0b0}"
    "input{width:100%;box-sizing:border-box;padding:11px;font-size:16px;"
    "border:1px solid #333a48;border-radius:8px;background:#1e222b;color:#e6e8ef}"
    "button{width:100%;padding:13px;font-size:16px;font-weight:600;border:0;"
    "border-radius:8px;background:#2dc85f;color:#0b0d12}"
    "small{color:#767c8c}"
    ".bad{background:#3a1f22;border:1px solid #7a3038;padding:12px;"
    "border-radius:8px;margin-bottom:20px}"
    ".note{background:#1e222b;border:1px solid #333a48;padding:12px;"
    "border-radius:8px;margin-bottom:20px;font-size:14px}"
    "a{color:#7fb0ff}"
    "</style></head><body>";

static const char PAGE_TAIL[] = "</body></html>";

/* HTML-escape into `out`, truncating rather than overrunning.
 *
 * This exists because of one line -- the value="..." attribute the current
 * SSID is pre-filled into. An SSID is arbitrary bytes as far as 802.11 is
 * concerned, so a network genuinely named with a double quote in it would
 * otherwise end the attribute early and lose the rest of the name, which
 * looks exactly like the chip having mangled your settings.
 *
 * It stops early rather than overrunning if `out` fills up. Truncating is an
 * acceptable failure *here specifically*, and only here: the result is a
 * pre-filled form field that visibly shows the wrong thing, on a page whose
 * whole purpose is to let you correct the field before submitting it. Compare
 * that with form_field below, which refuses to truncate at all, because there
 * the value is on its way into a credential. Same operation, opposite call,
 * and the difference is which direction the data is travelling. */
static void html_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;

    for (const char *p = in; *p != '\0'; p++) {
        const char *rep = NULL;

        switch (*p) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case 0x27: rep = "&#39;";  break;   /* a single quote */
            default:   break;
        }

        if (rep != NULL) {
            size_t n = strlen(rep);
            if (o + n >= cap) { break; }
            memcpy(out + o, rep, n);
            o += n;
        } else {
            if (o + 1 >= cap) { break; }
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

/* The worst case is every character becoming "&quot;", six bytes. */
#define ESCAPED_CAP(n)  ((n) * 6 + 1)

/* Both pages below are assembled as a sequence of chunks -- fixed text, then a
 * value, then more fixed text -- rather than snprintf'd into one buffer.
 *
 * That is not a style preference. Escaping can sextuple a string (every
 * character becoming "&quot;"), so an escaped 63-character host is up to 378
 * bytes on its own, and any buffer comfortable enough to hold a page around it
 * is uncomfortable on a 6 KB task stack. The compiler makes the same point
 * with -Wformat-truncation, which IDF builds as an error. Chunks have no
 * buffer to overflow: each piece is sent as it is produced, and the response
 * is complete when the zero-length chunk goes out. */
/* `req` is the request: esp_http_server hands one to every handler, and it is
 * both the thing you read the request from and the thing you write the reply
 * to. Returning ESP_OK means "answered"; returning an error makes the server
 * close the connection, which is only right when there is nothing left to
 * answer with. */
static esp_err_t form_get(httpd_req_t *req)
{
    /* One buffer, reused for each escaped field in turn, sized for the largest
     * of them -- the host, at 63 characters and therefore up to 378 escaped.
     * These live on the server task's stack, which is why provision_start
     * raises it above the 4 KB default. */
    char esc[ESCAPED_CAP(CFG_HOST_CAP)];

    /* "65535" and a NUL is six; eight is the next round number. */
    char port_str[8];

    ESP_LOGI(TAG, "serving the form");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req,
        "<h1>Token monitor</h1>"
        "<p class=\"sub\">WiFi, and where the PC service lives.</p>"
        "<form method=\"post\" action=\"/save\">"
        "<label><span>WiFi network</span>"
        "<input name=\"ssid\" value=\"");

    html_escape(s_form.ssid, esc, sizeof(esc));
    httpd_resp_sendstr_chunk(req, esc);

    httpd_resp_sendstr_chunk(req,
        "\" maxlength=\"32\" required autocapitalize=\"off\" "
        "autocorrect=\"off\" spellcheck=\"false\"></label>");

    /* The password field is ALWAYS empty. The stored one is not in this
     * module's memory to render even by accident -- see s_form. */
    httpd_resp_sendstr_chunk(req,
        "<label><span>WiFi password</span>"
        "<input name=\"pass\" type=\"password\" maxlength=\"64\" "
        "placeholder=\"leave blank to keep current\" "
        "autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\">"
        "<small>Leave blank to keep the current password.</small></label>"
        "<label><span>PC address</span>"
        "<input name=\"host\" value=\"");

    html_escape(s_form.host, esc, sizeof(esc));
    httpd_resp_sendstr_chunk(req, esc);

    httpd_resp_sendstr_chunk(req,
        "\" maxlength=\"63\" required autocapitalize=\"off\" "
        "autocorrect=\"off\" spellcheck=\"false\"></label>"
        "<label><span>PC service port</span>"
        "<input name=\"port\" type=\"number\" min=\"1\" max=\"65535\" required "
        "value=\"");

    snprintf(port_str, sizeof(port_str), "%u", (unsigned)s_form.port);
    httpd_resp_sendstr_chunk(req, port_str);

    httpd_resp_sendstr_chunk(req,
        "\"></label>"
        "<button type=\"submit\">Save</button></form>"
        "<p><small>This build does not save yet &mdash; it reports what it "
        "received, so the form can be checked on its own.</small></p>");
    httpd_resp_sendstr_chunk(req, PAGE_TAIL);

    /* A zero-length chunk is what terminates a chunked response. Without it
     * the phone sits waiting for more of a page that is already complete. */
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* Browsers ask for /favicon.ico on their own, unprompted, and a 404 for it
 * logs two warning lines per page load. Nothing is wrong when that happens,
 * but it is the loudest thing in the log at exactly the moment someone is
 * reading the log to find out whether the form worked. 204 means "nothing
 * here, and that is fine", which is the truth. */
static esp_err_t favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* --- 4. reading the submission -------------------------------------------- */

/* A form POST arrives as application/x-www-form-urlencoded: key=value pairs
 * joined by '&', spaces as '+', and anything else interesting as %XX.
 *
 * esp_http_server ships httpd_query_key_value, which finds the pair but hands
 * back the value still encoded -- so a WiFi password of "a b&c" would arrive
 * as "a+b%26c" and be silently wrong. Hence this pair of functions: find, and
 * decode. */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

/* 0 = found and decoded, -1 = no such field, -2 = value too long for `out`.
 *
 * Three outcomes rather than a bool, because the difference matters to the
 * person filling in the form: "you left it blank" and "that does not fit" need
 * different messages. Truncating silently is not on the menu -- this project
 * has already lost an afternoon to a credential quietly one character short
 * (see the note in token_monitor.c's wifi_start). */
static int form_field(const char *body, const char *key, char *out, size_t cap)
{
    const size_t klen = strlen(key);
    const char  *p    = body;

    /* The loop walks field boundaries, not characters, and that is what makes
     * the match below safe. `p` is only ever at the start of the body or just
     * after an '&', so a key is compared against the beginning of a field and
     * never against the middle of one: searching for "pass" in
     * "bypass=x&pass=y" cannot match the first field, because the comparison
     * happens at the 'b'. Getting this wrong is the classic form-parsing bug,
     * and it would hand back the wrong value rather than fail. */
    while (*p != '\0') {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;

            size_t o = 0;
            while (*p != '\0' && *p != '&') {
                char c = *p++;

                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    /* Reading p[1] is safe even at the end of the string:
                     * the conditional below only reaches p[1] once the first
                     * digit has come back valid, so a '%' that is the last
                     * character of the body never leads to a read past the
                     * NUL. A malformed escape passes through as a literal
                     * '%', which is what a browser would have sent verbatim
                     * anyway. */
                    int hi = hexval(p[0]);
                    int lo = (hi < 0) ? -1 : hexval(p[1]);
                    if (lo >= 0) {
                        c  = (char)((hi << 4) | lo);
                        p += 2;
                    }
                }

                if (o + 1 >= cap) { return -2; }
                out[o++] = c;
            }
            out[o] = '\0';
            return 0;
        }

        const char *amp = strchr(p, '&');
        if (amp == NULL) { break; }
        p = amp + 1;
    }
    return -1;
}

/* Turn a form_field return code into something a person can act on.
 *
 * The static buffer is safe only because esp_http_server runs one handler at a
 * time on one task, and the result is consumed before the handler returns. */
static const char *field_problem(int rc, const char *label)
{
    static char msg[96];

    snprintf(msg, sizeof(msg),
             (rc == -2) ? "%s is too long." : "%s is missing.", label);
    return msg;
}

/* Wrap a ready-made fragment in the shared head and tail and send it.
 *
 * Only the short, fixed-size pages use this -- the error page and, before the
 * escaping problem forced the issue, the confirmation. The form and the
 * confirmation build themselves chunk by chunk instead, because they carry
 * values whose escaped size cannot be bounded comfortably. */
static esp_err_t send_page(httpd_req_t *req, const char *body_html)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, body_html);
    httpd_resp_sendstr_chunk(req, PAGE_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t send_problem(httpd_req_t *req, const char *problem)
{
    char page[512];

    /* 400 rather than a 200 carrying an apology: the request really was
     * malformed, and the status keeps the browser's own reporting honest if
     * the page itself ever fails to render. */
    httpd_resp_set_status(req, "400 Bad Request");
    snprintf(page, sizeof(page),
             "<h1>Not accepted</h1><div class=\"bad\">%s</div>"
             "<p><a href=\"/\">Back to the form</a></p>", problem);

    ESP_LOGW(TAG, "rejected a submission: %s", problem);
    return send_page(req, page);
}

static esp_err_t save_post(httpd_req_t *req)
{
    /* Room for the four fields fully percent-encoded -- every byte becoming
     * %XX is the worst case -- plus the keys. A body larger than this did not
     * come from our own form. */
    char body[768];

    char ssid[CFG_SSID_CAP];
    char pass[CFG_PASS_CAP];
    char host[CFG_HOST_CAP];
    char port_str[8];

    if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) {
        return send_problem(req, "The submission was empty or implausibly large.");
    }

    /* httpd_req_recv can return a short read -- it is one recv() on a socket,
     * not a promise to deliver everything. A filled-in form is about 120 bytes
     * and will nearly always arrive in one piece, and "nearly always" is how
     * this class of bug hides until the day someone has a long password. */
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);

        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;                    /* the client is merely slow */
        }
        if (r <= 0) {
            ESP_LOGW(TAG, "receiving the form body failed (%d)", r);
            return ESP_FAIL;             /* socket gone; nowhere to send a page */
        }
        received += r;
    }
    body[received] = '\0';

    /* Four fields, each checked before the next is looked at, so the first
     * problem is the one reported. Reporting all four at once would read
     * better on a desktop form; on a phone, one clear sentence beats a list. */
    int rc;
    if ((rc = form_field(body, "ssid", ssid, sizeof(ssid))) != 0) {
        return send_problem(req, field_problem(rc, "The WiFi network name"));
    }
    if ((rc = form_field(body, "pass", pass, sizeof(pass))) != 0) {
        return send_problem(req, field_problem(rc, "The WiFi password"));
    }
    if ((rc = form_field(body, "host", host, sizeof(host))) != 0) {
        return send_problem(req, field_problem(rc, "The PC address"));
    }
    if ((rc = form_field(body, "port", port_str, sizeof(port_str))) != 0) {
        return send_problem(req, field_problem(rc, "The port"));
    }

    /* Note which field is NOT checked for emptiness: the password. Blank is a
     * meaningful answer there -- "keep the one already stored" -- and it is
     * also what an open network legitimately has. config_is_complete makes the
     * same exception for the same two reasons. */
    if (ssid[0] == '\0') {
        return send_problem(req, "The WiFi network name cannot be empty.");
    }
    if (host[0] == '\0') {
        return send_problem(req, "The PC address cannot be empty.");
    }

    /* strtol rather than atoi, which cannot tell "0" from "not a number at
     * all" -- and 0 is exactly the value config.h reads as "unset", so a typo
     * would present later as a chip that thinks it was never configured. */
    char *end  = NULL;
    long  port = strtol(port_str, &end, 10);
    if (end == port_str || *end != '\0' || port < 1 || port > 65535) {
        return send_problem(req, "The port must be a number from 1 to 65535.");
    }

    /* The password's LENGTH, never the password itself. Same rule as config.c:
     * a value one character short is a real failure worth being able to see,
     * and the length makes it visible while giving away nothing worth having. */
    ESP_LOGI(TAG, "form submitted:");
    ESP_LOGI(TAG, "  ssid     \"%s\"", ssid);
    ESP_LOGI(TAG, "  password %u chars%s", (unsigned)strlen(pass),
             (pass[0] == '\0') ? " (blank -- would keep the current one)" : "");
    ESP_LOGI(TAG, "  host     \"%s\"", host);
    ESP_LOGI(TAG, "  port     %ld", port);
    ESP_LOGW(TAG, "NOT SAVED -- writing to nvs is stage 4; nothing changed");

    /* The reply says plainly that nothing was saved. A page that said "Saved!"
     * while discarding the values would be the most misleading screen in the
     * project: someone would reboot, find the old settings, and go hunting for
     * a bug in an NVS write that does not exist yet. */
    char esc[ESCAPED_CAP(CFG_HOST_CAP)];
    char num[24];

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req,
        "<h1>Received</h1>"
        "<div class=\"note\"><b>Nothing was saved.</b> This build parses and "
        "reports the form; writing the values is the next stage.</div>"
        "<p>WiFi network: <b>");

    html_escape(ssid, esc, sizeof(esc));
    httpd_resp_sendstr_chunk(req, esc);

    httpd_resp_sendstr_chunk(req, "</b><br>WiFi password: <b>");
    snprintf(num, sizeof(num), "%u characters", (unsigned)strlen(pass));
    httpd_resp_sendstr_chunk(req, num);
    httpd_resp_sendstr_chunk(req, "</b>");
    if (pass[0] == '\0') {
        httpd_resp_sendstr_chunk(req, " (blank &mdash; would keep the current one)");
    }

    httpd_resp_sendstr_chunk(req, "<br>PC address: <b>");
    html_escape(host, esc, sizeof(esc));
    httpd_resp_sendstr_chunk(req, esc);

    httpd_resp_sendstr_chunk(req, "</b><br>Port: <b>");
    snprintf(num, sizeof(num), "%ld", port);
    httpd_resp_sendstr_chunk(req, num);

    httpd_resp_sendstr_chunk(req,
        "</b></p><p><a href=\"/\">Back to the form</a></p>");
    httpd_resp_sendstr_chunk(req, PAGE_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* --- 5. provision_start: the swap itself ------------------------------------------------ */

/* Logged so the serial monitor shows a phone arriving. Without it, "I joined
 * the network but the page will not load" and "I never actually joined" look
 * identical from the desk, and they have completely different fixes. */
static void on_ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "a device joined (aid %d) -- the form is on the screen's url",
                 e->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "a device left (aid %d)", e->aid);
    }
}

/* Undo whatever got as far as starting. Only the failure paths in
 * provision_start use this: the normal way out of setup mode is a reboot. */
static void stop_all(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_wifi_stop();
}

esp_err_t provision_start(const app_config_t *current, provision_info_t *out)
{
    memset(out, 0, sizeof(*out));

    /* The prefill copy, before anything can fail: cheap, and it keeps the
     * "the handlers never see the stored password" property in one obvious
     * place rather than spread across four of them. */
    snprintf(s_form.ssid, sizeof(s_form.ssid), "%s", current->ssid);
    snprintf(s_form.host, sizeof(s_form.host), "%s", current->host);
    s_form.port = current->port;

    make_ssid(out->ssid, sizeof(out->ssid));
    make_password(out->password, sizeof(out->password));

    /* Leave station mode.
     *
     * disconnect() first, so the AP we were on is told we are going rather
     * than left to time the association out; then stop(), which powers the
     * station side down. Both can legitimately fail on a chip that never
     * associated -- an unconfigured one, or one that could not find its
     * network -- and that is not a reason to refuse to enter setup mode.
     * Setup mode is precisely what a chip in that state needs. So: log, and
     * carry on.
     *
     * The caller has already suppressed its reconnect logic (see provision.h),
     * so the STA_DISCONNECTED this emits on the way out goes nowhere. */
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "not associated at entry (%s) -- fine, continuing",
                 esp_err_to_name(err));
    }
    err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
    }

    /* The AP interface, with its own DHCP server -- that is what
     * esp_netif_create_default_wifi_ap sets up, and it is why a phone joining
     * this network gets an address without a line of code here. The station
     * netif created at boot is left alone: it costs a little RAM and nothing
     * else, and destroying it would only add a way for this path to fail. */
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            ESP_LOGE(TAG, "could not create the ap netif");
            return ESP_FAIL;
        }
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &on_ap_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, &on_ap_event, NULL, NULL));

    /* The = { 0 } matters for the same reason it does in wifi_start: it leaves
     * every field we do not set at its default, and the credential arrays
     * NUL-padded after the partial copies below. */
    wifi_config_t ap = { 0 };

    size_t ssid_len = strlen(out->ssid);
    memcpy(ap.ap.ssid, out->ssid, ssid_len);
    ap.ap.ssid_len = (uint8_t)ssid_len;   /* length-counted, not NUL-terminated */
    memcpy(ap.ap.password, out->password, strlen(out->password));

    ap.ap.channel        = 1;
    ap.ap.max_connection = 2;             /* one phone, and a slot spare       */

    /* An open network would let anyone in range repoint the gadget at their
     * own machine, or read the house SSID out of the form. WPA2-PSK with a
     * password shown on the screen costs one more thing to type and closes
     * that off. provision.h has the reasoning for generating it per entry. */
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) { err = esp_wifi_set_config(WIFI_IF_AP, &ap); }
    if (err == ESP_OK) { err = esp_wifi_start(); }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not raise the ap: %s", esp_err_to_name(err));
        stop_all();
        return err;
    }

    /* Ask the interface for its address rather than hardcoding 192.168.4.1.
     * That is what IDF's default AP netif uses today, and this number goes on
     * the screen as an instruction -- so it is the one place it must not be
     * guessed. */
    esp_netif_ip_info_t ip = { 0 };
    if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(out->url, sizeof(out->url), IPSTR, IP2STR(&ip.ip));
    } else {
        snprintf(out->url, sizeof(out->url), "192.168.4.1");
        ESP_LOGW(TAG, "could not read the ap address -- showing the default");
    }

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();

    /* The default 4 KB is uncomfortably close for form_get, which holds an
     * escaped-host buffer and a 512-byte chunk on the stack at the same time.
     * Overflowing it is a canary panic and a reboot -- i.e. dropping out of
     * setup mode with no explanation on the screen -- so this is not the place
     * to economise. */
    hcfg.stack_size       = 6144;
    hcfg.lru_purge_enable = true;   /* a phone that leaves sockets open must
                                     * not be able to lock everyone else out */

    err = httpd_start(&s_server, &hcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        stop_all();
        return err;
    }

    /* Three routes: the form, the thing it posts to, and the icon browsers
     * ask for whether you have one or not.
     *
     * `static` here is habit rather than necessity -- httpd_register_uri_handler
     * copies each struct into the server's own table and strdup's the URI, so
     * plain locals would work. It is worth knowing which of those two it is
     * before copying this pattern somewhere the lifetime does matter. */
    static const httpd_uri_t form_uri = {
        .uri = "/", .method = HTTP_GET, .handler = form_get,
    };
    static const httpd_uri_t save_uri = {
        .uri = "/save", .method = HTTP_POST, .handler = save_post,
    };
    static const httpd_uri_t icon_uri = {
        .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &form_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &save_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &icon_uri));

    ESP_LOGI(TAG, "setup mode: join \"%s\", then open http://%s",
             out->ssid, out->url);

    /* The AP password IS logged, unlike every other credential here. It is
     * random, it lives for five minutes, and it is already printed in large
     * type on a screen that anyone reading this serial console is sitting in
     * front of. USB is also how you would debug a setup mode you cannot get
     * into, which is exactly when this line earns its place. */
    ESP_LOGI(TAG, "ap password: %s", out->password);
    return ESP_OK;
}
