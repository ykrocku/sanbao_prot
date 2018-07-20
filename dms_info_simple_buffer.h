#ifndef _DMS_INFO_SIMPLE_BUFFER_H
#define _DMS_INFO_SIMPLE_BUFFER_H

//#include "dms_info.h"

/* 八字节格式的DMS数据 */

/*
    报警信息协议对接格式
    "topic"     "dms.alert.0x100"    str
    "time"      uint64_t usec        MSGPACK_OBJECT_POSITIVE_INTEGER
    "soucre"    "DMSNEWS"            str
    "data"      uint8_t buf[8]       bin
*/

#define DMS_INFO_TOPIC  ("dms.alert.0x100")
#define DMS_INFO_SOURCE  ("DMSNEWS")

#define DMS_INFO_SIMPLE_BUFFER_SIZE (8)
#define DMS_INFO_ALERT_BIT_NUM  (2)
#define DMS_INFO_ALERT_PER_BYTE (8/DMS_INFO_ALERT_BIT_NUM)
#define DMS_INFO_ALERT_BYTE_INDEX(index) (index / DMS_INFO_ALERT_PER_BYTE)
#define DMS_INFO_ALERT_MASK_OFFSET(index) (2 * (index % DMS_INFO_ALERT_PER_BYTE))
#define DMS_INFO_ALERT_MASK(index) (0x03 << DMS_INFO_ALERT_MASK_OFFSET(index)) 


#define DMS_INFO_GET_ALERT_FROM_BUFFER(buffer, index) \
    (((buffer[DMS_INFO_ALERT_BYTE_INDEX((index))] & DMS_INFO_ALERT_MASK((index))) >> DMS_INFO_ALERT_MASK_OFFSET((index))) & 0x03)

#define DMS_INFO_CLEAR_ALERT(buffer, index) \
    (buffer[DMS_INFO_ALERT_BYTE_INDEX((index))] &= ~(DMS_INFO_ALERT_MASK((index))))

#define DMS_INFO_SET_ALERT_TO_BUFFER(buffer, index, val) \
    do{\
        DMS_INFO_CLEAR_ALERT(buffer, (index));\
        buffer[DMS_INFO_ALERT_BYTE_INDEX(index)] |= (((val) & 0x03) << DMS_INFO_ALERT_MASK_OFFSET(index));\
    } while(0)

#endif /* _DMS_INFO_SIMPLE_BUFFER_H */
