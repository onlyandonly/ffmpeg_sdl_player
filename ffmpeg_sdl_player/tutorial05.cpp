#include "tutorial05.h"
#include <iostream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <SDL.h>
#include <SDL_thread.h>
}

using namespace std;

const int kFFRefreshEvent = SDL_USEREVENT;
const int kFFQuitEvent = SDL_USEREVENT + 1;
const int kSDLAudioSize = 1024;
const int kMaxAudioFrameSize = 192000;
const int kMaxAudioQueueSize = 5 * 16 * 1024;
const int kMaxVideoQueueSize = 5 * 256 * 1024;
const int kMaxVideoFrameQueueSize = 1;
const double kAVSyncThreshold = 0.01;
const double kAVNoSyncThreshold = 10;
const string kMediaFile = "../../../assets/green.mp4";

class PacketQueue {
public:
	PacketQueue()
		:firstPkt_(nullptr),
		lastPkt_(nullptr),
		nbPakcets_(0),
		mutex_(nullptr),
		cond_(nullptr),
		size_(0),
		quit_(0) {

	}
	~PacketQueue() {
	}

	int init() {
		quit_ = 0;
		mutex_ = SDL_CreateMutex();
		cond_ = SDL_CreateCond();
		return 0;
	}

	int deinit() {
		quit_ = 1;
		SDL_DestroyMutex(mutex_);
		SDL_DestroyCond(cond_);
		return 0;
	}

	int push(AVPacket* packet) {
		if (av_packet_make_refcounted(packet) < 0)
			return -1;
		AVPacketList* node = (AVPacketList*)av_malloc(sizeof(AVPacketList));
		if (!node)return -1;
		node->pkt = *packet;
		node->next = nullptr;

		SDL_LockMutex(mutex_);

		if (!firstPkt_)
			firstPkt_ = node;
		else
			lastPkt_->next = node;
		lastPkt_ = node;
		++nbPakcets_;
		size_ += packet->size;
		SDL_CondSignal(cond_);

		SDL_UnlockMutex(mutex_);
		return 0;
	}

	int pop(AVPacket* packet, int block) {
		int ret = 0;
		SDL_LockMutex(mutex_);

		for (;;) {
			if (quit_) {
				ret = -1;
				break;
			}
			if (nbPakcets_ > 0) {
				AVPacketList* node = firstPkt_;
				firstPkt_ = firstPkt_->next;
				if (!firstPkt_)lastPkt_ = nullptr;

				*packet = node->pkt;
				--nbPakcets_;
				size_ -= packet->size;
				av_free(node);

				ret = 1;
				break;
			}
			else if (!block) {
				break;
			}
			else {
				SDL_CondWait(cond_, mutex_);
			}
		}

		SDL_UnlockMutex(mutex_);

		return ret;
	}

	int size() const {
		return size_;
	}

private:
	AVPacketList* firstPkt_, * lastPkt_;
	int nbPakcets_;
	//the size is not list size, but sum of all packets' size
	int size_;
	SDL_mutex* mutex_;
	SDL_cond* cond_;
	int quit_;
};

typedef struct VideoPicture {
	AVFrame frame;
	bool empty;
	double pts;
}VideoPicture;

typedef struct VideoState {
	AVFormatContext* pFormatCtx;
	//audio
	int iAudioStreamIndex;
	AVStream* pAudioStream;
	AVCodecContext* pAudioCodecCtx;
	SwrContext* pSwrCtx;
	PacketQueue audioQueue;
	uint8_t audioBuf[kMaxAudioFrameSize * 3 / 2];
	unsigned audioBufSize;
	unsigned audioBufIndex;
	AVPacket audioPacket;
	AVFrame audioFrame;
	uint8_t outAudioBuf[kMaxAudioFrameSize * 2];
	double audioClock;
	//video
	int iVideoStreamIndex;
	AVStream* pVideoStream;
	AVCodecContext* pVideoCodecCtx;
	PacketQueue videoQueue;
	VideoPicture videoPicture;
	SDL_mutex* pVideoFrameMutex;
	SDL_cond* pVideoFrameCond;

	double videoClock;
	double frameLastPts;
	double frameLastDelay;
	double frameTimer;

	SDL_Window* pScreen;
	SDL_Renderer* pRenderer;
	SDL_Texture* pTexture;

	SDL_Thread* pParseThread;
	SDL_Thread* pVideoDecodeThread;

	char filename[1024];
	int quit;
}VideoState;

