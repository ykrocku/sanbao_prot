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
#include <opencv2/opencv.hpp>

#include <string.h>
#include <sys/time.h>

#include "common/ringbuf/RingBufFrame.h"
#include "common/ringbuf/CRingBuf.h"
#include "common/time/timestamp.h"
#include "common/hal/ma_api.h"
#include "common/hal/android_api.h"
#include <stdlib.h>
#include <fstream>
#include "common/mp4/MP4Writer.h"
#include "common/mjpeg/MjpegWriter.h"

#include <cstddef>
#include "common/base/closure.h"
#include "common/concurrency/this_thread.h"
#include "common/concurrency/thread_pool.h"

#include <iostream>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>


#include "prot.h"
#include "common.h"

#define USE_HW_JPEG
#ifdef USE_HW_JPEG
#include "common/hal/android_api.h"
#else
#include <opencv2/opencv.hpp>
#endif

using namespace std;

extern volatile int force_exit;

#define  ADAS_JPEG_SIZE (16* 1024 * 1024)


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

std::vector<uint8_t> jpeg_encode(uint8_t* data,\
        int cols, int rows, int out_cols, int out_rows, int quality) {
    std::vector<uint8_t> jpg_vec(512 * 1024);

#ifdef USE_HW_JPEG
    //fprintf(stderr, "Using hardware JPEG encoder.\n");
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
std::string GetTimestamp() {
    time_t rawtime;
    struct tm* timeinfo;
    char buffer[80];
    memset(buffer, 0, sizeof buffer);
    time(&rawtime);
    
    //convert to CST
    rawtime += 8*3600;

    timeinfo = localtime(&rawtime);
    strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M:%S", timeinfo);
    return buffer;
}


//填写报警信息的一些实时数据
std::string get_latitude_msg()
{
    char buffer[80];

    get_latitude_info(buffer, sizeof(buffer));
    //printf("latitude: %s\n", buffer);
    return buffer;
}




#if 1
// color
const CvScalar COLOR_BLACK = CV_RGB(0, 0, 0);
const CvScalar COLOR_DARKGRAY = CV_RGB(55, 55, 55);
const CvScalar COLOR_WHITE = CV_RGB(255, 255, 255);
const CvScalar COLOR_RED = CV_RGB(255, 0, 0);
const CvScalar COLOR_GREEN = CV_RGB(0, 255, 0);
const CvScalar COLOR_BLUE = CV_RGB(0, 0, 255);
#endif

int EncodeRingBufWrite(CRingBuf* pRB, void *buf, int len, int width, int height)
{
    static uint64_t mFrameIdx=0;
    RBFrame *pwFrame = nullptr;
    //request to write
    pwFrame = (RBFrame *) pRB->RequestWriteFrame(len + sizeof(RBFrame), CRB_FRAME_I_SLICE);
    if (!CRB_VALID_ADDRESS(pwFrame)) {
        printf("RequestWriteFrame %d byte failed", len);
        return -1;
    }

    //printf("jpg_size = %d\n", jpg_size);
    pwFrame->frameTag = RBFrameTag;
    pwFrame->frameType = IFrame;
    pwFrame->channel = 20;
    pwFrame->frameIdx = mFrameIdx++;
    pwFrame->frameNo  = mFrameIdx;
    pwFrame->dataLen = len;
    pwFrame->video.VWidth = width;
    pwFrame->video.VHeight = height;

    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);

    pwFrame->time = t.tv_sec * 1000 + t.tv_nsec/1000000;
    pwFrame->pts = pwFrame->time;
    memcpy(pwFrame->data, buf, len);
    //memcpy(pwFrame->data, info.addr, 2 * 1024 * 1024);
    pRB->CommitWrite();
    //print_frame("producer", pwFrame);

    return 0;
}



