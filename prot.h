#ifndef  __SAMPLE_H__
#define __SAMPLE_H__
#include <stdio.h>
#include <stdint.h>
#include <arpa/inet.h>

#include <queue>


#define WRITE_REAL_TIME_MSG 0
#define READ_REAL_TIME_MSG  1



//#define ENABLE_ADAS
//#define ENABLE_DMS

#define FILTER_ALERT_BY_SPEED
#define FILTER_ALERT_BY_TIME

//30 sec
#define FILTER_ADAS_ALERT_SET_TIME 30u
#define FILTER_DMS_ALERT_SET_TIME 30u


#define EXIT_MSG      2
#define NOTICE_MSG    1
#define WAIT_MSG      0
#define IS_EXIT_MSG(flag)   (flag == EXIT_MSG)



#define IS_BRDCST(id)   (SAMPLE_DEVICE_ID_BRDCST == (id))

#define SANBAO_VERSION 0x01
#define VENDOR_ID 0x1234

#define GET_NEXT_SEND_NUM        1
#define RECORD_RECV_NUM          2
#define GET_RECV_NUM             3

#define MM_PHOTO 0
#define MM_AUDIO 1
#define MM_VIDEO 2



#if defined ENABLE_ADAS
#define DO_DELETE_SNAP_SHOT_FILES "rm -r /data/snap/adas/*"
//#define SNAP_SHOT_JPEG_PATH "/data/snap/"
#define SNAP_SHOT_JPEG_PATH "/mnt/obb/adas/"

#elif defined ENABLE_DMS
#define DO_DELETE_SNAP_SHOT_FILES "rm -r /data/snap/dms/*"
//#define SNAP_SHOT_JPEG_PATH "/data/snap/"
#define SNAP_SHOT_JPEG_PATH "/mnt/obb/dms/"

#endif



#define LOCAL_ADAS_PRAR_FILE     "/data/adas_para"
#define LOCAL_DMS_PRAR_FILE     "/data/dms_para"

#if 0
#define UPGRADE_FILE_PATH     "/data/upgrade.mpk"
#define CLEAN_MPK " rm /data/upgrade.mpk"
#define UPGRADE_FILE_CMD     "/data/upgrade.sh /data/upgrade.mpk"
#else
#define UPGRADE_FILE_PATH     "/mnt/obb/package.bin"
#define CLEAN_MPK             "rm /mnt/obb/package.bin"


#if defined ENABLE_ADAS
    #define UPGRADE_FILE_CMD      "/data/adas_upgrade.sh"
#elif defined ENABLE_DMS
    #define UPGRADE_FILE_CMD      "/data/dms_upgrade.sh"
#endif


#endif


//protocol
#define PROTOCOL_USING_BIG_ENDIAN

#ifdef PROTOCOL_USING_BIG_ENDIAN
#define MY_HTONL(x)     htonl(x)
#define MY_HTONS(x)     htons(x)
#else
#define MY_HTONL(x)     (x)
#define MY_HTONS(x)     (x)
#endif

#define MESSAGE_CAN700	"output.can.0x700"
#define MESSAGE_CAN760	"output.can.0x760"


#define SAMPLE_DEVICE_ID_BRDCST         (0x0)
#define SAMPLE_DEVICE_ID_ADAS           (0x64)
#define SAMPLE_DEVICE_ID_DMS            (0x65)
#define SAMPLE_DEVICE_ID_TPMS           (0x66)
#define SAMPLE_DEVICE_ID_BSD            (0x67)

#define SAMPLE_CMD_QUERY                (0x2F)
#define SAMPLE_CMD_FACTORY_RESET        (0x30)
#define SAMPLE_CMD_SPEED_INFO           (0x31)
#define SAMPLE_CMD_DEVICE_INFO          (0x32)
#define SAMPLE_CMD_UPGRADE              (0x33)
#define SAMPLE_CMD_GET_PARAM            (0x34)
#define SAMPLE_CMD_SET_PARAM            (0x35)
#define SAMPLE_CMD_WARNING_REPORT       (0x36)
#define SAMPLE_CMD_REQ_STATUS           (0x37)
#define SAMPLE_CMD_UPLOAD_STATUS        (0x38)
#define SAMPLE_CMD_REQ_MM_DATA          (0x50)
#define SAMPLE_CMD_UPLOAD_MM_DATA       (0x51)
#define SAMPLE_CMD_SNAP_SHOT            (0x52)