static void audioCallback(void* userdata, uint8_t* stream, int len);
static int audioDecodeFrame(VideoState* pVideoState, uint8_t* audioBuf, int audioBufSize);
static int parseMediaFileThread(void* userdata);
static int videoDecodeThread(void* userdata);
static int openStreamComponent(VideoState* pVideoState, int streamIndex);
static void scheduleRefresh(VideoState* pVideoState, int delay);
static uint32_t sdlRefreshCallback(uint32_t interval, void* userdata);
static void videoRefreshTimer(void* userdata);
static double synchronizeVideo(VideoState* pVideoState, AVFrame* srcFrame);
static double getAudioClock(VideoState* pVideoState);

int tutorial05() {
	cout << "job's started." << endl;

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		cout << "SDL_Init failed: " << SDL_GetError() << endl;
		return -1;
	}

	VideoState* pVideoState;
	SDL_Event event;
	AVFormatContext* pFormatCtx = nullptr;
	int iVideoStreamIndex = -1;
	int iAudioStreamIndex = -1;

	pVideoState = (VideoState*)av_mallocz(sizeof(VideoState));
	strcpy_s(pVideoState->filename, kMediaFile.c_str());
	pVideoState->videoPicture.empty = true;
	pVideoState->pVideoFrameMutex = SDL_CreateMutex();
	pVideoState->pVideoFrameCond = SDL_CreateCond();

	scheduleRefresh(pVideoState, 40);

	//读取avformat
	if (avformat_open_input(&pFormatCtx, pVideoState->filename, nullptr, nullptr) < 0) {
		cout << "avformat_open_input failed" << endl;
		return -1;
	}

	pVideoState->pFormatCtx = pFormatCtx;

	//读取流信息，该函数会填充pFormatCtx->streams
	if (avformat_find_stream_info(pFormatCtx, nullptr) < 0) {
		cout << "avformat_find_stream_info failed" << endl;
		return -1;
	}

	//dump格式信息
	av_dump_format(pFormatCtx, 0, pVideoState->filename, 0);

	//找到视频流编码信息
	for (unsigned i = 0; i < pFormatCtx->nb_streams; ++i) {
		if (iVideoStreamIndex == -1 && pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			iVideoStreamIndex = i;
		}
		else if (iAudioStreamIndex == -1 && pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			iAudioStreamIndex = i;
		}
	}
	if (iVideoStreamIndex < 0 || iAudioStreamIndex < 0) {
		cout << "couldn't find video stream or audio stream" << endl;
		cout << "video stream index:" << iVideoStreamIndex << endl;
		cout << "audio stream index:" << iAudioStreamIndex << endl;
		SDL_Event event;
		event.type = kFFQuitEvent;
		event.user.data1 = pVideoState;
		SDL_PushEvent(&event);
		return -1;
	}
	openStreamComponent(pVideoState, iVideoStreamIndex);
	openStreamComponent(pVideoState, iAudioStreamIndex);

	pVideoState->pParseThread = SDL_CreateThread(parseMediaFileThread, "parseMediaFileThread", pVideoState);
	if (!pVideoState->pParseThread) {
		cout << "SDL_CreateThread failed: " << SDL_GetError() << endl;
		av_free(pVideoState);
		return -1;
	}

	for (;;) {
		SDL_WaitEvent(&event);
		switch (event.type)
		{
		case SDL_QUIT:
		case kFFQuitEvent:
			pVideoState->quit = 1;
			pVideoState->videoQueue.deinit();
			pVideoState->audioQueue.deinit();
			SDL_Quit();
			return 0;
			break;
		case kFFRefreshEvent:
			videoRefreshTimer(event.user.data1);
			break;
		default:
			break;
		}
	}


	avcodec_close(pVideoState->pVideoCodecCtx);
	avcodec_close(pVideoState->pAudioCodecCtx);

	avformat_close_input(&pVideoState->pFormatCtx);

	swr_close(pVideoState->pSwrCtx);

	SDL_DestroyTexture(pVideoState->pTexture);
	SDL_DestroyRenderer(pVideoState->pRenderer);
	SDL_DestroyWindow(pVideoState->pScreen);
	SDL_Quit();

	cout << "job's done" << endl;

	return 0;
}

