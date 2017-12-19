#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <signal.h>

#include <syslog.h>
#include <sys/time.h>
#include <unistd.h>

#include <libwebsockets.h>
#include <msgpack.h>
#include "sample.h"

#include <queue>
using namespace std;

int can_msg_pack(uint8_t *data, uint32_t len)
{
    unsigned int map_size = 3;
    uint32_t minlen;

    msgpack_sbuffer sbuf;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer pk;
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);
    msgpack_pack_map(&pk, map_size);

    msgpack_pack_str(&pk, 6);
    msgpack_pack_str_body(&pk, "source", 6);
    msgpack_pack_str(&pk, 11);
    msgpack_pack_str_body(&pk, "client-name", 11);

    msgpack_pack_str(&pk, 5);
    msgpack_pack_str_body(&pk, "topic", 5);
    msgpack_pack_str(&pk, 9);
    msgpack_pack_str_body(&pk, "subscribe", 9);

    msgpack_pack_str(&pk, 4);
    msgpack_pack_str_body(&pk, "data", 4);
    msgpack_pack_str(&pk, 16);
    //  msgpack_pack_str_body(&pk, "output.can.0x760", 16);
    msgpack_pack_str_body(&pk, "output.can.0x700", 16);

    minlen = (sbuf.size<len) ? sbuf.size : len;
    memcpy(data, sbuf.data, minlen);

    msgpack_sbuffer_destroy(&sbuf);

    return minlen;
}

