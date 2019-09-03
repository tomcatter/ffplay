/**
*
* 该版本使用SDL 2.0替换了第一个版本中的SDL 1.0。
* 注意：SDL 2.0中音频解码的API并无变化。唯一变化的地方在于
* 其回调函数的中的Audio Buffer并没有完全初始化，需要手动初始化。
* 本例子中即SDL_memset(stream, 0, len);
*
* This version use SDL 2.0 instead of SDL 1.2 in version 1
* Note:The good news for audio is that, with one exception,
* it's entirely backwards compatible with 1.2.
* That one really important exception: The audio callback
* does NOT start with a fully initialized buffer anymore.
* You must fully write to the buffer in all cases. In this
* example it is SDL_memset(stream, 0, len);
*
* Version 2.0
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <algorithm>

#include "AudioDecoder.h"
#include "VideoSampleList.h"
#include "Util.h"
//#include "QSeamlessPlayerCallBack.h"

#define __STDC_CONSTANT_MACROS

#define NB_CTX	3

#define MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio
#define MAX_AUDIO_PKT_QUEUE_SIZE 32   //max packet number
#define MAX_VIDEO_PKT_QUEUE_SISE 16
#define MAX_VIDEO_FRAME_QUEUE_SIZE 4
#define MAX_AUDIO_SAMPLE_QUEUE_SIZE 6
#define MAX_FILE_LIST_SIZE 128

#define OUT_CHANNEL_LAYOUT  AV_CH_LAYOUT_MONO
//Output PCM
#define OUTPUT_PCM 0
//Use SDL
#define USE_SDL_AUDIO 1

#define USE_SDL_VIDEO 1


typedef enum {
	READ_THREAD = 0,
	REFRESH_THREAD,
	VIDEO_THREAD,
	AUDIO_THREAD,
	THREAD_NUM
} THREAD_NAME;

typedef enum {
	FLIP_INVALID = -1,
	FLIP_READY = 0,
	FLIP_JUMP_FRAME,
	FLIP_FRAME_OVER,
	FLIP_PAUSE,
} FLIP_STATUS;

typedef enum {
	NULL_OP = -99,
	SEEK = -3,
	MULTIPLY_SPEED = -2,
	ENTER_BORDER = -1,
} PLAYER_OP;


//Buffer:
//|-----------|-------------|
//chunk-------pos---len-----|
static  Sint32  audio_len;
static  Uint8*  audio_pos;

struct SimpleAVState {
	AVFormatContext	*pFormatCtx[NB_CTX];

	int				audioStreamID[NB_CTX];
	AVCodecContext	*pAudioCodecCtx[NB_CTX];
	AVCodec			*pAudioCodec[NB_CTX];
	AVFrame			*pAudioFrame;
	uint32_t		flag[NB_CTX];

	uint8_t			*audio_out_buffer;
	AVRational		audio_time_base[NB_CTX];
	int64_t			cur_audio_pts;
	bool			audio_opened;
	int32_t			out_channels;
	int32_t			out_sample_rate;
	int32_t			out_frame_size;

	SDL_AudioSpec	wanted_spec;
	SwrContext		*au_convert_ctx[NB_CTX];

	int				videoStreamID[NB_CTX];
	AVCodecContext	*pVideoCodecCtx[NB_CTX];
	AVCodec			*pVideoCodec[NB_CTX];
	AVFrame			*pVideoFrame;
	AVFrame			*pYUVFrame[NB_CTX];

	uint8_t			*video_out_buffer[NB_CTX];
	AVRational		video_time_base[NB_CTX];
	int64_t			cur_video_pts;        

	SwsContext		*img_convert_ctx[NB_CTX];

	uint32_t		a_index[NB_CTX];
	uint32_t		v_index[NB_CTX];

	double			duration_per_frame[NB_CTX];  
	int64_t			duration[NB_CTX];     //ms

	FILE*			pPCMFile;
	FILE*			pYUVFile;

	char**			files;
	char**			vkeys;
	uint32_t		attr[MAX_FILE_LIST_SIZE];
	int32_t         add_idx;
	int32_t			cur_file_idx;

	bool			ready;
	bool			paused;
	bool			read_finished;
	bool			wait_packet;
	bool			seeking;
	bool			flip_mode; //翻页模式

	FileFinishCb	file_finish_cb;

	SDL_mutex		*start_mutex;
	SDL_cond		*start_cond;

	SDL_mutex		*pause_mutex;
	SDL_cond		*pause_cond;

	SDL_mutex		*wait_packet_mutex;
	SDL_cond		*wait_packet_cond;

	int64_t			a_last_ts;    //上一个文件最后的时间戳，单位ms
	int64_t			v_last_ts;
	
	int64_t			sys_clock;   //micro second
	int64_t			start_time;

	int64_t			seek_time;   //ms
	int32_t			flip_nb;  //第一个记录当前正在执行的翻页数，第二个记录在执行过程中收到的翻页数
	FLIP_STATUS		flip_mode_status;

	int32_t			speed_multiplier;
	double			volume_audio;
};

static SimpleAVState simple_av_state;
static int cur_v_ctx_idx = 0;
static int cur_a_ctx_idx = 0;

#if USE_SDL_VIDEO

typedef struct SDLVideoState{
	int screen_w;
	int screen_h;

	SDL_Window* screen;
	SDL_Renderer*	renderer;
	SDL_Texture*	texture;

	SDL_Rect		rect;
}SDLVideoState;
static SDLVideoState sdl_v_state;

#endif

typedef struct PacketQueue {
	AVPacketList *first_pkt, *last_pkt;
	int nb_packets;
	int size;
	SDL_mutex *mutex;
	SDL_cond *cond;
	int  wait_flag;
} PacketQueue;


typedef struct AVSample_t {
	uint8_t		*buffer;
	int32_t		buff_size;
	int64_t     pts;
	int32_t		width;						// frame width	
	int32_t		height;						// frame height
} AVSample_t;

typedef struct av_sample_qnode{
	AVSample_t av_sample;
	struct list_head list;
} av_sample_qnode;

static struct q_head  v_sample_q;
static struct q_head  a_sample_q;

static PacketQueue audioq;
static PacketQueue videoq;

static int quit = 0;
static SDL_Thread*  thread_ids[THREAD_NUM];

static Uint32 start32, now32;
static Uint64 start, now;

struct AudioThreadLocal {
	int		cur_a_codec_idx;
	bool	next_section;
	int		speed_multipier;
	int64_t	seek_a_ts;
	PLAYER_OP	op;

	AudioThreadLocal():
		cur_a_codec_idx(0), next_section(true), speed_multipier(1), seek_a_ts(0), op(NULL_OP) {}
};

struct VideoThreadLocal {
	int		cur_v_codec_idx;
	int		speed_multipier;
	int64_t	seek_v_ts;
	PLAYER_OP	op;

	VideoThreadLocal() :
		cur_v_codec_idx(0), speed_multipier(1), seek_v_ts(0),op(NULL_OP){}
};


void  fill_audio(void *udata, Uint8 *stream, int len);
void open_audio_device(int32_t out_sample_rate, int32_t out_channels, int32_t frame_size);
int64_t get_ref_clock(int index);
int seek_internal(int32_t*, int32_t*);


void packet_queue_init(PacketQueue *q) {
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
	q->wait_flag = 1;
}


int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
	AVPacketList *pktls;
	pktls = (AVPacketList *)av_malloc(sizeof(AVPacketList));
	if (!pktls) {
		return -1;
	}

	pktls->pkt = *pkt;
	pktls->next = NULL;

	SDL_LockMutex(q->mutex);

	if (!q->last_pkt) {
		q->first_pkt = pktls;
	}
	else {
		q->last_pkt->next = pktls;
	}

	q->last_pkt = pktls;
	q->nb_packets++;
	q->size += pktls->pkt.size;
	if (q->wait_flag) {
		q->wait_flag = 0;
		SDL_CondSignal(q->cond);
	}
	SDL_UnlockMutex(q->mutex);
	return 0;
}

int packet_queue_put_front(PacketQueue *q, AVPacket *pkt) {
	AVPacketList *pktls;
	pktls = (AVPacketList *)av_malloc(sizeof(AVPacketList));
	if (!pktls) {
		return -1;
	}

	pktls->pkt = *pkt;
	pktls->next = NULL;

	SDL_LockMutex(q->mutex);

	AVPacketList* t = NULL;
	if (q->first_pkt) {
		t = q->first_pkt->next;
		pktls->next = t;
	}
	q->first_pkt = pktls;

	q->nb_packets++;
	q->size += pktls->pkt.size;
	if (q->wait_flag) {
		q->wait_flag = 0;
		SDL_CondSignal(q->cond);
	}
	SDL_UnlockMutex(q->mutex);
	return 0;
}

int packet_queue_get(PacketQueue *q, AVPacket *pkt, bool block) {
	AVPacketList *pktls;
	int ret = -1;

	SDL_LockMutex(q->mutex);

	for (;;) {
		if (quit) {
			ret = -1;
			break;
		}

		pktls = q->first_pkt;
		if (pktls) {
			q->first_pkt = pktls->next;
			if (!q->first_pkt) {
				q->last_pkt = NULL;
			}
			q->nb_packets--;
			q->size -= pktls->pkt.size;
			*pkt = pktls->pkt;

			av_free(pktls);
			ret = 1;
			break;
		}
		else if (!block) {
			if (simple_av_state.read_finished)
				quit = 1;

			ret = 0;
			break;
		}
		else {
			if (simple_av_state.read_finished)
				quit = 1;

			ret = 0;
			q->wait_flag = 1;
			SDL_CondWait(q->cond, q->mutex);
		}
	}

	SDL_UnlockMutex(q->mutex);

	return ret;
}

static int64_t queue_get_header_pts(PacketQueue *q)
{
	AVPacketList *pktls;
	pktls = q->first_pkt;
	if (pktls) {
		return pktls->pkt.pts;
	}
	else {
		return -1;
	}
}


static void packet_queue_flush(PacketQueue *q) {
	AVPacketList *pkt, *pktls;

	SDL_LockMutex(q->mutex);
	for (pkt = q->first_pkt; pkt != NULL; pkt = pktls) {
		pktls = pkt->next;
		av_packet_unref(&pkt->pkt);
		av_freep(&pkt);
	}
	q->last_pkt = NULL;
	q->first_pkt = NULL;
	q->nb_packets = 0;
	q->size = 0;
	SDL_UnlockMutex(q->mutex);
}

int process_audio_thread_cmd(PLAYER_OP op, AudioThreadLocal* local)
{
	int ret = 0;
	switch (op)
	{
		case ENTER_BORDER:
		{
			LOG_PLAYER("audio decode packet, enter border");
			av_sample_qnode*  node = (av_sample_qnode*)av_mallocz(sizeof(av_sample_qnode));
			node->av_sample.buff_size = -1;
			node->av_sample.buffer = NULL;
			q_push(&(node->list), &a_sample_q);
			simple_av_state.a_index[local->cur_a_codec_idx] = 0;
			if (simple_av_state.pAudioCodecCtx[local->cur_a_codec_idx]) {
				avcodec_close(simple_av_state.pAudioCodecCtx[local->cur_a_codec_idx]);
				simple_av_state.pAudioCodecCtx[local->cur_a_codec_idx] = NULL;
			}

			local->cur_a_codec_idx = (local->cur_a_codec_idx + 1) % NB_CTX;
			local->next_section = true;

			break;
		}
		case MULTIPLY_SPEED:
		{
			local->speed_multipier = simple_av_state.speed_multiplier;
			open_audio_device(simple_av_state.out_sample_rate * local->speed_multipier, simple_av_state.out_channels, simple_av_state.out_frame_size);
			break;
		}
		case SEEK:
		{
			local->op = SEEK;
			if (local->cur_a_codec_idx != cur_a_ctx_idx) {
				local->cur_a_codec_idx = (local->cur_a_codec_idx - 1 + NB_CTX) % NB_CTX;
			}
			local->seek_a_ts = simple_av_state.seek_time * simple_av_state.audio_time_base[local->cur_a_codec_idx].den / 1000.0;
			break;
		}
		default:
			break;
	}
	return ret;
}


int audio_decode_packet(AudioThreadLocal& local)
{
	int got_picture = -1;
	int ret = 0;

	AVPacket packet;
	av_init_packet(&packet);

	do {
		if (audioq.nb_packets <= 0) {
			SDL_LockMutex(simple_av_state.wait_packet_mutex);
			if (simple_av_state.wait_packet)
				SDL_CondSignal(simple_av_state.wait_packet_cond);
			SDL_UnlockMutex(simple_av_state.wait_packet_mutex);
		}
		if (packet_queue_get(&audioq, &packet, true) < 0)
			return -1;
		else if (packet.pos < 0) {
			process_audio_thread_cmd((PLAYER_OP)packet.pos, &local);
			continue;
		}
		else {
			break;
		}
	} while (true);

	if (local.next_section) {
		local.next_section = false;

		//int out_sample_rate = simple_av_state.speed_multiplier * simple_av_state.pAudioCodecCtx[local.cur_a_codec_idx]->sample_rate;
		//int out_channels = av_get_channel_layout_nb_channels(simple_av_state.pAudioCodecCtx[local.cur_a_codec_idx]->channel_layout);
		//open_audio_device(out_sample_rate, out_channels, local.cur_a_codec_idx);
		simple_av_state.ready = true;
	}

	AVFrame* frame = simple_av_state.pAudioFrame;
	ret = avcodec_decode_audio4(simple_av_state.pAudioCodecCtx[local.cur_a_codec_idx], frame, &got_picture, &packet);
	if (ret < 0) {
		LOG_PLAYER("Error in decoding audio frame");
		ret = -1;
	}
	if (got_picture > 0) {
		if (local.op == SEEK) {
			if (packet.dts < local.seek_a_ts) {
				av_packet_unref(&packet);
				return 0;
			}
			else {
				simple_av_state.cur_audio_pts = packet.dts;
				simple_av_state.a_index[local.cur_a_codec_idx] = 0;
				local.op = NULL_OP;
			}
		}
		if (simple_av_state.au_convert_ctx) {
			int64_t dst_nb_samples = av_rescale_rnd(swr_get_delay(simple_av_state.au_convert_ctx[local.cur_a_codec_idx], frame->sample_rate) + frame->nb_samples,
										frame->sample_rate, frame->sample_rate, AV_ROUND_INF);
			// 转换，返回值为转换后的sample个数  
			int nb = swr_convert(simple_av_state.au_convert_ctx[local.cur_a_codec_idx], &simple_av_state.audio_out_buffer, dst_nb_samples,
						(const uint8_t**)frame->extended_data, frame->nb_samples);

			//根据布局获取声道数
			int out_channels = av_get_channel_layout_nb_channels(simple_av_state.pAudioCodecCtx[local.cur_a_codec_idx]->channel_layout);
			int data_size = out_channels * nb * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);

			av_sample_qnode*  node = (av_sample_qnode*)av_mallocz(sizeof(av_sample_qnode));
			node->av_sample.width = -1;
			node->av_sample.height = -1;
			node->av_sample.buff_size = data_size;
			node->av_sample.buffer = (uint8_t*)av_mallocz(node->av_sample.buff_size);
			node->av_sample.pts = packet.pts;

			double pos = frame->pts * av_q2d(simple_av_state.audio_time_base[cur_v_ctx_idx]) * 1000;

			std::cout << "cureent time :" << pos << std::endl;

			memcpy(node->av_sample.buffer, simple_av_state.audio_out_buffer, node->av_sample.buff_size);
			q_push(&(node->list), &a_sample_q);

			simple_av_state.a_index[local.cur_a_codec_idx]++;
#if OUTPUT_PCM
			//Write PCM
			fwrite(simple_av_state.audio_out_buffer[cur_au_arr_idx], 1, data_size, simple_av_state.pPCMFile);
#endif
		}
#if 0
		LOG_PLAYER("Audio index:%5d\t pts:%lld\t packet size:%d", simple_av_state.a_index[local.cur_a_codec_idx], packet.pts, packet.size);
#endif
	}
	else
	{
		LOG_PLAYER("Audio got frame fail");
		ret = -1;
	}

	av_packet_unref(&packet);
	return ret;
}


int audio_decode_loop(void* param)
{
	AudioThreadLocal local;

	while (!quit)
	{
		if (audio_decode_packet(local) > 0) {
			int nb = q_list_count(&a_sample_q);
			while (nb > MAX_AUDIO_SAMPLE_QUEUE_SIZE) {
				if (quit)
					break;
				SDL_Delay(2 * MAX_AUDIO_SAMPLE_QUEUE_SIZE);

				nb = q_list_count(&a_sample_q);
			}
		}
	}
	LOG_PLAYER("audio_decode_loop quit");
	return 0;
}


/* The audio function callback takes the following parameters:
* stream: A pointer to the audio buffer to be filled
* len: The length (in bytes) of the audio buffer
*/
void  fill_audio(void *udata, Uint8 *stream, int len) 
{
	av_sample_qnode* pqnode = NULL;
	AVSample_t sample;
	do {
		struct list_head* ptr = q_pop(&a_sample_q);
		if (!ptr) {
			LOG_PLAYER("audio sample queue empty! audio_len=%d  len=%d", audio_len, len);
			break;
		}

		pqnode = (av_sample_qnode *)list_entry(ptr, av_sample_qnode, list);
		sample = pqnode->av_sample;
		if (sample.buff_size == -1 && sample.buffer == NULL) {
			LOG_PLAYER("Fill Audio, enter audio border");
			simple_av_state.a_last_ts = 1000 * av_q2d(simple_av_state.audio_time_base[cur_a_ctx_idx]) * simple_av_state.cur_audio_pts;
			cur_a_ctx_idx = (cur_a_ctx_idx + 1) % NB_CTX;
			simple_av_state.cur_audio_pts = 0;

			av_free(pqnode);
			continue;
		}
		else {
			audio_pos = sample.buffer;
			audio_len = sample.buff_size;
			simple_av_state.cur_audio_pts = sample.pts;
			break;
		}
	} while (!quit);

	if (audio_len <= 0)
		return;

	//SDL 2.0
	if (audio_len < len ) {
		LOG_PLAYER("audio len less than len");
	}

	SDL_memset(stream, 0, len);
	len = len > audio_len ? audio_len : len;
	int32_t volume = simple_av_state.volume_audio * SDL_MIX_MAXVOLUME;
	volume = volume > SDL_MIX_MAXVOLUME ? SDL_MIX_MAXVOLUME : volume;
	volume = volume < 0 ? 0 : volume;
	SDL_MixAudio(stream, audio_pos, len, volume);
	//memcpy(stream, audio_pos, len);

	audio_pos += len;
	audio_len -= len;

	if (sample.buffer)
		av_free(sample.buffer);
	if (pqnode)
		av_free(pqnode);

}
//-----------------

