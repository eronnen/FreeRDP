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
#include <freerdp/log.h>
#include <freerdp/codec/h264.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include "h264.h"

const int COLOR_FormatYUV420Planar = 19;
const int COLOR_FormatYUV420Flexible = 0x7f420888;

struct _H264_CONTEXT_MEDIACODEC
{
    AMediaCodec* decoder;
    AImageReader* imageReader;
    AMediaFormat* inputFormat;
    AMediaFormat* outputFormat;
    int32_t width;
    int32_t height;
    ssize_t currentOutputBufferIndex;
    AImage* currnetImage;
};

typedef struct _H264_CONTEXT_MEDIACODEC H264_CONTEXT_MEDIACODEC;

static void set_mediacodec_format(H264_CONTEXT* h264, AMediaFormat** formatVariable, AMediaFormat* newFormat)
{
    media_status_t status = AMEDIA_OK;

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

static void release_current_outputbuffer(H264_CONTEXT* h264)
{
    H264_CONTEXT_MEDIACODEC* sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
    media_status_t status = AMEDIA_OK;

    if (sys->currentOutputBufferIndex < 0)
    {
        return;
    }

    WLog_Print(h264->log, WLOG_ERROR, "MediaCodec releasing output buffer %d", sys->currentOutputBufferIndex);
    status = AMediaCodec_releaseOutputBuffer(sys->decoder, sys->currentOutputBufferIndex, FALSE);
    if (status != AMEDIA_OK)
    {
        WLog_Print(h264->log, WLOG_ERROR, "Error AMediaCodec_releaseOutputBuffer %d", status);
    }

    sys->currentOutputBufferIndex = -1;

    if (sys->currnetImage == NULL)
    {
        return;
    }

    AImage_delete(sys->currnetImage);
    sys->currnetImage = NULL;
}

static int mediacodec_compress(H264_CONTEXT* h264, const BYTE** pSrcYuv, const UINT32* pStride,
                               BYTE** ppDstData, UINT32* pDstSize)
{
    WLog_Print(h264->log, WLOG_ERROR, "MediaCodec is not supported as an encoder");
    return -1;
}

static int mediacodec_decompress(H264_CONTEXT* h264, const BYTE* pSrcData, UINT32 SrcSize)
{
    H264_CONTEXT_MEDIACODEC* sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
    ssize_t inputBufferId = -1;
    size_t inputBufferSize, outputBufferSize;
    uint8_t* inputBuffer;
    media_status_t status;
    const char* media_format;
    BYTE** pYUVData = h264->pYUVData;
	UINT32* iStride = h264->iStride;

    release_current_outputbuffer(h264);

    if (sys->width == 0)
    {
        int32_t width = h264->width;
        int32_t height = h264->height;
        if (width % 16 != 0)
            width += 16 - width % 16;
        if (height % 16 != 0)
            height += 16 - height % 16;

        WLog_Print(h264->log, WLOG_INFO, "MediaCodec setting width and height [%d,%d]", width, height);
        AMediaFormat_setInt32(sys->inputFormat, AMEDIAFORMAT_KEY_WIDTH, width);
        AMediaFormat_setInt32(sys->inputFormat, AMEDIAFORMAT_KEY_HEIGHT, height);
        
        status = AMediaCodec_setParameters(sys->decoder, sys->inputFormat);
        if (status != AMEDIA_OK)
        {
            WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_setParameters failed: %d", status);
            return -1;
        }

        sys->width = width;
        sys->height = height;
    }

    while (true)
    {
        WLog_Print(h264->log, WLOG_INFO, "MediaCodec calling AMediaCodec_dequeueInputBuffer");
        inputBufferId = AMediaCodec_dequeueInputBuffer(sys->decoder, -1);
        WLog_Print(h264->log, WLOG_INFO, "MediaCodec decompress AMediaCodec_dequeueInputBuffer returned [%d]", inputBufferId);
        if (inputBufferId < 0)
        {
            WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_dequeueInputBuffer failed [%d]", inputBufferId);
            // TODO: sleep
            continue;
        }

        WLog_Print(h264->log, WLOG_INFO, "MediaCodec getting input buffer [%d]", inputBufferId);
        inputBuffer = AMediaCodec_getInputBuffer(sys->decoder, inputBufferId, &inputBufferSize);
        WLog_Print(h264->log, WLOG_INFO, "MediaCodec got input buffer: [%p, %d]", inputBuffer, inputBufferSize);
        if (inputBuffer == NULL)
        {
            WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_getInputBuffer failed");
            return -1;
        }

        if (SrcSize > inputBufferSize)
        {
            WLog_Print(h264->log, WLOG_ERROR, "MediaCodec input buffer size is too small: got [%d] but wanted [%d]", inputBufferSize, SrcSize);
            return -1;
        }

        WLog_Print(h264->log, WLOG_INFO, "MediaCodec copying [%d] bytes to the input buffer of size [%d]", SrcSize, inputBufferSize);
        memcpy(inputBuffer, pSrcData, SrcSize);
        WLog_Print(h264->log, WLOG_INFO, "MediaCodec queing input buffer [%d]", inputBufferId);
        status = AMediaCodec_queueInputBuffer(sys->decoder, inputBufferId, 0, SrcSize, 0, 0);
        if (status != AMEDIA_OK)
        {
            WLog_Print(h264->log, WLOG_ERROR, "Error AMediaCodec_queueInputBuffer %d", status);
            return -1;
        }

        while (true)
        {
            AMediaCodecBufferInfo bufferInfo;
            WLog_Print(h264->log, WLOG_INFO, "MediaCodec dequeing output buffer");
            ssize_t outputBufferId = AMediaCodec_dequeueOutputBuffer(sys->decoder, &bufferInfo, -1);
            WLog_Print(h264->log, WLOG_INFO, "MediaCodec got dequeued output buffer [%d]", outputBufferId);
            //ssize_t outputBufferId = 0;
            if (outputBufferId >= 0)
            {
                sys->currentOutputBufferIndex = outputBufferId;

                // uint8_t* outputBuffer;
                // WLog_Print(h264->log, WLOG_INFO, "MediaCodec getting output buffer");
                // outputBuffer = AMediaCodec_getOutputBuffer(sys->decoder, outputBufferId, &outputBufferSize);
                // WLog_Print(h264->log, WLOG_INFO, "MediaCodec got output buffer [%p,%d]", outputBuffer, outputBufferSize);
                // sys->currentOutputBufferIndex = outputBufferId;

                // if (outputBufferSize != (sys->width * sys->height + ((sys->width + 1) / 2) * ((sys->height + 1) / 2) * 2))
                // {
                //     WLog_Print(h264->log, WLOG_ERROR, "Error unexpected output buffer size %d", outputBufferSize);
                //     return -1;
                // }

                // iStride[0] = sys->width;
                // iStride[1] = (sys->width + 1) / 2;
                // iStride[2] = (sys->width + 1) / 2;
                // pYUVData[0] = outputBuffer;
                // pYUVData[1] = outputBuffer + iStride[0] * sys->height;
                // pYUVData[2] = outputBuffer + iStride[0] * sys->height + iStride[1] * ((sys->height + 1) / 2);
                
                AImage* image = NULL;
                int i = 0;
                int32_t numberOfPlanes = 0;
                while (true)
                {
                    WLog_Print(h264->log, WLOG_INFO, "MediaCodec getting latest image buffer");
                    status = AImageReader_acquireLatestImage(sys->imageReader, &image);
                    if (status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE)
                    {
                        WLog_Print(h264->log, WLOG_INFO, "MediaCodec AImageReader_acquireLatestImage no buffer available");
                        Sleep(1000);
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
                WLog_Print(h264->log, WLOG_INFO, "MediaCodec got number of planes: [%d]", numberOfPlanes);

                for (i = 0; i < 3; i++)
                {
                    int dataLength;
                    WLog_Print(h264->log, WLOG_INFO, "MediaCodec getting plane [%d] data", i);
                    status = AImage_getPlaneData(image, i, &pYUVData[i], &dataLength);
                    if (status != AMEDIA_OK)
                    {
                        WLog_Print(h264->log, WLOG_ERROR, "AImage_getPlaneData failed: %d", status);
                    }
                    WLog_Print(h264->log, WLOG_INFO, "MediaCodec got plane [%d] data: [%d]", dataLength);

                    WLog_Print(h264->log, WLOG_INFO, "MediaCodec getting plane [%d] stride", i);
                    status = AImage_getPlaneRowStride(image, i, &iStride[i]);
                    if (status != AMEDIA_OK)
                    {
                        WLog_Print(h264->log, WLOG_ERROR, "AImage_getPlaneRowStride failed: %d", status);
                    }
                    WLog_Print(h264->log, WLOG_INFO, "MediaCodec got plane [%d] stride: [%d]", iStride[i]);
                }

                break;
            }
            else if (outputBufferId ==  AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
            {
                AMediaFormat* outputFormat;
                WLog_Print(h264->log, WLOG_INFO, "MediaCodec Output format changed, getting new one");
                outputFormat = AMediaCodec_getOutputFormat(sys->decoder);
                if (outputFormat == NULL)
                {
                    WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_getOutputFormat failed");
                    return -1;
                }
                set_mediacodec_format(h264, &sys->outputFormat, outputFormat);
                
                media_format = AMediaFormat_toString(sys->outputFormat);
                if (media_format == NULL)
                {
                    WLog_Print(h264->log, WLOG_ERROR, "AMediaFormat_toString failed");
                    return -1;
                }
                WLog_Print(h264->log, WLOG_INFO, "Updated MediaCodec output format to [%s]", media_format);
            }
            else if (outputBufferId ==  AMEDIACODEC_INFO_TRY_AGAIN_LATER)
            {
                WLog_Print(h264->log, WLOG_WARN, "AMediaCodec_dequeueOutputBuffer need to try again later");
                // TODO: sleep
            }
            else if (outputBufferId == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
            {
                WLog_Print(h264->log, WLOG_WARN, "AMediaCodec_dequeueOutputBuffer returned deprecated value AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED, ignoring");
            }
            else
            {
                WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_dequeueOutputBuffer returned unknown value [%d]", outputBufferId);
                return -1;
            }
        }

        break;
    }

    WLog_Print(h264->log, WLOG_INFO, "MediaCodec decompressed successfully");
    return 1;
}

static void mediacodec_uninit(H264_CONTEXT* h264)
{
    H264_CONTEXT_MEDIACODEC* sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
    media_status_t status = AMEDIA_OK;

    WLog_Print(h264->log, WLOG_INFO, "Uninitializing MediaCodec");

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
        AImageReader_delete(sys->imageReader);   
    }

    set_mediacodec_format(h264, &sys->inputFormat, NULL);
    set_mediacodec_format(h264, &sys->outputFormat, NULL);

    WLog_Print(h264->log, WLOG_INFO, "Done uninitializing MediaCodec");

    free(sys);
	h264->pSystemData = NULL;
}

static BOOL mediacodec_init(H264_CONTEXT* h264)
{
    H264_CONTEXT_MEDIACODEC* sys;
    media_status_t status;
    char* codec_name, *media_format;
    AMediaFormat* inputFormat, *outputFormat;
    ANativeWindow* imageReaderNativeWindow = NULL;

    if (h264->Compressor)
    {
        WLog_Print(h264->log, WLOG_ERROR, "MediaCodec is not supported as an encoder");
        goto EXCEPTION;
    }

    WLog_Print(h264->log, WLOG_INFO, "Initializing MediaCodec");

	sys = (H264_CONTEXT_MEDIACODEC*)calloc(1, sizeof(H264_CONTEXT_MEDIACODEC));

	if (!sys)
	{
		goto EXCEPTION;
	}

	h264->pSystemData = (void*)sys;

    sys->currentOutputBufferIndex = -1;
    sys->currnetImage = NULL;
    sys->width = sys->height = 0; // update when we're given the height and width
    sys->width = 1920; 
    sys->height = 1088; // update when we're given the height and width
    sys->decoder = AMediaCodec_createDecoderByType("video/avc");
    if (sys->decoder == NULL)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_createCodecByName failed");
        goto EXCEPTION;
    }

    status = AMediaCodec_getName(sys->decoder, &codec_name);
    if (status != AMEDIA_OK)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_getName failed: %d", status);
        goto EXCEPTION;
    }

    WLog_Print(h264->log, WLOG_INFO, "MediaCodec using video/avc codec [%s]", codec_name);
    AMediaCodec_releaseName(sys->decoder, codec_name);

    status = AImageReader_new(1920, 1088, AIMAGE_FORMAT_YUV_420_888, 2, &sys->imageReader);
    if (status != AMEDIA_OK || sys->imageReader == NULL)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AImageReader_new failed: %d", status);
        goto EXCEPTION;
    }

    status = AImageReader_getWindow(sys->imageReader, &imageReaderNativeWindow);
    if (status != AMEDIA_OK || imageReaderNativeWindow == NULL)
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
    AMediaFormat_setInt32(sys->inputFormat, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(sys->inputFormat, AMEDIAFORMAT_KEY_HEIGHT, 1088);
    AMediaFormat_setInt32(sys->inputFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_FormatYUV420Flexible);
    //AMediaFormat_setInt32(sys->inputFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_FormatYUV420Planar);
    AMediaFormat_setInt32(sys->inputFormat, "allow-frame-drop", 0);
    AMediaFormat_setInt32(sys->inputFormat, AMEDIAFORMAT_KEY_PUSH_BLANK_BUFFERS_ON_STOP, 1);

    media_format = AMediaFormat_toString(sys->inputFormat);
    if (media_format == NULL)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AMediaFormat_toString failed");
        goto EXCEPTION;
    }

    WLog_Print(h264->log, WLOG_INFO, "Configuring MediaCodec with input MediaFormat [%s]", media_format);
    status = AMediaCodec_configure(sys->decoder, sys->inputFormat, imageReaderNativeWindow, NULL, 0);
    //status = AMediaCodec_configure(sys->decoder, sys->inputFormat, NULL, NULL, 0);
    if (status != AMEDIA_OK)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_configure failed: %d", status);
        goto EXCEPTION;
    }


    inputFormat = AMediaCodec_getInputFormat(sys->decoder);
    if (inputFormat == NULL)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_getInputFormat failed");
        return -1;
    }
    set_mediacodec_format(h264, &sys->inputFormat, inputFormat);
    
    media_format = AMediaFormat_toString(sys->inputFormat);
    if (media_format == NULL)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AMediaFormat_toString failed");
        goto EXCEPTION;
    }
    WLog_Print(h264->log, WLOG_INFO, "Using MediaCodec with input MediaFormat [%s]", media_format);

    outputFormat = AMediaCodec_getOutputFormat(sys->decoder);
    if (outputFormat == NULL)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_getOutputFormat failed");
        return -1;
    }
    set_mediacodec_format(h264, &sys->outputFormat, outputFormat);
    
    media_format = AMediaFormat_toString(sys->outputFormat);
    if (media_format == NULL)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AMediaFormat_toString failed");
        goto EXCEPTION;
    }
    WLog_Print(h264->log, WLOG_INFO, "Using MediaCodec with output MediaFormat [%s]", media_format);


    WLog_Print(h264->log, WLOG_INFO, "Starting MediaCodec");
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