#define SAMPLE_PROT_MAGIC               (0x7E)
#define SAMPLE_PROT_ESC_CHAR            (0x7D)
typedef struct _sample_prot_heade
{
    uint8_t     magic;
    uint8_t     checksum;
//    uint16_t    version;//modify
    uint16_t    serial_num;
    uint16_t    vendor_id;
    uint8_t     device_id;
    uint8_t     cmd;
    //uint8_t     payload[n];
    //uint8_t     magic1;
} __attribute__((packed)) sample_prot_header;

typedef struct _sample_mm_info
{
    uint8_t  type;
    uint32_t id;
} __attribute__((packed)) sample_mm_info;



typedef struct _send_mm_info
{
    uint8_t  devid;
    uint8_t  type;
    uint32_t id;
} __attribute__((packed)) send_mm_info;




typedef struct _sample_dev_info
{
    uint8_t     vendor_name_len;
    uint8_t     vendor_name[15];
    uint8_t     prod_code_len;
    uint8_t     prod_code[15];
    uint8_t     hw_ver_len;
    uint8_t     hw_ver[15];
    uint8_t     sw_ver_len;
    uint8_t     sw_ver[15];
    uint8_t     dev_id_len;
    uint8_t     dev_id[15];
    uint8_t     custom_code_len;
    uint8_t     custom_code[15];
} __attribute__((packed)) sample_dev_info;

#define SW_STATUS_BEGIN (0x1)
#define SW_STATUS_END   (0x2)
#define SW_STATUS_EVENT (0x0)

#define WARN_TYPE_NUM       (8)

#define SW_TYPE_FCW     (0x1)
#define SW_TYPE_LDW     (0x2)
#define SW_TYPE_HW      (0x3)
#define SW_TYPE_PCW     (0x4)
#define SW_TYPE_FLC     (0x5)
#define SW_TYPE_TSRW    (0x6)
#define SW_TYPE_TSR     (0x10)
#define SW_TYPE_SNAP    (0x11)

//#define SW_TYPE_TIMER_SNAP    (0x7)

#define SW_TSR_TYPE_SPEED   (0x1)
#define SW_TSR_TYPE_HIGHT   (0x2)
#define SW_TSR_TYPE_WEIGHT  (0x3)
typedef struct _sample_warning
{
    uint8_t     reserve0;
    uint8_t     status;
    uint8_t     type;
    uint8_t     reserve1;
    uint8_t     tsr_type;
    uint8_t     tsr_data;
    uint8_t     reserve2[2];
    uint8_t     mm_count;
} __attribute__((packed)) sample_warning;

typedef struct _sample_mm
{
    uint8_t     type;
    uint32_t    id;
    uint16_t    packet_total_num;
    uint16_t    packet_index;
} __attribute__((packed)) sample_mm;


typedef struct _sample_mm_ack
{
    uint8_t     req_type;
    uint32_t    mm_id;
    uint16_t    packet_total_num;
    uint16_t    packet_index;
    uint8_t     ack;
} __attribute__((packed)) sample_mm_ack;

