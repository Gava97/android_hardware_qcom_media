/*--------------------------------------------------------------------------
Copyright (c) 2009, 2016, The Linux Foundation. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of The Linux Foundation nor
      the names of its contributors may be used to endorse or promote
      products derived from this software without specific prior written
      permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
--------------------------------------------------------------------------*/
/*==========================================================================
                            O p e n M A X   w r a p p e r s
                             O p e n  M A X   C o r e

*//** @file qc_omx_msg.h
  This module contains the definitions of the OpenMAX core.

*//*========================================================================*/

#ifndef _QC_OMX_MSG_H_
#define _QC_OMX_MSG_H_

#ifdef _ENABLE_QC_MSG_LOG_
    #ifdef _ANDROID_
        #include <utils/Log.h>

        #ifdef __cplusplus
        extern "C" {
        #endif

        #ifndef LOGE
        #define LOGE ALOGE
        #endif

        #ifndef LOGW
        #define LOGW ALOGW
        #endif

        #ifndef LOGD
        #define LOGD ALOGD
        #endif

        #ifndef LOGV
        #define LOGV ALOGV
        #endif

        #ifndef LOGI
        #define LOGI ALOGI
        #endif

        #ifdef __cplusplus
        }
        #endif

        #define DEBUG_PRINT_ERROR LOGE
        #define DEBUG_PRINT       LOGI
        #define DEBUG_DETAIL      LOGV
    #else
#include <stdlib.h>
        enum {
          PRIO_ERROR=1,
          PRIO_INFO=1,
          PRIO_HIGH=2,
          PRIO_LOW=4
        };
        #define DEBUG_PRINT_CTL(level, fmt, args...)  \
          do {                                        \
            char *ptr = getenv("OMX_DEBUG_LEVEL");    \
            if ( level & (ptr?atoi(ptr):0) )          \
               printf(""fmt"\n", ##args);} while(0)
        #define DEBUG_PRINT_ERROR(fmt,args...) \
             DEBUG_PRINT_CTL(PRIO_ERROR, fmt, ##args)
        #define DEBUG_PRINT(fmt,args...)       \
             DEBUG_PRINT_CTL(PRIO_LOW, fmt, ##args)
        #define DEBUG_DETAIL(fmt,args...)      \
             DEBUG_PRINT_CTL(PRIO_HIGH, fmt, ##args)
    #endif // _ANDROID_
#else
    #define DEBUG_PRINT_ERROR(...)
    #define DEBUG_PRINT(...)
    #define DEBUG_DETAIL(...)
#endif // _ENABLE_QC_MSG_LOG_

#endif // _QC_OMX_MSG_H_