#if USE_SDL_VIDEO

void init_sdl_render(int pixel_w, int pixel_h)
{
	//SDL 2.0 Support for multiple windows  
	sdl_v_state.screen = SDL_CreateWindow("Simplest Video Play SDL2", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		sdl_v_state.screen_w, sdl_v_state.screen_h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	if (!sdl_v_state.screen) {
		LOG_PLAYER("SDL: could not create window - exiting:%s\n", SDL_GetError());
		return;
	}

	sdl_v_state.renderer = SDL_CreateRenderer(sdl_v_state.screen, -1, 0);

	Uint32 pixformat = 0;

	//IYUV: Y + U + V  (3 planes)  
	//YV12: Y + V + U  (3 planes)  
	pixformat = SDL_PIXELFORMAT_IYUV;

	sdl_v_state.texture = SDL_CreateTexture(sdl_v_state.renderer, pixformat, SDL_TEXTUREACCESS_STREAMING, pixel_w, pixel_h);
}

#endif

int init(FileFinishCb cb)
{
	int ret = 0;
	LOG_INIT();

	LOG_PLAYER("start init");
	memset(&simple_av_state, 0, sizeof(SimpleAVState));
	cur_v_ctx_idx = cur_a_ctx_idx = 0;
	quit = 0;
#if USE_SDL_VIDEO
	memset(&sdl_v_state, 0, sizeof(SDLVideoState));
#endif

	packet_queue_init(&audioq);
	packet_queue_init(&videoq);
	q_init(&v_sample_q);
	q_init(&a_sample_q);

	simple_av_state.pAudioFrame = av_frame_alloc();
	simple_av_state.pVideoFrame = av_frame_alloc();
	simple_av_state.audio_out_buffer = (uint8_t *)av_mallocz(MAX_AUDIO_FRAME_SIZE);

	for (int i = 0; i < NB_CTX; i++) {
		simple_av_state.pYUVFrame[i] = av_frame_alloc();
		simple_av_state.video_out_buffer[i] = (uint8_t *)av_mallocz(1920 * 1080 * 2);
	}

	simple_av_state.files = (char**)malloc(MAX_FILE_LIST_SIZE * sizeof(char*));
	memset(simple_av_state.files, 0, MAX_FILE_LIST_SIZE * sizeof(char*));
	//for (int i = 0; filename[i] != NULL; i++) {
	//	size_t len = strnlen_s(filename[i], 256);
	//	simple_av_state.files[i] = (char*)malloc(len + 1);
	//	memcpy_s(simple_av_state.files[i], len + 1, filename[i], len + 1);
	//}

	simple_av_state.add_idx = 0;
	simple_av_state.vkeys = (char**)malloc(MAX_FILE_LIST_SIZE * sizeof(char*));
	memset(simple_av_state.vkeys, 0, MAX_FILE_LIST_SIZE * sizeof(char*));
	//for (int i = 0; vkeys[i] != NULL; i++) {
	//	size_t len = strnlen_s(vkeys[i], 256);
	//	simple_av_state.vkeys[i] = (char*)malloc(len + 1);
	//	memcpy_s(simple_av_state.vkeys[i], len + 1, vkeys[i], len + 1);
	//	simple_av_state.add_idx++;
	//}
	
	av_register_all();
	avformat_network_init();
	av_log_set_callback(ffmpeg_log_callback);

	simple_av_state.file_finish_cb = cb;

	simple_av_state.start_mutex = SDL_CreateMutex();
	simple_av_state.start_cond = SDL_CreateCond();

	simple_av_state.pause_mutex = SDL_CreateMutex();
	simple_av_state.pause_cond = SDL_CreateCond();

	simple_av_state.wait_packet_mutex = SDL_CreateMutex();
	simple_av_state.wait_packet_cond = SDL_CreateCond();

	for (int i = 0; i < THREAD_NUM; i++) {
		thread_ids[i] = NULL;
	}

	simple_av_state.flip_mode_status = FLIP_INVALID;
	simple_av_state.speed_multiplier = 1;
	audio_len = 0;
	simple_av_state.volume_audio = 1.0;

	//Init
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		LOG_PLAYER("Could not initialize SDL - %s", SDL_GetError());
		ret = -1;
	}

#if OUTPUT_PCM
	simple_av_state.pPCMFile = fopen("output.pcm", "wb");
	if (NULL == simple_av_state.pPCMFile)
		ret = -1;

	 simple_av_state.pYUVFile = fopen("output.yuv", "wb+");
	 if (NULL == simple_av_state.pYUVFile)
		 ret = -1;
#endif

	return ret;
}


