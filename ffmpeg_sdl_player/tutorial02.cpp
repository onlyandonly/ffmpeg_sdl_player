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
* �������̣�
* ��ȡ��Ƶ��Ϣ=����ȡ������Ϣ=���ҵ�������=����ʼ��AVCodecContext==��
*															         =����ȡ��Ƶ��=�������YUV=����Ⱦ
* ��ʼ��SDL_Window=����ʼ��SDL_Renderer=����ʼ��SDL_Texture========��
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

	//��ʼ����������ֱ��open����
	//av_register_all();

	//��ȡavformat
	if (avformat_open_input(&pFormatCtx, kMediaFile.c_str(), nullptr, nullptr) < 0) {
		cout << "avformat_open_input failed" << endl;
		return -1;
	}

	//��ȡ����Ϣ���ú��������pFormatCtx->streams
	if (avformat_find_stream_info(pFormatCtx, nullptr) < 0) {
		cout << "avformat_find_stream_info failed" << endl;
		return -1;
	}

	//dump��ʽ��Ϣ
	av_dump_format(pFormatCtx, 0, kMediaFile.c_str(), 0);

	//�ҵ���Ƶ��������Ϣ
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
		//AVStream.codec ���滻Ϊ AVStream.codecpar
		//���https://lists.libav.org/pipermail/libav-commits/2016-February/018031.html
		pVideoCodecPar = pFormatCtx->streams[iVideoStream]->codecpar;

		//�ҵ���Ӧ��decoder
		pVideoCodec = avcodec_find_decoder(pVideoCodecPar->codec_id);
		if (pVideoCodec == nullptr) {
			cout << "avcodec_find_decoder failed" << endl;
			return -1;
		}

		//avcodec_open2ǰ����ʹ��avcodec_alloc_context3����context
		pVideoCodecCtx = avcodec_alloc_context3(pVideoCodec);
		if (avcodec_parameters_to_context(pVideoCodecCtx, pVideoCodecPar) < 0) {
			cout << "avcodec_parameters_to_context failed" << endl;
			return -1;
		}

		//ʹ��pVideoCodec��ʼ��pVideoCodecCtx
		if (avcodec_open2(pVideoCodecCtx, pVideoCodec, nullptr) < 0) {
			cout << "avcodec_open2 failed" << endl;
			return -1;
		}

		//pFrame���ڽ��ս�����YUV����
		pFrame = av_frame_alloc();
		if (pFrame == nullptr) {
			cout << "av_frame_alloc failed" << endl;
			return -1;
		}

		//��������
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

		//������ȾContext
		pRenderer = SDL_CreateRenderer(pScreen, -1, 0);
		if (pRenderer == nullptr) {
			cout << "SDL_CreateRenderer failed:" << SDL_GetError() << endl;
			return -1;
		}

		//��ʼ���ɰ�ɫ
		SDL_SetRenderDrawColor(pRenderer, 255, 255, 255, 255);
		SDL_RenderClear(pRenderer);
		SDL_RenderPresent(pRenderer);

		//����Render�����Texture�����ڷ���YUV����
		pTexture = SDL_CreateTexture(pRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, pVideoCodecCtx->width, pVideoCodecCtx->height);
		if (pTexture == nullptr) {
			cout << "SDL_CreateTexture failed:" << SDL_GetError() << endl;
			return -1;
		}
	}

	int count = 0;
	//��ȡ��Ƶ�������룬ת���������ļ�
	while (av_read_frame(pFormatCtx, &packet) >= 0) {
		if (packet.stream_index == iVideoStream) {
			avcodec_send_packet(pVideoCodecCtx, &packet);
			if (avcodec_receive_frame(pVideoCodecCtx, pFrame) == 0) {

				//��YUV������texture,Ȼ����Ⱦ
				SDL_UpdateYUVTexture(pTexture, NULL,
					pFrame->data[0], pFrame->linesize[0],
					pFrame->data[1], pFrame->linesize[1],
					pFrame->data[2], pFrame->linesize[2]);
				SDL_RenderClear(pRenderer);
				SDL_RenderCopy(pRenderer, pTexture, NULL, NULL);
				SDL_RenderPresent(pRenderer);
				cout << "presented frame" << ++count << endl;
			}
			//ȡ��packet���õ��ڴ棬ԭ�ȵ�av_free_packet����
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