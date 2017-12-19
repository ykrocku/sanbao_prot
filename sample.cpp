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

typedef struct _sqbuf{
    int32_t len;
    uint8_t *buf;
}msgbuf;

typedef struct _sq_queue{
#define QUEUE_BUF_SIZE   (1024 + 64)
#define QUEUE_BUF_CNT    (16)
#define nextIndex(n) ((n + 1) % (QUEUE_BUF_CNT))
    sem_t sem;
    pthread_mutex_t lock;
    msgbuf *msg;
    uint32_t cnt;
}data_to_ptr_queue;

queue<void *> ptr_queue;

queue<uint8_t> char_queue;
#define CHAR_QUEUE_SIZE    (2048)

static data_to_ptr_queue sq;

int msg_queue_init()
{
    int i=0;

    pthread_mutex_init(&sq.lock, NULL);
    memset(&sq, 0, sizeof(sq));

    sq.msg = (msgbuf *)malloc(QUEUE_BUF_CNT * sizeof(msgbuf));
    if(!sq.msg)
    {
        perror("malloc:");
        return -1;
    }
    for(i=0; i<QUEUE_BUF_CNT; i++)
    {
        sq.msg[i].buf = (uint8_t *)malloc(QUEUE_BUF_SIZE);
        if(!sq.msg[i].buf)
        {
            perror("malloc:");
            return -1;
        }
    }
    return 0;
}

int sq_queue_destory()
{
    int i=0;

    for(i=0; i<QUEUE_BUF_CNT; i++)
    {
        free(sq.msg[i].buf);
    }
    free(sq.msg);

    sem_destroy(&sq.sem);
    pthread_mutex_destroy(&sq.lock);

    return 0;
}

int ptr_queue_push(uint8_t *msgbuf, int len)
{
    void *addr;

    if((msgbuf == NULL) || (len < 0))
        return -1;

    pthread_mutex_lock(&sq.lock);

    if((int)ptr_queue.size() == QUEUE_BUF_CNT)
    {
        pthread_mutex_unlock(&sq.lock);
        return -1;
    }

    sq.msg[sq.cnt].len = (len > QUEUE_BUF_SIZE ? QUEUE_BUF_SIZE : len);
    memcpy(sq.msg[sq.cnt].buf, msgbuf, sq.msg[sq.cnt].len);
    printf("queue msg wr = %d\n", sq.cnt);
    printf("queue msg len = %d\n", sq.msg[sq.cnt].len);

    addr = (void *)&sq.msg[sq.cnt];
    ptr_queue.push(addr);

    sq.cnt = nextIndex(sq.cnt);

    pthread_mutex_unlock(&sq.lock);

    return 0;
}


int ptr_queue_pop(uint8_t *buf)
{
    void *addr = NULL;
    msgbuf *msg = NULL;
    int32_t len = 0;

    if(!ptr_queue.empty())
    {
        addr = ptr_queue.front();
        ptr_queue.pop();

        msg = (msgbuf *)addr;
        memcpy(buf, msg->buf, msg->len);
        len = msg->len;

        memset(msg->buf, 0, len);
        msg->len  = 0;
    }
    return len;
}

int char_queue_push(uint8_t *buf, int len)
{
    int i=0;

    if((buf == NULL) || (len < 0))
        return -1;

    for(i=0; i<len; i++)
    {
        if((int)ptr_queue.size() == CHAR_QUEUE_SIZE)
        {
            return -1;
        }
        char_queue.push(buf[i]);
    }
    return 0;
}
int8_t char_queue_pop(uint8_t *ch)
{

    if(!char_queue.empty())
    {
        *ch = char_queue.front();
        char_queue.pop();
        return 0;
    }
    
        return -1;
}

uint8_t sample_calc_crc(sample_prot_header *pHeader, int32_t msg_len)
{
    int32_t i = 0;
    uint32_t chksum = 0;
    uint8_t * start = (uint8_t *) &pHeader->vendor_id;

#define NON_CRC_LEN (2 * sizeof(pHeader->magic) /*head and tail*/ + \
        sizeof(pHeader->version) + \
        sizeof(pHeader->checksum))

    for (i = 0; i < msg_len - NON_CRC_LEN; i++) {
        chksum += start[i];

        //   NB_DEBUG("#%04d 0x%02hhx = 0x%08x\n", i, start[i], chksum);
    }
    return (uint8_t) (chksum & 0xFF);
}

