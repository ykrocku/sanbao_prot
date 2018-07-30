//#include <android_camrec.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <termios.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>
#include <semaphore.h>
#include <sys/prctl.h>

#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <semaphore.h>

#include "prot.h"
#include "common.h"
#include <stdbool.h>

#include <queue>
using namespace std;

static int32_t sample_send_image(uint8_t devid);

extern volatile int force_exit;
extern LocalConfig g_configini;

int hostsock = -1;

#define GET_NEXT_SEND_NUM        1
#define RECORD_RECV_NUM          2
#define GET_RECV_NUM             3

static uint32_t unescaple_msg(uint8_t *buf, uint8_t *msg, int msglen);

pthread_mutex_t photo_queue_lock = PTHREAD_MUTEX_INITIALIZER;
queue<ptr_queue_node *> g_image_queue;
queue<ptr_queue_node *> *g_image_queue_p = &g_image_queue;

pthread_mutex_t req_cmd_queue_lock = PTHREAD_MUTEX_INITIALIZER;
queue<ptr_queue_node *> g_req_cmd_queue;
queue<ptr_queue_node *> *g_req_cmd_queue_p = &g_req_cmd_queue;

pthread_mutex_t ptr_queue_lock = PTHREAD_MUTEX_INITIALIZER;
queue<ptr_queue_node *> g_ptr_queue;
queue<ptr_queue_node *> *g_send_q_p = &g_ptr_queue;

pthread_mutex_t uchar_queue_lock = PTHREAD_MUTEX_INITIALIZER;
queue<uint8_t> g_uchar_queue;

//insert mm info 
void push_mm_queue(InfoForStore *mm)
{
    ptr_queue_node header;
    header.buf = NULL;
    header.len = 0;

    memcpy(&header.mm, mm, sizeof(*mm));

    ptr_queue_push(g_image_queue_p, &header, &photo_queue_lock);
}

//pull node ,the info use to record the mp4 or jpeg
int pull_mm_queue(InfoForStore *mm)
{
    ptr_queue_node header;
    header.buf = NULL;
    header.len = 0;


    if(!ptr_queue_pop(g_image_queue_p, &header, &photo_queue_lock))
    {
        memcpy(mm, &header.mm, sizeof(*mm));
        return 0;
    }

    return -1;
}

//store req mm cmd
void push_mm_req_cmd_queue(SBMmHeader2 *mm_info)
{
    ptr_queue_node header;
    header.buf = NULL;
    header.len = 0;

    memcpy(&header.mm_info, mm_info, sizeof(*mm_info));

    //printf("push id = 0x%08x, type=%x\n", mm_info->id, mm_info->type);
    ptr_queue_push(g_req_cmd_queue_p, &header, &req_cmd_queue_lock);
}
//pull req cmd
int pull_mm_req_cmd_queue(SBMmHeader2 *mm_info)
{
    ptr_queue_node header;
    header.buf = NULL;
    header.len = 0;
    if(!ptr_queue_pop(g_req_cmd_queue_p, &header, &req_cmd_queue_lock))
    {
        memcpy(mm_info, &header.mm_info, sizeof(*mm_info));
        //printf("id = 0x%08x, type=%x\n", header.mm_info.id, header.mm_info.type);
        return 0;
    }

    return -1;
}
void get_local_time(uint8_t get_time[6])
{
    struct tm a;
    struct tm *p = &a;
    time_t rawtime = 0;   

    rawtime = time(NULL);
    //rawtime += 8*3600;

    p = localtime(&rawtime);
    get_time[0] = (p->tm_year+1900)%100;
    get_time[1] = p->tm_mon+1;
    get_time[2] = p->tm_mday;
    get_time[3] = p->tm_hour;
    get_time[4] = p->tm_min;
    get_time[5] = p->tm_sec;

    printf("local time %d-%d-%d %d:%d:%d\n", (1900 + p->tm_year), ( 1 + p->tm_mon), p->tm_mday,(p->tm_hour), p->tm_min, p->tm_sec); 
}

sem_t send_data;
void sem_send_init()
{
    sem_init(&send_data, 0, 0);
}

int clear_media()
{
    int ret = 0;
    char create_path_cmd[100];
    char clear_media_cmd[100];
    sprintf(clear_media_cmd, "rm %s -rf", SNAP_SHOT_JPEG_PATH);
    ret = system(clear_media_cmd);

    sprintf(create_path_cmd, "busybox mkdir -p %s", SNAP_SHOT_JPEG_PATH);
    ret = system(create_path_cmd);

    return ret;
}


#if defined ENABLE_ADAS
    #define PROT_LOG_NAME "/data/adasprot.log"
#elif defined ENABLE_DMS
    #define PROT_LOG_NAME "/data/dmsprot.log"
#endif

int global_var_init()
{

#if defined ENABLE_ADAS
    printf("adas device enter!\n");
#elif defined ENABLE_DMS
    printf("dms device enter!\n");
#else
    #define ENABLE_ADAS
    printf("using default, ENABLE_ADAS!\n");
#endif

    sem_send_init();
    read_local_adas_para_file(LOCAL_ADAS_PRAR_FILE);
    read_local_dms_para_file(LOCAL_DMS_PRAR_FILE);

    if(clear_media()){
        return -1;
    }
    //read_local_file_to_list();

    data_log_init(PROT_LOG_NAME, false);

    return 0;
}

int recv_ack = WAIT_MSG;
pthread_mutex_t  recv_ack_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t   recv_ack_cond = PTHREAD_COND_INITIALIZER;

int save_mp4 = 0;
pthread_mutex_t  save_mp4_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t   save_mp4_cond = PTHREAD_COND_INITIALIZER;

int GetFileSize(char *filename);



int setcondattr(pthread_cond_t *i_cv)
{
    pthread_condattr_t cattr;
	int ret = pthread_condattr_init(&cattr);
	if (ret != 0){
		return (1);
	}
	ret = pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
	ret = pthread_cond_init(i_cv, &cattr);
	return 0;
}

void notice_ack_msg()
{
    pthread_mutex_lock(&recv_ack_mutex);
    recv_ack = NOTICE_MSG;
    pthread_cond_signal(&recv_ack_cond);
    pthread_mutex_unlock(&recv_ack_mutex);
}

//实时数据处理
#define WRITE_REAL_TIME_MSG 0
#define READ_REAL_TIME_MSG  1
void RealTimeDdata_process(RealTimeData *data, int mode)
{
    static RealTimeData msg={0};
    static pthread_mutex_t real_time_msg_lock = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_lock(&real_time_msg_lock);
    if(mode == WRITE_REAL_TIME_MSG)
    {
        memcpy(&msg, data, sizeof(RealTimeData));
    }
    else if(mode == READ_REAL_TIME_MSG)
    {
        memcpy(data, &msg, sizeof(RealTimeData));
    }
    pthread_mutex_unlock(&real_time_msg_lock);
    
    //printf("get car speed: %d\n", data->car_speed);
}


void can760_message_process(CAN760Info *data, int mode)
{
    static CAN760Info msg;
    static pthread_mutex_t can760_lock = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_lock(&can760_lock);
    if(mode == WRITE_REAL_TIME_MSG)
    {
        memcpy(&msg, data, sizeof(CAN760Info));
    }
    else if(mode == READ_REAL_TIME_MSG)
    {
        memcpy(data, &msg, sizeof(CAN760Info));
    }
    pthread_mutex_unlock(&can760_lock);
    
    //printf("get car speed: %d\n", data->car_speed);
}




void get_latitude_info(char *buffer, int len)
{
    RealTimeData tmp;
    RealTimeDdata_process(&tmp, READ_REAL_TIME_MSG);

    snprintf(buffer, len, "BD:%.6fN,%.6fE",\
            (MY_HTONL(tmp.latitude)*1.0)/1000000, (MY_HTONL(tmp.longitude)*1.0)/1000000);

    //printf("latitude: %s\n", buffer);

}




#define WARNING_ID_MODE 0
#define MM_ID_MODE 1
//获取 报警ID，多媒体 ID
uint32_t get_next_id(int mode, uint32_t *id, uint32_t num)
{
    static pthread_mutex_t id_lock = PTHREAD_MUTEX_INITIALIZER;
    static uint32_t s_warning_id = 0;
    static uint32_t s_mm_id = 0;
    uint32_t warn_id = 0;
    uint32_t i;

    pthread_mutex_lock(&id_lock);
    if(mode == WARNING_ID_MODE)
    {
        s_warning_id ++;
        warn_id = s_warning_id;
    }
    else if(mode == MM_ID_MODE)
    {
        for(i=0; i<num; i++)
        {
            s_mm_id ++;
            id[i] = s_mm_id;
        }
    }
    else
    {
        warn_id = 0;
    }
    pthread_mutex_unlock(&id_lock);

    return warn_id;
}

//流水号处理 
void do_serial_num(uint16_t *num, int mode)
{
    static uint16_t send_serial_num= 0;
    static uint16_t recv_serial_num= 0;
    static pthread_mutex_t serial_num_lock = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_lock(&serial_num_lock);
    if(mode == GET_NEXT_SEND_NUM)
    {
        if(send_serial_num == 0xFF)
            send_serial_num = 0;
        else
        {
            send_serial_num ++;
            *num = send_serial_num;
        }
    }
    //记录接收到的序列号，发送序列号在此基础上累加 
    else if(mode == RECORD_RECV_NUM)
    {
        recv_serial_num = *num;
        if(*num > send_serial_num)
        {
            send_serial_num = *num;
        }
    }
    else if(mode == GET_RECV_NUM)
    {
        *num = recv_serial_num; 
    }
    else
    {
    }
    pthread_mutex_unlock(&serial_num_lock);
}


pkg_repeat_status g_pkg_status;
pkg_repeat_status *g_pkg_status_p = &g_pkg_status;

void send_stat_pkg_init()
{
    memset(g_pkg_status_p, 0, sizeof(pkg_repeat_status));
}

