/*
 *
 * Copyright 2012 Samsung Electronics S.LSI Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * @file        Exynos_OMX_Vdec.c
 * @brief
 * @author      SeungBeom Kim (sbcrux.kim@samsung.com)
 *              HyeYeon Chung (hyeon.chung@samsung.com)
 *              Yunji Kim (yunji.kim@samsung.com)
 * @version     2.0.0
 * @history
 *   2012.02.20 : Create
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Exynos_OMX_Macros.h"
#include "Exynos_OSAL_Event.h"
#include "Exynos_OMX_Vdec.h"
#include "Exynos_OMX_VdecControl.h"
#include "Exynos_OMX_Basecomponent.h"
#include "Exynos_OSAL_SharedMemory.h"
#include "Exynos_OSAL_Thread.h"
#include "Exynos_OSAL_Semaphore.h"
#include "Exynos_OSAL_Mutex.h"
#include "Exynos_OSAL_ETC.h"

#ifdef USE_ANB
#include "Exynos_OSAL_Android.h"
#endif

#include "ExynosVideoApi.h"
#include "csc.h"

#undef  EXYNOS_LOG_TAG
#define EXYNOS_LOG_TAG    "EXYNOS_VIDEO_DEC"
#define EXYNOS_LOG_OFF
//#define EXYNOS_TRACE_ON
#include "Exynos_OSAL_Log.h"

inline void Exynos_UpdateFrameSize(OMX_COMPONENTTYPE *pOMXComponent)
{
    EXYNOS_OMX_BASECOMPONENT *pExynosComponent = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    EXYNOS_OMX_BASEPORT      *exynosInputPort = &pExynosComponent->pExynosPort[INPUT_PORT_INDEX];
    EXYNOS_OMX_BASEPORT      *exynosOutputPort = &pExynosComponent->pExynosPort[OUTPUT_PORT_INDEX];

    if ((exynosOutputPort->portDefinition.format.video.nFrameWidth !=
            exynosInputPort->portDefinition.format.video.nFrameWidth) ||
        (exynosOutputPort->portDefinition.format.video.nFrameHeight !=
            exynosInputPort->portDefinition.format.video.nFrameHeight)) {
        OMX_U32 width = 0, height = 0;

        exynosOutputPort->portDefinition.format.video.nFrameWidth =
            exynosInputPort->portDefinition.format.video.nFrameWidth;
        exynosOutputPort->portDefinition.format.video.nFrameHeight =
            exynosInputPort->portDefinition.format.video.nFrameHeight;
        width = exynosOutputPort->portDefinition.format.video.nStride =
            exynosInputPort->portDefinition.format.video.nStride;
        height = exynosOutputPort->portDefinition.format.video.nSliceHeight =
            exynosInputPort->portDefinition.format.video.nSliceHeight;

        switch(exynosOutputPort->portDefinition.format.video.eColorFormat) {
        case OMX_COLOR_FormatYUV420Planar:
        case OMX_COLOR_FormatYUV420SemiPlanar:
        case OMX_SEC_COLOR_FormatNV12Tiled:
        case OMX_SEC_COLOR_FormatYVU420Planar:
        case OMX_SEC_COLOR_FormatNV21Linear:
            if (width && height)
                exynosOutputPort->portDefinition.nBufferSize = (width * height * 3) / 2;
            break;
        default:
            if (width && height)
                exynosOutputPort->portDefinition.nBufferSize = width * height * 2;
            break;
        }
    }

    return;
}

void Exynos_Free_CodecBuffers(
    OMX_COMPONENTTYPE   *pOMXComponent,
    OMX_U32              nPortIndex)
{
    OMX_ERRORTYPE                    ret                = OMX_ErrorNone;
    EXYNOS_OMX_BASECOMPONENT        *pExynosComponent   = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    EXYNOS_OMX_VIDEODEC_COMPONENT   *pVideoDec          = (EXYNOS_OMX_VIDEODEC_COMPONENT *)pExynosComponent->hComponentHandle;
    CODEC_DEC_BUFFER               **ppCodecBuffer      = NULL;

    OMX_U32 nBufferCnt = 0, nPlaneCnt = 0;
    int i, j;

    FunctionIn();

    if (nPortIndex == INPUT_PORT_INDEX) {
        ppCodecBuffer = &(pVideoDec->pMFCDecInputBuffer[0]);
        nBufferCnt = MFC_INPUT_BUFFER_NUM_MAX;
    } else {
        ppCodecBuffer = &(pVideoDec->pMFCDecOutputBuffer[0]);
        nBufferCnt = MFC_OUTPUT_BUFFER_NUM_MAX;
    }
    nPlaneCnt = pExynosComponent->pExynosPort[nPortIndex].nPlaneCnt;

    for (i = 0; i < nBufferCnt; i++) {
        if (ppCodecBuffer[i] != NULL) {
            for (j = 0; j < nPlaneCnt; j++) {
                if (ppCodecBuffer[i]->pVirAddr[j] != NULL)
                    Exynos_OSAL_SharedMemory_Free(pVideoDec->hSharedMemory, ppCodecBuffer[i]->pVirAddr[j]);
            }

            Exynos_OSAL_Free(ppCodecBuffer[i]);
            ppCodecBuffer[i] = NULL;
        }
    }

    FunctionOut();
}

OMX_ERRORTYPE Exynos_Allocate_CodecBuffers(
    OMX_COMPONENTTYPE   *pOMXComponent,
    OMX_U32              nPortIndex,
    OMX_U32              nBufferCnt,
    OMX_U32              nPlaneSize[MAX_BUFFER_PLANE])
{
    OMX_ERRORTYPE                    ret                = OMX_ErrorNone;
    EXYNOS_OMX_BASECOMPONENT        *pExynosComponent   = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    EXYNOS_OMX_VIDEODEC_COMPONENT   *pVideoDec          = (EXYNOS_OMX_VIDEODEC_COMPONENT *)pExynosComponent->hComponentHandle;
    MEMORY_TYPE                      eMemoryType        = SYSTEM_MEMORY;
    CODEC_DEC_BUFFER               **ppCodecBuffer      = NULL;

    OMX_U32 nPlaneCnt = 0;
    int i, j;

    FunctionIn();

    if (nPortIndex == INPUT_PORT_INDEX) {
        ppCodecBuffer = &(pVideoDec->pMFCDecInputBuffer[0]);
    } else {
        ppCodecBuffer = &(pVideoDec->pMFCDecOutputBuffer[0]);
#ifdef USE_CSC_HW
        eMemoryType = NORMAL_MEMORY;
#endif
    }
    nPlaneCnt = pExynosComponent->pExynosPort[nPortIndex].nPlaneCnt;

    if (pVideoDec->bDRMPlayerMode == OMX_TRUE)
        eMemoryType = SECURE_MEMORY;

    for (i = 0; i < nBufferCnt; i++) {
        ppCodecBuffer[i] = (CODEC_DEC_BUFFER *)Exynos_OSAL_Malloc(sizeof(CODEC_DEC_BUFFER));
        if (ppCodecBuffer[i] == NULL) {
            Exynos_OSAL_Log(EXYNOS_LOG_ERROR, "Failed to Alloc codec buffer");
            ret = OMX_ErrorInsufficientResources;
            goto EXIT;
        }
        Exynos_OSAL_Memset(ppCodecBuffer[i], 0, sizeof(CODEC_DEC_BUFFER));

        for (j = 0; j < nPlaneCnt; j++) {
            ppCodecBuffer[i]->pVirAddr[j] =
                (void *)Exynos_OSAL_SharedMemory_Alloc(pVideoDec->hSharedMemory, nPlaneSize[j], eMemoryType);
            if (ppCodecBuffer[i]->pVirAddr[j] == NULL) {
                Exynos_OSAL_Log(EXYNOS_LOG_ERROR, "Failed to Alloc plane");
                ret = OMX_ErrorInsufficientResources;
                goto EXIT;
            }

            ppCodecBuffer[i]->fd[j] =
                Exynos_OSAL_SharedMemory_VirtToION(pVideoDec->hSharedMemory, ppCodecBuffer[i]->pVirAddr[j]);
            ppCodecBuffer[i]->bufferSize[j] = nPlaneSize[j];
            Exynos_OSAL_Log(EXYNOS_LOG_TRACE, "PORT[%d]: pMFCCodecBuffer[%d]->pVirAddr[%d]: 0x%x", nPortIndex, i, j, ppCodecBuffer[i]->pVirAddr[j]);
        }

        ppCodecBuffer[i]->dataSize = 0;
    }

    return OMX_ErrorNone;

EXIT:
    Exynos_Free_CodecBuffers(pOMXComponent, nPortIndex);

    FunctionOut();

    return ret;
}

OMX_BOOL Exynos_Check_BufferProcess_State(EXYNOS_OMX_BASECOMPONENT *pExynosComponent, OMX_U32 nPortIndex)
{
    OMX_BOOL ret = OMX_FALSE;

    if ((pExynosComponent->currentState == OMX_StateExecuting) &&
        (pExynosComponent->pExynosPort[nPortIndex].portState == OMX_StateIdle) &&
        (pExynosComponent->transientState != EXYNOS_OMX_TransStateExecutingToIdle) &&
        (pExynosComponent->transientState != EXYNOS_OMX_TransStateIdleToExecuting)) {
        ret = OMX_TRUE;
    } else {
        ret = OMX_FALSE;
    }

    return ret;
}

OMX_ERRORTYPE Exynos_ResetAllPortConfig(OMX_COMPONENTTYPE *pOMXComponent)
{
    OMX_ERRORTYPE                  ret               = OMX_ErrorNone;
    EXYNOS_OMX_BASECOMPONENT      *pExynosComponent  = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    EXYNOS_OMX_BASEPORT           *pInputPort        = &pExynosComponent->pExynosPort[INPUT_PORT_INDEX];
    EXYNOS_OMX_BASEPORT           *pOutputPort       = &pExynosComponent->pExynosPort[OUTPUT_PORT_INDEX];

    /* Input port */
    pInputPort->portDefinition.format.video.nFrameWidth             = DEFAULT_FRAME_WIDTH;
    pInputPort->portDefinition.format.video.nFrameHeight            = DEFAULT_FRAME_HEIGHT;
    pInputPort->portDefinition.format.video.nStride                 = 0; /*DEFAULT_FRAME_WIDTH;*/
    pInputPort->portDefinition.format.video.nSliceHeight            = 0;
    pInputPort->portDefinition.format.video.pNativeRender           = 0;
    pInputPort->portDefinition.format.video.bFlagErrorConcealment   = OMX_FALSE;
    pInputPort->portDefinition.format.video.eColorFormat            = OMX_COLOR_FormatUnused;

    pInputPort->portDefinition.nBufferSize  = DEFAULT_VIDEO_INPUT_BUFFER_SIZE;
    pInputPort->portDefinition.bEnabled     = OMX_TRUE;

    pInputPort->bufferProcessType   = BUFFER_SHARE;
    pInputPort->portWayType         = WAY2_PORT;
    pInputPort->nPlaneCnt           = MFC_DEFAULT_INPUT_BUFFER_PLANE;

    /* Output port */
    pOutputPort->portDefinition.format.video.nFrameWidth            = DEFAULT_FRAME_WIDTH;
    pOutputPort->portDefinition.format.video.nFrameHeight           = DEFAULT_FRAME_HEIGHT;
    pOutputPort->portDefinition.format.video.nStride                = 0; /*DEFAULT_FRAME_WIDTH;*/
    pOutputPort->portDefinition.format.video.nSliceHeight           = 0;
    pOutputPort->portDefinition.format.video.pNativeRender          = 0;
    pOutputPort->portDefinition.format.video.bFlagErrorConcealment  = OMX_FALSE;
    pOutputPort->portDefinition.format.video.eColorFormat           = OMX_COLOR_FormatYUV420Planar;

    pOutputPort->portDefinition.nBufferSize  = DEFAULT_VIDEO_OUTPUT_BUFFER_SIZE;
    pOutputPort->portDefinition.bEnabled     = OMX_TRUE;

    pOutputPort->bufferProcessType  = BUFFER_COPY | BUFFER_ANBSHARE;
    pOutputPort->bIsANBEnabled      = OMX_FALSE;
    pOutputPort->portWayType        = WAY2_PORT;
    pOutputPort->nPlaneCnt          = Exynos_OSAL_GetPlaneCount(OMX_COLOR_FormatYUV420Planar);

    return ret;
}

