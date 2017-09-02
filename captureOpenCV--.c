/*
* Copyright (c) 2012-2013, NVIDIA CORPORATION. All rights reserved.
* All information contained herein is proprietary and confidential to NVIDIA
* Corporation.  Any use, reproduction, or disclosure without the written
* permission of NVIDIA Corporation is prohibited.
*/

// edited by Hyundai Autron
// gcc -I. -I./utils `pkg-config opencv --cflags` -I./include  -c -o captureOpenCV.o captureOpenCV.c
// gcc -I. -I./utils `pkg-config opencv --cflags` -I./include  -c -o nvthread.o nvthread.c
// gcc  -o captureOpenCV captureOpenCV.o nvthread.o  -L ./utils -lnvmedia -lnvtestutil_board -lnvtestutil_capture_input -lnvtestutil_i2c -lpthread `pkg-config opencv --libs`

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include<stdbool.h>
#include <nvcommon.h>
#include <nvmedia.h>

#include <testutil_board.h>
#include <testutil_capture_input.h>

#include "nvthread.h"


#include <highgui.h>
#include <cv.h>
#include <ResTable_720To320.h>
#include <pthread.h>
#include <unistd.h>     // for sleep

////////////////////////////////////////////////////////////////////////////
#include "car_lib.h"    //TY add 6.27


////////////////////////////////////////////////////////////////////////////

#define SERVO_CONTROL     // TY add 6.27// To servo control(steering & camera position)
//#define SPEED_CONTROL		
#define SPEED_CONTROL
//#define LIGHT_BEEP
//#define IMGSAVE

////////////////////////////////////////////////////////////////////////////


#define VIP_BUFFER_SIZE 6
#define VIP_FRAME_TIMEOUT_MS 100
#define VIP_NAME "vip"

#define MESSAGE_PRINTF printf

#define CRC32_POLYNOMIAL 0xEDB88320L

#define RESIZE_WIDTH  320
#define RESIZE_HEIGHT 240


void ObjectDetectAndControlSpeed(){
	int data = 0;
	static bool Departure = FALSE;
	#define CHANNEL1 1
	#define CHANNEL6 6
	data = DistanceSensor(CHANNEL1);
	if(!Departure){
		if(data<2800 &&data>1000 )//장애물 최초인식전까지 대기 7cm~22cm
		Departure = True;
		
		speed = 0; 
	}
	else {
	if(data<1000)speed = 160;//앞에 장애물 없음22cm 밖 
	else if(data>2500)speed = 80;//8cm이내앞에 장애물 있고 거리 가까움
	else speed  = 110;//앞에 장애물 있고 거리 멈22~8cm 
	
	data = DistanceSensor(CHANNEL6) 
	if(data>2000)//후방 장애물 발견시 10cm 내 
		speed = 180;
	}
}




static NvMediaVideoSurface *capSurf = NULL;

pthread_cond_t      cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t     mutex = PTHREAD_MUTEX_INITIALIZER;

bool turn_left_max = false;         //TY add
bool turn_right_max = false;


int table_298[256];
int table_409[256];
int table_100[256];
int table_208[256];
int table_516[256];

int color = 0;

typedef struct
{
	I2cId i2cDevice;

	CaptureInputDeviceId vipDeviceInUse;
	NvMediaVideoCaptureInterfaceFormat vipInputtVideoStd;
	unsigned int vipInputWidth;
	unsigned int vipInputHeight;
	float vipAspectRatio;

	unsigned int vipMixerWidth;
	unsigned int vipMixerHeight;

	NvBool vipDisplayEnabled;
	NvMediaVideoOutputType vipOutputType;
	NvMediaVideoOutputDevice vipOutputDevice[2];
	NvBool vipFileDumpEnabled;
	char * vipOutputFileName;

	unsigned int vipCaptureTime;
	unsigned int vipCaptureCount;
} TestArgs;

typedef struct
{
	NvMediaVideoSurface *surf;
	NvBool last;
} QueueElem;

typedef struct
{
	char *name;

	NvSemaphore *semStart, *semDone;

	NvMediaVideoCapture *capture;
	NvMediaVideoMixer *mixer;
	FILE *fout;

	unsigned int inputWidth;
	unsigned int inputHeight;

	unsigned int timeout;

	NvBool displayEnabled;
	NvBool fileDumpEnabled;

	NvBool timeNotCount;
	unsigned int last;
} CaptureContext;

static NvBool stop = NVMEDIA_FALSE;

static void SignalHandler(int signal)
{
	stop = NVMEDIA_TRUE;
	MESSAGE_PRINTF("%d signal received\n", signal);
}

static void GetTime(NvMediaTime *time)
{
	struct timeval t;

	gettimeofday(&t, NULL);

	time->tv_sec = t.tv_sec;
	time->tv_nsec = t.tv_usec * 1000;
}

static void AddTime(NvMediaTime *time, NvU64 uSec, NvMediaTime *res)
{
	NvU64 t, newTime;

	t = (NvU64)time->tv_sec * 1000000000LL + (NvU64)time->tv_nsec;
	newTime = t + uSec * 1000LL;
	res->tv_sec = newTime / 1000000000LL;
	res->tv_nsec = newTime % 1000000000LL;
}

//static NvS64 SubTime(NvMediaTime *time1, NvMediaTime *time2)
static NvBool SubTime(NvMediaTime *time1, NvMediaTime *time2)
{
	NvS64 t1, t2, delta;

	t1 = (NvS64)time1->tv_sec * 1000000000LL + (NvS64)time1->tv_nsec;
	t2 = (NvS64)time2->tv_sec * 1000000000LL + (NvS64)time2->tv_nsec;
	delta = t1 - t2;

	//    return delta / 1000LL;
	return delta > 0LL;
}


static void DisplayUsage(void)
{
	printf("Usage : nvmedia_capture [options]\n");
	printf("Brief: Displays this help if no arguments are given. Engages the respective capture module whenever a single \'c\' or \'v\' argument is supplied using default values for the missing parameters.\n");
	printf("Options:\n");
	printf("-va <aspect ratio>    VIP aspect ratio (default = 1.78 (16:9))\n");
	printf("-vmr <width>x<height> VIP mixer resolution (default 800x480)\n");
	printf("-vf <file name>       VIP output file name; default = off\n");
	printf("-vt [seconds]         VIP capture duration (default = 10 secs); overridden by -vn; default = off\n");
	printf("-vn [frames]          # VIP frames to be captured (default = 300); default = on if -vt is not used\n");
}