int encode_process(CRingBuf* pRB, CRingBuf* pwRB, int quality, int width, int height) {
    uint32_t jpg_size=0;
    RBFrame* pFrame = nullptr;
    uint32_t frameLen = 0;
    static uint64_t framecnt_old = 0;
#if 0
    pRB->SeekIndexByTime(0);  // seek to the latest frame
    pFrame = request_jpeg_frame(pRB, 10);
    if(pFrame == nullptr)
        return -1;
#endif
    do{
        //usleep(25000);
        pRB->SeekIndexByTime(0);  // seek to the latest frame
        pFrame = reinterpret_cast<RBFrame*>(pRB->RequestReadFrame(&frameLen));
        if (!CRB_VALID_ADDRESS(pFrame)) {
            fprintf(stderr, "Error: RequestRead origin Frame failed\n");
            usleep(25000);
            pFrame = nullptr;
            continue;
        }else{
            if (pFrame->data == nullptr ||
                    pFrame->video.VWidth == 0 ||
                    pFrame->video.VHeight == 0) {
                fprintf(stderr, "Error: origin image stream exhausted\n");
                pRB->CommitRead();
                //exit(2);
                //return -1;
                usleep(25000);
                continue;
            }
            //print_frame("origin", pFrame);
            if(pFrame->frameNo == framecnt_old){
                usleep(25000);
                continue;
            }



//两个摄像头都是15帧，取三分之二。
#if 1
//#if defined ENABLE_DMS
            if(pFrame->frameNo % 3 == 0){
                framecnt_old = pFrame->frameNo;
                usleep(20000);
            }else{
                break;
            }
#else
            if(pFrame->frameNo % 3 == 0){
                break;
            }else{
                framecnt_old = pFrame->frameNo;
                usleep(20000);
            }
#endif
        }
    }while(1);

    framecnt_old = pFrame->frameNo;
    //print_frame("origin even", pFrame);


/******************************************************
*在图像上添加时间戳的代码，在220行左右：
*std::string time = GetTimestamp();  // 时间戳字符串 YYYY-mm-dd HH:MM:SS
*cv::putText(new_image, time, cv::Point(20, 60),    // 文字在图像上的坐标(x, y)
*        CV_FONT_HERSHEY_DUPLEX, 1.5, COLOR_BLUE, 2, CV_AA);
*
*关于cv::putText参数的补充说明
*如果觉得文字太大，可以把 1.5改成 1.2或者1.1
*倒数第2个参数是thinkness，这里2表示粗体，不想要粗体的话可以改成1
*******************************************************/

#if 1
    //add color
    cv::Mat new_image;
    cv::Size dim(pFrame->video.VWidth, pFrame->video.VHeight);
    cv::Mat image(dim, CV_8UC3, pFrame->data);
    image.copyTo(new_image);
    pRB->CommitRead();

    std::string time = GetTimestamp();
    std::string latitude =  get_latitude_msg();

    cv::putText(new_image, time, cv::Point(20, 60),
                CV_FONT_HERSHEY_DUPLEX, 1.5, COLOR_BLUE, 2, CV_AA);

    cv::putText(new_image, latitude, cv::Point(20, 120),
                CV_FONT_HERSHEY_DUPLEX, 1.5, COLOR_BLUE, 2, CV_AA);

    std::vector<uint8_t> jpg_vec = jpeg_encode(new_image.data,
        new_image.cols, new_image.rows, width, height, quality);
#else
    std::vector<uint8_t> jpg_vec = jpeg_encode(pFrame->data,
            pFrame->video.VWidth, pFrame->video.VHeight, width, height, quality);
#endif
    pRB->CommitRead();
    jpg_size = jpg_vec.size();

    EncodeRingBufWrite(pwRB, jpg_vec.data(), jpg_size, width, height);
    EncodeRingBufWrite(pwRB, jpg_vec.data(), jpg_size, width, height);

    return 0;
}

int ConfigResolution[][2]={
352,288, //not use
352,288,
704,288,
704,576,
640,480,
1280,720,
1920,1080,
};