int add_file(const char* filename, const char* key, unsigned int attr)
{
	LOG_PLAYER("add file: %s, add idx:%d flat=%d", filename, simple_av_state.add_idx, attr);

	if (simple_av_state.files[simple_av_state.add_idx]) {
		free(simple_av_state.files[simple_av_state.add_idx]);
		simple_av_state.files[simple_av_state.add_idx] = NULL;
	}
	size_t len = strnlen_s(filename, 256);
	simple_av_state.files[simple_av_state.add_idx] = (char*)malloc(len + 1);
	memcpy_s(simple_av_state.files[simple_av_state.add_idx], len + 1, filename, len + 1);

	if (simple_av_state.vkeys[simple_av_state.add_idx]) {
		free(simple_av_state.vkeys[simple_av_state.add_idx]);
		simple_av_state.vkeys[simple_av_state.add_idx] = NULL;
	}
	len = strnlen_s(key, 256);
	simple_av_state.vkeys[simple_av_state.add_idx] = (char*)malloc(len + 1);
	memcpy_s(simple_av_state.vkeys[simple_av_state.add_idx], len + 1, key, len + 1);

	simple_av_state.attr[simple_av_state.add_idx] |= attr;

	simple_av_state.add_idx = (simple_av_state.add_idx + 1) % MAX_FILE_LIST_SIZE;

	SDL_LockMutex(simple_av_state.start_mutex);
	SDL_CondSignal(simple_av_state.start_cond);
	SDL_UnlockMutex(simple_av_state.start_mutex);

	audio_pause(0);
	return 0;
}