static int ParseOptions(int argc, char *argv[], TestArgs *args)
{
	int i = 1;

	// Set defaults if necessary - TBD
	args->i2cDevice = I2C4;     // i2c chnnel

	args->vipDeviceInUse = AnalogDevices_ADV7182;
	args->vipInputtVideoStd = NVMEDIA_VIDEO_CAPTURE_INTERFACE_FORMAT_VIP_NTSC;
	args->vipInputWidth = 720;
	args->vipInputHeight = 480;
	args->vipAspectRatio = 0.0f;

	args->vipMixerWidth = 800;
	args->vipMixerHeight = 480;

	args->vipDisplayEnabled = NVMEDIA_FALSE;
	args->vipOutputType = NvMediaVideoOutputType_OverlayYUV;
	args->vipOutputDevice[0] = NvMediaVideoOutputDevice_LVDS;
	args->vipFileDumpEnabled = NVMEDIA_FALSE;
	args->vipOutputFileName = NULL;

	args->vipCaptureTime = 0;
	args->vipCaptureCount = 0;



	if (i < argc && argv[i][0] == '-')
	{
		while (i < argc && argv[i][0] == '-')
		{
			if (i > 1 && argv[i][1] == '-')
			{
				MESSAGE_PRINTF("Using basic and custom options together is not supported\n");
				return 0;
			}

			// Get options
			if (!strcmp(argv[i], "-va"))
			{
				if (++i < argc)
				{
					if (sscanf(argv[i], "%f", &args->vipAspectRatio) != 1 || args->vipAspectRatio <= 0.0f) // TBC
					{
						MESSAGE_PRINTF("Bad VIP aspect ratio: %s\n", argv[i]);
						return 0;
					}
				}
				else
				{
					MESSAGE_PRINTF("Missing VIP aspect ratio\n");
					return 0;
				}
			}
			else if (!strcmp(argv[i], "-vmr"))
			{
				if (++i < argc)
				{
					if (sscanf(argv[i], "%ux%u", &args->vipMixerWidth, &args->vipMixerHeight) != 2)
					{
						MESSAGE_PRINTF("Bad VIP mixer resolution: %s\n", argv[i]);
						return 0;
					}
				}
				else
				{
					MESSAGE_PRINTF("Missing VIP mixer resolution\n");
					return 0;
				}
			}
			else if (!strcmp(argv[i], "-vf"))
			{
				args->vipFileDumpEnabled = NVMEDIA_TRUE;
				if (++i < argc)
					args->vipOutputFileName = argv[i];
				else
				{
					MESSAGE_PRINTF("Missing VIP output file name\n");
					return 0;
				}
			}
			else if (!strcmp(argv[i], "-vt"))
			{
				if (++i < argc)
					if (sscanf(argv[i], "%u", &args->vipCaptureTime) != 1)
					{
						MESSAGE_PRINTF("Bad VIP capture duration: %s\n", argv[i]);
						return 0;
					}
			}
			else if (!strcmp(argv[i], "-vn"))
			{
				if (++i < argc)
					if (sscanf(argv[i], "%u", &args->vipCaptureCount) != 1)
					{
						MESSAGE_PRINTF("Bad VIP capture count: %s\n", argv[i]);
						return 0;
					}
			}
			else
			{
				MESSAGE_PRINTF("%s is not a supported option\n", argv[i]);
				return 0;
			}

			i++;
		}
	}

	if (i < argc)
	{
		MESSAGE_PRINTF("%s is not a supported option\n", argv[i]);
		return 0;
	}

	// Check for consistency
	if (i < 2)
	{
		DisplayUsage();
		return 0;
	}


	if (args->vipAspectRatio == 0.0f)
		args->vipAspectRatio = 1.78f;

	if (!args->vipDisplayEnabled && !args->vipFileDumpEnabled)
		args->vipDisplayEnabled = NVMEDIA_TRUE;


	if (!args->vipCaptureTime && !args->vipCaptureCount)
		args->vipCaptureCount = 300;
	else if (args->vipCaptureTime && args->vipCaptureCount)
		args->vipCaptureTime = 0;



	return 1;
}

static int DumpFrame(FILE *fout, NvMediaVideoSurface *surf)
{
	NvMediaVideoSurfaceMap surfMap;
	unsigned int width, height;

	if (NvMediaVideoSurfaceLock(surf, &surfMap) != NVMEDIA_STATUS_OK)
	{
		MESSAGE_PRINTF("NvMediaVideoSurfaceLock() failed in DumpFrame()\n");
		return 0;
	}

	width = surf->width;
	height = surf->height;

	unsigned char *pY[2] = { surfMap.pY, surfMap.pY2 };
	unsigned char *pU[2] = { surfMap.pU, surfMap.pU2 };
	unsigned char *pV[2] = { surfMap.pV, surfMap.pV2 };
	unsigned int pitchY[2] = { surfMap.pitchY, surfMap.pitchY2 };
	unsigned int pitchU[2] = { surfMap.pitchU, surfMap.pitchU2 };
	unsigned int pitchV[2] = { surfMap.pitchV, surfMap.pitchV2 };
	unsigned int i, j;

	for (i = 0; i < 2; i++)
	{
		for (j = 0; j < height / 2; j++)
		{
			fwrite(pY[i], width, 1, fout);
			pY[i] += pitchY[i];
		}
		for (j = 0; j < height / 2; j++)
		{
			fwrite(pU[i], width / 2, 1, fout);
			pU[i] += pitchU[i];
		}
		for (j = 0; j < height / 2; j++)
		{
			fwrite(pV[i], width / 2, 1, fout);
			pV[i] += pitchV[i];
		}
	}


	NvMediaVideoSurfaceUnlock(surf);

	return 1;
}

static int Frame2Ipl(IplImage* img, IplImage* imgResult)
{
	NvMediaVideoSurfaceMap surfMap;
	unsigned int resWidth, resHeight;
	unsigned char y, u, v;
	int num;

	if (NvMediaVideoSurfaceLock(capSurf, &surfMap) != NVMEDIA_STATUS_OK)
	{
		MESSAGE_PRINTF("NvMediaVideoSurfaceLock() failed in Frame2Ipl()\n");
		return 0;
	}

	unsigned char *pY[2] = { surfMap.pY, surfMap.pY2 };
	unsigned char *pU[2] = { surfMap.pU, surfMap.pU2 };
	unsigned char *pV[2] = { surfMap.pV, surfMap.pV2 };
	unsigned int pitchY[2] = { surfMap.pitchY, surfMap.pitchY2 };
	unsigned int pitchU[2] = { surfMap.pitchU, surfMap.pitchU2 };
	unsigned int pitchV[2] = { surfMap.pitchV, surfMap.pitchV2 };
	unsigned int i, j, k, x;
	unsigned int stepY, stepU, stepV;

	unsigned int bin_num = 0;

	resWidth = RESIZE_WIDTH;
	resHeight = RESIZE_HEIGHT;

	// Frame2Ipl
	img->nSize = 112;
	img->ID = 0;
	img->nChannels = 3;
	img->alphaChannel = 0;
	img->depth = IPL_DEPTH_8U;    // 8
	img->colorModel[0] = 'R';
	img->colorModel[1] = 'G';
	img->colorModel[2] = 'B';
	img->channelSeq[0] = 'B';
	img->channelSeq[1] = 'G';
	img->channelSeq[2] = 'R';
	img->dataOrder = 0;
	img->origin = 0;
	img->align = 4;
	img->width = resWidth;
	img->height = resHeight;
	img->imageSize = resHeight*resWidth * 3;
	img->widthStep = resWidth * 3;
	img->BorderMode[0] = 0;
	img->BorderMode[1] = 0;
	img->BorderMode[2] = 0;
	img->BorderMode[3] = 0;
	img->BorderConst[0] = 0;
	img->BorderConst[1] = 0;
	img->BorderConst[2] = 0;
	img->BorderConst[3] = 0;

	stepY = 0;
	stepU = 0;
	stepV = 0;
	i = 0;

	for (j = 0; j < resHeight; j++)
	{
		for (k = 0; k < resWidth; k++)
		{
			x = ResTableX_720To320[k];
			y = pY[i][stepY + x];
			u = pU[i][stepU + x / 2];
			v = pV[i][stepV + x / 2];

			// - 39 , 111 , 51, 241
			num = 3 * k + 3 * resWidth*(j);
			bin_num = j*imgResult->widthStep + k;
			if (u > -39 && u < 120 && v>45 && v < 245) {
				// �씛�깋�쑝濡�
				imgResult->imageData[bin_num] = (char)255;
			}
			else {
				// 寃��젙�깋�쑝濡�
				imgResult->imageData[bin_num] = (char)0;
			}

			img->imageData[num] = y;
			img->imageData[num + 1] = u;
			img->imageData[num + 2] = v;

		}
		stepY += pitchY[i];
		stepU += pitchU[i];
		stepV += pitchV[i];
	}


	NvMediaVideoSurfaceUnlock(capSurf);

	return 1;
}