typedef struct __MECANWarningMessage {
    //#ifdef BIG_ENDIAN
#if 0
    uint8_t     byte0_resv:5;
    uint8_t     sound_type:3;

    uint8_t     byte1_resv0:2;
    uint8_t     zero_speed:1;
    uint8_t     byte1_resv1:5;

    uint8_t     headway_measurement:7;
    uint8_t     headway_valid:1;

    uint8_t     byte3_resv:7;
    uint8_t     no_error:1;

    uint8_t     byte4_resv:4;
    uint8_t     fcw_on:1;
    uint8_t     right_ldw:1;
    uint8_t     left_ldw:1;
    uint8_t     ldw_off:1;

    uint8_t     byte5_resv;
    uint8_t     byte6_resv;

    uint8_t     byte7_resv:6;
    uint8_t     headway_warning_level:2;
#else /*Little Endian*/
    uint8_t     sound_type:3;
    uint8_t     time_indicator:2;
    uint8_t     byte0_resv:3;

    uint8_t     byte1_resv1:5;
    uint8_t     zero_speed:1;
    uint8_t     byte1_resv0:2;

    uint8_t     headway_valid:1;
    uint8_t     headway_measurement:7;

    uint8_t     no_error:1;
    uint8_t     error_code:7;

    uint8_t     ldw_off:1;
    uint8_t     left_ldw:1;
    uint8_t     right_ldw:1;
    uint8_t     fcw_on:1;
    uint8_t     byte4_resv:2;
    uint8_t     maintenanc:1;
    uint8_t     failsafe:1;

    uint8_t     byte5_resv0:1;
    uint8_t     peds_fcw:1;
    uint8_t     peds_in_dz:1;
    uint8_t     byte5_resv1:2;
    uint8_t     tamper_alert:1;
    uint8_t     byte5_resv2:1;
    uint8_t     tsr_enable:1;

    uint8_t     tsr_warning_level:3;
    uint8_t     byte6_resv:5;

    uint8_t     headway_warning_level:2;
    uint8_t     hw_repeatable_enable:1;
    uint8_t     byte7_resv:5;
#endif
} __attribute__((packed)) MECANWarningMessage;



typedef struct __car_status {
    uint16_t    acc:1;
    uint16_t    left_signal:1;
    uint16_t    right_signal:1;
    uint16_t    wipers:1;
    uint16_t    brakes:1;
    uint16_t    card:1;
    uint16_t    byte_resv:10;

} __attribute__((packed)) car_status_s;


typedef struct _real_time_data{

    uint8_t     car_speed;
    uint8_t     reserve1;
    uint32_t     mileage;
    uint8_t     reserve2[2];

    uint16_t	altitude;
    uint32_t	latitude;
    uint32_t	longitude;

    uint8_t     time[6];
    car_status_s    car_status;

} __attribute__((packed)) real_time_data;


typedef struct _module_status{
    
#define MODULE_STANDBY          0x01
#define MODULE_WORKING          0x02
#define MODULE_MAINTAIN         0x03
#define MODULE_ABNORMAL         0x04
    uint8_t work_status;

    uint32_t camera_err:1;
    uint32_t main_memory_err:1;
    uint32_t aux_memory_err:1;
    uint32_t infrared_err:1;
    uint32_t speaker_err:1;
    uint32_t battery_err:1;
    uint32_t reserve6_err:1;
    uint32_t reserve7_err:1;
    uint32_t reserve8_err:1;
    uint32_t reserve9_err:1;
    uint32_t comm_module_err:1;
    uint32_t def_module_err:1;
    uint32_t reserve_err:20;

} __attribute__((packed)) module_status;


typedef struct __warningtext {

    uint32_t	warning_id;
    uint8_t		start_flag;
    uint8_t		sound_type;
    uint8_t		forward_car_speed;
    uint8_t		forward_car_distance;
    uint8_t		ldw_type;
    uint8_t		load_type;
    uint8_t		load_data;
    uint8_t		car_speed;
    uint16_t	altitude;
    uint32_t	latitude;
    uint32_t	longitude;
    uint8_t		time[6];
    car_status_s	car_status;	
    uint8_t		mm_num;
    sample_mm_info mm[0];

} __attribute__((packed)) warningtext;


#define DMS_FATIGUE_WARN            0x01
#define DMS_CALLING_WARN            0x02
#define DMS_SMOKING_WARN            0x03
#define DMS_DISTRACT_WARN           0x04
#define DMS_ABNORMAL_WARN           0x05

#define DMS_SANPSHOT_EVENT          0x10
#define DMS_DRIVER_CHANGE           0x11

typedef struct __dms_warningtext {

    uint32_t	warning_id;
    uint8_t		status_flag;
    uint8_t		sound_type;
    uint8_t		FatigueVal;
    uint8_t		resv[4];

    uint8_t		car_speed;
    uint16_t	altitude; //海拔
    uint32_t	latitude; //纬度
    uint32_t	longitude; //经度

    uint8_t		time[6];
    car_status_s	car_status;	
    uint8_t		mm_num;
    sample_mm_info mm[0];

} __attribute__((packed)) dms_warningtext;