void open_audio_device(int32_t out_sample_rate, int32_t out_channels, int32_t frame_size)
{
	//SDL------------------
#if USE_SDL_AUDIO
	SDL_AudioStatus  stat = SDL_GetAudioStatus();
	if (stat != SDL_AUDIO_STOPPED) {
		LOG_PLAYER("SDL close audio, reopen");

		SDL_LockAudio();
		SDL_CloseAudio();
		SDL_UnlockAudio();
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	}

	if (SDL_Init(SDL_INIT_AUDIO)) {
		LOG_PLAYER("open audio device, init sdl audio fail");
	}

	//nb_samples: AAC-1024or2048 MP3-1152
	int fill_nb_samples = frame_size;

	//SDL_AudioSpec
	SDL_AudioSpec& wanted_spec = simple_av_state.wanted_spec;
	wanted_spec.freq = out_sample_rate;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.channels = out_channels;
	wanted_spec.silence = 0;
	wanted_spec.samples = fill_nb_samples;
	wanted_spec.callback = fill_audio;
	wanted_spec.userdata = NULL;

	if (SDL_OpenAudio(&wanted_spec, NULL) < 0) {
		LOG_PLAYER("can't open audio.error:%s", SDL_GetError());
		return;
	}

	simple_av_state.audio_opened = true;
	//Play
	if (!simple_av_state.paused) {
		SDL_PauseAudio(0);
	}

	SDL_AudioStatus status = SDL_GetAudioStatus();
	LOG_PLAYER("audio device status: %d, pause status:%d", status, simple_av_state.paused);
#endif
}


int open_file_context(char* filename, int index)
{
	LOG_PLAYER("open filename:%s index=%d", filename, index);

	if (simple_av_state.pFormatCtx[index])
		avformat_close_input(&simple_av_state.pFormatCtx[index]);

	simple_av_state.pFormatCtx[index] = avformat_alloc_context();
	AVFormatContext* format_ctx = simple_av_state.pFormatCtx[index];
	if (avformat_open_input(&format_ctx, filename, NULL, NULL) != 0) {
		LOG_PLAYER("Couldn't open input stream.");
		return -1;
	}

	// Retrieve stream information
	if (avformat_find_stream_info(format_ctx, NULL) < 0) {
		LOG_PLAYER("Couldn't find stream information.");
		return -1;
	}
	// Dump valid information onto standard error
	av_dump_format(format_ctx, 0, filename, false);

	simple_av_state.audioStreamID[index] = simple_av_state.videoStreamID[index] = -1;
	for (unsigned int i = 0; i < format_ctx->nb_streams; i++){
		enum AVMediaType stream_type = format_ctx->streams[i]->codec->codec_type;
		if (stream_type == AVMEDIA_TYPE_AUDIO) {
			simple_av_state.audioStreamID[index] = i;
			break;
		}
	}

	for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
		enum AVMediaType stream_type = format_ctx->streams[i]->codec->codec_type;
		if (stream_type == AVMEDIA_TYPE_VIDEO)
		{
			simple_av_state.videoStreamID[index] = i;
			break;
		}
	}

	if (simple_av_state.audioStreamID[index] == -1 && simple_av_state.videoStreamID[index] == -1) {
		LOG_PLAYER("Didn't find a stream.");
		return -1;
	}


	// Find the decoder for the audio stream
	if (simple_av_state.audioStreamID[index] != -1) {
		AVStream* a_stream = format_ctx->streams[simple_av_state.audioStreamID[index]];
		simple_av_state.pAudioCodecCtx[index] = a_stream->codec;
		simple_av_state.audio_time_base[index] = a_stream->time_base;

		simple_av_state.pAudioCodec[index] = avcodec_find_decoder(simple_av_state.pAudioCodecCtx[index]->codec_id);
		if (simple_av_state.pAudioCodec[index] == NULL) {
			LOG_PLAYER("AudioCodec not found.");
			return -1;
		}

		if (avcodec_open2(simple_av_state.pAudioCodecCtx[index], simple_av_state.pAudioCodec[index], NULL) < 0) {
			LOG_PLAYER("Could not open audio codec.");
			return -1;
		}

		//FIX:Some Codec's Context Information is missing  
		int64_t	in_channel_layout = av_get_default_channel_layout(simple_av_state.pAudioCodecCtx[index]->channels);
		uint64_t out_channel_layout = in_channel_layout;
		AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
		int out_sample_rate = simple_av_state.pAudioCodecCtx[index]->sample_rate;

		//Swr 
		//sdl not support float fmt, and plannar
		simple_av_state.au_convert_ctx[index] = swr_alloc();
		simple_av_state.au_convert_ctx[index] = swr_alloc_set_opts(simple_av_state.au_convert_ctx[index], out_channel_layout, out_sample_fmt, out_sample_rate,
			in_channel_layout, simple_av_state.pAudioCodecCtx[index]->sample_fmt, simple_av_state.pAudioCodecCtx[index]->sample_rate, 0, NULL);
		swr_init(simple_av_state.au_convert_ctx[index]);
		LOG_PLAYER("open audio, out channels:%d, sample rate:%d, frame size:%d", simple_av_state.pAudioCodecCtx[index]->channels,
					out_sample_rate, simple_av_state.pAudioCodecCtx[index]->frame_size);

		simple_av_state.out_sample_rate = simple_av_state.pAudioCodecCtx[index]->sample_rate;
		simple_av_state.out_channels = simple_av_state.pAudioCodecCtx[index]->channels;
		simple_av_state.out_frame_size = simple_av_state.pAudioCodecCtx[index]->frame_size;

		if (simple_av_state.audio_opened == false) {
			open_audio_device(out_sample_rate, simple_av_state.out_channels, simple_av_state.out_frame_size);
		}
	}

	// Find the decoder for the video stream
	if (simple_av_state.videoStreamID[index] != -1) {
		AVStream*   v_stream = format_ctx->streams[simple_av_state.videoStreamID[index]];
		simple_av_state.pVideoCodecCtx[index] = v_stream->codec;
		simple_av_state.video_time_base[index] = v_stream->time_base;
		simple_av_state.duration[index] = av_q2d(v_stream->time_base) * 1000 * v_stream->duration;
		simple_av_state.duration_per_frame[index] = v_stream->duration / v_stream->nb_frames;

		simple_av_state.pVideoCodec[index] = avcodec_find_decoder(simple_av_state.pVideoCodecCtx[index]->codec_id);
		if (simple_av_state.pVideoCodec[index] == NULL) {
			LOG_PLAYER("VideoCodec not found.");
			return -1;
		}

		if (avcodec_open2(simple_av_state.pVideoCodecCtx[index], simple_av_state.pVideoCodec[index], NULL) < 0) {
			LOG_PLAYER("Could not open video codec.");
			return -1;
		}

		size_t	image_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, simple_av_state.pVideoCodecCtx[index]->width, simple_av_state.pVideoCodecCtx[index]->height, 1);
		simple_av_state.video_out_buffer[index] = (unsigned char *)av_malloc(image_size);
		av_image_fill_arrays(simple_av_state.pYUVFrame[index]->data, simple_av_state.pYUVFrame[index]->linesize, simple_av_state.video_out_buffer[index],
			AV_PIX_FMT_YUV420P, simple_av_state.pVideoCodecCtx[index]->width, simple_av_state.pVideoCodecCtx[index]->height, 1);

		simple_av_state.img_convert_ctx[index] = sws_getContext(simple_av_state.pVideoCodecCtx[index]->width, simple_av_state.pVideoCodecCtx[index]->height, 
			simple_av_state.pVideoCodecCtx[index]->pix_fmt, simple_av_state.pVideoCodecCtx[index]->width, 
			simple_av_state.pVideoCodecCtx[index]->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);

#if USE_SDL_VIDEO
		sdl_v_state.screen_w = simple_av_state.pVideoCodecCtx[index]->width;
		sdl_v_state.screen_h = simple_av_state.pVideoCodecCtx[index]->height;

		//if (first_open)
			init_sdl_render(simple_av_state.pVideoCodecCtx[index]->width, simple_av_state.pVideoCodecCtx[index]->height);
#endif
	}

	LOG_PLAYER("Success open file context");
	return 0;
}