void GetConfigResolution(int *w, int *h)
{
    int index = 0;

#if defined ENABLE_ADAS
    AdasParaSetting para;
    read_dev_para(&para, SAMPLE_DEVICE_ID_ADAS);
    index = para.image_Resolution%6;
    *w = ConfigResolution[index][0];
    *h = ConfigResolution[index][1];
#elif defined ENABLE_DMS
    DmsParaSetting para;
    read_dev_para(&para, SAMPLE_DEVICE_ID_DMS);
    index = para.image_Resolution%6;
    *w = ConfigResolution[index][0];
    *h = ConfigResolution[index][1];

#endif

    printf("GET Resolution %d x %d\n", *w, *h);

}

/*
0x01:352×288
0x02:704×288
0x03:704×576
0x04:640×480
0x05:1280×720
0x06:1920×1080
*/
#include <sys/prctl.h>
void *pthread_encode_jpeg(void *p)
{
    int ret = 0;
    int quality = 50;
    //int quality = 40;
    //int Vwidth = 640;
    //int Vheight = 360;
    
    int Vwidth = 704;
    int Vheight = 576;

    //GetConfigResolution(&Vwidth, &Vheight);

    int cnt = 0;
    struct timespec t;
    printf("%s enter!\n", __FUNCTION__);
    prctl(PR_SET_NAME, "encode");
#if defined ENABLE_ADAS 
    const char *rb_name = ma_api_get_rb_name(MA_RB_TYPE_ADAS_RAW);
    printf("get rb_name: %s\n", rb_name);
    int32_t rb_size = ma_api_get_rb_size(MA_RB_TYPE_ADAS_RAW);
    printf("get rb_size: %d\n", rb_size);
    ma_api_open_camera(MA_CAMERA_IDX_ADAS);
    printf("ma_api_open_camera MA_CAMERA_IDX_ADAS\n");
    ma_api_open_camera(MA_CAMERA_IDX_DRIVER);
    printf("ma_api_open_camera MA_CAMERA_IDX_DRIVER\n");
    CRingBuf* pRb = new CRingBuf("adas_encode_jpeg", rb_name, rb_size, CRB_PERSONALITY_READER, true);
    CRingBuf* pwjpg = new CRingBuf("adas_producer", "adas_jpg", ADAS_JPEG_SIZE, CRB_PERSONALITY_WRITER);
#else
    const char *rb_name = ma_api_get_rb_name(MA_RB_TYPE_DRIVER_RAW);
    printf("get rb_name: %s\n", rb_name);
    int32_t rb_size = ma_api_get_rb_size(MA_RB_TYPE_DRIVER_RAW);
    printf("get rb_size: %d\n", rb_size);
    ma_api_open_camera(MA_CAMERA_IDX_ADAS);
    ma_api_open_camera(MA_CAMERA_IDX_DRIVER);
    CRingBuf* pRb = new CRingBuf("dms_encode_jpeg", rb_name, rb_size, CRB_PERSONALITY_READER, true);
    CRingBuf* pwjpg = new CRingBuf("dms_producer", "dms_jpg", ADAS_JPEG_SIZE, CRB_PERSONALITY_WRITER);
#endif

    if(!pwjpg || !pRb)
    {
        printf("new CRingBuf fail!\n");
        return NULL;
    }
	clock_gettime(CLOCK_MONOTONIC, &t);
    while(!force_exit)
    {
        ret = encode_process(pRb, pwjpg, quality, Vwidth, Vheight);
        if(!ret)//encode success
            cnt++;
        else{
            usleep(25000);
        }
        if(timeout_trigger(&t, 2))
        {
            //GetConfigResolution(&Vwidth, &Vheight);
	        clock_gettime(CLOCK_MONOTONIC, &t);
            printf("encdoe speed %d per 2 sec\n", cnt);
            cnt = 0; 
        }
    }

#if defined ENABLE_ADAS 
    delete pRb;
#else
    delete pwjpg;
#endif
    pthread_exit(NULL);
}