typedef struct __car_info {
//#ifdef BIG_ENDIAN

#if 0
    uint8_t     byte0_resv1:1;
    uint8_t     byte0_resv0:1;
    uint8_t     high_beam:1;
    uint8_t     low_beam:1;
    uint8_t     wipers:1;
    uint8_t     right_signal:1;
    uint8_t     left_signal:1;
    uint8_t     brakes:1;

    uint8_t     speed_aval:1;
    uint8_t     byte1_resv1:1;
    uint8_t     high_beam_aval:1;
    uint8_t     low_beam_aval:1;
    uint8_t     wipers_aval:1;
    uint8_t     byte1_resv0:3;

    uint8_t     speed;

#else /*Little Endian*/
    uint8_t     brakes:1;
    uint8_t     left_signal:1;
    uint8_t     right_signal:1;
    uint8_t     wipers:1;
    uint8_t     low_beam:1;
    uint8_t     high_beam:1;
    uint8_t     byte0_resv0:1;
    uint8_t     byte0_resv1:1;

    uint8_t     byte1_resv0:3;
    uint8_t     wipers_aval:1;
    uint8_t     low_beam_aval:1;
    uint8_t     high_beam_aval:1;
    uint8_t     byte1_resv1:1;
    uint8_t     speed_aval:1;

    uint8_t     speed;

#endif
} __attribute__((packed)) car_info;



typedef struct __dms_alert_info {
    //#ifdef BIG_ENDIAN
#if 0

#else /*Little Endian*/


    /* 短时间闭眼报警 */
    uint8_t alert_eye_close1:2;
    /* 长时间闭眼报警 */
    uint8_t alert_eye_close2:2;
    /* 左顾右盼 */
    uint8_t alert_look_around:2;
    /* 打哈欠 */
    uint8_t alert_yawn:2;
    /* 打电话 */
    uint8_t alert_phone:2;
    /* 吸烟 */
    uint8_t alert_smoking:2;
    /* 离岗 */
    uint8_t alert_absence:2;
    /* 低头 */
    uint8_t alert_bow:2;


    uint8_t byte2_recv;
    uint8_t byte3_recv;
    uint8_t byte4_recv;
    uint8_t byte5_recv;
    uint8_t byte6_recv;
    uint8_t byte7_recv;

#endif
} __attribute__((packed)) dms_alert_info;

typedef struct __dms_can_778 {
    uint8_t Left_Eyelid_fraction;
    uint8_t Right_Eyelid_fraction;
    uint8_t Head_Yaw;
    uint8_t Head_Pitch;
    uint8_t Head_Roll;
    uint8_t Frame_Tag;
    uint8_t reserved[2];
} __attribute__((packed)) dms_can_778;

typedef struct __dms_can_779 {
    uint8_t Eye_Closure_Warning:2;
    uint8_t Yawn_warning:2;
    uint8_t Look_around_warning:2;
    uint8_t Look_up_warning:2;

    uint8_t Look_down_warning:2;
    uint8_t Phone_call_warning:2;
    uint8_t Smoking_warning:2;
    uint8_t Absence_warning:2;

    uint8_t Frame_Tag;
    uint8_t reserved[5];

} __attribute__((packed)) dms_can_779;


typedef struct __dms_can_frame {
    char can_779_valid;
    char can_778_valid;

    dms_can_779 can_779;
    dms_can_778 can_778;
} __attribute__((packed)) dms_can_frame;



/**********************can message*****************************/
typedef struct _can_struct{
    uint8_t warning[8];
    char source[20];
    uint64_t time;
    char topic[20];
}can_data_type;

#define HW_LEVEL_NO_CAR     (0)
#define HW_LEVEL_WHITE_CAR  (1)
#define HW_LEVEL_RED_CAR    (2)


#if 1
#define SOUND_WARN_NONE     (0x0)
#define SOUND_TYPE_SILENCE  (0)
#define SOUND_TYPE_LLDW     (1)
#define SOUND_TYPE_RLDW     (2)
#define SOUND_TYPE_HW       (3)
#define SOUND_TYPE_TSR      (4)
#define SOUND_TYPE_VB       (5)
#define SOUND_TYPE_FCW_PCW  (6)
#endif


