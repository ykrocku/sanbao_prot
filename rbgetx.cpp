// Copyright 2017 minieye.cc
// Author: Zhang Qi <zhangqi@minieye.cc>
// Date: 2017-12-07

#include <stdio.h>
#include <stdint.h>
#include <getopt.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <iostream>
#include <vector>

#include <string.h>
#include <sys/time.h>

#include "common/ringbuf/RingBufFrame.h"
#include "common/ringbuf/CRingBuf.h"
#include "common/time/timestamp.h"

#include "sample.h"
#include "MjpegWriter.h"

#include <cstddef>
#include "common/base/closure.h"
#include "common/concurrency/this_thread.h"
#include "common/concurrency/thread_pool.h"

#define USE_HW_JPEG
#ifdef USE_HW_JPEG
#include "common/hal/android_api.h"
#else
#include <opencv2/opencv.hpp>
#endif

// global variables
static const char* program = "rbget";
static const char* version = "1.1.0";





pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond =PTHREAD_COND_INITIALIZER;

warn_information warn_happen;
para_setting warn_para;



#define ID_SOURCE_CNT   4
typedef struct _mm_slot{
#define SLOT_EMPTY      0
#define SLOT_USED       1

#define SLOT_WRITING    2
#define SLOT_READING    3

char rw_flag;
char is_empty;
char type;
uint32_t id;

} __attribute__((packed)) mm_slot;


mm_slot mm_resource[WARN_TYPE_NUM];




static const char* usage = "Usage: %s [options]\n"
"\n"
"Options:\n"
"  -h, --help                   Print this help message and exit\n"
"  -v, --version                Print version message and exit\n"
"  -Q, --quality NUM            Specify JPEG quality (default 50)\n"
"\n";

void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms - ts.tv_sec * 1000) * 1000000;
    nanosleep(&ts, NULL);
}
void print_frame(const char * name, RBFrame* pFrame)
{
    std::cerr << "TAG="   << pFrame->frameTag
        << ", name=" << name
        << ", TYP=" << pFrame->frameType
        << ", CH="  << pFrame->channel
        << ", IDX=" << pFrame->frameIdx
        << ", NO="  << pFrame->frameNo
        << ", LEN=" << pFrame->dataLen
        << ", PTS=" << pFrame->pts
        << ", TM="  << pFrame->time
        << ", W="   << pFrame->video.VWidth
        << ", H="   << pFrame->video.VHeight
        << std::endl;
}

CRingBuf* init_ringbuf(const char *usrName, int personality) {
    const int RB_SIZE = 100 * 1024 * 1024;
    return new CRingBuf(usrName, "adas_image", RB_SIZE, personality);
}

int write_file(const char* filename, const void* data, size_t size) {
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "Cannot open for write: %s\n", filename);
        return 1;
    }

    size_t nb = fwrite(data, 1, size, fp);
    if (nb != size) {
        fprintf(stderr, "Error: didn't write enough bytes (%zd/%zd)\n", nb, size);
        fclose(fp);
        return 2;
    }

    fclose(fp);
    return 0;
}
int read_local_para(const char* filename) {
    FILE* fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Cannot open for write: %s, so create\n", filename);
        fp = fopen(filename, "w+");
        if (!fp) {
        fprintf(stderr, "create: %s, file\n", filename);
        return 1;
        }
    }
    size_t nb = fread(&warn_para, 1, sizeof(warn_para), fp);
    if (nb != sizeof(warn_para)) {
        fprintf(stderr, "Error: didn't write enough bytes (%zd/%zd)\n", nb, sizeof(warn_para));
        fclose(fp);
        return 2;
    }

    fclose(fp);
    return 0;
}

std::vector<uint8_t> jpeg_encode(uint8_t* data,
        int cols, int rows, int out_cols, int out_rows, int quality) {
    std::vector<uint8_t> jpg_vec(512 * 1024);

#ifdef USE_HW_JPEG
    fprintf(stderr, "Using hardware JPEG encoder.\n");
    int32_t bytes = ma_api_hw_jpeg_encode(data,
            cols, rows, MA_COLOR_FMT_RGB888,
            jpg_vec.data(), out_cols, out_rows, quality);
    jpg_vec.resize(bytes);
#else
    fprintf(stderr, "Using software JPEG encoder.\n");

    // resize
    cv::Mat img;
    cv::Mat big_img = cv::Mat(cv::Size(cols, rows), CV_8UC3, data);
    cv::resize(big_img, img, cv::Size(cols / 2, rows / 2));

    // encode
    std::vector<int> compression_params;
    compression_params.push_back(cv::IMWRITE_JPEG_QUALITY);
    compression_params.push_back(quality);
    cv::imencode(".jpg", img, jpg_vec, compression_params);
#endif

    return jpg_vec;
}