void store_one_jpeg(InfoForStore *mm, RBFrame* pFrame, int index)
{
    char filepath[100];
    char writefile_link[100];
    MmInfo_node node;

    sprintf(filepath,"%s%08d.jpg", SNAP_SHOT_JPEG_PATH,mm->photo_id[index]);

    fprintf(stdout, "Saving image file...%s\n", filepath);
    int rc = write_file(filepath, pFrame->data, pFrame->dataLen);
    if (rc == 0) {
        printf("Image saved to [%s]\n", filepath);
    } else {
        fprintf(stderr, "Cannot save image to %s\n", filepath);
    }

    sprintf(writefile_link,"ln -s %s %s%s-%08d.jpg",filepath, SNAP_SHOT_JPEG_PATH,\
            warning_type_to_str(mm->warn_type), mm->photo_id[index]);
    //system(writefile_link);

    node.warn_type = mm->warn_type;
    node.mm_type = MM_PHOTO;
    node.mm_id = mm->photo_id[index];
    //uint8_t time[6];
    insert_mm_resouce(node);
}

void store_warn_jpeg(CRingBuf* pRB, InfoForStore *mm)
{
    RBFrame* pFrame = nullptr;
    int jpeg_index = 0;
    uint32_t interval= 0; //usleep
    
    printf("%s enter!\n", __FUNCTION__);

    interval = mm->photo_time_period*100*1000;
    while(jpeg_index < mm->photo_num)
    {
        pRB->SeekIndexByTime(0);
        pFrame = request_jpeg_frame(pRB, 10);
        if(pFrame == nullptr)
            continue;

        usleep(interval);
        print_frame("jpeg", pFrame);
        store_one_jpeg(mm, pFrame, jpeg_index++);
        pRB->CommitRead();
    }
}