#if 0
#define INDEX_SILENCE  (0)
#define INDEX_LLDW     (1)
#define INDEX_RLDW     (2)
#define INDEX_HW       (3)
#define INDEX_TSR      (4)
#define INDEX_VB       (5)
#define FCW_PCW        (6)

#endif

#define MINIEYE_WARNING_CAN_ID  (0x700)
#define MINIEYE_CAR_INFO_CAN_ID (0x760)


typedef struct _adas_para_setting{

    uint8_t warning_speed_val;
    uint8_t warning_volume;
    uint8_t auto_photo_mode;
    uint16_t auto_photo_time_period;
    uint16_t auto_photo_distance_period;
    uint8_t photo_num;
    uint8_t photo_time_period;
    uint8_t image_Resolution;
    uint8_t video_Resolution;
    uint8_t reserve[9];

    uint8_t obstacle_distance_threshold;
    uint8_t obstacle_video_time;
    uint8_t obstacle_photo_num;
    uint8_t obstacle_photo_time_period;

    uint8_t FLC_time_threshold;
    uint8_t FLC_times_threshold;
    uint8_t FLC_video_time;
    uint8_t FLC_photo_num;
    uint8_t FLC_photo_time_period;

    uint8_t LDW_video_time;
    uint8_t LDW_photo_num;
    uint8_t LDW_photo_time_period;


    uint8_t FCW_time_threshold;
    uint8_t FCW_video_time;
    uint8_t FCW_photo_num;
    uint8_t FCW_photo_time_period;


    uint8_t PCW_time_threshold;
    uint8_t PCW_video_time;
    uint8_t PCW_photo_num;
    uint8_t PCW_photo_time_period;

    uint8_t HW_time_threshold;
    uint8_t HW_video_time;
    uint8_t HW_photo_num;
    uint8_t HW_photo_time_period;

    uint8_t TSR_photo_num;
    uint8_t TSR_photo_time_period;

    uint8_t reserve2[4];
} __attribute__((packed)) adas_para_setting;


typedef struct _dms_para_setting{

    //uint8_t Warn_SpeedThreshold;
    uint8_t warning_speed_val;
    uint8_t warning_volume;

    uint8_t auto_photo_mode;
    uint16_t auto_photo_time_period;
    uint16_t auto_photo_distance_period;

    uint8_t photo_num;
    uint8_t photo_time_period;

    uint8_t image_Resolution;
    uint8_t video_Resolution;
    uint8_t reserve[10];

    uint16_t Smoke_TimeIntervalThreshold;
    uint16_t Call_TimeIntervalThreshold;

    uint8_t FatigueDriv_VideoTime;
    uint8_t FatigueDriv_PhotoNum;
    uint8_t FatigueDriv_PhotoInterval;
    uint8_t FatigueDriv_resv;

    uint8_t CallingDriv_VideoTime;
    uint8_t CallingDriv_PhotoNum;
    uint8_t CallingDriv_PhotoInterval;

    uint8_t SmokingDriv_VideoTime;
    uint8_t SmokingDriv_PhotoNum;
    uint8_t SmokingDriv_PhotoInterval;

    uint8_t DistractionDriv_VideoTime;
    uint8_t DistractionDriv_PhotoNum;
    uint8_t DistractionDriv_PhotoInterval;

    uint8_t AbnormalDriv_VideoTime;
    uint8_t AbnormalDriv_PhotoNum;
    uint8_t AbnormalDriv_PhotoInterval;

    uint8_t reserve2[2];
} __attribute__((packed)) dms_para_setting;


typedef struct _mm_node{
#define SLOT_STABLE     0
#define SLOT_WRITING    1
#define SLOT_READING    2

    char rw_flag;
    uint8_t devid;
    uint8_t warn_type;
    uint8_t mm_type;
    uint32_t mm_id;
    uint8_t time[6];

} __attribute__((packed)) mm_node;

#define WARN_SNAP_NUM_MAX   10
typedef struct _InfoForStore{

    uint8_t flag;
    uint8_t warn_type;
    //uint8_t mm_type;
    uint8_t photo_enable;
    uint8_t sound_enable;
    uint8_t video_enable;

    uint8_t video_time;
    uint8_t photo_time_period;
    uint8_t photo_num;
    uint32_t photo_id[WARN_SNAP_NUM_MAX];
    uint32_t video_id[2];

} __attribute__((packed)) InfoForStore;