int32_t sample_escaple_msg(sample_prot_header *pHeader, int32_t msg_len)
{
    int32_t i = 0;
    int32_t escaped_len = msg_len;
    uint8_t *barray = (uint8_t*) pHeader;

    //ignore head/tail magic
    for (i = 1; i < escaped_len - 1; i++) {
        if (SAMPLE_PROT_MAGIC == barray[i]) {
            memmove(&barray[i+1], &barray[i], escaped_len - i);
            barray[i] = SAMPLE_PROT_ESC_CHAR;
            barray[i+1] = 0x2;
            i++;
            escaped_len ++;
        } else if (SAMPLE_PROT_ESC_CHAR == barray[i]) {
            memmove(&barray[i+1], &barray[i], escaped_len - i);
            barray[i]   = SAMPLE_PROT_ESC_CHAR;
            barray[i+1] = 0x1;
            i++;
            escaped_len ++;
        }
    }
    return escaped_len;
}

int32_t sample_unescaple_msg(sample_prot_header *pHeader, int32_t escaped_len)
{
    int32_t i = 0;
    int32_t msg_len = escaped_len;
    uint8_t *barray = (uint8_t*) pHeader;

    if (SAMPLE_PROT_MAGIC != barray[0] || SAMPLE_PROT_MAGIC != barray[escaped_len - 1]) {
        return 0;
    }

    //ignore head magic /char before tail magic
    for (i = 1; i < escaped_len - 2; i++) {
        if (SAMPLE_PROT_ESC_CHAR == barray[i] && (0x1 == barray[i+1] || 0x2 == barray[i+1])) {
            barray[i+1]   = SAMPLE_PROT_ESC_CHAR + (barray[i+1] - 1);
            memmove(&barray[i], &barray[i + 1], escaped_len - i);
            msg_len --; 
        }
    }
    return msg_len;
}

int32_t sample_assemble_msg(sample_prot_header *pHeader, uint8_t cmd,
        uint8_t *payload, int32_t payload_len)
{
    int32_t msg_len = sizeof(*pHeader) + 1 + payload_len;
    uint8_t *data = ((uint8_t*) pHeader + sizeof(*pHeader));
    uint8_t *tail = data + payload_len;

    memset(pHeader, 0, sizeof(*pHeader));
    pHeader->magic = SAMPLE_PROT_MAGIC;
    //    pHeader->version  = htons(SANBAO_VERSION); //used as message cnt
    pHeader->vendor_id= htons(VENDOR_ID);
    pHeader->device_id= SAMPLE_DEVICE_ID_ADAS;
    pHeader->cmd = cmd;

    if (payload_len > 0) {
        memcpy(data, payload, payload_len);
    }
    tail[0] = SAMPLE_PROT_MAGIC;

    pHeader->checksum = sample_calc_crc(pHeader, msg_len);

    msg_len = sample_escaple_msg(pHeader, msg_len);

    return msg_len;
}

uint32_t get_media_id()
{
    long int val;
    static char s = 1;

    if(s)
    {
        srand(time(NULL));
        s = 0;
    }
    val = rand();

    return val&0xFFFFFFFF;
}

