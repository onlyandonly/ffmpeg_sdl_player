#include "tutorial03.h"
#include <iostream>

#include "common.h"

PacketQueue gPacketQueue;
int gQuit = 0;

static void audioCallback(void* userdata, uint8_t* stream, int len);
static int audioDecodeFrame(SDLFFmpegAudioContext* pCtx, uint8_t* audioBuf, int audioBufSize);

/*
* ���벥�Ŵ������̣�
* �ҵ���Ƶ��ʽ=���ҵ������ʽ=���ҵ�������=����ȡ���ݰ�=���������=����SDL�ص������дӶ����õ����ݰ�=�����벥��
*/
int tutorial03() {
	cout << "job's started." << endl;

	SDL_Window* pScreen;
	SDL_Renderer* pRenderer;
	SDL_Texture* pTexture;
	SDL_Event       event;
	SDL_AudioSpec desiredSpec;
	SDL_AudioSpec obtainedSpec;

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		cout << "SDL_Init failed: " << SDL_GetError() << endl;
		return -1;
	}

	SDLFFmpegAudioContext* pSDLFFmpegAudioCtx;
	SwrContext* pSwrCtx = nullptr;
	AVFormatContext* pFormatCtx = nullptr;
	AVCodecContext* pAudioCodecCtx = nullptr;
	AVCodecParameters* pAudioCodecPar = nullptr;
	AVCodec* pAudioCodec = nullptr;
	AVCodecContext* pVideoCodecCtx = nullptr;
	AVCodecParameters* pVideoCodecPar = nullptr;
	AVCodec* pVideoCodec = nullptr;
	AVFrame* pFrame = nullptr;
	AVPacket packet;
	int iVideoStream = -1;
	int iAudioStream = -1;

	uint64_t inChannelLayout;
	AVSampleFormat outSampleFormat = AV_SAMPLE_FMT_S16;
	int outSampleRate = 0;
	int outSamples = 0;    //������
	uint64_t outChannelLayout = AV_CH_LAYOUT_STEREO;  //ͨ������ ���˫����
	int outChannels = 0;        //ͨ����
	uint8_t* outBuffer = nullptr;

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
		}
		else if (iAudioStream == -1 && pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			iAudioStream = i;
		}
	}
	if (iVideoStream == -1) {
		cout << "couldn't find video stream" << endl;
		return -1;
	}
	if (iAudioStream == -1) {
		cout << "couldn't find audio stream" << endl;
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

	{
		//audio codec��ʼ��
		pAudioCodecPar = pFormatCtx->streams[iAudioStream]->codecpar;
		pAudioCodec = avcodec_find_decoder(pAudioCodecPar->codec_id);
		if (pAudioCodec == nullptr) {
			cout << "avcodec_find_decoder failed" << endl;
			return -1;
		}

		//����һ��codec context����avcodec_open2��
		pAudioCodecCtx = avcodec_alloc_context3(pAudioCodec);
		if (avcodec_parameters_to_context(pAudioCodecCtx, pAudioCodecPar) < 0) {
			cout << "avcodec_parameters_to_context failed" << endl;
			return -1;
		}

		//open codec
		if (avcodec_open2(pAudioCodecCtx, pAudioCodec, nullptr) < 0) {
			cout << "avcodec_open2 failed" << endl;
			return -1;
		}

		//�����ʽ
		inChannelLayout = av_get_default_channel_layout(pAudioCodecCtx->channels);  //ͨ������ 

		//�����ʽ
		outSampleFormat = AV_SAMPLE_FMT_S16;
		outSampleRate = 44100;   //������
		outSamples = pAudioCodecCtx->frame_size;    //������
		outChannelLayout = AV_CH_LAYOUT_STEREO;  //ͨ������ ���˫����
		outChannels = av_get_channel_layout_nb_channels(outChannelLayout);        //ͨ����
		outBuffer = (uint8_t*)av_malloc(kMaxAudioFrameSize * 2);

		pSwrCtx = swr_alloc_set_opts(NULL,
			outChannelLayout, outSampleFormat, outSampleRate,
			inChannelLayout, pAudioCodecCtx->sample_fmt, pAudioCodecCtx->sample_rate,
			0, NULL);
		swr_init(pSwrCtx);

		pSDLFFmpegAudioCtx = (SDLFFmpegAudioContext*)malloc(sizeof(SDLFFmpegAudioContext));
		pSDLFFmpegAudioCtx->pAudioCodecCtx = pAudioCodecCtx;
		pSDLFFmpegAudioCtx->pSwrCtx = pSwrCtx;
		pSDLFFmpegAudioCtx->outBuffer = outBuffer;

		desiredSpec.freq = 22050;
		desiredSpec.format = AUDIO_S16SYS;
		desiredSpec.channels = outChannels;
		desiredSpec.silence = 0;
		desiredSpec.samples = outSamples;
		desiredSpec.callback = audioCallback;
		desiredSpec.userdata = pSDLFFmpegAudioCtx;
		if (SDL_OpenAudio(&desiredSpec, &obtainedSpec) < 0) {
			cout << "SDL_OpenAudio failed:" << SDL_GetError() << endl;
			return -1;
		}

		gPacketQueue.init();
		SDL_PauseAudio(0);
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
		else if (packet.stream_index == iAudioStream) {
			gPacketQueue.push(&packet);
		}
		else {
			av_packet_unref(&packet);
		}
		SDL_PollEvent(&event);
		switch (event.type) {
		case SDL_QUIT:
			gQuit = 1;
			gPacketQueue.deinit();
			SDL_Quit();
			exit(0);
			break;
		default:
			break;
		}
	}

	SDL_DestroyTexture(pTexture);
	SDL_DestroyRenderer(pRenderer);
	SDL_DestroyWindow(pScreen);
	SDL_Quit();

	av_free(pFrame);
	av_free(outBuffer);

	avcodec_close(pVideoCodecCtx);

	avformat_close_input(&pFormatCtx);

	swr_close(pSwrCtx);

	cout << "job's done" << endl;

	return 0;
}