static int Frame2Ipl_color(IplImage* img, IplImage* imgResult, int color)
{
	//color : 1. �궡�깋 2. �씛�깋 3. 鍮④컙�깋 4. �끂����깋 5. 珥덈줉�깋 6. 寃�����깋 7. �끂���李⑥꽑寃�異�
	NvMediaVideoSurfaceMap surfMap;
	unsigned int resWidth, resHeight;
	unsigned char y, u, v;
	int num;

	if (NvMediaVideoSurfaceLock(capSurf, &surfMap) != NVMEDIA_STATUS_OK)
	{
		MESSAGE_PRINTF("NvMediaVideoSurfaceLock() failed in Frame2Ipl()\n");
		return 0;
	}

	unsigned char *pY[2] = { surfMap.pY, surfMap.pY2 };
	unsigned char *pU[2] = { surfMap.pU, surfMap.pU2 };
	unsigned char *pV[2] = { surfMap.pV, surfMap.pV2 };
	unsigned int pitchY[2] = { surfMap.pitchY, surfMap.pitchY2 };
	unsigned int pitchU[2] = { surfMap.pitchU, surfMap.pitchU2 };
	unsigned int pitchV[2] = { surfMap.pitchV, surfMap.pitchV2 };
	unsigned int i, j, k, x;
	unsigned int stepY, stepU, stepV;

	unsigned int bin_num = 0;

	resWidth = RESIZE_WIDTH;
	resHeight = RESIZE_HEIGHT;

	// Frame2Ipl
	img->nSize = 112;
	img->ID = 0;
	img->nChannels = 3;
	img->alphaChannel = 0;
	img->depth = IPL_DEPTH_8U;    // 8
	img->colorModel[0] = 'R';
	img->colorModel[1] = 'G';
	img->colorModel[2] = 'B';
	img->channelSeq[0] = 'B';
	img->channelSeq[1] = 'G';
	img->channelSeq[2] = 'R';
	img->dataOrder = 0;
	img->origin = 0;
	img->align = 4;
	img->width = resWidth;
	img->height = resHeight;
	img->imageSize = resHeight*resWidth * 3;
	img->widthStep = resWidth * 3;
	img->BorderMode[0] = 0;
	img->BorderMode[1] = 0;
	img->BorderMode[2] = 0;
	img->BorderMode[3] = 0;
	img->BorderConst[0] = 0;
	img->BorderConst[1] = 0;
	img->BorderConst[2] = 0;
	img->BorderConst[3] = 0;

	stepY = 0;
	stepU = 0;
	stepV = 0;
	i = 0;

	for (j = 0; j < resHeight; j++)
	{
		for (k = 0; k < resWidth; k++)
		{
			x = ResTableX_720To320[k];
			y = pY[i][stepY + x];
			u = pU[i][stepU + x / 2];
			v = pV[i][stepV + x / 2];

			// - 39 , 111 , 51, 241
			num = 3 * k + 3 * resWidth*(j);
			bin_num = j*imgResult->widthStep + k;

			switch (color) {
			case 1:   //  �궡�깋, startmission
				if (v > 45 && v < 127) {
					// 寃��젙�깋�쑝濡�
					imgResult->imageData[bin_num] = (char)0;
				}
				else {
					// �씛�깋�쑝濡� -> �궡�깋
					imgResult->imageData[bin_num] = (char)255;
				}
				break;

			case 2:   //  �씛�깋
				if (y > 200 && u > 130) {
					// �씛�깋�쑝濡�
					imgResult->imageData[bin_num] = (char)255;
				}
				else {
					// 寃��젙�깋�쑝濡�
					imgResult->imageData[bin_num] = (char)0;
				}
				break;

			case 3:   //  鍮④컙�깋
				if (v > 140) {
					// �씛�깋�쑝濡�
					imgResult->imageData[bin_num] = (char)255;
				}
				else {
					// 寃��젙�깋�쑝濡�
					imgResult->imageData[bin_num] = (char)0;
				}
				break;

			case 4:   //  �끂����깋
				if (y > 90 && y < 105 && v>146) {
					// �씛�깋�쑝濡�
					imgResult->imageData[bin_num] = (char)255;
				}
				else {
					// 寃��젙�깋�쑝濡�
					imgResult->imageData[bin_num] = (char)0;
				}
				break;

			case 5:   //  珥덈줉�깋
				if (y < 100 && u < 127 && v < 123) {
					// �씛�깋�쑝濡�
					imgResult->imageData[bin_num] = (char)255;
				}
				else {
					// 寃��젙�깋�쑝濡�
					imgResult->imageData[bin_num] = (char)0;
				}
				break;

			case 6:   //  寃�����깋
				if (y > 35 && y < 50 && u>125) {
					// �씛�깋�쑝濡� -> �떎�젣 寃�����깋
					imgResult->imageData[bin_num] = (char)255;
				}
				else {
					// 寃��젙�깋�쑝濡�
					imgResult->imageData[bin_num] = (char)0;
				}
				break;


			default:  //  湲곕낯 : �끂��� 李⑥꽑寃�異�
				if (u > -39 && u < 120 && v>45 && v < 245) {
					// �씛�깋�쑝濡�
					imgResult->imageData[bin_num] = (char)255;
				}
				else {
					// 寃��젙�깋�쑝濡�
					imgResult->imageData[bin_num] = (char)0;
				}
				break;
			}


			img->imageData[num] = y;
			img->imageData[num + 1] = u;
			img->imageData[num + 2] = v;

		}
		stepY += pitchY[i];
		stepU += pitchU[i];
		stepV += pitchV[i];
	}


	NvMediaVideoSurfaceUnlock(capSurf);

	return 1;
}