static MECANWarningMessage g_last_warning_data;
static MECANWarningMessage g_last_can_msg;
static uint8_t g_last_trigger_warning[sizeof(MECANWarningMessage)];
int can_message_send(can_algo *sourcecan)
{
    time_t timep;   
    struct tm *p; 
    warningtext uploadmsg;
    MECANWarningMessage can;
    car_info carinfo;
    uint8_t cmd = 0;
    static uint32_t warning_id = 0;
    uint8_t txbuf[512];
    size_t msg_len = 0;
    char warn_event = 0;
    static uint8_t image_cnt = 0;
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


#if 0
    printf("type:%d\n", can.sound_type);
    //    printf("%d\n", can.byte0_resv);
    //    printf("%d\n", can.byte1_resv1);
    printf("zero:%d\n", can.zero_speed);
    //    printf("%d\n", can.byte1_resv0);
    printf("headway valid:%d\n", can.headway_valid);
    printf("headway mea:%d\n", can.headway_measurement);
    printf("no_error:%d\n", can.no_error);
    //    printf("%d\n", can.byte3_resv);
    printf("ldwoff:%d\n", can.ldw_off);
    printf("left_ldw:%d\n", can.left_ldw);
    printf("right_ldw:%d\n", can.right_ldw);
    printf("fcwon:%d\n", can.fcw_on);
    //    printf("%d\n", can.byte4_resv);
    //    printf("%d\n", can.byte5_resv);
    //    printf("%d\n", can.byte6_resv);
    printf("level:%d\n", can.headway_warning_level);
    //    printf("%d\n", can.byte7_resv);
#endif

    if(!memcmp(sourcecan->topic, MESSAGE_CAN700, strlen(MESSAGE_CAN700)))
    {
        printf("recv can 700 data!\n");
   //     printbuf(sourcecan->warning, 8);

        memcpy(&can, sourcecan->warning, sizeof(sourcecan->warning));

        for (i = 0; i < sizeof(all_warning_masks); i++) {
            all_warning_data[i] = sourcecan->warning[i] & all_warning_masks[i];
            trigger_data[i]     = sourcecan->warning[i] & trigger_warning_masks[i];
        }
  //      printbuf(all_warning_data, 8);
    //    printbuf((uint8_t *)&g_last_warning_data, 8);

        if (0 == memcmp(all_warning_data, &g_last_warning_data, sizeof(g_last_warning_data))) {
            return 0;
        }
        printf("warning happened........\n");
        printf("headway mea:%d\n", can.headway_measurement);
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



#if 1       
    uploadmsg.warning_id = htons(warning_id & 0xFFFFFFFF);
    warning_id++ ;
    uploadmsg.start_flag = 0;
    if(!system("/system/bin/rbget"))//get image success
    {
        uploadmsg.mm.id = uploadmsg.warning_id;
        sprintf(image_name, "/mnt/obb/rb_last_frame%d.jpg", image_cnt%10);
        rename("/mnt/obb/rb_last_frame.jpg", image_name);
        uploadmsg.mm_num = image_cnt;
        uploadmsg.mm.type = MM_PHOTO;
        //   uploadmsg.mm.id = htons(get_media_id());
        image_cnt++;
    }
#endif


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

            msg_len = sample_assemble_msg(pSend, SAMPLE_CMD_WARNING_REPORT, (uint8_t *)&uploadmsg, sizeof(uploadmsg));
            ptr_queue_push(txbuf, msg_len);
            printf("send mgslen = %d, warning_id = %d\n", (int)msg_len, warning_id);
        }
        if (can.fcw_on) {
            uploadmsg.start_flag = SW_STATUS_EVENT;
            uploadmsg.sound_type = SW_TYPE_FCW;
            msg_len = sample_assemble_msg(pSend, SAMPLE_CMD_WARNING_REPORT, (uint8_t *)&uploadmsg, sizeof(uploadmsg));
            ptr_queue_push(txbuf, msg_len);
            printf("send mgslen = %d, warning_id = %d\n", (int)msg_len, warning_id);
        }
    }
    //Headway
    if (g_last_warning_data.headway_warning_level != can.headway_warning_level) {
        printf("------Headway event-----------\n");
        if (HW_LEVEL_RED_CAR == can.headway_warning_level) {
            uploadmsg.start_flag = SW_STATUS_BEGIN;
            uploadmsg.sound_type = SW_TYPE_HW;
            msg_len = sample_assemble_msg(pSend, SAMPLE_CMD_WARNING_REPORT, (uint8_t *)&uploadmsg, sizeof(uploadmsg));
            ptr_queue_push(txbuf, msg_len);
            printf("send mgslen = %d, warning_id = %d\n", (int)msg_len, warning_id);
        } else if (HW_LEVEL_RED_CAR == g_last_warning_data.headway_warning_level) {
            uploadmsg.start_flag = SW_STATUS_END;
            uploadmsg.sound_type = SW_TYPE_HW;
            msg_len = sample_assemble_msg(pSend, SAMPLE_CMD_WARNING_REPORT, (uint8_t *)&uploadmsg, sizeof(uploadmsg));
            ptr_queue_push(txbuf, msg_len);
            printf("send mgslen = %d, warning_id = %d\n", (int)msg_len, warning_id);
        }
    }

    memcpy(&g_last_can_msg, &can, sizeof(g_last_can_msg));
    memcpy(&g_last_warning_data, all_warning_data, sizeof(g_last_warning_data));
    memcpy(&g_last_trigger_warning, trigger_data, sizeof(g_last_trigger_warning));
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

