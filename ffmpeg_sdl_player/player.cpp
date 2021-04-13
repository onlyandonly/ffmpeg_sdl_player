/*******************************************************************
*  Copyright(c) 2021
*  All rights reserved.
*
*  文件名称:  player.cpp
*  简要描述:  测试入口  
*
*  作者:  非一 
*         https://cloud.tencent.com/developer/user/5984999
*  说明:  基于教程 http://dranger.com/ffmpeg/ 使用FFmpeg4.0 + SDL2.0的具体实现
*
*******************************************************************/

#include <iostream>
#include "tutorial01.h"
#include "tutorial02.h"
#include "tutorial03.h"
#include "tutorial04.h"
#include "tutorial05.h"
#include "tutorial06.h"
#include "tutorial07.h"

using namespace std;

int main(int argc, char* argv[]) {

	//解码视频数据，转换为RGB，保存至PPM文件
	//tutorial01();

	//解码视频数据，SDL渲染播放
	//tutorial02();

	//解码音频数据，SDL播放
	//tutorial03();

	//解码播放音视频，线程版本
	//tutorial04();

	//解码播放音视频，仅同步视频
	//tutorial05();

	//解码播放音视频，支持同步音频和视频，默认同步音频
	//tutorial06();

	//进度定位
	tutorial07();

	return 0;
}