void msgpack_object_get(FILE* out, msgpack_object o, can_algo *can)
{
#define CAN_DATA	1
#define CAN_SOURCE	2
#define CAN_TIME	3
#define CAN_TOPIC	4
    static char data_type = 0;

    switch(o.type) {
        case MSGPACK_OBJECT_NIL:
            fprintf(out, "nil");
            break;

        case MSGPACK_OBJECT_BOOLEAN:
            fprintf(out, (o.via.boolean ? "true" : "false"));
            break;

        case MSGPACK_OBJECT_POSITIVE_INTEGER:
#if defined(PRIu64)
            fprintf(out, "%" PRIu64, o.via.u64);
#else
            if (o.via.u64 > ULONG_MAX)
                fprintf(out, "over 4294967295");
            else
                fprintf(out, "%lu", (unsigned long)o.via.u64);
#endif
            break;

        case MSGPACK_OBJECT_NEGATIVE_INTEGER:
#if defined(PRIi64)
            fprintf(out, "%" PRIi64, o.via.i64);
#else
            if (o.via.i64 > LONG_MAX)
                fprintf(out, "over +2147483647");
            else if (o.via.i64 < LONG_MIN)
                fprintf(out, "under -2147483648");
            else
                fprintf(out, "%ld", (signed long)o.via.i64);
#endif
            break;

        case MSGPACK_OBJECT_FLOAT32:
        case MSGPACK_OBJECT_FLOAT64:
            fprintf(out, "%f", o.via.f64);
            break;

        case MSGPACK_OBJECT_STR:
#if 1
            if(!strncmp(o.via.str.ptr, "data", strlen("data")))
            {
                data_type = CAN_DATA;
                break;
            }
            if(!strncmp(o.via.str.ptr, "source", strlen("source")))
            {
                data_type = CAN_SOURCE;
                break;
            }
            if(!strncmp(o.via.str.ptr, "time", strlen("time")))
            {
                data_type = CAN_TIME;
                break;
            }
            if(!strncmp(o.via.str.ptr, "topic", strlen("topic")))
            {
                data_type = CAN_TOPIC;
                break;
            }

#endif
#if 1
            if(data_type == CAN_DATA)
            {	
                data_type = 0;
                //   	fwrite(o.via.str.ptr, o.via.str.size, 1, out);
                memcpy(can->warning, o.via.str.ptr, (o.via.str.size > sizeof(can->warning) ? sizeof(can->warning) : o.via.str.size));

                break;
            }
            else if(data_type == CAN_SOURCE)
            {
                data_type = 0;
                //  	fwrite(o.via.str.ptr, o.via.str.size, 1, out);
                memcpy(can->source, o.via.str.ptr, (o.via.str.size > sizeof(can->source) ? sizeof(can->source) : o.via.str.size));
                break;
            }
            else if(data_type == CAN_TIME)
            {
                data_type = 0;
                //  	fwrite(o.via.str.ptr, o.via.str.size, 1, out);
                memcpy(can->time, o.via.str.ptr, (o.via.str.size > sizeof(can->time) ? sizeof(can->time) : o.via.str.size));
                break;
            }
            else if(data_type == CAN_TOPIC)
            {
                data_type = 0;
                //	fwrite(o.via.str.ptr, o.via.str.size, 1, out);
                memcpy(can->topic, o.via.str.ptr, (o.via.str.size > sizeof(can->topic) ? sizeof(can->topic) : o.via.str.size));
                break;
            }
            else
            {
                break;
            }

#endif
            break;

        case MSGPACK_OBJECT_BIN:
            fprintf(out, "\"");
            //        msgpack_object_bin_print(out, o.via.bin.ptr, o.via.bin.size);
            fprintf(out, "\"");
            break;

        case MSGPACK_OBJECT_EXT:
#if defined(PRIi8)
            fprintf(out, "(ext: %" PRIi8 ")", o.via.ext.type);
#else
            fprintf(out, "(ext: %d)", (int)o.via.ext.type);
#endif
            fprintf(out, "\"");
            //        msgpack_object_bin_print(out, o.via.ext.ptr, o.via.ext.size);
            fprintf(out, "\"");
            break;

        case MSGPACK_OBJECT_ARRAY:
            fprintf(out, "[");
            if(o.via.array.size != 0) {
                msgpack_object* p = o.via.array.ptr;
                msgpack_object* const pend = o.via.array.ptr + o.via.array.size;
                msgpack_object_get(out, *p, NULL);
                ++p;
                for(; p < pend; ++p) {
                    fprintf(out, ", ");
                    msgpack_object_get(out, *p, NULL);
                }
            }
            fprintf(out, "]");
            break;

        case MSGPACK_OBJECT_MAP:
            //        fprintf(out, "{");
            if(o.via.map.size != 0) {
                msgpack_object_kv* p = o.via.map.ptr;
                msgpack_object_kv* const pend = o.via.map.ptr + o.via.map.size;
                msgpack_object_get(out, p->key, NULL);
                //   fprintf(out, "=>");
                msgpack_object_get(out, p->val, can);
                ++p;
                for(; p < pend; ++p) {
                    //  fprintf(out, ", ");
                    msgpack_object_get(out, p->key, NULL);
                    //    fprintf(out, "=>");
                    msgpack_object_get(out, p->val, can);
                }
            }
            //   fprintf(out, "}");
            break;

        default:
            // FIXME
#if defined(PRIu64)
            fprintf(out, "#<UNKNOWN %i %" PRIu64 ">", o.type, o.via.u64);
#else
            if (o.via.u64 > ULONG_MAX)
                fprintf(out, "#<UNKNOWN %i over 4294967295>", o.type);
            else
                fprintf(out, "#<UNKNOWN %i %lu>", o.type, (unsigned long)o.via.u64);
#endif

    }
}

int can_msg_unpack(uint8_t *data, int size)
{
    msgpack_zone mempool;
    msgpack_object deserialized;
    can_algo can;
    static int sendcnt=0;

    msgpack_zone_init(&mempool, 2048);

    msgpack_unpack((const char *)data, size, NULL, &mempool, &deserialized);

    memset(&can, 0, sizeof(can));

    msgpack_object_get(stdout, deserialized, &can);

    msgpack_zone_destroy(&mempool);

    can_message_send(&can);

    return 0;
}