//return if error happened, or data write over
static void package_write(int sock, uint8_t *buf, int len)
{
    int ret = 0;
    int offset = 0;

    if(sock < 0 || len < 0 || !buf){
        return ;
    } else{
        while(offset < len)
        {
            ret = write(sock, &buf[offset], len-offset);
            if(ret <= 0){
                //write error deal
                perror("tcp write:");
                if(errno == EINTR || errno == EAGAIN){
                    printf("package write wait mommemt, continue!\n");
                    usleep(10000);
                    continue;
                }else
                    return ;
                    //return -1;
            }else{
                offset += ret;
            }
        }
    }
    //return offset;
    return ;
}
static uint8_t sample_calc_sum(SBProtHeader *pHeader, int32_t msg_len)
{
    int32_t i = 0;
    uint32_t chksum = 0;
    uint8_t * start = (uint8_t *) &pHeader->vendor_id;

#define NON_CRC_LEN (2 * sizeof(pHeader->magic) /*head and tail*/ + \
        sizeof(pHeader->serial_num) + \
        sizeof(pHeader->checksum))

    for (i = 0; i < int32_t(msg_len - NON_CRC_LEN); i++)
    {
        chksum += start[i];

        //   MY_DEBUG("#%04d 0x%02hhx = 0x%08x\n", i, start[i], chksum);
    }
    return (uint8_t) (chksum & 0xFF);
}
static int32_t sample_escaple_msg(SBProtHeader *pHeader, int32_t msg_len)
{
    int32_t i = 0;
    int32_t escaped_len = msg_len;
    uint8_t *barray = (uint8_t*) pHeader;

    //ignore head/tail magic
    for (i = 1; i < escaped_len - 1; i++)
    {
        if (SAMPLE_PROT_MAGIC == barray[i]) 
        {
            memmove(&barray[i+1], &barray[i], escaped_len - i);
            barray[i] = SAMPLE_PROT_ESC_CHAR;
            barray[i+1] = 0x2;
            i++;
            escaped_len ++;
        }
        else if (SAMPLE_PROT_ESC_CHAR == barray[i]) 
        {
            memmove(&barray[i+1], &barray[i], escaped_len - i);
            barray[i]   = SAMPLE_PROT_ESC_CHAR;
            barray[i+1] = 0x1;
            i++;
            escaped_len ++;
        }
    }
    return escaped_len;
}
//push到发送队列
static int32_t message_queue_send(SBProtHeader *pHeader, uint8_t devid, uint8_t cmd,uint8_t *payload, int32_t payload_len)
{
    static uint8_t s_index = 0;
    uint16_t serial_num = 0;
    char MasterIsMe = 0;
    ptr_queue_node msg;
    int32_t msg_len = sizeof(*pHeader) + 1 + payload_len;
    uint8_t *data = ((uint8_t*) pHeader + sizeof(*pHeader));
    uint8_t *tail = data + payload_len;

    switch(cmd)
    {
        case SAMPLE_CMD_QUERY:
        case SAMPLE_CMD_FACTORY_RESET:
        case SAMPLE_CMD_SPEED_INFO:        
        case SAMPLE_CMD_DEVICE_INFO:
        case SAMPLE_CMD_UPGRADE:
        case SAMPLE_CMD_GET_PARAM:
        case SAMPLE_CMD_SET_PARAM:
        case SAMPLE_CMD_REQ_STATUS:
        case SAMPLE_CMD_REQ_MM_DATA:
            msg.pkg.ack_status = MSG_ACK_READY;
            MasterIsMe = 0;
            break;

            //send as master
        case SAMPLE_CMD_WARNING_REPORT:
        case SAMPLE_CMD_UPLOAD_MM_DATA:
        case SAMPLE_CMD_UPLOAD_STATUS:
            MasterIsMe = 1;
            msg.pkg.ack_status = MSG_ACK_WAITING;
            break;

        default:
            msg.pkg.ack_status = MSG_ACK_READY;
            break;
    }

    memset(pHeader, 0, sizeof(*pHeader));
    pHeader->magic = SAMPLE_PROT_MAGIC;

    //如果当前发送的数据是主动发送，即需要ACK的，序列号就直接累加
    if(MasterIsMe)
        do_serial_num(&serial_num, GET_NEXT_SEND_NUM);
    else
        do_serial_num(&serial_num, GET_RECV_NUM);

    pHeader->serial_num= MY_HTONS(serial_num); //used as message cnt
    pHeader->vendor_id= MY_HTONS(VENDOR_ID);
    //pHeader->device_id= SAMPLE_DEVICE_ID_ADAS;

#if defined ENABLE_ADAS
    pHeader->device_id= SAMPLE_DEVICE_ID_ADAS;
#elif defined ENABLE_DMS
    pHeader->device_id= SAMPLE_DEVICE_ID_DMS;
#endif
    pHeader->cmd = cmd;

    if (payload_len > 0) 
    {
        memcpy(data, payload, payload_len);
    }
    tail[0] = SAMPLE_PROT_MAGIC;

    pHeader->checksum = sample_calc_sum(pHeader, msg_len);
    msg_len = sample_escaple_msg(pHeader, msg_len);

    msg.pkg.cmd = cmd;
    msg.pkg.index = (s_index++)%0xFF;
    msg.pkg.send_repeat = 0;
    msg.len = msg_len;
    msg.buf = (uint8_t *)pHeader;
    //    printf("sendpackage cmd = 0x%x,msg.need_ack = %d, len=%d, push!\n",msg.cmd, msg.need_ack, msg.len);
    ptr_queue_push(g_send_q_p, &msg, &ptr_queue_lock);

    printf("push cmd = 0x%x, index = %d\n", msg.pkg.cmd, msg.pkg.index);
    sem_post(&send_data);
    //printf("push queue, len = %d\n", msg.len);

    return msg_len;
}


//填写报警信息的一些实时数据
void using_can760_msg(AdasWarnFrame *uploadmsg)
{
    CAN760Info carinfo;
    can760_message_process(&carinfo, READ_REAL_TIME_MSG);
    uploadmsg->car_speed = carinfo.speed;
    printf("using can760 speed = %d\n", carinfo.speed);
}


//填写报警信息的一些实时数据
void get_real_time_msg(AdasWarnFrame *uploadmsg)
{
    RealTimeData tmp;
    RealTimeDdata_process(&tmp, READ_REAL_TIME_MSG);

    uploadmsg->altitude = tmp.altitude;
    uploadmsg->latitude = tmp.latitude;
    uploadmsg->longitude = tmp.longitude;
    uploadmsg->car_speed = tmp.car_speed;


    memcpy(uploadmsg->time, tmp.time, sizeof(uploadmsg->time));
    memcpy(&uploadmsg->car_status, &tmp.car_status, sizeof(uploadmsg->car_status));

#if defined ENABLE_ADAS
#ifdef USE_CAN760_MESSAGE
    using_can760_msg(uploadmsg);
#endif
#endif
}



void get_adas_Info_for_store(uint8_t type, InfoForStore *mm_store)
{
    AdasParaSetting para;

    read_dev_para(&para, SAMPLE_DEVICE_ID_ADAS);
    mm_store->warn_type = type;
    switch(type)
    {
        case SB_WARN_TYPE_FCW:
        case SB_WARN_TYPE_LDW:
        case SB_WARN_TYPE_HW:
        case SB_WARN_TYPE_PCW:
        case SB_WARN_TYPE_FLC:
            if(type == SB_WARN_TYPE_FCW){
                mm_store->photo_num = para.FCW_photo_num;
                mm_store->photo_time_period = para.FCW_photo_time_period;
                mm_store->video_time = para.FCW_video_time;
            }else if(type == SB_WARN_TYPE_LDW){
                mm_store->photo_num = para.LDW_photo_num;
                mm_store->photo_time_period = para.LDW_photo_time_period;
                mm_store->video_time = para.LDW_video_time;
            }else if(type == SB_WARN_TYPE_HW){
                mm_store->photo_num = para.HW_photo_num;
                mm_store->photo_time_period = para.HW_photo_time_period;
                mm_store->video_time = para.HW_video_time;
            }else if(type == SB_WARN_TYPE_PCW){
                mm_store->photo_num = para.PCW_photo_num;
                mm_store->photo_time_period = para.PCW_photo_time_period;
                mm_store->video_time = para.PCW_video_time;
            }else if(type == SB_WARN_TYPE_FLC){
                mm_store->photo_num = para.FLC_photo_num;
                mm_store->photo_time_period = para.FLC_photo_time_period;
                mm_store->video_time = para.FLC_video_time;
            }else{
                ;
            }
            if(mm_store->video_time != 0)
                mm_store->video_enable = 1; 
            else
                mm_store->video_enable = 0; 

            if((mm_store->photo_num != 0 && mm_store->photo_num < WARN_SNAP_NUM_MAX))
                mm_store->photo_enable = 1; 
            else
                mm_store->photo_enable = 0; 
        case SB_WARN_TYPE_TSRW:
        case SB_WARN_TYPE_TSR:
            break;

        case SB_WARN_TYPE_SNAP:
            mm_store->photo_num = para.photo_num;
            mm_store->photo_time_period = para.photo_time_period;
            mm_store->photo_enable = 1; 

        default:
            break;
    }
}

void get_dms_Info_for_store(uint8_t type, InfoForStore *mm_store)
{
    DmsParaSetting para;

    read_dev_para(&para, SAMPLE_DEVICE_ID_DMS);
    mm_store->warn_type = type;
    switch(type)
    {
        case DMS_FATIGUE_WARN:
        case DMS_CALLING_WARN:
        case DMS_SMOKING_WARN:
        case DMS_DISTRACT_WARN:
        case DMS_ABNORMAL_WARN:

            if(type == DMS_FATIGUE_WARN){
                mm_store->photo_num = para.FatigueDriv_PhotoNum;
                mm_store->photo_time_period = para.FatigueDriv_PhotoInterval;
                mm_store->video_time = para.FatigueDriv_VideoTime;
            }else if(type == DMS_CALLING_WARN){
                mm_store->photo_num = para.CallingDriv_PhotoNum;
                mm_store->photo_time_period = para.CallingDriv_PhotoInterval;
                mm_store->video_time = para.CallingDriv_VideoTime;
            }else if(type == DMS_SMOKING_WARN){
                mm_store->photo_num = para.SmokingDriv_PhotoNum;
                mm_store->photo_time_period = para.SmokingDriv_PhotoInterval;
                mm_store->video_time = para.SmokingDriv_VideoTime;
            }else if(type == DMS_DISTRACT_WARN){
                mm_store->photo_num = para.DistractionDriv_PhotoNum;
                mm_store->photo_time_period = para.DistractionDriv_PhotoInterval;
                mm_store->video_time = para.DistractionDriv_VideoTime;
            }else if(type == DMS_ABNORMAL_WARN){
                mm_store->photo_num = para.AbnormalDriv_PhotoNum;
                mm_store->photo_time_period = para.AbnormalDriv_PhotoInterval;
                mm_store->video_time = para.AbnormalDriv_VideoTime;
            }else{
                ;
            }
            if(mm_store->video_time != 0)
                mm_store->video_enable = 1; 
            else
                mm_store->video_enable = 0; 

            if((mm_store->photo_num != 0 && mm_store->photo_num < WARN_SNAP_NUM_MAX))
                mm_store->photo_enable = 1; 
            else
                mm_store->photo_enable = 0; 


        case DMS_DRIVER_CHANGE:
            break;

        case DMS_SANPSHOT_EVENT:
            mm_store->photo_num = para.photo_num;
            mm_store->photo_time_period = para.photo_time_period;
            mm_store->photo_enable = 1; 

        default:
            break;
    }
}


int filter_alert_by_time(time_t *last, unsigned int secs)
{
    struct timespec tv;

#ifndef FILTER_ALERT_BY_TIME
    return 1;
#endif

	clock_gettime(CLOCK_MONOTONIC, &tv);
    if (tv.tv_sec - (*last) < secs){
     
        printf("filter alert by time(%dsec), last = %ld, now = %ld\n", secs, *last, tv.tv_sec);
        return 0;
    }

    *last = tv.tv_sec;

    return 1;
}

int filter_alert_by_speed()
{
    RealTimeData tmp;
    AdasParaSetting para;
    CAN760Info carinfo;

   // return 1;
#ifndef FILTER_ALERT_BY_SPEED
    return 1;
#endif

    read_dev_para(&para, SAMPLE_DEVICE_ID_ADAS);

    RealTimeDdata_process(&tmp, READ_REAL_TIME_MSG);

#if defined ENABLE_ADAS
#ifdef USE_CAN760_MESSAGE
    can760_message_process(&carinfo, READ_REAL_TIME_MSG);
    tmp.car_speed = carinfo.speed;
#endif
#endif

    if(tmp.car_speed <= para.warning_speed_val)
    {
        printf("alert filter by speed(%d),cur_speed(%d)!\n", para.warning_speed_val, tmp.car_speed);
        return 0;
    }

    return 1;
}


void mmid_to_filename(uint32_t id, uint8_t type, char *filepath)
{
    sprintf(filepath,"%s%010u", SNAP_SHOT_JPEG_PATH, id);
}