int read_packet(void* param)
{
	LOG_PLAYER("read packet thread start, %d", quit);
	SimpleAVState& as = simple_av_state;
	
	AVPacket packet; 
	av_init_packet(&packet);

	while (!quit) {
		int fmt_ctx_idx = 0;
		bool need_open = true;
		LOG_PLAYER("read_packet loop ");
		for (int i = 0; simple_av_state.files[i] != NULL; i = (i+1) % MAX_FILE_LIST_SIZE) {
			simple_av_state.flag[fmt_ctx_idx] = simple_av_state.attr[i];
			simple_av_state.attr[i] = 0;

			char* filename = simple_av_state.files[i];
			int ret = 0;
			if (need_open) {
				ret = open_file_context(filename, fmt_ctx_idx);
			}
			else {
				need_open = true;
			}

			if (ret >= 0) {
				LOG_PLAYER("read frame, %d", fmt_ctx_idx);
				while (av_read_frame(as.pFormatCtx[fmt_ctx_idx], &packet) >= 0) {
					if (packet.stream_index == as.audioStreamID[fmt_ctx_idx]) {
						packet_queue_put(&audioq, &packet);
					}
					else if (packet.stream_index == as.videoStreamID[fmt_ctx_idx]) {
						packet_queue_put(&videoq, &packet);
					}
					else {
						av_packet_unref(&packet);
					}

					while (audioq.nb_packets >= MAX_AUDIO_PKT_QUEUE_SIZE && videoq.nb_packets >= MAX_VIDEO_PKT_QUEUE_SISE) {
						SDL_LockMutex(simple_av_state.wait_packet_mutex);
						simple_av_state.wait_packet = true;
						SDL_CondWaitTimeout(simple_av_state.wait_packet_cond, simple_av_state.wait_packet_mutex, 100);
						simple_av_state.wait_packet = false;
						SDL_UnlockMutex(simple_av_state.wait_packet_mutex);

						if (quit || simple_av_state.seeking)
							break;
					}

					if (simple_av_state.seeking) {
						LOG_PLAYER("seek in read packet");
						seek_internal(&fmt_ctx_idx, &i);
						simple_av_state.seeking = false;
					}

					if (quit)
						break;
				}

				ret = av_read_frame(as.pFormatCtx[fmt_ctx_idx], &packet);

				LOG_PLAYER("finish read one file! %s", filename);
				//av_init_packet(&packet);
				packet_queue_put(&audioq, &packet);
				packet_queue_put(&videoq, &packet);
			}

			fmt_ctx_idx = (fmt_ctx_idx + 1) % NB_CTX;

			while (simple_av_state.files[i + 1] == NULL) {
				if (quit)
					break;

				SDL_LockMutex(simple_av_state.start_mutex);
				SDL_CondWaitTimeout(simple_av_state.start_cond, simple_av_state.start_mutex, 300);
				SDL_UnlockMutex(simple_av_state.start_mutex);

				if (simple_av_state.seeking) {
					LOG_PLAYER("seek in wait file");
					if (seek_internal(&fmt_ctx_idx, &i) < 0) {
						need_open = false;
					}
					simple_av_state.seeking = false;
					break;
				}
			}
			if (quit)
				break;
		}
	}

 	av_packet_unref(&packet);
	simple_av_state.read_finished = true;
	return 0;
}

void audio_pause(int flag)
{
	LOG_PLAYER("Enter audio pause, flag:%d, pause:%d", flag, simple_av_state.paused);
	simple_av_state.flag[cur_v_ctx_idx] = 0;
	if (simple_av_state.paused) {
		if (0 == flag) {
			LOG_PLAYER("Restore audio");
			SDL_LockMutex(simple_av_state.pause_mutex);
			SDL_PauseAudio(0);
			simple_av_state.paused = false;

			simple_av_state.start_time = av_gettime();
			simple_av_state.flip_mode = false;
			SDL_CondSignal(simple_av_state.pause_cond);
			SDL_UnlockMutex(simple_av_state.pause_mutex);
		}
	}
	else {
		if (1 == flag) {
			LOG_PLAYER("Pause audio");
			SDL_LockMutex(simple_av_state.pause_mutex);
			SDL_PauseAudio(1);
			simple_av_state.paused = true;

			simple_av_state.sys_clock += av_gettime() - simple_av_state.start_time;
			SDL_UnlockMutex(simple_av_state.pause_mutex);
		}
	}
}


int process_video_thread_cmd(PLAYER_OP op, VideoThreadLocal* local)
{
	int ret = 0;
	switch (op)
	{
		case ENTER_BORDER:
		{
			//post a empty sample to video refresh thread
			av_sample_qnode*  node = (av_sample_qnode*)av_mallocz(sizeof(av_sample_qnode));
			node->av_sample.buff_size = -1;
			node->av_sample.buffer = NULL;
			q_push(&(node->list), &v_sample_q);
			simple_av_state.v_index[local->cur_v_codec_idx] = 0;
			if (simple_av_state.pVideoCodecCtx[local->cur_v_codec_idx]) {
				avcodec_close(simple_av_state.pVideoCodecCtx[local->cur_v_codec_idx]);
				simple_av_state.pVideoCodecCtx[local->cur_v_codec_idx] = NULL;
			}

			local->cur_v_codec_idx = (local->cur_v_codec_idx + 1) % NB_CTX;

			LOG_PLAYER("video decode packet, enter border");
			break;
		}
		case MULTIPLY_SPEED:
		{
			local->speed_multipier = simple_av_state.speed_multiplier;
			break;
		}
		case SEEK:
		{
			local->op = SEEK;
			if (local->cur_v_codec_idx != cur_v_ctx_idx) {
				local->cur_v_codec_idx = (local->cur_v_codec_idx - 1 + NB_CTX) % NB_CTX;
			}
			local->seek_v_ts = simple_av_state.seek_time * simple_av_state.video_time_base[local->cur_v_codec_idx].den / 1000.0;
			LOG_PLAYER("seek in video decode");
			break;
		}
		default:
			break;
	}
	return ret;
}


int video_decode_packet(void* param)
{
	VideoThreadLocal local;
	int ret = 0;
	while (!quit) {
		//start = SDL_GetPerformanceCounter();
		//start32 = SDL_GetTicks();

		int got_picture = -1;
		AVPacket packet;
		av_init_packet(&packet);

		do {
			if (videoq.nb_packets <= 0) {
				SDL_LockMutex(simple_av_state.wait_packet_mutex);
				if (simple_av_state.wait_packet)
					SDL_CondSignal(simple_av_state.wait_packet_cond);
				SDL_UnlockMutex(simple_av_state.wait_packet_mutex);
			}

			ret = packet_queue_get(&videoq, &packet, true);
			if ( ret < 0)
				return -1;

			if (packet.pos < 0) {
				process_video_thread_cmd((PLAYER_OP)packet.pos, &local);
				continue;
			}
			else {
				break;
			}
		} while (true);

		ret = avcodec_decode_video2(simple_av_state.pVideoCodecCtx[local.cur_v_codec_idx], simple_av_state.pVideoFrame, &got_picture, &packet);
		if (ret < 0) {
			LOG_PLAYER("Error in decoding video frame.");
			continue;
		}
		if (got_picture > 0) {
			if (local.op == SEEK) {
				if (packet.dts < local.seek_v_ts) {
					av_packet_unref(&packet);
					continue;
				}
				else {
					simple_av_state.cur_video_pts = packet.dts;
					simple_av_state.v_index[local.cur_v_codec_idx] = 0;
					local.op = NULL_OP;

					simple_av_state.start_time = av_gettime();
					simple_av_state.sys_clock = packet.pts * av_q2d(simple_av_state.video_time_base[local.cur_v_codec_idx]) * 1000000;

					//在视频之后才给音频线程发消息，因为视频从关键帧找到对应时间点，需要较长时间，这段时间内音视处于暂停状态
					AVPacket pkt;
					av_init_packet(&pkt);
					pkt.pos = (int64_t)SEEK;
					packet_queue_put_front(&audioq, &pkt);

					if (simple_av_state.paused) {
						SDL_LockMutex(simple_av_state.pause_mutex);
						SDL_PauseAudio(0);
						simple_av_state.paused = false;
						SDL_CondSignal(simple_av_state.pause_cond);
						SDL_UnlockMutex(simple_av_state.pause_mutex);
					}
				}
			}

			if (simple_av_state.img_convert_ctx) {
				sws_scale(simple_av_state.img_convert_ctx[local.cur_v_codec_idx], 
					(const uint8_t* const*)simple_av_state.pVideoFrame->data, simple_av_state.pVideoFrame->linesize,
					0, simple_av_state.pVideoCodecCtx[local.cur_v_codec_idx]->height, 
					simple_av_state.pYUVFrame[local.cur_v_codec_idx]->data, simple_av_state.pYUVFrame[local.cur_v_codec_idx]->linesize);
			}

#if 0
			LOG_PLAYER("Video index:%5d\t pts:%lld\t,  packet size:%d", simple_av_state.v_index[local.cur_v_codec_idx], packet.pts, packet.size);
#endif

			int y_size = simple_av_state.pVideoCodecCtx[local.cur_v_codec_idx]->width * simple_av_state.pVideoCodecCtx[local.cur_v_codec_idx]->height;
#if OUTPUT_PCM
			fwrite(simple_av_state.pYUVFrame[cur_v_idx]->data[0], 1, y_size, simple_av_state.pYUVFile);    //Y   
			fwrite(simple_av_state.pYUVFrame[cur_v_idx]->data[1], 1, y_size / 4, simple_av_state.pYUVFile);  //U  
			fwrite(simple_av_state.pYUVFrame[cur_v_idx]->data[2], 1, y_size / 4, simple_av_state.pYUVFile);  //V  
#endif

			av_sample_qnode*  node		= (av_sample_qnode*)av_mallocz(sizeof(av_sample_qnode));
			node->av_sample.width		= simple_av_state.pVideoCodecCtx[local.cur_v_codec_idx]->width;
			node->av_sample.height		= simple_av_state.pVideoCodecCtx[local.cur_v_codec_idx]->height;
			node->av_sample.buff_size	= (int32_t)node->av_sample.width * node->av_sample.height * 1.5;
			node->av_sample.buffer		= (uint8_t*)av_mallocz(node->av_sample.buff_size);
			node->av_sample.pts			= packet.dts;

			memcpy(node->av_sample.buffer, simple_av_state.pYUVFrame[local.cur_v_codec_idx]->data[0], y_size);
			memcpy(node->av_sample.buffer + y_size, simple_av_state.pYUVFrame[local.cur_v_codec_idx]->data[1], y_size / 4);
			memcpy(node->av_sample.buffer + y_size + y_size / 4, simple_av_state.pYUVFrame[local.cur_v_codec_idx]->data[2], y_size / 4);

			q_push(&(node->list), &v_sample_q);

			simple_av_state.v_index[local.cur_v_codec_idx]++;
			av_packet_unref(&packet);
		}
		else {
			LOG_PLAYER("Got picture fail");
			av_packet_unref(&packet);
			continue;
		}

		//now = SDL_GetPerformanceCounter();
		//now32 = SDL_GetTicks();
		//if (start > 0)
		//	LOG_PLAYER("Decdoe video time 1 second = %u ms in ticks, %f ms according to performance counter", 
		//		(now32 - start32), (double)((now - start) * 1000) / SDL_GetPerformanceFrequency());

		int32_t nb_frame = q_list_count(&v_sample_q);
		while (nb_frame > MAX_VIDEO_FRAME_QUEUE_SIZE) {
			if (quit)
				break;
			SDL_Delay(3 * MAX_VIDEO_FRAME_QUEUE_SIZE);

			nb_frame = q_list_count(&v_sample_q);
		}
	}
	return ret;
}