int process(CRingBuf* pRB, CRingBuf* pwRB, int quality, int width, int height) {

    static uint32_t mFrameIdx=0;
    uint32_t jpg_size=0;

    RBFrame* pFrame = nullptr;
    for (int j = 0; j < 10; ++j) {
        pRB->SeekIndexByTime(0);  // seek to the latest frame

        uint32_t frameLen = 0;
        pFrame = reinterpret_cast<RBFrame*>(pRB->RequestReadFrame(&frameLen));
        if (CRB_VALID_ADDRESS(pFrame)) {
            break;
        }
        sleep_ms(20);
    }

    if (!CRB_VALID_ADDRESS(pFrame)) {
        fprintf(stderr, "Error: RequestReadFrame failed\n");
        return -1;
    }

    if (pFrame->data == nullptr ||
            pFrame->video.VWidth == 0 ||
            pFrame->video.VHeight == 0) {
        fprintf(stderr, "Error: image stream exhausted\n");
        pRB->CommitRead();
        //exit(2);
        return -1;
    }

    std::vector<uint8_t> jpg_vec = jpeg_encode(pFrame->data,
            pFrame->video.VWidth, pFrame->video.VHeight, width, height, quality);
    pRB->CommitRead();

    jpg_size = jpg_vec.size();
    RBFrame *pwFrame = NULL;


    pwFrame = (RBFrame *) pwRB->RequestWriteFrame(jpg_size + sizeof(RBFrame), CRB_FRAME_I_SLICE);
    if (!CRB_VALID_ADDRESS(pwFrame)) {
        printf("RequestWriteFrame %d byte failed", jpg_size);
        return -1;
    }

    printf("jpg_size = %d\n", jpg_size);
    pwFrame->frameTag = RBFrameTag;
    pwFrame->frameType = IFrame;
    pwFrame->channel = 20;
    pwFrame->frameIdx = mFrameIdx++;
    pwFrame->frameNo  = pwFrame->frameIdx;
    pwFrame->dataLen = jpg_size;
    pwFrame->video.VWidth = width;
    pwFrame->video.VHeight = height;

    struct timespec t;
    struct timeval tv;
    clock_gettime(CLOCK_MONOTONIC, &t);
    gettimeofday(&tv, NULL);

    pwFrame->time = tv.tv_sec * 1000 + tv.tv_usec/1000;
    pwFrame->pts = t.tv_sec * 1000 + t.tv_nsec/(1000 * 1000);
    memcpy(pwFrame->data, jpg_vec.data(), jpg_size);
    //memcpy(pwFrame->data, info.addr, 2 * 1024 * 1024);
    pwRB->CommitWrite();

    print_frame("producer", pwFrame);

    return 0;

}

static CRingBuf* jpeg_init_ringbuf(const char* bufName,
        const char *usrName, int personality) {
    //const int RB_SIZE = FLAGS_ringbuf_size * 1024 * 1024;
    const int RB_SIZE = 1024* 1024 * 1024;
    return new CRingBuf(usrName, bufName, RB_SIZE, personality);
}

static inline RBFrame*
rb_request_read_frame(CRingBuf* pRB, uint32_t* frameLen) {
    uint8_t* ptr = pRB->RequestReadFrame(frameLen);
    return reinterpret_cast<RBFrame*>(ptr);
}

static inline RBFrame*
rb_request_write_frame(CRingBuf* pRB, uint32_t size) {
    uint8_t* ptr = pRB->RequestWriteFrame(size, CRB_FRAME_I_SLICE);
    return reinterpret_cast<RBFrame*>(ptr);
}