/*********************************
 *产生新的报警之前，先尝试把之前的同名文件删除。
 *
 *******************************/
void filter_media_num(InfoForStore *mm)
{
    char filepath[100];
    int i;
    if(mm->photo_enable){
        for(i=0; i<mm->photo_num; i++){
            if(mm->photo_id[i] > SAVE_MEDIA_NUM_MAX){
                mmid_to_filename(mm->photo_id[i]-SAVE_MEDIA_NUM_MAX, 0, filepath);
                printf("delete old media:%s\n", filepath);
                remove(filepath);
            }
        }
    }
    if(mm->video_enable){
        if( mm->video_id[0] > SAVE_MEDIA_NUM_MAX){
            mmid_to_filename(mm->video_id[0]-SAVE_MEDIA_NUM_MAX, 0, filepath);
            printf("delete old media:%s\n", filepath);
            remove(filepath);
        }
    }
}

void record_alert_log(uint8_t time[6], int type)
{
    char logbuf[256];
    //write log
    snprintf(logbuf, sizeof(logbuf), "[%d-%d-%d %d:%d:%d] warn_type:%d",\
            time[0],\
            time[1],\
            time[2],\
            time[3],\
            time[4],\
            time[5],\
            type); 
    data_log(logbuf);
    
}

/*********************************
* func: build adas warning package
* return: framelen
*********************************/
int build_adas_warn_frame(int type, char status_flag, AdasWarnFrame *uploadmsg)
{
    int i=0;
    InfoForStore mm;
    AdasParaSetting para;

    read_dev_para(&para, SAMPLE_DEVICE_ID_ADAS);
    memset(&mm, 0, sizeof(mm));
    get_adas_Info_for_store(type, &mm);

    get_local_time(uploadmsg->time);
    record_alert_log(uploadmsg->time, type);

    uploadmsg->status_flag = status_flag;
    uploadmsg->warning_id = MY_HTONL(get_next_id(WARNING_ID_MODE, NULL, 0));
    uploadmsg->sound_type = type;
    uploadmsg->mm_num = 0;
    get_real_time_msg(uploadmsg);

    if(status_flag == SB_WARN_STATUS_END){
        goto out;
    }
    

    switch(type)
    {
        case SB_WARN_TYPE_FCW:
        case SB_WARN_TYPE_LDW:
        case SB_WARN_TYPE_HW:
        case SB_WARN_TYPE_PCW:
        case SB_WARN_TYPE_FLC:
            if(mm.photo_enable){
                get_next_id(MM_ID_MODE, mm.photo_id, mm.photo_num);
                for(i=0; i<mm.photo_num; i++){
                    //printf("mm.photo_id[%d] = %d\n", i, mm.photo_id[i]);
                    uploadmsg->mm_num++;
                    uploadmsg->mm[i].type = MM_PHOTO;
                    uploadmsg->mm[i].id = MY_HTONL(mm.photo_id[i]);
                }
            }
            if(mm.video_enable){
                get_next_id(MM_ID_MODE, mm.video_id, 1);
                uploadmsg->mm_num++;
                uploadmsg->mm[i].type = MM_VIDEO;
                uploadmsg->mm[i].id = MY_HTONL(mm.video_id[0]);
                i++;
            }
            mm.get_another_camera_video= 0;
            filter_media_num(&mm);
            push_mm_queue(&mm);
#if defined SAVE_ANOTHER_CAMERA_VIDEO
            //add dms video
            if(mm.video_enable){
                mm.video_enable = 1; 
                mm.photo_enable = 0; 
                if(mm.video_enable){
                    get_next_id(MM_ID_MODE, mm.video_id, 1);
                    uploadmsg->mm_num++;
                    uploadmsg->mm[i].type = MM_VIDEO;
                    uploadmsg->mm[i].id = MY_HTONL(mm.video_id[0]);
                }
                mm.get_another_camera_video= 1;
                filter_media_num(&mm);
                push_mm_queue(&mm);
            }
#endif
            break;

        case SB_WARN_TYPE_TSRW:
        case SB_WARN_TYPE_TSR:
            break;

        case SB_WARN_TYPE_SNAP:
            //printf("snap: enable\n");
            if(mm.photo_enable){
                mm.warn_type = type;
                get_next_id(MM_ID_MODE, mm.photo_id, mm.photo_num);
                for(i=0; i<mm.photo_num; i++){
                    uploadmsg->mm_num++;
                    uploadmsg->mm[i].type = MM_PHOTO;
                    uploadmsg->mm[i].id = MY_HTONL(mm.photo_id[i]);
                }
                mm.get_another_camera_video= 0;
                filter_media_num(&mm);
                push_mm_queue(&mm);
            }
            break;
        default:
            break;
    }
out:
    return (sizeof(*uploadmsg) + uploadmsg->mm_num*sizeof(SBMmHeader));
}

/*********************************
* func: build dms warning package
* return: framelen
*********************************/
int build_dms_warn_frame(int type, char status_flag, DsmWarnFrame *uploadmsg)
{
    int i=0;
    InfoForStore mm;
    DmsParaSetting para;
    RealTimeData tmp;

    read_dev_para(&para, SAMPLE_DEVICE_ID_DMS);
    memset(&mm, 0, sizeof(mm));
    get_dms_Info_for_store(type, &mm);

    uploadmsg->status_flag = status_flag;
    uploadmsg->warning_id = MY_HTONL(get_next_id(WARNING_ID_MODE, NULL, 0));
    uploadmsg->sound_type = type;
    uploadmsg->mm_num = 0;

    get_local_time(uploadmsg->time);
    record_alert_log(uploadmsg->time, type);

    RealTimeDdata_process(&tmp, READ_REAL_TIME_MSG);
    uploadmsg->altitude = tmp.altitude;
    uploadmsg->latitude = tmp.latitude;
    uploadmsg->longitude = tmp.longitude;
    uploadmsg->car_speed = tmp.car_speed;
    memcpy(uploadmsg->time, tmp.time, sizeof(uploadmsg->time));
    memcpy(&uploadmsg->car_status, &tmp.car_status, sizeof(uploadmsg->car_status));

    if(status_flag == SB_WARN_STATUS_END){
        goto out;
    }

    switch(type)
    {
        case DMS_FATIGUE_WARN:
        case DMS_CALLING_WARN:
        case DMS_SMOKING_WARN:
        case DMS_DISTRACT_WARN:
        case DMS_ABNORMAL_WARN:
            
            if(mm.photo_enable){
                WSI_DEBUG("dms photo_enbale! num = %d\n", mm.photo_num);
                get_next_id(MM_ID_MODE, mm.photo_id, mm.photo_num);
                for(i=0; i<mm.photo_num; i++){
                    //printf("mm.photo_id[%d] = %d\n", i, mm.photo_id[i]);
                    uploadmsg->mm_num++;
                    uploadmsg->mm[i].type = MM_PHOTO;
                    uploadmsg->mm[i].id = MY_HTONL(mm.photo_id[i]);
                }
            }
            if(mm.video_enable){
                WSI_DEBUG("dms video_enbale!\n");
                get_next_id(MM_ID_MODE, mm.video_id, 1);
                uploadmsg->mm_num++;
                uploadmsg->mm[i].type = MM_VIDEO;
                uploadmsg->mm[i].id = MY_HTONL(mm.video_id[0]);
                i++;
            }
            mm.get_another_camera_video= 0;
            filter_media_num(&mm);
            push_mm_queue(&mm);
            
            WSI_DEBUG("num2 = %d\n", uploadmsg->mm_num);

#if defined SAVE_ANOTHER_CAMERA_VIDEO
            //add  video
            if(mm.video_enable){
                mm.video_enable = 1; 
                mm.photo_enable = 0; 
                if(mm.video_enable){
                    get_next_id(MM_ID_MODE, mm.video_id, 1);
                    uploadmsg->mm_num++;
                    uploadmsg->mm[i].type = MM_VIDEO;
                    uploadmsg->mm[i].id = MY_HTONL(mm.video_id[0]);
                }
                mm.get_another_camera_video= 1;
                filter_media_num(&mm);
                push_mm_queue(&mm);
                WSI_DEBUG("num3 = %d\n", uploadmsg->mm_num);
            }
#endif
            break;

        case DMS_DRIVER_CHANGE:
            break;

        case DMS_SANPSHOT_EVENT:
            if(mm.photo_enable){
                mm.warn_type = type;
                get_next_id(MM_ID_MODE, mm.photo_id, mm.photo_num);
                for(i=0; i<mm.photo_num; i++){
                    uploadmsg->mm_num++;
                    uploadmsg->mm[i].type = MM_PHOTO;
                    uploadmsg->mm[i].id = MY_HTONL(mm.photo_id[i]);
                }
                mm.get_another_camera_video= 0;
                filter_media_num(&mm);
                push_mm_queue(&mm);
            }
            break;

        default:
            break;
    }
out:
    return (sizeof(*uploadmsg) + uploadmsg->mm_num*sizeof(SBMmHeader));
}