int32_t sample_send_image(sample_prot_header *pHeader, int32_t len)
{
#define IMAGE_SIZE_PER_PACKET   (512)
    uint8_t data[IMAGE_SIZE_PER_PACKET + \
        sizeof(sample_prot_header) + sizeof(sample_mm_ack) + 64];
    uint8_t txbuf[(IMAGE_SIZE_PER_PACKET + \
            sizeof(sample_prot_header) + sizeof(sample_mm_ack) + 64)*2];
    size_t msg_len = 0;
    size_t filesize;
    sample_prot_header *pSend = (sample_prot_header *) txbuf;
    FILE *fp;
    size_t ret;
    int val;
    static char image_name[50];
    char cnt=0;

    static int32_t total=0;
    static int32_t i = 0;
    static int32_t sent = 0;
    static int32_t to_send = 0;
    static char req = 0;

    sample_mm mm;
    sample_mm *mm_ptr;
    sample_mm_ack mmack;
    uint32_t MMID = 0;

    if((pHeader->cmd == SAMPLE_CMD_UPLOAD_MM_DATA) && (req == 1))//recv ack
    {
        memcpy(&mmack, pHeader+1, sizeof(mmack));

        //        NB_DEBUG("mmack:\n");
        //        printbuf((uint8_t *)&mmack, sizeof(mmack));
        //        printf("ack = %d\n", mmack.ack);
        //        mmack.packet_idx = ntohs(mmack.packet_idx);
        //        printf("packet_idx = %d\n", mmack.packet_idx);

        if(mmack.ack)
        {
            printf("recv no ack!\n");
            return 0;
        }

    }
    else if(pHeader->cmd == SAMPLE_CMD_REQ_MM_DATA)//recv req
    {
        printf("------------req mm-------------\n");

        if(len == sizeof(mm) + sizeof(sample_prot_header))
        {
            mm_ptr = (sample_mm *)(pHeader + 1);
            printf("mm_ptr->mm_id = 0x%08x\n", mm_ptr->mm_id);
            MMID = mm_ptr->mm_id;
        }

        i = 0;
        to_send = 0;
        sent = 0;
        req = 1;

        //pHeader->cmd = SAMPLE_CMD_UPLOAD_MM_DATA;

        sprintf(image_name, "/mnt/obb/%d.jpg", MMID);
        printf("try find filename: %s\n", image_name);
        fp = fopen(image_name, "r");
        if(fp ==NULL)
        {
            printf("open %s fail\n", image_name);
            return -1;
        }
        else
        {
            printf("open %s ok\n", image_name);
        }

        fseek(fp, 0, SEEK_SET);
        fseek(fp, 0, SEEK_END);
        filesize = ftell(fp);
        fclose(fp);
        total = (filesize + IMAGE_SIZE_PER_PACKET - 1)/IMAGE_SIZE_PER_PACKET;

        msg_len = sample_assemble_msg(pHeader, SAMPLE_CMD_REQ_MM_DATA, NULL, 0);
        ret = ptr_queue_push((uint8_t *)pHeader, msg_len);//send
        if(ret < 0)
        {
            printf("ptr_queue_push error!\n");
        }

    }
    else
    {
        return 0;
    }

    mm.req_type = MM_PHOTO;
    mm.mm_id = htons(MM_ID);
    mm.packet_num = htons(total);
    mm.packet_idx = htons(i);

    memcpy(data, &mm, sizeof(mm));
    fp = fopen(image_name, "r");
    if(fp ==NULL)
    {
        printf("open %s fail\n", image_name);
        return -1;
    }
    fseek(fp, sent, SEEK_SET);
    to_send = fread(data+ sizeof(mm), 1, IMAGE_SIZE_PER_PACKET, fp);
    if(to_send >0)
    {
        msg_len = sample_assemble_msg(pSend, SAMPLE_CMD_UPLOAD_MM_DATA, data, sizeof(mm) + to_send);
        ret = ptr_queue_push((uint8_t *)pSend, msg_len);//send
        if(ret < 0)
        {
            printf("ptr_queue_push error!\n");
        }

        sent += to_send;
        printf("i==%d, tosend = %d, sent = %d\n", i, to_send, sent);
        i++;
    }
    else//end and clear
    {
        printf("transmit over!\n");
        printf("i==%d, tosend = %d, sent = %d\n", i, to_send, sent);
        i = 0;
        to_send = 0;
        sent = 0;
        total = 0;
        req = 0;
    }
    fclose(fp);

    return 0;
}