OMX_ERRORTYPE Exynos_Input_CodecBufferToData(EXYNOS_OMX_BASECOMPONENT *pExynosComponent, OMX_PTR codecBuffer, EXYNOS_OMX_DATA *pData)
{
    OMX_ERRORTYPE                  ret = OMX_ErrorNone;
    EXYNOS_OMX_VIDEODEC_COMPONENT *pVideoDec = (EXYNOS_OMX_VIDEODEC_COMPONENT *)pExynosComponent->hComponentHandle;
    CODEC_DEC_BUFFER *pInputCodecBuffer = (CODEC_DEC_BUFFER *)codecBuffer;

    pData->buffer.singlePlaneBuffer.dataBuffer = pInputCodecBuffer->pVirAddr[0];
    pData->buffer.singlePlaneBuffer.fd = pInputCodecBuffer->fd[0];
    pData->allocSize     = pInputCodecBuffer->bufferSize[0];
    pData->dataLen       = pInputCodecBuffer->dataSize;
    pData->usedDataLen   = 0;
    pData->remainDataLen = pInputCodecBuffer->dataSize;

    pData->nFlags        = 0;
    pData->timeStamp     = 0;
    pData->pPrivate      = codecBuffer;
    pData->bufferHeader  = NULL;

    return ret;
}

OMX_ERRORTYPE Exynos_Output_CodecBufferToData(
    CODEC_DEC_BUFFER    *pCodecBuffer,
    EXYNOS_OMX_DATA     *pData)
{
    OMX_ERRORTYPE                  ret = OMX_ErrorNone;

    pData->buffer.multiPlaneBuffer.dataBuffer[0] = pCodecBuffer->pVirAddr[0];
    pData->buffer.multiPlaneBuffer.dataBuffer[1] = pCodecBuffer->pVirAddr[1];
    pData->buffer.multiPlaneBuffer.dataBuffer[2] = pCodecBuffer->pVirAddr[2];
    pData->buffer.multiPlaneBuffer.fd[0] = pCodecBuffer->fd[0];
    pData->buffer.multiPlaneBuffer.fd[1] = pCodecBuffer->fd[1];
    pData->buffer.multiPlaneBuffer.fd[2] = pCodecBuffer->fd[2];
    pData->allocSize     = pCodecBuffer->bufferSize[0] + pCodecBuffer->bufferSize[1] + pCodecBuffer->bufferSize[2];
    pData->dataLen       = 0;
    pData->usedDataLen   = 0;
    pData->remainDataLen = 0;

    pData->nFlags        = 0;
    pData->timeStamp     = 0;
    pData->pPrivate      = pCodecBuffer;
    pData->bufferHeader  = NULL;

    return ret;
}

void Exynos_Wait_ProcessPause(EXYNOS_OMX_BASECOMPONENT *pExynosComponent, OMX_U32 nPortIndex)
{
    EXYNOS_OMX_BASEPORT *exynosOMXInputPort  = &pExynosComponent->pExynosPort[INPUT_PORT_INDEX];
    EXYNOS_OMX_BASEPORT *exynosOMXOutputPort = &pExynosComponent->pExynosPort[OUTPUT_PORT_INDEX];
    EXYNOS_OMX_BASEPORT *exynosOMXPort = NULL;

    FunctionIn();

    exynosOMXPort = &pExynosComponent->pExynosPort[nPortIndex];

    if (((pExynosComponent->currentState == OMX_StatePause) ||
        (pExynosComponent->currentState == OMX_StateIdle) ||
        (pExynosComponent->transientState == EXYNOS_OMX_TransStateLoadedToIdle) ||
        (pExynosComponent->transientState == EXYNOS_OMX_TransStateExecutingToIdle)) &&
        (pExynosComponent->transientState != EXYNOS_OMX_TransStateIdleToLoaded) &&
        (!CHECK_PORT_BEING_FLUSHED(exynosOMXPort))) {
        Exynos_OSAL_SignalWait(pExynosComponent->pExynosPort[nPortIndex].pauseEvent, DEF_MAX_WAIT_TIME);
        Exynos_OSAL_SignalReset(pExynosComponent->pExynosPort[nPortIndex].pauseEvent);
    }

    FunctionOut();

    return;
}