#if 1
void deal_wsi_dms_info(WsiFrame *can)
{
#if 1
    char status_flag = SB_WARN_STATUS_NONE;
    DmsAlertInfo msg;
    uint32_t playloadlen = 0;
    uint8_t msgbuf[512];
    uint8_t txbuf[512];
    uint8_t i=0;
    int alert_type = 0;
    static time_t dms_fatigue_warn = 0;
    static time_t dms_distract_warn = 0;
    static time_t dms_calling_warn = 0;
    static time_t dms_smoking_warn = 0;
    static time_t dms_abnormal_warn = 0;

    static uint8_t dms_alert_last[8] = {0,0,0,0,0,0,0,0};
    uint8_t dms_alert[8] = {0,0,0,0,0,0,0,0};
    uint8_t dms_alert_mask[8] = {0xFF,0xFF,0,0,0,0,0,0};

    SBProtHeader *pSend = (SBProtHeader *) txbuf;
    DsmWarnFrame *uploadmsg = (DsmWarnFrame *)&msgbuf[0];

#if 0
    printf("soure: %s\n", can->source);
    printf("time: %ld\n", can->time);
    printf("topic: %s\n", can->topic);
    printbuf(can->warning, sizeof(can->warning));
#endif
    //printbuf(can->warning, sizeof(can->warning));

    memcpy(&msg, can->warning, sizeof(can->warning));

    for(i=0; i<sizeof(can->warning); i++){
        dms_alert[i] = can->warning[i] & dms_alert_mask[i];
    }

    //filter the same alert
    if(!memcmp(dms_alert, dms_alert_last, sizeof(dms_alert))){
        //printf("alert is the same!\n");
        goto out;
    }


#if 0
    printf("msg.alert_eye_close1 %d\n", msg.alert_eye_close1);
    printf("msg.alert_eye_close2 %d\n", msg.alert_eye_close2);
    printf("msg.alert_look_around %d\n", msg.alert_look_around);
    printf("msg.alert_yawn %d\n", msg.alert_yawn);
    printf("msg.alert_phone %d\n", msg.alert_phone);
    printf("msg.alert_smoking %d\n", msg.alert_smoking);
    printf("msg.alert_absence %d\n", msg.alert_absence);
    printf("msg.alert_bow %d\n", msg.alert_bow);
#endif

    //按照优先级检查
    if(msg.alert_eye_close1 || msg.alert_eye_close2 || msg.alert_yawn || msg.alert_bow){
        alert_type = DMS_FATIGUE_WARN;
        if(!filter_alert_by_time(&dms_fatigue_warn, FILTER_DMS_ALERT_SET_TIME)){
            goto out;
        }
        if(!filter_alert_by_speed())
            goto out;
    }else if (msg.alert_look_around ){
        alert_type = DMS_DISTRACT_WARN;
        if(!filter_alert_by_time(&dms_distract_warn, FILTER_DMS_ALERT_SET_TIME)){
            goto out;
        }
        if(!filter_alert_by_speed())
            goto out;
    }else if(msg.alert_phone){
        alert_type = DMS_CALLING_WARN;
        if(!filter_alert_by_time(&dms_calling_warn, FILTER_DMS_ALERT_SET_TIME)){
            goto out;
        }
        if(!filter_alert_by_speed())
            goto out;
    }else if(msg.alert_smoking){
        alert_type = DMS_SMOKING_WARN;
        if(!filter_alert_by_time(&dms_smoking_warn, FILTER_DMS_ALERT_SET_TIME)){
            goto out;
        }
        if(!filter_alert_by_speed())
            goto out;
    }else if(msg.alert_absence){
        alert_type = DMS_ABNORMAL_WARN;
        //status_flag = SB_WARN_STATUS_BEGIN;
        if(!filter_alert_by_time(&dms_abnormal_warn, FILTER_DMS_ALERT_SET_TIME)){
            goto out;
        }
        if(!filter_alert_by_speed())
            goto out;
    }else
        goto out;

#if 1
    playloadlen = build_dms_warn_frame(alert_type, status_flag, uploadmsg);
    printf("send dms alert %d!\n", alert_type);
    printf("dms alert frame len = %ld\n", sizeof(*uploadmsg));
    //printbuf((uint8_t *)uploadmsg, playloadlen);
    message_queue_send(pSend, \
            SAMPLE_DEVICE_ID_DMS,\
            SAMPLE_CMD_WARNING_REPORT,\
            (uint8_t *)uploadmsg,\
            playloadlen);
#endif

out:
    memcpy(dms_alert_last, dms_alert, sizeof(dms_alert));
    return;
#endif
}
#else
void deal_wsi_dms_info2(dms_can_779 *msg)
{
    uint32_t playloadlen = 0;
    uint8_t msgbuf[512];
    uint8_t txbuf[512];
    uint8_t i=0;
    int alert_type = 0;
    static unsigned int dms_fatigue_warn = 0;
    static unsigned int dms_distract_warn = 0;
    static unsigned int dms_calling_warn = 0;
    static unsigned int dms_smoking_warn = 0;
    static unsigned int dms_abnormal_warn = 0;

    static uint8_t dms_alert_last[8] = {0,0,0,0,0,0,0,0};
    uint8_t dms_alert[8] = {0,0,0,0,0,0,0,0};
    uint8_t dms_alert_mask[8] = {0xFF,0xFF,0,0,0,0,0,0};

    SBProtHeader *pSend = (SBProtHeader *) txbuf;
    DsmWarnFrame *uploadmsg = (DsmWarnFrame *)&msgbuf[0];

    if(!filter_alert_by_speed())
        goto out;

    memcpy(&dms_alert, msg, sizeof(dms_can_779));
    for(i=0; i<sizeof(dms_can_779); i++){
        dms_alert[i] &= dms_alert_mask[i];
    }

    //filter the same alert
    if(!memcmp(dms_alert, dms_alert_last, sizeof(dms_alert))){
        printf("filter the same alert!\n");
        goto out;
    }

    if(msg->Eye_Closure_Warning || msg->Yawn_warning){
        alert_type = DMS_FATIGUE_WARN;
        if(!filter_alert_by_time(&dms_fatigue_warn, FILTER_DMS_ALERT_SET_TIME)){
      //      printf("dms filter alert by time!");
            goto out;
        }
    }else if (msg->Look_up_warning || msg->Look_around_warning || msg->Look_down_warning){
        alert_type = DMS_DISTRACT_WARN;
        if(!filter_alert_by_time(&dms_distract_warn, FILTER_DMS_ALERT_SET_TIME)){
        //    printf("dms filter alert by time!");
            goto out;
        }
    }else if(msg->Phone_call_warning){
        alert_type = DMS_CALLING_WARN;
        if(!filter_alert_by_time(&dms_calling_warn, FILTER_DMS_ALERT_SET_TIME)){
          //  printf("dms filter alert by time!");
            goto out;
        }
    }else if(msg->Smoking_warning){
        alert_type = DMS_SMOKING_WARN;
        if(!filter_alert_by_time(&dms_smoking_warn, FILTER_DMS_ALERT_SET_TIME)){
            //printf("dms filter alert by time!");
            goto out;
        }
    }else if(msg->Absence_warning){
        alert_type = DMS_ABNORMAL_WARN;
        if(!filter_alert_by_time(&dms_abnormal_warn, FILTER_DMS_ALERT_SET_TIME)){
            //printf("dms filter alert by time!");
            goto out;
        }
    }else
        goto out;

    playloadlen = build_dms_warn_frame(alert_type, status_flag, uploadmsg);
    WSI_DEBUG("send dms alert %d!\n", alert_type);
    WSI_DEBUG("dms alert frame len = %ld\n", sizeof(*uploadmsg));
    printbuf((uint8_t *)uploadmsg, playloadlen);
    message_queue_send(pSend, \
            SAMPLE_DEVICE_ID_DMS,\
            SAMPLE_CMD_WARNING_REPORT,\
            (uint8_t *)uploadmsg,\
            playloadlen);

out:
    memcpy(dms_alert_last, dms_alert, sizeof(dms_alert));
    return;
}

#endif

#if 0
#else
static int send_package(int sock, uint8_t *buf)
{
    int rc = 0;
    int len = 0;
    int i = 0;
    struct timespec ts;
    if(sock < 0)
    {
        printf("sock error\n");
        return -1;
    }

    ptr_queue_node header;
    header.buf = buf;
    header.len = PTR_QUEUE_BUF_SIZE;

    //fail
    if(ptr_queue_pop(g_send_q_p, &header, &ptr_queue_lock)){
        
        printf("queue no mesg\n");
        goto out;
    }

    WSI_DEBUG("[send] pop: cmd = 0x%x, index = %d, header len = %d\n", header.pkg.cmd,header.pkg.index, header.len);
    //printbuf(header.buf, header.len);
    if(header.pkg.ack_status == MSG_ACK_READY){// no need ack
        printf("no need ack!\n");
        package_write(sock, header.buf, header.len);
        goto out;
    }
    if(header.pkg.ack_status == MSG_ACK_WAITING){
        for(i = 0; i < 3; i++){
            printf("ack waiting!\n");
            package_write(sock, header.buf, header.len);
            header.pkg.send_repeat++;

            //printf("get lock\n");
            pthread_mutex_lock(&recv_ack_mutex);
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec += 2;
            rc = 0;
            while ((recv_ack != NOTICE_MSG) && rc == 0){
                //printf("cond_wait\n");
                rc = pthread_cond_timedwait(&recv_ack_cond, &recv_ack_mutex, &ts);
                //printf("rc == %d\n", rc);
            }
            if (rc == 0){
                recv_ack= WAIT_MSG;//clear
                printf("cond_wait get ack..\n");
            }else if(rc == ETIMEDOUT){//timeout
                printf("recv ack timeout! cnt = %d\n", header.pkg.send_repeat);
            }else{
                printf("recv error! cnt = %d\n", header.pkg.send_repeat);
            }
            pthread_mutex_unlock(&recv_ack_mutex);
            if (rc == 0){
                break;
            }
            if(header.pkg.send_repeat >= 3){//第一次发送
                printf("send three times..\n");
                g_pkg_status_p->mm_data_trans_waiting = 0;
                break;
            }
        }
    }

out:
    return 0;
}
#endif


void notice_tcp_send_exit()
{
    sem_post(&send_data);
}


void *pthread_tcp_send(void *para)
{
    uint8_t *writebuf = NULL;

    if(!setcondattr(&recv_ack_cond)){
        printf("setcondattr sucess!\n");
    }

    writebuf = (uint8_t *)malloc(PTR_QUEUE_BUF_SIZE);
    if(!writebuf){
        perror("send pkg malloc");
        goto out;
    }

    while (!force_exit) {
        printf("sem waiting...\n");
        sem_wait(&send_data);
        //send_pkg_to_host(hostsock, writebuf);
        send_package(hostsock, writebuf);
    }
out:
    if(writebuf)
        free(writebuf);

    pthread_exit(NULL);
}



void send_snap_shot_ack(SBProtHeader *pHeader, int32_t len)
{
    uint8_t txbuf[256] = {0};
    uint8_t ack = 0;
    SBProtHeader *pSend = (SBProtHeader *) txbuf;

    if(len == sizeof(SBProtHeader) + 1)
    {
        message_queue_send(pSend, pHeader->device_id, SAMPLE_CMD_SNAP_SHOT, (uint8_t *)&ack, 1);
    }
    else
    {
        printf("recv cmd:0x%x, data len maybe error!\n", pHeader->cmd);
    }
}

int do_snap_shot()
{
    uint32_t playloadlen = 0;
    uint8_t msgbuf[512];
    uint8_t txbuf[512];
    SBProtHeader *pSend = (SBProtHeader *) txbuf;

#if defined ENABLE_DMS
    DsmWarnFrame *uploadmsg = (DsmWarnFrame *)&msgbuf[0];
    playloadlen = build_dms_warn_frame(DMS_SANPSHOT_EVENT, SB_WARN_STATUS_NONE, uploadmsg);
    message_queue_send(pSend, \
            SAMPLE_DEVICE_ID_DMS,\
            SAMPLE_CMD_WARNING_REPORT,\
            (uint8_t *)uploadmsg,\
            playloadlen);

#elif defined ENABLE_ADAS
    AdasWarnFrame *uploadmsg = (AdasWarnFrame *)&msgbuf[0];
    playloadlen = build_adas_warn_frame(SB_WARN_TYPE_SNAP, SB_WARN_STATUS_NONE, uploadmsg);
    printf("sanp len = %d\n", playloadlen);
    message_queue_send(pSend, \
            SAMPLE_DEVICE_ID_ADAS,\
            SAMPLE_CMD_WARNING_REPORT,\
            (uint8_t *)uploadmsg,\
            playloadlen);
#endif

    return 0;
}

void set_BCD_time(AdasWarnFrame *uploadmsg, uint64_t usec)
{
    struct tm *p = NULL; 
    time_t timep = 0;   
    printf("time:%ld\n", usec);
    //timep = strtoul(second, NULL, 10);
    //timep = timep/1000000;
    timep = usec/1000000;
    p = localtime(&timep);
    uploadmsg->time[0] = (p->tm_year+1900)%100;
    uploadmsg->time[1] = p->tm_mon+1;
    uploadmsg->time[2] = p->tm_mday;
    uploadmsg->time[3] = p->tm_hour;
    uploadmsg->time[4] = p->tm_min;
    uploadmsg->time[5] = p->tm_sec;

    printf("%d-%d-%d %d:%d:%d\n", (1900 + p->tm_year), ( 1 + p->tm_mon), p->tm_mday,(p->tm_hour), p->tm_min, p->tm_sec); 
}


