#include <stdio.h>
#include <stdlib.h>
#include <iostream>

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
#include "libavformat/avio.h"
#include "libavutil/buffer.h"
#include "libavutil/error.h"
#include "libavutil/hwcontext.h"
//#include "libavutil/hwcontext_qsv.h"
#include "libavutil/mem.h"

#include "SDL.h"
}


//Refresh Event
#define SFM_REFRESH_EVENT  (SDL_USEREVENT + 1)

#define SFM_BREAK_EVENT  (SDL_USEREVENT + 2)

int thread_exit = 0;
int thread_pause = 0;



int sfp_refresh_thread(void *opaque)
{
	thread_exit = 0;
	thread_pause = 0;
	while (!thread_exit)
	{
		if (!thread_pause)
		{
			SDL_Event event;
			event.type = SFM_REFRESH_EVENT;
			SDL_PushEvent(&event);
		}
		SDL_Delay(40);
	}

	thread_exit = 0;
	thread_pause = 0;
	SDL_Event event;
	event.type = SFM_BREAK_EVENT;
	SDL_PushEvent(&event);
	return 0;
}



//int main(int argc, char* argv[])
//{
//
//	AVFormatContext* pFormatCtx;
//	int i, videoIndex;
//	AVCodecContext* pCodecCtx;
//	AVCodec* pCodec;
//	AVFrame* pFrame, *pFrameYUV;
//	unsigned char* out_buffer;
//	AVPacket* packet;
//	int y_size;
//	int ret = -1, got_picture;
//	struct SwsContext* img_convert_ctx;
//	char filepath[] = "v1-2.mp4";
//	int screen_w = 0;
//	int screen_h = 0;
//	SDL_Window* screen;
//	SDL_Renderer* sdlRenderer;
//	SDL_Texture* sdlTexture;
//	SDL_Rect sdlRect;
//
//	SDL_Thread *video_tid;
//	SDL_Event event;
//
//	av_register_all();
//	avformat_network_init();
//
//
//	pFormatCtx = avformat_alloc_context();
//
//	if (avformat_open_input(&pFormatCtx, filepath, NULL, NULL) != 0)
//	{
//		std::cout << "Coudldn't open input stream." << std::endl;
//		return -1;
//	}
//
//	ret = avformat_find_stream_info(pFormatCtx, NULL);
//	if (ret < 0)
//	{
//		std::cout << "Counldn't find stream information." << std::endl;
//		return -1;
//	}
//
//	videoIndex = -1;
//
//	AVRational avRational;
//	for (i = 0; i < pFormatCtx->nb_streams; ++i)
//	{
//		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
//		{
//			avRational = pFormatCtx->streams[i]->time_base;
//			videoIndex = i;
//
//			AVStream *videoStream = pFormatCtx->streams[i];
//			int den = videoStream->time_base.den;
//			int num = videoStream->time_base.num;
//			printf("\nfmtContext->duration = %lld, stream->duration = %lld\n",
//				pFormatCtx->duration == AV_NOPTS_VALUE ? -1 : pFormatCtx->duration / AV_TIME_BASE,
//				videoStream->duration == AV_NOPTS_VALUE ? -1 : videoStream->duration);
//			//printf("\nstream->numerator = %d, stream->denominator = %d\n", num, den);
//			if (pFormatCtx->duration != AV_NOPTS_VALUE)
//			{
//				printf("\nfmtContext->duration = %lf, fmtContext->duration = %d(s)%3d(ms)\n",
//					pFormatCtx->duration * av_q2d(avRational) * 1000,
//					(int)(pFormatCtx->duration * av_q2d(avRational) / 1000),
//					((int)(pFormatCtx->duration * av_q2d(avRational))) % 1000);
//			}
//			else 
//			{
//				printf("\nstream->duration = %lf\n", videoStream->duration * av_q2d(avRational));
//			}
//			printf("\nstream->duration = %lf\n", videoStream->duration * av_q2d(videoStream->time_base) * 1000);
//			
//		}
//		else if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
//		{
//			AVStream* audioStream = pFormatCtx->streams[i];
//			printf("audioStream->duration = %lf\n", audioStream->duration * av_q2d(audioStream->time_base) * 1000);
//		}
//	}
//
//	if (videoIndex == -1)
//	{
//		std::cout << "didn't find a video stream." << std::endl;
//		return -1;
//	}
//
//	pCodecCtx = pFormatCtx->streams[videoIndex]->codec;
//	pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
//	if (pCodec == NULL)
//	{
//		std::cout << "codec not found." << std::endl;
//		return -1;
//	}
//
//	if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
//	{
//		std::cout << "could not open codec." << std::endl;
//		return -1;
//	}
//
//	pFrame = av_frame_alloc();
//	pFrameYUV = av_frame_alloc();
//
//
//	out_buffer = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 1));
//	av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, out_buffer, AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 1);
//
//	packet = (AVPacket*)av_malloc(sizeof(AVPacket));
//
//	std::cout << "==================File Information==========================" << std::endl;
//	av_dump_format(pFormatCtx, 0, filepath, 0);
//
//	std::cout << "-------------------------------------------------" << std::endl;
//	img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
//		pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
//
//
//	if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_TIMER))
//	{
//		std::cout << "Could not initialize SDL" << std::endl;
//		return -1;
//	}
//
//	screen_w = pCodecCtx->width;
//	screen_h = pCodecCtx->height;
//
//	screen = SDL_CreateWindow("ffplayer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, screen_w, screen_h, SDL_WINDOW_OPENGL);
//
//	if (!screen)
//	{
//		printf("SDL: could not create window - exiting:%s\n", SDL_GetError());
//		return -1;
//	}
//
//	sdlRenderer = SDL_CreateRenderer(screen, -1, 0);
//
//	sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, pCodecCtx->width, pCodecCtx->height);
//
//	sdlRect.x = 0;
//	sdlRect.y = 0;
//	sdlRect.w = screen_w;
//	sdlRect.h = screen_h;
//
//
//	video_tid = SDL_CreateThread(sfp_refresh_thread, NULL, NULL);
//
//	for (;;)
//	{
//		SDL_WaitEvent(&event);
//		if (event.type == SFM_REFRESH_EVENT)
//		{
//			while ((ret = av_read_frame(pFormatCtx, packet)) >= 0)
//			{
//				/*ret = av_read_frame(pFormatCtx, packet);
//				if (ret < 0)
//				{
//					printf("av_read_frame %d\n", ret);
//					break;
//				}*/
//				if (packet->stream_index == videoIndex)
//				{
//					ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, packet);
//					if (ret < 0) {
//						printf("Decode Error.\n");
//						return -1;
//					}
//					if (got_picture)
//					{
//						sws_scale(img_convert_ctx, (const unsigned char* const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height,
//							pFrameYUV->data, pFrameYUV->linesize);
//
//						std::cout << "current time:" <<pFrame->pts * av_q2d(avRational) * 1000 << std::endl;
//
//						SDL_UpdateYUVTexture(sdlTexture, &sdlRect,
//							pFrameYUV->data[0], pFrameYUV->linesize[0],
//							pFrameYUV->data[1], pFrameYUV->linesize[1],
//							pFrameYUV->data[2], pFrameYUV->linesize[2]);
//
//						SDL_RenderClear(sdlRenderer);
//						SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &sdlRect);
//						SDL_RenderPresent(sdlRenderer);
//						SDL_Delay(40);
//					}
//				}
//
//				av_free_packet(packet);
//			}
//		}
//		else if (event.type == SDL_KEYDOWN) 
//		{
//			//Pause
//			if (event.key.keysym.sym == SDLK_SPACE)
//				thread_pause = !thread_pause;
//		}
//		else if (event.type == SDL_QUIT)
//		{
//			thread_exit = 1;
//		}
//		else if (event.type == SFM_BREAK_EVENT)
//		{
//			break;
//		}
//	}
//
//
//	while(1)
//	{
//		ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, packet);
//		if (ret < 0)
//			break;
//		if (!got_picture)
//			break;
//
//		sws_scale(img_convert_ctx, (const unsigned char* const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height,
//			pFrameYUV->data, pFrameYUV->linesize);
//		std::cout << "current time:" << pFrame->pts * av_q2d(avRational) * 1000 << std::endl;
//		//SDL---------------------------
//		SDL_UpdateTexture(sdlTexture, &sdlRect, pFrameYUV->data[0], pFrameYUV->linesize[0]);
//		SDL_RenderClear(sdlRenderer);
//		SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &sdlRect);
//		SDL_RenderPresent(sdlRenderer);
//		//SDL End-----------------------
//		//Delay 40ms
//		SDL_Delay(40);
//
//	}
//
//
//	sws_freeContext(img_convert_ctx);
//	SDL_Quit();
//	av_frame_free(&pFrameYUV);
//	av_frame_free(&pFrame);
//	avcodec_close(pCodecCtx);
//	avformat_close_input(&pFormatCtx);
//
//	system("pause");
//	return 0;
//}