#include "tutorial07.h"
#include <iostream>

#include "common.h"

static void audioCallback(void* userdata, uint8_t* stream, int len);
static int audioDecodeFrame(VideoState* pVideoState, uint8_t* audioBuf, int audioBufSize, double* pPts);
static int parseMediaFileThread(void* userdata);
static int videoDecodeThread(void* userdata);
static int openStreamComponent(VideoState* pVideoState, int streamIndex);
static void scheduleRefresh(VideoState* pVideoState, int delay);
static uint32_t sdlRefreshCallback(uint32_t interval, void* userdata);
static void videoRefreshTimer(void* userdata);
static double synchronizeVideo(VideoState* pVideoState, AVFrame* srcFrame);
static int synchronizeAudio(VideoState* pVideoState, int16_t* samples, int samplesSize, double pts);
static double getAudioClock(VideoState* pVideoState);
static double getVideoClock(VideoState* pVideoState);
static double getExternalClock(VideoState* pVideoState);
static double getMasterClock(VideoState* pVideoState);
static void streamSeek(VideoState* pVideoState, int64_t pos, double increment);

int tutorial07() {
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
	pVideoState->avSyncType = kAvSyncDefaultMaster;
	pVideoState->videoPicture.empty = true;
	pVideoState->pVideoFrameMutex = SDL_CreateMutex();
	pVideoState->pVideoFrameCond = SDL_CreateCond();
	av_init_packet(&pVideoState->flushPacket);
	pVideoState->flushPacket.data = (uint8_t*)"FLUSH";

	scheduleRefresh(pVideoState, 40);

	//??ȡavformat
	if (avformat_open_input(&pFormatCtx, pVideoState->filename, nullptr, nullptr) < 0) {
		cout << "avformat_open_input failed" << endl;
		return -1;
	}

	pVideoState->pFormatCtx = pFormatCtx;

	//??ȡ????Ϣ???ú?????????pFormatCtx->streams
	if (avformat_find_stream_info(pFormatCtx, nullptr) < 0) {
		cout << "avformat_find_stream_info failed" << endl;
		return -1;
	}

	//dump??ʽ??Ϣ
	av_dump_format(pFormatCtx, 0, pVideoState->filename, 0);

	//?ҵ???Ƶ????????Ϣ
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
		double increment = 0, pos;
		SDL_WaitEvent(&event);
		switch (event.type)
		{
		case SDL_KEYDOWN:
			switch (event.key.keysym.sym)
			{
			case SDLK_LEFT:
				increment = -10.0;
				break;
			case SDLK_RIGHT:
				increment = 10.0;
				break;
			case SDLK_UP:
				increment = 60.0;
				break;
			case SDLK_DOWN:
				increment = -60.0;
				break;
			default:
				break;
			}
			if (increment != 0) {
				pos = getMasterClock(pVideoState);
				pos += increment;
				streamSeek(pVideoState, (int64_t)(pos * AV_TIME_BASE), increment);
			}
			break;
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
	double pts = 0;

	while (len > 0) {
		if (pVideoState->audioBufIndex >= pVideoState->audioBufSize) {
			//??????Ƶ?????ѷ??????Ӷ?????????һ??
			decodedAudioSize = audioDecodeFrame(pVideoState, pVideoState->audioBuf, sizeof(pVideoState->audioBuf), &pts);
			if (decodedAudioSize < 0) {
				//?ò??????ݣ?????????
				pVideoState->audioBufSize = kSDLAudioSize;
				memset(pVideoState->audioBuf, 0, pVideoState->audioBufSize);
			}
			else {
				decodedAudioSize = synchronizeAudio(pVideoState, (short*)pVideoState->audioBuf, decodedAudioSize, pts);
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

int audioDecodeFrame(VideoState* pVideoState, uint8_t* audioBuf, int audioBufSize, double* pPts) {
	AVPacket* packet = &pVideoState->audioPacket;
	AVFrame* frame = &pVideoState->audioFrame;
	int len = -1;

	if (pVideoState->quit || pVideoState->audioQueue.pop(packet, 1) < 0) {
		return -1;
	}

	if (packet->data == pVideoState->flushPacket.data) {
		avcodec_flush_buffers(pVideoState->pVideoCodecCtx);
		return -1;
	}

	//????Ƶ????ʼ????ʱ??pts
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

		//??????????Ƶ?????Ž???ʱ??pts
		*pPts = pVideoState->audioClock;
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

	//??ȡ???ݰ?
	for (;;) {
		if (pVideoState->quit) {
			break;
		}
		if (pVideoState->seekReq) {
			int streamIndex = -1;
			int64_t seekTarget = pVideoState->seekPos;
			if (pVideoState->iVideoStreamIndex >= 0) streamIndex = pVideoState->iVideoStreamIndex;
			else if (pVideoState->iAudioStreamIndex >= 0)streamIndex = pVideoState->iAudioStreamIndex;

			if (streamIndex >= 0) {
				//#define AV_TIME_BASE_Q          (AVRational){1, AV_TIME_BASE}
				//?ڶ???????ԭ????AV_TIME_BASE_Q,????AVRational???ߵ????ŵ?????vs2019?????벻ͨ??
				seekTarget = av_rescale_q(seekTarget, AVRational{ 1, AV_TIME_BASE }, pVideoState->pFormatCtx->streams[streamIndex]->time_base);
			}

			if (av_seek_frame(pVideoState->pFormatCtx, streamIndex, seekTarget, pVideoState->seekFlags) < 0) {
				cerr << "av_seek_frame failed" << endl;
			}
			else {
				if (pVideoState->iVideoStreamIndex >= 0) {
					pVideoState->videoQueue.flush();
					pVideoState->videoQueue.push(&pVideoState->flushPacket);
				}
				if (pVideoState->iAudioStreamIndex >= 0) {
					pVideoState->audioQueue.flush();
					pVideoState->audioQueue.push(&pVideoState->flushPacket);
				}
			}
			pVideoState->seekReq = 0;
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
	//????SDL???????ڸ??̴߳????ģ??????ڸ??߳??˳???????Ҳ???ر?
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
		if (packet.data == pVideoState->flushPacket.data) {
			avcodec_flush_buffers(pCodecCtx);
			continue;
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
			//ֱ?Ӹ?ֵ??͵???Ҳ??õ?????
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
		pVideoState->videoCurrentPtsTime = av_gettime();
		pVideoState->videoQueue.init();
		pVideoState->pVideoDecodeThread = SDL_CreateThread(videoDecodeThread, "videoDecode", pVideoState);

		//????????
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

		//??????ȾContext
		pVideoState->pRenderer = SDL_CreateRenderer(pVideoState->pScreen, -1, 0);
		if (pVideoState->pRenderer == nullptr) {
			cout << "SDL_CreateRenderer failed:" << SDL_GetError() << endl;
			return -1;
		}

		//??ʼ???ɰ?ɫ
		SDL_SetRenderDrawColor(pVideoState->pRenderer, 255, 255, 255, 255);
		SDL_RenderClear(pVideoState->pRenderer);
		SDL_RenderPresent(pVideoState->pRenderer);

		//????Render??????Texture?????ڷ???YUV????
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
			//??????ʽ
			uint64_t inChannelLayout = av_get_default_channel_layout(pCodecCtx->channels);  //ͨ?????? 

			//??????ʽ
			AVSampleFormat outSampleFormat = AV_SAMPLE_FMT_S16;
			int outSampleRate = 44100;   //??????
			int outSamples = pCodecCtx->frame_size;    //??????
			uint64_t outChannelLayout = AV_CH_LAYOUT_STEREO;  //ͨ?????? ????˫????
			int outChannels = av_get_channel_layout_nb_channels(outChannelLayout);        //ͨ????

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

	pVideoState->videoCurrentPts = pVideoPicture->pts;
	pVideoState->videoCurrentPtsTime = av_gettime();

	//??????pts????һ??pts?Ĳ?ֵԤ??delay
	delay = pVideoPicture->pts - pVideoState->frameLastPts;
	if (delay <= 0 || delay >= 1) {
		//delay?????????????ϴε?delay
		delay = pVideoState->frameLastDelay;
	}

	pVideoState->frameLastDelay = delay;
	pVideoState->frameLastPts = pVideoPicture->pts;

	if (pVideoState->avSyncType != kAVSyncVideoMaster) {
		refClock = getAudioClock(pVideoState);
		diff = pVideoPicture->pts - refClock;

		//cout << "diff " << diff << endl;
		//????????Ƶ????ʱ?䳬????ֵ???????̲?????һ֡???෴???????ӳ?????ʱ??
		syncThreshold = delay > kAVSyncThreshold ? delay : kAVSyncThreshold;
		if (fabs(diff) < kAVNoSyncThreshold) {
			if (diff <= -syncThreshold) {
				delay = 0;
			}
			else if (diff >= syncThreshold) {
				delay *= 2;
			}
		}
	}

	pVideoState->frameTimer += delay;
	actualDelay = pVideoState->frameTimer - av_gettime() / 1000000.0;
	if (actualDelay < 0.01) {
		//????Ӧ??ֱ????????֡????????0.01??????Ⱦ
		actualDelay = 0.01;
	}

	//cout << "actual delay " << actualDelay << endl;
	scheduleRefresh(pVideoState, actualDelay * 1000 + 0.5);

	//??YUV??????texture,Ȼ????Ⱦ
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

	//??һptsΪ0????????Ԥ????videoClock???
	pts *= av_q2d(pVideoState->pVideoStream->time_base);
	if (pts) {
		pVideoState->videoClock = pts;
	}
	else {
		pts = pVideoState->videoClock;
	}

	//frameDelay=1/frameRate,?????ֵ????Ҳ???30fps????Ƶ????????time_baseȴ??1/60??
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

int synchronizeAudio(VideoState* pVideoState, int16_t* samples, int samplesSize, double pts)
{
	int bytesPerSamples;
	double refClock;

	bytesPerSamples = sizeof(int16_t) * pVideoState->pAudioCodecCtx->channels;

	if (pVideoState->avSyncType != kAVSyncAudioMaster) {
		double diff, avgDiff;
		int wantedSize, minSize, maxSize;

		refClock = getMasterClock(pVideoState);
		diff = getAudioClock(pVideoState) - refClock;
		if (fabs(diff) < kAVNoSyncThreshold) {
			//?Ȼ??۲?ֵ
			pVideoState->audioDiffCum = diff + pVideoState->audioDiffCum * pVideoState->audioDiffAvgCoef;
			if (pVideoState->audioDiffAvgCount < kAudioDiffAvgNb) {
				++pVideoState->audioDiffAvgCount;
			}
			else {
				//???????ۼƵ?20??ʱ?Ż᳢??ȥͬ??
				avgDiff = pVideoState->audioDiffCum * (1.0 - pVideoState->audioDiffAvgCoef);
				//cout << "audio diff: " << avgDiff << endl;
				if (fabs(avgDiff) >= 0.04) {
					//ȷ??sync????sample??һ????Χ??
					wantedSize = samplesSize + ((int)(diff * pVideoState->pAudioCodecCtx->sample_rate) * bytesPerSamples);
					minSize = samplesSize * (1 - kSampleCorrectionPercentMax);
					maxSize = samplesSize * (1 + kSampleCorrectionPercentMax);
					if (wantedSize > maxSize) {
						wantedSize = maxSize;
					}
					else if (wantedSize < minSize) {
						wantedSize = minSize;
					}
					if (wantedSize < samplesSize) {
						samplesSize = wantedSize;
					}
					if (wantedSize > samplesSize) {
						//??????һ??sample?????ж??????Ŀռ?????
						uint8_t* lastSample, * ptr;
						int fillSize;

						fillSize = wantedSize - samplesSize;
						lastSample = (uint8_t*)samples + samplesSize - bytesPerSamples;
						ptr = lastSample + bytesPerSamples;
						while (fillSize > 0) {
							memcpy(ptr, lastSample, bytesPerSamples);
							ptr += bytesPerSamples;
							fillSize -= bytesPerSamples;
						}
						samplesSize = wantedSize;
					}
				}
			}
		}
		else {
			//????Ƶʱ????ֵ????
			pVideoState->audioDiffCum = 0;
			pVideoState->audioDiffAvgCount = 0;
		}
	}
	return samplesSize;
}

double getVideoClock(VideoState* pVideoState) {
	double delta;

	delta = (av_gettime() - pVideoState->videoCurrentPtsTime) / 1000000.0;
	return pVideoState->videoCurrentPts + delta;
}

double getExternalClock(VideoState* pVideoState) {
	return av_gettime() / 1000000.0;
}

double getMasterClock(VideoState* pVideoState) {
	if (pVideoState->avSyncType == kAVSyncAudioMaster) {
		return getAudioClock(pVideoState);
	}
	else if (pVideoState->avSyncType == kAVSyncVideoMaster) {
		return getVideoClock(pVideoState);
	}
	else {
		return getExternalClock(pVideoState);
	}
}

void streamSeek(VideoState* pVideoState, int64_t pos, double increment)
{
	if (pVideoState) {
		pVideoState->seekPos = pos;
		pVideoState->seekFlags = increment < 0 ? AVSEEK_FLAG_BACKWARD : 0;
		pVideoState->seekReq = 1;
	}
}