#define RECORD_JPEG_NEED 1
#define RECORD_JPEG_NO_NEED 0
void store_one_mp4(CRingBuf* pRB, InfoForStore *mm, int jpeg_flag)
{
#define VIDEO_FRAMES_PER_SECOND   12
    RBFrame* pFrame = nullptr;
    char mp4filepath[100];
    char testfilepath[100];
    MmInfo_node node;
    uint32_t FrameNumEnd = 0;
    int jpeg_index = 0;
    uint32_t interval= 0; //ms
    uint32_t framecnt = 0;
    int seektime = 0;
    int fps = 0;
    int i=0;
    int sec = 0;
    int usec = 0;
    int64_t frame_oldtime=0;

    printf("%s enter!\n", __FUNCTION__);
    //interval = mm->photo_time_period*100*1000; //us
    interval = mm->photo_time_period; //单位改成秒
//start
#define SEEK_TIME_MAX   60
#define ENCODE_FRAME_MAX 15

#if 1
    if(jpeg_flag){//recode jpg time

        for(i=0; i<mm->photo_num; i++){
            pRB->SeekIndexByTime(0);
            pFrame = request_jpeg_frame(pRB, 10);
            if(pFrame == nullptr)
                return;
            print_frame("video jpeg", pFrame);
            store_one_jpeg(mm, pFrame, jpeg_index++);
            //usleep(interval);
            sleep(interval);
            //usleep(interval*1200000);
        }
    }
#else
    printf("store jpg interval = %d\n", interval);
    if(jpeg_flag){//recode jpg time
        while(i<mm->photo_num)
        {
            pRB->SeekIndexByTime(0);
            pFrame = request_jpeg_frame(pRB, 0);
            if(pFrame == nullptr){
                usleep(60000);
                continue;
            }
            print_frame("pre jpeg", pFrame);
            if(pFrame->time - frame_oldtime > interval*1100){
                frame_oldtime = pFrame->time;
                print_frame("get jpeg", pFrame);
                store_one_jpeg(mm, pFrame, jpeg_index++);
                i++;
            }
        }
    }
#endif

    //sleep(mm->video_time - interval/1000000);
    sleep(mm->video_time - (mm->photo_num -1)*interval);
    pRB->SeekIndexByTime(0);//get last frame
    pFrame = request_jpeg_frame(pRB, 10);
    if(pFrame == nullptr)
    {
        printf("Get frame saving file fail!\n");
        return;
    }
    FrameNumEnd = pFrame->frameNo;

    seektime = (2*mm->video_time);
    printf("seek time:%d\n", 0-seektime);
    pRB->SeekIndexByTime(0-seektime);
    do{
        pFrame = request_jpeg_frame(pRB, 10);
        if(pFrame == nullptr)
            break;

        print_frame("seek jpeg", pFrame);

        //framecnt = (FrameNumEnd > pFrame->frameNo) ? (FrameNumEnd - pFrame->frameNo) : (FrameNumEnd - pFrame->frameNo);
        framecnt = (FrameNumEnd > pFrame->frameNo) ? (FrameNumEnd - pFrame->frameNo) : (100);
        printf("frame sub = %d\n", framecnt);
        framecnt = framecnt % (SEEK_TIME_MAX*ENCODE_FRAME_MAX);
        printf("frame sub2 = %d\n", framecnt);
        fps = framecnt/seektime;

#define VIDEO_MP4
#ifdef VIDEO_MP4
        sprintf(mp4filepath,"%s%08d.mp4", SNAP_SHOT_JPEG_PATH, mm->video_id[0]);
        std::ofstream ofs(mp4filepath, std::ofstream::out | std::ofstream::binary);
        MP4Writer mp4(ofs, fps, pFrame->video.VWidth, pFrame->video.VHeight);
        fprintf(stdout, "Saving image file...%s, fps = %d\n", mp4filepath, fps);
        while(framecnt--)
        {
            pFrame = request_jpeg_frame(pRB, 10);
            if(pFrame == nullptr)
                break;
            mp4.Write(pFrame->data, pFrame->dataLen);
            print_frame("video jpeg", pFrame);
            pRB->CommitRead();
        }

        mp4.End();
        ofs.close();
        printf("%s mp4 done!\n", warning_type_to_str(mm->warn_type));
#else


        sprintf(mp4filepath,"%s%08d.mp4", SNAP_SHOT_JPEG_PATH, mm->video_id[0]);
        std::ofstream ofs(mp4filepath, std::ofstream::out | std::ofstream::binary);
        MjpegWriter mp4(ofs, fps, pFrame->video.VWidth, pFrame->video.VHeight);
        fprintf(stdout, "Saving image file...%s, fps = %d\n", mp4filepath, fps);
        while(framecnt--)
        {
            pFrame = request_jpeg_frame(pRB, 10);
            if(pFrame == nullptr)
                break;
            mp4.Write(pFrame->data, pFrame->dataLen);
            print_frame("video jpeg", pFrame);
            pRB->CommitRead();
        }

        mp4.End();
        ofs.close();
        printf("%s mp4 done!\n", warning_type_to_str(mm->warn_type));

#if 0
        sprintf(mp4filepath,"%s%08d.avi", SNAP_SHOT_JPEG_PATH, mm->video_id[0]);
        MjpegWriter mjpeg;
        mjpeg.Open(mp4filepath, fps, pFrame->video.VWidth, pFrame->video.VHeight);
        fprintf(stdout, "Saving image file...%s, fps = %d\n", mp4filepath, fps);
        while(framecnt--)
        {
            pFrame = request_jpeg_frame(pRB, 10);
            if(pFrame == nullptr)
                break;
            mjpeg.Write(pFrame->data, pFrame->dataLen);
            print_frame("video jpeg", pFrame);
            pRB->CommitRead();
        }
        mjpeg.Close();
        printf("%s avi done!\n", warning_type_to_str(mm->warn_type));
#endif

#endif
        node.warn_type = mm->warn_type;
        node.mm_type = MM_VIDEO;
        node.mm_id = mm->video_id[0];
        //uint8_t time[6];
        insert_mm_resouce(node);
        //display_mm_resource();
    }while(0);
}