void audioCallback(void* userdata, uint8_t* stream, int len) {
	VideoState* pVideoState = (VideoState*)userdata;
	int decodedLen, decodedAudioSize = 0;

	while (len > 0) {
		if (pVideoState->audioBufIndex >= pVideoState->audioBufSize) {
			//所有音频数据已发出，从队列里再拿一份
			decodedAudioSize = audioDecodeFrame(pVideoState, pVideoState->audioBuf, sizeof(pVideoState->audioBuf));
			if (decodedAudioSize < 0) {
				//拿不到数据，静音播放
				pVideoState->audioBufSize = kSDLAudioSize;
				memset(pVideoState->audioBuf, 0, pVideoState->audioBufSize);
			}
			else {
				pVideoState->audioBufSize = decodedAudioSize;
			}
			pVideoState->audioBufIndex = 0;
		}
		decodedLen = pVideoState->audioBufSize - pVideoState->audioBufIndex;
		if (decodedLen > len)
			decodedLen = len;
		memcpy(stream, pVideoState->audioBuf + pVideoState->audioBufIndex, decodedLen);
		len -= decodedLen;
		stream += decodedLen;
		pVideoState->audioBufIndex += decodedLen;
	}
}

int audioDecodeFrame(VideoState* pVideoState, uint8_t* audioBuf, int audioBufSize) {
	AVPacket* packet = &pVideoState->audioPacket;
	AVFrame* frame = &pVideoState->audioFrame;
	int len = -1;

	if (pVideoState->quit || pVideoState->audioQueue.pop(packet, 1) < 0) {
		return -1;
	}

	//该音频包开始播放时的pts
	pVideoState->audioClock = av_q2d(pVideoState->pAudioStream->time_base) * packet->pts;

	//cout << "audioClock:" << pVideoState->audioClock << endl;
	if (avcodec_send_packet(pVideoState->pAudioCodecCtx, packet)) {
		cerr << "avcodec_send_packet failed" << endl;
	}
	if (avcodec_receive_frame(pVideoState->pAudioCodecCtx, frame) == 0) {
		uint8_t* outBuf = pVideoState->outAudioBuf;
		int samples = swr_convert(pVideoState->pSwrCtx, &outBuf, kMaxAudioFrameSize, (const uint8_t**)frame->data, frame->nb_samples);
		len = av_samples_get_buffer_size(nullptr, frame->channels, samples, AV_SAMPLE_FMT_S16, 1);
		memcpy(audioBuf, frame->data[0], len);

		//计算出该音频包播放结束时的pts
		int bytesPerSamples = sizeof(int16_t) * pVideoState->pAudioCodecCtx->channels;
		pVideoState->audioClock += (double)samples / (double)(bytesPerSamples * pVideoState->pAudioCodecCtx->sample_rate);
	}
	av_packet_unref(packet);
	return len;
}

