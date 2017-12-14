//#include <android_camrec.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <termios.h>
#include <time.h>
#include <arpa/inet.h>

#include <assert.h>

#include "sample.h"

#include <stdbool.h>
#include "ringbuffer.h"

#if 0
#define __swap16(x) \
    ((unsigned short int) ((((x) >> 8) & 0xff) | (((x) & 0xff) << 8)))
#define __swap32(x) \
    ((((x) & 0xff000000) >> 24) | (((x) & 0x00ff0000) >>  8) |		      \
     (((x) & 0x0000ff00) <<  8) | (((x) & 0x000000ff) << 24))
#define htons(s) __swap16(s)
#define ntohs(s) __swap16(s)
#define htonl(l) __swap32(l)
#define ntohl(l) __swap32(l)
#endif


/**************cirbuf******************/
ringBuffer_typedef(unsigned char, intBuffer);
intBuffer myBuffer;
intBuffer* myBuffer_ptr;


/**************tcp******************/
int hostsock = -1;

int cir_buf_init()
{

    bufferInit(myBuffer, 2048, unsigned char);

    myBuffer_ptr = &myBuffer;

    return 0;
}

ssize_t SendData(uint8_t *buf, size_t len)
{
    ssize_t ret;

    if(hostsock > 0)
    {
        ret = write(hostsock, buf, len);
    }

    return ret;
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
    pHeader->version  = htons(SANBAO_VERSION);
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


int can_message_send(can_algo *sourcecan)
{
    time_t timep;   
    struct tm *p; 
    warningtext uploadmsg;
    MECANWarningMessage can;
    car_info carinfo;
    uint8_t cmd = 0;
    uint8_t txbuf[512];
    size_t msg_len = 0;
    sample_prot_header *pSend = (sample_prot_header *) txbuf;

    //	uploadmsg.warning_id = 0; //htons(warning_id);
    //	uploadmsg.start_flag = can.

#if 0
    printf("%d\n", can.sound_type);
    printf("%d\n", can.byte0_resv);
    printf("%d\n", can.byte1_resv1);
    printf("%d\n", can.zero_speed);
    printf("%d\n", can.byte1_resv0);
    printf("%d\n", can.headway_valid);
    printf("%d\n", can.headway_measurement);
    printf("%d\n", can.error);
    printf("%d\n", can.byte3_resv);
    printf("%d\n", can.ldw_off);
    printf("%d\n", can.left_ldw);
    printf("%d\n", can.right_ldw);
    printf("%d\n", can.fcw_on);
    printf("%d\n", can.byte4_resv);
    printf("%d\n", can.byte5_resv);
    printf("%d\n", can.byte6_resv);
    printf("%d\n", can.headway_warning_level);
    printf("%d\n", can.byte7_resv);
#endif

    if(!strncmp(sourcecan->topic, MESSAGE_CAN700, strlen(MESSAGE_CAN700)))
    {

         //test warning
        sourcecan->warning[0] = 0x01;
        memcpy(&can, sourcecan->warning, sizeof(sourcecan->warning));

        printf("can.sound_type = %d\n", can.sound_type);
        //warning type
        if(can.sound_type == SOUND_TYPE_SILENCE)
        {
            //no warning ?
            return 0;
        }
        else if((can.sound_type == SOUND_TYPE_LLDW) || \
                (can.sound_type == SOUND_TYPE_RLDW))
        {
            uploadmsg.sound_type = SW_TYPE_LDW;
            if(!can.ldw_off)
            {
                if(can.right_ldw)
                    uploadmsg.ldw_type = can.right_ldw;
                if(can.left_ldw)
                    uploadmsg.ldw_type = can.left_ldw;
            }
        }
        else if(can.sound_type == SOUND_TYPE_HW) //??
        {
            uploadmsg.sound_type = SW_TYPE_HW;
            if(can.headway_measurement)
                uploadmsg.forward_speed = can.headway_measurement;

            //	uploadmsg.forward_Distance = can.  ?

        }
#if 0
        else if(can.sound_type == SOUND_TYPE_TSR)
        {

        }
        else if(can.sound_type == SOUND_TYPE_VB)
        {

        }
#endif
        else if(can.sound_type == SOUND_TYPE_FCW_PCW)
        {
            uploadmsg.sound_type = SW_TYPE_FCW;
            if(can.fcw_on)
            {
               // uploadmsg.forward_Distance
            }
        }
        else
        {
            return 0;
        }
        
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

    printf("%d-%d-%d %d:%d:%d\n", (1900 + p->tm_year), ( 1 + p->tm_mon), p->tm_mday,(p->tm_hour + 12), p->tm_min, p->tm_sec); 

    }

    if(!strncmp(sourcecan->topic, MESSAGE_CAN760, strlen(MESSAGE_CAN760)))
    {
        memcpy(&carinfo, sourcecan->warning, sizeof(sourcecan->warning));
        //			uploadmsg.high = carinfo.
        //			uploadmsg.altitude = carinfo. //htons(altitude)
        //			uploadmsg.longitude= carinfo. //htons(longitude)
        //			uploadmsg.status.acc = 
        uploadmsg.status.left_signal = carinfo.left_signal;
        uploadmsg.status.right_signal = carinfo.right_signal;

        if(carinfo.wipers_aval)
            uploadmsg.status.wipers = carinfo.wipers;

        uploadmsg.status.brakes = carinfo.brakes;
        //			uploadmsg.status.insert = 
    }

    //	uploadmsg.forward_speed = can.
    //	uploadmsg.forward_Distance = can.
    //	uploadmsg.ldw_type = can.
    //	uploadmsg.load_type = can.
    //	uploadmsg.load_data = can.
    //	uploadmsg.car_speed = can.
    //	uploadmsg.high = can.
    //	uploadmsg.altitude = can.
    //	uploadmsg.longitude= can.
    //	uploadmsg.time[6] = can.
    //	uploadmsg.car_status = can.
    //	uploadmsg.mm_num = can.

    msg_len = sample_assemble_msg(pSend, SAMPLE_CMD_WARNING_REPORT, (uint8_t *)&uploadmsg, sizeof(uploadmsg));
    SendData(txbuf, msg_len);
    
    printf("send mgslen = %d\n", (int)msg_len);
}


int32_t sample_send_image(sample_prot_header *pHeader, int32_t len)
{
#define IMAGE_SIZE_PER_PACKET   (512)
    uint8_t data[IMAGE_SIZE_PER_PACKET + sizeof(sample_prot_header) + sizeof(sample_mm_ack) + 64];
    uint8_t txbuf[(IMAGE_SIZE_PER_PACKET + sizeof(sample_prot_header) + sizeof(sample_mm_ack) + 64)*2];
    size_t msg_len = 0;
    size_t filesize;
    sample_prot_header *pSend = (sample_prot_header *) txbuf;
    FILE *fp;
    size_t ret;
    int val;

    static int32_t total=0;
    static int32_t i = 0;
    static int32_t sent = 0;
    static int32_t to_send = 0;
    static char req = 0;

    sample_mm mm;
    sample_mm_ack mmack;

    if((pHeader->cmd == SAMPLE_CMD_UPLOAD_MM_DATA) && (req == 1))//recv ack
    {
        memcpy(&mmack, pHeader+1, sizeof(mmack));
        NB_DEBUG("mmack:\n");
        printbuf((uint8_t *)&mmack, sizeof(mmack));
        printf("ack = %d\n", mmack.ack);
        mmack.packet_idx = ntohs(mmack.packet_idx);
        printf("packet_idx = %d\n", mmack.packet_idx);
    }
    else if(pHeader->cmd == SAMPLE_CMD_REQ_MM_DATA)//recv req
    {
        i = 0;
        to_send = 0;
        sent = 0;
        req = 1;

        pHeader->cmd == SAMPLE_CMD_UPLOAD_MM_DATA;
        val= system("/system/bin/rbget");//get jpeg
        NB_DEBUG("system val =%d\n", val);

        fp = fopen("/mnt/obb/rb_last_frame.jpg", "r");
        if(fp ==NULL)
        {
            perror("/mnt/obb/rb_last_frame.jpg:");
            return -1;
        }
        fseek(fp, 0, SEEK_SET);
        fseek(fp, 0, SEEK_END);
        filesize = ftell(fp);
        fclose(fp);
        total = (filesize + IMAGE_SIZE_PER_PACKET - 1)/IMAGE_SIZE_PER_PACKET;
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
    fp = fopen("/mnt/obb/rb_last_frame.jpg", "r");
    if(fp ==NULL)
    {
        perror("/mnt/obb/rb_last_frame.jpg:");
        return -1;
    }
    fseek(fp, sent, SEEK_SET);
    to_send = fread(data+ sizeof(mm), 1, IMAGE_SIZE_PER_PACKET, fp);
    if(to_send >0)
    {
        msg_len = sample_assemble_msg(pSend, SAMPLE_CMD_UPLOAD_MM_DATA, data, sizeof(mm) + to_send);
        ret = SendData((uint8_t *)pSend, msg_len);//send

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

    unsigned char *pchar = (unsigned char *)pHeader;

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
            msg_len = sample_assemble_msg(pSend, SAMPLE_CMD_QUERY, NULL, 0);
            SendData(txbuf, msg_len);
            break;

        case SAMPLE_CMD_DEVICE_INFO:
            msg_len = sample_assemble_msg(pSend, SAMPLE_CMD_DEVICE_INFO,
                    (uint8_t*)&dev_info, sizeof(dev_info));
            SendData(txbuf, msg_len);
            break;

        case SAMPLE_CMD_WARNING_REPORT:
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

static int socket_to_host_init(void)
{
#define SERVER_PORT (2017)
    int s = -1;
    int enable = 1;
    int32_t ret = 0;
    //    const char *server_ip = "192.168.2.100";
    const char *server_ip = "0.0.0.0";
    struct sockaddr_in minit_serv_addr;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("Create socket failed %s\n", strerror(errno));
        exit(1);
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    memset(&minit_serv_addr, 0, sizeof(minit_serv_addr));
    minit_serv_addr.sin_family = AF_INET;
    minit_serv_addr.sin_port   = htons(SERVER_PORT);

    //	minit_serv_addr.sin_addr.s_addr = INADDR_ANY;
#if 1
    ret = inet_aton(server_ip, &minit_serv_addr.sin_addr);
    if (0 == ret) {
        printf("inet_aton failed %d %s\n", ret, strerror(errno));
        exit(1);
    }
#endif
    ret = bind(s, (struct sockaddr *) &minit_serv_addr, sizeof(minit_serv_addr));
    if (0 != ret) {
        printf("bind failed %d %s\n", ret, strerror(errno));
        exit(2);
    }

    ret = listen(s, 1);
    if (0 != ret) {
        printf("listen failed %d %s\n", ret, strerror(errno));
        exit(3);
    }
    return s;
}

void *recv_from_host(void *para)
{
    int32_t ret = 0;
    struct sockaddr_in in_sa;
    uint32_t in_sa_len = 0;
    uint8_t readbuf[1024];
    int i;

    int32_t cmd_sock = socket_to_host_init();
    if (cmd_sock < 0) {
        return NULL;
    }
    while (1) {

        printf("waiting client connect..\n");
        hostsock = accept(cmd_sock, (struct sockaddr *) &in_sa, &in_sa_len);
        if (hostsock < 0) {
            printf("accept failed %d %s\n", ret, strerror(errno));
            exit(4);
        }
        printf("connect success!\n");
        while (1) {
            memset(readbuf, 0, sizeof(readbuf));
            ret = read(hostsock, readbuf, sizeof(readbuf));
            if (ret <= 0) {
                printf("read failed %d %s\n", ret, strerror(errno));
                close(hostsock);
                hostsock = -1;
                break;
            }
            else//write to buf
            {
                NB_DEBUG("recv raw cmd:\n");
                printbuf(readbuf, ret);

                i=0;
                while(ret != i)
                {
                    //recv host cmd, push to queue
                    if(!isBufferFull(myBuffer_ptr))
                    {
                        bufferWrite(myBuffer_ptr, readbuf[i++]);
                    }
                    else
                    {
                        usleep(20);
                        printf("cir buf flow\n");
                    }
                }
                //		printf("push over!\n");
            }
        }
        printf("close socket!\n");
        close(hostsock);
    }

    return NULL;
}

void *deal_host_cmd(void *para)
{
    unsigned char ch, crc;
    char start = 0;
    char flag = 0;
    unsigned char readbuf[512];
    sample_prot_header *pHeader = (sample_prot_header *) readbuf;

    int cnt=0;
    int framelen = 0;
    while(1)
    {
        //read
        if(!isBufferEmpty(myBuffer_ptr))
        {
            bufferRead(myBuffer_ptr,ch);
            //	NB_DEBUG("0x%02x\n", ch);

            if(!start && (ch == SAMPLE_PROT_MAGIC) && (cnt == 0))//get head
            {
                readbuf[cnt] = SAMPLE_PROT_MAGIC;
                cnt++;
                start = 1;
                continue;
            }
            else if(start && (ch == SAMPLE_PROT_MAGIC) && (cnt > 0))//get tail
            {
                if(cnt < 6)//maybe error frame, as head, restart
                {
                    cnt = 0;
                    readbuf[cnt] = SAMPLE_PROT_MAGIC;
                    cnt++;
                    start = 1;
                    continue;
                }

                //get tail
                readbuf[cnt] = SAMPLE_PROT_MAGIC;
                start = 0;//over
                framelen = cnt + 1;
                cnt = 0;
                continue;
            }
            else if(!start)//error data
            {
                continue;
            }
            else
            {
                if((ch == SAMPLE_PROT_ESC_CHAR) && !flag)//need deal
                {
                    flag = 1;
                    readbuf[cnt] = ch;
                }
                else if(flag && (ch == 0x02))
                {
                    readbuf[cnt] = SAMPLE_PROT_MAGIC;
                    cnt++;
                    flag = 0;
                }
                else if(flag && (ch == 0x01))
                {
                    readbuf[cnt] = SAMPLE_PROT_ESC_CHAR;
                    cnt++;
                    flag = 0;
                }
                else if(flag && (ch != 0x01) && (ch != 0x02))
                {
                    cnt++;
                    readbuf[cnt] = ch;
                    cnt++;
                    flag = 0;
                }
                else
                {
                    readbuf[cnt] = ch;
                    cnt++;
                }

            }

        }
        else
        {
            usleep(20);
        }

        if(framelen >0)
        {
            NB_DEBUG("get a framelen = %d\n", framelen);
            printbuf(readbuf, framelen);


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



            framelen = 0;

        }

    }
}


