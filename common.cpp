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
#include <queue>

#include <string.h>
#include <errno.h>
#include <assert.h>
#include <semaphore.h>
#include <sys/prctl.h>

#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>


using namespace std;
#include "prot.h"
#include "common.h"


//return 0, 超时，单位是毫秒 
int timeout_trigger(struct timespec *tv, int sec)
{
    struct timespec cur;
    long last = 0, now = 0;

	clock_gettime(CLOCK_MONOTONIC, &cur);

    last =  tv->tv_sec*1000 + tv->tv_nsec/1000000;
    now =  cur.tv_sec*1000 + cur.tv_nsec/1000000;

    //if((cur.tv_sec >= tv->tv_sec + sec) && (cur.tv_nsec > tv->tv_nsec)){
    if(now - last > sec*1000){
        //printf("timeout_trigger! %d s\n", sec);
        return 1;
    }
    else
        return 0;
}


//推入队列，可以只有node的header，数据可以为空
int ptr_queue_push(queue<ptr_queue_node *> *p, ptr_queue_node *in,  pthread_mutex_t *lock)
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
int ptr_queue_pop(queue<ptr_queue_node*> *p, ptr_queue_node *out,  pthread_mutex_t *lock)
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
        //printf("no data in node!\n");
        memcpy(out, header, sizeof(ptr_queue_node));
    }
    //node have data,
    else
    {
        //user don't need data
        if(!out->buf)
        {
            //printf("user no need data out!\n");
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




static list<MmInfo_node> mmlist;
static pthread_mutex_t mm_resource_lock = PTHREAD_MUTEX_INITIALIZER;

void display_mm_resource()
{
    list<MmInfo_node>::iterator it;  

    pthread_mutex_lock(&mm_resource_lock);
    for(it=mmlist.begin();it!=mmlist.end();it++)  
    {  
        printf("display list id = %d\n",it->mm_id);
        printf("warn_type = %d, mm_type=%d\n",it->warn_type, it->mm_type);
    }  
    pthread_mutex_unlock(&mm_resource_lock);
}

int32_t find_mm_resource(uint32_t id, MmInfo_node *m)
{
    list<MmInfo_node>::iterator it;  
    int ret = -1;

    pthread_mutex_lock(&mm_resource_lock);
    for(it=mmlist.begin();it!=mmlist.end();it++)  
    {  
        //  printf("find id=%d,list id = %d\n",id, it->mm_id);
        //  printf("warn_type = %d, mm_type=%d\n",it->warn_type, it->mm_type);
        if(it->mm_id == id)
        {
            memcpy(m, &it->rw_flag, sizeof(MmInfo_node));  
            ret = 0;
            break;
        }
    }  
    pthread_mutex_unlock(&mm_resource_lock);
    return ret;
}

int32_t delete_mm_resource(uint32_t id)
{
    list<MmInfo_node>::iterator it;  
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
#if 0
                sprintf(filepath, "%s%s-%08d.jpg",SNAP_SHOT_JPEG_PATH,\
                        warning_type_to_str(it->warn_type), id);
#endif
                sprintf(filepath, "%s%08d.jpg",SNAP_SHOT_JPEG_PATH,id);
                printf("rm jpeg %s\n", filepath);
                remove(filepath);
                it = mmlist.erase(it);  
                ret = 0;
                break;
            }
            if(it->mm_type == MM_VIDEO)
            {
#if 0
                sprintf(filepath,"%s%s-%08d.mp4",SNAP_SHOT_JPEG_PATH,\
                        warning_type_to_str(it->warn_type), id);
#endif

                sprintf(filepath,"%s%08d.mp4",SNAP_SHOT_JPEG_PATH,id);

                printf("rm mp4 %s\n", filepath);
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

void insert_mm_resouce(MmInfo_node m)
{
    pthread_mutex_lock(&mm_resource_lock);
    mmlist.push_back(m); 
    pthread_mutex_unlock(&mm_resource_lock);
}