OMX_BOOL Exynos_CSC_OutputData(OMX_COMPONENTTYPE *pOMXComponent, EXYNOS_OMX_DATA *dstOutputData)
{
    OMX_BOOL                       ret              = OMX_FALSE;
    EXYNOS_OMX_BASECOMPONENT      *pExynosComponent = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    EXYNOS_OMX_VIDEODEC_COMPONENT *pVideoDec        = (EXYNOS_OMX_VIDEODEC_COMPONENT *)pExynosComponent->hComponentHandle;
    EXYNOS_OMX_BASEPORT           *exynosOutputPort = &pExynosComponent->pExynosPort[OUTPUT_PORT_INDEX];
    EXYNOS_OMX_DATABUFFER         *outputUseBuffer  = &exynosOutputPort->way.port2WayDataBuffer.outputDataBuffer;
    OMX_U32                        copySize         = 0;
    DECODE_CODEC_EXTRA_BUFFERINFO *pBufferInfo      = NULL;
    OMX_COLOR_FORMATTYPE           eColorFormat     = exynosOutputPort->portDefinition.format.video.eColorFormat;

    FunctionIn();

    void *pOutputBuf                = (void *)outputUseBuffer->bufferHeader->pBuffer;
    void *pSrcBuf[MAX_BUFFER_PLANE] = {NULL, };
    void *pYUVBuf[MAX_BUFFER_PLANE] = {NULL, };

    ExynosVideoPlane planes[MAX_BUFFER_PLANE];
    OMX_U32          nPlaneSize[MAX_BUFFER_PLANE] = {0, 0, 0};

    OMX_U32 nFrameWidth = 0, nFrameHeight = 0;
    OMX_U32 nImageWidth = 0, nImageHeight = 0, stride = 0;

    CSC_MEMTYPE     csc_memType = CSC_MEMORY_USERPTR;
    CSC_METHOD      csc_method  = CSC_METHOD_SW;
    CSC_ERRORCODE   cscRet      = CSC_ErrorNone;
    unsigned int srcCacheable = 1, dstCacheable = 1;

    nFrameWidth  = exynosOutputPort->portDefinition.format.video.nStride;
    nFrameHeight = exynosOutputPort->portDefinition.format.video.nSliceHeight;

    pBufferInfo  = (DECODE_CODEC_EXTRA_BUFFERINFO *)dstOutputData->extInfo;
    nImageWidth  = pBufferInfo->imageWidth;
    nImageHeight = pBufferInfo->imageHeight;

    pSrcBuf[0] = dstOutputData->buffer.multiPlaneBuffer.dataBuffer[0];
    pSrcBuf[1] = dstOutputData->buffer.multiPlaneBuffer.dataBuffer[1];
    pSrcBuf[2] = dstOutputData->buffer.multiPlaneBuffer.dataBuffer[2];

    Exynos_OSAL_GetPlaneSize(eColorFormat, nImageWidth, nImageHeight, nPlaneSize);

    pYUVBuf[0]  = (unsigned char *)pOutputBuf;
    pYUVBuf[1]  = (unsigned char *)pOutputBuf + nPlaneSize[0];
    pYUVBuf[2]  = (unsigned char *)pOutputBuf + nPlaneSize[0] + nPlaneSize[1];

#ifdef USE_ANB
    if (exynosOutputPort->bIsANBEnabled == OMX_TRUE) {
        Exynos_OSAL_LockANBHandle(pOutputBuf, nImageWidth, nImageHeight, eColorFormat, &stride, planes);
        nImageWidth = stride;
        outputUseBuffer->dataLen = sizeof(void *);

        pYUVBuf[0]  = (unsigned char *)planes[0].addr;
        pYUVBuf[1]  = (unsigned char *)planes[1].addr;
        pYUVBuf[2]  = (unsigned char *)planes[2].addr;
    }
#endif

    if (pVideoDec->bDualDPBMode == OMX_TRUE) {
        switch (eColorFormat) {
        case OMX_COLOR_FormatYUV420SemiPlanar:
        case (OMX_COLOR_FORMATTYPE)OMX_SEC_COLOR_FormatNV21Linear:
        case (OMX_COLOR_FORMATTYPE)OMX_SEC_COLOR_FormatNV12Tiled:
        case OMX_COLOR_FormatYUV420Planar:
        case (OMX_COLOR_FORMATTYPE)OMX_SEC_COLOR_FormatYVU420Planar:
            csc_set_method(pVideoDec->csc_handle, CSC_METHOD_SW);
            csc_memType = CSC_MEMORY_MFC;
            if (pVideoDec->csc_set_format == OMX_FALSE) {
                csc_set_src_format(
                    pVideoDec->csc_handle,                              /* handle */
                    nFrameWidth,                                        /* width */
                    nFrameHeight,                                       /* height */
                    0,                                                  /* crop_left */
                    0,                                                  /* crop_right */
                    nImageWidth,                                        /* crop_width */
                    nImageHeight,                                       /* crop_height */
                    omx_2_hal_pixel_format(pBufferInfo->ColorFormat),   /* color_format */
                    0);                                                 /* cacheable */
                csc_set_dst_format(
                    pVideoDec->csc_handle,                              /* handle */
                    nImageWidth,                                        /* width */
                    nImageHeight,                                       /* height */
                    0,                                                  /* crop_left */
                    0,                                                  /* crop_right */
                    nImageWidth,                                        /* crop_width */
                    nImageHeight,                                       /* crop_height */
                    omx_2_hal_pixel_format(eColorFormat),               /* color_format */
                    0);                                                 /* cacheable */
                pVideoDec->csc_set_format = OMX_TRUE;
            }
            break;
        default:
            Exynos_OSAL_Log(EXYNOS_LOG_ERROR, "[%s] unspported color format : %x. can't support this format", __FUNCTION__, eColorFormat);
            ret = OMX_FALSE;
            goto EXIT;
            break;
        }
    } else {
        csc_get_method(pVideoDec->csc_handle, &csc_method);
        if (csc_method == CSC_METHOD_HW)
            srcCacheable = 0;

#ifdef USE_DMA_BUF
        if (csc_method == CSC_METHOD_HW) {
            csc_memType = CSC_MEMORY_DMABUF;

            pSrcBuf[0] = dstOutputData->buffer.multiPlaneBuffer.fd[0];
            pSrcBuf[1] = dstOutputData->buffer.multiPlaneBuffer.fd[1];
            pSrcBuf[2] = dstOutputData->buffer.multiPlaneBuffer.fd[2];

            if (exynosOutputPort->bIsANBEnabled == OMX_TRUE) {
                pYUVBuf[0]  = (unsigned char *)planes[0].fd;
                pYUVBuf[1]  = (unsigned char *)planes[1].fd;
                pYUVBuf[2]  = (unsigned char *)planes[2].fd;
            } else {
                pYUVBuf[0] = Exynos_OSAL_SharedMemory_VirtToION(pVideoDec->hSharedMemory, pOutputBuf);
                pYUVBuf[1] = NULL;
                pYUVBuf[2] = NULL;
            }
        }
#endif
        if (pVideoDec->csc_set_format == OMX_FALSE) {
            csc_set_src_format(
                pVideoDec->csc_handle,                              /* handle */
                nImageWidth,                                        /* width */
                nImageHeight,                                       /* height */
                0,                                                  /* crop_left */
                0,                                                  /* crop_right */
                nImageWidth,                                        /* crop_width */
                nImageHeight,                                       /* crop_height */
                omx_2_hal_pixel_format(pBufferInfo->ColorFormat),   /* color_format */
                srcCacheable);                                      /* cacheable */
            csc_set_dst_format(
                pVideoDec->csc_handle,                              /* handle */
                nImageWidth,                                        /* width */
                nImageHeight,                                       /* height */
                0,                                                  /* crop_left */
                0,                                                  /* crop_right */
                nImageWidth,                                        /* crop_width */
                nImageHeight,                                       /* crop_height */
                omx_2_hal_pixel_format(eColorFormat),               /* color_format */
                dstCacheable);                                      /* cacheable */
            pVideoDec->csc_set_format = OMX_TRUE;
        }
    }

    csc_set_src_buffer(
        pVideoDec->csc_handle,  /* handle */
        pSrcBuf,
        csc_memType);           /* YUV Addr or FD */
    csc_set_dst_buffer(
        pVideoDec->csc_handle,  /* handle */
        pYUVBuf,
        csc_memType);           /* YUV Addr or FD */
    cscRet = csc_convert(pVideoDec->csc_handle);
    if (cscRet != CSC_ErrorNone)
        ret = OMX_FALSE;
    else
        ret = OMX_TRUE;

#ifdef USE_ANB
    if (exynosOutputPort->bIsANBEnabled == OMX_TRUE) {
        Exynos_OSAL_UnlockANBHandle(pOutputBuf);
    }
#endif

EXIT:
    FunctionOut();

    return ret;
}

