/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * H.264 Bitmap Compression
 *
 * Copyright 2022 Ely Ronnen <elyronnen@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <winpr/wlog.h>
#include <winpr/assert.h>
#include <winpr/library.h>

#include <freerdp/log.h>
#include <freerdp/codec/h264.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImageReader.h>

#include "h264.h"

static const char* CODEC_NAME = "video/avc";

static const int COLOR_FormatYUV420Planar = 19;
static const int COLOR_FormatYUV420Flexible = 0x7f420888;

/* https://developer.android.com/reference/android/media/MediaCodec#qualityFloor */
static const int MEDIACODEC_MINIMUM_WIDTH = 320;
static const int MEDIACODEC_MINIMUM_HEIGHT = 240;

typedef struct
{
	AMediaCodec* decoder;
	AMediaFormat* inputFormat;
	AMediaFormat* outputFormat;
	int32_t width;
	int32_t height;
	int32_t outputWidth;
	int32_t outputHeight;

	ANativeWindow* nativeWindow;
    AImageReader* imageReader;
	AImage* currnetImage;
} H264_CONTEXT_MEDIACODEC;

static AMediaFormat* mediacodec_format_new(wLog* log, int width, int height)
{
	const char* media_format;
	AMediaFormat* format = AMediaFormat_new();
	if (format == NULL)
	{
		WLog_Print(log, WLOG_ERROR, "AMediaFormat_new failed");
		return NULL;
	}

	AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, CODEC_NAME);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_FormatYUV420Planar);

	media_format = AMediaFormat_toString(format);
	if (media_format == NULL)
	{
		WLog_Print(log, WLOG_ERROR, "AMediaFormat_toString failed");
		AMediaFormat_delete(format);
		return NULL;
	}

	WLog_Print(log, WLOG_DEBUG, "MediaCodec configuring with desired output format [%s]",
	           media_format);

	return format;
}

static void set_mediacodec_format(H264_CONTEXT* h264, AMediaFormat** formatVariable,
                                  AMediaFormat* newFormat)
{
	media_status_t status = AMEDIA_OK;
	H264_CONTEXT_MEDIACODEC* sys;

	WINPR_ASSERT(h264);
	WINPR_ASSERT(formatVariable);

	sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
	WINPR_ASSERT(sys);

	if (*formatVariable == newFormat)
		return;

	if (*formatVariable != NULL)
	{
		status = AMediaFormat_delete(*formatVariable);
		if (status != AMEDIA_OK)
		{
			WLog_Print(h264->log, WLOG_ERROR, "Error AMediaFormat_delete %d", status);
		}
	}

	*formatVariable = newFormat;
}

static int update_mediacodec_inputformat(H264_CONTEXT* h264)
{
	H264_CONTEXT_MEDIACODEC* sys;
	AMediaFormat* inputFormat;
	const char* mediaFormatName;

	WINPR_ASSERT(h264);

	sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
	WINPR_ASSERT(sys);

#if __ANDROID__ >= 21
	inputFormat = AMediaCodec_getInputFormat(sys->decoder);
	if (inputFormat == NULL)
	{
		WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_getInputFormat failed");
		return -1;
	}
#else
	inputFormat = sys->inputFormat;
#endif
	set_mediacodec_format(h264, &sys->inputFormat, inputFormat);

	mediaFormatName = AMediaFormat_toString(sys->inputFormat);
	if (mediaFormatName == NULL)
	{
		WLog_Print(h264->log, WLOG_ERROR, "AMediaFormat_toString failed");
		return -1;
	}
	WLog_Print(h264->log, WLOG_DEBUG, "Using MediaCodec with input MediaFormat [%s]",
	           mediaFormatName);

	return 1;
}