int parseMediaFileThread(void* userdata)
{
	VideoState* pVideoState = (VideoState*)userdata;
	AVPacket packet;
	int ret;

	//读取数据包
	for (;;) {
		if (pVideoState->quit) {
			break;
		}
		if (pVideoState->audioQueue.size() > kMaxAudioQueueSize
			|| pVideoState->videoQueue.size() > kMaxVideoQueueSize) {
			SDL_Delay(10);
		}
		if ((ret = av_read_frame(pVideoState->pFormatCtx, &packet)) < 0) {
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				break;
			}
			if (pVideoState->pFormatCtx->pb->error == 0) {
				SDL_Delay(100);
				continue;
			}
			else {
				break;
			}
		}
		if (packet.stream_index == pVideoState->iVideoStreamIndex) {
			pVideoState->videoQueue.push(&packet);
		}
		else if (packet.stream_index == pVideoState->iAudioStreamIndex) {
			pVideoState->audioQueue.push(&packet);
		}
		else {
			av_packet_unref(&packet);
		}
	}
	//由于SDL窗口是在该线程创建的，如果在该线程退出，窗口也会关闭
	while (!pVideoState->quit) {
		SDL_Delay(100);
	}

	return 0;
}

int videoDecodeThread(void* userdata)
{
	VideoState* pVideoState = (VideoState*)userdata;
	AVCodecContext* pCodecCtx = pVideoState->pVideoCodecCtx;
	AVFrame* pFrame;
	AVPacket packet;
	double pts;

	pFrame = av_frame_alloc();

	for (;;) {
		if (pVideoState->videoQueue.pop(&packet, 1) < 0) {
			break;
		}

		avcodec_send_packet(pCodecCtx, &packet);

		if (avcodec_receive_frame(pCodecCtx, pFrame) == 0) {
			pts = synchronizeVideo(pVideoState, pFrame);

			SDL_LockMutex(pVideoState->pVideoFrameMutex);
			while (!pVideoState->videoPicture.empty && !pVideoState->quit) {
				SDL_CondWait(pVideoState->pVideoFrameCond, pVideoState->pVideoFrameMutex);
			}
			SDL_UnlockMutex(pVideoState->pVideoFrameMutex);

			if (pVideoState->quit)return -1;
			//直接赋值是偷懒且不好的做法
			pVideoState->videoPicture.frame = *pFrame;
			pVideoState->videoPicture.pts = pts;

			SDL_LockMutex(pVideoState->pVideoFrameMutex);
			pVideoState->videoPicture.empty = false;
			SDL_UnlockMutex(pVideoState->pVideoFrameMutex);
		}
		av_packet_unref(&packet);
	}
	av_frame_free(&pFrame);
	return 0;
}

