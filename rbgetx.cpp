#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
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



#define  ADAS_JPEG_SIZE (4* 1024 * 1024)
#define  ADAS_IMAGE_SIZE (100* 1024 * 1024)


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

    warn_para_check(&warn_para);

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
                sprintf(filepath, "%s%s-%08d.jpg",SNAP_SHOT_JPEG_PATH,\
                        warning_type_to_str(it->warn_type), id);
                printf("rm jpeg %s\n", filepath);
                remove(filepath);
                it = mmlist.erase(it);  
                ret = 0;
                break;
            }
            if(it->mm_type == MM_VIDEO)
            {
                sprintf(filepath,"%s%s-%08d.avi",SNAP_SHOT_JPEG_PATH,\
                        warning_type_to_str(it->warn_type), id);
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
    char cmd[512];

    FILE* fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Cannot open for write: %s, so create\n", filename);
        fp = fopen(filename, "w+");
        if (!fp) {
            fprintf(stderr, "create: %s, file\n", filename);
            return 1;
        }
        fclose(fp);

        //no para file ,so set default to file
        set_para_setting_default();
        write_local_para_file(filename);
    }
    size_t nb = fread(&para, 1, sizeof(para), fp);
    if (nb != sizeof(para)) {
        fprintf(stderr, "Error: didn't read enough bytes (%zd/%zd)\n", nb, sizeof(para));
        fclose(fp);
        return 2;
    }
    write_warn_para(&para);

#if 0
    //set alog detect.flag
    sprintf(cmd, "busybox sed -i 's/^.*--test_speed.*$/--test_speed=%d/' /data/xiao/install/detect.flag",\
            para.warning_speed_val);