static MECANWarningMessage g_last_warning_data;
static MECANWarningMessage g_last_can_msg;
static uint8_t g_last_trigger_warning[sizeof(MECANWarningMessage)];
int deal_wsi_adas_can700(WsiFrame *sourcecan)
{
    uint32_t warning_id = 0;
    uint8_t msgbuf[512];
    uint32_t playloadlen = 0;
    AdasWarnFrame *uploadmsg = (AdasWarnFrame *)&msgbuf[0];
    MECANWarningMessage can;
    uint8_t txbuf[512];
    SBProtHeader *pSend = (SBProtHeader *) txbuf;
    static time_t hw_alert = 0;
    static time_t fcw_alert = 0;
    static time_t ldw_alert = 0;
    char logbuf[256];

    uint32_t i = 0;
    uint8_t all_warning_masks[sizeof(MECANWarningMessage)] = {
        0x00, 0x00, 0x01, 0x00,
        0x0E, 0x00, 0x00, 0x03};
    uint8_t trigger_warning_masks[sizeof(MECANWarningMessage)] = {
        0x00, 0x00, 0x00, 0x00,
        0x0E, 0x00, 0x00, 0x00};
    uint8_t all_warning_data[sizeof(MECANWarningMessage)] = {0};
    uint8_t trigger_data[sizeof(MECANWarningMessage)] = {0};

#if 0
        snprintf(logbuf, sizeof(logbuf), "%x %x %x %x %x %x %x %x",\
                sourcecan->warning[0],\
                sourcecan->warning[1],\
                sourcecan->warning[2],\
                sourcecan->warning[3],\
                sourcecan->warning[4],\
                sourcecan->warning[5],\
                sourcecan->warning[6],\
                sourcecan->warning[7],\
                sourcecan->warning[8]);  
        data_log(logbuf);
#endif
        //printbuf(sourcecan->warning, 8);
        memcpy(&can, sourcecan->warning, sizeof(sourcecan->warning));

        for (i = 0; i < sizeof(all_warning_masks); i++) {
            all_warning_data[i] = sourcecan->warning[i] & all_warning_masks[i];
            trigger_data[i]     = sourcecan->warning[i] & trigger_warning_masks[i];
        }

        if (0 == memcmp(all_warning_data, &g_last_warning_data, sizeof(g_last_warning_data))) {
            return 0;
        }

        //filter alert
        if(!filter_alert_by_speed())
            goto out;

        //LDW and FCW event
        if (0 != memcmp(trigger_data, &g_last_trigger_warning, sizeof(g_last_trigger_warning)) && 0 != trigger_data[4]) {
            if (can.left_ldw || can.right_ldw) {
                printf("------LDW event-----------\n");
                if(!filter_alert_by_time(&ldw_alert, FILTER_ADAS_ALERT_SET_TIME)){
                    printf("ldw filter alert by time!\n");
                }else{
                    memset(uploadmsg, 0, sizeof(*uploadmsg));
                    playloadlen = build_adas_warn_frame(SB_WARN_TYPE_LDW,SB_WARN_STATUS_NONE, uploadmsg);

                    if(can.left_ldw)
                        uploadmsg->ldw_type = SOUND_TYPE_LLDW;
                    if(can.right_ldw)
                        uploadmsg->ldw_type = SOUND_TYPE_RLDW;

                    WSI_DEBUG("send LDW alert message!\n");
                    message_queue_send(pSend,SAMPLE_DEVICE_ID_ADAS, SAMPLE_CMD_WARNING_REPORT,\
                            (uint8_t *)uploadmsg, \
                            playloadlen);
                }
            }
            if (can.fcw_on) {
                printf("------FCW event-----------\n");
                if(!filter_alert_by_time(&fcw_alert, FILTER_ADAS_ALERT_SET_TIME)){
                    printf("ldw filter alert by time!\n");
                }else{
                    playloadlen = build_adas_warn_frame(SB_WARN_TYPE_FCW,SB_WARN_STATUS_NONE, uploadmsg);
                    uploadmsg->sound_type = SB_WARN_TYPE_FCW;

                    WSI_DEBUG("send FCW alert message!\n");
                    message_queue_send(pSend,SAMPLE_DEVICE_ID_ADAS, SAMPLE_CMD_WARNING_REPORT,\
                            (uint8_t *)uploadmsg, \
                            playloadlen);
                }
            }
        }
        //Headway
        if (g_last_warning_data.headway_warning_level != can.headway_warning_level) {
            printf("------Headway event-----------\n");
            printf("headway_warning_level:%d\n", can.headway_warning_level);
            //printf("headway_measurement:%d\n", can.headway_measurement);
#if 0
            if (HW_LEVEL_RED_CAR == can.headway_warning_level) {
                if(!filter_alert_by_time(&hw_alert, FILTER_ADAS_ALERT_SET_TIME)){
                    printf("ldw filter alert by time!\n");
                }else{
                    playloadlen = build_adas_warn_frame(SB_WARN_TYPE_HW,SB_WARN_STATUS_NONE, uploadmsg);
                    //uploadmsg->status_flag = SB_WARN_STATUS_BEGIN;
                    uploadmsg->sound_type = SB_WARN_TYPE_HW;
                    WSI_DEBUG("send HW alert message!\n");
                    message_queue_send(pSend,SAMPLE_DEVICE_ID_ADAS, SAMPLE_CMD_WARNING_REPORT,\
                            (uint8_t *)uploadmsg, \
                            playloadlen);
                }
            }
#else
            if (HW_LEVEL_RED_CAR == can.headway_warning_level) {
                playloadlen = build_adas_warn_frame(SB_WARN_TYPE_HW, SB_WARN_STATUS_BEGIN, uploadmsg);
                WSI_DEBUG("send HW alert start message!\n");
                message_queue_send(pSend,SAMPLE_DEVICE_ID_ADAS, SAMPLE_CMD_WARNING_REPORT,\
                        (uint8_t *)uploadmsg, \
                        playloadlen);
            } else if (HW_LEVEL_RED_CAR == \
                    g_last_warning_data.headway_warning_level) {
                playloadlen = build_adas_warn_frame(SB_WARN_TYPE_HW, SB_WARN_STATUS_END, uploadmsg);
                WSI_DEBUG("send HW alert end message!\n");
                message_queue_send(pSend,SAMPLE_DEVICE_ID_ADAS, SAMPLE_CMD_WARNING_REPORT,\
                        (uint8_t *)uploadmsg,\
                        playloadlen);
            }
#endif
        }
out:
        memcpy(&g_last_can_msg, &can, sizeof(g_last_can_msg));
        memcpy(&g_last_warning_data, all_warning_data, sizeof(g_last_warning_data));
        memcpy(&g_last_trigger_warning, trigger_data,\
                sizeof(g_last_trigger_warning));
    return 0;
}

int deal_wsi_adas_can760(WsiFrame *sourcecan)
{
    CAN760Info carinfo;
    //printf("760 come.........................\n");
    //printbuf(sourcecan->warning, 8);
    memcpy(&carinfo, sourcecan->warning, sizeof(carinfo));
    can760_message_process(&carinfo, WRITE_REAL_TIME_MSG);
#if 0
#if 0
    printf("carinfo.brakes= %d\n",carinfo.brakes);
    printf("carinfo.left_signal= %d\n",carinfo.left_signal);
    printf("carinfo.right_signal= %d\n",carinfo.right_signal);
    printf("carinfo.wipers= %d\n",carinfo.wipers);
    printf("carinfo.low_beam= %d\n",carinfo.low_beam);
    printf("carinfo.high_beam= %d\n",carinfo.high_beam);
    printf("carinfo.byte0_resv0= %d\n",carinfo.byte0_resv0);
    printf("carinfo.byte0_resv1= %d\n",carinfo.byte0_resv1);
    printf("carinfo.byte1_resv0= %d\n",carinfo.byte1_resv0);
    printf("carinfo.wipers_aval= %d\n",carinfo.wipers_aval);
    printf("carinfo.low_beam_avavl= %d\n",carinfo.low_beam_aval);
    printf("carinfo.high_beam_aval= %d\n",carinfo.high_beam_aval);
    printf("carinfo.byte1_resv1= %d\n",carinfo.byte1_resv1);
#endif
    printf("carinfo.speed_aval= %d\n",carinfo.speed_aval);
    printf("carinfo.speed= %d\n",carinfo.speed);
#endif
    return 0;
}


int find_local_image_name(uint8_t type, uint32_t id, char *filepath)
{
    MmInfo_node node;
#if 0
    //查找本地多媒体文件
    if(find_mm_resource(id, &node))
    {
        printf("find id[%d] fail!\n", id);
        return -1;
    }
#endif
    mmid_to_filename(id, type, filepath);
    return GetFileSize(filepath);
}

int GetFileSize(char *filename)
{
    int filesize = 0;
    FILE *fp = NULL;

    printf("try open image filename: %s\n", filename);
    fp = fopen(filename, "rb");
    if(fp == NULL)
    {
        printf("open %s fail...\n", filename);
        return -1;
    }
    else
    {
        printf("open %s ok\n", filename);
    }

    fseek(fp, 0, SEEK_SET);
    fseek(fp, 0, SEEK_END);
    filesize = ftell(fp);
    fclose(fp);

    return filesize;
}

//发送多媒体请求应答
static int32_t send_mm_req_ack(SBProtHeader *pHeader, int len)
{
    uint32_t mm_id = 0;
    uint8_t mm_type = 0;
    int32_t filesize = 0;
    SBMmHeader *mm_ptr = NULL;
    SBMmHeader2 send_mm;
    int ret = 0;

    if(pHeader->cmd == SAMPLE_CMD_REQ_MM_DATA && !g_pkg_status_p->mm_data_trans_waiting) //recv req
    {
        printf("------------req mm-------------\n");
        printbuf((uint8_t *)pHeader, len);
        //检查接收幀的完整性
        if(len != sizeof(SBMmHeader) + sizeof(SBProtHeader) + 1){
            printf("recv cmd:0x%x, data len maybe error[%d]/[%ld]!\n", \
                    pHeader->cmd, len,\
                    sizeof(SBMmHeader) + sizeof(SBProtHeader) + 1);
            return -1;
        }
        mm_ptr = (SBMmHeader *)(pHeader + 1);

        mm_id = MY_NTOHL(mm_ptr->id);
        mm_type = mm_ptr->type;
        printf("req mm_type = %d\n", mm_type);
        printf("req mm_id = %10u\n", mm_id);

        filesize = find_local_image_name(mm_type, mm_id,  g_pkg_status_p->filepath);
        if(filesize > 0){//media found
            printf("find file ok!\n");
            //send ack
            message_queue_send(pHeader,pHeader->device_id, SAMPLE_CMD_REQ_MM_DATA, NULL, 0);
            g_pkg_status_p->mm_data_trans_waiting = 1;

            //记录当前包的信息, 发送应答
            g_pkg_status_p->mm.type = mm_type;
            g_pkg_status_p->mm.id = mm_id;
            g_pkg_status_p->mm.packet_index = 0;
            g_pkg_status_p->mm.packet_total_num = (filesize + IMAGE_SIZE_PER_PACKET - 1)/IMAGE_SIZE_PER_PACKET;

            //send first package
            printf("send first package!\n");
            sample_send_image(pHeader->device_id);
        }else{
            printf("find file fail!\n");
        }
    }else{
        printf("current package is not valid!\n");
        return -1;
    }
    return 0;
}

void clear_old_media(uint32_t id)
{
    char filepath[100];
    static uint32_t s_last_id = 0;
    uint32_t i = 0;

    for(i=s_last_id; i<id; i++){
        mmid_to_filename(i, 0, filepath);
        printf("delete old media:%s\n", filepath);
        remove(filepath);
    }
    s_last_id = id;
}

