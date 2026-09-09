/* The DNS responder behind the captive portal. See captive_dns.h for why.
 *
 * A DNS message, which is all this file needs to know:
 *
 *   bytes  0-1   id            copied back unchanged, so the client can match
 *                              the answer to its question
 *   bytes  2-3   flags         we replace these with "this is a reply"
 *   bytes  4-5   qdcount       number of questions, always 1 in practice
 *   bytes  6-7   ancount       answers; 0 in a query, 1 in our reply
 *   bytes  8-11  nscount, arcount
 *   byte  12..   the question: a name, then 2 bytes of type, 2 of class
 *
 * The name is length-prefixed labels rather than dots: "captive.apple.com"
 * arrives as 7 'c','a','p','t','i','v','e' 5 'a','p','p','l','e' 3 'c','o','m'
 * 0. We never read the name -- any name gets the same answer -- but we do have
 * to walk it to find where the question ends, because the answer is appended
 * after it.
 *
 * The reply is the query echoed back, plus one answer record that uses a
 * pointer to the name already present rather than repeating it: 0xC00C, where
 * the top two bits mark a pointer and 0x00C is offset 12, the start of the
 * question. That compression is not an optimisation here, it is what keeps
 * this short enough to be obviously correct.
 */

#include <string.h>

#include <lwip/sockets.h>            /* the BSD socket API lwIP provides     */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"               /* IPSTR / IP2STR, for the query log    */

#include "captive_dns.h"

static const char *TAG = "captive_dns";

#define DNS_PORT        53
#define DNS_HEADER_LEN  12

/* Bigger than any question a phone's probe will ask, and small enough that a
 * malformed jumbo packet is rejected by the read rather than by us. Standard
 * DNS over UDP caps at 512 bytes anyway. */
#define DNS_BUF_LEN     512

static int              s_sock = -1;
static uint32_t         s_answered;      /* queries answered, for the log */
static TaskHandle_t     s_task;
static volatile bool    s_stop;
static uint32_t         s_answer_ip;

/* Walk the length-prefixed labels and return the offset just past the name,
 * or -1 if it runs off the end of the packet.
 *
 * The bounds check is the whole function. This parses data straight off a
 * network socket, so a length byte claiming 200 characters in a 40-byte packet
 * is a thing that can arrive -- from a broken client, a fuzzer, or a bored
 * neighbour -- and following it blindly would read past the buffer. */
static int skip_name(const uint8_t *buf, int len, int pos)
{
    while (pos < len) {
        uint8_t label = buf[pos];

        if (label == 0) {
            return pos + 1;                  /* the root label ends the name */
        }
        if ((label & 0xC0) == 0xC0) {
            /* A compression pointer, two bytes, and the end of the name. A
             * query has no reason to use one, but answering rather than
             * choking costs one line. */
            return (pos + 2 <= len) ? pos + 2 : -1;
        }
        if (label > 63) {
            return -1;                       /* not a length, not a pointer */
        }
        pos += 1 + label;
    }
    return -1;
}