//修改为同时获取jpg 和mp4 
void record_mm_infor(CRingBuf* pRB, InfoForStore info)
{
    int i=0;
    RBFrame* pFrame = nullptr;
    char mp4filepath[100];
    InfoForStore mm;
    MmInfo_node node;
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
        store_one_mp4(pRB, &mm, RECORD_JPEG_NO_NEED);
    }
    else if(mm.photo_enable && mm.video_enable)
    {
        store_one_mp4(pRB, &mm, RECORD_JPEG_NEED);
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
    else if(!strncmp("mp4", file_type, strlen("mp4")))
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
    MmInfo_node node;
    uint32_t i=0, j=0;
    int ret = 0;
    char *pos = NULL;

    pos = &mm_id[0];
    memset(warn_name, 0, sizeof(warn_name));
    memset(mm_id, 0, sizeof(mm_id));
    memset(file_type, 0, sizeof(file_type));
    for(i=0; i<strlen(filename); i++)
    {
#if 0
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
#else
        if(filename[i] == '.')
        {
            j=0;
            pos = &file_type[0];
            continue;
        }
        pos[j++] = filename[i];

#endif

    }
#if 0
    ret = str_to_warning_type(warn_name, &node.warn_type);
    if(ret < 0)
        return;
#endif
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


ThreadPool pool; // 0 - cpu member
int read_pthread_num(uint32_t i)
{
#if 0
    struct Stats
    {
        size_t NumThreads;
        size_t NumBusyThreads;
        size_t NumPendingTasks;
    };
#endif

    //struct Stats pstat;
    //pool.Stats pstat;
    struct timespec rec_time;
    ThreadPool::Stats pstat;
    pool.GetStats(&pstat);
    clock_gettime(CLOCK_MONOTONIC, &rec_time);
    printf(" i =%d, Num = %ld, busy = %ld, pending = %ld, time = %ld.%ld\n", i, pstat.NumThreads, pstat.NumBusyThreads, pstat.NumPendingTasks, rec_time.tv_sec, rec_time.tv_nsec);

    return pstat.NumThreads;
}

extern int save_mp4;
extern pthread_mutex_t  save_mp4_mutex;
extern pthread_cond_t   save_mp4_cond;

void *pthread_save_media(void *p)
{
#define CUSTOMER_NUM   8 
    int cnt = 0;
    uint32_t i = 0;
    int index = 0;
    InfoForStore mm;
    CRingBuf* pr[WARN_TYPE_NUM];
    CRingBuf* ptest;
    Closure<void>* cls[WARN_TYPE_NUM];
    char user_name[CUSTOMER_NUM][20]={
        "customer_FCW_mp4","customer_LDW_mp4","customer_HW_mp4","customer_PCW_mp4",
        "customer_FLC_mp4","customer_TSRW_mp4","customer_TSR_mp4","customer_SNAP_mp4",
    };

    char dms_user_name[CUSTOMER_NUM][20]={
        "DMS_FATIGUE_WARN","DMS_CALLING_WARN","DMS_SMOKING_WARN","DMS_DISTRACT_WARN",
        "DMS_ABNORMAL_WARN","DMS_SANPSHOT_EVENT","DMS_DRIVER_CHANGE","DMS_RESV",
    };

    //for(i=0; i<WARN_TYPE_NUM; i++)
    for(i=0; i<CUSTOMER_NUM; i++)
    {

#if defined ENABLE_ADAS 
        printf("name:%s\n", user_name[i]);
        pr[i] = new CRingBuf(user_name[i], "adas_jpg", ADAS_JPEG_SIZE, CRB_PERSONALITY_READER);
#elif defined ENABLE_DMS
        printf("name:%s\n", dms_user_name[i]);
        pr[i] = new CRingBuf(dms_user_name[i], "dms_jpg", ADAS_JPEG_SIZE, CRB_PERSONALITY_READER);
#endif
    }

#if defined ENABLE_ADAS 
    ptest = new CRingBuf("adas_get_dms", "dms_jpg", ADAS_JPEG_SIZE, CRB_PERSONALITY_READER);
#elif defined ENABLE_DMS
    ptest = new CRingBuf("dms_get_adas", "adas_jpg", ADAS_JPEG_SIZE, CRB_PERSONALITY_READER);
#endif

    pool.SetMinThreads(8);
    //pool.SetMaxThreads(4);
    printf("min pthread = %d\n", pool.GetMinThreads());
    printf("max pthread = %d\n", pool.GetMaxThreads());
    printf("GetIdleTime = %d\n", pool.GetIdleTime());

    //sleep(3);
    //sleep(20);
    while(1)
    {
        //read_pthread_num(i);
#if 1       

#if 0
        pthread_mutex_lock(&save_mp4_mutex);
        while(!save_mp4)
            pthread_cond_wait(&save_mp4_cond, &save_mp4_mutex);
        if(save_mp4 == NOTICE_MSG)
            save_mp4 = WAIT_MSG;
        pthread_mutex_unlock(&save_mp4_mutex);

        if(IS_EXIT_MSG(save_mp4))
            break;
#endif
        if(pull_mm_queue(&mm))
        {
            usleep(10000);
            // sleep(1);
            continue;
        }

#ifdef ENABLE_DMS
        //printf("photo num: 0x%x\n", mm.photo_num);
        //produce_dms_image(&mm);

        // continue;
#endif

#else//debug

        mm.video_time = 4;
        mm.warn_type = 1;
        mm.video_enable = 1;
        mm.photo_enable = 1;

        mm.photo_time_period = 4;
        mm.photo_num = 3;
        mm.photo_id[0] = 1;
        mm.photo_id[1] = 2;
        mm.photo_id[2] = 3;
        mm.video_id[0] = 4;

        //if(cnt >= 2)
        //    sleep(2);

        //if(cnt >= 4)
        //    sleep(2000000);
#endif

#if 0
        printf("warn type: 0x%x, period0x%x\n", mm.warn_type, mm.photo_time_period);
        printf("mm type: 0x%x, mmid: 0x%x\n", mm.mm_type, mm.mm_id[0]);
        printf("video_time: 0x%x, num: 0x%x\n", mm.video_time, mm.photo_num);
#endif
#if 0
        if(read_pthread_num(i) >= CUSTOMER_NUM)
        {
            printf("threadpool busy!");
            continue;
        }
#endif
        i = i % 8;
        if(!mm.flag){
            cls[i] = NewClosure(record_mm_infor, pr[i], mm);
            pool.AddTask(cls[i]);
            i++;
        }else{
            printf("store mp4 alone!\n");
            cls[i] = NewClosure(store_one_mp4, ptest, &mm, RECORD_JPEG_NO_NEED);
            pool.AddTask(cls[i]);
            i++;
        }
    }
    //for(i=0; i<WARN_TYPE_NUM; i++)
    for(i=0; i<CUSTOMER_NUM; i++)
    {
#if defined ENABLE_ADAS 
        printf("name:%s\n", user_name[i]);
        delete pr[i];
#elif defined ENABLE_DMS
        printf("name:%s\n", dms_user_name[i]);
        delete pr[i];
#endif
    }

#if defined ENABLE_ADAS 
    delete ptest;
#elif defined ENABLE_DMS
    delete ptest;
#endif
    pthread_exit(NULL);
}