static int recv_ack_and_send_image(SBProtHeader *pHeader, int32_t len)
{
    SendStatus pkg;
    MmAckInfo mmack;
    uint32_t id;

    //WSI_DEBUG("recv ack...........!\n");
    memcpy(&mmack, pHeader+1, sizeof(mmack));
    if(mmack.ack){
        printf("recv ack err!\n");
        return -1;
    }else{
        WSI_DEBUG("send pkg index = 0x%08x, recv ack index = 0x%08x\n",\
                g_pkg_status_p->mm.packet_index, MY_NTOHS(mmack.packet_index));

        //recv ack index is correct
        if(g_pkg_status_p->mm.packet_index == MY_NTOHS(mmack.packet_index) + 1){
            //改变发送包，接收ACK状态为ready
            notice_ack_msg();

            //最后一个ACK
            if(g_pkg_status_p->mm.packet_total_num == g_pkg_status_p->mm.packet_index){
                g_pkg_status_p->mm_data_trans_waiting = 0;
                printf("transmit one file over!\n");

                id = g_pkg_status_p->mm.id;
                delete_mm_resource(id);
                //clear_old_media(id);
                //display_mm_resource();
            }else{
                sample_send_image(pHeader->device_id);
            }
        }else{
            printf("recv package index error!\n");
        }
    }
    return 0;
}

static int32_t sample_send_image(uint8_t devid)
{
    int ret=0;
    int offset=0;
    uint32_t retval = 0;
    uint8_t *data=NULL;
    uint8_t *txbuf=NULL;
    uint32_t datalen=0;
    uint32_t txbuflen=0;
    MmPacketIndex trans_mm;

    size_t filesize = 0;
    FILE *fp = NULL;
    SBProtHeader *pSend = NULL;

    datalen = IMAGE_SIZE_PER_PACKET + \
        sizeof(SBProtHeader) + sizeof(MmAckInfo) + 64;
    txbuflen = (IMAGE_SIZE_PER_PACKET + \
            sizeof(SBProtHeader) + sizeof(MmAckInfo) + 64)*2;

    data = (uint8_t *)malloc(datalen);
    if(!data){
        perror("send image malloc");
        retval = 1;
        goto out;
    }
    txbuf = (uint8_t *)malloc(txbuflen);
    if(!txbuf){
        perror("send image malloc");
        retval = 1;
        goto out;
    }

    pSend = (SBProtHeader *) txbuf;
    mmid_to_filename(g_pkg_status_p->mm.id, g_pkg_status_p->mm.type, g_pkg_status_p->filepath);
    fp = fopen(g_pkg_status_p->filepath, "rb");
    if(fp ==NULL){
        printf("open %s fail\n", g_pkg_status_p->filepath);
        retval = -1;
        goto out;
    }
    trans_mm.type = g_pkg_status_p->mm.type;
    trans_mm.id = MY_HTONL(g_pkg_status_p->mm.id);
    trans_mm.packet_index = MY_HTONS(g_pkg_status_p->mm.packet_index);
    trans_mm.packet_total_num = MY_HTONS(g_pkg_status_p->mm.packet_total_num);

    memcpy(data, &trans_mm, sizeof(trans_mm));
    offset = g_pkg_status_p->mm.packet_index * IMAGE_SIZE_PER_PACKET;
    fseek(fp, offset, SEEK_SET);
    ret = fread(data + sizeof(g_pkg_status_p->mm), 1, IMAGE_SIZE_PER_PACKET, fp);
    fclose(fp);
    if(ret>0){
        message_queue_send(pSend,SAMPLE_DEVICE_ID_ADAS, SAMPLE_CMD_UPLOAD_MM_DATA, \
                data, (sizeof(g_pkg_status_p->mm) + ret));
        //printbuf(data, 64);
        WSI_DEBUG("send...[%d/%d]\n", g_pkg_status_p->mm.packet_total_num,\
                g_pkg_status_p->mm.packet_index);
        g_pkg_status_p->mm.packet_index += 1; 
    }else{//end and clear
        printf("read file ret <=0\n");
        perror("error: read image file:");
    }

out:
    if(data)
        free(data);
    if(txbuf)
        free(txbuf);

    return retval;
}

void write_RealTimeData(SBProtHeader *pHeader, int32_t len)
{
    RealTimeData *data;

    if(len == sizeof(SBProtHeader) + 1 + sizeof(RealTimeData))
    {
        RealTimeDdata_process((RealTimeData *)(pHeader+1), WRITE_REAL_TIME_MSG);
        data = (RealTimeData *)(pHeader+1);
        printf("recv car speed: %d\n", data->car_speed);
    }
    else
    {
        printf("recv cmd:0x%x, data len maybe error!\n", pHeader->cmd);
    }
}

void do_factory_reset(uint8_t dev_id)
{
    if(dev_id == SAMPLE_DEVICE_ID_ADAS){
        set_AdasParaSetting_default();
        write_local_adas_para_file(LOCAL_ADAS_PRAR_FILE);

    }else if(dev_id == SAMPLE_DEVICE_ID_DMS){
        set_DmsParaSetting_default();
        write_local_dms_para_file(LOCAL_DMS_PRAR_FILE);
    }
}

void recv_para_setting(SBProtHeader *pHeader, int32_t len)
{
    AdasParaSetting recv_adas_para;
    DmsParaSetting recv_dms_para;
    uint8_t ack = 0;
    char cmd[100];
    uint8_t txbuf[128] = {0};
    SBProtHeader *pSend = (SBProtHeader *) txbuf;
    int ret = -1;

    if(pHeader->device_id == SAMPLE_DEVICE_ID_ADAS){

        if(len == sizeof(SBProtHeader) + 1 + sizeof(recv_adas_para)){
            memcpy(&recv_adas_para, pHeader+1, sizeof(recv_adas_para));

            //大端传输
            recv_adas_para.auto_photo_time_period = MY_NTOHS(recv_adas_para.auto_photo_time_period);
            recv_adas_para.auto_photo_distance_period = MY_NTOHS(recv_adas_para.auto_photo_distance_period);
            write_dev_para(&recv_adas_para, SAMPLE_DEVICE_ID_ADAS);

            printf("recv adas para...\n");
            print_adas_para(&recv_adas_para);

            ret = write_local_adas_para_file(LOCAL_ADAS_PRAR_FILE);
        }else{
            printf("recv cmd:0x%x, adas data len=%d maybe error!\n",len, pHeader->cmd);
        }
    }else if(pHeader->device_id == SAMPLE_DEVICE_ID_DMS){
        if(len == sizeof(SBProtHeader) + 1 + sizeof(recv_dms_para)){
            memcpy(&recv_dms_para, pHeader+1, sizeof(recv_dms_para));

            //大端传输
            recv_dms_para.auto_photo_time_period = MY_NTOHS(recv_dms_para.auto_photo_time_period);
            recv_dms_para.auto_photo_distance_period = MY_NTOHS(recv_dms_para.auto_photo_distance_period);
            recv_dms_para.Smoke_TimeIntervalThreshold = MY_NTOHS(recv_dms_para.Smoke_TimeIntervalThreshold);
            recv_dms_para.Call_TimeIntervalThreshold = MY_NTOHS(recv_dms_para.Call_TimeIntervalThreshold);

            printf("recv dms para...\n");
            print_dms_para(&recv_dms_para);

            write_dev_para(&recv_dms_para, SAMPLE_DEVICE_ID_DMS);
            ret = write_local_dms_para_file(LOCAL_DMS_PRAR_FILE);

        }else{
            printf("recv cmd:0x%x, dms data len=%d maybe error!\n", len, pHeader->cmd);
        }
    }

#if 0
    printf("start to kill algo!\n");
    //set alog detect.flag
    system("busybox killall -9 split_detect"); //killall
    sprintf(cmd, "busybox sed -i 's/^.*--output_lane_info_speed_thresh.*$/--output_lane_info_speed_thresh=%d/' \
            /data/xiao/install/detect.flag",recv_para.warning_speed_val);
    ret = system(cmd);
    printf("setting para ret = %d\n", ret);
    usleep(100000);
    printf("restart algo!\n");
    system("/data/algo.sh & >/dev/null");//restart
#endif

    //设置参数成功
    if(!ret){
        ack = 0;
        message_queue_send(pSend, pHeader->device_id, SAMPLE_CMD_SET_PARAM, (uint8_t*)&ack, 1);
    }else{
        ack = 1;
        message_queue_send(pSend, pHeader->device_id, SAMPLE_CMD_SET_PARAM, \
                (uint8_t*)&ack, 1);
    }
}

void send_para_setting(SBProtHeader *pHeader, int32_t len)
{
    AdasParaSetting send_adas_para;
    DmsParaSetting send_dms_para;
    uint8_t txbuf[256] = {0};
    SBProtHeader *pSend = (SBProtHeader *) txbuf;

    if(len == sizeof(SBProtHeader) + 1)
    {
        if(pHeader->device_id == SAMPLE_DEVICE_ID_ADAS){
            read_dev_para(&send_adas_para, SAMPLE_DEVICE_ID_ADAS);
            send_adas_para.auto_photo_time_period = MY_HTONS(send_adas_para.auto_photo_time_period);
            send_adas_para.auto_photo_distance_period = MY_HTONS(send_adas_para.auto_photo_distance_period);
            message_queue_send(pSend,pHeader->device_id, SAMPLE_CMD_GET_PARAM, \
                    (uint8_t*)&send_adas_para, sizeof(send_adas_para));

        }else if(pHeader->device_id == SAMPLE_DEVICE_ID_DMS){
            read_dev_para(&send_dms_para, SAMPLE_DEVICE_ID_DMS);
            send_dms_para.auto_photo_time_period = MY_HTONS(send_dms_para.auto_photo_time_period);
            send_dms_para.auto_photo_distance_period = MY_HTONS(send_dms_para.auto_photo_distance_period);
            send_dms_para.Smoke_TimeIntervalThreshold = MY_HTONS(send_dms_para.Smoke_TimeIntervalThreshold);
            send_dms_para.Call_TimeIntervalThreshold = MY_HTONS(send_dms_para.Call_TimeIntervalThreshold);
            message_queue_send(pSend, pHeader->device_id, SAMPLE_CMD_GET_PARAM, \
                    (uint8_t*)&send_dms_para, sizeof(send_dms_para));
        }
    }else{
        printf("recv cmd:0x%x, data len maybe error!\n", pHeader->cmd);
    }
}

void recv_warning_ack(SBProtHeader *pHeader, int32_t len)
{
    SendStatus pkg;

    if(len == sizeof(SBProtHeader) + 1){
        printf("push warning ack!\n");
        notice_ack_msg();

    }else{
        printf("recv cmd:0x%x, data len maybe error!\n", pHeader->cmd);
    }
}

void send_work_status_req_ack(SBProtHeader *pHeader, int32_t len)
{
    ModuleStatus module;
    uint8_t txbuf[256] = {0};
    SBProtHeader *pSend = (SBProtHeader *) txbuf;

    memset(&module, 0, sizeof(module));

    if(len == sizeof(SBProtHeader) + 1){
        module.work_status = MODULE_WORKING;
        message_queue_send(pSend, pHeader->device_id, SAMPLE_CMD_REQ_STATUS, \
                (uint8_t*)&module, sizeof(module));
    }else{
        printf("recv cmd:0x%x, data len maybe error!\n", pHeader->cmd);
    }
}
void send_work_status(uint8_t devid)
{
    ModuleStatus module;
    uint8_t txbuf[256] = {0};
    SBProtHeader *pSend = (SBProtHeader *) txbuf;

    memset(&module, 0, sizeof(module));
    module.work_status = MODULE_WORKING;
    message_queue_send(pSend, devid, SAMPLE_CMD_REQ_STATUS, \
            (uint8_t*)&module, sizeof(module));
}