static int update_mediacodec_outputformat(H264_CONTEXT* h264)
{
	H264_CONTEXT_MEDIACODEC* sys;
	media_status_t status;
	AMediaFormat* outputFormat;
	const char* mediaFormatName;
	int32_t outputWidth, outputHeight;

	WINPR_ASSERT(h264);

	sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
	WINPR_ASSERT(sys);

	outputFormat = AMediaCodec_getOutputFormat(sys->decoder);
	if (outputFormat == NULL)
	{
		WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_getOutputFormat failed");
		return -1;
	}
	set_mediacodec_format(h264, &sys->outputFormat, outputFormat);

	mediaFormatName = AMediaFormat_toString(sys->outputFormat);
	if (mediaFormatName == NULL)
	{
		WLog_Print(h264->log, WLOG_ERROR, "AMediaFormat_toString failed");
		return -1;
	}
	WLog_Print(h264->log, WLOG_DEBUG, "Using MediaCodec with output MediaFormat [%s]",
	           mediaFormatName);

	if (!AMediaFormat_getInt32(sys->outputFormat, AMEDIAFORMAT_KEY_WIDTH, &outputWidth))
	{
		WLog_Print(h264->log, WLOG_ERROR, "fnAMediaFormat_getInt32 failed getting width");
		return -1;
	}

	if (!AMediaFormat_getInt32(sys->outputFormat, AMEDIAFORMAT_KEY_HEIGHT, &outputHeight))
	{
		WLog_Print(h264->log, WLOG_ERROR, "fnAMediaFormat_getInt32 failed getting height");
		return -1;
	}

	if (sys->outputWidth == outputWidth && sys->outputHeight == outputHeight)
	{
		return 1;
	}

	if (sys->imageReader != NULL)
	{
		sys->nativeWindow = NULL;
		AImageReader_delete(sys->imageReader);
	}

	status = AImageReader_new(outputWidth, outputHeight, AIMAGE_FORMAT_YUV_420_888, 1, &sys->imageReader);
	if (status != AMEDIA_OK || sys->imageReader == NULL)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AImageReader_new failed: %d", status);
        return -1;
    }

	status = AImageReader_getWindow(sys->imageReader, &sys->nativeWindow);
    if (status != AMEDIA_OK || sys->nativeWindow == NULL)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AImageReader_getWindow failed: %d", status);
        return -1;
    }

	status = AMediaCodec_setOutputSurface(sys->decoder, sys->nativeWindow);
	if (status != AMEDIA_OK)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_setOutputSurface failed: %d", status);
        return -1;
    }

	sys->outputWidth = outputWidth;
	sys->outputHeight = outputHeight;

	return 1;
}

static void release_current_outputbuffer(H264_CONTEXT* h264)
{
	media_status_t status = AMEDIA_OK;
	H264_CONTEXT_MEDIACODEC* sys;

	WINPR_ASSERT(h264);
	sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
	WINPR_ASSERT(sys);

	if (sys->currnetImage != NULL)
    {
        AImage_delete(sys->currnetImage);
    	sys->currnetImage = NULL;
    }
}

static int mediacodec_compress(H264_CONTEXT* h264, const BYTE** pSrcYuv, const UINT32* pStride,
                               BYTE** ppDstData, UINT32* pDstSize)
{
	WINPR_ASSERT(h264);
	WINPR_ASSERT(pSrcYuv);
	WINPR_ASSERT(pStride);
	WINPR_ASSERT(ppDstData);
	WINPR_ASSERT(pDstSize);

	WLog_Print(h264->log, WLOG_ERROR, "MediaCodec is not supported as an encoder");
	return -1;
}