int64_t	get_v_ts(int index)
{
	int64_t	clock = (int64_t)simple_av_state.cur_video_pts * av_q2d(simple_av_state.video_time_base[index]) *1000;// +simple_av_state.v_last_ts;
	return clock;
}

int64_t get_a_ts(int index)
{
	int64_t clock = simple_av_state.cur_audio_pts * av_q2d(simple_av_state.audio_time_base[index]) * 1000;// + simple_av_state.a_last_ts;
	int32_t next = (cur_v_ctx_idx + 1) % NB_CTX;
	if (next == cur_a_ctx_idx)
		clock = INT64_MAX;
	return clock;
}

bool use_external_clock()
{
	SDL_AudioStatus status = SDL_GetAudioStatus();
	return SDL_AUDIO_STOPPED == status || !simple_av_state.audio_opened;
}


int64_t	get_ref_clock(int index)
{
	int64_t	clock = 0;
	
	if (use_external_clock()) {
		clock = (av_gettime() - simple_av_state.start_time + simple_av_state.sys_clock) / 1000;// +simple_av_state.a_last_ts;
	}
	else if (simple_av_state.flip_mode) {
		clock = get_v_ts(index);
	}
	else {
		clock = get_a_ts(index);
	}

	return clock;
}

void clear_sample_queue(q_head*  queue)
{
	if (NULL == queue)
		return;
	while (q_list_count(queue) > 0) {
		if (quit)
			return;

		struct list_head* ptr = q_pop(queue);
		if (!ptr) {
			break;
		}

		av_sample_qnode* pqnode = (av_sample_qnode *)list_entry(ptr, av_sample_qnode, list);
		AVSample_t sample = pqnode->av_sample;

		if (sample.buffer)
			av_free(sample.buffer);
		av_free(pqnode);
	}
}


void jump_audio_frame(int64_t ref, bool first_flag)
{
	int64_t audio_clock = get_a_ts(cur_a_ctx_idx);
	bool jump = first_flag;
	while (ref > audio_clock + 10 || jump) {
		if (quit)
			return;
		if (jump)
			if (cur_a_ctx_idx == cur_v_ctx_idx)
				return;

		struct list_head* ptr = q_pop(&a_sample_q);
		if (!ptr) {
			SDL_LockMutex(simple_av_state.wait_packet_mutex);
			if (simple_av_state.wait_packet) {
				LOG_PLAYER("audio sample queue empty, signal condition");
				SDL_CondSignal(simple_av_state.wait_packet_cond);
			}
			SDL_UnlockMutex(simple_av_state.wait_packet_mutex);

			break;
		}

		av_sample_qnode* pqnode = (av_sample_qnode *)list_entry(ptr, av_sample_qnode, list);
		AVSample_t sample = pqnode->av_sample;
		if (sample.buff_size == -1 && sample.buffer == NULL) {
			LOG_PLAYER("Jump, enter audio border, sample queue");

			jump = false;
			simple_av_state.a_last_ts = 1000 * av_q2d(simple_av_state.audio_time_base[cur_a_ctx_idx]) * simple_av_state.cur_audio_pts;
			cur_a_ctx_idx = (cur_a_ctx_idx + 1) % NB_CTX;
			simple_av_state.cur_audio_pts = 0;

			av_free(pqnode);
			continue;
		}

		simple_av_state.cur_audio_pts = sample.pts;
		audio_clock = get_a_ts(cur_a_ctx_idx);
		LOG_PLAYER("Jump, one sample");

		if (sample.buffer)
			av_free(sample.buffer);
		av_free(pqnode);
	}

	while (ref > audio_clock + 10 || jump) {
		if (quit)
			return;

		if (jump)
			if (cur_a_ctx_idx == cur_v_ctx_idx)
				return;

		AVPacket packet;
		av_init_packet(&packet);

		if (packet_queue_get(&audioq, &packet, false) <= 0) {
			break;
		}

		if (packet.pos == -1) {
			LOG_PLAYER("Jump, enter audio border, packet queue");

			jump = false;
			packet_queue_put_front(&audioq, &packet);
			av_packet_unref(&packet);
			break;
		}

		simple_av_state.cur_audio_pts = packet.pts;
		audio_clock = get_a_ts(cur_a_ctx_idx);
		LOG_PLAYER("Jump, one packet");

		av_packet_unref(&packet);
	}
}


void enter_video_border()
{
	// TODO
	double pos = simple_av_state.duration[cur_v_ctx_idx];// simple_av_state.pFormatCtx[cur_v_ctx_idx]->streams[simple_av_state.videoStreamID[cur_v_ctx_idx]]->duration * / AV_TIME_BASE;// *1000;

	

	//std::cout << "cureent time :" << pos << std::endl;

	LOG_PLAYER("[enter_video_border] current files idex=%d", simple_av_state.cur_file_idx);
	if (simple_av_state.files[simple_av_state.cur_file_idx + 1] == NULL) {
		LOG_PLAYER("[enter_video_border] next files null idex=%d", simple_av_state.cur_file_idx + 1);
		audio_pause(1);
	}



	FileFinishCb file_finish = simple_av_state.file_finish_cb;
	if (file_finish) {
		file_finish((void*)simple_av_state.vkeys[simple_av_state.cur_file_idx]);
	}

	simple_av_state.v_last_ts = 1000 * av_q2d(simple_av_state.video_time_base[cur_v_ctx_idx]) * simple_av_state.cur_video_pts;
	cur_v_ctx_idx = (cur_v_ctx_idx + 1) % NB_CTX;
	simple_av_state.cur_video_pts = 0;
	simple_av_state.cur_file_idx = (simple_av_state.cur_file_idx + 1) % MAX_FILE_LIST_SIZE;

	SDL_LockMutex(simple_av_state.pause_mutex);
	simple_av_state.start_time = av_gettime();
	simple_av_state.sys_clock = 0;
	SDL_UnlockMutex(simple_av_state.pause_mutex);
}