int openStreamComponent(VideoState* pVideoState, int streamIndex)
{
	AVFormatContext* pFormatCtx = pVideoState->pFormatCtx;
	AVCodecContext* pCodecCtx = nullptr;
	AVCodec* pCodec = nullptr;

	if (streamIndex < 0 || streamIndex >= pFormatCtx->nb_streams) {
		return -1;
	}

	pCodec = avcodec_find_decoder(pFormatCtx->streams[streamIndex]->codecpar->codec_id);
	if (!pCodec) {
		cout << "avcodec_find_decoder failed" << endl;
		return -1;
	}

	pCodecCtx = avcodec_alloc_context3(pCodec);
	if (!pCodecCtx) {
		cout << "avcodec_alloc_context3 failed" << endl;
		return -1;
	}

	if (avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[streamIndex]->codecpar)) {
		cout << "avcodec_parameters_to_context failed" << endl;
		return -1;
	}

	if (avcodec_open2(pCodecCtx, pCodec, nullptr)) {
		cout << "avcodec_open2 failed" << endl;
		return -1;
	}

	switch (pCodecCtx->codec_type)
	{
	case AVMEDIA_TYPE_VIDEO:
		pVideoState->iVideoStreamIndex = streamIndex;
		pVideoState->pVideoStream = pFormatCtx->streams[streamIndex];
		pVideoState->pVideoCodecCtx = pCodecCtx;
		pVideoState->frameTimer = (double)av_gettime() / 1000000.0;
		pVideoState->frameLastDelay = 40e-3;
		pVideoState->videoQueue.init();
		pVideoState->pVideoDecodeThread = SDL_CreateThread(videoDecodeThread, "videoDecode", pVideoState);

		//创建窗口
		pVideoState->pScreen = SDL_CreateWindow("SDLPlayer",
			SDL_WINDOWPOS_UNDEFINED,
			SDL_WINDOWPOS_UNDEFINED,
			pVideoState->pVideoCodecCtx->width,
			pVideoState->pVideoCodecCtx->height,
			0);
		if (pVideoState->pScreen == nullptr) {
			cout << "SDL_CreateWindow failed:" << SDL_GetError() << endl;
			return -1;
		}

		//创建渲染Context
		pVideoState->pRenderer = SDL_CreateRenderer(pVideoState->pScreen, -1, 0);
		if (pVideoState->pRenderer == nullptr) {
			cout << "SDL_CreateRenderer failed:" << SDL_GetError() << endl;
			return -1;
		}

		//初始化成白色
		SDL_SetRenderDrawColor(pVideoState->pRenderer, 255, 255, 255, 255);
		SDL_RenderClear(pVideoState->pRenderer);
		SDL_RenderPresent(pVideoState->pRenderer);

		//创建Render所需的Texture，用于放置YUV数据
		pVideoState->pTexture = SDL_CreateTexture(pVideoState->pRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, pCodecCtx->width, pCodecCtx->height);
		if (pVideoState->pTexture == nullptr) {
			cout << "SDL_CreateTexture failed:" << SDL_GetError() << endl;
			return -1;
		}

		break;
	case AVMEDIA_TYPE_AUDIO:
		pVideoState->iAudioStreamIndex = streamIndex;
		pVideoState->pAudioStream = pFormatCtx->streams[streamIndex];
		pVideoState->pAudioCodecCtx = pCodecCtx;
		pVideoState->audioQueue.init();
		pVideoState->audioBufIndex = 0;
		pVideoState->audioBufSize = 0;
		memset(&pVideoState->audioPacket, 0, sizeof(pVideoState->audioPacket));

		{
			//输入格式
			uint64_t inChannelLayout = av_get_default_channel_layout(pCodecCtx->channels);  //通道布局 

			//输出格式
			AVSampleFormat outSampleFormat = AV_SAMPLE_FMT_S16;
			int outSampleRate = 44100;   //采样率
			int outSamples = pCodecCtx->frame_size;    //样本数
			uint64_t outChannelLayout = AV_CH_LAYOUT_STEREO;  //通道布局 输出双声道
			int outChannels = av_get_channel_layout_nb_channels(outChannelLayout);        //通道数

			pVideoState->pSwrCtx = swr_alloc_set_opts(NULL,
				outChannelLayout, outSampleFormat, outSampleRate,
				inChannelLayout, pCodecCtx->sample_fmt, pCodecCtx->sample_rate,
				0, NULL);
			swr_init(pVideoState->pSwrCtx);

			SDL_AudioSpec desiredSpec;
			SDL_AudioSpec obtainedSpec;
			desiredSpec.freq = 22050;
			desiredSpec.format = AUDIO_S16SYS;
			desiredSpec.channels = outChannels;
			desiredSpec.silence = 0;
			desiredSpec.samples = outSamples;
			desiredSpec.callback = audioCallback;
			desiredSpec.userdata = pVideoState;
			if (SDL_OpenAudio(&desiredSpec, &obtainedSpec) < 0) {
				cout << "SDL_OpenAudio failed:" << SDL_GetError() << endl;
				return -1;
			}
			SDL_PauseAudio(0);
		}
		break;
	default:
		break;
	}

	return 0;
}

void scheduleRefresh(VideoState* pVideoState, int delay)
{
	SDL_AddTimer(delay, sdlRefreshCallback, pVideoState);
}

uint32_t sdlRefreshCallback(uint32_t interval, void* userdata)
{
	SDL_Event event;
	event.type = kFFRefreshEvent;
	event.user.data1 = userdata;
	SDL_PushEvent(&event);
	return 0; // 0 means stop timer
}