static int mediacodec_decompress(H264_CONTEXT* h264, const BYTE* pSrcData, UINT32 SrcSize)
{
	ssize_t inputBufferId = -1;
	size_t inputBufferSize, outputBufferSize;
	uint8_t* inputBuffer;
	media_status_t status;
	BYTE** pYUVData;
	UINT32* iStride;
	H264_CONTEXT_MEDIACODEC* sys;
	int i = 0;

	WINPR_ASSERT(h264);
	WINPR_ASSERT(pSrcData);

	sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
	WINPR_ASSERT(sys);

	pYUVData = h264->pYUVData;
	WINPR_ASSERT(pYUVData);

	iStride = h264->iStride;
	WINPR_ASSERT(iStride);

	WLog_Print(h264->log, WLOG_DEBUG, "MediaCodec decompressing frame");
	release_current_outputbuffer(h264);

	if (sys->width != h264->width || sys->height != h264->height)
	{
		sys->width = h264->width;
		sys->height = h264->height;

		if (sys->width < MEDIACODEC_MINIMUM_WIDTH || sys->height < MEDIACODEC_MINIMUM_HEIGHT)
		{
			WLog_Print(h264->log, WLOG_ERROR,
			           "MediaCodec got width or height smaller than minimum [%d,%d]", sys->width,
			           sys->height);
			return -1;
		}

		WLog_Print(h264->log, WLOG_DEBUG, "MediaCodec setting new input width and height [%d,%d]",
		           sys->width, sys->height);

#if __ANDROID__ >= 26
		AMediaFormat_setInt32(sys->inputFormat, AMEDIAFORMAT_KEY_WIDTH, sys->width);
		AMediaFormat_setInt32(sys->inputFormat, AMEDIAFORMAT_KEY_HEIGHT, sys->height);
		status = AMediaCodec_setParameters(sys->decoder, sys->inputFormat);
		if (status != AMEDIA_OK)
		{
			WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_setParameters failed: %d", status);
			return -1;
		}
#else
		set_mediacodec_format(h264, &sys->inputFormat,
		                      mediacodec_format_new(h264->log, sys->width, sys->height));
#endif

		// The codec can change output width and height
		if (update_mediacodec_outputformat(h264) < 0)
		{
			WLog_Print(h264->log, WLOG_ERROR, "MediaCodec failed updating input format");
			return -1;
		}
	}

	while (true)
	{
		UINT32 inputBufferCurrnetOffset = 0;
		while (inputBufferCurrnetOffset < SrcSize)
		{
			UINT32 numberOfBytesToCopy = SrcSize - inputBufferCurrnetOffset;
			inputBufferId = AMediaCodec_dequeueInputBuffer(sys->decoder, -1);
			if (inputBufferId < 0)
			{
				WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_dequeueInputBuffer failed [%d]",
				           inputBufferId);
				// TODO: sleep?
				continue;
			}

			inputBuffer = AMediaCodec_getInputBuffer(sys->decoder, inputBufferId, &inputBufferSize);
			if (inputBuffer == NULL)
			{
				WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_getInputBuffer failed");
				return -1;
			}

			if (numberOfBytesToCopy > inputBufferSize)
			{
				WLog_Print(h264->log, WLOG_WARN,
				           "MediaCodec inputBufferSize: got [%d] but wanted [%d]", inputBufferSize,
				           numberOfBytesToCopy);
				numberOfBytesToCopy = inputBufferSize;
			}

			memcpy(inputBuffer, pSrcData + inputBufferCurrnetOffset, numberOfBytesToCopy);
			inputBufferCurrnetOffset += numberOfBytesToCopy;

			status = AMediaCodec_queueInputBuffer(sys->decoder, inputBufferId, 0,
			                                      numberOfBytesToCopy, 0, 0);
			if (status != AMEDIA_OK)
			{
				WLog_Print(h264->log, WLOG_ERROR, "Error AMediaCodec_queueInputBuffer %d", status);
				return -1;
			}
		}

		while (true)
		{
			AMediaCodecBufferInfo bufferInfo;
			AImage* image = NULL;
			int32_t numberOfPlanes = 0;
			int32_t imageWidth, imageHeight, imageFormat;
			ssize_t outputBufferId =
			    AMediaCodec_dequeueOutputBuffer(sys->decoder, &bufferInfo, -1);
			WLog_Print(h264->log, WLOG_INFO, "MediaCodec dequeued output buffer with timestamp [%lld], flags [%d]", bufferInfo.presentationTimeUs, bufferInfo.flags);
			
			if (outputBufferId >= 0)
			{
				WLog_Print(h264->log, WLOG_INFO, "MediaCodec releasing output buffer with render=true");
				status = AMediaCodec_releaseOutputBuffer(sys->decoder, outputBufferId, true);
				if (status != AMEDIA_OK)
				{
					WLog_Print(h264->log, WLOG_ERROR, "Error AMediaCodec_releaseOutputBuffer with decoded buffer %d", status);
					return -1;
				}

				while (true)
				{
					WLog_Print(h264->log, WLOG_INFO, "MediaCodec getting latest image buffer");
					status = AImageReader_acquireNextImage(sys->imageReader, &image);
					if (status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE)
					{
						WLog_Print(h264->log, WLOG_WARN, "MediaCodec AImageReader_acquireLatestImage no buffer available");
						Sleep(1);
						continue;
					}
					if (status != AMEDIA_OK || image == NULL)
					{
						WLog_Print(h264->log, WLOG_ERROR, "AImageReader_acquireLatestImage failed: %d", status);
						return -1;
					}

					break;
				}

				WLog_Print(h264->log, WLOG_INFO, "MediaCodec got latest image buffer");

				status = AImage_getNumberOfPlanes(image, &numberOfPlanes);
				if (status != AMEDIA_OK)
				{
					WLog_Print(h264->log, WLOG_ERROR, "Error AImage_getNumberOfPlanes %d", status);
					return -1;
				}
				WLog_Print(h264->log, WLOG_INFO, "MediaCodec got number of planes: [%d]", numberOfPlanes);

				status = AImage_getFormat(image, &imageFormat);
				if (status != AMEDIA_OK)
				{
					WLog_Print(h264->log, WLOG_ERROR, "Error AImage_getFormat %d", status);
					return -1;
				}
				WLog_Print(h264->log, WLOG_INFO, "MediaCodec got format: [%d]", imageFormat);

				status = AImage_getWidth(image, &imageWidth);
				if (status != AMEDIA_OK)
				{
					WLog_Print(h264->log, WLOG_ERROR, "Error AImage_getWidth %d", status);
					return -1;
				}
				WLog_Print(h264->log, WLOG_INFO, "MediaCodec got width: [%d]", imageWidth);

				status = AImage_getHeight(image, &imageHeight);
				if (status != AMEDIA_OK)
				{
					WLog_Print(h264->log, WLOG_ERROR, "Error AImage_getHeight %d", status);
					return -1;
				}
				WLog_Print(h264->log, WLOG_INFO, "MediaCodec got height: [%d]", imageHeight);

				for (i = 0; i < 3; i++)
				{
					int dataLength;
					int32_t pixelStride;
					status = AImage_getPlaneData(image, i, &pYUVData[i], &dataLength);
					if (status != AMEDIA_OK)
					{
						WLog_Print(h264->log, WLOG_ERROR, "AImage_getPlaneData failed: %d", status);
					}
					WLog_Print(h264->log, WLOG_INFO, "MediaCodec got plane [%d] data: [%p],[%d]", i, pYUVData[i], dataLength);

					status = AImage_getPlaneRowStride(image, i, &iStride[i]);
					if (status != AMEDIA_OK)
					{
						WLog_Print(h264->log, WLOG_ERROR, "AImage_getPlaneRowStride failed: %d", status);
					}
					WLog_Print(h264->log, WLOG_INFO, "MediaCodec got plane [%d] stride: [%d]", i, iStride[i]);

					status = AImage_getPlanePixelStride(image, i, &pixelStride);
					if (status != AMEDIA_OK)
					{
						WLog_Print(h264->log, WLOG_ERROR, "AImage_getPlanePixelStride failed: %d", status);
					}
					WLog_Print(h264->log, WLOG_INFO, "MediaCodec got plane [%d] pixel stride: [%d]", i, pixelStride);
				}

				sys->currnetImage = image;
				break;
			}
			else if (outputBufferId == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
			{
				if (update_mediacodec_outputformat(h264) < 0)
				{
					WLog_Print(h264->log, WLOG_ERROR,
					           "MediaCodec failed updating output format in decompress");
					return -1;
				}
			}
			else if (outputBufferId == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
			{
				WLog_Print(h264->log, WLOG_WARN,
				           "AMediaCodec_dequeueOutputBuffer need to try again later");
				// TODO: sleep?
			}
			else if (outputBufferId == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
			{
				WLog_Print(h264->log, WLOG_WARN,
				           "AMediaCodec_dequeueOutputBuffer returned deprecated value "
				           "AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED, ignoring");
			}
			else
			{
				WLog_Print(h264->log, WLOG_ERROR,
				           "AMediaCodec_dequeueOutputBuffer returned unknown value [%d]",
				           outputBufferId);
				return -1;
			}
		}

		break;
	}

	return 1;
}

static void mediacodec_uninit(H264_CONTEXT* h264)
{
	media_status_t status = AMEDIA_OK;
	H264_CONTEXT_MEDIACODEC* sys;

	WINPR_ASSERT(h264);

	sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;

	WLog_Print(h264->log, WLOG_DEBUG, "Uninitializing MediaCodec");

	if (!sys)
		return;

	if (sys->decoder != NULL)
	{
		release_current_outputbuffer(h264);
		status = AMediaCodec_stop(sys->decoder);
		if (status != AMEDIA_OK)
		{
			WLog_Print(h264->log, WLOG_ERROR, "Error AMediaCodec_stop %d", status);
		}

		status = AMediaCodec_delete(sys->decoder);
		if (status != AMEDIA_OK)
		{
			WLog_Print(h264->log, WLOG_ERROR, "Error AMediaCodec_delete %d", status);
		}

		sys->decoder = NULL;
	}

	if (sys->imageReader != NULL)
    {
		sys->nativeWindow = NULL;
        AImageReader_delete(sys->imageReader);   
    }

	set_mediacodec_format(h264, &sys->inputFormat, NULL);
	set_mediacodec_format(h264, &sys->outputFormat, NULL);

	free(sys);
	h264->pSystemData = NULL;
}

static BOOL mediacodec_init(H264_CONTEXT* h264)
{
	H264_CONTEXT_MEDIACODEC* sys;
	media_status_t status;

	WINPR_ASSERT(h264);

	if (h264->Compressor)
	{
		WLog_Print(h264->log, WLOG_ERROR, "MediaCodec is not supported as an encoder");
		goto EXCEPTION;
	}

	WLog_Print(h264->log, WLOG_DEBUG, "Initializing MediaCodec");

	sys = (H264_CONTEXT_MEDIACODEC*)calloc(1, sizeof(H264_CONTEXT_MEDIACODEC));

	if (!sys)
	{
		goto EXCEPTION;
	}

	h264->pSystemData = (void*)sys;

	// updated when we're given the height and width for the first time
	sys->width = sys->outputWidth = MEDIACODEC_MINIMUM_WIDTH;
	sys->height = sys->outputHeight = MEDIACODEC_MINIMUM_HEIGHT;
	sys->decoder = AMediaCodec_createDecoderByType(CODEC_NAME);
	if (sys->decoder == NULL)
	{
		WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_createCodecByName failed");
		goto EXCEPTION;
	}

#if __ANDROID_API__ >= 28
	char* codec_name;
	status = AMediaCodec_getName(sys->decoder, &codec_name);
	if (status != AMEDIA_OK)
	{
		WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_getName failed: %d", status);
		goto EXCEPTION;
	}

	WLog_Print(h264->log, WLOG_DEBUG, "MediaCodec using %s codec [%s]", CODEC_NAME, codec_name);
	AMediaCodec_releaseName(sys->decoder, codec_name);

	status = AImageReader_new(sys->width, sys->height, AIMAGE_FORMAT_YUV_420_888, 1, &sys->imageReader);
	if (status != AMEDIA_OK || sys->imageReader == NULL)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AImageReader_new failed: %d", status);
        goto EXCEPTION;
    }

	status = AImageReader_getWindow(sys->imageReader, &sys->nativeWindow);
    if (status != AMEDIA_OK || sys->nativeWindow == NULL)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AImageReader_getWindow failed: %d", status);
        goto EXCEPTION;
    }

	sys->inputFormat = AMediaFormat_new();
	if (sys->inputFormat == NULL)
	{
		WLog_Print(h264->log, WLOG_ERROR, "AMediaFormat_new failed");
		goto EXCEPTION;
	}

	AMediaFormat_setString(sys->inputFormat, AMEDIAFORMAT_KEY_MIME, "video/avc");
	AMediaFormat_setInt32(sys->inputFormat, AMEDIAFORMAT_KEY_WIDTH, sys->width);
	AMediaFormat_setInt32(sys->inputFormat, AMEDIAFORMAT_KEY_HEIGHT, sys->height);
	AMediaFormat_setInt32(sys->inputFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_FormatYUV420Flexible);
	AMediaFormat_setInt32(sys->inputFormat, "allow-frame-drop", 0);

	set_mediacodec_format(h264, &sys->inputFormat,
	                      mediacodec_format_new(h264->log, sys->width, sys->height));

	status = AMediaCodec_configure(sys->decoder, sys->inputFormat, sys->nativeWindow, NULL, 0);
	if (status != AMEDIA_OK)
	{
		WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_configure failed: %d", status);
		goto EXCEPTION;
	}

	if (update_mediacodec_inputformat(h264) < 0)
	{
		WLog_Print(h264->log, WLOG_ERROR, "MediaCodec failed updating input format");
		goto EXCEPTION;
	}

	if (update_mediacodec_outputformat(h264) < 0)
	{
		WLog_Print(h264->log, WLOG_ERROR, "MediaCodec failed updating output format");
		goto EXCEPTION;
	}

	WLog_Print(h264->log, WLOG_DEBUG, "Starting MediaCodec");
	status = AMediaCodec_start(sys->decoder);
	if (status != AMEDIA_OK)
	{
		WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_start failed %d", status);
		goto EXCEPTION;
	}

	return TRUE;
EXCEPTION:
	mediacodec_uninit(h264);
	return FALSE;
}

H264_CONTEXT_SUBSYSTEM g_Subsystem_mediacodec = { "MediaCodec", mediacodec_init, mediacodec_uninit,
	                                              mediacodec_decompress, mediacodec_compress };