void *pthread_encode_jpeg(void *p)
{
    int timems;
    int quality = 50;
    int Vwidth = 640;
    int Vheight = 360;
    int start_record=1;
    struct timeval ta, tb, record_time;  
    int cnt = 0;

    CRingBuf* pRB = init_ringbuf(program, CRB_PERSONALITY_READER);

    CRingBuf* pwjpg = jpeg_init_ringbuf("adas_jpg", "producer",
            CRB_PERSONALITY_WRITER);
    

    while(1)
    {
        if(start_record)
        {
            cnt = 0;
            start_record = 0;
            gettimeofday(&record_time, NULL);
        }
        if(!send_package_timeout(&record_time, 1))//timeout
        {
            printf("one second encode cnt = %d\n", cnt);
            start_record = 1;
#if 0
            pthread_mutex_lock(&mutex);
            warn_happen.type |= AUTO_TAKE_PHOTO;
            pthread_mutex_unlock(&mutex);
            pthread_cond_signal(&cond);
#endif
        }

        if(process(pRB, pwjpg, quality, Vwidth, Vheight))
            usleep(500000);
    }
};

void set_para_setting_default()
{

    warn_happen.type = SOUND_WARN_NONE;
    memset(warn_happen.id, 0, sizeof(warn_happen.id));
    memset(mm_resource, 0, sizeof(mm_resource));

    warn_para.warning_speed_val = 60;// km/h
    warn_para.warning_volume = 6;
    warn_para.auto_photo_mode = 1;
    warn_para.auto_photo_time_period = 1800; //单位：秒
    warn_para.auto_photo_distance_period = 100; //单位：米
    warn_para.photo_num = 3;
    warn_para.photo_time_period = 2; //单位：100ms
    warn_para.image_Resolution = 1;
    warn_para.video_Resolution = 1;
    memset(&warn_para.reserve[0], 0, sizeof(warn_para.reserve));
    


    warn_para.obstacle_distance_threshold = 30; // 单位：100ms
    warn_para.obstacle_video_time = 5; //单位：秒
    warn_para.obstacle_photo_num = 3;
    warn_para.obstacle_photo_time_period = 2; // 单位：100ms

    warn_para.FLC_time_threshold = 30;
    warn_para.FLC_times_threshold = 5 ;
    warn_para.FLC_video_time = 5;
    warn_para.FLC_photo_num = 3;
    warn_para.FLC_photo_time_period = 2;

    warn_para.LDW_video_time = 5;
    warn_para.LDW_photo_num = 3;
    warn_para.LDW_photo_time_period = 2;


    warn_para.FCW_time_threshold = 30;
    warn_para.FCW_video_time = 5;
    warn_para.FCW_photo_num = 3;
    warn_para.FCW_photo_time_period = 2;


    warn_para.PCW_time_threshold = 30;
    warn_para.PCW_video_time = 5;
    warn_para.PCW_photo_num = 3;
    warn_para.PCW_photo_time_period = 20;

    warn_para.HW_time_threshold = 30; // 单位：100ms 
    warn_para.HW_video_time = 5;
    warn_para.HW_photo_num = 3;
    warn_para.HW_photo_time_period = 2; // 单位：100ms

    warn_para.TSR_photo_num = 3;
    warn_para.TSR_photo_time_period = 10;

}
char *warning_type_to_str(int type)
{
    static char name[20];

    strcpy(name, "default");

    switch(type)
    {
        case SOUND_WARN_NONE:
            return name;
        case SOUND_TYPE_SILENCE:
        case SOUND_TYPE_LLDW:
        case SOUND_TYPE_RLDW:
        case SOUND_TYPE_HW:
        case SOUND_TYPE_TSR:
        case SOUND_TYPE_VB:
        case SOUND_TYPE_FCW_PCW:
            printf("---%s\n", strcpy(name, "headway"));
            return strcpy(name, "headway");

        default:
                return name;
    }

}


int record_mm_resouce(uint32_t type, uint32_t id)
{
    static uint32_t pos = 0;
    int i=0;

    for(i=0; i<WARN_TYPE_NUM; i++)
    {
        printf("--%d--%d--\n",mm_resource[i].is_empty, SLOT_EMPTY);
       if(mm_resource[i].is_empty == SLOT_EMPTY)
           break;
    }
    
    if(i < WARN_TYPE_NUM)//find empty slot, we can write
    {
      mm_resource[i].type = type;  
      mm_resource[i].id = id;  
      printf("find i = %d\n", i);
      return i; //return the slot
    }
    else // no empty slot can be write, 
    {
        #if 0  
        for(i=0; i<WARN_TYPE_NUM; i++)
        if(mm_resource[i].rw_flag == )
        #endif
        return -1;
    }
}
int32_t id_to_free_slot(uint32_t id)
{
    int k=0;

    for(k=0; k<WARN_TYPE_NUM; k++)
    {
        if(mm_resource[k].is_empty == SLOT_USED && mm_resource[k].id == id)
        {
            //mm_resource[k].is_empty = SLOT_EMPTY;
            memset(&mm_resource[k], 0, sizeof(mm_resource[k]));
            return 0;
        }
    }
    return -1;
}

