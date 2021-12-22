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
    AMediaFormat* format;
};
typedef struct _H264_CONTEXT_MEDIACODEC H264_CONTEXT_MEDIACODEC;

static int set_mediacodec_format(H264_CONTEXT* h264, AMediaFormat* format)
{
    H264_CONTEXT_MEDIACODEC* sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
    media_status_t status = AMEDIA_OK;

    if (sys->format != NULL)
    {
        status = AMediaFormat_delete(sys->format);
        if (status != AMEDIA_OK)
        {
            WLog_Print(h264->log, WLOG_ERROR, "Error AMediaFormat_delete %d", status);
        }
    }

    sys->format = format;
}

static int mediacodec_compress(H264_CONTEXT* h264, const BYTE** pSrcYuv, const UINT32* pStride,
                               BYTE** ppDstData, UINT32* pDstSize)
{
    WLog_Print(h264->log, WLOG_ERROR, "MediaCodec is not supported as an encoder");
    return -1;
}

static int mediacodec_decompress(H264_CONTEXT* h264, const BYTE* pSrcData, UINT32 SrcSize)
{
    WLog_Print(h264->log, WLOG_ERROR, "MediaCodec not implemented yet bitch");
    return -1;
}

static void mediacodec_uninit(H264_CONTEXT* h264)
{
    H264_CONTEXT_MEDIACODEC* sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
    media_status_t status = AMEDIA_OK;

	if (!sys)
		return;

    if (sys->decoder != NULL)
    {
        status = AMediaCodec_delete(sys->decoder);
        if (status != AMEDIA_OK)
        {
            WLog_Print(h264->log, WLOG_ERROR, "Error AMediaCodec_delete %d", status);
        }

        sys->decoder = NULL;
    }

    set_mediacodec_format(h264, NULL);

    free(sys);
	h264->pSystemData = NULL;
}

static BOOL mediacodec_init(H264_CONTEXT* h264)
{
    H264_CONTEXT_MEDIACODEC* sys;
    media_status_t status;
    AMediaFormat* output_format;

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

    char* codec_name;
    status = AMediaCodec_getName(sys->decoder, &codec_name);
    if (status != AMEDIA_OK)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_getName failed: %d", status);
        goto EXCEPTION;
    }

    WLog_Print(h264->log, WLOG_INFO, "MediaCodec using video/avc codec [%s]", codec_name);
    AMediaCodec_releaseName(sys->decoder, codec_name);

    sys->format = AMediaFormat_new();
    if (sys->format == NULL)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AMediaFormat_new failed");
        goto EXCEPTION;
    }

    AMediaFormat_setString(sys->format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(sys->format, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(sys->format, AMEDIAFORMAT_KEY_HEIGHT, 1088);
    //AMediaFormat_setInt32(sys->format, AMEDIAFORMAT_KEY_COLOR_FORMAT, AIMAGE_FORMAT_YUV_420_888);

    WLog_Print(h264->log, WLOG_INFO, "Configuring MediaCodec");
    status = AMediaCodec_configure(sys->decoder, sys->format, NULL, NULL, 0);
    if (status != AMEDIA_OK)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_configure failed: %d", status);
        goto EXCEPTION;
    }

    output_format = AMediaCodec_getOutputFormat(sys->decoder);
    if (output_format == NULL)
    {
        WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_getOutputFormat failed");
        goto EXCEPTION;
    }

    set_mediacodec_format(h264, output_format);

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