OMX_BOOL Exynos_Preprocessor_InputData(OMX_COMPONENTTYPE *pOMXComponent, EXYNOS_OMX_DATA *srcInputData)
{
    OMX_BOOL               ret = OMX_FALSE;
    EXYNOS_OMX_BASECOMPONENT *pExynosComponent = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    EXYNOS_OMX_VIDEODEC_COMPONENT *pVideoDec = (EXYNOS_OMX_VIDEODEC_COMPONENT *)pExynosComponent->hComponentHandle;
    EXYNOS_OMX_BASEPORT      *exynosInputPort = &pExynosComponent->pExynosPort[INPUT_PORT_INDEX];
    EXYNOS_OMX_DATABUFFER    *inputUseBuffer = &exynosInputPort->way.port2WayDataBuffer.inputDataBuffer;
    OMX_U32                copySize = 0;
    OMX_BYTE               checkInputStream = NULL;
    OMX_U32                checkInputStreamLen = 0;

    FunctionIn();

    if (exynosInputPort->bufferProcessType & BUFFER_COPY) {
        if ((srcInputData->buffer.singlePlaneBuffer.dataBuffer == NULL) ||
            (srcInputData->pPrivate == NULL)) {
            ret = OMX_FALSE;
            goto EXIT;
        }
    }

    if (inputUseBuffer->dataValid == OMX_TRUE) {
        if (exynosInputPort->bufferProcessType & BUFFER_SHARE) {
            Exynos_Shared_BufferToData(inputUseBuffer, srcInputData, ONE_PLANE);

            if (pVideoDec->bDRMPlayerMode == OMX_TRUE) {
                OMX_PTR dataBuffer = NULL;

                dataBuffer = Exynos_OSAL_SharedMemory_IONToVirt(pVideoDec->hSharedMemory,
                                                            srcInputData->buffer.singlePlaneBuffer.dataBuffer);
                if (dataBuffer == NULL) {
                    ret = OMX_FALSE;
                    goto EXIT;
                }

                srcInputData->buffer.singlePlaneBuffer.fd = srcInputData->buffer.singlePlaneBuffer.dataBuffer;
                srcInputData->buffer.singlePlaneBuffer.dataBuffer = dataBuffer;
            } else {
                srcInputData->buffer.singlePlaneBuffer.fd =
                    Exynos_OSAL_SharedMemory_VirtToION(pVideoDec->hSharedMemory,
                                srcInputData->buffer.singlePlaneBuffer.dataBuffer);
            }

            /* reset dataBuffer */
            Exynos_ResetDataBuffer(inputUseBuffer);
        } else if (exynosInputPort->bufferProcessType & BUFFER_COPY) {
            checkInputStream = inputUseBuffer->bufferHeader->pBuffer + inputUseBuffer->usedDataLen;
            checkInputStreamLen = inputUseBuffer->remainDataLen;

            pExynosComponent->bUseFlagEOF = OMX_TRUE;

            copySize = checkInputStreamLen;
            Exynos_OSAL_Log(EXYNOS_LOG_TRACE, "exynos_checkInputFrame : OMX_TRUE");

            if (((srcInputData->allocSize) - (srcInputData->dataLen)) >= copySize) {
                if (copySize > 0) {
                    Exynos_OSAL_Memcpy(srcInputData->buffer.singlePlaneBuffer.dataBuffer + srcInputData->dataLen,
                                       checkInputStream, copySize);
                }

                inputUseBuffer->dataLen -= copySize;
                inputUseBuffer->remainDataLen -= copySize;
                inputUseBuffer->usedDataLen += copySize;

                srcInputData->dataLen += copySize;
                srcInputData->remainDataLen += copySize;

                srcInputData->timeStamp = inputUseBuffer->timeStamp;
                srcInputData->nFlags = inputUseBuffer->nFlags;
                srcInputData->bufferHeader = inputUseBuffer->bufferHeader;
            } else {
                Exynos_OSAL_Log(EXYNOS_LOG_ERROR, "input codec buffer is smaller than decoded input data size Out Length");
                pExynosComponent->pCallbacks->EventHandler((OMX_HANDLETYPE)pOMXComponent,
                                                        pExynosComponent->callbackData,
                                                        OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                ret = OMX_FALSE;
            }

            Exynos_InputBufferReturn(pOMXComponent, inputUseBuffer);
        }

        if ((srcInputData->nFlags & OMX_BUFFERFLAG_EOS) == OMX_BUFFERFLAG_EOS) {
            Exynos_OSAL_Log(EXYNOS_LOG_TRACE, "bSaveFlagEOS : OMX_TRUE");
            pExynosComponent->bSaveFlagEOS = OMX_TRUE;
            if (srcInputData->dataLen != 0)
                pExynosComponent->bBehaviorEOS = OMX_TRUE;
        }

        if (pExynosComponent->checkTimeStamp.needSetStartTimeStamp == OMX_TRUE) {
            pExynosComponent->checkTimeStamp.needCheckStartTimeStamp = OMX_TRUE;
            pExynosComponent->checkTimeStamp.startTimeStamp = srcInputData->timeStamp;
            pExynosComponent->checkTimeStamp.nStartFlags = srcInputData->nFlags;
            pExynosComponent->checkTimeStamp.needSetStartTimeStamp = OMX_FALSE;
            Exynos_OSAL_Log(EXYNOS_LOG_TRACE, "first frame timestamp after seeking %lld us (%.2f secs)",
                            srcInputData->timeStamp, srcInputData->timeStamp / 1E6);
        }

        ret = OMX_TRUE;
    }

EXIT:

    FunctionOut();

    return ret;
}

OMX_BOOL Exynos_Postprocess_OutputData(OMX_COMPONENTTYPE *pOMXComponent, EXYNOS_OMX_DATA *dstOutputData)
{
    OMX_BOOL                   ret = OMX_FALSE;
    EXYNOS_OMX_BASECOMPONENT  *pExynosComponent = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    EXYNOS_OMX_VIDEODEC_COMPONENT *pVideoDec = (EXYNOS_OMX_VIDEODEC_COMPONENT *)pExynosComponent->hComponentHandle;
    EXYNOS_OMX_BASEPORT       *exynosOutputPort = &pExynosComponent->pExynosPort[OUTPUT_PORT_INDEX];
    EXYNOS_OMX_DATABUFFER     *outputUseBuffer = &exynosOutputPort->way.port2WayDataBuffer.outputDataBuffer;
    OMX_U32                    copySize = 0;
    DECODE_CODEC_EXTRA_BUFFERINFO *pBufferInfo = NULL;

    FunctionIn();

    if (exynosOutputPort->bufferProcessType & BUFFER_SHARE) {
        if (exynosOutputPort->bIsANBEnabled == OMX_FALSE) {
            if (Exynos_Shared_DataToBuffer(dstOutputData, outputUseBuffer) == OMX_ErrorNone)
                outputUseBuffer->dataValid = OMX_TRUE;
        } else {
            if (Exynos_Shared_DataToANBBuffer(dstOutputData, outputUseBuffer, exynosOutputPort) == OMX_ErrorNone) {
                outputUseBuffer->dataValid = OMX_TRUE;
            } else {
                ret = OMX_FALSE;
                goto EXIT;
            }
        }
    }

    if (outputUseBuffer->dataValid == OMX_TRUE) {
        Exynos_OSAL_Log(EXYNOS_LOG_TRACE, "out timestamp after seeking %lld us (%.2f secs)",
            dstOutputData->timeStamp, dstOutputData->timeStamp / 1E6);
        if ((pExynosComponent->checkTimeStamp.needCheckStartTimeStamp == OMX_TRUE) &&
            ((dstOutputData->nFlags & OMX_BUFFERFLAG_EOS) != OMX_BUFFERFLAG_EOS)) {
            if ((pExynosComponent->checkTimeStamp.startTimeStamp == dstOutputData->timeStamp) &&
                (pExynosComponent->checkTimeStamp.nStartFlags == dstOutputData->nFlags)){
                pExynosComponent->checkTimeStamp.startTimeStamp = -19761123;
                pExynosComponent->checkTimeStamp.nStartFlags = 0x0;
                pExynosComponent->checkTimeStamp.needSetStartTimeStamp = OMX_FALSE;
                pExynosComponent->checkTimeStamp.needCheckStartTimeStamp = OMX_FALSE;
            } else {
                if (pExynosComponent->checkTimeStamp.startTimeStamp < dstOutputData->timeStamp) {
                    pExynosComponent->checkTimeStamp.startTimeStamp = -19761123;
                    pExynosComponent->checkTimeStamp.nStartFlags = 0x0;
                    pExynosComponent->checkTimeStamp.needSetStartTimeStamp = OMX_FALSE;
                    pExynosComponent->checkTimeStamp.needCheckStartTimeStamp = OMX_FALSE;
                } else {
                    Exynos_OSAL_Log(EXYNOS_LOG_TRACE, "drop frame after seeking", pExynosComponent);
                    if (exynosOutputPort->bufferProcessType & BUFFER_SHARE)
                        Exynos_OMX_FillThisBuffer(pOMXComponent, outputUseBuffer->bufferHeader);

                    ret = OMX_TRUE;
                    goto EXIT;
                }
            }
        } else if ((pExynosComponent->checkTimeStamp.needSetStartTimeStamp == OMX_TRUE)) {
            ret = OMX_TRUE;
            Exynos_OSAL_Log(EXYNOS_LOG_TRACE, "not set check timestame after seeking");
            goto EXIT;
        }

        if (exynosOutputPort->bufferProcessType & BUFFER_COPY) {
            if ((dstOutputData->remainDataLen <= (outputUseBuffer->allocSize - outputUseBuffer->dataLen)) &&
                (!CHECK_PORT_BEING_FLUSHED(exynosOutputPort))) {
                copySize = dstOutputData->remainDataLen;
                Exynos_OSAL_Log(EXYNOS_LOG_TRACE,"copySize: %d", copySize);

                outputUseBuffer->dataLen += copySize;
                outputUseBuffer->remainDataLen += copySize;
                outputUseBuffer->nFlags = dstOutputData->nFlags;
                outputUseBuffer->timeStamp = dstOutputData->timeStamp;

                if (outputUseBuffer->remainDataLen > 0) {
                    ret = Exynos_CSC_OutputData(pOMXComponent, dstOutputData);
                } else {
                    ret = OMX_TRUE;
                }

                if (ret == OMX_TRUE) {
                    if ((outputUseBuffer->remainDataLen > 0) ||
                        ((outputUseBuffer->nFlags & OMX_BUFFERFLAG_EOS) == OMX_BUFFERFLAG_EOS) ||
                        (CHECK_PORT_BEING_FLUSHED(exynosOutputPort))) {
                        Exynos_OutputBufferReturn(pOMXComponent, outputUseBuffer);
                    }
                } else {
                    Exynos_OSAL_Log(EXYNOS_LOG_ERROR, "csc_convert Error");
                    pExynosComponent->pCallbacks->EventHandler((OMX_HANDLETYPE)pOMXComponent,
                                                            pExynosComponent->callbackData,
                                                            OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                    ret = OMX_FALSE;
                }
            } else if (CHECK_PORT_BEING_FLUSHED(exynosOutputPort)) {
                outputUseBuffer->dataLen = 0;
                outputUseBuffer->remainDataLen = 0;
                outputUseBuffer->nFlags = dstOutputData->nFlags;
                outputUseBuffer->timeStamp = dstOutputData->timeStamp;
                Exynos_OutputBufferReturn(pOMXComponent, outputUseBuffer);
            } else {
                Exynos_OSAL_Log(EXYNOS_LOG_ERROR, "output buffer is smaller than decoded data size Out Length");
                pExynosComponent->pCallbacks->EventHandler((OMX_HANDLETYPE)pOMXComponent,
                                                        pExynosComponent->callbackData,
                                                        OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                ret = OMX_FALSE;
            }
        } else if (exynosOutputPort->bufferProcessType & BUFFER_SHARE) {
            if ((outputUseBuffer->remainDataLen > 0) ||
                ((outputUseBuffer->nFlags & OMX_BUFFERFLAG_EOS) == OMX_BUFFERFLAG_EOS) ||
                (CHECK_PORT_BEING_FLUSHED(exynosOutputPort)))
                Exynos_OutputBufferReturn(pOMXComponent, outputUseBuffer);
            else
                Exynos_OMX_FillThisBuffer(pOMXComponent, outputUseBuffer->bufferHeader);
        }
    } else {
        ret = OMX_FALSE;
    }

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE Exynos_OMX_SrcInputBufferProcess(OMX_HANDLETYPE hComponent)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = (OMX_COMPONENTTYPE *)hComponent;
    EXYNOS_OMX_BASECOMPONENT *pExynosComponent = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    EXYNOS_OMX_VIDEODEC_COMPONENT *pVideoDec = (EXYNOS_OMX_VIDEODEC_COMPONENT *)pExynosComponent->hComponentHandle;
    EXYNOS_OMX_BASEPORT      *exynosInputPort = &pExynosComponent->pExynosPort[INPUT_PORT_INDEX];
    EXYNOS_OMX_BASEPORT      *exynosOutputPort = &pExynosComponent->pExynosPort[OUTPUT_PORT_INDEX];
    EXYNOS_OMX_DATABUFFER    *srcInputUseBuffer = &exynosInputPort->way.port2WayDataBuffer.inputDataBuffer;
    EXYNOS_OMX_DATA          *pSrcInputData = &exynosInputPort->processData;
    OMX_BOOL               bCheckInputData = OMX_FALSE;
    OMX_BOOL               bValidCodecData = OMX_FALSE;

    FunctionIn();

    while (!pVideoDec->bExitBufferProcessThread) {
        Exynos_OSAL_SleepMillisec(0);
        Exynos_Wait_ProcessPause(pExynosComponent, INPUT_PORT_INDEX);

        while ((Exynos_Check_BufferProcess_State(pExynosComponent, INPUT_PORT_INDEX)) &&
               (!pVideoDec->bExitBufferProcessThread)) {
            Exynos_OSAL_SleepMillisec(0);

            if ((CHECK_PORT_BEING_FLUSHED(exynosInputPort)) ||
                ((exynosOutputPort->exceptionFlag == NEED_PORT_DISABLE) && (ret == OMX_ErrorInputDataDecodeYet)) ||
                (exynosOutputPort->exceptionFlag == INVALID_STATE))
                break;
            if (exynosInputPort->portState != OMX_StateIdle)
                break;

            Exynos_OSAL_MutexLock(srcInputUseBuffer->bufferMutex);
            if (ret != OMX_ErrorInputDataDecodeYet) {
                if (exynosInputPort->bufferProcessType & BUFFER_COPY) {
                    OMX_PTR codecBuffer;
                    if ((pSrcInputData->buffer.singlePlaneBuffer.dataBuffer == NULL) || (pSrcInputData->pPrivate == NULL)) {
                        Exynos_CodecBufferDeQueue(pExynosComponent, INPUT_PORT_INDEX, &codecBuffer);
                        if (codecBuffer != NULL) {
                            Exynos_Input_CodecBufferToData(pExynosComponent, codecBuffer, pSrcInputData);
                        }
                        Exynos_OSAL_MutexUnlock(srcInputUseBuffer->bufferMutex);
                        break;
                    }
                }

                if (srcInputUseBuffer->dataValid == OMX_TRUE) {
                    bCheckInputData = Exynos_Preprocessor_InputData(pOMXComponent, pSrcInputData);
                } else {
                    bCheckInputData = OMX_FALSE;
                }

                if ((bCheckInputData == OMX_FALSE) &&
                    (!CHECK_PORT_BEING_FLUSHED(exynosInputPort))) {
                    ret = Exynos_InputBufferGetQueue(pExynosComponent);
                    Exynos_OSAL_MutexUnlock(srcInputUseBuffer->bufferMutex);
                    break;
                }

                if (CHECK_PORT_BEING_FLUSHED(exynosInputPort)) {
                    Exynos_OSAL_MutexUnlock(srcInputUseBuffer->bufferMutex);
                    break;
                }
            }

            ret = pVideoDec->exynos_codec_srcInputProcess(pOMXComponent, pSrcInputData);
            if (ret == OMX_ErrorCorruptedFrame) {
                if (exynosInputPort->bufferProcessType & BUFFER_COPY) {
                    OMX_PTR codecBuffer;
                    codecBuffer = pSrcInputData->pPrivate;
                    if (codecBuffer != NULL)
                        Exynos_CodecBufferEnQueue(pExynosComponent, INPUT_PORT_INDEX, codecBuffer);
                }

                if (exynosInputPort->bufferProcessType & BUFFER_SHARE) {
                    Exynos_OMX_InputBufferReturn(pOMXComponent, pSrcInputData->bufferHeader);
                }
            }

            if (ret != OMX_ErrorInputDataDecodeYet) {
                Exynos_ResetCodecData(pSrcInputData);
            }
            Exynos_OSAL_MutexUnlock(srcInputUseBuffer->bufferMutex);
            if (ret == OMX_ErrorCodecInit)
                pVideoDec->bExitBufferProcessThread = OMX_TRUE;
        }
    }

EXIT:

    FunctionOut();

    return ret;
}

OMX_ERRORTYPE Exynos_OMX_SrcOutputBufferProcess(OMX_HANDLETYPE hComponent)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = (OMX_COMPONENTTYPE *)hComponent;
    EXYNOS_OMX_BASECOMPONENT *pExynosComponent = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    EXYNOS_OMX_VIDEODEC_COMPONENT *pVideoDec = (EXYNOS_OMX_VIDEODEC_COMPONENT *)pExynosComponent->hComponentHandle;
    EXYNOS_OMX_BASEPORT      *exynosInputPort = &pExynosComponent->pExynosPort[INPUT_PORT_INDEX];
    EXYNOS_OMX_DATABUFFER    *srcOutputUseBuffer = &exynosInputPort->way.port2WayDataBuffer.outputDataBuffer;
    EXYNOS_OMX_DATA           srcOutputData;

    FunctionIn();

    while (!pVideoDec->bExitBufferProcessThread) {
        Exynos_OSAL_SleepMillisec(0);

        while (!pVideoDec->bExitBufferProcessThread) {
            if (exynosInputPort->bufferProcessType & BUFFER_COPY) {
                if (Exynos_Check_BufferProcess_State(pExynosComponent, INPUT_PORT_INDEX) == OMX_FALSE)
                    break;
            }
            Exynos_OSAL_SleepMillisec(0);

            if (CHECK_PORT_BEING_FLUSHED(exynosInputPort))
                break;

            Exynos_OSAL_MutexLock(srcOutputUseBuffer->bufferMutex);
            ret = pVideoDec->exynos_codec_srcOutputProcess(pOMXComponent, &srcOutputData);

            if (ret == OMX_ErrorNone) {
                if (exynosInputPort->bufferProcessType & BUFFER_COPY) {
                    OMX_PTR codecBuffer;
                    codecBuffer = srcOutputData.pPrivate;
                    if (codecBuffer != NULL)
                        Exynos_CodecBufferEnQueue(pExynosComponent, INPUT_PORT_INDEX, codecBuffer);
                }
                if (exynosInputPort->bufferProcessType & BUFFER_SHARE) {
                    Exynos_Shared_DataToBuffer(&srcOutputData, srcOutputUseBuffer);
                    Exynos_InputBufferReturn(pOMXComponent, srcOutputUseBuffer);
                }
                Exynos_ResetCodecData(&srcOutputData);
            }
            Exynos_OSAL_MutexUnlock(srcOutputUseBuffer->bufferMutex);
        }
    }

EXIT:

    FunctionOut();

    return ret;
}

OMX_ERRORTYPE Exynos_OMX_DstInputBufferProcess(OMX_HANDLETYPE hComponent)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = (OMX_COMPONENTTYPE *)hComponent;
    EXYNOS_OMX_BASECOMPONENT *pExynosComponent = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    EXYNOS_OMX_VIDEODEC_COMPONENT *pVideoDec = (EXYNOS_OMX_VIDEODEC_COMPONENT *)pExynosComponent->hComponentHandle;
    EXYNOS_OMX_BASEPORT      *exynosOutputPort = &pExynosComponent->pExynosPort[OUTPUT_PORT_INDEX];
    EXYNOS_OMX_DATABUFFER    *dstInputUseBuffer = &exynosOutputPort->way.port2WayDataBuffer.inputDataBuffer;
    EXYNOS_OMX_DATA           dstInputData;

    FunctionIn();

    while (!pVideoDec->bExitBufferProcessThread) {
        Exynos_OSAL_SleepMillisec(0);

        while ((Exynos_Check_BufferProcess_State(pExynosComponent, OUTPUT_PORT_INDEX)) &&
               (!pVideoDec->bExitBufferProcessThread)) {
            Exynos_OSAL_SleepMillisec(0);

            if ((CHECK_PORT_BEING_FLUSHED(exynosOutputPort)) ||
                (!CHECK_PORT_POPULATED(exynosOutputPort)))
                break;
            if (exynosOutputPort->portState != OMX_StateIdle)
                break;

            Exynos_OSAL_MutexLock(dstInputUseBuffer->bufferMutex);
            if (ret != OMX_ErrorOutputBufferUseYet) {
                if (exynosOutputPort->bufferProcessType & BUFFER_COPY) {
                    CODEC_DEC_BUFFER *pCodecBuffer = NULL;
                    ret = Exynos_CodecBufferDeQueue(pExynosComponent, OUTPUT_PORT_INDEX, (OMX_PTR *)&pCodecBuffer);
                    if (ret != OMX_ErrorNone) {
                        Exynos_OSAL_MutexUnlock(dstInputUseBuffer->bufferMutex);
                        break;
                    }
                    Exynos_Output_CodecBufferToData(pCodecBuffer, &dstInputData);
                }

                if (exynosOutputPort->bufferProcessType & BUFFER_SHARE) {
                    if ((dstInputUseBuffer->dataValid != OMX_TRUE) &&
                        (!CHECK_PORT_BEING_FLUSHED(exynosOutputPort))) {
                        ret = Exynos_OutputBufferGetQueue(pExynosComponent);
                        if (ret != OMX_ErrorNone) {
                            Exynos_OSAL_MutexUnlock(dstInputUseBuffer->bufferMutex);
                            break;
                        }
                        if (exynosOutputPort->bIsANBEnabled == OMX_FALSE) {
                            Exynos_Shared_BufferToData(dstInputUseBuffer, &dstInputData, TWO_PLANE);
                        } else {
                            ret = Exynos_Shared_ANBBufferToData(dstInputUseBuffer, &dstInputData, exynosOutputPort, TWO_PLANE);
                            if (ret != OMX_ErrorNone) {
                                dstInputUseBuffer->dataValid = OMX_FALSE;
                                Exynos_OSAL_MutexUnlock(dstInputUseBuffer->bufferMutex);
                                break;
                            }
                        }

                        OMX_PTR pBufferHandle = dstInputData.bufferHeader->pBuffer;
                        if (exynosOutputPort->bStoreMetaData == OMX_TRUE) {
                            Exynos_OSAL_GetInfoFromMetaData(dstInputData.bufferHeader->pBuffer, &pBufferHandle);
                        }
                        Exynos_ResetDataBuffer(dstInputUseBuffer);
                    }
                }

                if (CHECK_PORT_BEING_FLUSHED(exynosOutputPort)) {
                    Exynos_OSAL_MutexUnlock(dstInputUseBuffer->bufferMutex);
                    break;
                }
            }

            ret = pVideoDec->exynos_codec_dstInputProcess(pOMXComponent, &dstInputData);
            if (ret != OMX_ErrorOutputBufferUseYet) {
                Exynos_ResetCodecData(&dstInputData);
            }
            Exynos_OSAL_MutexUnlock(dstInputUseBuffer->bufferMutex);
        }
    }

EXIT:

    FunctionOut();

    return ret;
}

OMX_ERRORTYPE Exynos_OMX_DstOutputBufferProcess(OMX_HANDLETYPE hComponent)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = (OMX_COMPONENTTYPE *)hComponent;
    EXYNOS_OMX_BASECOMPONENT *pExynosComponent = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    EXYNOS_OMX_VIDEODEC_COMPONENT *pVideoDec = (EXYNOS_OMX_VIDEODEC_COMPONENT *)pExynosComponent->hComponentHandle;
    EXYNOS_OMX_BASEPORT      *exynosOutputPort = &pExynosComponent->pExynosPort[OUTPUT_PORT_INDEX];
    EXYNOS_OMX_DATABUFFER    *dstOutputUseBuffer = &exynosOutputPort->way.port2WayDataBuffer.outputDataBuffer;
    EXYNOS_OMX_DATA          *pDstOutputData = &exynosOutputPort->processData;

    FunctionIn();

    while (!pVideoDec->bExitBufferProcessThread) {
        Exynos_OSAL_SleepMillisec(0);
        Exynos_Wait_ProcessPause(pExynosComponent, OUTPUT_PORT_INDEX);

        while ((Exynos_Check_BufferProcess_State(pExynosComponent, OUTPUT_PORT_INDEX)) &&
               (!pVideoDec->bExitBufferProcessThread)) {
            Exynos_OSAL_SleepMillisec(0);

            if (CHECK_PORT_BEING_FLUSHED(exynosOutputPort))
                break;

            Exynos_OSAL_MutexLock(dstOutputUseBuffer->bufferMutex);
            if (exynosOutputPort->bufferProcessType & BUFFER_COPY) {
                if ((dstOutputUseBuffer->dataValid != OMX_TRUE) &&
                    (!CHECK_PORT_BEING_FLUSHED(exynosOutputPort))) {
                    ret = Exynos_OutputBufferGetQueue(pExynosComponent);
                    if (ret != OMX_ErrorNone) {
                        Exynos_OSAL_MutexUnlock(dstOutputUseBuffer->bufferMutex);
                        break;
                    }
                }
            }

            if ((dstOutputUseBuffer->dataValid == OMX_TRUE) ||
                (exynosOutputPort->bufferProcessType & BUFFER_SHARE))
                ret = pVideoDec->exynos_codec_dstOutputProcess(pOMXComponent, pDstOutputData);

            if (((ret == OMX_ErrorNone) && (dstOutputUseBuffer->dataValid == OMX_TRUE)) ||
                (exynosOutputPort->bufferProcessType & BUFFER_SHARE)) {
                Exynos_Postprocess_OutputData(pOMXComponent, pDstOutputData);
            }

            if (exynosOutputPort->bufferProcessType & BUFFER_COPY) {
                if (pDstOutputData->pPrivate != NULL) {
                    Exynos_CodecBufferEnQueue(pExynosComponent, OUTPUT_PORT_INDEX, pDstOutputData->pPrivate);
                    pDstOutputData->pPrivate = NULL;
                }
            }

            /* reset outputData */
            Exynos_ResetCodecData(pDstOutputData);
            Exynos_OSAL_MutexUnlock(dstOutputUseBuffer->bufferMutex);
        }
    }

EXIT:

    FunctionOut();

    return ret;
}