int32_t sample_on_cmd(sample_prot_header *pHeader, int32_t len)
{

    sample_dev_info dev_info = {
        15, "MINIEYE",
        15, "M3",
        15, "1.0.0.1",
        15, "1.0.1.2",
        15, "0xF0321564",
        15, "SAMPLE",
    };
    size_t msg_len = 0;
    uint8_t txbuf[128] = {0};
    sample_prot_header *pSend = (sample_prot_header *) txbuf;

    //do myself cmd
    if (SAMPLE_DEVICE_ID_ADAS != pHeader->device_id) {
        return 0;
    }

    NB_DEBUG("cmd = 0x%x\n", pHeader->cmd);
    switch (pHeader->cmd)
    {
        case SAMPLE_CMD_QUERY:
            msg_len = sample_assemble_msg(pHeader, SAMPLE_CMD_QUERY, NULL, 0);
            ptr_queue_push((uint8_t *)pHeader, msg_len);
            break;

        case SAMPLE_CMD_FACTORY_RESET:
            msg_len = sample_assemble_msg(pHeader, SAMPLE_CMD_FACTORY_RESET, NULL, 0);
            ptr_queue_push((uint8_t *)pHeader, msg_len);
            break;

        case SAMPLE_CMD_DEVICE_INFO:
            msg_len = sample_assemble_msg(pSend, SAMPLE_CMD_DEVICE_INFO,
                    (uint8_t*)&dev_info, sizeof(dev_info));
            ptr_queue_push(txbuf, msg_len);
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
            //     msg_len = sample_escaple_msg(pHeader, len);
            //   ptr_queue_push((unsigned char *)pHeader, len);
            //    sample_send_image(pHeader, len);
            break;

        case SAMPLE_CMD_REQ_STATUS:
            break;

        case SAMPLE_CMD_UPLOAD_MM_DATA:
        case SAMPLE_CMD_REQ_MM_DATA:

            sample_send_image(pHeader, len);
            break;
        default:
            break;
    }
    return 0;
}

int try_connect()
{
#define HOST_SERVER_PORT (8888)

    int sock;
    int32_t ret = 0;
    int enable = 1;
    //    const char *server_ip = "127.0.0.1";
    const char *server_ip = "192.168.100.100";
    struct sockaddr_in minit_serv_addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("Create socket failed %s\n", strerror(errno));
        exit(1);
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    memset(&minit_serv_addr, 0, sizeof(minit_serv_addr));
    minit_serv_addr.sin_family = AF_INET;
    minit_serv_addr.sin_port   = htons(HOST_SERVER_PORT);

    ret = inet_aton(server_ip, &minit_serv_addr.sin_addr);
    if (0 == ret) {
        printf("inet_aton failed %d %s\n", ret, strerror(errno));
        exit(1);
    }

    while(1)
    {

        if( 0 == connect(sock, (struct sockaddr *)&minit_serv_addr, sizeof(minit_serv_addr)))
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

void *recv_from_host(void *para)
{
    uint8_t readbuf[1024];
    uint8_t sendbuf[QUEUE_BUF_SIZE];
    int32_t ret = 0;
    int retval;
    int hostsock;
    fd_set rfds, wfds;
    struct timeval tv;

    hostsock = try_connect();

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
            hostsock = try_connect();
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
                    hostsock = -2;
                    continue;
                }
                else//write to buf
                {
                    NB_DEBUG("recv raw cmd:\n");
                    printbuf(readbuf, ret);
                    char_queue_push(readbuf, ret);
                }
            }
            if(FD_ISSET(hostsock, &wfds))
            {
                ret = ptr_queue_pop(sendbuf);
                if(ret > 0)
                {
                    ret = write(hostsock, sendbuf, ret);
                    if(ret < 0)
                    {
                        perror("tcp send error");
                    }
                }
            }
        }
        else
        {
            printf("No data within 2 seconds.\n");
        }
    }
    return NULL;
}

