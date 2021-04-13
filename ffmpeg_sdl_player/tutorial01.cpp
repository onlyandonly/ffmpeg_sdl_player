#include "tutorial01.h"
#include <iostream>
#include <fstream>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <SDL.h>
#include <SDL_thread.h>
}

using namespace std;

const string kBaseDir = "../../../assets/";
const string kMediaFile = "../../../assets/destiny.mp4";
static void saveFrame(uint8_t** data, int* linesizes, int width, int height, int count);

/*
* �������̣�
* ��ȡ��Ƶ��Ϣ=����ȡ������Ϣ=���ҵ�������=����ʼ��AVCodecContext=����ȡ��Ƶ��=�������YUV=��ת����RGB=�������ļ�
*/
int tutorial01() {
	cout<< "job's started." << endl;

	SwsContext* pSwsCtx = nullptr;
	AVFormatContext* pFormatCtx = nullptr;
	AVCodecContext* pVideoCodecCtx = nullptr;
	AVCodecParameters* pVideoCodecPar = nullptr;
	AVCodec* pVideoCodec = nullptr;
	AVFrame* pFrame = nullptr;
	AVPacket packet;
	uint8_t* data[4] = { nullptr };
	int linesizes[4] = { 0 };
	int iVideoStream = -1;

	//��ʼ����������ֱ��avformat_open_input����
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

		//����洢RGB��buffer
		//ԭ��avpicture_get_size=>av_malloc=>avpicture_fill���׽ӿڱ�������
		//�½ӿ� <libavutil/imgutils.h>::av_image_alloc
		if (av_image_alloc(data, linesizes, pVideoCodecCtx->width, pVideoCodecCtx->height, AV_PIX_FMT_RGB24, 16) < 0) {
			cout << "av_image_alloc failed" << endl;
			return -1;
		}

		//YUV2RGBǰ��Ҫ��ʼ��sws_context
		pSwsCtx = sws_getContext(pVideoCodecCtx->width, pVideoCodecCtx->height, pVideoCodecCtx->pix_fmt,
			pVideoCodecCtx->width, pVideoCodecCtx->height, AV_PIX_FMT_RGB24,
			SWS_BILINEAR, nullptr, nullptr, nullptr);
	}

	int count = 0;
	//��ȡ��Ƶ�������룬ת���������ļ�
	while (av_read_frame(pFormatCtx, &packet) >= 0) {
		if (packet.stream_index == iVideoStream) {
			avcodec_send_packet(pVideoCodecCtx, &packet);
			if (avcodec_receive_frame(pVideoCodecCtx, pFrame) == 0) {

				//ԭ��ʹ����pFrameRGB����ṹ����Ϊdata��linesizes�����壬����������
				sws_scale(pSwsCtx,
					pFrame->data, pFrame->linesize, 0, pFrame->height,
					data, linesizes);
				if (count++ % 100 == 0) {
					saveFrame(data, linesizes, pVideoCodecCtx->width, pVideoCodecCtx->height, count);
					cout << "save frame" << count << endl;
				}
			}
			//ȡ��packet���õ��ڴ棬ԭ�ȵ�av_free_packet����
			av_packet_unref(&packet);
		}
	}

	av_freep(data);
	av_free(pFrame);

	avcodec_close(pVideoCodecCtx);

	avformat_close_input(&pFormatCtx);

	cout << "job's done" << endl;

	return 0;
}

void saveFrame(uint8_t** data, int* linesizes, int width, int height, int count) {
	ofstream ofs;
	string fileName;

	fileName = kBaseDir + to_string(count) + ".ppm";
	ofs.open(fileName, ios::binary);
	if (!ofs.is_open()) {
		cout << "can't open file " << fileName << endl;
		return;
	}

	//ppm header
	string header = "P6\n" + to_string(width) + " " + to_string(height) + "\n255\n";
	ofs.write(header.c_str(), header.size());
	//wiite rgb data by line
	for (int i = 0; i < height; ++i) {
		//����RGB��ʽ��linesize[0]��һ�е��ֽ���,��linesizes[0]==3*width
		ofs.write((const char*)data[0] + i * linesizes[0], linesizes[0]);
	}

	ofs.close();
}