struct lws_poly_gen {
    uint32_t cyc[2];
};

#define block_size (3 * 4096)

static int deny_deflate, longlived, mirror_lifetime, test_post, once;
static struct lws *wsi_dumb, *wsi_mirror;
static struct lws *wsi_multi[3];
static volatile int force_exit;
static unsigned int opts, rl_multi[3];
static int flag_no_mirror_traffic, justmirror, flag_echo;
static uint32_t count_blocks = 1024, txb, rxb, rx_count, errs;
static struct lws_poly_gen tx = { { 0xabcde, 0x23456789 } },
                           rx = { { 0xabcde, 0x23456789 } }
                           ;

#if defined(LWS_OPENSSL_SUPPORT) && defined(LWS_HAVE_SSL_CTX_set1_param)
char crl_path[1024] = "";
#endif

/*
 * This demo shows how to connect multiple websockets simultaneously to a
 * websocket server (there is no restriction on their having to be the same
 * server just it simplifies the demo).
 *
 *  dumb-increment-protocol:  we connect to the server and print the number
 *				we are given
 *
 *  lws-mirror-protocol: draws random circles, which are mirrored on to every
 *				client (see them being drawn in every browser
 *				session also using the test server)
 */

enum demo_protocols {

    PROTOCOL_DUMB_INCREMENT,
    PROTOCOL_LWS_MIRROR,

    /* always last */
    DEMO_PROTOCOL_COUNT
};

    static uint8_t
lws_poly_rand(struct lws_poly_gen *p)
{
    p->cyc[0] = p->cyc[0] & 1 ? (p->cyc[0] >> 1) ^ 0xb4bcd35c :
        p->cyc[0] >> 1;
    p->cyc[0] = p->cyc[0] & 1 ? (p->cyc[0] >> 1) ^ 0xb4bcd35c :
        p->cyc[0] >> 1;
    p->cyc[1] = p->cyc[1] & 1 ? (p->cyc[1] >> 1) ^ 0x7a5bc2e3 :
        p->cyc[1] >> 1;

    return p->cyc[0] ^ p->cyc[1];
}

static void show_http_content(const char *p, size_t l)
{
    if (lwsl_visible(LLL_INFO)) {
        while (l--)
            if (*p < 0x7f)
                putchar(*p++);
            else
                putchar('.');
    }
}


void printbuf(uint8_t *buf, int len)
{
    int i;
#ifdef DEBUG_BUF
    for(i=0; i<len; i++)
    {
        if(i && (i%16==0))
            printf("\n");

        printf("0x%02x ", buf[i]);
    }
    printf("\n");
#endif   
}



/*
 * dumb_increment protocol
 *
 * since this also happens to be protocols[0], some callbacks that are not
 * bound to a specific protocol also turn up here.
 */

    static int
