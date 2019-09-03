#pragma once

#ifdef _WIN32
//Windows
extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
#include "libavutil/time.h"
};
#include "SDL.h"
#else
//Linux...
#ifdef __cplusplus
extern "C"
{
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#ifdef __cplusplus
};
#endif
#include <SDL.h>
#endif

#include <string>

typedef int  (*FileFinishCb)(void*);

int init(FileFinishCb);

double get_time_pos_in_file(std::string& );

void start_play();

int add_file(const char* filename, const char* key, unsigned int attr);

void audio_pause(int pause_flag);

void seek_in_file(int ts);

int get_cur_file_duration();

bool getCurrentState();

void enter_flip_mode(int frames);

void set_speed_multiplier(int multiplier);

void set_audio_volume(double vol);

void uninit();