static void dns_task(void *arg)
{
    uint8_t             buf[DNS_BUF_LEN];
    struct sockaddr_in  from;

    ESP_LOGI(TAG, "answering every name with the setup address");

    while (!s_stop) {
        socklen_t from_len = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &from_len);

        if (n < 0) {
            /* The socket has a receive timeout so this loop can notice
             * s_stop; a timeout is the normal quiet case, not an error. */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (s_stop) {
                break;                       /* closed under us, on purpose */
            }
            ESP_LOGW(TAG, "recvfrom failed (errno %d) -- stopping", errno);
            break;
        }

        if (n < DNS_HEADER_LEN) {
            continue;                        /* too short to be a question */
        }

        /* Only answer standard queries that ask something. The check is cheap
         * and keeps this from replying to replies, which is how a pair of
         * these could otherwise talk to each other forever. */
        const bool is_response = (buf[2] & 0x80) != 0;
        const int  qdcount     = (buf[4] << 8) | buf[5];
        if (is_response || qdcount != 1) {
            continue;
        }

        int qend = skip_name(buf, n, DNS_HEADER_LEN);
        if (qend < 0 || qend + 4 > n) {
            continue;                        /* truncated or malformed name */
        }

        const int qtype = (buf[qend] << 8) | buf[qend + 1];
        qend += 4;                           /* past type and class */

        /* Only A records get an address. Anything else -- AAAA especially,
         * which phones ask for constantly -- is answered with a reply that
         * carries no records. That is deliberate and it matters: replying to
         * an AAAA query with an A record is malformed, and answering nothing
         * at all leaves the phone waiting on a timeout before it tries IPv4.
         * An empty, correct answer makes it move on immediately. */
        const bool answer_it = (qtype == 1) && (qend + 16 <= DNS_BUF_LEN);

        buf[2] = 0x84;    /* response, authoritative                        */
        buf[3] = 0x00;    /* no error                                       */
        buf[6] = 0x00;
        buf[7] = answer_it ? 0x01 : 0x00;    /* ancount */
        buf[8] = buf[9] = buf[10] = buf[11] = 0x00;   /* no ns, no ar */

        int out_len = qend;

        if (answer_it) {
            uint8_t *a = buf + qend;

            a[0] = 0xC0; a[1] = 0x0C;        /* name: pointer to offset 12   */
            a[2] = 0x00; a[3] = 0x01;        /* type A                       */
            a[4] = 0x00; a[5] = 0x01;        /* class IN                     */

            /* TTL zero. The lie is only true while this hotspot exists, and a
             * cached answer would outlive it -- leaving a phone convinced that
             * some real domain lives at 192.168.4.1 long after the gadget went
             * back to being a station. */
            a[6] = a[7] = a[8] = a[9] = 0x00;

            a[10] = 0x00; a[11] = 0x04;      /* four bytes of address        */
            memcpy(a + 12, &s_answer_ip, 4); /* already in network order     */

            out_len = qend + 16;
        }

        sendto(s_sock, buf, out_len, 0, (struct sockaddr *)&from, from_len);

        /* The first one only, then every fiftieth. Evidence that the phone is
         * actually asking US rather than a resolver it remembered from another
         * network -- which is the difference between "the portal did not pop
         * up" and "the portal was never consulted", and the two have
         * completely different fixes. Logging every query would bury the rest
         * of setup mode: a phone emits dozens a minute. */
        if (s_answered == 0 || (s_answered % 50) == 0) {
            ESP_LOGI(TAG, "answered %u queries (latest type %d from " IPSTR ")",
                     (unsigned)(s_answered + 1), qtype,
                     IP2STR((esp_ip4_addr_t *)&from.sin_addr.s_addr));
        }
        s_answered++;
    }

    ESP_LOGI(TAG, "stopped");

    /* The task owns the socket's lifetime from here: closing it in
     * captive_dns_stop while this loop might still touch it would be a use
     * after free of a file descriptor, which is the kind that gets reused by
     * something else and fails somewhere unrelated. */
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t captive_dns_start(uint32_t ip)
{
    if (s_task != NULL) {
        ESP_LOGW(TAG, "already running");
        return ESP_OK;
    }

    s_answer_ip = ip;
    s_stop      = false;
    s_answered  = 0;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket failed (errno %d)", errno);
        return ESP_FAIL;
    }

    /* A receive timeout, so the loop wakes often enough to see s_stop. Without
     * it recvfrom blocks forever on a quiet network and the task could only be
     * stopped by killing it mid-syscall. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind to port %d failed (errno %d)", DNS_PORT, errno);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    /* 3 KB is comfortable for a loop whose largest local is a 512-byte buffer.
     * Priority 4 sits below usage_task's 5 and above the button's 3: answering
     * a probe promptly is what makes the portal pop up rather than appear a
     * few seconds later, but nothing here outranks the display. */
    if (xTaskCreate(&dns_task, "captive_dns", 3072, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "could not create the dns task");
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void captive_dns_stop(void)
{
    if (s_task == NULL) {
        return;
    }

    /* Ask, then wait. The task closes its own socket and deletes itself; this
     * only has to stop touching it. One second is the receive timeout, so two
     * is a generous ceiling on how long that can take. */
    s_stop = true;
    for (int i = 0; i < 200 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_task != NULL) {
        ESP_LOGW(TAG, "dns task did not stop; leaving it be");
    }
}
