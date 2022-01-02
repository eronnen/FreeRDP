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

#include "h264.h"

struct _H264_CONTEXT_MEDIACODEC
{
    AMediaCodec* decoder;
    AMediaFormat* inputFormat;
    AMediaFormat* outputFormat;
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

    while (true)
    {
        WLog_Print(h264->log, WLOG_INFO, "MediaCodec calling AMediaCodec_dequeueInputBuffer");
        inputBufferId = AMediaCodec_dequeueInputBuffer(sys->decoder, -1);
        WLog_Print(h264->log, WLOG_INFO, "MediaCodec decompress AMediaCodec_dequeueInputBuffer returned [%d]", inputBufferId);
        if (inputBufferId < 0)
        {
            WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_dequeueInputBuffer failed [%d]", inputBufferId);
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

        while (true)
        {
            AMediaCodecBufferInfo bufferInfo;
            WLog_Print(h264->log, WLOG_INFO, "MediaCodec dequeing output buffer");
            ssize_t outputBufferId = AMediaCodec_dequeueOutputBuffer(sys->decoder, &bufferInfo, -1);
            WLog_Print(h264->log, WLOG_INFO, "MediaCodec got dequeued output buffer [%d]", outputBufferId);
            
            if (outputBufferId >= 0)
            {
                uint8_t* outputBuffer;
                WLog_Print(h264->log, WLOG_INFO, "MediaCodec getting output buffer");
                outputBuffer = AMediaCodec_getOutputBuffer(sys->decoder, outputBufferId, &outputBufferSize);
                WLog_Print(h264->log, WLOG_INFO, "MediaCodec got output buffer 0x[%p]", outputBuffer);
                status = AMediaCodec_releaseOutputBuffer(sys->decoder, outputBuffer, 0);
                break;
            }
            else if (outputBufferId ==  AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
            {
                AMediaFormat* outputFormat;
                WLog_Print(h264->log, WLOG_INFO, "MediaCodec Output buffer changed, getting new one");
                outputFormat = AMediaCodec_getOutputFormat(sys->decoder);
                if (outputFormat == NULL)
                {
                    WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_getOutputFormat failed");
                    return -1;
                }
                set_mediacodec_format(h264, &sys->outputFormat, outputFormat);
            }
            else if (outputBufferId ==  AMEDIACODEC_INFO_TRY_AGAIN_LATER)
            {
                WLog_Print(h264->log, WLOG_WARN, "AMediaCodec_dequeueOutputBuffer need to try again later");
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

    set_mediacodec_format(h264, sys->inputFormat, NULL);
    set_mediacodec_format(h264, sys->outputFormat, NULL);

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

    sys->inputFormat = AMediaFormat_new();
    if (sys->inputFormat == NULL)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AMediaFormat_new failed");
        goto EXCEPTION;
    }

    AMediaFormat_setString(sys->inputFormat, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(sys->inputFormat, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(sys->inputFormat, AMEDIAFORMAT_KEY_HEIGHT, 1088);
    AMediaFormat_setInt32(sys->inputFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT, AIMAGE_FORMAT_YUV_420_888);

    media_format = AMediaFormat_toString(sys->inputFormat);
    if (media_format == NULL)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AMediaFormat_toString failed");
        goto EXCEPTION;
    }

    WLog_Print(h264->log, WLOG_INFO, "Configuring MediaCodec with input MediaFormat [%s]", media_format);
    status = AMediaCodec_configure(sys->decoder, sys->inputFormat, NULL, NULL, 0);
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