callback_dumb_increment(struct lws *wsi, enum lws_callback_reasons reason,
        void *user, void *in, size_t len)
{
#if defined(LWS_OPENSSL_SUPPORT)
    union lws_tls_cert_info_results ci;
#endif
    const char *which = "http";
    char which_wsi[10], buf[50 + LWS_PRE];
    int n, msgpacklen;
    uint8_t datacmd[100];
    static int sendflag=1;


    //	printf("reason: %d\n", reason);
    //	printf("callback: %d\n", LWS_CALLBACK_CLIENT_ESTABLISHED);

    switch (reason) {

        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            printf("dumb:: LWS_CALLBACK_CLIENT_ESTABLISHED\n");

            lwsl_info("dumb: LWS_CALLBACK_CLIENT_ESTABLISHED\n");
            break;

        case LWS_CALLBACK_CLOSED:
            printf("dumb:: LWS_CALLBACK_CLOSED\n");
            lwsl_notice("dumb: LWS_CALLBACK_CLOSED\n");
            wsi_dumb = NULL;
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:

            //		printf("receive:\n");
            //		printbuf((uint8_t *)in, (int)len);
            can_msg_unpack((uint8_t *)in, (int)len);





            //		((char *)in)[len] = '\0';
            //		lwsl_info("rx %d '%s'\n", (int)len, (char *)in);
            break;

            /* because we are protocols[0] ... */

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            if (wsi == wsi_dumb) {
                which = "dumb";
                wsi_dumb = NULL;
            }
            if (wsi == wsi_mirror) {
                which = "mirror";
                wsi_mirror = NULL;
            }

            for (n = 0; n < (int)ARRAY_SIZE(wsi_multi); n++)
                if (wsi == wsi_multi[n]) {
                    sprintf(which_wsi, "multi %d", n);
                    which = which_wsi;
                    wsi_multi[n] = NULL;
                }

            lwsl_err("CLIENT_CONNECTION_ERROR: %s: %s\n", which,
                    in ? (char *)in : "(null)");
            break;

        case LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED:
            printf("LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED\n");
            if ((strcmp((const char *)in, "deflate-stream") == 0) &&
                    deny_deflate) {
                lwsl_notice("denied deflate-stream extension\n");
                return 1;
            }
            if ((strcmp((const char *)in, "x-webkit-deflate-frame") == 0))
                return 1;
            if ((strcmp((const char *)in, "deflate-frame") == 0))
                return 1;
            break;

        case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
            printf("LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP\n");
            lwsl_notice("lws_http_client_http_response %d\n",
                    lws_http_client_http_response(wsi));
#if defined(LWS_OPENSSL_SUPPORT)
            if (!lws_tls_peer_cert_info(wsi, LWS_TLS_CERT_INFO_COMMON_NAME,
                        &ci, sizeof(ci.ns.name)))
                lwsl_notice(" Peer Cert CN        : %s\n", ci.ns.name);

            if (!lws_tls_peer_cert_info(wsi, LWS_TLS_CERT_INFO_ISSUER_NAME,
                        &ci, sizeof(ci.ns.name)))
                lwsl_notice(" Peer Cert issuer    : %s\n", ci.ns.name);

            if (!lws_tls_peer_cert_info(wsi, LWS_TLS_CERT_INFO_VALIDITY_FROM,
                        &ci, 0))
                lwsl_notice(" Peer Cert Valid from: %s", ctime(&ci.time));

            if (!lws_tls_peer_cert_info(wsi, LWS_TLS_CERT_INFO_VALIDITY_TO,
                        &ci, 0))
                lwsl_notice(" Peer Cert Valid to  : %s", ctime(&ci.time));
            if (!lws_tls_peer_cert_info(wsi, LWS_TLS_CERT_INFO_USAGE,
                        &ci, 0))
                lwsl_notice(" Peer Cert usage bits: 0x%x\n", ci.usage);
#endif
            break;

            /* chunked content */
        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ:
            lwsl_notice("LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ: %ld\n",
                    (long)len);
            show_http_content((const char *)in, len);
            break;

            /* unchunked content */
        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP:
            printf("LWS_CALLBACK_RECEIVE_CLIENT_HTTP\n");
            {
                char buffer[1024 + LWS_PRE];
                char *px = buffer + LWS_PRE;
                int lenx = sizeof(buffer) - LWS_PRE;

                /*
                 * Often you need to flow control this by something
                 * else being writable.  In that case call the api
                 * to get a callback when writable here, and do the
                 * pending client read in the writeable callback of
                 * the output.
                 *
                 * In the case of chunked content, this will call back
                 * LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ once per
                 * chunk or partial chunk in the buffer, and report
                 * zero length back here.
                 */
                if (lws_http_client_read(wsi, &px, &lenx) < 0)
                    return -1;
            }
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
            //	printf("LWS_CALLBACK_CLIENT_WRITEABLE\n");
            if(sendflag == 1)
            {
                sendflag = 0;
                printf("client send request!\n");
                msgpacklen = can_msg_pack(datacmd, sizeof(datacmd));
                memcpy(buf + LWS_PRE, datacmd, msgpacklen);

                //		n = lws_write(wsi, (unsigned char *)&buf[LWS_PRE], msgpacklen, opts | LWS_WRITE_BINARY);
                n = lws_write(wsi, (unsigned char *)&buf[LWS_PRE], msgpacklen, LWS_WRITE_BINARY);

                lwsl_info("Client wsi %p writable\n", wsi);
            }
            break;

        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
            printf("LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER\n");
            if (test_post) {
                unsigned char **p = (unsigned char **)in, *end = (*p) + len;

                if (lws_add_http_header_by_token(wsi,
                            WSI_TOKEN_HTTP_CONTENT_LENGTH,
                            (unsigned char *)"29", 2, p, end))
                    return -1;
                if (lws_add_http_header_by_token(wsi,
                            WSI_TOKEN_HTTP_CONTENT_TYPE,
                            (unsigned char *)"application/x-www-form-urlencoded",
                            33, p, end))
                    return -1;

                /* inform lws we have http body to send */
                lws_client_http_body_pending(wsi, 1);
                lws_callback_on_writable(wsi);
            }
            break;

        case LWS_CALLBACK_CLIENT_HTTP_WRITEABLE:
            printf("http writeable!\n");
            strcpy(buf + LWS_PRE, "text=hello&send=Send+the+form");
            n = lws_write(wsi, (unsigned char *)&buf[LWS_PRE],
                    strlen(&buf[LWS_PRE]), LWS_WRITE_HTTP);
            if (n < 0)
                return -1;
            /* we only had one thing to send, so inform lws we are done
             * if we had more to send, call lws_callback_on_writable(wsi);
             * and just return 0 from callback.  On having sent the last
             * part, call the below api instead.*/
            lws_client_http_body_pending(wsi, 0);
            break;

        case LWS_CALLBACK_COMPLETED_CLIENT_HTTP:
            printf("client http completed!\n");
            wsi_dumb = NULL;
            force_exit = 1;
            break;

#if defined(LWS_OPENSSL_SUPPORT) && defined(LWS_HAVE_SSL_CTX_set1_param) && \
            !defined(LWS_WITH_MBEDTLS)
        case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
            if (crl_path[0]) {
                /* Enable CRL checking of the server certificate */
                X509_VERIFY_PARAM *param = X509_VERIFY_PARAM_new();
                X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_CRL_CHECK);
                SSL_CTX_set1_param((SSL_CTX*)user, param);
                X509_STORE *store = SSL_CTX_get_cert_store((SSL_CTX*)user);
                X509_LOOKUP *lookup = X509_STORE_add_lookup(store,
                        X509_LOOKUP_file());
                int n = X509_load_cert_crl_file(lookup, crl_path,
                        X509_FILETYPE_PEM);
                X509_VERIFY_PARAM_free(param);
                if (n != 1) {
                    char errbuf[256];
                    n = ERR_get_error();
                    lwsl_err("EXTRA_CLIENT_VERIFY_CERTS: "
                            "SSL error: %s (%d)\n",
                            ERR_error_string(n, errbuf), n);
                    return 1;
                }
            }
            break;
#endif

        default:
            break;
    }

    return 0;
}

