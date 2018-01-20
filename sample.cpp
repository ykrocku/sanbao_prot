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

pthread_mutex_t ptr_queue_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t uchar_queue_lock = PTHREAD_MUTEX_INITIALIZER;

queue<ptr_queue_node *> g_ptr_queue;
queue<ptr_queue_node *> *g_ptr_queue_p = &g_ptr_queue;

queue<ptr_queue_node *> g_ack_queue;
queue<ptr_queue_node *> *g_ack_queue_p = &g_ack_queue;

queue<uint8_t> g_uchar_queue;

pkg_repeat_status g_pkg_status;
pkg_repeat_status *g_pkg_status_p = &g_pkg_status;

void repeat_send_pkg_status_init()
{
    memset(g_pkg_status_p, 0, sizeof(pkg_repeat_status));
}

static int ptr_queue_push(queue<ptr_queue_node *> *p, ptr_queue_node *in)
{
    int ret;
    ptr_queue_node *msg = NULL;
    uint8_t *ptr = NULL;

    if(!in || !p)
        return -1;

    pthread_mutex_lock(&ptr_queue_lock);
    if((int)p->size() == PTR_QUEUE_BUF_CNT)
    {
        ret = -1;
        goto over;
    }
    else
    {
        msg = (ptr_queue_node *)malloc(sizeof(ptr_queue_node));
        if(!msg)
        {
            perror("ptr_queue_push malloc1");
            ret = -1;
            goto over;
        }
        ptr = (uint8_t *)malloc(PTR_QUEUE_BUF_SIZE);
        if(!ptr)
        {
            perror("ptr_queue_push malloc2");
            free(msg);
            ret = -1;
            goto over;
        }

        memcpy(msg, in, sizeof(ptr_queue_node));
        msg->buf = ptr;
        if(!in->buf)//no buf push
        {
            msg->len = PTR_QUEUE_BUF_SIZE;
            memset(msg->buf, 0, msg->len);
        }
        else
        {
            msg->len = in->len > PTR_QUEUE_BUF_SIZE ? PTR_QUEUE_BUF_SIZE : in->len;
            memcpy(msg->buf, in->buf, msg->len);
        }

        p->push(msg);
        ret = 0;
        goto over;
    }

over:
    pthread_mutex_unlock(&ptr_queue_lock);
    return ret;
}

static int ptr_queue_pop(queue<ptr_queue_node*> *p, ptr_queue_node *out)
{
    ptr_queue_node *msg = NULL;
    int32_t len = 0;
    uint8_t *ptr = NULL;

    if(!out || !p)
        return -1;

    pthread_mutex_lock(&ptr_queue_lock);
    if(!p->empty())
    {
        msg = p->front();
        p->pop();
    }
    pthread_mutex_unlock(&ptr_queue_lock);

    if(!msg)
        return -1;

    if(!out->buf)//don't need data
    {
        memcpy(out, msg, sizeof(ptr_queue_node));
        out->buf = NULL;
    }
    else
    {
        ptr = out->buf;
        memcpy(out, msg, sizeof(ptr_queue_node));
        out->buf = ptr;
        out->len = msg->len > PTR_QUEUE_BUF_SIZE ? PTR_QUEUE_BUF_SIZE : msg->len;
        memcpy(out->buf, msg->buf, out->len);
    }

    free(msg->buf);
    free(msg);
    return 0;

}

static int send_package_timeout(struct timeval *tv, int timeout_sec)
{
    struct timeval tv_cur;

    gettimeofday(&tv_cur, NULL);

    if((tv_cur.tv_sec >= tv->tv_sec + timeout_sec) && \
            (tv_cur.tv_usec >= tv->tv_usec))
    {
        printf("recv ack timeout! %ds\n", timeout_sec);
        return 0;
    }
    else
        return 1;
}