#endif

    //set alog detect.flag
    sprintf(cmd, "busybox sed -i 's/^.*--output_lane_info_speed_thresh.*$/--output_lane_info_speed_thresh=%d/' /data/xiao/install/detect.flag",\
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

    CRingBuf* pRB = new CRingBuf("encode_jpeg", "adas_image", ADAS_IMAGE_SIZE, CRB_PERSONALITY_READER);
    CRingBuf* pwjpg = new CRingBuf("producer", "adas_jpg", ADAS_JPEG_SIZE, CRB_PERSONALITY_WRITER);

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

//if para error, set default val
void warn_para_check(para_setting *p)
{
    para_setting *para = (para_setting *)p;
    
    if(para->warning_speed_val < 0 || para->warning_speed_val > 60)
    {
        para->warning_speed_val = 30;// km/h, 0-60
        printf("warn speed valid, set to default!\n");
    }
    if(para->warning_volume < 0 || para->warning_volume > 8)
    {
        para->warning_volume = 6;//0~8
        printf("warn volume valid, set to default!\n");
    }

    //initiative
    if(para->auto_photo_mode < 0 || para->auto_photo_mode > 3)
    {
        para->auto_photo_mode = 0;//主动拍照默认关闭
        printf("warn photo para valid, set to default!\n");
    }
    if(para->auto_photo_time_period <0 || para->auto_photo_time_period > 3600)
    {
        printf("warn para valid, set to default!\n");
        para->auto_photo_time_period = 1800; //单位：秒, 0~3600
    }
    if(para->auto_photo_distance_period < 0 || para->auto_photo_distance_period > 60000)
    {
        printf("warn para valid, set to default!\n");
        para->auto_photo_distance_period = 100; //单位：米, 0-60000
    }

    //photo
    if(para->photo_num < 1 || para->photo_num > 10)
    {
        printf("warn photo valid, set to default!\n");
        para->photo_num = 3;//1-10
    }
    if(para->photo_time_period < 1 || para->photo_time_period > 5)
    {
        printf("warn photo valid, set to default!\n");
        para->photo_time_period = 2; //单位：100ms, 1-5
    }
    if(para->image_Resolution < 1 || para->image_Resolution > 6)
    {
        printf("warn photo valid, set to default!\n");
        para->image_Resolution = 1;
    }
    if(para->video_Resolution < 1 || para->video_Resolution > 7)
    {
        printf("warn photo valid, set to default!\n");
        para->video_Resolution = 1;
    }

    //obstacle
    if(para->obstacle_distance_threshold < 10 || para->obstacle_distance_threshold > 50)
    {
        printf("warn obstacle valid, set to default!\n");
        para->obstacle_distance_threshold = 30; // 单位：100ms
    }
    if(para->obstacle_video_time < 0 || para->obstacle_video_time > 60)
    {
        printf("warn obstacle valid, set to default!\n");
        para->obstacle_video_time = 5; //单位：秒
    }
    if(para->obstacle_photo_num < 0 || para->obstacle_photo_num > 10)
    {
        printf("warn obstacle valid, set to default!\n");
        para->obstacle_photo_num = 3;
    }
    if(para->obstacle_photo_time_period < 1 || para->obstacle_photo_time_period > 10)
    {
        printf("warn obstacle valid, set to default!\n");
        para->obstacle_photo_time_period = 2; // 单位：100ms
    }

    //FLC
    if(para->FLC_time_threshold < 30 || para->FLC_time_threshold >120)
    {
        printf("warn FLC valid, set to default!\n");
        para->FLC_time_threshold = 60;
    }
    if(para->FLC_times_threshold < 3 || para->FLC_times_threshold >10)
    {
        printf("warn FLC valid, set to default!\n");
        para->FLC_times_threshold = 5 ;
    }
    if(para->FLC_video_time < 0 || para->FLC_video_time > 60)
    {
        printf("warn FLC valid, set to default!\n");
        para->FLC_video_time = 5;
    }
    if(para->FLC_photo_num < 0 || para->FLC_photo_num >10)
    {
        printf("warn FLC valid, set to default!\n");
        para->FLC_photo_num = 3;
    }
    if(para->FLC_photo_time_period < 1 || para->FLC_photo_time_period > 10)
    {
        printf("warn FLC valid, set to default!\n");
        para->FLC_photo_time_period = 2;
    }

    //LDW
    if(para->LDW_video_time < 0 || para->LDW_video_time > 60)
    {
        printf("warn LDW valid, set to default!\n");
        para->LDW_video_time = 5;
    }
    if(para->LDW_photo_num < 0 || para->LDW_photo_num > 10)
    {
        printf("warn LDW valid, set to default!\n");
        para->LDW_photo_num = 3;
    }
    if(para->LDW_photo_time_period < 1 || para->LDW_photo_time_period > 10)
    {
        printf("warn LDW valid, set to default!\n");
        para->LDW_photo_time_period = 2;
    }

    //FCW
    if(para->FCW_time_threshold < 10 || para->FCW_time_threshold > 50)
    {
        printf("warn FCW valid, set to default!\n");
        para->FCW_time_threshold = 27;
    }
    if(para->FCW_video_time < 0 || para->FCW_video_time > 60)
    {
        printf("warn FCW valid, set to default!\n");
        para->FCW_video_time = 5;
    }
    if(para->FCW_photo_num < 0 || para->FCW_photo_num > 10)
    {
        printf("warn FCW valid, set to default!\n");
        para->FCW_photo_num = 3;
    }
    if(para->FCW_photo_time_period < 1 || para->FCW_photo_time_period > 10)
    {
        printf("warn FCW valid, set to default!\n");
        para->FCW_photo_time_period = 2;
    }

    //PCW
    if(para->PCW_time_threshold < 0 || para->PCW_time_threshold > 50)
    {
        printf("warn PCW valid, set to default!\n");
        para->PCW_time_threshold = 30;
    }
    if(para->PCW_video_time < 10 || para->PCW_video_time > 60)
    {
        printf("warn pcw valid, set to default!\n");
        para->PCW_video_time = 5;
    }
    if(para->PCW_photo_num < 0 || para->PCW_photo_num > 10)
    {
        printf("warn PCW valid, set to default!\n");
        para->PCW_photo_num = 3;
    }
    if(para->PCW_photo_time_period < 1 || para->PCW_photo_time_period > 10)
    {
        printf("warn PCW valid, set to default!\n");
        para->PCW_photo_time_period = 2;
    }

    //HMW
    if(para->HW_time_threshold < 0 ||  para->HW_time_threshold > 50)
    {
        printf("warn HMW valid, set to default!\n");
        para->HW_time_threshold = 30; // 单位：100ms 
    }
    if(para->HW_video_time < 0 || para->HW_video_time > 60)
    {
        printf("warn HMW valid, set to default!\n");
        para->HW_video_time = 5;
    }
    if(para->HW_photo_num < 0 || para->HW_photo_num > 10)
    {
        printf("warn HMW valid, set to default!\n");
        para->HW_photo_num = 3;
    }
    if(para->HW_photo_time_period < 1 || para->HW_photo_time_period > 10)
    {
        printf("warn HMW valid, set to default!\n");
        para->HW_photo_time_period = 2; // 单位：100ms
    }

    //TSR
    if(para->TSR_photo_num < 0 || para->TSR_photo_num > 10)
    {
        printf("warn TSR valid, set to default!\n");
        para->TSR_photo_num = 3;
    }
    if(para->TSR_photo_time_period < 1 || para->TSR_photo_time_period > 10)
    {
        printf("warn TSR valid, set to default!\n");
        para->TSR_photo_time_period = 2;
    }
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
    static char name[20];

    strcpy(name, "default");

    switch(type)
    {
        case SW_TYPE_FCW:
            return strcpy(name, FCW_NAME);
        case SW_TYPE_LDW:
            return strcpy(name, LDW_NAME);
        case SW_TYPE_HW:
            return strcpy(name, HW_NAME);
        case SW_TYPE_PCW:
            return strcpy(name, PCW_NAME);
        case SW_TYPE_FLC:
            return strcpy(name, FLC_NAME);
        case SW_TYPE_TSRW:
            return strcpy(name, TSRW_NAME);
        case SW_TYPE_TSR:
            return strcpy(name, TSR_NAME);
        case SW_TYPE_SNAP:
            return strcpy(name, SNAP_NAME);

            // case SW_TYPE_TIMER_SNAP:
            //   return strcpy(name, "TIMER_SNAP");
        default:
            return name;
    }
}

int str_to_warning_type(char *type, uint8_t *val)
{

    if(!strncmp(FCW_NAME, type, sizeof(FCW_NAME)))
    {
        *val = SW_TYPE_FCW;
    }
    else if(!strncmp(LDW_NAME, type, sizeof(LDW_NAME)))
    {
        *val = SW_TYPE_LDW;
    }
    else if(!strncmp(HW_NAME, type, sizeof(HW_NAME)))
    {
        *val = SW_TYPE_HW;
    }
    else if(!strncmp(PCW_NAME, type, sizeof(PCW_NAME)))
    {
        *val = SW_TYPE_PCW;
    }
    else if(!strncmp(FLC_NAME, type, sizeof(FLC_NAME)))
    {
        *val = SW_TYPE_FLC;
    }
    else if(!strncmp(TSRW_NAME, type, sizeof(TSRW_NAME)))
    {
        *val = SW_TYPE_TSRW;
    }
    else if(!strncmp(TSR_NAME, type, sizeof(TSR_NAME)))
    {
        *val = SW_TYPE_TSR;
    }
    else if(!strncmp(SNAP_NAME, type, sizeof(SNAP_NAME)))
    {
        *val = SW_TYPE_SNAP;
    }
    else
    {
        printf("unknow warn type: %s\n", type);
        return -1;   
    }

    return 0;
}

void store_one_jpeg(mm_header_info *mm, RBFrame* pFrame, int index)
{
    char filepath[100];
    mm_node node;

    sprintf(filepath,"%s%s-%08d.jpg",SNAP_SHOT_JPEG_PATH,\
            warning_type_to_str(mm->warn_type), mm->photo_id[index]);

    fprintf(stdout, "Saving image file...%s\n", filepath);
    int rc = write_file(filepath, pFrame->data, pFrame->dataLen);
    if (rc == 0) {
        printf("Image saved to [%s]\n", filepath);
    } else {
        fprintf(stderr, "Cannot save image to %s\n", filepath);
    }

    node.warn_type = mm->warn_type;
    node.mm_type = MM_PHOTO;
    node.mm_id = mm->photo_id[index];
    //uint8_t time[6];
    insert_mm_resouce(node);
}

RBFrame* request_jpeg_frame(CRingBuf* pRB, uint32_t repeat_times)
{
    RBFrame* pFrame = nullptr;
    uint32_t frameLen = 0;
    uint32_t try_times = 0;

    do{
        pFrame = reinterpret_cast<RBFrame*>(pRB->RequestReadFrame(&frameLen));
        if (!CRB_VALID_ADDRESS(pFrame)) {
            fprintf(stderr, "Error: RequestReadFrame failed\n");
            usleep(10000);
            pFrame = nullptr;
        }
        else
        {
            break;
        }
    }while(try_times++ < repeat_times);

    return pFrame;
}


void store_warn_jpeg(CRingBuf* pRB, mm_header_info *mm)
{
    RBFrame* pFrame = nullptr;
    int jpeg_index = 0;
    uint64_t jpeg_timestart = 0;
    struct timeval record_time;
    int force_exit_time;


    printf("%s enter!\n", __FUNCTION__);
    force_exit_time = mm->photo_time_period*100*mm->photo_num;
    gettimeofday(&record_time, NULL);
    while(jpeg_index < mm->photo_num)
    {
        if(!is_timeout(&record_time, force_exit_time))//timeout
        {
            printf("store jpeg force exit!\n");
            break;
        }

        pRB->SeekIndexByTime(0);
        pFrame = request_jpeg_frame(pRB, 0);
        if(pFrame == nullptr)
        {
            usleep(25000);
            continue;
        }
        print_frame("video-read", pFrame);

        if(jpeg_index == 0 || (uint64_t)pFrame->time > mm->photo_time_period*100 + jpeg_timestart)//record first jpeg
        {
            print_frame("jpeg", pFrame);
            jpeg_timestart = pFrame->time;
            store_one_jpeg(mm, pFrame, jpeg_index++);
        }

        pRB->CommitRead();
    }
}

#define RECORD_JPEG_NEED 1
#define RECORD_JPEG_NO_NEED 0

void store_one_avi(CRingBuf* pRB, mm_header_info *mm, int jpeg_flag)
{
#define VIDEO_FRAMES_PER_SECOND   15
    RBFrame* pFrame = nullptr;
    char avifilepath[100];
    MjpegWriter mjpeg;
    mm_node node;
    struct timeval record_time;  
    int jpeg_index = 0;
    uint64_t jpeg_timestart = 0;
    int force_exit_time;

    printf("%s enter!\n", __FUNCTION__);

    pRB->SeekIndexByTime(0);
    pFrame = request_jpeg_frame(pRB, 10);
    if(pFrame == nullptr)
        return;

    if(jpeg_flag)
    {

        print_frame("curtent-read", pFrame);
        jpeg_timestart = pFrame->time;
        print_frame("jpeg", pFrame);
        store_one_jpeg(mm, pFrame, jpeg_index++);
    }

    sprintf(avifilepath,"%s%s-%08d.avi", SNAP_SHOT_JPEG_PATH,\
            warning_type_to_str(mm->warn_type), mm->video_id[0]);

    printf("seek time:%d\n", 0-mm->video_time);
    pRB->SeekIndexByTime((0-mm->video_time));

    mjpeg.Open(avifilepath, VIDEO_FRAMES_PER_SECOND, pFrame->video.VWidth, pFrame->video.VHeight);
    fprintf(stdout, "Saving image file...%s\n", avifilepath);

    force_exit_time = mm->video_time*1000;
    gettimeofday(&record_time, NULL);
    while(1)
    {
        if(!is_timeout(&record_time, force_exit_time))
        {
            printf("avi timeout break\n");
            break;
        }

        pFrame = request_jpeg_frame(pRB, 0);
        if(pFrame == nullptr)
        {
            usleep(20000);
            continue;
        }
        print_frame("video-read", pFrame);

        mjpeg.Write(pFrame->data, pFrame->dataLen);

        if(jpeg_flag)
        {
            if(jpeg_index < mm->photo_num && ((uint64_t)pFrame->time > mm->photo_time_period*100 + jpeg_timestart))//record first jpeg
            {
                print_frame("jpeg", pFrame);
                jpeg_timestart = pFrame->time;
                store_one_jpeg(mm, pFrame, jpeg_index++);
            }
        }
        pRB->CommitRead();
    }
    mjpeg.Close();
    printf("%s avi done!\n", warning_type_to_str(mm->warn_type));

    node.warn_type = mm->warn_type;
    node.mm_type = MM_VIDEO;
    node.mm_id = mm->video_id[0];
    //uint8_t time[6];
    insert_mm_resouce(node);
    //display_mm_resource();
}

//修改为同时获取jpg 和 avi
void record_jpeg(CRingBuf* pRB, mm_header_info info)
{
    int i=0;
    RBFrame* pFrame = nullptr;
    char avifilepath[100];
    mm_header_info mm;
    MjpegWriter mjpeg;
    mm_node node;
    uint64_t timestart = 0;
    struct timeval record_time;  
    int cnt = 1;

    memcpy(&mm, &info, sizeof(mm));

    if(mm.photo_enable && !mm.video_enable)
    {
        store_warn_jpeg(pRB, &mm);
    }
    else if(!mm.photo_enable && mm.video_enable)
    {
        store_one_avi(pRB, &mm, RECORD_JPEG_NO_NEED);
    }
    else if(mm.photo_enable && mm.video_enable)
    {
        store_one_avi(pRB, &mm, RECORD_JPEG_NEED);
    }
    else //do nothing
    {
        ;
    }
}

int get_mm_type(char *file_type, uint8_t *val)
{

    if(!strncmp("jpg", file_type, strlen("jpg")))
    {
        *val = MM_PHOTO;
    }
    else if(!strncmp("wav", file_type, strlen("wav")))
    {
        *val = MM_AUDIO;
    }
    else if(!strncmp("avi", file_type, strlen("avi")))
    {
        *val = MM_VIDEO;
    }
    else
    {
        printf("unknow mm file type: %s\n", file_type);
        return -1;
    }

    return 0;
}

void parse_filename(char *filename)
{
    char warn_name[32];
    char mm_id[32];
    char file_type[32];
    mm_node node;
    uint32_t i=0, j=0;
    int ret = 0;
    char *pos = &warn_name[0];

    memset(warn_name, 0, sizeof(warn_name));
    memset(mm_id, 0, sizeof(mm_id));
    memset(file_type, 0, sizeof(file_type));
    for(i=0; i<strlen(filename); i++)
    {
        if(filename[i] == '-')
        {
            j=0;
            pos = &mm_id[0];
            continue;
        }
        else if(filename[i] == '.')
        {
            j=0;
            pos = &file_type[0];
            continue;
        }
        pos[j++] = filename[i];
    }

    ret = str_to_warning_type(warn_name, &node.warn_type);
    if(ret < 0)
        return;

    ret = get_mm_type(file_type, &node.mm_type);
    if(ret < 0)
        return;

    node.mm_id = strtol(mm_id, NULL, 10);

    printf("warn type = %d\n", node.warn_type);
    printf("mmid = %d\n", node.mm_id);
    printf("mm type = %d\n", node.mm_type);

    insert_mm_resouce(node);

}

int traverse_directory(DIR *dirp)
{
    struct dirent *dir_info = NULL;

    do{
        dir_info = readdir(dirp);
        if(!dir_info) //end or error
        {
            return 1;
        }
        else
        {
            printf("---------------------\n");
            printf("dir_info->d_ino = %d\n", dir_info->d_type);
            printf("dir_info->d_name = %s\n", dir_info->d_name);

            if(!strncmp(".", dir_info->d_name, 1))
            {
                printf("ignore .\n");
            }
            else if(!strncmp("..", dir_info->d_name, 2))
            {
                printf("ignore ..\n");
            }
            else
            {
                parse_filename(dir_info->d_name);
            }

        }
    }while(dir_info);

    return 0;
}

int read_local_file_to_list()
{
    DIR *pdir;

    pdir = opendir(SNAP_SHOT_JPEG_PATH);
    if(!pdir)
    {
        perror("error");
        return -1;
    }

    traverse_directory(pdir);

    closedir(pdir);

    return 0;
}

void global_var_init()
{
    read_local_para_file(LOCAL_PRAR_FILE);


    //    system(DO_DELETE_SNAP_SHOT_FILES);
    //    printf("do %s\n", DO_DELETE_SNAP_SHOT_FILES);


    read_local_file_to_list();
}

void *pthread_sav_warning_jpg(void *p)
{
#define CUSTOMER_NUM   8 
    int cnt = 0;
    int i = 0;
    int index = 0;
    mm_header_info mm;
    struct timeval time_begin[WARN_TYPE_NUM];
    char first_record_time[WARN_TYPE_NUM] = {1, 1, 1, 1, 1, 1, 1, 1};
    CRingBuf* pr[WARN_TYPE_NUM];
    Closure<void>* cls[WARN_TYPE_NUM];
    // char user_name[WARN_TYPE_NUM][20]={
    char user_name[CUSTOMER_NUM][20]={
        "customer_FCW_avi","customer_LDW_avi","customer_HW_avi","customer_PCW_avi",
        "customer_FLC_avi","customer_TSRW_avi","customer_TSR_avi","customer_SNAP_avi",
#if 0
        "customer_FCW_jpg","customer_LDW_jpg","customer_HW_jpg","customer_PCW_jpg",
        "customer_FLC_jpg","customer_TSRW_jpg","customer_TSR_jpg","customer_SNAP_jpg",
#endif
    };

    //for(i=0; i<WARN_TYPE_NUM; i++)
    for(i=0; i<CUSTOMER_NUM; i++)
    {
        printf("name:%s\n", user_name[i]);
        pr[i] = new CRingBuf(user_name[i], "adas_jpg", ADAS_JPEG_SIZE, CRB_PERSONALITY_READER);
    }

    ThreadPool pool; // 0 - cpu member
    pool.SetMinThreads(4);

    sleep(3);
    while(1)
    {
#if 0        
        if(pull_mm_queue(&mm))
        {
            usleep(10000);
            continue;
        }

#else//debug

        mm.video_time = 3;
        mm.warn_type = 1;
        mm.video_id[0] = cnt++;
        mm.video_enable = 1;
        mm.photo_enable = 0;
 //       sleep(2);

        if(cnt >= 2)
            sleep(2);

        if(cnt >= 4)
            sleep(2000000);
#endif

#if 0
        printf("warn type: 0x%x, period0x%x\n", mm.warn_type, mm.photo_time_period);
        printf("mm type: 0x%x, mmid: 0x%x\n", mm.mm_type, mm.mm_id[0]);
        printf("video_time: 0x%x, num: 0x%x\n", mm.video_time, mm.photo_num);
#endif

        i = 0;
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
                i=6;
                break;

        }
        
        //上一个报警视频没有获取完成，当有新的同类型报警,视频不再获取。
        if(mm.video_enable)
        {
            if(first_record_time[i])
            {
                first_record_time[i] = 0;
                gettimeofday(&time_begin[i], NULL);
            }
            else
            {
                if(!is_timeout(&time_begin[i], mm.video_time*1000)) //we can record again
                {
                    gettimeofday(&time_begin[i], NULL);
                }
                else
                {
                    printf("new warning video ignore!\n");
                    continue;
                }
            }
        }
        // printf("Thread pool enter! i = %d\n", i);
        cls[i] = NewClosure(record_jpeg, pr[i], mm);
        pool.AddTask(cls[i]);
    }

    return NULL;
}