void audioCallback(void* userdata, uint8_t* stream, int len) {
	SDLFFmpegAudioContext* pCtx = (SDLFFmpegAudioContext *)userdata;
	int decodedLen, decodedAudioSize = 0;

	static uint8_t audioBuf[kMaxAudioFrameSize * 3 / 2];
	static unsigned audioBufSize = 0;
	static unsigned audioBufIndex = 0;
	while (len > 0) {
		if (audioBufIndex >= audioBufSize) {
			//������Ƶ�����ѷ������Ӷ���������һ��
			decodedAudioSize = audioDecodeFrame(pCtx, audioBuf, sizeof(audioBuf));
			if (decodedAudioSize < 0) {
				//�ò������ݣ���������
				audioBufSize = kSDLAudioSize;
				memset(audioBuf, 0, audioBufSize);
			}
			else {
				audioBufSize = decodedAudioSize;
			}
			audioBufIndex = 0;
		}
		decodedLen = audioBufSize - audioBufIndex;
		if (decodedLen > len)
			decodedLen = len;
		memcpy(stream, audioBuf + audioBufIndex, decodedLen);
		len -= decodedLen;
		stream += decodedLen;
		audioBufIndex += decodedLen;
	}
}

int audioDecodeFrame(SDLFFmpegAudioContext* pCtx, uint8_t* audioBuf, int audioBufSize) {
	static AVPacket packet;
	static AVFrame frame;

	int len = -1;

	if (gQuit || gPacketQueue.pop(&packet, 1) < 0) {
		return -1;
	}
	if (avcodec_send_packet(pCtx->pAudioCodecCtx, &packet)) {
		cerr << "avcodec_send_packet failed" << endl;
	}
	if (avcodec_receive_frame(pCtx->pAudioCodecCtx, &frame) == 0) {
		int samples = swr_convert(pCtx->pSwrCtx, &pCtx->outBuffer, kMaxAudioFrameSize, (const uint8_t**)frame.data, frame.nb_samples);
		len = av_samples_get_buffer_size(nullptr, frame.channels, samples, AV_SAMPLE_FMT_S16, 1);
		memcpy(audioBuf, frame.data[0], len);
	}
	if (packet.data)
		av_packet_unref(&packet);
	return len;
}