static OMX_ERRORTYPE Exynos_OMX_SrcInputProcessThread(OMX_PTR threadData)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = NULL;
    EXYNOS_OMX_BASECOMPONENT *pExynosComponent = NULL;
    EXYNOS_OMX_MESSAGE       *message = NULL;

    FunctionIn();

    if (threadData == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }
    pOMXComponent = (OMX_COMPONENTTYPE *)threadData;
    ret = Exynos_OMX_Check_SizeVersion(pOMXComponent, sizeof(OMX_COMPONENTTYPE));
    if (ret != OMX_ErrorNone) {
        goto EXIT;
    }
    pExynosComponent = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    Exynos_OMX_SrcInputBufferProcess(pOMXComponent);

    Exynos_OSAL_ThreadExit(NULL);

EXIT:
    FunctionOut();

    return ret;
}

static OMX_ERRORTYPE Exynos_OMX_SrcOutputProcessThread(OMX_PTR threadData)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = NULL;
    EXYNOS_OMX_BASECOMPONENT *pExynosComponent = NULL;
    EXYNOS_OMX_MESSAGE       *message = NULL;

    FunctionIn();

    if (threadData == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }
    pOMXComponent = (OMX_COMPONENTTYPE *)threadData;
    ret = Exynos_OMX_Check_SizeVersion(pOMXComponent, sizeof(OMX_COMPONENTTYPE));
    if (ret != OMX_ErrorNone) {
        goto EXIT;
    }
    pExynosComponent = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    Exynos_OMX_SrcOutputBufferProcess(pOMXComponent);

    Exynos_OSAL_ThreadExit(NULL);