/**********queue and repeat_send struct****************/
//#define IMAGE_SIZE_PER_PACKET   (1024*16)
#define IMAGE_SIZE_PER_PACKET   (64*1024)
//#define IMAGE_SIZE_PER_PACKET   (4*1024)

#define PTR_QUEUE_BUF_SIZE   (2*(IMAGE_SIZE_PER_PACKET + 64)) //加64, 大于 header + tail, 
#define PTR_QUEUE_BUF_CNT    (16)
#define UCHAR_QUEUE_SIZE    (128*1024)



#define MSG_ACK_READY           0
#define MSG_ACK_WAITING         1
#define MSG_NO_NEED_ACK         2

#define NO_MESSAGE_TO_SEND      1
#define NO_MESSAGE_TO_SEND      1

typedef struct queue_node_status{
    uint8_t ack_status;
    uint8_t send_repeat;
    struct timeval send_time;
    sample_mm mm;
    bool mm_data_trans_waiting;
}SendStatus;

typedef struct _ptr_queue_node{
    uint8_t cmd;
    
    send_mm_info mm_info;
    SendStatus pkg;
    InfoForStore mm;

    uint32_t len;
    uint8_t *buf;
} __attribute__((packed)) ptr_queue_node;

typedef struct _package_repeat_status{
#define REPEAT_SEND_TIMES_MAX   3


    char filepath[100];
    sample_mm mm;

    bool mm_data_trans_waiting;
    uint8_t repeat_cnt;
    struct timeval msg_sendtime;
    bool start_wait_ack;
    ptr_queue_node msgsend;
} __attribute__((packed)) pkg_repeat_status;



void *pthread_snap_shot(void *p);
void *pthread_sav_warning_jpg(void *p);
void *pthread_encode_jpeg(void *p);
void *pthread_tcp_process(void *para);
void *pthread_parse_cmd(void *para);
void *pthread_req_cmd_process(void *para);
void *pthread_tcp_send(void *para);


void set_algo_para();

void read_dev_para(void *para, uint8_t para_type);
void write_dev_para(void *para, uint8_t para_type);

int write_local_adas_para_file(const char* filename);
int write_local_dms_para_file(const char* filename);

void set_adas_para_setting_default();
void global_var_init();
int pull_mm_queue(InfoForStore *mm);
void push_mm_queue(InfoForStore *mm);
int write_file(const char* filename, const void* data, size_t size);
int record_snap_shot();
int do_snap_shot();
void display_mm_resource();
int32_t find_mm_resource(uint32_t id, mm_node *m);
int32_t delete_mm_resource(uint32_t id);
char *warning_type_to_str(uint8_t type);
int timeout_trigger(struct timeval *tv, int ms);
void repeat_send_pkg_status_init();
void printbuf(void *buf, int len);
int can_message_send(can_data_type *sourcecan);
void adas_para_check(adas_para_setting *para);

void print_dms_para(dms_para_setting *para);
void print_adas_para(adas_para_setting *para);
int read_local_adas_para_file(const char* filename);
int read_local_dms_para_file(const char* filename);

void recv_dms_message( can_data_type *can);
void recv_dms_message2( dms_can_779 *can);

void produce_dms_image(InfoForStore *mm);
void set_dms_para_setting_default();


void insert_mm_resouce(mm_node m);
int32_t sample_assemble_msg_to_push(sample_prot_header *pHeader, uint8_t devid, uint8_t cmd,uint8_t *payload, int32_t payload_len);
void get_local_time(uint8_t get_time[6]);
int ratelimit_connects(unsigned int *last, unsigned int secs);
void sem_send_init();

void RealTimeDdata_process(real_time_data *data, int mode);

void get_latitude_info(char *buffer, int len);

#define DEBUG_G
#ifdef DEBUG_G 
//#define MY_DEBUG(format,...) printf("File: "__FILE__", Line: %05d:\n", __LINE__, ##__VA_ARGS__)
#define WSI_DEBUG(format,...) printf(format, ##__VA_ARGS__)
#else
#define WSI_DEBUG(format,...)
#endif



#endif
