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

#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "sample.h"
#include <stdbool.h>

#include <queue>
using namespace std;

static int32_t sample_send_image();
#define WRITE_REAL_TIME_MSG 0
#define READ_REAL_TIME_MSG  1

//实时数据处理
void RealTimeDdata_process(real_time_data *data, int mode)
{
    static real_time_data msg={0};
    static pthread_mutex_t real_time_msg_lock = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_lock(&real_time_msg_lock);
    if(mode == WRITE_REAL_TIME_MSG)
    {
        memcpy(&msg, data, sizeof(real_time_data));
    }
    else if(mode == READ_REAL_TIME_MSG)
    {
        memcpy(data, &msg, sizeof(real_time_data));
    }
    pthread_mutex_unlock(&real_time_msg_lock);
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

pthread_mutex_t photo_queue_lock = PTHREAD_MUTEX_INITIALIZER;
queue<ptr_queue_node *> g_image_queue;
queue<ptr_queue_node *> *g_image_queue_p = &g_image_queue;

pthread_mutex_t ptr_queue_lock = PTHREAD_MUTEX_INITIALIZER;
queue<ptr_queue_node *> g_ptr_queue;
queue<ptr_queue_node *> *g_ptr_queue_p = &g_ptr_queue;

pthread_mutex_t uchar_queue_lock = PTHREAD_MUTEX_INITIALIZER;
queue<uint8_t> g_uchar_queue;

pkg_repeat_status g_pkg_status;
pkg_repeat_status *g_pkg_status_p = &g_pkg_status;

void repeat_send_pkg_status_init()
{
    memset(g_pkg_status_p, 0, sizeof(pkg_repeat_status));
}

//推入队列，可以只有node的header，数据可以为空
static int ptr_queue_push(queue<ptr_queue_node *> *p, ptr_queue_node *in,  pthread_mutex_t *lock)
{
    int ret;
    ptr_queue_node *header = NULL;
    uint8_t *ptr = NULL;

    if(!in || !p)
        return -1;

    pthread_mutex_lock(lock);
    if((int)p->size() == PTR_QUEUE_BUF_CNT)
    {
        printf("ptr queue overflow...\n");
        ret = -1;
        goto out;
    }
    else
    {
        header = (ptr_queue_node *)malloc(sizeof(ptr_queue_node));
        if(!header)
        {
            perror("ptr_queue_push malloc1");
            ret = -1;
            goto out;
        }

        memcpy(header, in, sizeof(ptr_queue_node));
        if(!in->buf)//user don't need buffer
        {
            header->len = 0;
            header->buf = NULL;
        }
        else //user need buffer
        {
            ptr = (uint8_t *)malloc(PTR_QUEUE_BUF_SIZE);
            if(!ptr)
            {
                perror("ptr_queue_push malloc2");
                free(header);
                ret = -1;
                goto out;
            }

            header->buf = ptr;
            header->len = in->len > PTR_QUEUE_BUF_SIZE ? PTR_QUEUE_BUF_SIZE : in->len;
            memcpy(header->buf, in->buf, header->len);
        }

        p->push(header);
        ret = 0;
        goto out;
    }

out:
    pthread_mutex_unlock(lock);
    return ret;
}

//弹出队列，可以只取node的header，数据不要
static int ptr_queue_pop(queue<ptr_queue_node*> *p, ptr_queue_node *out,  pthread_mutex_t *lock)
{
    ptr_queue_node *header = NULL;
    uint32_t user_buflen = 0;
    uint8_t *ptr = NULL;

    if(!out || !p)
        return -1;

    pthread_mutex_lock(lock);
    if(!p->empty())
    {
        header = p->front();
        p->pop();
    }
    pthread_mutex_unlock(lock);

    if(!header)
        return -1;

    //no data in node
    if(!header->buf)
    {
        memcpy(out, header, sizeof(ptr_queue_node));
    }
    //node have data,
    else
    {
        //user don't need data
        if(!out->buf)
        {
            memcpy(out, header, sizeof(ptr_queue_node));
        }
        else
        {
            //record ptr and len
            ptr = out->buf;
            user_buflen = out->len;
            memcpy(out, header, sizeof(ptr_queue_node));
            out->buf = ptr;
            //get the min len
            header->len = header->len > PTR_QUEUE_BUF_SIZE ? PTR_QUEUE_BUF_SIZE : header->len;
            out->len = header->len > user_buflen ? user_buflen : header->len;
            memcpy(out->buf, header->buf, out->len);
        }
        free(header->buf);
    }

    free(header);
    return 0;
}

void free_header_node(queue<ptr_queue_node*> *p, pthread_mutex_t *lock)
{
    if(!p)
        return;

    pthread_mutex_lock(lock);
    if(!p->empty())
    {
        //header = p->front();
        p->pop();
    }
    pthread_mutex_unlock(lock);
}

static int read_header_node(queue<ptr_queue_node*> *p, SendStatus *out, pthread_mutex_t *lock)
{
    ptr_queue_node *header = NULL;
    int ret;

    if(!p)
        return 1;

    pthread_mutex_lock(lock);

    if(!p->empty())
    {
        header = p->front();
        memcpy(out, &header->pkg, sizeof(header->pkg));
        ret = 0;
        goto out;
    }
    else
    {
        ret = 1;
        goto out;
    }

out:
    pthread_mutex_unlock(lock);
    return ret;
}

static int write_header_node(queue<ptr_queue_node*> *p, SendStatus *in, pthread_mutex_t *lock)
{
    ptr_queue_node *header = NULL;
    int ret;

    if(!p)
        return 1;

    pthread_mutex_lock(lock);

    if(!p->empty())
    {
        header = p->front();
        memcpy(&header->pkg, in, sizeof(header->pkg));
        ret = 0;
        goto out;
    }
    else
    {
        ret = 1;
        goto out;
    }

out:
    pthread_mutex_unlock(lock);
    return ret;
}

static int get_node_buf(queue<ptr_queue_node*> *p, uint8_t *out, int *len, pthread_mutex_t *lock)
{
    ptr_queue_node *header = NULL;
    int ret = 0;

    if(!p)
        return -1;

    pthread_mutex_lock(lock);
    if(!p->empty())
    {
        header = p->front();
        //p->pop();
        memcpy(out, header->buf, header->len);
        *len = header->len;

        ret = 0;
        goto out;
    }
    else
    {
        ret = 1;
        goto out;
    }
out:
    pthread_mutex_unlock(lock);
    return ret;
}

//return 0, 超时，单位是毫秒 
int timeout_trigger(struct timeval *tv, int ms)
{
    struct timeval tv_cur;
    int timeout_sec, timeout_usec;

    timeout_sec = (ms)/1000;
    timeout_usec  = ((ms)%1000)*1000;

    gettimeofday(&tv_cur, NULL);

    if((tv_cur.tv_sec > tv->tv_sec + timeout_sec) || \
            ((tv_cur.tv_sec == tv->tv_sec + timeout_sec) && (tv_cur.tv_usec > tv->tv_usec + timeout_usec)))
    {
        printf("timeout! %d ms\n", ms);
        return 1;
    }
    else
        return 0;
}

#define RECORD_START 0
#define RECORD_END   1
//use to test file transmit speed
void record_time(int mode)
{
    int val = 0;
    static struct timeval tv_start;
    static struct timeval tv_end;
    if(mode == RECORD_START)
    {
        gettimeofday(&tv_start, NULL);
    }
    else if(mode == RECORD_END)
    {
        gettimeofday(&tv_end, NULL);
        val = (1000*1000*(tv_end.tv_sec - tv_start.tv_sec) + tv_end.tv_usec - tv_start.tv_usec)/(1000);
        printf("file trans time = %dms\n", val);
    }
}

//return if error happened, or data write over
static int unblock_write(int sock, uint8_t *buf, int len)
{
    int ret = 0;
    int offset = 0;

    if(len <0 || len >1400)
    {
        printf("write len = %d.............\n", len);
    }

    if(sock < 0 || len < 0 || !buf)
    {
        return -1;
    }
    else
    {
        while(offset < len)
        {
            ret = write(sock, &buf[offset], len-offset);
            if(ret <= 0)
            {
                //write error deal
                perror("tcp write:");
                if(errno == EINTR || errno == EAGAIN)
                {
                    printf("wait mommemt, continue!\n");
                    usleep(10000);
                    continue;
                }
                else
                    return -1;
            }
            else
            {
                offset += ret;
            }
        }
    }

    return offset;
}

//insert mm info 
void push_mm_queue(mm_header_info *mm)
{
    ptr_queue_node header;
    header.buf = NULL;
    header.len = 0;

    memcpy(&header.mm, mm, sizeof(*mm));

    ptr_queue_push(g_image_queue_p, &header, &photo_queue_lock);
}

//pull node ,the info use to record the avi or jpeg
int pull_mm_queue(mm_header_info *mm)
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

//填写报警信息的一些实时数据
void get_real_time_msg(warningtext *uploadmsg)
{
    real_time_data tmp;
    RealTimeDdata_process(&tmp, READ_REAL_TIME_MSG);

    uploadmsg->height = tmp.height;
    uploadmsg->altitude = tmp.altitude;
    uploadmsg->longitude = tmp.longitude;
    uploadmsg->car_speed = tmp.car_speed;

    memcpy(uploadmsg->time, tmp.time, sizeof(uploadmsg->time));
    memcpy(&uploadmsg->car_status, &tmp.car_status, sizeof(uploadmsg->car_status));
}

//填写和can数据不相关的字段
int do_record_warning_info(int type, warningtext *uploadmsg)
{
    uint8_t video_time=0;
    uint8_t photo_num=0;
    uint8_t photo_time_period=0;
    int i=0;
    mm_header_info mm;
    para_setting para;

    read_warn_para(&para);
    memset(&mm, 0, sizeof(mm));

    get_real_time_msg(uploadmsg);

    switch(type)
    {
        case SW_TYPE_FCW:
        case SW_TYPE_LDW:
        case SW_TYPE_HW:
        case SW_TYPE_PCW:
        case SW_TYPE_FLC:
            if(type == SW_TYPE_FCW){
                photo_num = para.FCW_photo_num;
                photo_time_period = para.FCW_photo_time_period;
                video_time = para.FCW_video_time;
            }else if(type == SW_TYPE_LDW){
                photo_num = para.LDW_photo_num;
                photo_time_period = para.LDW_photo_time_period;
                video_time = para.LDW_video_time;
            }else if(type == SW_TYPE_HW){
                photo_num = para.HW_photo_num;
                photo_time_period = para.HW_photo_time_period;
                video_time = para.HW_video_time;
            }else if(type == SW_TYPE_PCW){
                photo_num = para.PCW_photo_num;
                photo_time_period = para.PCW_photo_time_period;
                video_time = para.PCW_video_time;
            }else if(type == SW_TYPE_FLC){
                photo_num = para.FLC_photo_num;
                photo_time_period = para.FLC_photo_time_period;
                video_time = para.FLC_video_time;
            }else{
                ;
            }
            //printf("---------------------type=%d\n", type);
            uploadmsg->warning_id = MY_HTONL(get_next_id(WARNING_ID_MODE, NULL, 0));
            uploadmsg->sound_type = type;
            uploadmsg->car_speed = 0;
            uploadmsg->mm_num = 0;

            mm.warn_type = type;
            mm.video_time = video_time;
            mm.photo_time_period = photo_time_period;
            mm.photo_num = photo_num;
            if((photo_num != 0 && photo_num < WARN_SNAP_NUM_MAX) || (video_time != 0))
            {
                if(photo_num != 0 && photo_num < WARN_SNAP_NUM_MAX)
                {
                    mm.photo_enable = 1; 
                    get_next_id(MM_ID_MODE, mm.photo_id, photo_num);
                    for(i=0; i<photo_num; i++)
                    {
                        //printf("mm.photo_id[%d] = %d\n", i, mm.photo_id[i]);
                        uploadmsg->mm_num++;
                        uploadmsg->mm[i].type = MM_PHOTO;
                        uploadmsg->mm[i].id = MY_HTONL(mm.photo_id[i]);
                    }
                }
                if(video_time != 0)
                {
                    mm.video_enable = 1; 
                    get_next_id(MM_ID_MODE, mm.video_id, 1);

                    uploadmsg->mm_num++;
                    uploadmsg->mm[i].type = MM_VIDEO;
                    uploadmsg->mm[i].id = MY_HTONL(mm.video_id[0]);
                }
                push_mm_queue(&mm);
            }

            break;

        case SW_TYPE_TSRW:
        case SW_TYPE_TSR:
            break;

        case SW_TYPE_SNAP:
            //case SW_TYPE_TIMER_SNAP:

            photo_num = para.photo_num;
            photo_time_period = para.photo_time_period;

            uploadmsg->warning_id = MY_HTONL(get_next_id(WARNING_ID_MODE, NULL, 0));
            uploadmsg->sound_type = type;
            uploadmsg->car_speed = 0;
            uploadmsg->mm_num = 0;
            // printf("snap shot num = %d\n", photo_num);
            if(photo_num != 0 && photo_num < WARN_SNAP_NUM_MAX)
            {
                mm.warn_type = type;
                mm.photo_enable = 1; 
                mm.photo_num = photo_num;
                mm.photo_time_period = photo_time_period;
                get_next_id(MM_ID_MODE, mm.photo_id, photo_num);
                for(i=0; i<photo_num; i++)
                {
                    uploadmsg->mm_num++;
                    uploadmsg->mm[i].type = MM_PHOTO;
                    uploadmsg->mm[i].id = MY_HTONL(mm.photo_id[i]);
                }
                push_mm_queue(&mm);
            }
            break;

        default:
            break;
    }

    return uploadmsg->mm_num;
}

#define SEND_PKG_TIME_OUT_1S    1000
static int send_pkg_to_host(int sock)
{
    //uint8_t buf[PTR_QUEUE_BUF_SIZE];
    uint8_t *buf = NULL;
    int ret = 0;
    int retval = 0;
    int len = 0;
    SendStatus pkg;

    buf = (uint8_t *)malloc(PTR_QUEUE_BUF_SIZE);
    if(!buf)
    {
        perror("send pkg malloc");
        retval = 1;
        goto out;
    }

    //no message
    if(read_header_node(g_ptr_queue_p, &pkg, &ptr_queue_lock))
    {
        retval = 1;
        goto out;
    }

    if(pkg.ack_status == MSG_ACK_READY)
    {
        if(pkg.send_repeat == 0)
        {
            if(!get_node_buf(g_ptr_queue_p, buf, &len, &ptr_queue_lock))
                ret = unblock_write(sock, buf, len);
            //如果发送失败，比如断网的情况，怎么处理
        }
        //发送成功，释放头节点
        free_header_node(g_ptr_queue_p, &ptr_queue_lock);
        retval = 0;
        goto out;
    }
    else if(pkg.ack_status == MSG_ACK_WAITING)
    {
        if(pkg.send_repeat == 0)
        {
            if(get_node_buf(g_ptr_queue_p, buf, &len, &ptr_queue_lock))
            {
                retval = 0;
                goto out;
            }

            ret = unblock_write(sock, buf, len);
            gettimeofday(&pkg.send_time, NULL);
            pkg.send_repeat++;
            write_header_node(g_ptr_queue_p, &pkg, &ptr_queue_lock);
        }
        else if(pkg.send_repeat > 0 && pkg.send_repeat < 3)
        {
            if(timeout_trigger(&pkg.send_time, SEND_PKG_TIME_OUT_1S))
            {
                if(get_node_buf(g_ptr_queue_p, buf, &len, &ptr_queue_lock))
                {
                    retval = 1;
                    goto out;
                }

                ret = unblock_write(sock, buf, len);
                gettimeofday(&pkg.send_time, NULL);
                pkg.send_repeat++;
                write_header_node(g_ptr_queue_p, &pkg, &ptr_queue_lock);
            }
            else
            {
                usleep(20);
            }
        }
        else
        {
            //3次都已经重发，释放头节点
            free_header_node(g_ptr_queue_p, &ptr_queue_lock);
        }
    }

out:
    if(buf)
        free(buf);
    return retval;
}

static int uchar_queue_push(uint8_t *ch)
{
    int ret = -1;
    if(!ch)
        return ret;

    pthread_mutex_lock(&uchar_queue_lock);
    if((int)g_uchar_queue.size() == UCHAR_QUEUE_SIZE)
    {
        printf("g_uchar_queue full flow\n");
        ret = -1;
        goto out;
    }
    else
    {
        g_uchar_queue.push(ch[0]);
        ret = 0;
        goto out;
    }
out:
    pthread_mutex_unlock(&uchar_queue_lock);
    return ret;
}

static int8_t uchar_queue_pop(uint8_t *ch)
{
    int ret = -1;
    if(!ch)
        return ret;

    pthread_mutex_lock(&uchar_queue_lock);
    if(!g_uchar_queue.empty())
    {
        *ch = g_uchar_queue.front();
        g_uchar_queue.pop();
        ret = 0;
        goto out;
    }
    else
    {
        ret = -1;
        goto out;
    }
out:
    pthread_mutex_unlock(&uchar_queue_lock);
    return ret;
}

static uint8_t sample_calc_sum(sample_prot_header *pHeader, int32_t msg_len)
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

static int32_t sample_escaple_msg(sample_prot_header *pHeader, int32_t msg_len)
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
static int32_t sample_assemble_msg_to_push(sample_prot_header *pHeader, uint8_t cmd,
        uint8_t *payload, int32_t payload_len)
{
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
    pHeader->device_id= SAMPLE_DEVICE_ID_ADAS;
    pHeader->cmd = cmd;

    if (payload_len > 0) 
    {
        memcpy(data, payload, payload_len);
    }
    tail[0] = SAMPLE_PROT_MAGIC;

    pHeader->checksum = sample_calc_sum(pHeader, msg_len);
    msg_len = sample_escaple_msg(pHeader, msg_len);

    msg.pkg.send_repeat = 0;
    msg.len = msg_len;
    msg.buf = (uint8_t *)pHeader;
    //    printf("sendpackage cmd = 0x%x,msg.need_ack = %d, len=%d, push!\n",msg.cmd, msg.need_ack, msg.len);
    ptr_queue_push(g_ptr_queue_p, &msg, &ptr_queue_lock);

    return msg_len;
}

void send_snap_shot_ack(sample_prot_header *pHeader, int32_t len)
{
    uint8_t txbuf[256] = {0};
    uint8_t ack = 0;
    sample_prot_header *pSend = (sample_prot_header *) txbuf;

    if(len == sizeof(sample_prot_header) + 1)
    {
        sample_assemble_msg_to_push(pSend, SAMPLE_CMD_SNAP_SHOT, (uint8_t *)&ack, 1);
    }
    else
    {
        printf("recv cmd:0x%x, data len maybe error!\n", pHeader->cmd);
    }
}

int do_snap_shot()
{
    uint8_t mm_num = 0;
    uint8_t msgbuf[512];
    warningtext *uploadmsg = (warningtext *)&msgbuf[0];
    uint8_t txbuf[512];
    sample_prot_header *pSend = (sample_prot_header *) txbuf;

    mm_num = do_record_warning_info(SW_TYPE_SNAP, uploadmsg);

    sample_assemble_msg_to_push(pSend, \
            SAMPLE_CMD_WARNING_REPORT,\
            (uint8_t *)uploadmsg,\
            sizeof(*uploadmsg) + mm_num*sizeof(sample_mm_info));

    return 0;
}

void set_BCD_time(warningtext *uploadmsg, char *second)
{
    struct tm *p = NULL; 
    time_t timep = 0;   
    printf("time:%s\n", second);
    timep = strtoul(second, NULL, 10);
    timep = timep/1000000;
    p = localtime(&timep);
    uploadmsg->time[0] = p->tm_year;
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
int can_message_send(can_data_type *sourcecan)
{
    uint32_t warning_id = 0;
    uint8_t msgbuf[512];
    uint8_t mm_num = 0;
    warningtext *uploadmsg = (warningtext *)&msgbuf[0];
    MECANWarningMessage can;
    car_info carinfo;
    uint8_t txbuf[512];
    char image_name[50];
    sample_prot_header *pSend = (sample_prot_header *) txbuf;

    uint32_t i = 0;
    uint8_t all_warning_masks[sizeof(MECANWarningMessage)] = {
        0x00, 0x00, 0x01, 0x00,
        0x0E, 0x00, 0x00, 0x03};
    uint8_t trigger_warning_masks[sizeof(MECANWarningMessage)] = {
        0x00, 0x00, 0x00, 0x00,
        0x0E, 0x00, 0x00, 0x00};
    uint8_t all_warning_data[sizeof(MECANWarningMessage)] = {0};
    uint8_t trigger_data[sizeof(MECANWarningMessage)] = {0};

    if(!memcmp(sourcecan->topic, MESSAGE_CAN700, strlen(MESSAGE_CAN700)))
    {
        printf("700 come********************************\n");
        memcpy(&can, sourcecan->warning, sizeof(sourcecan->warning));

        for (i = 0; i < sizeof(all_warning_masks); i++) {
            all_warning_data[i] = sourcecan->warning[i] & all_warning_masks[i];
            trigger_data[i]     = sourcecan->warning[i] & trigger_warning_masks[i];
        }

        if (0 == memcmp(all_warning_data, &g_last_warning_data, sizeof(g_last_warning_data))) {
            return 0;
        }
#if 1
        if( (g_last_warning_data.headway_warning_level != can.headway_warning_level) || \
                (0 != memcmp(trigger_data, &g_last_trigger_warning, sizeof(g_last_trigger_warning)) && 0 != trigger_data[4]) )
        {
            printf("warning happened.........................\n");
            memset(msgbuf, 0, sizeof(msgbuf));
            set_BCD_time(uploadmsg, sourcecan->time);
        }
#endif
        //LDW and FCW event
        if (0 != memcmp(trigger_data, &g_last_trigger_warning, sizeof(g_last_trigger_warning)) && 0 != trigger_data[4]) {
            printf("------LDW/FCW event-----------\n");

            if (can.left_ldw || can.right_ldw) {
                memset(uploadmsg, 0, sizeof(*uploadmsg));
                mm_num = do_record_warning_info(SW_TYPE_LDW, uploadmsg);
                uploadmsg->start_flag = SW_STATUS_EVENT;

                if(can.left_ldw)
                    uploadmsg->ldw_type = SOUND_TYPE_LLDW;
                if(can.right_ldw)
                    uploadmsg->ldw_type = SOUND_TYPE_RLDW;

                sample_assemble_msg_to_push(pSend, SAMPLE_CMD_WARNING_REPORT,\
                        (uint8_t *)uploadmsg, \
                        sizeof(*uploadmsg) + mm_num*sizeof(sample_mm_info));
            }
            if (can.fcw_on) {
                mm_num = do_record_warning_info(SW_TYPE_FCW, uploadmsg);
                uploadmsg->start_flag = SW_STATUS_EVENT;
                uploadmsg->sound_type = SW_TYPE_FCW;
                sample_assemble_msg_to_push(pSend, SAMPLE_CMD_WARNING_REPORT,\
                        (uint8_t *)uploadmsg, \
                        sizeof(*uploadmsg) + mm_num*sizeof(sample_mm_info));
            }
        }
        //Headway
        if (g_last_warning_data.headway_warning_level != can.headway_warning_level) {
            printf("------Headway event-----------\n");
            printf("headway_warning_level:%d\n", can.headway_warning_level);
            printf("headway_measurement:%d\n", can.headway_measurement);

            if (HW_LEVEL_RED_CAR == can.headway_warning_level) {
                mm_num = do_record_warning_info(SW_TYPE_HW, uploadmsg);
                uploadmsg->start_flag = SW_STATUS_BEGIN;
                uploadmsg->sound_type = SW_TYPE_HW;

                sample_assemble_msg_to_push(pSend, SAMPLE_CMD_WARNING_REPORT,\
                        (uint8_t *)uploadmsg, \
                        sizeof(*uploadmsg) + mm_num*sizeof(sample_mm_info));

            } else if (HW_LEVEL_RED_CAR == \
                    g_last_warning_data.headway_warning_level) {
                mm_num = do_record_warning_info(SW_TYPE_HW, uploadmsg);
                uploadmsg->start_flag = SW_STATUS_END;
                uploadmsg->sound_type = SW_TYPE_HW;

                sample_assemble_msg_to_push(pSend, SAMPLE_CMD_WARNING_REPORT,\
                        (uint8_t *)uploadmsg,\
                        sizeof(*uploadmsg) + mm_num*sizeof(sample_mm_info));
            }
        }

        memcpy(&g_last_can_msg, &can, sizeof(g_last_can_msg));
        memcpy(&g_last_warning_data, all_warning_data, sizeof(g_last_warning_data));
        memcpy(&g_last_trigger_warning, trigger_data,\
                sizeof(g_last_trigger_warning));
    }
    if(!strncmp(sourcecan->topic, MESSAGE_CAN760, strlen(MESSAGE_CAN760)))
    {
        printf("760 come.........................\n");
        printbuf(sourcecan->warning, 8);
        memcpy(&carinfo, sourcecan->warning, sizeof(carinfo));
#if 0
        //			uploadmsg.high = carinfo.
        //			uploadmsg.altitude = carinfo. //MY_HTONS(altitude)
        //			uploadmsg.longitude= carinfo. //MY_HTONS(longitude)
        //			uploadmsg.car_status.acc = 
        uploadmsg->car_status.left_signal = carinfo.left_signal;
        uploadmsg->car_status.right_signal = carinfo.right_signal;

        if(carinfo.wipers_aval)
            uploadmsg->car_status.wipers = carinfo.wipers;
        uploadmsg->car_status.brakes = carinfo.brakes;
        //			uploadmsg.car_status.insert = 
#endif
    }
    return 0;
}

int find_local_image_name(uint8_t type, uint32_t id, char *filepath)
{
    mm_node node;

    //查找本地多媒体文件
    if(find_mm_resource(id, &node))
    {
        printf("find id[0x%x] fail!\n", id);
        return -1;
    }

    if(type == MM_PHOTO)
        sprintf(filepath,"%s%s-%08d.jpg", SNAP_SHOT_JPEG_PATH, warning_type_to_str(node.warn_type), id);
    else if(type == MM_AUDIO)
        sprintf(filepath,"%s%s-%08d.wav", SNAP_SHOT_JPEG_PATH, warning_type_to_str(node.warn_type), id);
    else if(type == MM_VIDEO)
        sprintf(filepath,"%s%s-%08d.avi", SNAP_SHOT_JPEG_PATH, warning_type_to_str(node.warn_type), id);
    else
    {
        printf("mm type not valid, val=%d\n", type);
        return -1;
    }

    return 0;
}

int GetFileSize(char *filename)
{
    int filesize = 0;
    FILE *fp = NULL;

    printf("try open image filename: %s\n", filename);
    fp = fopen(filename, "rb");
    if(fp == NULL)
    {
        printf("open %s fail\n", filename);
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
static int32_t send_mm_req_ack(sample_prot_header *pHeader, int len)
{
    uint32_t mm_id = 0;
    uint8_t warn_type = 0;
    size_t filesize = 0;
    sample_mm_info *mm_ptr = NULL;

    if(pHeader->cmd == SAMPLE_CMD_REQ_MM_DATA && !g_pkg_status_p->mm_data_trans_waiting) //recv req
    {
        printf("------------req mm-------------\n");
        printbuf((uint8_t *)pHeader, len);

        //检查接收幀的完整性
        if(len == sizeof(sample_mm_info) + sizeof(sample_prot_header) + 1)
        {
            mm_ptr = (sample_mm_info *)(pHeader + 1);
            printf("req mm_type = 0x%x\n", mm_ptr->type);
            printf("req mm_id = 0x%08x\n", MY_HTONL(mm_ptr->id));
        }
        else
        {
            printf("recv cmd:0x%x, data len maybe error[%d]/[%ld]!\n", \
                    pHeader->cmd, len,\
                    sizeof(sample_mm_info) + sizeof(sample_prot_header) + 1);
            return -1;
        }

        mm_id = MY_HTONL(mm_ptr->id);
        if(find_local_image_name(mm_ptr->type, mm_id,  g_pkg_status_p->filepath))
            return -1;

        if((filesize = GetFileSize(g_pkg_status_p->filepath)) < 0)
            return -1;

        //记录当前包的信息, 发送应答
        g_pkg_status_p->mm.type = mm_ptr->type;
        g_pkg_status_p->mm.id = mm_ptr->id;
        g_pkg_status_p->mm.packet_index = 0;
        g_pkg_status_p->mm.packet_total_num = MY_HTONS((filesize + IMAGE_SIZE_PER_PACKET - 1)/IMAGE_SIZE_PER_PACKET);
        g_pkg_status_p->mm_data_trans_waiting = 1;
        sample_assemble_msg_to_push(pHeader, SAMPLE_CMD_REQ_MM_DATA, NULL, 0);

        record_time(RECORD_START);
    }
    else
    {
        printf("current package is not valid!\n");
        return -1;
    }
    return 0;
}

static int recv_ack_and_send_image(sample_prot_header *pHeader, int32_t len)
{
    SendStatus pkg;
    sample_mm_ack mmack;

    //printf("recv ack...........!\n");
    if(pHeader->cmd == SAMPLE_CMD_UPLOAD_MM_DATA)//recv ack
    {
        memcpy(&mmack, pHeader+1, sizeof(mmack));
        if(mmack.ack)
        {
            printf("recv ack err!\n");
            return -1;
        }
        else
        {
            //printf("index = 0x%08x, index2 = 0x%08x\n", g_pkg_status_p->mm.packet_index, mmack.packet_index);

            if(MY_HTONS(g_pkg_status_p->mm.packet_index) == \
                    (MY_HTONS(mmack.packet_index) + 1)) //recv ack index is correct
            {
                //改变发送包，接收ACK状态为ready
                read_header_node(g_ptr_queue_p, &pkg, &ptr_queue_lock);
                pkg.ack_status = MSG_ACK_READY;
                write_header_node(g_ptr_queue_p, &pkg, &ptr_queue_lock);

                //最后一个ACK
                if(MY_HTONS(g_pkg_status_p->mm.packet_total_num) == \
                        MY_HTONS(g_pkg_status_p->mm.packet_index)) //the last pkg ack
                {
                    g_pkg_status_p->mm_data_trans_waiting = 0;
                    printf("transmit one file over!\n");
                    record_time(RECORD_END);
                    delete_mm_resource(MY_HTONL(g_pkg_status_p->mm.id));
                    //display_mm_resource();
                }
                else
                {
                    sample_send_image();
                }
            }
        }
    }
    return 0;
}

static int32_t sample_send_image()
{
    int ret=0;
    int offset=0;
    uint32_t retval = 0;
    uint8_t *data=NULL;
    uint8_t *txbuf=NULL;
    uint32_t datalen=0;
    uint32_t txbuflen=0;

    size_t filesize = 0;
    FILE *fp = NULL;
    sample_prot_header *pSend = NULL;


#if 0
    uint8_t data[IMAGE_SIZE_PER_PACKET + \
        sizeof(sample_prot_header) + sizeof(sample_mm_ack) + 64];
    uint8_t txbuf[(IMAGE_SIZE_PER_PACKET + \
            sizeof(sample_prot_header) + sizeof(sample_mm_ack) + 64)*2];
#endif

    datalen = IMAGE_SIZE_PER_PACKET + \
        sizeof(sample_prot_header) + sizeof(sample_mm_ack) + 64;
    txbuflen = (IMAGE_SIZE_PER_PACKET + \
            sizeof(sample_prot_header) + sizeof(sample_mm_ack) + 64)*2;

    data = (uint8_t *)malloc(datalen);
    if(!data)
    {
        perror("send image malloc");
        retval = 1;
        goto out;
    }
    txbuf = (uint8_t *)malloc(txbuflen);
    if(!txbuf)
    {
        perror("send image malloc");
        retval = 1;
        goto out;
    }

    pSend = (sample_prot_header *) txbuf;

    fp = fopen(g_pkg_status_p->filepath, "rb");
    if(fp ==NULL)
    {
        printf("open %s fail\n", g_pkg_status_p->filepath);
        retval = -1;
        goto out;
    }
    memcpy(data, &g_pkg_status_p->mm, sizeof(g_pkg_status_p->mm));
    offset = MY_HTONS(g_pkg_status_p->mm.packet_index) * IMAGE_SIZE_PER_PACKET;
    fseek(fp, offset, SEEK_SET);
    ret = fread(data + sizeof(g_pkg_status_p->mm), 1, IMAGE_SIZE_PER_PACKET, fp);
    fclose(fp);
    if(ret>0)
    {
        sample_assemble_msg_to_push(pSend, SAMPLE_CMD_UPLOAD_MM_DATA, \
                data, (sizeof(g_pkg_status_p->mm) + ret));
        printf("send...[%d/%d]\n", MY_HTONS(g_pkg_status_p->mm.packet_total_num),\
                MY_HTONS(g_pkg_status_p->mm.packet_index));
        g_pkg_status_p->mm.packet_index = MY_HTONS(MY_HTONS(g_pkg_status_p->mm.packet_index) + 1);
    }
    else//end and clear
    {
        printf("read file ret <=0\n");
    }

out:
    if(data)
        free(data);
    if(txbuf)
        free(txbuf);

    return retval;
}

void write_real_time_data(sample_prot_header *pHeader, int32_t len)
{
    if(len == sizeof(sample_prot_header) + 1 + sizeof(real_time_data))
    {
        RealTimeDdata_process((real_time_data *)(pHeader+1), WRITE_REAL_TIME_MSG);
    }
    else
    {
        printf("recv cmd:0x%x, data len maybe error!\n", pHeader->cmd);
    }
}

void do_factory_reset()
{
    set_para_setting_default();

    write_local_para_file(LOCAL_PRAR_FILE);
}

void recv_para_setting(sample_prot_header *pHeader, int32_t len)
{
    para_setting recv_para;
    uint8_t ack = 0;
    char cmd[100];
    uint8_t txbuf[128] = {0};
    sample_prot_header *pSend = (sample_prot_header *) txbuf;
    int ret = 0;

    if(len == sizeof(sample_prot_header) + 1 + sizeof(recv_para))
    {
        memcpy(&recv_para, pHeader+1, sizeof(recv_para));

        //大端传输
        recv_para.auto_photo_time_period = MY_HTONS(recv_para.auto_photo_time_period);
        recv_para.auto_photo_distance_period = MY_HTONS(recv_para.auto_photo_distance_period);

        write_warn_para(&recv_para);
        ret = write_local_para_file(LOCAL_PRAR_FILE);

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
        if(!ret)
        {
            ack = 0;
            sample_assemble_msg_to_push(pSend, SAMPLE_CMD_SET_PARAM, (uint8_t*)&ack, 1);
        }
        else
        {
            ack = 1;
            sample_assemble_msg_to_push(pSend, SAMPLE_CMD_SET_PARAM, \
                    (uint8_t*)&ack, 1);
        }
    }
    else
    {
        printf("recv cmd:0x%x, data len maybe error!\n", pHeader->cmd);
    }
}

void send_para_setting(sample_prot_header *pHeader, int32_t len)
{
    para_setting send_para;
    uint8_t txbuf[256] = {0};
    sample_prot_header *pSend = (sample_prot_header *) txbuf;

    if(len == sizeof(sample_prot_header) + 1)
    {
        read_warn_para(&send_para);
        send_para.auto_photo_time_period = MY_HTONS(send_para.auto_photo_time_period);
        send_para.auto_photo_distance_period = MY_HTONS(send_para.auto_photo_distance_period);

        sample_assemble_msg_to_push(pSend, SAMPLE_CMD_GET_PARAM, \
                (uint8_t*)&send_para, sizeof(send_para));
    }
    else
    {
        printf("recv cmd:0x%x, data len maybe error!\n", pHeader->cmd);
    }
}

void recv_warning_ack(sample_prot_header *pHeader, int32_t len)
{
    SendStatus pkg;

    if(len == sizeof(sample_prot_header) + 1)
    {
        read_header_node(g_ptr_queue_p, &pkg, &ptr_queue_lock);
        pkg.ack_status = MSG_ACK_READY;
        write_header_node(g_ptr_queue_p, &pkg, &ptr_queue_lock);
    }
    else
    {
        printf("recv cmd:0x%x, data len maybe error!\n", pHeader->cmd);
    }
}

void send_work_status_req_ack(sample_prot_header *pHeader, int32_t len)
{
    module_status module;
    uint8_t txbuf[256] = {0};
    sample_prot_header *pSend = (sample_prot_header *) txbuf;

    memset(&module, 0, sizeof(module));

    if(len == sizeof(sample_prot_header) + 1)
    {
        module.work_status = MODULE_WORKING;
        sample_assemble_msg_to_push(pSend, SAMPLE_CMD_REQ_STATUS, \
                (uint8_t*)&module, sizeof(module));
    }
    else
    {
        printf("recv cmd:0x%x, data len maybe error!\n", pHeader->cmd);
    }
}
void send_work_status()
{
    module_status module;
    uint8_t txbuf[256] = {0};
    sample_prot_header *pSend = (sample_prot_header *) txbuf;

    memset(&module, 0, sizeof(module));
    module.work_status = MODULE_WORKING;
    sample_assemble_msg_to_push(pSend, SAMPLE_CMD_REQ_STATUS, \
            (uint8_t*)&module, sizeof(module));
}


void recv_upload_status_cmd_ack(sample_prot_header *pHeader, int32_t len)
{
    SendStatus pkg;

    if(len == sizeof(sample_prot_header) + 1)
    {
        read_header_node(g_ptr_queue_p, &pkg, &ptr_queue_lock);
        pkg.ack_status = MSG_ACK_READY;
        write_header_node(g_ptr_queue_p, &pkg, &ptr_queue_lock);
    }
    else
    {
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
int recv_upgrade_file(sample_prot_header *pHeader, int32_t len)
{
    int ret;
    int fd;
    char cmd_rm_file[50];
    static uint32_t sum = 0;
    static uint32_t sum_new = 0;
    uint8_t data[4];
    uint8_t data_ack[5];
    uint8_t ack[2];
    int32_t datalen = 0;
    uint8_t *pchar=NULL;
    static uint32_t packet_num = 0;
    static uint32_t packet_index = 0;
    static uint32_t offset = 0;
    uint8_t     message_id = 0x03;
    file_trans_msg file_trans;
    unsigned char txbuf[256] = {0};
    sample_prot_header * pSend = (sample_prot_header *) txbuf;

    pchar = (uint8_t *)(pHeader+1);
    message_id = *pchar;

    printf("doing message_id 0x%02x\n", message_id);
    if(message_id == UPGRADE_CMD_START ||\
            message_id == UPGRADE_CMD_CLEAN ||\
            message_id == UPGRADE_CMD_RUN)
    {

        if(message_id == UPGRADE_CMD_CLEAN)
        {
            //do clean
        }
        if(message_id == UPGRADE_CMD_RUN)
        {
            //do run, do upgrade
            printf("upgrade...\n");
            //system(UPGRADE_FILE_CMD);

        }
        ack[0] = message_id;
        ack[1] = 0;
        sample_assemble_msg_to_push(pSend, SAMPLE_CMD_UPGRADE, \
                ack, sizeof(ack));
    }
    else if(message_id == UPGRADE_CMD_TRANS) //recv file
    {
        memcpy(&file_trans, pchar+1, sizeof(file_trans));
        packet_num = MY_HTONS(file_trans.packet_num);
        packet_index = MY_HTONS(file_trans.packet_index);

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
            packet_num = MY_HTONS(file_trans.packet_num);
            packet_index = MY_HTONS(file_trans.packet_index);

            datalen = len - (sizeof(sample_prot_header) + 1 + 5);
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

                    memcpy(data_ack, pchar, sizeof(data_ack));
                    sample_assemble_msg_to_push(pSend, SAMPLE_CMD_UPGRADE, \
                            data_ack, sizeof(data_ack));

                    return 0;
                }
                else
                {
                    printf("sun check err!\n");
                }
            }
        }
        memcpy(data_ack, pchar, sizeof(data_ack));
        sample_assemble_msg_to_push(pSend, SAMPLE_CMD_UPGRADE, \
                data_ack, sizeof(data_ack));
    }
    else
        ;

    return 0;
}
static int32_t sample_on_cmd(sample_prot_header *pHeader, int32_t len)
{
    ptr_queue_node msgack;
    uint16_t serial_num = 0;

    sample_dev_info dev_info = {
        15, "MINIEYE",
        15, "M3",
        15, "1.0.0.1",
        15, "1.0.1.2", //soft version
        15, "0xF0321564",
        15, "SAMPLE",
    };
    uint8_t txbuf[128] = {0};
    sample_prot_header *pSend = (sample_prot_header *) txbuf;

    //do myself cmd
    if (SAMPLE_DEVICE_ID_ADAS != pHeader->device_id) {
        return 0;
    }

    serial_num = MY_HTONS(pHeader->serial_num);
    do_serial_num(&serial_num, RECORD_RECV_NUM);

    //printf("------cmd = 0x%x------\n", pHeader->cmd);
    switch (pHeader->cmd)
    {
        case SAMPLE_CMD_QUERY:
            sample_assemble_msg_to_push(pHeader, SAMPLE_CMD_QUERY, NULL, 0);
            break;

        case SAMPLE_CMD_FACTORY_RESET:
            sample_assemble_msg_to_push(pHeader, SAMPLE_CMD_FACTORY_RESET, NULL, 0);
            do_factory_reset();
            break;

        case SAMPLE_CMD_SPEED_INFO: //不需要应答
            write_real_time_data(pHeader, len);
            break;

        case SAMPLE_CMD_DEVICE_INFO:
            sample_assemble_msg_to_push(pSend, SAMPLE_CMD_DEVICE_INFO,
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
            //发送多媒体请求应答 和 多媒体包
            if(!send_mm_req_ack(pHeader,len))
                sample_send_image();
            break;

        case SAMPLE_CMD_UPLOAD_MM_DATA:
            recv_ack_and_send_image(pHeader, len);
            break;

        case SAMPLE_CMD_SNAP_SHOT:
            printf("------snap shot----------\n");
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

static int try_connect()
{
#define HOST_SERVER_PORT (8888)

    int sock;
    int32_t ret = 0;
    int enable = 1;
    const char *server_ip = "192.168.100.100";
    struct sockaddr_in host_serv_addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("Create socket failed %s\n", strerror(errno));
        return -2;
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    memset(&host_serv_addr, 0, sizeof(host_serv_addr));
    host_serv_addr.sin_family = AF_INET;
    host_serv_addr.sin_port   = MY_HTONS(HOST_SERVER_PORT);

    ret = inet_aton(server_ip, &host_serv_addr.sin_addr);
    if (0 == ret) {
        printf("inet_aton failed %d %s\n", ret, strerror(errno));
        return -2;
    }

    while(1)
    {

        if( 0 == connect(sock, (struct sockaddr *)&host_serv_addr, sizeof(host_serv_addr)))
        {
            printf("connect ok!\n");
            return sock;
        }
        else
        {
            sleep(1);
            printf("try connect!\n");
        }
    }
}

void *communicate_with_host(void *para)
{
    uint8_t readbuf[1024];
    int32_t ret = 0;
    int retval = 0;;
    int hostsock = -1;
    fd_set rfds, wfds;
    struct timeval tv;
    int i=0;
    static int tcprecvcnt = 0;

    repeat_send_pkg_status_init();

connect_again:
    hostsock = try_connect();
    if(hostsock < 0)
        goto connect_again;

    while (1) {

        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(hostsock, &rfds);
        FD_SET(hostsock, &wfds);

        tv.tv_sec = 2;
        tv.tv_usec = 0;
        retval = select((hostsock + 1), &rfds, &wfds, NULL, &tv);
        if (retval == -1)
        {
            perror("select()");
            continue;
        }
        else if (retval)
        {
            if(FD_ISSET(hostsock, &rfds))
            {
                memset(readbuf, 0, sizeof(readbuf));
                ret = read(hostsock, readbuf, sizeof(readbuf));
                if (ret <= 0) {
                    printf("read failed %d %s\n", ret, strerror(errno));
                    close(hostsock);
                    hostsock = -1;
                    goto connect_again;

                    //continue;
                }
                else//write to buf
                {
                    //   MY_DEBUG("recv raw cmd, tcprecvcnt = %d:\n", tcprecvcnt++);
                    //     printbuf(readbuf, ret);
                    i=0;
                    while(ret--)
                    {
                        if(!uchar_queue_push(&readbuf[i]))
                        {
                            i++;
                            //printf("i = %d\n", i);
                            continue;
                        }
                        else
                            usleep(10);
                    }
                }
            }
            if(FD_ISSET(hostsock, &wfds))
            {
                send_pkg_to_host(hostsock);
                //tcpsend_pkg_to_host(hostsock);
            }
        }
        else
        {
            printf("No data within 2 seconds.\n");
        }
    }
    return NULL;
}

static int unescaple_msg(uint8_t *msg, int msglen)
{
    uint8_t ch = 0;
    char get_head = 0;
    char got_esc_char = 0;
    int cnt = 0;

    int i = 0;

    if(!msg || msglen <0)
        return -1;

    while(1)
    {
        if(cnt+1 > msglen)
        {
            printf("error: msg too long\n");
            return -1;
        }
        if(!uchar_queue_pop(&ch))//pop success
        {
            // printf("data[%d] = 0x%02x\n", i++, ch);
            if(!get_head)//not recv head
            {
                if((ch == SAMPLE_PROT_MAGIC) && (cnt == 0))//get head
                {
                    msg[cnt] = SAMPLE_PROT_MAGIC;
                    cnt++;
                    get_head = 1;
                    continue;
                }
                else
                    continue;
            }
            else//recv head
            {
                if((ch == SAMPLE_PROT_MAGIC) && (cnt > 0))//get tail
                {

                    if(cnt < 6)//maybe error frame, as header, restart
                    {
                        cnt = 0;
                        msg[cnt] = SAMPLE_PROT_MAGIC;
                        cnt++;
                        get_head = 1;
                        continue;
                    }
                    else
                    {
                        //printf("get tail! cnt = %d, return\n", cnt);
                        msg[cnt] = SAMPLE_PROT_MAGIC;
                        get_head = 0;//over
                        cnt++;
                        return cnt;
                    }
                }
                else//get text
                {
                    if((ch == SAMPLE_PROT_ESC_CHAR) && !got_esc_char)//need deal
                    {
                        got_esc_char = 1;
                        msg[cnt] = ch;
                        cnt++;
                    }
                    else if(got_esc_char && (ch == 0x02))
                    {
                        msg[cnt-1] = SAMPLE_PROT_MAGIC;
                        got_esc_char = 0;
                    }
                    else if(got_esc_char && (ch == 0x01))
                    {
                        msg[cnt-1] = SAMPLE_PROT_ESC_CHAR;
                        got_esc_char = 0;
                    }
                    else
                    {
                        msg[cnt] = ch;
                        cnt++;
                        got_esc_char = 0;
                    }
                }
            }
        }
        else
        {
            usleep(20);
        }
    }
}

void *parse_host_cmd(void *para)
{
    int32_t ret = 0;
    uint8_t sum = 0;
    uint32_t framelen = 0;
    unsigned char msgbuf[2048];
    sample_prot_header *pHeader = (sample_prot_header *) msgbuf;

    while(1)
    {
        ret = unescaple_msg(msgbuf, sizeof(msgbuf));
        if(ret>0)
        {
            framelen = ret;
            //printf("recv framelen = %d\n", framelen);
            sum = sample_calc_sum(pHeader, framelen);
            if (sum != pHeader->checksum) {
                printf("Checksum missmatch calcated: 0x%02hhx != 0x%2hhx\n",
                        sum, pHeader->checksum);
            }
            else
            {
                sample_on_cmd(pHeader, framelen);
            }
        }
        else
            ;
    }
}

#define SNAP_SHOT_CLOSE            0
#define SNAP_SHOT_BY_TIME          1
#define SNAP_SHOT_BY_DISTANCE      2
void *pthread_snap_shot(void *p)
{
    para_setting tmp;
    real_time_data rt_data;;
    uint32_t mileage = 0;
    int start = 0;
    int mileage_start = 0;
    struct timeval record_time;  

    send_work_status();
    while(1)
    {
        read_warn_para(&tmp);
        if(tmp.auto_photo_mode == SNAP_SHOT_CLOSE)
        {
            sleep(2);
        }
        else if(tmp.auto_photo_mode == SNAP_SHOT_BY_TIME)
        {
            if(start == 0 && tmp.auto_photo_time_period != 0)
            {
                gettimeofday(&record_time, NULL);
                start = 1;
            }
            if(start == 1)
            {
                if(timeout_trigger(&record_time, tmp.auto_photo_time_period*1000))//timeout
                {
                    start = 0;
                    do_snap_shot();
                }
            }
            usleep(600000);//wait 600ms, get new rt_data

        }
        else if(tmp.auto_photo_mode == SNAP_SHOT_BY_DISTANCE)
        {
            RealTimeDdata_process(&rt_data, READ_REAL_TIME_MSG);
            if(rt_data.mileage != 0)
            {
                if(mileage_start == 0)
                {
                    mileage_start = 1;
                    mileage = rt_data.mileage; //单位是0.1km
                    printf("start snap by mileage!\n");
                }
                if((rt_data.mileage - mileage)*100 >= tmp.auto_photo_distance_period)
                {
                    printf("snap by mileage!\n");
                    mileage = rt_data.mileage; //单位是0.1km
                    do_snap_shot();
                }
                usleep(600000);//wait 600ms, get new rt_data
            }
        }
        else
        {
            sleep(2);
        }
    }
}