static unsigned int CaptureThread(void *params)
{
	int i = 0;
	NvU64 stime, ctime;
	NvMediaTime t1 = { 0 }, t2 = { 0 }, st = { 0 }, ct = { 0 };
	CaptureContext *ctx = (CaptureContext *)params;
	NvMediaVideoSurface *releaseList[4] = { NULL }, **relList;
	NvMediaRect primarySrcRect;
	NvMediaPrimaryVideo primaryVideo;

	primarySrcRect.x0 = 0;
	primarySrcRect.y0 = 0;
	primarySrcRect.x1 = ctx->inputWidth;
	primarySrcRect.y1 = ctx->inputHeight;

	primaryVideo.next = NULL;
	primaryVideo.previous = NULL;
	primaryVideo.previous2 = NULL;
	primaryVideo.srcRect = &primarySrcRect;
	primaryVideo.dstRect = NULL;


	NvSemaphoreDecrement(ctx->semStart, NV_TIMEOUT_INFINITE);

	if (ctx->timeNotCount)
	{
		GetTime(&t1);
		AddTime(&t1, ctx->last * 1000000LL, &t1);
		GetTime(&t2);
		printf("timeNotCount\n");
	}
	GetTime(&st);
	stime = (NvU64)st.tv_sec * 1000000000LL + (NvU64)st.tv_nsec;

	while ((ctx->timeNotCount ? (SubTime(&t1, &t2)) : ((unsigned int)i < ctx->last)) && !stop)
	{
		GetTime(&ct);
		ctime = (NvU64)ct.tv_sec * 1000000000LL + (NvU64)ct.tv_nsec;
		//printf("frame=%3d, time=%llu.%09llu[s] \n", i, (ctime-stime)/1000000000LL, (ctime-stime)%1000000000LL);

		pthread_mutex_lock(&mutex);            // for ControlThread()

		if (!(capSurf = NvMediaVideoCaptureGetFrame(ctx->capture, ctx->timeout)))
		{ // TBD
			MESSAGE_PRINTF("NvMediaVideoCaptureGetFrame() failed in %sThread\n", ctx->name);
			stop = NVMEDIA_TRUE;
			break;
		}

		if (i % 3 == 0)                        // once in three loop = 10 Hz
			pthread_cond_signal(&cond);        // ControlThread() is called

		pthread_mutex_unlock(&mutex);        // for ControlThread()

		primaryVideo.current = capSurf;
		primaryVideo.pictureStructure = NVMEDIA_PICTURE_STRUCTURE_TOP_FIELD;

		if (NVMEDIA_STATUS_OK != NvMediaVideoMixerRender(ctx->mixer, // mixer
			NVMEDIA_OUTPUT_DEVICE_0, // outputDeviceMask
			NULL, // background
			&primaryVideo, // primaryVideo
			NULL, // secondaryVideo
			NULL, // graphics0
			NULL, // graphics1
			releaseList, // releaseList
			NULL)) // timeStamp
		{ // TBD
			MESSAGE_PRINTF("NvMediaVideoMixerRender() failed for the top field in %sThread\n", ctx->name);
			stop = NVMEDIA_TRUE;
		}

		primaryVideo.pictureStructure = NVMEDIA_PICTURE_STRUCTURE_BOTTOM_FIELD;
		if (NVMEDIA_STATUS_OK != NvMediaVideoMixerRender(ctx->mixer, // mixer
			NVMEDIA_OUTPUT_DEVICE_0, // outputDeviceMask
			NULL, // background
			&primaryVideo, // primaryVideo
			NULL, // secondaryVideo
			NULL, // graphics0
			NULL, // graphics1
			releaseList, // releaseList
			NULL)) // timeStamp
		{ // TBD
			MESSAGE_PRINTF("NvMediaVideoMixerRender() failed for the bottom field in %sThread\n", ctx->name);
			stop = NVMEDIA_TRUE;
		}

		if (ctx->fileDumpEnabled)
		{
			if (!DumpFrame(ctx->fout, capSurf))
			{ // TBD
				MESSAGE_PRINTF("DumpFrame() failed in %sThread\n", ctx->name);
				stop = NVMEDIA_TRUE;
			}

			if (!ctx->displayEnabled)
				releaseList[0] = capSurf;
		}

		relList = &releaseList[0];

		while (*relList)
		{
			if (NvMediaVideoCaptureReturnFrame(ctx->capture, *relList) != NVMEDIA_STATUS_OK)
			{ // TBD
				MESSAGE_PRINTF("NvMediaVideoCaptureReturnFrame() failed in %sThread\n", ctx->name);
				stop = NVMEDIA_TRUE;
				break;
			}
			relList++;
		}

		if (ctx->timeNotCount)
			GetTime(&t2);

		i++;
	} // while end

	  // Release any left-over frames
	  //    if(ctx->displayEnabled && capSurf && capSurf->type != NvMediaSurfaceType_YV16x2) // To allow returning frames after breaking out of the while loop in case of error
	if (ctx->displayEnabled && capSurf)
	{
		NvMediaVideoMixerRender(ctx->mixer, // mixer
			NVMEDIA_OUTPUT_DEVICE_0, // outputDeviceMask
			NULL, // background
			NULL, // primaryVideo
			NULL, // secondaryVideo
			NULL, // graphics0
			NULL, // graphics1
			releaseList, // releaseList
			NULL); // timeStamp

		relList = &releaseList[0];

		while (*relList)
		{
			if (NvMediaVideoCaptureReturnFrame(ctx->capture, *relList) != NVMEDIA_STATUS_OK)
				MESSAGE_PRINTF("NvMediaVideoCaptureReturnFrame() failed in %sThread\n", ctx->name);

			relList++;
		}
	}

	NvSemaphoreIncrement(ctx->semDone);
	return 0;
}

static void CheckDisplayDevice(NvMediaVideoOutputDevice deviceType, NvMediaBool *enabled, unsigned int *displayId)
{
	int outputDevices;
	NvMediaVideoOutputDeviceParams *outputParams;
	int i;

	// By default set it as not enabled (initialized)
	*enabled = NVMEDIA_FALSE;
	*displayId = 0;

	// Get the number of devices
	if (NvMediaVideoOutputDevicesQuery(&outputDevices, NULL) != NVMEDIA_STATUS_OK) {
		return;
	}

	// Allocate memory for information for all devices
	outputParams = malloc(outputDevices * sizeof(NvMediaVideoOutputDeviceParams));
	if (!outputParams) {
		return;
	}

	// Get device information for acll devices
	if (NvMediaVideoOutputDevicesQuery(&outputDevices, outputParams) != NVMEDIA_STATUS_OK) {
		free(outputParams);
		return;
	}

	// Find desired device
	for (i = 0; i < outputDevices; i++) {
		if ((outputParams + i)->outputDevice == deviceType) {
			// Return information
			*enabled = (outputParams + i)->enabled;
			*displayId = (outputParams + i)->displayId;
			break;
		}
	}

	// Free information memory
	free(outputParams);
}

////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////  Find_Center : TY �쁺�긽泥섎━�븯�뿬 議고뼢媛� 李얠븘�궡�뒗 �븣怨좊━利�.
/////////////////////////////////////  << 異뷀썑 議고뼢媛믩쭔 諛섑솚�븯怨�, �떎�젣議고뼢�븯�뒗 �븿�닔瑜� �뵲濡� 遺꾨━�빐二쇱뼱�빞�븿.
/////////////////////////////////////  鍮덇났媛꾩뿉 �썝�삎留� �꽑�뼵�빐�몺.
////////////////////////////////////////////////////////////////////////////////////////////

int Stop_line() {
	int line_cnt=0;
	int i;

	for (i = 0; i < 8; i++)	{
		if (!(LineSensor_Read()>>i &0x1)) {
			line_cnt++;
		}
	}
	if (line_cnt > 5) { //linesensor 5媛쒖씠�긽 媛먯��
		return 1;
	}
	
	//蹂댁셿 : linesensor媛� 5媛쒖씠�긽 �씛�깋(=0)�씠硫� 硫덉텛寃� 諛붽씀湲�
	return 0;
}
int IsStop(IplImage* imgResult) {
	int x, y;
	static int stop_camera = 0;
	int stop_IR = 0;
	int white_line;

	printf("stop\n");
	if (Stop_line() == 1 && stop_camera > 3) { ///stop_flag �쓽 怨꾩닔 3 ��� test�뵲�씪 蹂�寃�
		DesireSpeed_Write(0);
		stop_camera = 0;
		printf("stop\n");
		return 1;
	}
	for (y = 200; y < imgResult->height; y++) {
		for (x = 0; x < imgResult->width; x++) {
			if (imgResult->imageData[y*(imgResult->widthStep) + x] == 255) {
				//if (imgResult->imageData[y*(imgResult->widthStep) + x] == -1) {
				white_line++;
			}
		}
	}
	if (white_line > 800) {
		stop_camera++;
		printf("stop camera = %d\n", stop_camera);
	}
	return 0;
}