void recv_upload_status_cmd_ack(SBProtHeader *pHeader, int32_t len)
{
    SendStatus pkg;

    if(len == sizeof(SBProtHeader) + 1){
        notice_ack_msg();
    }else{
        printf("recv cmd:0x%x, data len maybe error!\n", pHeader->cmd);
    }
}

typedef struct _file_trans_msg
{
    uint16_t    packet_num;
    uint16_t    packet_index;
} __attribute__((packed)) file_trans_msg;

#define UPGRADE_CMD_START       0x1
#define UPGRADE_CMD_CLEAN       0x2
#define UPGRADE_CMD_TRANS       0x3
#define UPGRADE_CMD_RUN         0x4
uint32_t get_sum(uint8_t *buf, int len)
{
    uint32_t sum = 0;
    int i=0;

    for(i=0; i<len; i++)
    {
        sum += buf[i];
    }
    return sum;
}
int recv_upgrade_file(SBProtHeader *pHeader, int32_t len)
{
    int ret;
    int fd;
    char cmd_rm_file[50];
    static uint32_t sum = 0;
    static uint32_t sum_new = 0;
    uint8_t data[4];
    uint8_t data_ack[6];
    uint8_t ack[2];
    int32_t datalen = 0;
    uint8_t *pchar=NULL;
    static uint32_t packet_num = 0;
    static uint32_t packet_index = 0;
    static uint32_t offset = 0;
    uint8_t     message_id = 0x03;
    file_trans_msg file_trans;
    unsigned char txbuf[256] = {0};
    SBProtHeader * pSend = (SBProtHeader *) txbuf;

    pchar = (uint8_t *)(pHeader+1);
    message_id = *pchar;

    printf("doing message_id 0x%02x\n", message_id);
    if(message_id == UPGRADE_CMD_START ||\
            message_id == UPGRADE_CMD_CLEAN ||\
            message_id == UPGRADE_CMD_RUN)
    {

        if(message_id == UPGRADE_CMD_CLEAN)
        {
            system(CLEAN_MPK);
            //do clean
        }
        if(message_id == UPGRADE_CMD_RUN)
        {
            //do run, do upgrade
            printf("exe new app...\n");
           // system(UPGRADE_FILE_CMD);
            //system("stop bootanim;echo 'restart...';/system/bin/main.sh");

            ack[0] = message_id;
            ack[1] = 0;
            message_queue_send(pSend,pHeader->device_id, SAMPLE_CMD_UPGRADE, \
                    ack, sizeof(ack));

            system("touch /mnt/obb/restart");
            //system("/data/restart.sh &");
            //return 0;
        }
        ack[0] = message_id;
        ack[1] = 0;
        message_queue_send(pSend,pHeader->device_id, SAMPLE_CMD_UPGRADE, \
                ack, sizeof(ack));
    }
    else if(message_id == UPGRADE_CMD_TRANS) //recv file
    {
        memcpy(&file_trans, pchar+1, sizeof(file_trans));
        packet_num = MY_NTOHS(file_trans.packet_num);
        packet_index = MY_NTOHS(file_trans.packet_index);

        if(packet_index == 0)
        {
            sprintf(cmd_rm_file, "rm %s",UPGRADE_FILE_PATH);
            system(cmd_rm_file);
            memcpy(&data, pchar+1+sizeof(file_trans), 4);

            //  sum = *(uint32_t *)&data[0];
            //sum = MY_HTONL(*(uint32_t *)&data[0]);
            sum = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];

            printf("recv sum = 0x%08x\n", sum);
        }
        else //recv file
        {
            memcpy(&file_trans, pchar+1, sizeof(file_trans));
            packet_num = MY_NTOHS(file_trans.packet_num);
            packet_index = MY_NTOHS(file_trans.packet_index);

            datalen = len - (sizeof(SBProtHeader) + 1 + 5);
            printf("recv [%d]/[%d], datalen = %d\n", packet_num, packet_index, datalen);

            fd = open(UPGRADE_FILE_PATH, O_RDWR|O_CREAT, 0644);
            lseek(fd, offset, SEEK_SET);
            ret = write(fd, pchar+5, datalen);
            close(fd);

            if(ret > 0)
                offset += ret;
            sum_new += get_sum((uint8_t *)pchar+5, datalen);
            if(packet_index+1 == packet_num) //the last packet , packet 0 is sum, no data!
            {
                printf("sumnew = 0x%08x, sum=0x%08x\n", sum_new, sum);
                if(sum_new == sum)
                {
                    printf("sun check ok!\n");
                    data_ack[5] = 0;
                    printf("upgrade...\n");
                    system(UPGRADE_FILE_CMD);
                }
                else
                {
                    printf("sun check err!\n");
                    data_ack[5] = 1;
                }
                memcpy(data_ack, pchar, 5);
                message_queue_send(pSend,pHeader->device_id, SAMPLE_CMD_UPGRADE, \
                        data_ack, sizeof(data_ack));
                return 0;
            }
        }
        memcpy(data_ack, pchar, 5);
        data_ack[5] = 0;
        message_queue_send(pSend,pHeader->device_id, SAMPLE_CMD_UPGRADE, \
                data_ack, sizeof(data_ack));
    }
    else
        ;

    return 0;
}
static int32_t sample_on_cmd(SBProtHeader *pHeader, int32_t len)
{
    ptr_queue_node msgack;
    uint16_t serial_num = 0;

    M4DevInfo dev_info = {
        15, "MINIEYE",
        15, "M4",
        15, "1.0.0.1",
        15, "1.0.1.9", //soft version
        15, "0xF0321564",
        15, "SAMPLE",
    };
    uint8_t txbuf[128] = {0};
    SBProtHeader *pSend = (SBProtHeader *) txbuf;

#if defined ENABLE_ADAS
    if(pHeader->device_id != SAMPLE_DEVICE_ID_ADAS &&\
            pHeader->device_id != SAMPLE_DEVICE_ID_BRDCST)
        return 0;

#elif defined ENABLE_DMS
    if((pHeader->device_id != SAMPLE_DEVICE_ID_DMS) && (pHeader->device_id != SAMPLE_DEVICE_ID_BRDCST)){
        return 0;
    }
#else
    printf("no defien device.\n");
    return 0;
#endif
    serial_num = MY_HTONS(pHeader->serial_num);
    do_serial_num(&serial_num, RECORD_RECV_NUM);

    printf("------cmd = 0x%x------\n", pHeader->cmd);
    switch (pHeader->cmd)
    {
        case SAMPLE_CMD_QUERY:
            message_queue_send(pHeader,pHeader->device_id, SAMPLE_CMD_QUERY, NULL, 0);
            break;

        case SAMPLE_CMD_FACTORY_RESET:
            message_queue_send(pHeader,pHeader->device_id, SAMPLE_CMD_FACTORY_RESET, NULL, 0);
            do_factory_reset(pHeader->device_id);
            break;

        case SAMPLE_CMD_SPEED_INFO: //不需要应答
            write_RealTimeData(pHeader, len);
            break;

        case SAMPLE_CMD_DEVICE_INFO:
            message_queue_send(pSend,pHeader->device_id, SAMPLE_CMD_DEVICE_INFO,
                    (uint8_t*)&dev_info, sizeof(dev_info));
            break;

        case SAMPLE_CMD_UPGRADE:
            recv_upgrade_file(pHeader, len);
            break;

        case SAMPLE_CMD_GET_PARAM:
            send_para_setting(pHeader, len);
            break;

        case SAMPLE_CMD_SET_PARAM:
            recv_para_setting(pHeader, len);
            break;

        case SAMPLE_CMD_WARNING_REPORT: //recv warning ack
            recv_warning_ack(pHeader, len);
            break;

        case SAMPLE_CMD_REQ_STATUS: 
            send_work_status_req_ack(pHeader, len);
            break;

        case SAMPLE_CMD_UPLOAD_STATUS: //主动上报状态后，接收到ack
            recv_upload_status_cmd_ack(pHeader, len);
            break;

        case SAMPLE_CMD_REQ_MM_DATA:
            //发送多媒体请求应答
            send_mm_req_ack(pHeader,len);
            break;

        case SAMPLE_CMD_UPLOAD_MM_DATA:
            recv_ack_and_send_image(pHeader, len);
            break;

        case SAMPLE_CMD_SNAP_SHOT:
            WSI_DEBUG("------snap shot----------\n");
            send_snap_shot_ack(pHeader, len);
            do_snap_shot();
            break;

        default:
            printf("****************UNKNOW frame!*************\n");
            printbuf((uint8_t *)pHeader, len);
            break;
    }
    return 0;
}

#include <arpa/inet.h>
#include <linux/if_arp.h>
void bond_net_device(int sock)
{
	int ret;
	struct ifreq interface;
	//const char *inf = "eth0";
	const char *inf = g_configini.netdev_name;
	
	memset(&interface, 0x00, sizeof(interface));
	strncpy(interface.ifr_name, inf, IFNAMSIZ);
	if(setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, (char *)&interface, sizeof(interface)) < 0)
	{
		perror("setsockopt error:");
        return;
	}
}

static int socket_init()
{
#define HOST_SERVER_PORT (8888)

    int sock;
    int32_t ret = 0;
    int enable = 1;
    socklen_t optlen;
    int bufsize = 0;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("Create socket failed %s\n", strerror(errno));
        return -1;
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    bufsize = 0;
    optlen = sizeof(bufsize);
    getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize, &optlen);
    printf("get recv buf size = %d\n", bufsize);
    getsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsize, &optlen);
    printf("get send buf size = %d\n", bufsize);
    //int setsockopt(int sockfd, int level, int optname,const void *optval, socklen_t optlen);

    printf("set buf size = %d\n", bufsize);
    bufsize = 64*1024;
    optlen = sizeof(bufsize);
    ret = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize, optlen);
    if(ret == -1)
    {
        printf("%s:%d error\n", __FILE__, __LINE__);
        return -1;
    }
    bufsize = 64*1024;
    optlen = sizeof(bufsize);
    ret = setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsize, optlen);
    if(ret == -1)
    {
        printf("%s:%d error\n", __FILE__, __LINE__);
        return -1;
    }

    bufsize = 0;
    optlen = sizeof(bufsize);
    getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize, &optlen);
    printf("get recv buf size = %d\n", bufsize);
    getsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsize, &optlen);
    printf("get send buf size = %d\n", bufsize);
    
    return sock;
}

int try_connect(int sock)
{
    int ret=0;
    struct sockaddr_in host_serv_addr;
    //const char *server_ip = "192.168.100.100";
    const char *server_ip = g_configini.serverip;

    memset(&host_serv_addr, 0, sizeof(host_serv_addr));
    host_serv_addr.sin_family = AF_INET;
    //host_serv_addr.sin_port   = MY_HTONS(HOST_SERVER_PORT);
    host_serv_addr.sin_port   = MY_HTONS(g_configini.serverport);

    ret = inet_aton(server_ip, &host_serv_addr.sin_addr);
    if (0 == ret) {
        printf("inet_aton failed %d %s\n", ret, strerror(errno));
        return -1;
    }
    bond_net_device(sock);
    ret = connect(sock, (struct sockaddr *)&host_serv_addr, sizeof(host_serv_addr));
    if(ret){
        perror("connect:");
    }
    return ret;
}

