#include <stdio.h>
#include <stdint.h>
#include <getopt.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <iostream>
#include <vector>
#include <list>

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

using namespace std;

void print_para(para_setting *para);

// global variables
static const char* version = "1.1.0";

static list<mm_node> mmlist;
static pthread_mutex_t mm_resource_lock = PTHREAD_MUTEX_INITIALIZER;

static para_setting warn_para;
static pthread_mutex_t para_lock = PTHREAD_MUTEX_INITIALIZER;
void read_warn_para(para_setting *para)
{
    pthread_mutex_lock(&para_lock);
    memcpy(para, &warn_para, sizeof(warn_para));
    pthread_mutex_unlock(&para_lock);
}
void write_warn_para(para_setting *para)
{
    uint32_t i;
    uint8_t *in8 = NULL;
    uint8_t *out8 = NULL;
    
    in8 = (uint8_t *)&para->warning_speed_val;
    out8 = (uint8_t *)&warn_para.warning_speed_val;

    pthread_mutex_lock(&para_lock);
    if(para->auto_photo_time_period != 0xFFFF)
        warn_para.auto_photo_time_period = para->auto_photo_time_period;
    if(para->auto_photo_distance_period != 0xFFFF)
        warn_para.auto_photo_distance_period = para->auto_photo_distance_period;

    for(i=0; i< sizeof(para_setting); i++)
    {
        if(i==3)
        {
            i+=4;
        }

        if(in8[i] != 0xFF)
        {
            out8[i] = in8[i];
        }
    }

    pthread_mutex_unlock(&para_lock);
}

void display_mm_resource()
{
    list<mm_node>::iterator it;  

    pthread_mutex_lock(&mm_resource_lock);
    for(it=mmlist.begin();it!=mmlist.end();it++)  
    {  
        printf("display list id = %d\n",it->mm_id);
        printf("warn_type = %d, mm_type=%d\n",it->warn_type, it->mm_type);
    }  
    pthread_mutex_unlock(&mm_resource_lock);
}

int32_t find_mm_resource(uint32_t id, mm_node *m)
{
    list<mm_node>::iterator it;  
    int ret = -1;

    pthread_mutex_lock(&mm_resource_lock);
    for(it=mmlist.begin();it!=mmlist.end();it++)  
    {  
      //  printf("find id=%d,list id = %d\n",id, it->mm_id);
      //  printf("warn_type = %d, mm_type=%d\n",it->warn_type, it->mm_type);
        if(it->mm_id == id)
        {
            memcpy(m, &it->rw_flag, sizeof(mm_node));  
            ret = 0;
            break;
        }
    }  
    pthread_mutex_unlock(&mm_resource_lock);
    return ret;
}

int32_t delete_mm_resource(uint32_t id)
{
    list<mm_node>::iterator it;  
    int ret = -1;
    char filepath[100];

    pthread_mutex_lock(&mm_resource_lock);
    for(it=mmlist.begin();it!=mmlist.end();it++)  
    {  
     //   printf("delete id=%d, list id = %d\n",id, it->mm_id);
     //   printf("warn_type = %d, mm_type=%d\n",it->warn_type, it->mm_type);
        if(it->mm_id == id)
        {
            if(it->mm_type == MM_PHOTO)
            {
                sprintf(filepath, SNAP_SHOT_JPEG_PATH);
                sprintf(&filepath[strlen(SNAP_SHOT_JPEG_PATH)],\
                        "%s-%08d.jpg",warning_type_to_str(it->warn_type), id);
                printf("rm jpeg %s\n", filepath);
                remove(filepath);
                it = mmlist.erase(it);  
                ret = 0;
                break;
            }
            if(it->mm_type == MM_VIDEO)
            {
                sprintf(filepath, SNAP_SHOT_JPEG_PATH);
                sprintf(&filepath[strlen(SNAP_SHOT_JPEG_PATH)],\
                        "%s-%08d.avi",warning_type_to_str(it->warn_type), id);
                printf("rm avi %s\n", filepath);
                remove(filepath);
                it = mmlist.erase(it);  
                ret = 0;
                break;
            }

        }
    }  
    pthread_mutex_unlock(&mm_resource_lock);

    return ret;
}