int32_t id_to_warning_type(uint32_t id, uint32_t *type)
{
    int k=0;

    for(k=0; k<WARN_TYPE_NUM; k++)
    {
        printf("empty:%d--id:%d\n", mm_resource[k].is_empty,mm_resource[k].id);

        if(mm_resource[k].is_empty == SLOT_USED && mm_resource[k].id == id)
        {
            *type = mm_resource[k].type;
            return 0;
        }
    }
    return -1;
}

struct record_para{

    int video_time;
    int photo_num;
    int photo_time_period; // 单位：100ms
};

void get_para_val(struct record_para *para, int type)
{
    switch(type)
    {
        case SOUND_TYPE_HW:
            para->video_time = warn_para.HW_video_time;
            para->photo_num = warn_para.HW_photo_num;
            para->photo_time_period = warn_para.HW_photo_time_period; // 单位：100ms
            break;

        default:
            printf("no this warning type!\n");
            break;
    }

}

void record_video(CRingBuf* pjpg, int index)
{
    //avi
    int i=0;
    MjpegWriter mjpeg;
    RBFrame* pFrame = nullptr;
    uint32_t frameLen = 0;
    char filepath[100];
    uint64_t timestart = 0;

    struct record_para para;

    get_para_val(&para, mm_resource[index].type);

    printf("record_video enter!\n");

#if 0
    pjpg->SeekIndexByTime(0);  // seek to the latest frame
    pFrame = reinterpret_cast<RBFrame*>(pjpg->RequestReadFrame(&frameLen));
    if (!CRB_VALID_ADDRESS(pFrame)) {
        fprintf(stderr, "Error: RequestReadFrame failed\n");
    }
    print_frame("seek 0", pFrame);
#endif


    //seek time
    printf("seek time:%d\n", 0-para.video_time);
    pjpg->SeekIndexByTime((0-para.video_time));
    pFrame = reinterpret_cast<RBFrame*>(pjpg->RequestReadFrame(&frameLen));
    if (!CRB_VALID_ADDRESS(pFrame)) {
        fprintf(stderr, "Error: RequestReadFrame failed\n");
    }
    timestart = pFrame->time;
    print_frame("video-0", pFrame);

    pjpg->CommitRead();
    sprintf(filepath, "/mnt/obb/%s-%08x.avi", warning_type_to_str(mm_resource[index].type), mm_resource[index].id);
    //sprintf(filepath, "/mnt/obb/%08x.avi", 1);
    mjpeg.Open(filepath, 15, 720, 360);
    while(1)
    {
        pFrame = reinterpret_cast<RBFrame*>(pjpg->RequestReadFrame(&frameLen));
        if (!CRB_VALID_ADDRESS(pFrame)) {
            fprintf(stderr, "Error: RequestReadFrame failed\n");
            usleep(100000);
            continue;
        }
        mjpeg.Write(pFrame->data, pFrame->dataLen);
        pjpg->CommitRead();
        print_frame("video-read", pFrame);
        if((uint64_t)pFrame->time > para.video_time*2*1000 + timestart)
        {
            break;
        }
    }

    mjpeg.Close();


    mm_resource[index].is_empty = SLOT_USED;
    printf("avi done!\n");
}




int record_snap_shot()
{
    int i=0;
    uint32_t id=0;
    uint32_t type=0;
    RBFrame* pFrame = nullptr;
    uint32_t frameLen = 0;
    char filepath[100];
    static CRingBuf* pjpg = NULL;
    
    if(!pjpg)
        pjpg = jpeg_init_ringbuf("adas_jpg", "customer1",
                CRB_PERSONALITY_READER);

    printf("record_jpeg enter!\n");
    for(i=0; i<warn_para.photo_num; i++)
    {
        pjpg->SeekIndexByTime(0);  // seek to the latest frame
        pFrame = reinterpret_cast<RBFrame*>(pjpg->RequestReadFrame(&frameLen));
        if (!CRB_VALID_ADDRESS(pFrame)) {
            fprintf(stderr, "Error: RequestReadFrame failed\n");
            continue;
        }
            
        id = get_next_warning_id();

       // print_frame("jpeg", pFrame);
        sprintf(filepath, "/mnt/obb/%s-%08x-%d.jpg", "snap_shot", id, i);
     //   sprintf(filepath, "/mnt/obb/%08x-%d.jpg", 1, i);
        fprintf(stdout, "Saving image file...%s\n", filepath);
        int rc = write_file(filepath, pFrame->data, pFrame->dataLen);
        if (rc == 0) {
            printf("Image saved to [%s]\n", filepath);
        } else {
            fprintf(stderr, "Cannot save image to %s\n", filepath);
        }

        if(i+1 != warn_para.photo_num)//last frame don't sleep
            sleep_ms(warn_para.photo_time_period);
    }

    send_snap_shot_event(id);
    return 0;
}





