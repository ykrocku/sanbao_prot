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
#include "prot.h"
#include <sys/prctl.h>
#include <queue>

#include "rapidjson/document.h"     // rapidjson's DOM-style API
#include "rapidjson/prettywriter.h" // for stringify JSON
#include <cstdio>

#include "common/hal/halio.h"
#include "common/hal/canio.h"

#include "common/hal/android_gsensor.h"
#include "common/hal/tty.h"
#include "common/hal/halio.h"
#include "common/hal/esr.h"
#include "common/hal/fmu.h"
#include "common/hal/wrs.h"
#include "common/hal/canio.h"
#include "common/hal/camctl.h"


#define MOBILEYE_WARNING_ID     (0x700)
#define MOBILEYE_CARSIGNAL_ID   (0x760)

//#include "can_signal.cpp"


using namespace rapidjson;
using namespace std;

HalIO &halio = HalIO::Instance();

#ifndef _DSM_INFO_SIMPLE_BUFFER_H
#define _DSM_INFO_SIMPLE_BUFFER_H



/* 八字节格式的DSM数据 */
/*
报警信息协议对接格式
"topic"     "dsm.alert.0x100"    str
"time"      uint64_t usec        MSGPACK_OBJECT_POSITIVE_INTEGER
"soucre"    "DSMNEWS"            str
"data"      uint8_t buf[8]       bin
*/


//#define DSM_INFO_TOPIC  ("dsm.alert.0x100")
#define DSM_INFO_TOPIC  ("output.info.v1")
#define DSM_INFO_SOURCE  ("DSMNEWS")

#define DSM_INFO_ALERT_BIT_NUM  (2)
#define DSM_INFO_ALERT_PER_BYTE (8/DSM_INFO_ALERT_BIT_NUM)
#define DSM_INFO_ALERT_BYTE_INDEX(index) (index / DSM_INFO_ALERT_PER_BYTE)
#define DSM_INFO_ALERT_MASK_OFFSET(index) (2 * (index % DSM_INFO_ALERT_PER_BYTE))
#define DSM_INFO_ALERT_MASK(index) (0x03 << DSM_INFO_ALERT_MASK_OFFSET(index)) 

/* 报警枚举 */
enum
{
    /* 短时间闭眼报警 */
    DSM_INFO_ALERT_EYE_CLOSE1 = 0,
    /* 长时间闭眼报警 */
    DSM_INFO_ALERT_EYE_CLOSE2,
    /* 左顾右盼 */
    DSM_INFO_ALERT_LOOK_AROUND,
    /* 打哈欠 */
    DSM_INFO_ALERT_YAWN,
    /* 打电话 */
    DSM_INFO_ALERT_PHONE,
    /* 吸烟 */
    DSM_INFO_ALERT_SMOKING,
    /* 离岗 */
    DSM_INFO_ALERT_ABSENCE,
    /* 低头 */
    DSM_INFO_ALERT_BOW,

    DSM_INFO_ALERT_NUM,
};

#define DSM_INFO_GET_ALERT_FROM_BUFFER(buffer, index) \
    (((buffer[DSM_INFO_ALERT_BYTE_INDEX((index))] & DSM_INFO_ALERT_MASK((index))) >> DSM_INFO_ALERT_MASK_OFFSET((index))) & 0x03)

#define DSM_INFO_CLEAR_ALERT(buffer, index) \
    (buffer[DSM_INFO_ALERT_BYTE_INDEX((index))] &= ~(DSM_INFO_ALERT_MASK((index))))

#define DSM_INFO_SET_ALERT_TO_BUFFER(buffer, index, val) \
    do{\
        DSM_INFO_CLEAR_ALERT(buffer, (index));\
        buffer[DSM_INFO_ALERT_BYTE_INDEX(index)] |= (((val) & 0x03) << DSM_INFO_ALERT_MASK_OFFSET(index));\
    } while(0)

