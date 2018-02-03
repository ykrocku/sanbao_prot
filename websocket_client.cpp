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
#include "rbgetx.h"

#include <queue>
using namespace std;


#define PACK_MAP_MSG(type, type_len, content, content_len)\
    msgpack_pack_str(&pk, type_len);\
    msgpack_pack_str_body(&pk, type, type_len);\
    msgpack_pack_str(&pk, content_len);\
    msgpack_pack_str_body(&pk, content, content_len);

int pack_req_can_cmd(uint8_t *data, uint32_t len, const char *name)
{
    unsigned int map_size = 3;
    uint32_t minlen = 0;

    if(!data || len < 0)
        return -1;

    msgpack_sbuffer sbuf;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer pk;
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);
    msgpack_pack_map(&pk, map_size);

    PACK_MAP_MSG("source", strlen("source"), "client-name", strlen("client-name"));
    PACK_MAP_MSG("topic", strlen("topic"), "subscribe", strlen("subscribe"));

    if(!memcmp(name, "700", strlen("700")))
    {
        PACK_MAP_MSG("data", strlen("data"), "output.can.0x700", strlen("output.can.0x700"));
    }
    else if(!memcmp(name, "760", strlen("760")))
    {
        PACK_MAP_MSG("data", strlen("data"), "output.can.0x760", strlen("output.can.0x700"));
    }
    else
        ;

    minlen = (sbuf.size<len) ? sbuf.size : len;
    memcpy(data, sbuf.data, minlen);

    msgpack_sbuffer_destroy(&sbuf);

    return minlen;
}
void msgpack_object_get(FILE* out, msgpack_object o, can_data_type *can)
{
    enum MSG_DATA_TYPE{
        CAN_DATA=1,
        CAN_SOURCE,
        CAN_TIME,
        CAN_TOPIC,
    };
    static char data_type = 0;

    switch(o.type) {
        case MSGPACK_OBJECT_NIL:
        case MSGPACK_OBJECT_BOOLEAN:
        case MSGPACK_OBJECT_POSITIVE_INTEGER:
        case MSGPACK_OBJECT_NEGATIVE_INTEGER:
        case MSGPACK_OBJECT_FLOAT32:
        case MSGPACK_OBJECT_FLOAT64:
        case MSGPACK_OBJECT_BIN:
        case MSGPACK_OBJECT_EXT:
        case MSGPACK_OBJECT_ARRAY:
            break;

        case MSGPACK_OBJECT_STR:

            if(data_type == CAN_DATA)
            {	
                data_type = 0;
                memcpy(can->warning, o.via.str.ptr, (o.via.str.size > sizeof(can->warning) ? sizeof(can->warning) : o.via.str.size));
                break;
            }
            else if(data_type == CAN_SOURCE)
            {
                data_type = 0;
                memcpy(can->source, o.via.str.ptr, (o.via.str.size > sizeof(can->source) ? sizeof(can->source) : o.via.str.size));
                break;
            }
            else if(data_type == CAN_TIME)
            {
                data_type = 0;
                memcpy(can->time, o.via.str.ptr, (o.via.str.size > sizeof(can->time) ? sizeof(can->time) : o.via.str.size));
                break;
            }
            else if(data_type == CAN_TOPIC)
            {
                data_type = 0;
                memcpy(can->topic, o.via.str.ptr, (o.via.str.size > sizeof(can->topic) ? sizeof(can->topic) : o.via.str.size));
                break;
            }
            else
            {
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
            }
            break;

        case MSGPACK_OBJECT_MAP:
            if(o.via.map.size != 0) {
                msgpack_object_kv* p = o.via.map.ptr;
                msgpack_object_kv* const pend = o.via.map.ptr + o.via.map.size;
                msgpack_object_get(out, p->key, NULL);
                msgpack_object_get(out, p->val, can);
                ++p;
                for(; p < pend; ++p) {
                    msgpack_object_get(out, p->key, NULL);
                    msgpack_object_get(out, p->val, can);
                }
            }
            break;

        default:
            if (o.via.u64 > ULONG_MAX)
                fprintf(out, "#<UNKNOWN %i over 4294967295>", o.type);
            else
                fprintf(out, "#<UNKNOWN %i %lu>", o.type, (unsigned long)o.via.u64);
    }
    return;
}

int unpack_recv_can_msg(uint8_t *data, int size)
{
    msgpack_zone mempool;
    msgpack_object deserialized;
    can_data_type can;

 //   if(!data || size < 0)
 //       return -1;

    msgpack_zone_init(&mempool, 2048);

    msgpack_unpack((const char *)data, size, NULL, &mempool, &deserialized);

    memset(&can, 0, sizeof(can));

    msgpack_object_get(stdout, deserialized, &can);

    msgpack_zone_destroy(&mempool);

    can_message_send(&can);

    return 0;
}

static volatile int force_exit;
static struct lws *wsi_dumb;
enum demo_protocols {
    PROTOCOL_LWS_TO_CAN,

    /* always last */
    DEMO_PROTOCOL_COUNT
};

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