void insert_mm_resouce(mm_node m)
{
    pthread_mutex_lock(&mm_resource_lock);
    mmlist.push_back(m); 
    pthread_mutex_unlock(&mm_resource_lock);
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


int read_local_para_file(const char* filename) {
    para_setting para;
    char cmd[100];

    FILE* fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Cannot open for write: %s, so create\n", filename);
        fp = fopen(filename, "w+");
        if (!fp) {
        fprintf(stderr, "create: %s, file\n", filename);
        return 1;
        }
    }
    size_t nb = fread(&para, 1, sizeof(para), fp);
    if (nb != sizeof(para)) {
        fprintf(stderr, "Error: didn't read enough bytes (%zd/%zd)\n", nb, sizeof(para));
        fclose(fp);
        return 2;
    }
    write_warn_para(&para);

    //set alog detect.flag
    sprintf(cmd, "busybox sed -i 's/^.*--test_speed.*$/--test_speed=%d/' /data/xiao/install/detect.flag",\
            para.warning_speed_val);
    system(cmd);

    print_para(&para);

    fclose(fp);
    return 0;
}
int write_local_para_file(const char* filename) {
    para_setting para;

    read_warn_para(&para);
    print_para(&para);
    write_file(filename, &para, sizeof(para));

    return 0;
}