/* list of supported protocols and callbacks */

static const struct lws_protocols protocols[] = {
    {
        "-increment-protocol",
        callback_dumb_increment,
        0,
        //		20,
        200,
    },
    {
        "lws-mirror-protocol",
        NULL,
        0,
        4096,
    }, {
        "lws-test-raw-client",
        NULL,
        0,
        128
    },
    { NULL, NULL, 0, 0 } /* end */
};

static const struct lws_extension exts[] = {
    {
        "permessage-deflate",
        lws_extension_callback_pm_deflate,
        "permessage-deflate; client_no_context_takeover"
    },
    {
        "deflate-frame",
        lws_extension_callback_pm_deflate,
        "deflate_frame"
    },
    { NULL, NULL, NULL /* terminator */ }
};



void sighandler(int sig)
{
    force_exit = 1;
}

static int ratelimit_connects(unsigned int *last, unsigned int secs)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);

    if (tv.tv_sec - (*last) < secs)
        return 0;

    *last = tv.tv_sec;

    return 1;
}

void *websocket_connect_can(void *para)
{
    int port = 7681, use_ssl = 0, ietf_version = -1;
    unsigned int rl_dumb = 0,  do_ws = 1, pp_secs = 0;
    struct lws_context_creation_info info;
    struct lws_client_connect_info i;
    struct lws_context *context;
    const char *prot;
    unsigned long last = lws_now_secs();

    memset(&info, 0, sizeof info);
    memset(&i, 0, sizeof(i));

    i.port = 24012;
    i.address = "127.0.0.1";
    i.path = "./";

    /*
     * create the websockets context.  This tracks open connections and
     * knows how to route any traffic and which protocol version to use,
     * and if each connection is client or server side.
     *
     * For this client-only demo, we tell it to not listen on any port.
     */

    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.ws_ping_pong_interval = pp_secs;
    info.extensions = exts;

    context = lws_create_context(&info);
    if (context == NULL) {
        fprintf(stderr, "Creating libwebsocket context failed\n");
        return NULL;
    }

    i.context = context;
    i.ssl_connection = use_ssl;
    i.host = i.address;
    i.origin = i.address;
    i.ietf_version_or_minus_one = ietf_version;

    if (!strcmp(prot, "http") || !strcmp(prot, "https")) {
        lwsl_notice("using %s mode (non-ws)\n", prot);
        if (test_post) {
            i.method = "POST";
            lwsl_notice("POST mode\n");
        }
        else
            i.method = "GET";
        do_ws = 0;
    } else if (!strcmp(prot, "raw")) {
        i.method = "RAW";
        i.protocol = "lws-test-raw-client";
        lwsl_notice("using RAW mode connection\n");
        do_ws = 0;
    } else
        lwsl_notice("using %s mode (ws)\n", prot);

    /*
     * sit there servicing the websocket context to handle incoming
     * packets, and drawing random circles on the mirror protocol websocket
     *
     * nothing happens until the client websocket connection is
     * asynchronously established... calling lws_client_connect() only
     * instantiates the connection logically, lws_service() progresses it
     * asynchronously.
     */
    while (!force_exit) {
#if 0
        if (!flag_echo && !justmirror && !wsi_dumb && ratelimit_connects(&rl_dumb, 2u)) {
            lwsl_notice("dumb: connecting.\n");
            i.protocol = protocols[PROTOCOL_DUMB_INCREMENT].name;
            i.pwsi = &wsi_dumb;
            lws_client_connect_via_info(&i);
        }
#endif
#if 1
        if(i.pwsi == NULL)
        {
            i.pwsi = &wsi_dumb;
            i.protocol = protocols[PROTOCOL_DUMB_INCREMENT].name;
            lws_client_connect_via_info(&i);
            printf("connect!\n");
        }
#endif
        lws_service(context, 200);

    }

    lwsl_err("Exiting\n");
    lws_context_destroy(context);

}

int main(int argc, char **argv)
{
    pthread_t pth[10];

    msg_queue_init();
    signal(SIGINT, sighandler);

    pthread_create(&pth[0], NULL, recv_from_host, NULL);
    pthread_create(&pth[1], NULL, websocket_connect_can, NULL);
    pthread_create(&pth[2], NULL, parse_host_cmd, NULL);

    pthread_join(pth[1], NULL);
    printf("join pthread 1\n");

    //pthread_join(pth[0], NULL);
    //printf("join pthread 0\n");
    //pthread_join(pth[1], NULL);
    //printf("join pthread 1\n");

    return 0;
}