void Find_Center(IplImage* imgResult)		//TY add 6.27
{
	int i = 0;
	int j = 0;
	int k = 0;

	int y_start_line = 150;     //y_start_line怨� y_end_line 李⑤뒗 line_gap�쓽 諛곗닔�씠�뼱�빞 �븿.
	int y_end_line = 130;
	int y_high_start_line = 110;
	int y_high_end_line = 90;

	int valid_left_amount = 0;
	int valid_right_amount = 0;
	int valid_high_left_amount = 0;
	int valid_high_right_amount = 0;

	int left_line_start = 0;
	int left_line_end = 0;
	int right_line_start = 0;
	int right_line_end = 0;
	int bottom_line = 200;
	int line_tolerance = 70;
	int low_line_width = 20;
	int high_line_width = 10;
	int speed = 0;
	int curve_speed = 100;       //default : 60
	int straight_speed = 130;    //default : 90

	int line_gap = 5;  //line by line �뒪罹붿떆, lower line怨� upper line�쓽 李⑥씠�뒗 line_gap px
	int tolerance = 25; // center pixel +- tolerance px �궡�뿉�꽌 �씪�씤寃�異쒖떆 for臾� 醫낅즺 �슜�룄
	int high_tolerance = 60;
	int angle = 1500;
	float low_line_weight = 320; // control angle weight
	float high_line_weight = 80;
	float control_angle = 0;

	int left[240] = { 0 };
	int right[240] = { imgResult->width - 1 };
	float left_slope[2] = { 0.0 };
	float right_slope[2] = { 0.0 };

	bool continue_turn_left = false;
	bool continue_turn_right = false;

	printf("driving\n");

	for (i = y_start_line; i > y_end_line; i = i - line_gap) {
		if (turn_right_max == true)
			j = imgResult->width - 1;
		else
			j = (imgResult->width) / 2;
		for (; j > 0; j--) {                            //Searching the left line point
			left[y_start_line - i] = j;
			if (imgResult->imageData[i*imgResult->widthStep + j] == 255) {
				for (k = 0; k < low_line_width; k++) {                     //李⑥꽑�씠 line_width留뚰겮 �뿰�냽�쑝濡� �굹�삤�뒗吏� �솗�씤
					if (j - k <= 0)
						k = low_line_width - 1;
					else if (imgResult->imageData[i*imgResult->widthStep + j - k] == 255)
						continue;
					break;
				}
				if (k = low_line_width - 1) {
					valid_left_amount++;
					break;
				}
			}
		}
		if (turn_left_max == true)
			j = 0;
		else
			j = (imgResult->width) / 2;
		for (; j < imgResult->width; j++) {             //Searching the right line point
			right[y_start_line - i] = j;
			if (imgResult->imageData[i*imgResult->widthStep + j] == 255) {
				for (k = 0; k < low_line_width; k++) {                   //李⑥꽑�씠 line_width留뚰겮 �뿰�냽�쑝濡� �굹�삤�뒗吏� �솗�씤
					if (j + k >= imgResult->widthStep)
						k = low_line_width - 1;
					else if (imgResult->imageData[i*imgResult->widthStep + j + k] == 255)
						continue;
					break;
				}
				if (k = low_line_width - 1) {
					valid_right_amount++;
					break;
				}
			}
		}
		if (left[y_start_line - i] > ((imgResult->width / 2) - tolerance) || right[y_start_line - i] < ((imgResult->width / 2) + tolerance)) {     //寃�異쒕맂 李⑥꽑�씠 �솕硫댁쨷�븰遺�洹쇱뿉 �엳�뒗寃쎌슦, 李⑥꽑寃�異� 醫낅즺�썑 諛섎��諛⑺뼢�쑝濡� 理쒕��議고뼢 flag set
			if (valid_left_amount >= valid_right_amount && turn_left_max == false) {
				//printf("continue_turn_right set!\n");
				continue_turn_right = true;
			}
			else if (valid_right_amount >= valid_left_amount && turn_right_max == false) {
				//printf("continue_turn_left set!\n");
				continue_turn_left = true;
			}
			//printf("valid_left_amount = %d , valid_right_amount = %d \n", valid_left_amount, valid_right_amount);
			break;
		}
	}

	if (continue_turn_left == false && continue_turn_right == false) {          //turn max flag媛� false�씤 寃쎌슦 1. 吏곸꽑 2. 怨쇰떎怨≪꽑
		//printf("continue_turn_flag_off__1__\n");
		if (turn_left_max == true) {                                  //2. 怨쇰떎怨≪꽑�씤 寃쎌슦, 李⑥꽑�씠 �젙�긽寃�異쒕쾾�쐞�궡濡� �룎�븘�삱�븣源뚯�� �꽩 �쑀吏�
			for (i = imgResult->widthStep - 1; i > (imgResult->width / 2) + line_tolerance; i--) {
				if (imgResult->imageData[y_start_line*imgResult->widthStep + i] == 255) {
					continue_turn_left = false;
					//printf("continue_turn_flag_OFF__overCurve_left__\n");
					break;
				}
				else if (i == imgResult->width / 2 + line_tolerance + 1) {
					//printf("continue_turn_flag_ON__overCurve_left__\n");
					continue_turn_left = true;
					break;
				}
			}
		}
		else if (turn_right_max == true) {
			for (i = 0; i < (imgResult->width / 2) - line_tolerance; i++) {
				if (imgResult->imageData[y_start_line*imgResult->widthStep + i] == 255) {
					continue_turn_right = false;
					//printf("continue_turn_flag_OFF__2_right__i:%d\n", i);
					break;
				}
				else if (i == imgResult->width / 2 - line_tolerance - 1) {
					//printf("continue_turn_flag_ON__2_right__\n");
					continue_turn_right = true;
					break;
				}
			}
		}
	}

	if (continue_turn_left == false && continue_turn_right == false) {   //1. 吏곸꽑�씤 寃쎌슦, 議고뼢�쓣 �쐞�븳 醫뚯슦痢� 李⑥꽑 寃�異� �썑 湲곗슱湲� 怨꾩궛
		//printf("continue_turn_flag_all_off__3__\n");
		for (i = 0; i <= valid_left_amount; i++) {                        //醫뚯륫 李⑥꽑 寃�異�
			if (left[i*line_gap] != 0) {
				left_line_start = y_start_line - i * line_gap;
				left_line_end = y_start_line - (i + valid_left_amount) * line_gap;
				break;
			}
		}
		for (i = 0; i <= valid_right_amount; i++) {                        //�슦痢� 李⑥꽑 寃�異�
			if (right[i*line_gap] != imgResult->width - 1) {
				right_line_start = y_start_line - i * line_gap;
				right_line_end = y_start_line - (i + valid_right_amount) * line_gap;
				break;
			}
		}

		//printf("\nleft line = ");
		for (i = 0; i < valid_left_amount; i++)printf("%d  ", left[i*line_gap]);
		//printf("    valid left line = %d\n", valid_left_amount);
		//printf("right line = ");
		for (i = 0; i < valid_right_amount; i++)printf("%d ", right[i*line_gap]);
		//printf("    valid right line = %d\n", valid_right_amount);

		if (valid_left_amount > 1) {                                          //醫뚯륫 李⑥꽑 湲곗슱湲� 怨꾩궛
			left_slope[0] = (float)(left[0] - left[(valid_left_amount - 1)*line_gap]) / (float)(valid_left_amount*line_gap);
		}
		else left_slope[0] = 0;

		if (valid_right_amount > 1) {                                          //�슦痢� 李⑥꽑 湲곗슱湲� 怨꾩궛
			right_slope[0] = (float)(right[0] - right[(valid_right_amount - 1)*line_gap]) / (float)(valid_right_amount*line_gap);
		}
		else right_slope[0] = 0;

		control_angle = (left_slope[0] + right_slope[0])*low_line_weight;        //李⑤웾 議고뼢 湲곗슱湲� 怨꾩궛

		//printf("left_slope : %f ,right_slope : %f   	", left_slope[0], right_slope[0]);
		//printf("Control_Angle_low : %f \n\n", control_angle);
	}

	turn_left_max = continue_turn_left;             //�쁽�옱 �봽�젅�엫�뿉�꽌 理쒕��議고뼢�씠�씪怨� �뙋�떒�븷 寃쎌슦, 理쒕��議고뼢 �쟾�뿭蹂��닔 set.
	turn_right_max = continue_turn_right;

	if (turn_left_max == false && turn_right_max == false && control_angle == 0.0) {   //�븘�옯履쎌감�꽑 寃�異쒖떆�룄�썑, 吏곸쭊二쇳뻾 �븯�뒗 寃쎌슦,
		//printf("Does not detected low_lain\n");
		for (i = y_high_start_line; i > y_high_end_line; i = i - line_gap) {
			for (j = (imgResult->width) / 2; j > 0; j--) {                            //Searching the left line point
				left[y_high_start_line - i] = j;
				if (imgResult->imageData[i*imgResult->widthStep + j] == 255) {
					for (k = 0; k < high_line_width; k++) {                     //李⑥꽑�씠 line_width留뚰겮 �뿰�냽�쑝濡� �굹�삤�뒗吏� �솗�씤
						if (j - k <= 0)
							k = high_line_width - 1;
						else if (imgResult->imageData[i*imgResult->widthStep + j - k] == 255)
							continue;
						break;
					}
					if (k = high_line_width - 1) {
						valid_high_left_amount++;
						break;
					}
				}
			}
			for (j = (imgResult->width) / 2; j < imgResult->width; j++) {             //Searching the right line point
				right[y_high_start_line - i] = j;
				if (imgResult->imageData[i*imgResult->widthStep + j] == 255) {
					for (k = 0; k < high_line_width; k++) {                   //李⑥꽑�씠 line_width留뚰겮 �뿰�냽�쑝濡� �굹�삤�뒗吏� �솗�씤
						if (j + k >= imgResult->widthStep)
							k = high_line_width - 1;
						else if (imgResult->imageData[i*imgResult->widthStep + j + k] == 255)
							continue;
						break;
					}
					if (k = high_line_width - 1) {
						valid_high_right_amount++;
						break;
					}
				}
			}
			//   if(left[y_high_start_line-i]>((imgResult->width/2)-high_tolerance)||right[y_high_start_line-i]<((imgResult->width/2)+high_tolerance))     //寃�異쒕맂 李⑥꽑�씠 �솕硫댁쨷�븰遺�洹쇱뿉 �엳�뒗寃쎌슦, �븘�옯履쎌감�꽑源뚯�� �삱�닔�엳�룄濡� 臾댁떆
			//       break;
		}

		for (i = 0; i <= valid_high_left_amount; i++) {                        //醫뚯륫 李⑥꽑 寃�異�
			if (left[i*line_gap] != 0) {
				left_line_start = y_high_start_line - i * line_gap;
				left_line_end = y_high_start_line - (i + valid_high_left_amount) * line_gap;
				break;
			}
		}
		for (i = 0; i <= valid_high_right_amount; i++) {                        //�슦痢� 李⑥꽑 寃�異�
			if (right[i*line_gap] != imgResult->width - 1) {
				right_line_start = y_high_start_line - i * line_gap;
				right_line_end = y_high_start_line - (i + valid_right_amount) * line_gap;
				break;
			}
		}

		//printf("\nleft high line = ");
		for (i = 0; i < valid_high_left_amount; i++)printf("%d  ", left[i*line_gap]);
		//printf("    valid high left line = %d\n", valid_high_left_amount);
		//printf("right high line = ");
		for (i = 0; i < valid_high_right_amount; i++)printf("%d ", right[i*line_gap]);
		//printf("    valid high right line = %d\n", valid_high_right_amount);

		if (valid_high_left_amount > 1) {                                          //醫뚯륫 李⑥꽑 湲곗슱湲� 怨꾩궛
			left_slope[0] = (float)(left[0] - left[(valid_high_left_amount - 1)*line_gap]) / (float)(valid_high_left_amount*line_gap);
		}
		else left_slope[0] = 0;

		if (valid_high_right_amount > 1) {                                          //�슦痢� 李⑥꽑 湲곗슱湲� 怨꾩궛
			right_slope[0] = (float)(right[0] - right[(valid_high_right_amount - 1)*line_gap]) / (float)(valid_high_right_amount*line_gap);
		}
		else right_slope[0] = 0;

		control_angle = (left_slope[0] + right_slope[0])*high_line_weight;        //李⑤웾 議고뼢 湲곗슱湲� 怨꾩궛

		//printf("left_slope : %f ,right_slope : %f   	", left_slope[0], right_slope[0]);
		//printf("Control_Angle_high : %f \n\n", control_angle);

		if (abs(control_angle) > 100)    //�쐞履쎌감�꽑�뿉�꽌 怨쇳븯寃� 爰얠쓣寃쎌슦, 諛⑹�� ; 肄붾꼫�뿉�꽌 �씤肄붿뒪濡� �뱾�뼱�삤�뒗嫄� 諛⑹��
			control_angle = 0;

	}


	if (turn_left_max == true)                      //李⑤웾 議고뼢媛곷룄 �뙋蹂�
		angle = 2000;
	else if (turn_right_max == true)
		angle = 1000;
	else {
		angle = 1500 + control_angle;                                  // Range : 1000(Right)~1500(default)~2000(Left)
		angle = angle > 2000 ? 2000 : angle < 1000 ? 1000 : angle;           // Bounding the angle range
	}
	SteeringServoControl_Write(angle);

#ifdef SPEED_CONTROL
	if (angle < 1200 || angle>1800)      //吏곸꽑肄붿뒪�쓽 �냽�룄��� 怨≪꽑肄붿뒪�쓽 �냽�룄 �떎瑜닿쾶 �쟻�슜
		speed = curve_speed;
	else
		speed = straight_speed;
	DesireSpeed_Write(speed);
#endif

}
int Traffic_Light(IplImage* imgResult) {
	//char arr[131];
	//IplImage* result_img = cvCreateImage(cvGetSize(imgOrigin), IPL_DEPTH_8U, 1);
	//	IplImage* roi_img= cvCreateImage(cvSize(320,240), IPL_DEPTH_8U, 3);
	int x, y;
	int white_count = 0;
	int start_x = 60, end_x = 310;
	int start_y = 90, end_y = 190;

	CameraYServoControl_Write(1500);
	cvWaitKey(100);
	printf("color = %d\n", color);
	//result_img = color_check(origin_img, color);
	//cvNamedWindow("result", CV_WINDOW_AUTOSIZE);
	//cvShowImage("result", result_img);
	for (x = start_x; x < end_x; x++) {
		for (y = start_y; y < end_y; y++) {
			//result_img->imageData[y*(result_img->widthStep) + x] = 255;
			if (imgResult->imageData[y*(imgResult->widthStep) + x] == 255) {
				white_count++;
			}
		}
	}

	printf("white count %d color = %d\n", white_count, color);
	if (color == 3 && white_count < 200) {
		printf("�쉶�쟾援먯감濡�\n");
		return 3;
	}
	if (color == 5) {
		if (white_count > 290 && white_count < 500) {
			return 1;
		}
		else if (white_count >= 500) {
			return 2;
		}
	}
	else if (white_count > 800) {
		color++;
	}
	cvWaitKey(200);
	//cvReleaseImage(&origin_img);
	//cvReleaseImage(&result_img);

}
void After_Light(int direction) {
	CameraYServoControl_Write(1800);
	cvWaitKey(200);
	if (direction == 1) {//turn left
		printf("turn left\n");
	}
	else if (direction == 2) {//turn right
		printf("turn right\n");
	}
}
void *ControlThread(void *unused)
{
	int i = 0;
	static int count = 1;
	static int traffic = -1;
	char fileName[40];
	char fileName1[40];         // TY add 6.27
								//char fileName2[30];           // TY add 6.27
	NvMediaTime pt1 = { 0 }, pt2 = { 0 };
	NvU64 ptime1, ptime2;
	struct timespec;

	IplImage* imgOrigin;
	IplImage* imgResult;            // TY add 6.27
									//IplImage* imgCenter;          // TY add 6.27

									// cvCreateImage
	imgOrigin = cvCreateImage(cvSize(RESIZE_WIDTH, RESIZE_HEIGHT), IPL_DEPTH_8U, 3);
	imgResult = cvCreateImage(cvGetSize(imgOrigin), IPL_DEPTH_8U, 1);           // TY add 6.27
																				//imgCenter = cvCreateImage(cvGetSize(imgOrigin), IPL_DEPTH_8U, 1);         // TY add 6.27

	cvZero(imgResult);          // TY add 6.27
								//cvZero(imgCenter);            // TY add 6.27

	while (1)
	{

		if (color != 0 && color != 2) {
			color = color;
		}
		else if (count % 5 == 0) {//�씛 �젙吏��꽑
			color = 2;
			count = 1;
		}
		else {//�끂�옉�꽑
			color = 0;
			count++;
		}
		printf("count = %d\t color = %d\n", color);

		pthread_mutex_lock(&mutex);
		pthread_cond_wait(&cond, &mutex);

		GetTime(&pt1);
		ptime1 = (NvU64)pt1.tv_sec * 1000000000LL + (NvU64)pt1.tv_nsec;

		Frame2Ipl_color(imgOrigin, imgResult, color); // save image to IplImage structure & resize image from 720x480 to 320x240
													  // TY modified 6.27  Frame2Ipl(imgOrigin) -> Frame2Ipl(imgOrigin, imgResult)

		pthread_mutex_unlock(&mutex);

		// TODO : control steering angle based on captured image ---------------

		/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
		//////////////////////////////////////TY.留뚯빟 IMGSAVE(26踰덉㎏以�)媛� �젙�쓽�릺�뼱�엳�쑝硫� imgOrigin.png , imgResult.png �뙆�씪�쓣 captureImage�뤃�뜑濡� ����옣.
		//

		if (color == 0) {
			Find_Center(imgResult);
		}
		else if (color == 6) {
			After_Light(traffic);
		}
		else if (color >= 3) {
			traffic = Traffic_Light(imgResult);
			if (traffic == 3)//�쉶�쟾援먯감濡�
				color = 0;
			else if (traffic == 1 || traffic == 2) {
				color = 6;
			}
		}
		else if (color == 2) {
			if (IsStop(imgResult) == 1)
				color = 3;
		}

		//Find_Center(imgResult); // TY Centerline 寃�異쒗빐�꽌 議고뼢�빐二쇰뒗 �븣怨좊━利�
		/////////////////////////////////////  << 異뷀썑 議고뼢媛믩쭔 諛섑솚�븯怨�, �떎�젣議고뼢�븯�뒗 �븿�닔瑜� �뵲濡� 遺꾨━�빐二쇱뼱�빞�븿.

#ifdef IMGSAVE
		sprintf(fileName, "captureImage/imgOrigin%d.png", i);
		sprintf(fileName1, "captureImage/imgResult%d.png", i);          // TY add 6.27
																		//sprintf(fileName2, "captureImage/imgCenter%d.png", i);            // TY add 6.27


		cvSaveImage(fileName, imgOrigin, 0);
		cvSaveImage(fileName1, imgResult, 0);           // TY add 6.27
														//cvSaveImage(fileName2, imgCenter, 0);         // TY add 6.27
#endif

														//TY�꽕紐� �궡�슜
														//imgCenter�뒗 李⑥꽑寃�異� 諛� 議고뼢泥섎━ 寃곌낵瑜� �솗�씤�븯湲곗쐞�빐 �씠誘몄��濡� 異쒕젰�븷 寃쎌슦 �궗�슜�븷 �삁�젙.
														//imgCenter�뒗 �븘吏� 援ы쁽 �븞�릺�뼱�엳�쑝硫� �븘�슂�떆 �븘�옒�쓽 肄붾뱶 二쇱꽍泥섎━ �빐�젣�떆 �궗�슜媛��뒫
														//char fileName2[30] , IplImage* imgCenter, imgCenter = cvCreateImage(cvGetSize(imgOrigin),
														//IPL_DEPTH_8U, 1), cvZero(imgCenter), sprintf(fileName2, "captureImage/imgCenter%d.png", i), cvSaveImage(fileName2, imgCenter, 0)
														/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

														// ---------------------------------------------------------------------

		GetTime(&pt2);
		ptime2 = (NvU64)pt2.tv_sec * 1000000000LL + (NvU64)pt2.tv_nsec;
		printf("--------------------------------operation time=%llu.%09llu[s]\n", (ptime2 - ptime1) / 1000000000LL, (ptime2 - ptime1) % 1000000000LL);


		i++;
	}
}

