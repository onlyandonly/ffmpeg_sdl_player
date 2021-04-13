#include "tutorial02.h"
#include <iostream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <SDL.h>
#include <SDL_thread.h>
}

using namespace std;

const string kMediaFile = "../../../assets/green.mp4";

/*
* 大致流程：
* 读取视频信息=》读取编码信息=》找到解码器=》初始化AVCodecContext==》
*															         =》读取视频流=》解码成YUV=》渲染
* 初始化SDL_Window=》初始化SDL_Renderer=》初始化SDL_Texture========》
*/
int tutorial02() {
	cout << "job's started." << endl;

	SDL_Window* pScreen;
	SDL_Renderer* pRenderer;
	SDL_Texture* pTexture;

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		cout << "SDL_Init failed: " << SDL_GetError() << endl;
		return -1;
	}

	AVFormatContext* pFormatCtx = nullptr;
	AVCodecContext* pVideoCodecCtx = nullptr;
	AVCodecParameters* pVideoCodecPar = nullptr;
	AVCodec* pVideoCodec = nullptr;
	AVFrame* pFrame = nullptr;
	AVPacket packet;
	uint8_t* data[4] = { nullptr };
	int linesizes[4] = { 0 };
	int iVideoStream = -1;

	//初始化不用啦，直接open即可
	//av_register_all();

	//读取avformat
	if (avformat_open_input(&pFormatCtx, kMediaFile.c_str(), nullptr, nullptr) < 0) {
		cout << "avformat_open_input failed" << endl;
		return -1;
	}

	//读取流信息，该函数会填充pFormatCtx->streams
	if (avformat_find_stream_info(pFormatCtx, nullptr) < 0) {
		cout << "avformat_find_stream_info failed" << endl;
		return -1;
	}

	//dump格式信息
	av_dump_format(pFormatCtx, 0, kMediaFile.c_str(), 0);

	//找到视频流编码信息
	for (unsigned i = 0; i < pFormatCtx->nb_streams; ++i) {
		if (iVideoStream == -1 && pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			iVideoStream = i;
			break;
		}
	}
	if (iVideoStream == -1) {
		cout << "couldn't find video stream" << endl;
		return -1;
	}

	{
		//AVStream.codec 被替换为 AVStream.codecpar
		//详见https://lists.libav.org/pipermail/libav-commits/2016-February/018031.html
		pVideoCodecPar = pFormatCtx->streams[iVideoStream]->codecpar;

		//找到对应的decoder
		pVideoCodec = avcodec_find_decoder(pVideoCodecPar->codec_id);
		if (pVideoCodec == nullptr) {
			cout << "avcodec_find_decoder failed" << endl;
			return -1;
		}

		//avcodec_open2前必须使用avcodec_alloc_context3生成context
		pVideoCodecCtx = avcodec_alloc_context3(pVideoCodec);
		if (avcodec_parameters_to_context(pVideoCodecCtx, pVideoCodecPar) < 0) {
			cout << "avcodec_parameters_to_context failed" << endl;
			return -1;
		}

		//使用pVideoCodec初始化pVideoCodecCtx
		if (avcodec_open2(pVideoCodecCtx, pVideoCodec, nullptr) < 0) {
			cout << "avcodec_open2 failed" << endl;
			return -1;
		}

		//pFrame用于接收解码后的YUV数据
		pFrame = av_frame_alloc();
		if (pFrame == nullptr) {
			cout << "av_frame_alloc failed" << endl;
			return -1;
		}

		//创建窗口
		pScreen = SDL_CreateWindow("SDLPlayer",
			SDL_WINDOWPOS_UNDEFINED,
			SDL_WINDOWPOS_UNDEFINED,
			pVideoCodecCtx->width,
			pVideoCodecCtx->height,
			0);
		if (pScreen == nullptr) {
			cout << "SDL_CreateWindow failed:" << SDL_GetError() << endl;
			return -1;
		}

		//创建渲染Context
		pRenderer = SDL_CreateRenderer(pScreen, -1, 0);
		if (pRenderer == nullptr) {
			cout << "SDL_CreateRenderer failed:" << SDL_GetError() << endl;
			return -1;
		}

		//初始化成白色
		SDL_SetRenderDrawColor(pRenderer, 255, 255, 255, 255);
		SDL_RenderClear(pRenderer);
		SDL_RenderPresent(pRenderer);

		//创建Render所需的Texture，用于放置YUV数据
		pTexture = SDL_CreateTexture(pRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, pVideoCodecCtx->width, pVideoCodecCtx->height);
		if (pTexture == nullptr) {
			cout << "SDL_CreateTexture failed:" << SDL_GetError() << endl;
			return -1;
		}
	}

	int count = 0;
	//读取视频包并解码，转换，保存文件
	while (av_read_frame(pFormatCtx, &packet) >= 0) {
		if (packet.stream_index == iVideoStream) {
			avcodec_send_packet(pVideoCodecCtx, &packet);
			if (avcodec_receive_frame(pVideoCodecCtx, pFrame) == 0) {

				//将YUV更新至texture,然后渲染
				SDL_UpdateYUVTexture(pTexture, NULL,
					pFrame->data[0], pFrame->linesize[0],
					pFrame->data[1], pFrame->linesize[1],
					pFrame->data[2], pFrame->linesize[2]);
				SDL_RenderClear(pRenderer);
				SDL_RenderCopy(pRenderer, pTexture, NULL, NULL);
				SDL_RenderPresent(pRenderer);
				cout << "presented frame" << ++count << endl;
			}
			//取消packet引用的内存，原先的av_free_packet弃用
			av_packet_unref(&packet);
		}
	}

	SDL_DestroyTexture(pTexture);
	SDL_DestroyRenderer(pRenderer);
	SDL_DestroyWindow(pScreen);
	SDL_Quit();

	av_freep(data);
	av_free(pFrame);

	avcodec_close(pVideoCodecCtx);

	avformat_close_input(&pFormatCtx);

	cout << "job's done" << endl;

	return 0;
}