void record_jpeg(CRingBuf* pjpg, int index)
{
    int i=0;
    RBFrame* pFrame = nullptr;
    uint32_t frameLen = 0;
    char filepath[100];
    struct record_para para;

    get_para_val(&para, mm_resource[index].type);


    printf("record_jpeg enter!\n");
    for(i=0; i<para.photo_num; i++)
    {
        pjpg->SeekIndexByTime(0);  // seek to the latest frame
        pFrame = reinterpret_cast<RBFrame*>(pjpg->RequestReadFrame(&frameLen));
        if (!CRB_VALID_ADDRESS(pFrame)) {
            fprintf(stderr, "Error: RequestReadFrame failed\n");
            continue;
        }

       // print_frame("jpeg", pFrame);
        sprintf(filepath, "/mnt/obb/%s-%08x-%d.jpg", warning_type_to_str(mm_resource[index].type), mm_resource[index].id, i);
     //   sprintf(filepath, "/mnt/obb/%08x-%d.jpg", 1, i);
        fprintf(stdout, "Saving image file...%s\n", filepath);
        int rc = write_file(filepath, pFrame->data, pFrame->dataLen);
        if (rc == 0) {
            printf("Image saved to [%s]\n", filepath);
        } else {
            fprintf(stderr, "Cannot save image to %s\n", filepath);
        }

        if(i+1 != para.photo_num)
            sleep_ms(para.photo_time_period);
    }

    mm_resource[index].is_empty = SLOT_USED;
}

void *pthread_sav_warning_jpg(void *p)
{
    int i=0, k=0;
    warn_information warn_local;
    char filepath[100];
    RBFrame* pFrame = nullptr;
    uint32_t frameLen = 0;
    uint32_t type = 0;
    int index = 0;


    set_para_setting_default();

    read_local_para(LOCAL_PRAR_FILE);



    CRingBuf* pvideo = jpeg_init_ringbuf("adas_jpg", "customer1",
            CRB_PERSONALITY_READER);
    CRingBuf* pjpeg = jpeg_init_ringbuf("adas_jpg", "customer2",
            CRB_PERSONALITY_READER);

    ThreadPool pool; // 0 - cpu member
    pool.SetMinThreads(4);

    while(1)
    {
        pthread_mutex_lock(&mutex);
        while(warn_happen.type == SOUND_WARN_NONE){
            pthread_cond_wait(&cond, &mutex);
        }   
        memcpy(&warn_local, &warn_happen, sizeof(warn_local));
        warn_happen.type = SOUND_WARN_NONE;
        pthread_mutex_unlock(&mutex);

#if 1
        printf("auto photo start: 0x%x\n", warn_local.type | AUTO_TAKE_PHOTO);
        if(warn_local.type & AUTO_TAKE_PHOTO)
        {
            printf("auto photo start!\n");
            Closure<void>* cls2 = NewClosure(record_jpeg, pjpeg,  1);
            pool.AddTask(cls2);
        }
#endif

        printf("deal with warning: 0x%x\n", warn_local.type);
        for(k=0; k<WARN_TYPE_NUM; k++)
        {
            type = (1<<k);
            printf("warn_local==0x%x\n", warn_local.type & type);
            if(!(warn_local.type & (1<<k)))//warning not happened
                continue;

            index = record_mm_resouce(type, warn_local.id[k]);
            if(index >= 0)
            {
                printf("Thread pool enter!\n");
                Closure<void>* cls2 = NewClosure(record_jpeg, pjpeg, index);
                pool.AddTask(cls2);
                Closure<void>* cls1 = NewClosure(record_video, pvideo, index);
                pool.AddTask(cls1);
            }
            printf("waitting.....k=%d\n", k);
            sleep(100000);
        }
    }
}