static int callback_lws_communicate(struct lws *wsi, enum lws_callback_reasons reason,
        void *user, void *in, size_t len)
{
#define LWS_WRITE_BUF_LEN   512
    const char *which = "http";
    char buf[LWS_WRITE_BUF_LEN + LWS_PRE];
    int n=0, msgpacklen=0;
    uint8_t datacmd[512];
    static int sendflag=1;

    //	printf("reason: %d\n", reason);
    switch (reason) {

        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            printf("dumb:: LWS_CALLBACK_CLIENT_ESTABLISHED\n");
            lws_callback_on_writable(wsi);

            break;

        case LWS_CALLBACK_CLOSED:
            printf("dumb:: LWS_CALLBACK_CLOSED\n");
            sendflag=1;
            wsi_dumb = NULL;
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            //		printf("receive:\n");
            //		printbuf((uint8_t *)in, (int)len);
            unpack_recv_can_msg((uint8_t *)in, (int)len);
            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            wsi_dumb = NULL;
            printf("dumb:: LWS_CALLBACK_ERROR!\n");
            lwsl_err("CLIENT_CONNECTION_ERROR: %s: %s\n", which,
                    in ? (char *)in : "(null)");
            break;

        case LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED:
            if ((strcmp((const char *)in, "x-webkit-deflate-frame") == 0))
                return 1;
            if ((strcmp((const char *)in, "deflate-frame") == 0))
                return 1;
            break;

        case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
            printf("LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP\n");
            lwsl_notice("lws_http_client_http_response %d\n",
                    lws_http_client_http_response(wsi));
            break;

            /* chunked content */
        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ:
            lwsl_notice("LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ: %ld\n",
                    (long)len);
            break;

            /* unchunked content */
        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP:
            {
                char buffer[1024 + LWS_PRE];
                char *px = buffer + LWS_PRE;
                int lenx = sizeof(buffer) - LWS_PRE;

                if (lws_http_client_read(wsi, &px, &lenx) < 0)
                    return -1;
            }
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
            printf("LWS_CALLBACK_CLIENT_WRITEABLE\n");
            if(sendflag == 1 || sendflag == 2)
            {
                if(sendflag == 2)
                {
                    printf("request 760!\n");
                    msgpacklen = pack_req_can_cmd(datacmd, sizeof(datacmd), "760");
                    sendflag = 0;
                }
                if(sendflag == 1)
                {
                    printf("request 700!\n");
                    msgpacklen = pack_req_can_cmd(datacmd, sizeof(datacmd), "700");
                    sendflag = 2;
                }
                printf("client send request, ret = %d!\n", msgpacklen);
                if(msgpacklen < LWS_WRITE_BUF_LEN && msgpacklen >0)
                {
                    memcpy(buf + LWS_PRE, datacmd, msgpacklen);
                    n = lws_write(wsi, (unsigned char *)&buf[LWS_PRE], msgpacklen, LWS_WRITE_BINARY);
                    if(n<=0)
                    {
                        printf("lws write ret = %d\n", n);
                    }
                }
                lws_callback_on_writable(wsi);
            }
            break;

        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
            printf("LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER\n");
            break;

        case LWS_CALLBACK_CLIENT_HTTP_WRITEABLE:
            break;

        case LWS_CALLBACK_COMPLETED_CLIENT_HTTP:
            //   force_exit = 1;
            break;

        case LWS_CALLBACK_PROTOCOL_DESTROY:
            printf("protocol destroy!\n");
            break;

        case LWS_CALLBACK_WSI_DESTROY:
            printf("wsi destroy!\n");
            sendflag=1;
            wsi_dumb = NULL;
            break;

        default:
            break;
    }

    return 0;
}

/* list of supported protocols and callbacks */
static const struct lws_protocols protocols[] = {
    {
        "lws-to-can-protocol",
        callback_lws_communicate,
        0,
        200,
    },
    { NULL, NULL, 0, 0 } /* end */
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

void *websocket_client(void *para)
{
    int port = 7681, use_ssl = 0, ietf_version = -1;
    unsigned int rl_dumb = 0,  do_ws = 1, pp_secs = 0;
    struct lws_context_creation_info info;
    struct lws_client_connect_info i;
    struct lws_context *context;

    memset(&info, 0, sizeof info);
    memset(&i, 0, sizeof(i));

    i.port = 24012;
    i.address = "127.0.0.1";
    i.path = "./";

    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.ws_ping_pong_interval = pp_secs;
    //   info.extensions = exts;

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

    while (!force_exit) {
        if(!wsi_dumb && ratelimit_connects(&rl_dumb, 2u))
        {
            i.pwsi = &wsi_dumb;
            i.protocol = protocols[PROTOCOL_LWS_TO_CAN].name;
            lws_client_connect_via_info(&i);
            printf("connecting to websocket!\n");
        }
        lws_service(context, 200);
    }
    lwsl_err("Exiting\n");
    lws_context_destroy(context);

    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t pth[10];

    signal(SIGINT, sighandler);
    global_var_init();

    if(pthread_create(&pth[0], NULL, communicate_with_host, NULL))
    {
        printf("pthread_create fail!\n");
        return -1;
    }
    if(pthread_create(&pth[1], NULL, websocket_client, NULL))
    {
        printf("pthread_create fail!\n");
        return -1;
    }
    if(pthread_create(&pth[2], NULL, parse_host_cmd, NULL))
    {
        printf("pthread_create fail!\n");
        return -1;
    }
    if(pthread_create(&pth[3], NULL, pthread_encode_jpeg, NULL))
    {
        printf("pthread_create fail!\n");
        return -1;
    }
    if(pthread_create(&pth[4], NULL, pthread_sav_warning_jpg, NULL))
    {
        printf("pthread_create fail!\n");
        return -1;
    }
    if(pthread_create(&pth[5], NULL, pthread_snap_shot, NULL))
    {
        printf("pthread_create fail!\n");
        return -1;
    }

    pthread_join(pth[1], NULL);
    printf("join pthread 1\n");
//    pthread_join(pth[1], NULL);
//    printf("join pthread 2\n");
//    pthread_join(pth[2], NULL);
//    printf("join pthread 3\n");

    return 0;
}