#endif /* _DSM_INFO_SIMPLE_BUFFER_H */







#define PACK_MAP_MSG(type, type_len, content, content_len)\
    msgpack_pack_str(&pk, type_len);\
    msgpack_pack_str_body(&pk, type, type_len);\
    msgpack_pack_str(&pk, content_len);\
    msgpack_pack_str_body(&pk, content, content_len);

/**************
 *
 *  topic: subscribe
 *  source: client-name
 *  data: dsm.alert.0x100
 *
 * ****************/
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
    else if(!memcmp(name, "dsm_alert", strlen("dsm_alert")))
    {
        PACK_MAP_MSG("data", strlen("data"), DSM_INFO_TOPIC, strlen("dsm.alert.0x100"));
    }
    else
        ;

    minlen = (sbuf.size<len) ? sbuf.size : len;
    memcpy(data, sbuf.data, minlen);

    msgpack_sbuffer_destroy(&sbuf);

    return minlen;
}

#if 0
const char* type_to_str(uint8_t type)
{
    switch(type){
        case MSGPACK_OBJECT_NIL:
            return "OBJECT_NIL";
        case MSGPACK_OBJECT_BOOLEAN:
            return "BOOLEAN";
        case MSGPACK_OBJECT_POSITIVE_INTEGER:
            return "POSITIVE_INTEGER";
        case MSGPACK_OBJECT_NEGATIVE_INTEGER:
            return "NEGATIVE_INTEGER";
        case MSGPACK_OBJECT_FLOAT32:
            return "FLOAT32";
        case MSGPACK_OBJECT_FLOAT64:
            return "FLOAT64";
        case MSGPACK_OBJECT_EXT:
            return "EXT";
        case MSGPACK_OBJECT_ARRAY:
            return "ARRAY";

        case MSGPACK_OBJECT_BIN:
            return "BIN";
        case MSGPACK_OBJECT_STR:
            return "STR";
        case MSGPACK_OBJECT_MAP:
            return "MAP";
        default:
            return "DEFAULT";
    }
}

#endif