std::vector<uint8_t> jpeg_encode(uint8_t* data,\
        int cols, int rows, int out_cols, int out_rows, int quality) {
    std::vector<uint8_t> jpg_vec(512 * 1024);

#ifdef USE_HW_JPEG
    fprintf(stderr, "Using hardware JPEG encoder.\n");
    int32_t bytes = ma_api_hw_jpeg_encode(data,\
            cols, rows, MA_COLOR_FMT_RGB888,\
            jpg_vec.data(), out_cols, out_rows, quality);
    jpg_vec.resize(bytes);
#else
    fprintf(stderr, "Using software JPEG encoder.\n");

    // resize
    cv::Mat img;
    cv::Mat big_img = cv::Mat(cv::Size(cols, rows), CV_8UC3, data);
    cv::resize(big_img, img, cv::Size(cols, rows));

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
    uint32_t frameLen = 0;

    pRB->SeekIndexByTime(0);  // seek to the latest frame
    pFrame = reinterpret_cast<RBFrame*>(pRB->RequestReadFrame(&frameLen));
    if (!CRB_VALID_ADDRESS(pFrame)) {
        fprintf(stderr, "process Error: RequestReadFrame failed\n");
        return -1;
    }

    if (pFrame->data == nullptr ||
            pFrame->video.VWidth == 0 ||
            pFrame->video.VHeight == 0) {
        fprintf(stderr, "process Error: image stream exhausted\n");
        pRB->CommitRead();
        //exit(2);
        return -1;
    }

    std::vector<uint8_t> jpg_vec = jpeg_encode(pFrame->data,
            pFrame->video.VWidth, pFrame->video.VHeight, width, height, quality);
    pRB->CommitRead();

    jpg_size = jpg_vec.size();
    RBFrame *pwFrame = NULL;

    //request to write
    pwFrame = (RBFrame *) pwRB->RequestWriteFrame(jpg_size + sizeof(RBFrame), CRB_FRAME_I_SLICE);
    if (!CRB_VALID_ADDRESS(pwFrame)) {
        printf("RequestWriteFrame %d byte failed", jpg_size);
        return -1;
    }

    //printf("jpg_size = %d\n", jpg_size);
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

void *pthread_encode_jpeg(void *p)
{
    int timems;
    int quality = 50;
    int Vwidth = 640;
    int Vheight = 360;
    int start_record=1;
    struct timeval ta, tb, record_time;  
    int cnt = 0;
    mm_header_info mm;

    const int RB_SIZE = 100 * 1024 * 1024;
    const int RB_SIZE2 = 8* 1024 * 1024;
    CRingBuf* pRB = new CRingBuf("encode_jpeg", "adas_image", RB_SIZE, CRB_PERSONALITY_READER);
    CRingBuf* pwjpg = new CRingBuf("producer", "adas_jpg", RB_SIZE2, CRB_PERSONALITY_WRITER);

    while(1)
    {
        if(process(pRB, pwjpg, quality, Vwidth, Vheight))
            usleep(500000);
    }
};
void print_para(para_setting *para)
{
    printf("para->warning_speed_val       = %d\n", para->warning_speed_val);
    printf("para->warning_volume          = %d\n", para->warning_volume);
    printf("para->auto_photo_mode         = %d\n", para->auto_photo_mode);
    printf("para->auto_photo_time_period  = %d\n", (para->auto_photo_time_period));
    printf("para->auto_photo_distance_peri= %d\n", (para->auto_photo_distance_period));
    printf("para->photo_num               = %d\n", para->photo_num);
    printf("para->photo_time_period       = %d\n", para->photo_time_period);
    printf("para->image_Resolution        = %d\n", para->image_Resolution);
    printf("para->video_Resolution        = %d\n", para->video_Resolution);
    //printf("para->reserve[9]);            = %d\n", para->reserve[9]);
    printbuf(para->reserve, 9);
    printf("para->obstacle_distance_thresh= %d\n", para->obstacle_distance_threshold);
    printf("para->obstacle_video_time     = %d\n", para->obstacle_video_time);
    printf("para->obstacle_photo_num      = %d\n", para->obstacle_photo_num);
    printf("para->obstacle_photo_time_peri= %d\n", para->obstacle_photo_time_period);
    printf("para->FLC_time_threshold      = %d\n", para->FLC_time_threshold);
    printf("para->FLC_times_threshold     = %d\n", para->FLC_times_threshold);
    printf("para->FLC_video_time          = %d\n", para->FLC_video_time);
    printf("para->FLC_photo_num           = %d\n", para->FLC_photo_num);
    printf("para->FLC_photo_time_period   = %d\n", para->FLC_photo_time_period);
    printf("para->LDW_video_time          = %d\n", para->LDW_video_time);
    printf("para->LDW_photo_num           = %d\n", para->LDW_photo_num);
    printf("para->LDW_photo_time_period   = %d\n", para->LDW_photo_time_period);
    printf("para->FCW_time_threshold      = %d\n", para->FCW_time_threshold);
    printf("para->FCW_video_time          = %d\n", para->FCW_video_time);
    printf("para->FCW_photo_num           = %d\n", para->FCW_photo_num);
    printf("para->FCW_photo_time_period   = %d\n", para->FCW_photo_time_period);
    printf("para->PCW_time_threshold      = %d\n", para->PCW_time_threshold);
    printf("para->PCW_video_time          = %d\n", para->PCW_video_time);
    printf("para->PCW_photo_num           = %d\n", para->PCW_photo_num);
    printf("para->PCW_photo_time_period   = %d\n", para->PCW_photo_time_period);
    printf("para->HW_time_threshold       = %d\n", para->HW_time_threshold);
    printf("para->HW_video_time           = %d\n", para->HW_video_time);
    printf("para->HW_photo_num            = %d\n", para->HW_photo_num);
    printf("para->HW_photo_time_period    = %d\n", para->HW_photo_time_period);
    printf("para->TSR_photo_num           = %d\n", para->TSR_photo_num);
    printf("para->TSR_photo_time_period   = %d\n", para->TSR_photo_time_period);
}



void set_para_setting_default()
{
    para_setting para;

    para.warning_speed_val = 30;// km/h, 0-60
    para.warning_volume = 6;//0~8
    para.auto_photo_mode = 0;//主动拍照默认关闭
    para.auto_photo_time_period = 1800; //单位：秒, 0~3600
    para.auto_photo_distance_period = 100; //单位：米, 0-60000
    para.photo_num = 3;//1-10
    para.photo_time_period = 2; //单位：100ms, 1-5
    para.image_Resolution = 1;
    para.video_Resolution = 1;
    memset(&para.reserve[0], 0, sizeof(para.reserve));

    para.obstacle_distance_threshold = 30; // 单位：100ms
    para.obstacle_video_time = 5; //单位：秒
    para.obstacle_photo_num = 3;
    para.obstacle_photo_time_period = 2; // 单位：100ms

    para.FLC_time_threshold = 60;
    para.FLC_times_threshold = 5 ;
    para.FLC_video_time = 5;
    para.FLC_photo_num = 3;
    para.FLC_photo_time_period = 2;

    para.LDW_video_time = 5;
    para.LDW_photo_num = 3;
    para.LDW_photo_time_period = 2;


    para.FCW_time_threshold = 27;
    para.FCW_video_time = 5;
    para.FCW_photo_num = 3;
    para.FCW_photo_time_period = 2;


    para.PCW_time_threshold = 30;
    para.PCW_video_time = 5;
    para.PCW_photo_num = 3;
    para.PCW_photo_time_period = 2;

    para.HW_time_threshold = 30; // 单位：100ms 
    para.HW_video_time = 5;
    para.HW_photo_num = 3;
    para.HW_photo_time_period = 2; // 单位：100ms

    para.TSR_photo_num = 3;
    para.TSR_photo_time_period = 2;

    memset(&para.reserve2[0], 0, sizeof(para.reserve2));

    write_warn_para(&para);
}
char *warning_type_to_str(uint8_t type)
{
    static char name[20];

    strcpy(name, "default");

    switch(type)
    {
        case SW_TYPE_FCW:
            return strcpy(name, "FCW");
        case SW_TYPE_LDW:
            return strcpy(name, "LDW");
        case SW_TYPE_HW:
            return strcpy(name, "HW");
        case SW_TYPE_PCW:
            return strcpy(name, "PCW");
        case SW_TYPE_FLC:
            return strcpy(name, "FLC");
        case SW_TYPE_TSRW:
            return strcpy(name, "TSRW");
        case SW_TYPE_TSR:
            return strcpy(name, "TSR");
        case SW_TYPE_SNAP:
            return strcpy(name, "SNAP");

       // case SW_TYPE_TIMER_SNAP:
         //   return strcpy(name, "TIMER_SNAP");
        default:
                return name;
    }

}

void record_jpeg(CRingBuf* pjpg, mm_header_info info)
{
    int i=0;
    RBFrame* pFrame = nullptr;
    uint32_t frameLen = 0;
    char filepath[100];
    mm_header_info mm;
    MjpegWriter mjpeg;
    mm_node node;
    uint64_t timestart = 0;
    struct timeval record_time;  
    
    printf("--pthread run---- enter!\n");
    memcpy(&mm, &info, sizeof(mm));
    printf("mm.mm_type = %d \n", mm.mm_type);
    if(mm.mm_type == MM_PHOTO)
    {
        printf("record_jpeg enter!\n");
        for(i=0; i<mm.photo_num; i++)
        {
            pjpg->SeekIndexByTime(0);  // seek to the latest frame
            pFrame = reinterpret_cast<RBFrame*>(pjpg->RequestReadFrame(&frameLen));
            if (!CRB_VALID_ADDRESS(pFrame)) {
                fprintf(stderr, "Error: RequestReadFrame failed\n");
                continue;
            }
            if (pFrame->data == nullptr ||
                    pFrame->video.VWidth == 0 ||
                    pFrame->video.VHeight == 0) {
                fprintf(stderr, "Error:: image stream exhausted\n");
                pjpg->CommitRead();
                continue;
            }

            print_frame("jpeg", pFrame);

            sprintf(filepath, SNAP_SHOT_JPEG_PATH);
            sprintf(&filepath[strlen(SNAP_SHOT_JPEG_PATH)],\
                    "%s-%08d.jpg", warning_type_to_str(mm.warn_type), mm.mm_id[i]);

            fprintf(stdout, "Saving image file...%s\n", filepath);
            int rc = write_file(filepath, pFrame->data, pFrame->dataLen);
            if (rc == 0) {
                printf("Image saved to [%s]\n", filepath);
            } else {
                fprintf(stderr, "Cannot save image to %s\n", filepath);
            }
           // pjpg->CommitRead();
         
            node.warn_type = mm.warn_type;
            node.mm_type = MM_PHOTO;
            node.mm_id = mm.mm_id[i];
            //uint8_t time[6];
            insert_mm_resouce(node);
            //display_mm_resource();

            if(i+1 != mm.photo_num)
                usleep(100*mm.photo_time_period*1000);
        }
    }
    else if(mm.mm_type == MM_VIDEO)
    {
        printf("record_video enter!\n");
        //seek time
        printf("seek time:%d\n", 0-mm.video_time);
        //pjpg->SeekIndexByTime((0-2));
        pjpg->SeekIndexByTime((0-mm.video_time));
        pFrame = reinterpret_cast<RBFrame*>(pjpg->RequestReadFrame(&frameLen));
        if (!CRB_VALID_ADDRESS(pFrame)) {
            fprintf(stderr, "Error: RequestReadFrame failed\n");
        }
        timestart = pFrame->time;
       // print_frame("video-0", pFrame);
        pjpg->CommitRead();

        sprintf(filepath, SNAP_SHOT_JPEG_PATH);
        sprintf(&filepath[strlen(SNAP_SHOT_JPEG_PATH)],\
                "%s-%08d.avi",warning_type_to_str(mm.warn_type), mm.mm_id[0]);
        mjpeg.Open(filepath, 15, 720, 360);
        fprintf(stdout, "Saving image file...%s\n", filepath);

        gettimeofday(&record_time, NULL);
        while(1)
        {
            if(!is_timeout(&record_time, mm.video_time+1))//timeout
            {
                printf("avi timeout break\n");
                break;
            }
            pFrame = reinterpret_cast<RBFrame*>(pjpg->RequestReadFrame(&frameLen));
            if (!CRB_VALID_ADDRESS(pFrame)) {
                fprintf(stderr, "Error: RequestReadFrame failed\n");
                usleep(200000);
                continue;
            }
            print_frame("video-read", pFrame);
            mjpeg.Write(pFrame->data, pFrame->dataLen);
            if((uint64_t)pFrame->time > mm.video_time*2*1000 + timestart)
            {
                printf("avi break\n");
                pjpg->CommitRead();
                break;
            }
            pjpg->CommitRead();
        }

        mjpeg.Close();
        printf("%s avi done!\n", warning_type_to_str(mm.warn_type));

        node.warn_type = mm.warn_type;
        node.mm_type = MM_VIDEO;
        node.mm_id = mm.mm_id[0];
        //uint8_t time[6];
        insert_mm_resouce(node);
        //display_mm_resource();

    }
}

void global_var_init()
{
    set_para_setting_default();
    read_local_para_file(LOCAL_PRAR_FILE);
    
    system(DO_DELETE_SNAP_SHOT_FILES);
    printf("do %s\n", DO_DELETE_SNAP_SHOT_FILES);
}

void *pthread_sav_warning_jpg(void *p)
{
    int i = 0;
    int index = 0;
    mm_header_info mm;
    CRingBuf* pr[WARN_TYPE_NUM];
    Closure<void>* cls[WARN_TYPE_NUM];
    char user_name[WARN_TYPE_NUM][20]={
    "customer1","customer2","customer3","customer4",
    "customer5","customer6","customer7","customer8",
#if 0
    "customer9","customer10","customer11","customer12",
    "customer13","customer14", "customer15","customer16",
#endif
    };
    const int RB_SIZE2 = 8* 1024 * 1024;

    for(i=0; i<WARN_TYPE_NUM; i++)
    {
        printf("name:%s\n", user_name[i]);
        pr[i] = new CRingBuf(user_name[i], "adas_jpg", RB_SIZE2, CRB_PERSONALITY_READER);
    }

    ThreadPool pool; // 0 - cpu member
    pool.SetMinThreads(4);

    while(1)
    {
        if(pull_mm_queue(&mm))
        {
            usleep(10000);
            continue;
        }
#if 0
        printf("warn type: 0x%x, period0x%x\n", mm.warn_type, mm.photo_time_period);
        printf("mm type: 0x%x, mmid: 0x%x\n", mm.mm_type, mm.mm_id[0]);
        printf("video_time: 0x%x, num: 0x%x\n", mm.video_time, mm.photo_num);
#endif

        switch(mm.warn_type)
        {
            case SW_TYPE_FCW:
                i=0;
                break;
            case SW_TYPE_LDW:
                i=1;
                break;
            case SW_TYPE_HW:
                i=2;
                break;
            case SW_TYPE_PCW:
                i=3;
                break;
            case SW_TYPE_FLC:
                i=4;
                break;
            case SW_TYPE_TSRW:
                i=5;
                break;
            case SW_TYPE_TSR:
                i=6;
                break;
            case SW_TYPE_SNAP:
                i=7;
                break;
            default:
                break;

        }
           // printf("Thread pool enter! i = %d\n", i);
            cls[i] = NewClosure(record_jpeg, pr[i], mm);
            pool.AddTask(cls[i]);
    }

    return NULL;
}