void videoRefreshTimer(void* userdata)
{
	VideoState* pVideoState = (VideoState*)userdata;
	VideoPicture* pVideoPicture = &pVideoState->videoPicture;
	AVFrame* pFrame = &pVideoPicture->frame;
	double actualDelay, delay, syncThreshold, refClock, diff;

	if (!pVideoState) {
		scheduleRefresh(pVideoState, 100);
		return;
	}
	if (pVideoState->videoPicture.empty) {
		scheduleRefresh(pVideoState, 1);
		return;
	}

	//用这次pts与上一次pts的差值预测delay
	delay = pVideoPicture->pts - pVideoState->frameLastPts;
	if (delay <= 0 || delay >= 1) {
		//delay不合理，延用上次的delay
		delay = pVideoState->frameLastDelay;
	}
	
	pVideoState->frameLastDelay = delay;
	pVideoState->frameLastPts = pVideoPicture->pts;

	refClock = getAudioClock(pVideoState);
	diff = pVideoPicture->pts - refClock;

	//cout << "diff " << diff << endl;
	//如果比音频慢的时间超过阈值，就立刻播放下一帧，相反情况则延迟两倍时间
	syncThreshold = delay > kAVSyncThreshold ? delay : kAVSyncThreshold;
	if (fabs(diff) < kAVNoSyncThreshold) {
		if (diff <= -syncThreshold) {
			delay = 0;
		}
		else if (diff >= syncThreshold) {
			delay *= 2;
		}
	}

	pVideoState->frameTimer += delay;
	actualDelay = pVideoState->frameTimer - av_gettime() / 1000000.0;
	if (actualDelay < 0.01) {
		//这里应该直接跳过该帧，而不是0.01秒后渲染
		actualDelay = 0.01;
	}

	//cout << "actual delay " << actualDelay << endl;
	scheduleRefresh(pVideoState, actualDelay * 1000 + 0.5);

	//将YUV更新至texture,然后渲染
	SDL_UpdateYUVTexture(pVideoState->pTexture, NULL,
		pFrame->data[0], pFrame->linesize[0],
		pFrame->data[1], pFrame->linesize[1],
		pFrame->data[2], pFrame->linesize[2]);
	SDL_RenderClear(pVideoState->pRenderer);
	SDL_RenderCopy(pVideoState->pRenderer, pVideoState->pTexture, NULL, NULL);
	SDL_RenderPresent(pVideoState->pRenderer);

	SDL_LockMutex(pVideoState->pVideoFrameMutex);
	pVideoState->videoPicture.empty = true;
	SDL_CondSignal(pVideoState->pVideoFrameCond);
	SDL_UnlockMutex(pVideoState->pVideoFrameMutex);
}

double synchronizeVideo(VideoState* pVideoState, AVFrame* srcFrame)
{
	double pts = srcFrame->pts;
	double frameDelay;

	//万一pts为0，我们用预测的videoClock来填补
	pts *= av_q2d(pVideoState->pVideoStream->time_base);
	if (pts) {
		pVideoState->videoClock = pts;
	}
	else {
		pts = pVideoState->videoClock;
	}
	
	//frameDelay=1/frameRate,但奇怪的是我测试30fps的视频，这里的time_base却是1/60？
	frameDelay = av_q2d(pVideoState->pVideoCodecCtx->time_base);
	frameDelay += srcFrame->repeat_pict * (frameDelay * 0.5);
	pVideoState->videoClock += frameDelay;

	return pts;
}

double getAudioClock(VideoState* pVideoState)
{
	double pts;
	int restBufSize, bytesPerSec, bytesPerSamples;

	pts = pVideoState->audioClock;
	restBufSize = pVideoState->audioBufSize - pVideoState->audioBufIndex;
	bytesPerSec = 0;
	bytesPerSamples = sizeof(int16_t) * pVideoState->pVideoCodecCtx->channels;
	if (pVideoState->pAudioCodecCtx) {
		bytesPerSec = pVideoState->pAudioCodecCtx->sample_rate * bytesPerSamples;
	}
	if (bytesPerSec) {
		pts -= (double)restBufSize / bytesPerSec;
	}

	return pts;
}