void *parse_host_cmd(void *para)
{
    int8_t ret = 0;
    uint8_t crc = 0;
    uint8_t sec = 2;
    uint32_t framelen;
    unsigned char msgbuf[512];
    sample_prot_header *pHeader = (sample_prot_header *) msgbuf;
    while(1)
    {
        ret = qpop_unescaple_msg(msgbuf, sizeof(msgbuf), sec);
        if(ret>0){
            framelen = ret;
            NB_DEBUG("get a framelen = %d\n", framelen);
            printbuf(msgbuf, framelen);

            crc = sample_calc_crc(pHeader, framelen);
            if (crc != pHeader->checksum) {
                printf("Checksum missmatch calcated: 0x%02hhx != 0x%2hhx\n",
                        crc, pHeader->checksum);
            }
            else
            {
                printf("frame checksum match!\n");
                sample_on_cmd(pHeader, framelen);
            }

        }
        else if(ret == -1)
        {

        }
        else if(ret == -2)
        {

        }
        else
            ;
    }
}


int qpop_unescaple_msg(uint8_t *msg, int msglen, int timeout)
{
    uint8_t ch;
    char start = 0;
    char flag = 0;
    int cnt=0;
    int framelen = 0;
    int time_start_s = 0;
    int time_start_us = 0;
    struct timeval tv;

    gettimeofday(&tv, NULL);

    time_start_s = tv.tv_sec;
    time_start_us = tv.tv_usec;

    while(1)
    {

        gettimeofday(&tv, NULL);
        if((tv.tv_sec >= time_start_s + timeout) && (tv.tv_usec > time_start_us))
        {
           // printf("qpop msg timeout!\n");
            return -2;
        }
        if(cnt+1 > msglen)
        {
            printf("error: msg too long\n");
            return -1;
        }
        if(!char_queue_pop(&ch))
        {
            if(!start)
            {
                if((ch == SAMPLE_PROT_MAGIC) && (cnt == 0))//get head
                {
                    msg[cnt] = SAMPLE_PROT_MAGIC;
                    cnt++;
                    start = 1;
                    continue;
                }
                else
                    continue;
            }
            else
            {
                if((ch == SAMPLE_PROT_MAGIC) && (cnt > 0))//get tail
                {
                    if(cnt < 6)//maybe error frame, as head, restart
                    {
                        cnt = 0;
                        msg[cnt] = SAMPLE_PROT_MAGIC;
                        cnt++;
                        start = 1;
                        continue;
                    }

                    //get tail
                    msg[cnt] = SAMPLE_PROT_MAGIC;
                    start = 0;//over
                    cnt++;
                    //cnt = 0;

                    return cnt;

                }
                else//get text
                {
                    if((ch == SAMPLE_PROT_ESC_CHAR) && !flag)//need deal
                    {
                        flag = 1;
                        msg[cnt] = ch;
                        cnt++;
                    }
                    else if(flag && (ch == 0x02))
                    {
                        msg[cnt-1] = SAMPLE_PROT_MAGIC;
                        flag = 0;
                    }
                    else if(flag && (ch == 0x01))
                    {
                        msg[cnt-1] = SAMPLE_PROT_ESC_CHAR;
                        flag = 0;
                    }
                    else if(flag && (ch != 0x01) && (ch != 0x02))
                    {
                        msg[cnt] = ch;
                        cnt++;
                        flag = 0;
                    }
                    else
                    {
                        msg[cnt] = ch;
                        cnt++;
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