void msgpack_object_get(FILE* out, msgpack_object o, can_data_type *can)
{
    enum MSG_DATA_TYPE{
        CAN_DATA=1,
        CAN_SOURCE,
        CAN_TIME,
        CAN_TOPIC,
    };
    static char data_type = 0;

    //WSI_DEBUG("msgpack type = %d\n", o.type);
    switch(o.type) {
        case MSGPACK_OBJECT_NIL:
        case MSGPACK_OBJECT_BOOLEAN:
        case MSGPACK_OBJECT_POSITIVE_INTEGER:
        case MSGPACK_OBJECT_NEGATIVE_INTEGER:
        case MSGPACK_OBJECT_FLOAT32:
        case MSGPACK_OBJECT_FLOAT64:
        case MSGPACK_OBJECT_EXT:
        case MSGPACK_OBJECT_ARRAY:
            if(data_type == CAN_TIME)
            {
                data_type = 0;
                //printf("get uint64: %ld\n", o.via.u64);
                can->time = o.via.u64;
                break;
            }
            break;
        case MSGPACK_OBJECT_BIN:
            //printf("get bin: %d\n", o.via.bin.size);
            //printbuf((uint8_t *)o.via.bin.ptr,o.via.bin.size);
            //printf("get bin: %d\n", o.via.i64);

            if(data_type == CAN_DATA)
            {
                data_type = 0;
                //printf("data enter!\n");
                memcpy(can->warning, o.via.bin.ptr, (o.via.bin.size > sizeof(can->warning) ? sizeof(can->warning) : o.via.bin.size));
                break;
            }
            break;

        case MSGPACK_OBJECT_STR:
            //printf("get str: %s\n", o.via.str.ptr);

            if(data_type == CAN_SOURCE)
            {	
                data_type = 0;
                memcpy(can->source, o.via.str.ptr, (o.via.str.size > sizeof(can->source) ? sizeof(can->source) : o.via.str.size));
                break;
            }
            else if(data_type == CAN_TOPIC)
            {
                //printf("topic enter!\n");
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

int msgpack_object_dsm_get(FILE* out, msgpack_object o, can_data_type *can, uint8_t *buf, uint32_t *buflen)
{
    enum MSG_DATA_TYPE{
        CAN_DATA=1,
        CAN_SOURCE,
        CAN_TIME,
        CAN_TOPIC,
    };
    static char data_type = 0;

    //printf("type = %s\n", type_to_str(o.type));
    switch(o.type) {
        case MSGPACK_OBJECT_NIL:
        case MSGPACK_OBJECT_BOOLEAN:
        case MSGPACK_OBJECT_POSITIVE_INTEGER:
        case MSGPACK_OBJECT_NEGATIVE_INTEGER:
        case MSGPACK_OBJECT_FLOAT32:
        case MSGPACK_OBJECT_FLOAT64:
        case MSGPACK_OBJECT_EXT:
        case MSGPACK_OBJECT_ARRAY:
            //case MSGPACK_OBJECT_BIN:
            //printf("get uint64: %ld\n", o.via.u64);
            //printf("get bin: %d\n", o.via.i64);
            if(data_type == CAN_TIME)
            {
                data_type = 0;
                //printf("get uint64: %ld\n", o.via.u64);
                can->time = o.via.u64;
                break;
            }
            break;
        case MSGPACK_OBJECT_BIN:
            //printf("get bin: %d\n", o.via.bin.size);
            //printbuf((uint8_t *)o.via.bin.ptr,o.via.bin.size);
            //printf("get bin: %d\n", o.via.i64);

            if(data_type == CAN_DATA)
            {
                data_type = 0;
                //memcpy(can->warning, o.via.bin.ptr, (o.via.bin.size > sizeof(can->warning) ? sizeof(can->warning) : o.via.bin.size));
                *buflen = o.via.bin.size > *buflen ? *buflen : o.via.bin.size;
                //printf("data enter! len = %d / %d\n", o.via.bin.size, *buflen);
                memcpy(buf, o.via.bin.ptr, *buflen);
                break;
            }
            break;

        case MSGPACK_OBJECT_STR:
            //printf("get str: %s\n", o.via.str.ptr);

            if(data_type == CAN_SOURCE)
            {	
                data_type = 0;
                memcpy(can->source, o.via.str.ptr, (o.via.str.size > sizeof(can->source) ? sizeof(can->source) : o.via.str.size));
                break;
            }
            else if(data_type == CAN_TOPIC)
            {
                //printf("topic enter!\n");
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
                msgpack_object_dsm_get(out, p->key, NULL, buf, buflen);
                msgpack_object_dsm_get(out, p->val, can, buf, buflen);
                ++p;
                for(; p < pend; ++p) {
                    msgpack_object_dsm_get(out, p->key, NULL, buf, buflen);
                    msgpack_object_dsm_get(out, p->val, can, buf, buflen);
                }
            }
            break;

        default:
            if (o.via.u64 > ULONG_MAX)
                fprintf(out, "#<UNKNOWN %i over 4294967295>", o.type);
            else
                fprintf(out, "#<UNKNOWN %i %lu>", o.type, (unsigned long)o.via.u64);
    }
    return 0;
}

int dsm_parse_data_json(char *buffer, dsm_can_frame *can_frame);
#define DSM_JSON_MSG_LEN (1024*1024)
int unpack_recv_can_msg(char *data, size_t size)
{
    dsm_can_frame can_frame;
    uint32_t len = 0;
    msgpack_zone mempool;
    msgpack_object deserialized;
    can_data_type can;
    uint8_t data_msg[8192];

 //   if(!data || size < 0)
 //       return -1;

#if defined ENABLE_ADAS 
    msgpack_zone_init(&mempool, DSM_JSON_MSG_LEN);//1M
    msgpack_unpack((const char *)data, size, NULL, &mempool, &deserialized);
    memset(&can, 0, sizeof(can));
    msgpack_object_get(stdout, deserialized, &can);
    msgpack_zone_destroy(&mempool);
    can_message_send(&can);

#elif defined ENABLE_DSM
    msgpack_zone_init(&mempool, DSM_JSON_MSG_LEN);//1M
    //printbuf(data, size);
    msgpack_unpack((const char *)data, size, NULL, &mempool, &deserialized);
    //get bin data
    len = sizeof(data_msg);
    msgpack_object_dsm_get(stdout, deserialized, &can, data_msg, &len);
    WSI_DEBUG("get bin data len = %d\n", len);
    //printbuf(data_msg, len);

    //second unpack
    char *buffer = (char *)malloc(DSM_JSON_MSG_LEN);
    msgpack_unpack((const char *)data_msg, len, NULL, &mempool, &deserialized);
    msgpack_object_print_buffer(buffer, DSM_JSON_MSG_LEN, deserialized);
    msgpack_zone_destroy(&mempool);
    WSI_DEBUG("unpack:\n %s\n", buffer);

    memset(&can_frame, 0, sizeof(dsm_can_frame));
    dsm_parse_data_json(buffer, &can_frame);
    if(can_frame.can_779_valid){
        WSI_DEBUG("can779:\n");
        //printbuf(&can_frame.can_779, sizeof(dsm_can_779));
        halio.send_can_frame(0x779, (char *)&can_frame.can_779);
        recv_dsm_message(&can_frame.can_779);
    }
    if(can_frame.can_778_valid){
        WSI_DEBUG("can778:\n");
        //printbuf(&can_frame.can_778, sizeof(dsm_can_778));
        halio.send_can_frame(0x778, (char *)&can_frame.can_778);
    }
    if(buffer)
        free(buffer);
#endif
    return 0;
}

static int get_dsm_alert(const rapidjson::Value& val)
{
    assert(val["score"].IsNumber());
    assert(val["score"].IsDouble());
    assert(val["alerting"].IsBool());
    assert(val.HasMember("alerting"));

    if (val.HasMember("score")) {
        WSI_DEBUG("score = %f\n", val["score"].GetDouble());
    }   

    if(val["score"].GetDouble() == 0){
        WSI_DEBUG("no detect!\n");
        return 0;
    }

    //is alert
    if(val["alerting"].GetBool()){
        WSI_DEBUG("alert!\n");
        return 2;
    }else{
        WSI_DEBUG("detect!\n");
        return 1;
    }
}

static bool get_dsm_pose(const rapidjson::Value& val, uint8_t *yaw, uint8_t *pitch, uint8_t *roll)
{
    assert(val["yaw"].IsNumber());
    assert(val["yaw"].IsDouble());
    *yaw = (uint8_t)(val["yaw"].GetDouble() + 127);
    WSI_DEBUG("yaw = %g\n", val["yaw"].GetDouble());

    assert(val["pitch"].IsNumber());
    assert(val["pitch"].IsDouble());
    *pitch = (uint8_t)(val["pitch"].GetDouble() + 127);
    WSI_DEBUG("pitch = %g\n", val["pitch"].GetDouble());

    assert(val["roll"].IsNumber());
    assert(val["roll"].IsDouble());
    *roll= (uint8_t)(val["roll"].GetDouble() + 127);
    WSI_DEBUG("roll = %g\n", val["roll"].GetDouble());

    return true;
}

int dsm_parse_data_json(char *buffer, dsm_can_frame *can_frame)
{
    Document document;  // Default template parameter uses UTF8 and MemoryPoolAllocator.
    static uint32_t s_frame_tag_778 = 0, s_frame_tag_779 = 0;
    uint8_t yaw = 0, pitch = 0, roll = 0;

    // In-situ parsing, decode strings directly in the source string. Source must be string.
    if (document.ParseInsitu(buffer).HasParseError())
        return 1;

    WSI_DEBUG("Parsing to document succeeded.\n");
    assert(document.IsObject()); 
    assert(document.HasMember("eye_alert"));
    assert(document.HasMember("yawn_alert"));
    assert(document.HasMember("look_around_alert"));
    assert(document.HasMember("look_up_alert"));
    assert(document.HasMember("look_down_alert"));
    assert(document.HasMember("phone_alert"));
    assert(document.HasMember("smoking_alert"));
    assert(document.HasMember("absence_alert"));

    //build can 779
    can_frame->can_779.Eye_Closure_Warning = get_dsm_alert(document["eye_alert"]);
    can_frame->can_779.Yawn_warning = get_dsm_alert(document["yawn_alert"]);
    can_frame->can_779.Look_around_warning = get_dsm_alert(document["look_around_alert"]);
    can_frame->can_779.Look_up_warning = get_dsm_alert(document["look_up_alert"]);
    can_frame->can_779.Look_down_warning = get_dsm_alert(document["look_down_alert"]);
    can_frame->can_779.Phone_call_warning = get_dsm_alert(document["phone_alert"]);
    can_frame->can_779.Smoking_warning = get_dsm_alert(document["smoking_alert"]);
    can_frame->can_779.Absence_warning = get_dsm_alert(document["absence_alert"]);

    can_frame->can_779.Frame_Tag = s_frame_tag_779 & 0xFF;
    s_frame_tag_779 ++;
    memset(can_frame->can_779.reserved, 0, sizeof(can_frame->can_779.reserved));
    can_frame->can_779_valid = 1;

    //build can 778
    assert(document["face_detected"].IsBool());
    if(document["face_detected"].GetBool()){
        WSI_DEBUG("face detected!\n");
        assert(document["left_eye_open_faction"].IsNumber());
        assert(document["left_eye_open_faction"].IsDouble());
        can_frame->can_778.Left_Eyelid_fraction = (uint8_t)(document["left_eye_open_faction"].GetDouble()*100);

        assert(document["right_eye_open_faction"].IsNumber());
        assert(document["right_eye_open_faction"].IsDouble());
        can_frame->can_778.Right_Eyelid_fraction = (uint8_t)(document["right_eye_open_faction"].GetDouble()*100);

        assert(document.HasMember("intrinsic_pose"));
        assert(document["intrinsic_pose"].HasMember("yaw"));
        assert(document["intrinsic_pose"].HasMember("pitch"));
        assert(document["intrinsic_pose"].HasMember("roll"));
        get_dsm_pose(document["intrinsic_pose"], &yaw, &pitch, &roll);
        can_frame->can_778.Head_Yaw = yaw;
        can_frame->can_778.Head_Pitch = pitch;
        can_frame->can_778.Head_Roll = roll;
        memset(can_frame->can_778.reserved, 0, sizeof(can_frame->can_778.reserved));
        can_frame->can_778.Frame_Tag = s_frame_tag_778 & 0xFF;
        s_frame_tag_778 ++;
        can_frame->can_778_valid = 1;
    }
    return 0;
}

volatile int force_exit = 0;
static struct lws *wsi_dumb;
enum demo_protocols {
    PROTOCOL_LWS_TO_CAN,

    /* always last */
    DEMO_PROTOCOL_COUNT
};

void printbuf(void *buffer, int len)
{
    int i;
    uint8_t *buf = (uint8_t *)buffer;

    for(i=0; i<len; i++)
    {
        if(i && (i%16==0))
            WSI_DEBUG("\n");

        WSI_DEBUG("0x%02x ", buf[i]);
    }
    WSI_DEBUG("\n");
}

//void write_wsi_log(char *buf, int len)
void write_wsi_log(int reasons)
{
    FILE *fp;
    char buf[50];
    int len = 0;

    switch(reasons){
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            sprintf(buf, "%s\n", "CLIENT_ESTABLISHED");
            break;
        case LWS_CALLBACK_CLOSED:
            sprintf(buf, "%s\n", "CLIENT_CLOSED");
            break;
        case LWS_CALLBACK_CLIENT_RECEIVE:
            sprintf(buf, "%s\n", "CLIENT_RECV");
            break;
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            sprintf(buf, "%s\n", "ONNECTION_ERROR");
            break;
        case LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED:
            sprintf(buf, "%s\n", "CLIENT_CONFIRM_EXTENSION_SUPPORTED");
            break;
        case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
            sprintf(buf, "%s\n", "ESTABLISHED_CLIENT_HTTP");
            break;
        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ:
            sprintf(buf, "%s\n", "RECEIVE_CLIENT_HTTP_READ");
            break;
        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP:
            sprintf(buf, "%s\n", "RECEIVE_CLIENT_HTTP");
            break;
        case LWS_CALLBACK_CLIENT_WRITEABLE:
            sprintf(buf, "%s\n", "CLIENT_WRITEABLE");
            break;
        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
            sprintf(buf, "%s\n", "CLIENT_APPEND_HANDSHAKE_HEADER");
            break;
        case LWS_CALLBACK_CLIENT_HTTP_WRITEABLE:
            sprintf(buf, "%s\n", "CLIENT_HTTP_WRITEABLE");
            break;
        case LWS_CALLBACK_COMPLETED_CLIENT_HTTP:
            sprintf(buf, "%s\n", "COMPLETED_CLIENT_HTTP");
            break;
        case LWS_CALLBACK_PROTOCOL_DESTROY:
            sprintf(buf, "%s\n", "PROTOCOL_DESTROY");
            break;
        case LWS_CALLBACK_WSI_DESTROY:
            sprintf(buf, "%s\n", "WSI_DESTROY");
            break;
        default:
            sprintf(buf, " reason = %d, %s\n", reasons, "unknow reason");
            break;

    }

    fp = fopen("/mnt/obb/wsi.log", "a");
    len = strlen(buf);
    fwrite(buf, 1, len, fp);
    fclose(fp);
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
    //write_wsi_log(reason);
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
            //printf("receive: %ld\n", len);
            //printbuf((uint8_t *)in, (int)len);
            unpack_recv_can_msg((char *)in, len);
            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            wsi_dumb = NULL;
            //printf("dumb:: LWS_CALLBACK_ERROR!\n");
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
            //printf("LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP\n");
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
#if defined ENABLE_ADAS
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
#elif defined ENABLE_DSM
                msgpacklen = pack_req_can_cmd(datacmd, sizeof(datacmd), "dsm_alert");
                sendflag = 0;
#endif
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
            printf("wsi destroy!!!\n");
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
        8192, //set recv len max
    },
    { NULL, NULL, 0, 0 } /* end */
};

void sighandler(int sig)
{
    force_exit = 1;
    //exit(0);
}
int ratelimit_connects(unsigned int *last, unsigned int secs)
{
    struct timeval tv; 

    gettimeofday(&tv, NULL);

    if (tv.tv_sec - (*last) < secs)
        return 0;

    *last = tv.tv_sec;

    return 1;
}
void *pthread_websocket_client(void *para)
{
    int port = 7681, use_ssl = 0, ietf_version = -1;
    unsigned int rl_dumb = 0,  do_ws = 1, pp_secs = 0;
    struct lws_context_creation_info info;
    struct lws_client_connect_info i;
    struct lws_context *context;

    prctl(PR_SET_NAME, "websocket");
    memset(&info, 0, sizeof info);
    memset(&i, 0, sizeof(i));

#if defined ENABLE_ADAS
    i.port = 24012;
#elif defined ENABLE_DSM
    i.port = 24011;
#endif
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
    lwsl_err("wsi Exiting\n");
    lws_context_destroy(context);

    pthread_exit(NULL);
}

size_t ReadFile(char *buf, int len, const char *filename)
{
    FILE *fp;
    size_t size = 0;

    fp = fopen(filename, "rb");
    if(!fp){
        printf("read file fail:%s\n", strerror(errno));
        return 0;
    }   
    size = fread(buf, 1, len, fp);
    fclose(fp);

    return size;
}

void can_send_init(void)
{
    //HalIO &halio = HalIO::Instance();
    halio.Init(NULL, 0, true);
}

extern int req_flag;
extern pthread_mutex_t  req_mutex;
extern pthread_cond_t   req_cond;

extern int tcp_recv_data;
extern pthread_mutex_t  tcp_recv_mutex;
extern pthread_cond_t   tcp_recv_cond;

extern int save_mp4;
extern pthread_mutex_t  save_mp4_mutex;
extern pthread_cond_t   save_mp4_cond;

void pthread_exit_notice(void)
{
        pthread_mutex_lock(&req_mutex);
        req_flag = EXIT_MSG;
        pthread_cond_signal(&req_cond);
        pthread_mutex_unlock(&req_mutex);

        pthread_mutex_lock(&tcp_recv_mutex);
        tcp_recv_data = EXIT_MSG;
        pthread_cond_signal(&tcp_recv_cond);
        pthread_mutex_unlock(&tcp_recv_mutex);

        pthread_mutex_lock(&tcp_recv_mutex);
        tcp_recv_data = EXIT_MSG;
        pthread_cond_signal(&tcp_recv_cond);
        pthread_mutex_unlock(&tcp_recv_mutex);
}

int main(int argc, char **argv)
{
    pthread_t pth[10];
    int i =0;

    printf("compile time %s %s\n", __DATE__, __TIME__);

    signal(SIGINT, sighandler);
    global_var_init();
    //can_send_init();
    
    if(pthread_create(&pth[0], NULL, pthread_websocket_client, NULL))
    {
        printf("pthread_create fail!\n");
        return -1;
    }
    if(pthread_create(&pth[1], NULL, pthread_tcp_process, NULL))
    {
        printf("pthread_create fail!\n");
        return -1;
    }
    if(pthread_create(&pth[2], NULL, pthread_encode_jpeg, NULL))
    {
        printf("pthread_create fail!\n");
        return -1;
    }
    if(pthread_create(&pth[4], NULL, pthread_sav_warning_jpg, NULL))
    {
        printf("pthread_create fail!\n");
        return -1;
    }
    if(pthread_create(&pth[6], NULL, pthread_req_cmd_process, NULL))
    {
        printf("pthread_create fail!\n");
        return -1;
    }
    if(pthread_create(&pth[5], NULL, pthread_snap_shot, NULL))
    {
        printf("pthread_create fail!\n");
        return -1;
    }
    if(pthread_create(&pth[7], NULL, pthread_tcp_send, NULL))
    {
        printf("pthread_create fail!\n");
        return -1;
    }
#if 1
    while(!force_exit)
    {
        sleep(1);
    }
#endif
    pthread_join(pth[0], NULL);
    printf("join %d\n", i++);
    pthread_join(pth[1], NULL);
    printf("join %d\n", i++);
    pthread_join(pth[2], NULL);
    printf("join %d\n", i++);
    pthread_join(pth[3], NULL);
    printf("join %d\n", i++);
    pthread_join(pth[4], NULL);
    printf("join %d\n", i++);
    pthread_join(pth[5], NULL);
    printf("join %d\n", i++);
    pthread_join(pth[6], NULL);
    printf("join %d\n", i++);
    pthread_join(pth[7], NULL);
    printf("join %d\n", i++);
    return 0;
}

