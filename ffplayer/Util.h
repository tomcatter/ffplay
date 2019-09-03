#pragma once

#include <stdio.h>
#include <sstream>

std::string format_time();
void player_log(std::string& fun, const char *fmt, ...);
void ffmpeg_log_callback(void* ptr, int level, const char* fmt, va_list vl);
void init_log();


#define LOG_INIT() do \
{\
	init_log();\
} while (false)


#define LOG_PLAYER(fmt, ...)  do \
{\
	std::ostringstream oss;\
	oss << __FUNCTION__<< " line:"<<__LINE__;\
	player_log(oss.str(),fmt,## __VA_ARGS__);\
}while (false)