int main(int argc, char *argv[])
{
	int err = -1;
	TestArgs testArgs;

	//////////////////////////////// TY add 6.27
	unsigned char status;         // To using CAR Control
	short speed;
	unsigned char gain;
	int position, position_now;
	short angle;
	int channel;
	int data;
	char sensor;
	int i, j;
	int tol;
	char byte = 0x80;
	////////////////////////////////

	CaptureInputHandle handle;

	NvMediaVideoCapture *vipCapture = NULL;
	NvMediaDevice *device = NULL;
	NvMediaVideoMixer *vipMixer = NULL;
	NvMediaVideoOutput *vipOutput[2] = { NULL, NULL };
	NvMediaVideoOutput *nullOutputList[1] = { NULL };
	FILE *vipFile = NULL;

	NvSemaphore *vipStartSem = NULL, *vipDoneSem = NULL;
	NvThread *vipThread = NULL;

	CaptureContext vipCtx;
	NvMediaBool deviceEnabled = NVMEDIA_FALSE;
	unsigned int displayId;

	pthread_t cntThread;

	signal(SIGINT, SignalHandler);

	memset(&testArgs, 0, sizeof(TestArgs));
	if (!ParseOptions(argc, argv, &testArgs))
		return -1;



	// 3. servo control ----------------------------------------------------------
#ifdef SERVO_CONTROL                                    //TY add 6.27

	printf("0. Car initializing \n");
	CarControlInit();


	printf("\n\n 0.1 servo control\n");

	//camera x servo set
	angle = 1500;                       // Range : 600(Right)~1200(default)~1800(Left)
	CameraXServoControl_Write(angle);
	//camera y servo set

	angle = 1800;                       // Range : 1200(Up)~1500(default)~1800(Down)
	CameraYServoControl_Write(angle);

	/*                              //Servo restoration(disable)
	angle = 1500;
	SteeringServoControl_Write(angle);
	CameraXServoControl_Write(angle);
	CameraYServoControl_Write(angle);
	*/
#endif

#ifdef SPEED_CONTROL
	// 2. speed control ---------------------------------------------------------- TY
	printf("\n\nspeed control\n");
	PositionControlOnOff_Write(UNCONTROL); // �뿏肄붾뜑 �븞�벐怨� 二쇳뻾�븷 寃쎌슦 UNCONTROL�꽭�똿
										   //control on/off
	SpeedControlOnOff_Write(CONTROL);
	//speed controller gain set            // PID range : 1~50 default : 20
	//P-gain
	gain = 20;
	SpeedPIDProportional_Write(gain);
	//I-gain
	gain = 20;
	SpeedPIDIntegral_Write(gain);
	//D-gain
	gain = 20;
	SpeedPIDDifferential_Write(gain);
	//speed set
	speed = 0;
	DesireSpeed_Write(speed);
#endif

#ifdef LIGHT_BEEP
	//0. light and beep Control --------------------------------------------------
	CarLight_Write(FRONT_ON);
	usleep(1000000);
#endif

	printf("1. Create NvMedia capture \n");
	// Create NvMedia capture(s)
	switch (testArgs.vipDeviceInUse)
	{
	case AnalogDevices_ADV7180:
		break;
	case AnalogDevices_ADV7182:
	{
		CaptureInputConfigParams params;

		params.width = testArgs.vipInputWidth;
		params.height = testArgs.vipInputHeight;
		params.vip.std = testArgs.vipInputtVideoStd;

		if (testutil_capture_input_open(testArgs.i2cDevice, testArgs.vipDeviceInUse, NVMEDIA_TRUE, &handle) < 0)
		{
			MESSAGE_PRINTF("Failed to open VIP device\n");
			goto fail;
		}

		if (testutil_capture_input_configure(handle, &params) < 0)
		{
			MESSAGE_PRINTF("Failed to configure VIP device\n");
			goto fail;
		}

		break;
	}
	default:
		MESSAGE_PRINTF("Bad VIP device\n");
		goto fail;
	}


	if (!(vipCapture = NvMediaVideoCaptureCreate(testArgs.vipInputtVideoStd, // interfaceFormat
		NULL, // settings
		VIP_BUFFER_SIZE)))// numBuffers
	{
		MESSAGE_PRINTF("NvMediaVideoCaptureCreate() failed for vipCapture\n");
		goto fail;
	}


	printf("2. Create NvMedia device \n");
	// Create NvMedia device
	if (!(device = NvMediaDeviceCreate()))
	{
		MESSAGE_PRINTF("NvMediaDeviceCreate() failed\n");
		goto fail;
	}

	printf("3. Create NvMedia mixer(s) and output(s) and bind them \n");
	// Create NvMedia mixer(s) and output(s) and bind them
	unsigned int features = 0;


	features |= NVMEDIA_VIDEO_MIXER_FEATURE_VIDEO_SURFACE_TYPE_YV16X2;
	features |= NVMEDIA_VIDEO_MIXER_FEATURE_PRIMARY_VIDEO_DEINTERLACING; // Bob the 16x2 format by default
	if (testArgs.vipOutputType != NvMediaVideoOutputType_OverlayYUV)
		features |= NVMEDIA_VIDEO_MIXER_FEATURE_DVD_MIXING_MODE;

	if (!(vipMixer = NvMediaVideoMixerCreate(device, // device
		testArgs.vipMixerWidth, // mixerWidth
		testArgs.vipMixerHeight, // mixerHeight
		testArgs.vipAspectRatio, //sourceAspectRatio
		testArgs.vipInputWidth, // primaryVideoWidth
		testArgs.vipInputHeight, // primaryVideoHeight
		0, // secondaryVideoWidth
		0, // secondaryVideoHeight
		0, // graphics0Width
		0, // graphics0Height
		0, // graphics1Width
		0, // graphics1Height
		features, // features
		nullOutputList))) // outputList
	{
		MESSAGE_PRINTF("NvMediaVideoMixerCreate() failed for vipMixer\n");
		goto fail;
	}

	printf("4. Check that the device is enabled (initialized) \n");
	// Check that the device is enabled (initialized)
	CheckDisplayDevice(
		testArgs.vipOutputDevice[0],
		&deviceEnabled,
		&displayId);

	if ((vipOutput[0] = NvMediaVideoOutputCreate(testArgs.vipOutputType, // outputType
		testArgs.vipOutputDevice[0], // outputDevice
		NULL, // outputPreference
		deviceEnabled, // alreadyCreated
		displayId, // displayId
		NULL))) // displayHandle
	{
		if (NvMediaVideoMixerBindOutput(vipMixer, vipOutput[0], NVMEDIA_OUTPUT_DEVICE_0) != NVMEDIA_STATUS_OK)
		{
			MESSAGE_PRINTF("Failed to bind VIP output to mixer\n");
			goto fail;
		}
	}
	else
	{
		MESSAGE_PRINTF("NvMediaVideoOutputCreate() failed for vipOutput\n");
		goto fail;
	}



	printf("5. Open output file(s) \n");
	// Open output file(s)
	if (testArgs.vipFileDumpEnabled)
	{
		vipFile = fopen(testArgs.vipOutputFileName, "w");
		if (!vipFile || ferror(vipFile))
		{
			MESSAGE_PRINTF("Error opening output file for VIP\n");
			goto fail;
		}
	}

	printf("6. Create vip pool(s), queue(s), fetch threads and stream start/done semaphores \n");
	// Create vip pool(s), queue(s), fetch threads and stream start/done semaphores
	if (NvSemaphoreCreate(&vipStartSem, 0, 1) != RESULT_OK)
	{
		MESSAGE_PRINTF("NvSemaphoreCreate() failed for vipStartSem\n");
		goto fail;
	}

	if (NvSemaphoreCreate(&vipDoneSem, 0, 1) != RESULT_OK)
	{
		MESSAGE_PRINTF("NvSemaphoreCreate() failed for vipDoneSem\n");
		goto fail;
	}

	vipCtx.name = VIP_NAME;

	vipCtx.semStart = vipStartSem;
	vipCtx.semDone = vipDoneSem;

	vipCtx.capture = vipCapture;
	vipCtx.mixer = vipMixer;
	vipCtx.fout = vipFile;

	vipCtx.inputWidth = testArgs.vipInputWidth;
	vipCtx.inputHeight = testArgs.vipInputHeight;

	vipCtx.timeout = VIP_FRAME_TIMEOUT_MS;

	vipCtx.displayEnabled = testArgs.vipDisplayEnabled;
	vipCtx.fileDumpEnabled = testArgs.vipFileDumpEnabled;

	if (testArgs.vipCaptureTime)
	{
		vipCtx.timeNotCount = NVMEDIA_TRUE;
		vipCtx.last = testArgs.vipCaptureTime;
	}
	else
	{
		vipCtx.timeNotCount = NVMEDIA_FALSE;
		vipCtx.last = testArgs.vipCaptureCount;
	}


	if (NvThreadCreate(&vipThread, CaptureThread, &vipCtx, NV_THREAD_PRIORITY_NORMAL) != RESULT_OK)
	{
		MESSAGE_PRINTF("NvThreadCreate() failed for vipThread\n");
		goto fail;
	}

	printf("wait for ADV7182 ... one second\n");
	sleep(1);

	printf("7. Kickoff \n");
	// Kickoff
	NvMediaVideoCaptureStart(vipCapture); \
		NvSemaphoreIncrement(vipStartSem);

	printf("8. Control Thread\n");
	pthread_create(&cntThread, NULL, &ControlThread, NULL);

	printf("9. Wait for completion \n");
	// Wait for completion
	NvSemaphoreDecrement(vipDoneSem, NV_TIMEOUT_INFINITE);


	err = 0;

fail: // Run down sequence
	  // Destroy vip threads and stream start/done semaphores
	if (vipThread)
		NvThreadDestroy(vipThread);
	if (vipDoneSem)
		NvSemaphoreDestroy(vipDoneSem);
	if (vipStartSem)
		NvSemaphoreDestroy(vipStartSem);

	printf("10. Close output file(s) \n");
	// Close output file(s)
	if (vipFile)
		fclose(vipFile);

	// Unbind NvMedia mixer(s) and output(s) and destroy them
	if (vipOutput[0])
	{
		NvMediaVideoMixerUnbindOutput(vipMixer, vipOutput[0], NULL);
		NvMediaVideoOutputDestroy(vipOutput[0]);
	}
	if (vipOutput[1])
	{
		NvMediaVideoMixerUnbindOutput(vipMixer, vipOutput[1], NULL);
		NvMediaVideoOutputDestroy(vipOutput[1]);
	}
	if (vipMixer)
		NvMediaVideoMixerDestroy(vipMixer);


	// Destroy NvMedia device
	if (device)
		NvMediaDeviceDestroy(device);

	// Destroy NvMedia capture(s)
	if (vipCapture)
	{
		NvMediaVideoCaptureDestroy(vipCapture);

		// Reset VIP settings of the board
		switch (testArgs.vipDeviceInUse)
		{
		case AnalogDevices_ADV7180: // TBD
			break;
		case AnalogDevices_ADV7182: // TBD
									//testutil_capture_input_close(handle);
			break;
		default:
			break;
		}
	}

	return err;
}
