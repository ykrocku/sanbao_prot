/// @file MjpegWriter.h
/// @brief A Motion-JPEG Writer
/// @author pengquanhua (pengquanhua@minieye.cc)
/// @date 2018-1-15
/// Copyright (C) 2018 - MiniEye INC.

#ifndef COMMON_MJPEG_MJPEGWRITER_H_
#define COMMON_MJPEG_MJPEGWRITER_H_

#include <stdint.h>
#include <vector>

class MjpegWriter {
 public:
    MjpegWriter();
    int Open(char* outfile, int fps, int width, int height);
    int Write(void *pBuf, int pBufSize);  // 写入一帧
    int Close();

 private:
    FILE *outFile;  // C stream
    char *outfileName;
    int outfps;
    int width, height, FrameNum;
    int chunkPointer, moviPointer;
    std::vector<int> FrameOffset, FrameSize, AVIChunkSizeIndex, FrameNumIndexes;
    bool isOpen;

    void StartWriteAVI();
    void WriteStreamHeader();
    void WriteIndex();
    bool WriteFrame(void *pBuf, int pBufSize);
    void FinishWriteAVI();
    void PutInt(int elem);
    void PutShort(int16_t elem);
    void StartWriteChunk(int fourcc);
    void EndWriteChunk();
};

#endif  // COMMON_MJPEG_MJPEGWRITER_H_