static int unblock_write(int sock, uint8_t *buf, int len)
{
    int ret = 0;
    int offset = 0;

    if(sock < 0 || len < 0 || !buf)
    {
        return -1;
    }
    else
    {
        while(offset < len)
        {
            ret = write(sock, &buf[offset], len);
            if(ret < 0)
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

static int send_pkg_to_host(int sock)
{
    static uint8_t s_buf[PTR_QUEUE_BUF_SIZE];
    ptr_queue_node msgack;
    int ret = 0;

    g_pkg_status_p->msgsend.buf = s_buf;
    if(g_pkg_status_p->start_wait_ack || !ptr_queue_pop(g_ptr_queue_p, &g_pkg_status_p->msgsend))
    {
        if(g_pkg_status_p->msgsend.need_ack)// deal ack
        {
            if(g_pkg_status_p->repeat_cnt < REPEAT_SEND_TIMES_MAX) 
            {
                if(g_pkg_status_p->start_wait_ack == 0)
                {
                    ret = write(sock, g_pkg_status_p->msgsend.buf, g_pkg_status_p->msgsend.len);
                    g_pkg_status_p->repeat_cnt++;
                    gettimeofday(&g_pkg_status_p->msg_sendtime, NULL);
                    g_pkg_status_p->start_wait_ack = 1;
                }
                if(!send_package_timeout(&g_pkg_status_p->msg_sendtime, 2))//timeout
                {
                    g_pkg_status_p->repeat_cnt++;
                    ret = write(sock, g_pkg_status_p->msgsend.buf, g_pkg_status_p->msgsend.len);
                    gettimeofday(&g_pkg_status_p->msg_sendtime, NULL);
                }
                else//poll ack
                {
                    msgack.buf = NULL;
                    msgack.len = 0;
                    if(!ptr_queue_pop(g_ack_queue_p, &msgack))
                    {
                        if(msgack.cmd == g_pkg_status_p->msgsend.cmd)//send cmd and recv cmd match
                        {
                            g_pkg_status_p->repeat_cnt = 0;
                            g_pkg_status_p->start_wait_ack = 0;//reset pop
                            return 0;
                        }
                        else
                        {
                            printf("get ack cmd dismathch: ack cmd = %d, send cmd = 0x%x\n", msgack.cmd, g_pkg_status_p->msgsend.cmd);
                        }
                    }
                }
            }
            else//repeat 3 times over
            {
                if(g_pkg_status_p->msgsend.cmd == SAMPLE_CMD_UPLOAD_MM_DATA)//repeat over, trans over
                {
                    g_pkg_status_p->mm_data_trans_waiting = 0;
                }
                g_pkg_status_p->repeat_cnt = 0;
                g_pkg_status_p->start_wait_ack = 0;
            }
        }
        else
        {
            ret = unblock_write(sock, g_pkg_status_p->msgsend.buf, g_pkg_status_p->msgsend.len);
            if(ret >0)
            {
                printf("tcp write ret = %d, over\n", ret);
            }
            else
            {
                printf("tcp write fail:");
            }
        }
    }
    return 0;
}

static int uchar_queue_push(uint8_t *ch)
{
    if(!ch)
        return -1;

    pthread_mutex_lock(&uchar_queue_lock);
    if((int)g_uchar_queue.size() == UCHAR_QUEUE_SIZE)
    {
        printf("g_uchar_queue full flow\n");
        pthread_mutex_unlock(&uchar_queue_lock);
        return -1;
    }
    g_uchar_queue.push(ch[0]);
    pthread_mutex_unlock(&uchar_queue_lock);
    return 0;
}

static int8_t uchar_queue_pop(uint8_t *ch)
{
    pthread_mutex_lock(&uchar_queue_lock);
    if(!g_uchar_queue.empty())
    {
        *ch = g_uchar_queue.front();
        g_uchar_queue.pop();
        pthread_mutex_unlock(&uchar_queue_lock);
        return 0;
    }
    else
    {
        pthread_mutex_unlock(&uchar_queue_lock);
        return -1;
    }
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

static int32_t sample_assemble_msg_to_push(sample_prot_header *pHeader, uint8_t cmd,
        uint8_t *payload, int32_t payload_len)
{
    static uint16_t s_serial_num = 0;
    ptr_queue_node msg;
    int32_t msg_len = sizeof(*pHeader) + 1 + payload_len;
    uint8_t *data = ((uint8_t*) pHeader + sizeof(*pHeader));
    uint8_t *tail = data + payload_len;

    memset(pHeader, 0, sizeof(*pHeader));
    pHeader->magic = SAMPLE_PROT_MAGIC;
    pHeader->serial_num= htons(s_serial_num++); //used as message cnt
    pHeader->vendor_id= htons(VENDOR_ID);
    pHeader->device_id= SAMPLE_DEVICE_ID_ADAS;
    pHeader->cmd = cmd;

    if (payload_len > 0) 
    {
        memcpy(data, payload, payload_len);
    }
    tail[0] = SAMPLE_PROT_MAGIC;

    pHeader->checksum = sample_calc_sum(pHeader, msg_len);

    msg_len = sample_escaple_msg(pHeader, msg_len);

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
        case SAMPLE_CMD_UPLOAD_STATUS:
        case SAMPLE_CMD_REQ_MM_DATA:
            msg.need_ack = 0;
            break;

            //send as master
        case SAMPLE_CMD_UPLOAD_MM_DATA:
        case SAMPLE_CMD_WARNING_REPORT:
            msg.need_ack = 1;

            break;

        default:
            msg.need_ack = 0;
            break;

    }


    msg.cmd = cmd;
    msg.len = msg_len;
    msg.buf = (uint8_t *)pHeader;
    //    printf("sendpackage cmd = 0x%x,msg.need_ack = %d, len=%d, push!\n",msg.cmd, msg.need_ack, msg.len);
    ptr_queue_push(g_ptr_queue_p, &msg);

    return msg_len;
}

static MECANWarningMessage g_last_warning_data;
static MECANWarningMessage g_last_can_msg;
static uint8_t g_last_trigger_warning[sizeof(MECANWarningMessage)];
int can_message_send(can_data_type *sourcecan)
{
    time_t timep = 0;   
    struct tm *p = NULL; 
    warningtext uploadmsg;
    MECANWarningMessage can;
    car_info carinfo;
    static uint32_t s_warning_id = 0;
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
        memcpy(&can, sourcecan->warning, sizeof(sourcecan->warning));
        
        //printf("recv can700 data:\n");


        for (i = 0; i < sizeof(all_warning_masks); i++) {
            all_warning_data[i] = sourcecan->warning[i] & all_warning_masks[i];
            trigger_data[i]     = sourcecan->warning[i] & trigger_warning_masks[i];
        }

        if (0 == memcmp(all_warning_data, &g_last_warning_data, sizeof(g_last_warning_data))) {
            return 0;
        }

        if( (g_last_warning_data.headway_warning_level != can.headway_warning_level) || \
                (0 != memcmp(trigger_data, &g_last_trigger_warning, sizeof(g_last_trigger_warning)) && 0 != trigger_data[4]) )
        {
            printf("warning happened.........................\n");
            memset(&uploadmsg, 0, sizeof(uploadmsg));

            printf("time:%s\n", sourcecan->time);
            timep = strtoul(sourcecan->time, NULL, 10);
            timep = timep/1000000;
            p = localtime(&timep);
            uploadmsg.time[0] = p->tm_year;
            uploadmsg.time[1] = p->tm_mon+1;
            uploadmsg.time[2] = p->tm_mday;
            uploadmsg.time[3] = p->tm_hour;
            uploadmsg.time[4] = p->tm_min;
            uploadmsg.time[5] = p->tm_sec;

            printf("%d-%d-%d %d:%d:%d\n", (1900 + p->tm_year), ( 1 + p->tm_mon), p->tm_mday,(p->tm_hour), p->tm_min, p->tm_sec); 

            uploadmsg.warning_id = htonl(s_warning_id & 0xFFFFFFFF);
            uploadmsg.start_flag = 0;
            if(!system("/system/bin/rbget >/dev/null"))//get image success
            {
                uploadmsg.mm.id = uploadmsg.warning_id;
                sprintf(image_name, "/mnt/obb/rb_last_frame%d.jpg", s_warning_id % IMAGE_CACHE_NUM);
                rename("/mnt/obb/rb_last_frame.jpg", image_name);
                uploadmsg.mm_num = uploadmsg.mm.id;
                uploadmsg.mm.type = MM_PHOTO;
                //   uploadmsg.mm.id = htonl(get_media_id());
            }
            s_warning_id++ ;
        }
        //LDW and FCW event
        if (0 != memcmp(trigger_data, &g_last_trigger_warning, sizeof(g_last_trigger_warning)) && 0 != trigger_data[4]) {
            printf("------LDW/FCW event-----------\n");

            if (can.left_ldw || can.right_ldw) {
                uploadmsg.start_flag = SW_STATUS_EVENT;
                uploadmsg.sound_type = SW_TYPE_LDW;

                if(can.left_ldw)
                    uploadmsg.ldw_type = SOUND_TYPE_LLDW;
                if(can.right_ldw)
                    uploadmsg.ldw_type = SOUND_TYPE_RLDW;

                sample_assemble_msg_to_push(pSend, SAMPLE_CMD_WARNING_REPORT, (uint8_t *)&uploadmsg, sizeof(uploadmsg));
            }
            if (can.fcw_on) {
                uploadmsg.start_flag = SW_STATUS_EVENT;
                uploadmsg.sound_type = SW_TYPE_FCW;
                sample_assemble_msg_to_push(pSend, SAMPLE_CMD_WARNING_REPORT, (uint8_t *)&uploadmsg, sizeof(uploadmsg));
            }
        }
        //Headway
        if (g_last_warning_data.headway_warning_level != can.headway_warning_level) {
            printf("------Headway event-----------\n");
            printf("headway_warning_level:%d\n", can.headway_warning_level);
            printf("headway_measurement:%d\n", can.headway_measurement);

            if (HW_LEVEL_RED_CAR == can.headway_warning_level) {
                uploadmsg.start_flag = SW_STATUS_BEGIN;
                uploadmsg.sound_type = SW_TYPE_HW;
                sample_assemble_msg_to_push(pSend, SAMPLE_CMD_WARNING_REPORT, (uint8_t *)&uploadmsg, sizeof(uploadmsg));

            } else if (HW_LEVEL_RED_CAR == g_last_warning_data.headway_warning_level) {
                uploadmsg.start_flag = SW_STATUS_END;
                uploadmsg.sound_type = SW_TYPE_HW;
                sample_assemble_msg_to_push(pSend, SAMPLE_CMD_WARNING_REPORT, (uint8_t *)&uploadmsg, sizeof(uploadmsg));
            }
        }

        memcpy(&g_last_can_msg, &can, sizeof(g_last_can_msg));
        memcpy(&g_last_warning_data, all_warning_data, sizeof(g_last_warning_data));
        memcpy(&g_last_trigger_warning, trigger_data, sizeof(g_last_trigger_warning));

    }
#if 1
    if(!strncmp(sourcecan->topic, MESSAGE_CAN760, strlen(MESSAGE_CAN760)))
    {
        memcpy(&carinfo, sourcecan->warning, sizeof(sourcecan->warning));
        //			uploadmsg.high = carinfo.
        //			uploadmsg.altitude = carinfo. //htons(altitude)
        //			uploadmsg.longitude= carinfo. //htons(longitude)
        //			uploadmsg.car_status.acc = 
        uploadmsg.car_status.left_signal = carinfo.left_signal;
        uploadmsg.car_status.right_signal = carinfo.right_signal;

        if(carinfo.wipers_aval)
            uploadmsg.car_status.wipers = carinfo.wipers;

        uploadmsg.car_status.brakes = carinfo.brakes;
        //			uploadmsg.car_status.insert = 
    }
#endif

    return 0;
}

static int32_t sample_send_image(sample_prot_header *pHeader, int32_t len)
{
#define IMAGE_SIZE_PER_PACKET   (512)
    uint8_t data[IMAGE_SIZE_PER_PACKET + \
        sizeof(sample_prot_header) + sizeof(sample_mm_ack) + 64];
    uint8_t txbuf[(IMAGE_SIZE_PER_PACKET + \
            sizeof(sample_prot_header) + sizeof(sample_mm_ack) + 64)*2];
    size_t filesize = 0;
    FILE *fp = NULL;
    sample_prot_header *pSend = (sample_prot_header *) txbuf;

    static int32_t s_total=0;
    static char s_image_name[50];
    static int32_t s_package_cnt = 0;
    static int32_t s_sent = 0;
    static int32_t s_to_send = 0;
    static char s_req = 0;
    ptr_queue_node msg;

    sample_mm mm;
    sample_mm *mm_ptr = NULL;
    sample_mm_ack mmack;
    uint32_t MMID = 0;

    if((pHeader->cmd == SAMPLE_CMD_UPLOAD_MM_DATA) && (s_req == 1))//recv ack
    {
        memcpy(&mmack, pHeader+1, sizeof(mmack));
        if(mmack.ack)
        {
            printf("recv no ack!\n");
            return 0;
        }
        else
        {
            msg.cmd = SAMPLE_CMD_UPLOAD_MM_DATA;
            msg.len = 0;
            msg.buf = NULL;
            if(s_package_cnt == ntohs(mmack.packet_index) + 1)//recv ack index is correct
            {
                ptr_queue_push(g_ack_queue_p, &msg);
                if(s_total == s_package_cnt)
                {
                    printf("transmit over!\n");
                    g_pkg_status_p->mm_data_trans_waiting = 0;
                    s_package_cnt = 0;
                    s_to_send = 0;
                    s_sent = 0;
                    s_total = 0;
                    s_req = 0;
                    return 0;
                }
            }

        }
    }
    else if(pHeader->cmd == SAMPLE_CMD_REQ_MM_DATA && !g_pkg_status_p->mm_data_trans_waiting) //recv req
    {
        printf("------------req mm-------------\n");
        if(len == sizeof(mm) + sizeof(sample_prot_header))
        {
            mm_ptr = (sample_mm *)(pHeader + 1);
            printf("mm_ptr->mm_id = 0x%08x\n", mm_ptr->mm_id);
            MMID = htonl(mm_ptr->mm_id);
        }

        g_pkg_status_p->mm_data_trans_waiting = 1;
        s_package_cnt = 0;
        s_to_send = 0;
        s_sent = 0;
        s_req = 1;

        sprintf(s_image_name, "/mnt/obb/rb_last_frame%d.jpg", MMID % IMAGE_CACHE_NUM);
        printf("try find filename: %s\n", s_image_name);
        fp = fopen(s_image_name, "r");
        if(fp ==NULL)
        {
            printf("open %s fail\n", s_image_name);
            return -1;
        }
        else
        {
            printf("open %s ok\n", s_image_name);
        }

        fseek(fp, 0, SEEK_SET);
        fseek(fp, 0, SEEK_END);
        filesize = ftell(fp);
        fclose(fp);
        s_total = (filesize + IMAGE_SIZE_PER_PACKET - 1)/IMAGE_SIZE_PER_PACKET;
        sample_assemble_msg_to_push(pHeader, SAMPLE_CMD_REQ_MM_DATA, NULL, 0);
    }
    else
    {

        printf("current package is not valid!\n");

        return 0;
    }

    mm.req_type = MM_PHOTO;
    mm.mm_id = htonl(MM_ID);
    mm.packet_total_num = htons(s_total);
    mm.packet_index = htons(s_package_cnt);
    memcpy(data, &mm, sizeof(mm));
    fp = fopen(s_image_name, "r");
    if(fp ==NULL)
    {
        printf("open %s fail\n", s_image_name);
        return -1;
    }
    fseek(fp, s_sent, SEEK_SET);
    s_to_send = fread(data+ sizeof(mm), 1, IMAGE_SIZE_PER_PACKET, fp);
    fclose(fp);
    if(s_to_send >0)
    {
        sample_assemble_msg_to_push(pSend, SAMPLE_CMD_UPLOAD_MM_DATA, data, (sizeof(mm) + s_to_send));
        s_sent += s_to_send;
        printf("i==%d/%d, tosend = %d, sent = %d, cnt = %d\n", s_package_cnt, s_total, s_to_send, s_sent, s_package_cnt);
        s_package_cnt++;
    }
    else//end and clear
    {
    }
    return 0;
}

static int32_t sample_on_cmd(sample_prot_header *pHeader, int32_t len)
{
    ptr_queue_node msg;
    ptr_queue_node msgack;
    uint8_t buf[PTR_QUEUE_BUF_SIZE];

    sample_dev_info dev_info = {
        15, "MINIEYE",
        15, "M3",
        15, "1.0.0.1",
        15, "1.0.1.2",
        15, "0xF0321564",
        15, "SAMPLE",
    };
    uint8_t txbuf[128] = {0};
    sample_prot_header *pSend = (sample_prot_header *) txbuf;

    //do myself cmd
    if (SAMPLE_DEVICE_ID_ADAS != pHeader->device_id) {
        return 0;
    }

    switch (pHeader->cmd)
    {
        case SAMPLE_CMD_QUERY:
            sample_assemble_msg_to_push(pHeader, SAMPLE_CMD_QUERY, NULL, 0);
            break;

        case SAMPLE_CMD_FACTORY_RESET:
            sample_assemble_msg_to_push(pHeader, SAMPLE_CMD_FACTORY_RESET, NULL, 0);
            break;

        case SAMPLE_CMD_DEVICE_INFO:
            sample_assemble_msg_to_push(pSend, SAMPLE_CMD_DEVICE_INFO,
                    (uint8_t*)&dev_info, sizeof(dev_info));

            break;

        case SAMPLE_CMD_SPEED_INFO:
            //get car status
            break;

        case SAMPLE_CMD_UPGRADE:
            break;

        case SAMPLE_CMD_GET_PARAM:
            break;

        case SAMPLE_CMD_SET_PARAM:
            break;

        case SAMPLE_CMD_WARNING_REPORT:
            msg.cmd = SAMPLE_CMD_WARNING_REPORT;
            msg.buf = NULL;
            msg.len = 0;
            ptr_queue_push(g_ack_queue_p, &msg);//send
            break;

        case SAMPLE_CMD_REQ_STATUS:
            break;

        case SAMPLE_CMD_UPLOAD_MM_DATA:
        case SAMPLE_CMD_REQ_MM_DATA:
            sample_send_image(pHeader, len);
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
    host_serv_addr.sin_port   = htons(HOST_SERVER_PORT);

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
    uint8_t sendbuf[PTR_QUEUE_BUF_SIZE];
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
                        if(!uchar_queue_push(&readbuf[i++]))
                            continue;
                        else
                            usleep(10);
                    }
                }
            }
            if(FD_ISSET(hostsock, &wfds))
            {
                send_pkg_to_host(hostsock);
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
            if(!get_head)
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
            else
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
    int8_t ret = 0;
    uint8_t sum = 0;
    uint32_t framelen = 0;
    unsigned char msgbuf[512];
    sample_prot_header *pHeader = (sample_prot_header *) msgbuf;

    while(1)
    {
        ret = unescaple_msg(msgbuf, sizeof(msgbuf));
        if(ret>0)
        {
            framelen = ret;
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

