#pragma once
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libavutil/time.h>
#include <SDL.h>
#include <SDL_thread.h>
}

using namespace std;

enum {
	kAVSyncAudioMaster,
	kAVSyncVideoMaster,
	kAVSyncExternalMaster,
};

const int kAvSyncDefaultMaster = kAVSyncAudioMaster;

const int kFFRefreshEvent = SDL_USEREVENT;
const int kFFQuitEvent = SDL_USEREVENT + 1;
const int kSDLAudioSize = 1024;
const int kMaxAudioFrameSize = 192000;
const int kMaxAudioQueueSize = 5 * 16 * 1024;
const int kMaxVideoQueueSize = 5 * 256 * 1024;
const int kMaxVideoFrameQueueSize = 1;
const int kAudioDiffAvgNb = 20;
const double kAVSyncThreshold = 0.01;
const double kAVNoSyncThreshold = 10;
const double kSampleCorrectionPercentMax = 0;
const string kBaseDir = "../../../assets/";
const string kMediaFile = "../../../assets/ganbei.mp4";

class PacketQueue {
public:
	PacketQueue()
		:firstPkt_(nullptr),
		lastPkt_(nullptr),
		nbPakcets_(0),
		mutex_(nullptr),
		cond_(nullptr),
		size_(0),
		quit_(0),
		flushPacket_(nullptr) {

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
		if (packet != flushPacket_ && av_packet_make_refcounted(packet) < 0)
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

	void flush() {
		SDL_LockMutex(mutex_);

		AVPacketList* cur = firstPkt_, * pre;
		while (cur) {
			pre = cur;
			cur = cur->next;
			av_packet_unref(&pre->pkt);
			av_free(pre);
		}

		firstPkt_ = nullptr;
		lastPkt_ = nullptr;
		size_ = 0;
		nbPakcets_ = 0;
		SDL_UnlockMutex(mutex_);
	}

	void setFlushPacket(AVPacket* flushPacket) {
		flushPacket_ = flushPacket;
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

	AVPacket* flushPacket_;
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
	double audioDiffCum; //avÆ½¾ù²îÖµ
	double audioDiffAvgCoef;
	double audioDiffAvgCount;

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

	double videoCurrentPts;
	uint64_t videoCurrentPtsTime;

	SDL_Window* pScreen;
	SDL_Renderer* pRenderer;
	SDL_Texture* pTexture;

	SDL_Thread* pParseThread;
	SDL_Thread* pVideoDecodeThread;

	int avSyncType;

	int seekReq;
	int seekFlags;
	int64_t seekPos;
	AVPacket flushPacket;

	char filename[1024];
	int quit;
}VideoState;

typedef struct SDLFFmpegAudioContext {
	SwrContext* pSwrCtx;
	AVCodecContext* pAudioCodecCtx;
	uint8_t* outBuffer;
}SDLFFmpegAudioContext;