void jump_video_frame(int32_t frame_nb, bool* first_frame)
{
	for (int i = 0; i < frame_nb; i++) {
		if (quit)
			return;

		struct list_head* ptr = q_pop(&v_sample_q);
		if (!ptr) {
			SDL_LockMutex(simple_av_state.wait_packet_mutex);
			if (simple_av_state.wait_packet)
				SDL_CondSignal(simple_av_state.wait_packet_cond);
			SDL_UnlockMutex(simple_av_state.wait_packet_mutex);

			break;
		}

		av_sample_qnode* pqnode = (av_sample_qnode *)list_entry(ptr, av_sample_qnode, list);
		AVSample_t sample = pqnode->av_sample;
		if (sample.buff_size == -1 && sample.buffer == NULL) {
			LOG_PLAYER("Jump, enter video border");
			*first_frame = true;
			enter_video_border();

			av_free(pqnode);
			i--;
			continue;
		}

		if (sample.buffer)
			av_free(sample.buffer);
		av_free(pqnode);

		simple_av_state.cur_video_pts = sample.pts;
		if (*first_frame) {
			*first_frame = false;
		}
	}
}


void enter_flip_mode(int frames)
{
	if (!simple_av_state.flip_mode || (simple_av_state.flip_mode  && simple_av_state.paused)) {
		simple_av_state.flip_mode_status = FLIP_READY;
		simple_av_state.flip_nb = frames;

		SDL_LockMutex(simple_av_state.pause_mutex);
		simple_av_state.flip_mode = true;
		LOG_PLAYER("enter flip mode");
		if (simple_av_state.paused) {
			LOG_PLAYER("enter flip mode when paused");
			SDL_CondSignal(simple_av_state.pause_cond);
			simple_av_state.paused = false;
			SDL_PauseAudio(0);
		}

		SDL_UnlockMutex(simple_av_state.pause_mutex);
	}
	else
	{
		LOG_PLAYER("fliping , ingore this request");
	}
}


int video_refresh(void* param)
{
	LOG_PLAYER("video refresh thread");

	bool first_frame = true;
	while (!quit) {
		SDL_Event event;
		int32_t pending = SDL_PollEvent(&event);

		if (pending) {
			LOG_PLAYER("sdl poll event, type:%x", event.type);
			switch (event.type) 
			{
				case SDL_AUDIODEVICEADDED: 
				{
					LOG_PLAYER("sdl event, audio device added");
					if (simple_av_state.audio_opened  == false && simple_av_state.out_sample_rate > 0)
					{
						LOG_PLAYER("sdl event, audio device added");
						open_audio_device(simple_av_state.out_sample_rate, simple_av_state.out_channels, simple_av_state.out_frame_size);
					}
					break;
				}
				case SDL_AUDIODEVICEREMOVED:
				{
					LOG_PLAYER("sdl event, audio device removed");
					simple_av_state.audio_opened = false;
					SDL_LockAudio();
					SDL_CloseAudio();
					SDL_UnlockAudio();
					SDL_QuitSubSystem(SDL_INIT_AUDIO);

					simple_av_state.sys_clock = (int64_t)simple_av_state.cur_video_pts * av_q2d(simple_av_state.video_time_base[cur_v_ctx_idx]) * 1000000;
					simple_av_state.start_time = av_gettime();
					LOG_PLAYER("sys clock:%lld, pts:%lld", simple_av_state.sys_clock, simple_av_state.cur_video_pts);
					break;
				}
				default:
					break;
			}
		}

		if (simple_av_state.ready)
		{			
			SDL_LockMutex(simple_av_state.pause_mutex);
			if (simple_av_state.paused) {
				LOG_PLAYER("Video refresh thread, pause wait");
				if (simple_av_state.flip_mode && simple_av_state.flip_mode_status == FLIP_FRAME_OVER)
					simple_av_state.flip_mode_status = FLIP_PAUSE;
				SDL_CondWait(simple_av_state.pause_cond, simple_av_state.pause_mutex);
				simple_av_state.paused = false;
				LOG_PLAYER("Video refresh thread, wake up, cond");
			}
			SDL_UnlockMutex(simple_av_state.pause_mutex);

			if (simple_av_state.flip_mode && simple_av_state.flip_mode_status == FLIP_READY) {
				simple_av_state.flip_mode_status = FLIP_JUMP_FRAME;
				LOG_PLAYER("Flip mode, jump %d frames", simple_av_state.flip_nb);
				jump_video_frame(simple_av_state.flip_nb, &first_frame);
				simple_av_state.sys_clock = simple_av_state.cur_video_pts * av_q2d(simple_av_state.video_time_base[cur_v_ctx_idx]) * 1000000;
				simple_av_state.start_time = av_gettime();
			}

			struct list_head* ptr = q_pop(&v_sample_q);
			if (!ptr)
			{
				SDL_LockMutex(simple_av_state.wait_packet_mutex);
				if (simple_av_state.wait_packet)
					SDL_CondSignal(simple_av_state.wait_packet_cond);
				SDL_UnlockMutex(simple_av_state.wait_packet_mutex);

				SDL_Delay(5);

				continue;
			}

			av_sample_qnode* pqnode = (av_sample_qnode *)list_entry(ptr, av_sample_qnode, list);
			AVSample_t sample = pqnode->av_sample;
			int64_t	ref_clock = get_ref_clock(cur_a_ctx_idx);
			if (sample.buff_size == -1 && sample.buffer == NULL) {
				LOG_PLAYER("Enter video border");
				first_frame = true;

				enter_video_border();
				jump_audio_frame(ref_clock, first_frame);

				av_free(pqnode);
				continue;
			}
			
			simple_av_state.cur_video_pts = sample.pts;


			ref_clock = get_ref_clock(cur_a_ctx_idx);
			if (use_external_clock() || simple_av_state.flip_mode)
				jump_audio_frame(ref_clock, first_frame);

			if (first_frame) {
				first_frame = false;
				simple_av_state.start_time = av_gettime();

				if (simple_av_state.flag[cur_v_ctx_idx]) {
					//LOG_PLAYER("[video_refresh] flag true idex=%d", simple_av_state.cur_file_idx);
					audio_pause(1);
					simple_av_state.flag[cur_v_ctx_idx] = 0;
				}
			}

			ref_clock = get_ref_clock(cur_a_ctx_idx);
			int64_t	v_ts = get_v_ts(cur_v_ctx_idx);
			//LOG_PLAYER("TS %5lld ms, ref_clock: %5lld,  video_ts: %5lld", v_ts - ref_clock, ref_clock, v_ts);
			if (v_ts > ref_clock + 5 && !(simple_av_state.flip_mode && quit && simple_av_state.paused)) {
				int32_t  sleep_time = v_ts - ref_clock;
				//LOG_PLAYER("Sleep %5lld ms, ref_clock: %5lld,  video_ts: %5lld", sleep_time, ref_clock, v_ts);
				if (sleep_time > 50)
					SDL_Delay(50);
				else if (sleep_time > 5)
					SDL_Delay(sleep_time);
			}

#if USE_SDL_VIDEO
			//SDL_UpdateYUVTexture(sdl_v_state.texture, NULL,
			//	simple_av_state.pYUVFrame->data[0], simple_av_state.pYUVFrame->linesize[0],
			//	simple_av_state.pYUVFrame->data[1], simple_av_state.pYUVFrame->linesize[1],
			//	simple_av_state.pYUVFrame->data[2], simple_av_state.pYUVFrame->linesize[2]);

			//FIX: If window is resize  
			//TODO
			//double pos = simple_av_state.cur_video_pts * av_q2d(simple_av_state.audio_time_base[cur_v_ctx_idx]) * 1000;

			//std::cout << "cureent time :" << pos << std::endl;

			sdl_v_state.rect.x = 0;
			sdl_v_state.rect.y = 0;
			sdl_v_state.rect.w = sdl_v_state.screen_w;
			sdl_v_state.rect.h = sdl_v_state.screen_h;

			SDL_UpdateTexture(sdl_v_state.texture, &sdl_v_state.rect, sample.buffer, sample.width);

			SDL_RenderClear(sdl_v_state.renderer);
			SDL_RenderCopy(sdl_v_state.renderer, sdl_v_state.texture, NULL, &sdl_v_state.rect);
			SDL_RenderPresent(sdl_v_state.renderer);
#else
			//show 
			//playNextFrame(sample.buffer, sample.width, sample.height);
#endif
			if (simple_av_state.flip_mode && simple_av_state.flip_mode_status == FLIP_JUMP_FRAME) {
				SDL_LockMutex(simple_av_state.pause_mutex);
				LOG_PLAYER("Flip mode, refresh frame over");
				simple_av_state.flip_mode_status = FLIP_FRAME_OVER;
				simple_av_state.flip_nb = 0;
				simple_av_state.paused = true;
				SDL_PauseAudio(1);
				SDL_UnlockMutex(simple_av_state.pause_mutex);
			}
			SDL_Delay(1);

			av_free(pqnode);
			av_free(sample.buffer);
		}
	}

	return 0;
}