#define TCP_READ_BUF_SIZE (64*1024)
#define RECV_HOST_DATA_BUF_SIZE (128*1024)
void parse_cmd(uint8_t *buf, uint8_t *msgbuf)
{
    uint32_t ret = 0;
    uint8_t sum = 0;
    uint32_t framelen = 0;
    SBProtHeader *pHeader = NULL;
    pHeader = (SBProtHeader *) msgbuf;
    ret = unescaple_msg(buf, msgbuf, RECV_HOST_DATA_BUF_SIZE);
    if(ret>0){
        framelen = ret;
        //printf("recv framelen = %d\n", framelen);
        sum = sample_calc_sum(pHeader, framelen);
        if (sum != pHeader->checksum) {
            printf("Checksum missmatch calcated: 0x%02hhx != 0x%2hhx\n",
                    sum, pHeader->checksum);
        }else{
            sample_on_cmd(pHeader, framelen);
        }
    }
}

#if 0
/*******************************************************************/
/* reads 'count' bytes from a socket  */
/********************************************************************/

int
Nread(int fd, char *buf, size_t count, int prot)
{
    register ssize_t r;
    register size_t nleft = count;

    while (nleft > 0) {
        r = read(fd, buf, nleft);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            else
                return NET_HARDERROR;
        } else if (r == 0)
            break;

        nleft -= r;
        buf += r;
    }
    return count - nleft;
}


/*
 *                      N W R I T E
 */

int
Nwrite(int fd, const char *buf, size_t count, int prot)
{
    register ssize_t r;
    register size_t nleft = count;

    while (nleft > 0) {
	r = write(fd, buf, nleft);
	if (r < 0) {
	    switch (errno) {
		case EINTR:
		case EAGAIN:
#if (EAGAIN != EWOULDBLOCK)
		case EWOULDBLOCK:
#endif
		return count - nleft;

		case ENOBUFS:
		return NET_SOFTERROR;

		default:
		return NET_HARDERROR;
	    }
	} else if (r == 0)
	    return NET_SOFTERROR;
	nleft -= r;
	buf += r;
    }
    return count;
}
#endif



void pthread_tcp_recv_exit()
{
    close(hostsock);
}

void *pthread_tcp_recv(void *para)
{
    int32_t ret = 0;
    int i=0;
    static int tcprecvcnt = 0;
    uint8_t *readbuf = NULL;
    uint8_t *msgbuf = NULL;

    prctl(PR_SET_NAME, "tcp_process");
    send_stat_pkg_init();

    msgbuf = (uint8_t *)malloc(RECV_HOST_DATA_BUF_SIZE);
    if(!msgbuf)
    {
        perror("parse_host_cmd malloc");
        return NULL;
    }
    readbuf = (uint8_t *)malloc(TCP_READ_BUF_SIZE);
    if(!readbuf){
        perror("tcp readbuf malloc");
        goto out;
    }

connect_again:
    hostsock = socket_init();
    if(hostsock < 0){
        goto out;
    }
    while (!force_exit) {
        if(try_connect(hostsock)){
            sleep(1);
            printf("try connect!\n");
            continue;
        }else{
            printf("connected!\n");
        }
#if defined ENABLE_ADAS
        send_work_status(SAMPLE_DEVICE_ID_ADAS);
#elif defined ENABLE_DMS
        send_work_status(SAMPLE_DEVICE_ID_DMS);
#endif

        while(1){
#if 1
            ret = read(hostsock, readbuf, TCP_READ_BUF_SIZE);
            if (ret < 0) {
                printf("read failed %d %s\n", ret, strerror(errno));
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK){
                    usleep(10000);
                    continue;
                }else{
                    //break;
                    goto connect_again;
                }
            }else if (ret == 0) {
                close(hostsock);
                printf("tcp disconnect! sock = %d\n",hostsock);
                //hostsock = -1;
                goto connect_again;
                //break;
            }else{//write to buf
                //MY_DEBUG("recv raw cmd, tcprecvcnt = %d:\n", tcprecvcnt++);
                //printbuf(readbuf, ret);
                i=0;
                while(ret--){
                    parse_cmd(&readbuf[i++], msgbuf);
                }
            }
#else
            fd_set rfds;
            struct timeval tv;
            int retval;
            FD_ZERO(&rfds);
            FD_SET(hostsock, &rfds);

            /* Wait up to five seconds. */
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            retval = select(hostsock+1, &rfds, NULL, NULL, &tv);
            if (retval == -1 ){
                if(errno != EINTR){
                    perror("select()");
                    close(hostsock);
                    hostsock = -1;
                    goto connect_again;
                }
            }else if (retval){
                if(FD_ISSET(hostsock, &rfds)){
                    retval = read(hostsock, readbuf, TCP_READ_BUF_SIZE);
                    if (retval < 0) {
                        printf("read failed %d %s\n", retval, strerror(errno));
                        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK){
                            usleep(10000);
                        }else if (retval == 0) {
                            close(hostsock);
                            hostsock = -1;
                            goto connect_again;
                        }else{
                            i=0;
                            while(ret--){
                                parse_cmd(&readbuf[i++], msgbuf);
                            }
                        }
                    }
                }
            }else{
                printf("No data within five seconds.\n");
            }

#endif
        }
    }
out:
    printf("%s exit!\n", __FUNCTION__);
    if(readbuf)
        free(readbuf);
    if(msgbuf)
        free(msgbuf);

    if(hostsock>0)
        close(hostsock);
    pthread_exit(NULL);
}
static char get_head = 0;
static char got_esc_char = 0;
static int cnt = 0;
void clear_frame_flag()
{
        //clear
        get_head = 0;
        got_esc_char = 0;
        cnt = 0;
}
static uint32_t unescaple_msg(uint8_t *buf, uint8_t *msg, int msglen)
{
    uint8_t ch = 0;
    uint32_t framelen = 0;
    ch = buf[0];

    if(cnt+1 > msglen){
        printf("error: recv msg too long\n");
        clear_frame_flag();
        return 0;
    }
    //printf("ch = 0x%x\n", buf[0]);
    //not recv head
    if(!get_head){
        if((ch == SAMPLE_PROT_MAGIC) && (cnt == 0)){
            msg[cnt] = SAMPLE_PROT_MAGIC;
            cnt++;
            get_head = 1;
            return 0;
        }
    }else{//recv head
        if((ch == SAMPLE_PROT_MAGIC) && (cnt > 0)) {//get tail
            if(cnt < 6){//maybe error frame, as header, restart
                cnt = 0;
                msg[cnt] = SAMPLE_PROT_MAGIC;
                cnt++;
                get_head = 1;

                //clear_frame_flag();
                return 0;

            }else{ //success
                msg[cnt] = SAMPLE_PROT_MAGIC;
                get_head = 0;//over
                cnt++;
                framelen = cnt;

                clear_frame_flag();
                return framelen;
            }
        }else{//get text
            if((ch == SAMPLE_PROT_ESC_CHAR) && !got_esc_char){//need deal
                got_esc_char = 1;
                msg[cnt] = ch;
                cnt++;
            }else if(got_esc_char && (ch == 0x02)){
                msg[cnt-1] = SAMPLE_PROT_MAGIC;
                got_esc_char = 0;
            }else if(got_esc_char && (ch == 0x01)){
                msg[cnt-1] = SAMPLE_PROT_ESC_CHAR;
                got_esc_char = 0;
            }else{
                msg[cnt] = ch;
                cnt++;
                got_esc_char = 0;
            }
        }
    }
    return 0;
}

#define SNAP_SHOT_CLOSE            0
#define SNAP_SHOT_BY_TIME          1
#define SNAP_SHOT_BY_DISTANCE      2
void *pthread_snap_shot(void *p)
{
#ifdef ENABLE_ADAS
    AdasParaSetting tmp;
    uint8_t para_type = SAMPLE_DEVICE_ID_ADAS;
#else
    DmsParaSetting tmp;
    uint8_t para_type = SAMPLE_DEVICE_ID_DMS;
#endif
    RealTimeData rt_data;;
    uint32_t mileage_last = 0;

    prctl(PR_SET_NAME, "pthread_snap");

    while(!force_exit)
    {
        read_dev_para(&tmp, para_type);
#if 1
        if(tmp.auto_photo_mode == SNAP_SHOT_BY_TIME){
            printf("auto snap shot!\n");
            if(tmp.auto_photo_time_period != 0)
                do_snap_shot();
            sleep(tmp.auto_photo_time_period);
#else
        if(1){
            sleep(5);
            if(1)
            {
                printf("auto snap shot!\n");
                do_snap_shot();
            }
#endif
        }else if(tmp.auto_photo_mode == SNAP_SHOT_BY_DISTANCE){
            RealTimeDdata_process(&rt_data, READ_REAL_TIME_MSG);
            if((rt_data.mileage - mileage_last)*100 >= tmp.auto_photo_distance_period){
                if(mileage_last != 0){
                    printf("snap by mileage!\n");
                    do_snap_shot();
                }
                mileage_last = rt_data.mileage; //单位是0.1km
            }else{
                //sleep(1);
                usleep(200000);
            }
        }else{
            usleep(200000);
        }
    }
    pthread_exit(NULL);
}

#define FCW_NAME            "FCW"
#define LDW_NAME            "LDW"
#define HW_NAME             "HW"
#define PCW_NAME            "PCW"
#define FLC_NAME            "FLC"
#define TSRW_NAME           "TSRW"
#define TSR_NAME            "TSR"
#define SNAP_NAME           "SNAP"
char *warning_type_to_str(uint8_t type)
{
    static char s_name[20];
    strcpy(s_name, "default");
    switch(type)
    {
        case SB_WARN_TYPE_FCW:
            return strcpy(s_name, FCW_NAME);
        case SB_WARN_TYPE_LDW:
            return strcpy(s_name, LDW_NAME);
        case SB_WARN_TYPE_HW:
            return strcpy(s_name, HW_NAME);
        case SB_WARN_TYPE_PCW:
            return strcpy(s_name, PCW_NAME);
        case SB_WARN_TYPE_FLC:
            return strcpy(s_name, FLC_NAME);
        case SB_WARN_TYPE_TSRW:
            return strcpy(s_name, TSRW_NAME);
        case SB_WARN_TYPE_TSR:
            return strcpy(s_name, TSR_NAME);
        case SB_WARN_TYPE_SNAP:
            return strcpy(s_name, SNAP_NAME);
        default:
            return s_name;
    }
}

int str_to_warning_type(char *type, uint8_t *val)
{

    if(!strncmp(FCW_NAME, type, sizeof(FCW_NAME)))
    {
        *val = SB_WARN_TYPE_FCW;
    }
    else if(!strncmp(LDW_NAME, type, sizeof(LDW_NAME)))
    {
        *val = SB_WARN_TYPE_LDW;
    }
    else if(!strncmp(HW_NAME, type, sizeof(HW_NAME)))
    {
        *val = SB_WARN_TYPE_HW;
    }
    else if(!strncmp(PCW_NAME, type, sizeof(PCW_NAME)))
    {
        *val = SB_WARN_TYPE_PCW;
    }
    else if(!strncmp(FLC_NAME, type, sizeof(FLC_NAME)))
    {
        *val = SB_WARN_TYPE_FLC;
    }
    else if(!strncmp(TSRW_NAME, type, sizeof(TSRW_NAME)))
    {
        *val = SB_WARN_TYPE_TSRW;
    }
    else if(!strncmp(TSR_NAME, type, sizeof(TSR_NAME)))
    {
        *val = SB_WARN_TYPE_TSR;
    }
    else if(!strncmp(SNAP_NAME, type, sizeof(SNAP_NAME)))
    {
        *val = SB_WARN_TYPE_SNAP;
    }
    else
    {
        printf("unknow warn type: %s\n", type);
        return -1;   
    }

    return 0;
}