EXIT:
    FunctionOut();

    return ret;
}

static OMX_ERRORTYPE Exynos_OMX_DstInputProcessThread(OMX_PTR threadData)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = NULL;
    EXYNOS_OMX_BASECOMPONENT *pExynosComponent = NULL;
    EXYNOS_OMX_MESSAGE       *message = NULL;

    FunctionIn();

    if (threadData == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }
    pOMXComponent = (OMX_COMPONENTTYPE *)threadData;
    ret = Exynos_OMX_Check_SizeVersion(pOMXComponent, sizeof(OMX_COMPONENTTYPE));
    if (ret != OMX_ErrorNone) {
        goto EXIT;
    }
    pExynosComponent = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    Exynos_OMX_DstInputBufferProcess(pOMXComponent);

    Exynos_OSAL_ThreadExit(NULL);

EXIT:
    FunctionOut();

    return ret;
}

static OMX_ERRORTYPE Exynos_OMX_DstOutputProcessThread(OMX_PTR threadData)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = NULL;
    EXYNOS_OMX_BASECOMPONENT *pExynosComponent = NULL;
    EXYNOS_OMX_MESSAGE       *message = NULL;

    FunctionIn();

    if (threadData == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }
    pOMXComponent = (OMX_COMPONENTTYPE *)threadData;
    ret = Exynos_OMX_Check_SizeVersion(pOMXComponent, sizeof(OMX_COMPONENTTYPE));
    if (ret != OMX_ErrorNone) {
        goto EXIT;
    }
    pExynosComponent = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    Exynos_OMX_DstOutputBufferProcess(pOMXComponent);

    Exynos_OSAL_ThreadExit(NULL);

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE Exynos_OMX_BufferProcess_Create(OMX_HANDLETYPE hComponent)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = (OMX_COMPONENTTYPE *)hComponent;
    EXYNOS_OMX_BASECOMPONENT *pExynosComponent = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    EXYNOS_OMX_VIDEODEC_COMPONENT *pVideoDec = (EXYNOS_OMX_VIDEODEC_COMPONENT *)pExynosComponent->hComponentHandle;

    FunctionIn();

    pVideoDec->bExitBufferProcessThread = OMX_FALSE;

    ret = Exynos_OSAL_ThreadCreate(&pVideoDec->hDstOutputThread,
                 Exynos_OMX_DstOutputProcessThread,
                 pOMXComponent);
    if (ret == OMX_ErrorNone)
        ret = Exynos_OSAL_ThreadCreate(&pVideoDec->hSrcOutputThread,
                     Exynos_OMX_SrcOutputProcessThread,
                     pOMXComponent);
    if (ret == OMX_ErrorNone)
        ret = Exynos_OSAL_ThreadCreate(&pVideoDec->hDstInputThread,
                     Exynos_OMX_DstInputProcessThread,
                     pOMXComponent);
    if (ret == OMX_ErrorNone)
        ret = Exynos_OSAL_ThreadCreate(&pVideoDec->hSrcInputThread,
                     Exynos_OMX_SrcInputProcessThread,
                     pOMXComponent);

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE Exynos_OMX_BufferProcess_Terminate(OMX_HANDLETYPE hComponent)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = (OMX_COMPONENTTYPE *)hComponent;
    EXYNOS_OMX_BASECOMPONENT *pExynosComponent = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    EXYNOS_OMX_VIDEODEC_COMPONENT *pVideoDec = (EXYNOS_OMX_VIDEODEC_COMPONENT *)pExynosComponent->hComponentHandle;
    OMX_S32                countValue = 0;
    unsigned int           i = 0;

    FunctionIn();

    pVideoDec->bExitBufferProcessThread = OMX_TRUE;

    Exynos_OSAL_Get_SemaphoreCount(pExynosComponent->pExynosPort[INPUT_PORT_INDEX].bufferSemID, &countValue);
    if (countValue == 0)
        Exynos_OSAL_SemaphorePost(pExynosComponent->pExynosPort[INPUT_PORT_INDEX].bufferSemID);
    Exynos_OSAL_Get_SemaphoreCount(pExynosComponent->pExynosPort[INPUT_PORT_INDEX].codecSemID, &countValue);
    if (countValue == 0)
        Exynos_OSAL_SemaphorePost(pExynosComponent->pExynosPort[INPUT_PORT_INDEX].codecSemID);
    Exynos_OSAL_SignalSet(pExynosComponent->pExynosPort[INPUT_PORT_INDEX].pauseEvent);
    Exynos_OSAL_ThreadTerminate(pVideoDec->hSrcInputThread);
    pVideoDec->hSrcInputThread = NULL;

    Exynos_OSAL_Get_SemaphoreCount(pExynosComponent->pExynosPort[OUTPUT_PORT_INDEX].bufferSemID, &countValue);
    if (countValue == 0)
        Exynos_OSAL_SemaphorePost(pExynosComponent->pExynosPort[OUTPUT_PORT_INDEX].bufferSemID);
    Exynos_OSAL_Get_SemaphoreCount(pExynosComponent->pExynosPort[OUTPUT_PORT_INDEX].codecSemID, &countValue);
    if (countValue == 0)
        Exynos_OSAL_SemaphorePost(pExynosComponent->pExynosPort[OUTPUT_PORT_INDEX].codecSemID);
    Exynos_OSAL_SignalSet(pExynosComponent->pExynosPort[OUTPUT_PORT_INDEX].pauseEvent);
    Exynos_OSAL_ThreadTerminate(pVideoDec->hDstInputThread);
    pVideoDec->hDstInputThread = NULL;

    pVideoDec->exynos_codec_stop(pOMXComponent, INPUT_PORT_INDEX);
    pVideoDec->exynos_codec_bufferProcessRun(pOMXComponent, INPUT_PORT_INDEX);
    Exynos_OSAL_SignalSet(pExynosComponent->pExynosPort[INPUT_PORT_INDEX].pauseEvent);
    Exynos_OSAL_ThreadTerminate(pVideoDec->hSrcOutputThread);
    pVideoDec->hSrcOutputThread = NULL;

    pVideoDec->exynos_codec_stop(pOMXComponent, OUTPUT_PORT_INDEX);
    pVideoDec->exynos_codec_bufferProcessRun(pOMXComponent, INPUT_PORT_INDEX);
    Exynos_OSAL_SignalSet(pExynosComponent->pExynosPort[OUTPUT_PORT_INDEX].pauseEvent);
    Exynos_OSAL_ThreadTerminate(pVideoDec->hDstOutputThread);
    pVideoDec->hDstOutputThread = NULL;

    pExynosComponent->checkTimeStamp.needSetStartTimeStamp      = OMX_FALSE;
    pExynosComponent->checkTimeStamp.needCheckStartTimeStamp    = OMX_FALSE;

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE Exynos_OMX_VideoDecodeComponentInit(OMX_IN OMX_HANDLETYPE hComponent)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = NULL;
    EXYNOS_OMX_BASECOMPONENT *pExynosComponent = NULL;
    EXYNOS_OMX_BASEPORT      *pExynosPort = NULL;
    EXYNOS_OMX_VIDEODEC_COMPONENT *pVideoDec = NULL;

    FunctionIn();

    if (hComponent == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }
    pOMXComponent = (OMX_COMPONENTTYPE *)hComponent;
    ret = Exynos_OMX_Check_SizeVersion(pOMXComponent, sizeof(OMX_COMPONENTTYPE));
    if (ret != OMX_ErrorNone) {
        Exynos_OSAL_Log(EXYNOS_LOG_ERROR, "OMX_Error, Line:%d", __LINE__);
        goto EXIT;
    }

    ret = Exynos_OMX_BaseComponent_Constructor(pOMXComponent);
    if (ret != OMX_ErrorNone) {
        Exynos_OSAL_Log(EXYNOS_LOG_ERROR, "OMX_Error, Line:%d", __LINE__);
        goto EXIT;
    }

    ret = Exynos_OMX_Port_Constructor(pOMXComponent);
    if (ret != OMX_ErrorNone) {
        Exynos_OMX_BaseComponent_Destructor(pOMXComponent);
        Exynos_OSAL_Log(EXYNOS_LOG_ERROR, "OMX_Error, Line:%d", __LINE__);
        goto EXIT;
    }

    pExynosComponent = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;

    pVideoDec = Exynos_OSAL_Malloc(sizeof(EXYNOS_OMX_VIDEODEC_COMPONENT));
    if (pVideoDec == NULL) {
        Exynos_OMX_BaseComponent_Destructor(pOMXComponent);
        ret = OMX_ErrorInsufficientResources;
        Exynos_OSAL_Log(EXYNOS_LOG_ERROR, "OMX_ErrorInsufficientResources, Line:%d", __LINE__);
        goto EXIT;
    }

    Exynos_OSAL_Memset(pVideoDec, 0, sizeof(EXYNOS_OMX_VIDEODEC_COMPONENT));
    pVideoDec->bReconfigDPB = OMX_FALSE;
    pVideoDec->bDTSMode     = OMX_FALSE;
    pVideoDec->bQosChanged  = OMX_FALSE;
    pVideoDec->nQosRatio    = 0;
    pVideoDec->bDualDPBMode = OMX_FALSE;
    pExynosComponent->hComponentHandle = (OMX_HANDLETYPE)pVideoDec;

    pExynosComponent->bSaveFlagEOS = OMX_FALSE;
    pExynosComponent->bBehaviorEOS = OMX_FALSE;
    pExynosComponent->bMultiThreadProcess = OMX_TRUE;

    /* Input port */
    pExynosPort = &pExynosComponent->pExynosPort[INPUT_PORT_INDEX];
    pExynosPort->portDefinition.nBufferCountActual = MAX_VIDEO_INPUTBUFFER_NUM;
    pExynosPort->portDefinition.nBufferCountMin = MAX_VIDEO_INPUTBUFFER_NUM;
    pExynosPort->portDefinition.nBufferSize = 0;
    pExynosPort->portDefinition.eDomain = OMX_PortDomainVideo;

    pExynosPort->portDefinition.format.video.cMIMEType = Exynos_OSAL_Malloc(MAX_OMX_MIMETYPE_SIZE);
    Exynos_OSAL_Strcpy(pExynosPort->portDefinition.format.video.cMIMEType, "raw/video");
    pExynosPort->portDefinition.format.video.pNativeRender = 0;
    pExynosPort->portDefinition.format.video.bFlagErrorConcealment = OMX_FALSE;
    pExynosPort->portDefinition.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;

    pExynosPort->portDefinition.format.video.nFrameWidth = 0;
    pExynosPort->portDefinition.format.video.nFrameHeight= 0;
    pExynosPort->portDefinition.format.video.nStride = 0;
    pExynosPort->portDefinition.format.video.nSliceHeight = 0;
    pExynosPort->portDefinition.format.video.nBitrate = 64000;
    pExynosPort->portDefinition.format.video.xFramerate = (15 << 16);
    pExynosPort->portDefinition.format.video.eColorFormat = OMX_COLOR_FormatUnused;
    pExynosPort->portDefinition.format.video.pNativeWindow = NULL;

    /* Output port */
    pExynosPort = &pExynosComponent->pExynosPort[OUTPUT_PORT_INDEX];
    pExynosPort->portDefinition.nBufferCountActual = MAX_VIDEO_OUTPUTBUFFER_NUM;
    pExynosPort->portDefinition.nBufferCountMin = MAX_VIDEO_OUTPUTBUFFER_NUM;
    pExynosPort->portDefinition.nBufferSize = DEFAULT_VIDEO_OUTPUT_BUFFER_SIZE;
    pExynosPort->portDefinition.eDomain = OMX_PortDomainVideo;

    pExynosPort->portDefinition.format.video.cMIMEType = Exynos_OSAL_Malloc(MAX_OMX_MIMETYPE_SIZE);
    Exynos_OSAL_Strcpy(pExynosPort->portDefinition.format.video.cMIMEType, "raw/video");
    pExynosPort->portDefinition.format.video.pNativeRender = 0;
    pExynosPort->portDefinition.format.video.bFlagErrorConcealment = OMX_FALSE;
    pExynosPort->portDefinition.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;

    pExynosPort->portDefinition.format.video.nFrameWidth = 0;
    pExynosPort->portDefinition.format.video.nFrameHeight= 0;
    pExynosPort->portDefinition.format.video.nStride = 0;
    pExynosPort->portDefinition.format.video.nSliceHeight = 0;
    pExynosPort->portDefinition.format.video.nBitrate = 64000;
    pExynosPort->portDefinition.format.video.xFramerate = (15 << 16);
    pExynosPort->portDefinition.format.video.eColorFormat = OMX_COLOR_FormatUnused;
    pExynosPort->portDefinition.format.video.pNativeWindow = NULL;

    pExynosPort->processData.extInfo = (OMX_PTR)Exynos_OSAL_Malloc(sizeof(DECODE_CODEC_EXTRA_BUFFERINFO));

    pOMXComponent->UseBuffer              = &Exynos_OMX_UseBuffer;
    pOMXComponent->AllocateBuffer         = &Exynos_OMX_AllocateBuffer;
    pOMXComponent->FreeBuffer             = &Exynos_OMX_FreeBuffer;
    pOMXComponent->ComponentTunnelRequest = &Exynos_OMX_ComponentTunnelRequest;

    pExynosComponent->exynos_AllocateTunnelBuffer = &Exynos_OMX_AllocateTunnelBuffer;
    pExynosComponent->exynos_FreeTunnelBuffer     = &Exynos_OMX_FreeTunnelBuffer;
    pExynosComponent->exynos_BufferProcessCreate    = &Exynos_OMX_BufferProcess_Create;
    pExynosComponent->exynos_BufferProcessTerminate = &Exynos_OMX_BufferProcess_Terminate;
    pExynosComponent->exynos_BufferFlush          = &Exynos_OMX_BufferFlush;

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE Exynos_OMX_VideoDecodeComponentDeinit(OMX_IN OMX_HANDLETYPE hComponent)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = NULL;
    EXYNOS_OMX_BASECOMPONENT *pExynosComponent = NULL;
    EXYNOS_OMX_BASEPORT      *pExynosPort = NULL;
    EXYNOS_OMX_VIDEODEC_COMPONENT *pVideoDec = NULL;
    int                    i = 0;

    FunctionIn();

    if (hComponent == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }
    pOMXComponent = (OMX_COMPONENTTYPE *)hComponent;
    ret = Exynos_OMX_Check_SizeVersion(pOMXComponent, sizeof(OMX_COMPONENTTYPE));
    if (ret != OMX_ErrorNone) {
        goto EXIT;
    }

    if (pOMXComponent->pComponentPrivate == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }
    pExynosComponent = (EXYNOS_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;

    pVideoDec = (EXYNOS_OMX_VIDEODEC_COMPONENT *)pExynosComponent->hComponentHandle;

    Exynos_OSAL_Free(pVideoDec);
    pExynosComponent->hComponentHandle = pVideoDec = NULL;

    pExynosPort = &pExynosComponent->pExynosPort[OUTPUT_PORT_INDEX];
    if (pExynosPort->processData.extInfo != NULL) {
        Exynos_OSAL_Free(pExynosPort->processData.extInfo);
        pExynosPort->processData.extInfo = NULL;
    }

    for(i = 0; i < ALL_PORT_NUM; i++) {
        pExynosPort = &pExynosComponent->pExynosPort[i];
        Exynos_OSAL_Free(pExynosPort->portDefinition.format.video.cMIMEType);
        pExynosPort->portDefinition.format.video.cMIMEType = NULL;
    }

    ret = Exynos_OMX_Port_Destructor(pOMXComponent);

    ret = Exynos_OMX_BaseComponent_Destructor(hComponent);

EXIT:
    FunctionOut();

    return ret;
}