void start_play()
{
	LOG_PLAYER("start_play");
	thread_ids[READ_THREAD] = SDL_CreateThread(read_packet, "audio play thread", NULL);

	thread_ids[REFRESH_THREAD] = SDL_CreateThread(video_refresh, "video fresh thread", NULL);

	thread_ids[VIDEO_THREAD] = SDL_CreateThread(video_decode_packet, "video decode thread", NULL);

	thread_ids[AUDIO_THREAD] = SDL_CreateThread(audio_decode_loop, "audio decode thread", NULL);
}

double get_time_pos_in_file(std::string& key_out)
{
	double pos = 0; 
	if (simple_av_state.vkeys[simple_av_state.cur_file_idx]) {
		if (!simple_av_state.seeking) {
			pos = simple_av_state.cur_video_pts * av_q2d(simple_av_state.video_time_base[cur_v_ctx_idx]) * 1000;
		}
		else {
			pos = simple_av_state.seek_time;
		}
		key_out = simple_av_state.vkeys[simple_av_state.cur_file_idx];
	}
	return pos;
}


void seek_in_file(int ts)
{
	LOG_PLAYER("seek pos %d", ts);

	SDL_LockMutex(simple_av_state.wait_packet_mutex);
	if (simple_av_state.wait_packet) {
		SDL_CondSignal(simple_av_state.wait_packet_cond);
	}
	simple_av_state.flip_mode = false;
	simple_av_state.seeking = true;;
	simple_av_state.seek_time = ts;
	SDL_UnlockMutex(simple_av_state.wait_packet_mutex);

	SDL_LockMutex(simple_av_state.pause_mutex);
	if (!simple_av_state.paused) {
		SDL_PauseAudio(1);
		simple_av_state.paused = true;
	}
	SDL_UnlockMutex(simple_av_state.pause_mutex);
}


int seek_internal(int32_t* p_ctx_idx, int32_t* file_idx)
{
	int ret = 1;
	LOG_PLAYER("seek begin");

	clear_sample_queue(&a_sample_q);
	clear_sample_queue(&v_sample_q);

	packet_queue_flush(&audioq);
	packet_queue_flush(&videoq);

	if (*p_ctx_idx != cur_v_ctx_idx) {
		*p_ctx_idx = (*p_ctx_idx - 1 + NB_CTX) % NB_CTX;
		//for语句第三段会对*file_idx做++操作
		*file_idx = (*file_idx - 1) % MAX_FILE_LIST_SIZE;
		ret = -1;
	}

	int64_t seek_v_ts = simple_av_state.seek_time * simple_av_state.video_time_base[*p_ctx_idx].den / 1000.0;
	int64_t seek_a_ts = simple_av_state.seek_time * simple_av_state.audio_time_base[*p_ctx_idx].den / 1000.0;
	av_seek_frame(simple_av_state.pFormatCtx[*p_ctx_idx], simple_av_state.audioStreamID[*p_ctx_idx], seek_a_ts, AVSEEK_FLAG_BACKWARD);
	av_seek_frame(simple_av_state.pFormatCtx[*p_ctx_idx], simple_av_state.videoStreamID[*p_ctx_idx], seek_v_ts, AVSEEK_FLAG_BACKWARD);

	simple_av_state.cur_video_pts = seek_v_ts;
	simple_av_state.cur_audio_pts = seek_a_ts;

	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.pos = (int64_t)SEEK;
	packet_queue_put(&videoq, &pkt);

	LOG_PLAYER("seek over",);
	return ret;
}

int get_cur_file_duration()
{
	return simple_av_state.duration[cur_v_ctx_idx];
}

bool getCurrentState()
{
	return simple_av_state.paused;
}


void set_speed_multiplier(int multiplier)
{
	LOG_PLAYER("set speed multiplier");
	simple_av_state.speed_multiplier = multiplier;

	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.pos = (int64_t)MULTIPLY_SPEED;
	packet_queue_put_front(&audioq, &pkt);
}


void set_audio_volume(double vol)
{
	LOG_PLAYER("set audio volume:%f", vol);
	simple_av_state.volume_audio = vol;
}


void uninit()
{
	if (1 == quit)
		return;
	quit = 1;

	LOG_PLAYER("Uninit, wait thread exit");
	
	SDL_CondBroadcast(simple_av_state.start_cond);
	SDL_CondBroadcast(simple_av_state.pause_cond);
	SDL_CondBroadcast(simple_av_state.wait_packet_cond);
	SDL_CondBroadcast(videoq.cond);
	SDL_CondBroadcast(audioq.cond);

	for (int i = 0; i < THREAD_NUM; i++) {
		int ret = -1;
		if (thread_ids[i]) {
			SDL_WaitThread(thread_ids[i], &ret);
			thread_ids[i] = NULL;
		}
	}

	for (int i = 0; simple_av_state.files[i] != NULL; i++) {
		free(simple_av_state.files[i]);
	}
	free(simple_av_state.files);
	simple_av_state.files = NULL;

	for (int i = 0; simple_av_state.vkeys[i] != NULL; i++) {
		free(simple_av_state.vkeys[i]);
	}
	free(simple_av_state.vkeys);
	simple_av_state.vkeys = NULL;

	SDL_DestroyCond(simple_av_state.start_cond);
	SDL_DestroyMutex(simple_av_state.start_mutex);

	SDL_DestroyCond(simple_av_state.pause_cond);
	SDL_DestroyMutex(simple_av_state.pause_mutex);

	SDL_DestroyCond(simple_av_state.wait_packet_cond);
	SDL_DestroyMutex(simple_av_state.wait_packet_mutex);

#if USE_SDL_AUDIO
	SDL_CloseAudio();//Close SDL
	SDL_Quit();
#endif

#if OUTPUT_PCM
	if (simple_av_state.pPCMFile) {
		fclose(simple_av_state.pPCMFile);
		simple_av_state.pPCMFile = NULL;
	}
	if (simple_av_state.pYUVFile) {
		fclose(simple_av_state.pYUVFile);
		simple_av_state.pYUVFile = NULL;
	}
#endif
	if (simple_av_state.audio_out_buffer)
		av_free(simple_av_state.audio_out_buffer);
	if (simple_av_state.pVideoFrame)
		av_frame_unref(simple_av_state.pVideoFrame);
	if (simple_av_state.pAudioFrame)
		av_frame_unref(simple_av_state.pAudioFrame);

	q_destroy(&v_sample_q);
	for (int i = 0; i < NB_CTX; i++) {
		if (simple_av_state.video_out_buffer[i]) {
			av_free(simple_av_state.video_out_buffer[i]);
			simple_av_state.video_out_buffer[i] = NULL;
		}

		if (simple_av_state.pAudioCodecCtx[i]) {
			avcodec_close(simple_av_state.pAudioCodecCtx[i]);
			simple_av_state.pAudioCodecCtx[i] = NULL;
		}

		if (simple_av_state.pYUVFrame[i]) {
			av_frame_unref(simple_av_state.pYUVFrame[i]);
			simple_av_state.pYUVFrame[i] = NULL;
		}

		if (simple_av_state.pVideoCodecCtx[i]) {
			avcodec_close(simple_av_state.pVideoCodecCtx[i]);
			simple_av_state.pVideoCodecCtx[i] = NULL;
		}
		 
		if (simple_av_state.pFormatCtx[i]) {
			avformat_close_input(&simple_av_state.pFormatCtx[i]);
			simple_av_state.pFormatCtx[i] = NULL;
		}
	}

	LOG_PLAYER("Over");
}
int i = 1;
typedef struct A{
	int i;
} A;

struct A a;

int main(int argc, char* argv[])
{

	init(NULL);

	add_file("H:\\11-2-1.mp4", "1", 0);
	start_play();

	a.i = 1;

	std::cout << [=]() {
		a.i = 2;
		return std::abs(a.i);
	} ();
	std::cout << a.i;
	system("pause");
	return 0;
}