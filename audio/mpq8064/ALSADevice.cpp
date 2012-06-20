/* ALSADevice.cpp
 **
 ** Copyright 2009 Wind River Systems
 ** Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#define LOG_TAG "ALSADevice"
#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0
#include <utils/Log.h>
#include <cutils/properties.h>
#include <linux/ioctl.h>
#include "AudioHardwareALSA.h"
#include <media/AudioRecord.h>

#define BTSCO_RATE_16KHZ 16000
#define USECASE_TYPE_RX 1
#define USECASE_TYPE_TX 2

#define AFE_PROXY_PERIOD_SIZE 3072
#define KILL_A2DP_THREAD 1
#define SIGNAL_A2DP_THREAD 2
#define PROXY_CAPTURE_DEVICE_NAME (const char *)("hw:0,8")

namespace sys_close {
    ssize_t lib_close(int fd) {
        return close(fd);
    }
};


namespace android_audio_legacy
{

ALSADevice::ALSADevice() {
    mDevSettingsFlag = TTY_OFF;
    btsco_samplerate = 8000;
    int callMode = AudioSystem::MODE_NORMAL;

    char value[128];
    property_get("persist.audio.handset.mic",value,"0");
    strlcpy(mic_type, value, sizeof(mic_type));
    property_get("persist.audio.fluence.mode",value,"0");
    if (!strncmp("broadside", value,9)) {
        fluence_mode = FLUENCE_MODE_BROADSIDE;
    } else {
        fluence_mode = FLUENCE_MODE_ENDFIRE;
    }
    strlcpy(curRxUCMDevice, "None", sizeof(curRxUCMDevice));
    strlcpy(curTxUCMDevice, "None", sizeof(curTxUCMDevice));

    mMixer = mixer_open("/dev/snd/controlC0");
    mProxyParams.mExitRead = false;
    resetProxyVariables();
    mProxyParams.mCaptureBufferSize = AFE_PROXY_PERIOD_SIZE;
    mProxyParams.mCaptureBuffer = NULL;
    mProxyParams.mProxyState = proxy_params::EProxyClosed;
    mProxyParams.mProxyPcmHandle = NULL;
    LOGD("ALSA Device opened");
};

ALSADevice::~ALSADevice()
{
    if (mMixer) mixer_close(mMixer);
    if(mProxyParams.mCaptureBuffer != NULL) {
        free(mProxyParams.mCaptureBuffer);
        mProxyParams.mCaptureBuffer = NULL;
    }
    mProxyParams.mProxyState = proxy_params::EProxyClosed;
}

// ----------------------------------------------------------------------------

int ALSADevice::deviceName(alsa_handle_t *handle, unsigned flags, char **value)
{
    LOGV("deviceName");
    int ret = 0;
    char ident[70];
    char *rxDevice, useCase[70];

    if (flags & PCM_IN) {
        strlcpy(ident, "CapturePCM/", sizeof(ident));
    } else {
        strlcpy(ident, "PlaybackPCM/", sizeof(ident));
    }
    strlcat(ident, handle->useCase, sizeof(ident));
    ret = snd_use_case_get(handle->ucMgr, ident, (const char **)value);
    LOGD("Device value returned is %s", (*value));
    return ret;
}

status_t ALSADevice::setHardwareParams(alsa_handle_t *handle)
{
    struct snd_pcm_hw_params *params;
    struct snd_compr_caps compr_cap;
    struct snd_compr_params compr_params;

    int32_t minPeroid, maxPeroid;
    unsigned long bufferSize, reqBuffSize;
    unsigned int periodTime, bufferTime;

    int status = 0;
    status_t err = NO_ERROR;
    unsigned int requestedRate = handle->sampleRate;
    int format = handle->format;

    bool dtsTranscode = false;
    char spdifFormat[20];
    char hdmiFormat[20];

    property_get("mpq.audio.spdif.format",spdifFormat,"0");
    property_get("mpq.audio.hdmi.format",hdmiFormat,"0");
    if (!strncmp(spdifFormat,"dts",sizeof(spdifFormat)) ||
        !strncmp(hdmiFormat,"dts",sizeof(hdmiFormat)))
        dtsTranscode = true;

    reqBuffSize = handle->bufferSize;
    if ((!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL,
                          strlen(SND_USE_CASE_VERB_HIFI_TUNNEL)) ||
        (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL,
                          strlen(SND_USE_CASE_MOD_PLAY_TUNNEL)))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL2,
                          strlen(SND_USE_CASE_VERB_HIFI_TUNNEL2)) ||
        (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL2,
                          strlen(SND_USE_CASE_MOD_PLAY_TUNNEL2))))) {
        if (ioctl(handle->handle->fd, SNDRV_COMPRESS_GET_CAPS, &compr_cap)) {
            LOGE("SNDRV_COMPRESS_GET_CAPS, failed Error no %d \n", errno);
            err = -errno;
            return err;
        }

        minPeroid = compr_cap.min_fragment_size;
        maxPeroid = compr_cap.max_fragment_size;
        LOGV("Min peroid size = %d , Maximum Peroid size = %d",\
            minPeroid, maxPeroid);
        //TODO: what if codec not supported or the array has wrong codec!!!!
        if (format == AUDIO_FORMAT_WMA || format == AUDIO_FORMAT_WMA_PRO) {
            LOGV("WMA CODEC");
            if (format == AUDIO_FORMAT_WMA_PRO) {
                compr_params.codec.id = compr_cap.codecs[4];
                compr_params.codec.ch_in = handle->channels;
            }
            else {
                compr_params.codec.id = compr_cap.codecs[3];
            }
            if (mWMA_params == NULL) {
                LOGV("WMA param config missing.");
                return BAD_VALUE;
            }
            compr_params.codec.bit_rate = mWMA_params[0];
            compr_params.codec.align = mWMA_params[1];
            compr_params.codec.options.wma.encodeopt = mWMA_params[2];
            compr_params.codec.format = mWMA_params[3];
            compr_params.codec.options.wma.bits_per_sample = mWMA_params[4];
            compr_params.codec.options.wma.channelmask = mWMA_params[5];
            compr_params.codec.options.wma.encodeopt1 = mWMA_params[6];
            compr_params.codec.options.wma.encodeopt2 = mWMA_params[7];
			compr_params.codec.sample_rate = handle->sampleRate;
        } else if(format == AUDIO_FORMAT_AAC || format == AUDIO_FORMAT_HE_AAC_V1 ||
           format == AUDIO_FORMAT_HE_AAC_V2 || format == AUDIO_FORMAT_AAC_ADIF) {
            LOGV("AAC CODEC");
            compr_params.codec.id = compr_cap.codecs[2];
        } else if(format == AUDIO_FORMAT_AC3 ||
                 (format == AUDIO_FORMAT_EAC3)) {
            LOGV("AC3 CODEC");
            compr_params.codec.id = compr_cap.codecs[2];
        } else if(format == AUDIO_FORMAT_MP3) {
             LOGV("MP3 CODEC");
             compr_params.codec.id = compr_cap.codecs[0];
        } else if(format == AUDIO_FORMAT_DTS) {
             LOGV("DTS CODEC");
             compr_params.codec.id = compr_cap.codecs[5];
        } else {
             LOGE("format not supported to open tunnel device");
             return BAD_VALUE;
        }
        if (dtsTranscode) {
//            Handle transcode path here
        }
        if (ioctl(handle->handle->fd, SNDRV_COMPRESS_SET_PARAMS, &compr_params)) {
            LOGE("SNDRV_COMPRESS_SET_PARAMS,failed Error no %d \n", errno);
            err = -errno;
            return err;
        }
        if (handle->channels > 2)
            handle->channels = 2;
    }
	if(handle->sampleRate > 48000) {
		LOGE("Sample rate >48000, opening the driver with 48000Hz");
		handle->sampleRate     = 48000;
	}
    params = (snd_pcm_hw_params*) calloc(1, sizeof(struct snd_pcm_hw_params));
    if (!params) {
		//SMANI:: Commented to fix build issues. FIX IT.
        //LOG_ALWAYS_FATAL("Failed to allocate ALSA hardware parameters!");
        return NO_INIT;
    }

    LOGD("setHardwareParams: reqBuffSize %d channels %d sampleRate %d",
         (int) reqBuffSize, handle->channels, handle->sampleRate);

    param_init(params);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS,
        (handle->handle->flags & PCM_MMAP) ? SNDRV_PCM_ACCESS_MMAP_INTERLEAVED
        : SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
                   SNDRV_PCM_FORMAT_S16_LE);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT,
                   SNDRV_PCM_SUBFORMAT_STD);
    LOGV("hw params -  before hifi2 condition %s", handle->useCase);

    if ((!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI2,
                           strlen(SND_USE_CASE_VERB_HIFI2))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_MUSIC2,
                           strlen(SND_USE_CASE_MOD_PLAY_MUSIC2))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI3,
                           strlen(SND_USE_CASE_VERB_HIFI3))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_MUSIC3,
                           strlen(SND_USE_CASE_MOD_PLAY_MUSIC3)))) {
        int ALSAbufferSize = getALSABufferSize(handle);
        param_set_int(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, ALSAbufferSize);
        LOGD("ALSAbufferSize = %d",ALSAbufferSize);
        param_set_int(params, SNDRV_PCM_HW_PARAM_PERIODS, MULTI_CHANNEL_PERIOD_COUNT);
    }
    else {
        param_set_min(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, reqBuffSize);
    }

    param_set_int(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_FRAME_BITS,
                   handle->channels - 1 ? 32 : 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_CHANNELS,
                  handle->channels);
    param_set_int(params, SNDRV_PCM_HW_PARAM_RATE, handle->sampleRate);
    param_set_hw_refine(handle->handle, params);

    if (param_set_hw_params(handle->handle, params)) {
        LOGE("cannot set hw params");
        return NO_INIT;
    }
    param_dump(params);

    handle->handle->buffer_size = pcm_buffer_size(params);
    handle->handle->period_size = pcm_period_size(params);
    handle->handle->period_cnt = handle->handle->buffer_size/handle->handle->period_size;
    LOGD("setHardwareParams: buffer_size %d, period_size %d, period_cnt %d",
        handle->handle->buffer_size, handle->handle->period_size,
        handle->handle->period_cnt);
    handle->handle->rate = handle->sampleRate;
    handle->handle->channels = handle->channels;
    handle->periodSize = handle->handle->period_size;
    handle->bufferSize = handle->handle->period_size;
    if ((!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI2,
                           strlen(SND_USE_CASE_VERB_HIFI2))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_MUSIC2,
                           strlen(SND_USE_CASE_MOD_PLAY_MUSIC2))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI3,
                           strlen(SND_USE_CASE_VERB_HIFI3))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_MUSIC3,
                           strlen(SND_USE_CASE_MOD_PLAY_MUSIC3)))) {
        handle->latency += (handle->handle->period_cnt * PCM_BUFFER_DURATION);
    }
    return NO_ERROR;
}

status_t ALSADevice::setSoftwareParams(alsa_handle_t *handle)
{
    struct snd_pcm_sw_params* params;
    struct pcm* pcm = handle->handle;

    unsigned long periodSize = pcm->period_size;

    params = (snd_pcm_sw_params*) calloc(1, sizeof(struct snd_pcm_sw_params));
    if (!params) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA software parameters!");
        return NO_INIT;
    }

    // Get the current software parameters
    params->tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    params->period_step = 1;
    if(((!strncmp(handle->useCase,SND_USE_CASE_MOD_PLAY_VOIP,
                            strlen(SND_USE_CASE_MOD_PLAY_VOIP))) ||
        (!strncmp(handle->useCase,SND_USE_CASE_VERB_IP_VOICECALL,
                            strlen(SND_USE_CASE_VERB_IP_VOICECALL))))){
          LOGV("setparam:  start & stop threshold for Voip ");
          params->avail_min = handle->channels - 1 ? periodSize/4 : periodSize/2;
          params->start_threshold = periodSize/2;
          params->stop_threshold = INT_MAX;
     } else {
         params->avail_min = handle->channels - 1 ? periodSize/2 : periodSize/4;
         params->start_threshold = handle->channels - 1 ? periodSize : periodSize/2;
         //Data required in packets for WMA which could be upto 16K.
         if (handle->format == AUDIO_FORMAT_WMA ||
              handle->format == AUDIO_FORMAT_WMA_PRO)
             params->start_threshold = params->start_threshold * 2;
         params->stop_threshold = INT_MAX;
     }
    if ((!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL,
                           strlen(SND_USE_CASE_VERB_HIFI_TUNNEL))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL,
                           strlen(SND_USE_CASE_MOD_PLAY_TUNNEL))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL2,
                           strlen(SND_USE_CASE_VERB_HIFI_TUNNEL2))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL2,
                           strlen(SND_USE_CASE_MOD_PLAY_TUNNEL2)))) {
        params->tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
        params->period_step = 1;
        params->xfer_align = (handle->handle->flags & PCM_MONO) ?
            handle->handle->period_size/2 : handle->handle->period_size/4;
    }
    params->silence_threshold = 0;
    params->silence_size = 0;

    if (param_set_sw_params(handle->handle, params)) {
        LOGE("cannot set sw params");
        return NO_INIT;
    }
    return NO_ERROR;
}

void ALSADevice::switchDevice(uint32_t devices, uint32_t mode)
{
    LOGV("switchDevice devices = %d, mode = %d", devices,mode);
    for(ALSAHandleList::iterator it = mDeviceList->begin(); it != mDeviceList->end(); ++it) {
        if((strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL,
                          strlen(SND_USE_CASE_VERB_HIFI_TUNNEL))) &&
           (strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL,
                          strlen(SND_USE_CASE_MOD_PLAY_TUNNEL))) &&
           (strncmp(it->useCase, SND_USE_CASE_VERB_HIFI2,
                          strlen(SND_USE_CASE_VERB_HIFI2))) &&
           (strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_MUSIC2,
                          strlen(SND_USE_CASE_MOD_PLAY_MUSIC2))) &&
           (strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL2,
                          strlen(SND_USE_CASE_VERB_HIFI_TUNNEL2))) &&
           (strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL2,
                          strlen(SND_USE_CASE_MOD_PLAY_TUNNEL2))) &&
           (strncmp(it->useCase, SND_USE_CASE_VERB_HIFI3,
                          strlen(SND_USE_CASE_VERB_HIFI3))) &&
           (strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_MUSIC3,
                          strlen(SND_USE_CASE_MOD_PLAY_MUSIC3)))) {
            switchDeviceUseCase(&(*it),devices,mode);
        }
    }
}

void ALSADevice::switchDeviceUseCase(alsa_handle_t *handle,
                                              uint32_t devices, uint32_t mode)
{
    unsigned usecase_type = 0;
    bool inCallDevSwitch = false;
    char useCase[MAX_STR_LEN] = {'\0'},*use_case = NULL;
    char *rxDeviceNew=NULL, *txDeviceNew=NULL;
    char *rxDeviceOld=NULL, *txDeviceOld=NULL;

    LOGV("switchDeviceUseCase devices = %d, mode = %d", devices,mode);
    if(handle->devices == devices)
        return;

    getDevices(devices, mode, &rxDeviceNew, &txDeviceNew);
    getDevices(handle->devices, handle->mode, &rxDeviceOld, &txDeviceOld);
    if ((rxDeviceNew != NULL) && (txDeviceNew != NULL)) {
        if (((strcmp(rxDeviceNew, rxDeviceOld)) || (strcmp(txDeviceNew, txDeviceOld))) &&
            (mode == AudioSystem::MODE_IN_CALL))
            inCallDevSwitch = true;
    }

    snd_use_case_get(handle->ucMgr, "_verb", (const char **)&use_case);

    if ((rxDeviceNew != NULL) && (rxDeviceOld != NULL)) {
        if ( ((strcmp(rxDeviceNew, rxDeviceOld)) || (inCallDevSwitch == true))) {
            if ((use_case != NULL) && (strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
                    strlen(SND_USE_CASE_VERB_INACTIVE)))) {
                usecase_type = getUseCaseType(use_case);
                if (usecase_type & USECASE_TYPE_RX) {
                    strlcpy(useCase, handle->useCase, MAX_STR_LEN);
                    LOGD("Deroute use case %s type is %d\n", use_case, usecase_type);
                    if(!strcmp(use_case,handle->useCase)) {
                        snd_use_case_set_case(handle->ucMgr, "_verb", SND_USE_CASE_VERB_INACTIVE,rxDeviceOld);
                    } else {
                        snd_use_case_set_case(handle->ucMgr, "_dismod",useCase,rxDeviceOld);
                    }
                }
            }
        }
    }
    if ((txDeviceNew != NULL) && (txDeviceOld != NULL)) {
        if ( ((strcmp(txDeviceNew, txDeviceOld)) || (inCallDevSwitch == true))) {
            if ((use_case != NULL) && (strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
                    strlen(SND_USE_CASE_VERB_INACTIVE)))) {
                usecase_type = getUseCaseType(use_case);
                if ((usecase_type & USECASE_TYPE_TX) && (!(usecase_type & USECASE_TYPE_RX))) {
                    strlcpy(useCase, handle->useCase, MAX_STR_LEN);
                    LOGD("Deroute use case %s type is %d\n", use_case, usecase_type);
                    if(!strcmp(use_case,handle->useCase)) {
                        snd_use_case_set_case(handle->ucMgr, "_verb", SND_USE_CASE_VERB_INACTIVE,txDeviceOld);
                    } else {
                        snd_use_case_set_case(handle->ucMgr, "_dismod",useCase,txDeviceOld);
                    }
                }
            }
        }
    }

    disableDevice(handle);

    if (rxDeviceNew != NULL) {
        if ((use_case != NULL) && (strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
            strlen(SND_USE_CASE_VERB_INACTIVE))) && (!strncmp(use_case, useCase, MAX_UC_LEN))) {
            snd_use_case_set_case(handle->ucMgr, "_verb", useCase,rxDeviceNew);
        } else {
            snd_use_case_set_case(handle->ucMgr, "_enamod", useCase, rxDeviceNew);
        }
        handle->activeDevice = devices;
        handle->devices = devices;
    }
    if (txDeviceNew != NULL) {
        if ((use_case != NULL) && (strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
            strlen(SND_USE_CASE_VERB_INACTIVE))) && (!strncmp(use_case, useCase, MAX_UC_LEN))) {
            snd_use_case_set_case(handle->ucMgr, "_verb", useCase,txDeviceNew);
        } else {
            snd_use_case_set_case(handle->ucMgr, "_enamod", useCase, txDeviceNew);
        }
        handle->activeDevice = devices;
        handle->devices = devices;
    }
    LOGE("After enable device");
    if(rxDeviceNew != NULL) {
        free(rxDeviceNew);
        rxDeviceNew = NULL;
    }
    if(rxDeviceOld != NULL) {
        free(rxDeviceOld);
        rxDeviceOld = NULL;
    }
    if(txDeviceNew != NULL) {
        free(txDeviceNew);
        txDeviceNew = NULL;
    }
    if(txDeviceOld != NULL) {
        free(txDeviceOld);
        txDeviceOld = NULL;
    }
    if (use_case != NULL) {
        free(use_case);
        use_case = NULL;
    }
}
// ----------------------------------------------------------------------------

status_t ALSADevice::open(alsa_handle_t *handle)
{
    LOGV("open");
    char *devName;
    unsigned flags = 0;
    int err = NO_ERROR;
    /* No need to call s_close for LPA as pcm device open and close is handled by LPAPlayer in stagefright */
    if((!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER,
                           strlen(SND_USE_CASE_VERB_HIFI_LOW_POWER))) || 
       (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_LPA,
                           strlen(SND_USE_CASE_VERB_HIFI_LOW_POWER)))) {
        LOGD("s_open: Opening LPA playback");
        return NO_ERROR;
    }

    close(handle);
    LOGD("s_open: handle %p", handle);

    if(handle->channels == 1 && handle->devices & AudioSystem::DEVICE_OUT_AUX_DIGITAL) {
        err = setHDMIChannelCount();
        if(err != OK) {
            LOGE("setHDMIChannelCount err = %d", err);
            return err;
        }
    }

    // ASoC multicomponent requires a valid path (frontend/backend) for
    // the device to be opened

    // The PCM stream is opened in blocking mode, per ALSA defaults.  The
    // AudioFlinger seems to assume blocking mode too, so asynchronous mode
    // should not be used.
    // ToDo: Add a condition check for HIFI2 use cases also
    if ((!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI,
                            strlen(SND_USE_CASE_VERB_HIFI))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI2,
                            strlen(SND_USE_CASE_VERB_HIFI2))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_MUSIC,
                            strlen(SND_USE_CASE_MOD_PLAY_MUSIC))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_MUSIC2,
                            strlen(SND_USE_CASE_MOD_PLAY_MUSIC2))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL,
                            strlen(SND_USE_CASE_VERB_HIFI_TUNNEL))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL,
                            strlen(SND_USE_CASE_MOD_PLAY_TUNNEL))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI3,
                            strlen(SND_USE_CASE_VERB_HIFI3))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_MUSIC3,
                            strlen(SND_USE_CASE_MOD_PLAY_MUSIC3))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL2,
                            strlen(SND_USE_CASE_VERB_HIFI_TUNNEL2))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL2,
                            strlen(SND_USE_CASE_MOD_PLAY_TUNNEL2)))) {
        flags = PCM_OUT;
    } else {
        flags = PCM_IN;
    }

    if (handle->channels == 1) {
        flags |= PCM_MONO;
    } else if (handle->channels == 6) {
        flags |= PCM_5POINT1;
    } else {
        flags |= PCM_STEREO;
    }
    LOGD("s_open: handle %p", handle);
    if (deviceName(handle, flags, &devName) < 0) {
        LOGE("Failed to get pcm device node: %s", devName);
        return NO_INIT;
    }
    if (!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL,
                           strlen(SND_USE_CASE_VERB_HIFI_TUNNEL)) ||
        (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL,
                            strlen(SND_USE_CASE_MOD_PLAY_TUNNEL))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL2,
                            strlen(SND_USE_CASE_VERB_HIFI_TUNNEL2))) ||
        (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL2,
                            strlen(SND_USE_CASE_MOD_PLAY_TUNNEL2)))) {
        flags |= DEBUG_ON | PCM_MMAP;
    }
    handle->handle = pcm_open(flags, (char*)devName);
    LOGE("s_open: opening ALSA device '%s'", devName);
    if (!handle->handle) {
        LOGE("s_open: Failed to initialize ALSA device '%s'", devName);
        free(devName);
        return NO_INIT;
    }
    handle->handle->flags = flags;
    LOGD("setting hardware parameters");

    err = setHardwareParams(handle);
    if (err == NO_ERROR) {
        LOGD("setting software parameters");
        err = setSoftwareParams(handle);
    }
    if(err != NO_ERROR) {
        LOGE("Set HW/SW params failed: Closing the pcm stream");
        standby(handle);
        if(devName) {
            free(devName);
            devName = NULL;
        }
        return err;
    }

    free(devName);
    return NO_ERROR;
}

status_t ALSADevice::startVoipCall(alsa_handle_t *handle)
{

    char* devName;
    char* devName1;
    unsigned flags = 0;
    int err = NO_ERROR;
    uint8_t voc_pkt[VOIP_BUFFER_MAX_SIZE];

    close(handle);
    flags = PCM_OUT;
    flags |= PCM_MONO;
    LOGV("s_open:s_start_voip_call  handle %p", handle);

    if (deviceName(handle, flags, &devName) < 0) {
         LOGE("Failed to get pcm device node");
         return NO_INIT;
    }

     handle->handle = pcm_open(flags, (char*)devName);

     if (!handle->handle) {
          free(devName);
          LOGE("s_open: Failed to initialize ALSA device '%s'", devName);
          return NO_INIT;
     }

     if (!pcm_ready(handle->handle)) {
         LOGE(" pcm ready failed");
     }

     handle->handle->flags = flags;
     err = setHardwareParams(handle);

     if (err == NO_ERROR) {
         err = setSoftwareParams(handle);
     }

     err = pcm_prepare(handle->handle);
     if(err != NO_ERROR) {
         LOGE("DEVICE_OUT_DIRECTOUTPUT: pcm_prepare failed");
     }

     /* first write required start dsp */
     memset(&voc_pkt,0,sizeof(voc_pkt));
     pcm_write(handle->handle,&voc_pkt,handle->handle->period_size);
     handle->rxHandle = handle->handle;
     if(devName) {
         free(devName);
         devName = NULL;
     }
     LOGV("s_open: DEVICE_IN_COMMUNICATION ");
     flags = PCM_IN;
     flags |= PCM_MONO;
     handle->handle = 0;

     if (deviceName(handle, flags, &devName1) < 0) {
        LOGE("Failed to get pcm device node");
        return NO_INIT;
     }
     handle->handle = pcm_open(flags, (char*)devName1);

     if (!handle->handle) {
         if(devName) {
             free(devName);
             devName = NULL;
         }
         if(devName1) {
             free(devName1);
             devName1 = NULL;
         }
         LOGE("s_open: Failed to initialize ALSA device '%s'", devName);
         return NO_INIT;
     }

     if (!pcm_ready(handle->handle)) {
        LOGE(" pcm ready in failed");
     }

     handle->handle->flags = flags;
     err = setHardwareParams(handle);

     if (err == NO_ERROR) {
         err = setSoftwareParams(handle);
     }


     err = pcm_prepare(handle->handle);
     if(err != NO_ERROR) {
         LOGE("DEVICE_IN_COMMUNICATION: pcm_prepare failed");
     }

     /* first read required start dsp */
     memset(&voc_pkt,0,sizeof(voc_pkt));
     pcm_read(handle->handle,&voc_pkt,handle->handle->period_size);

     if(devName) {
         free(devName);
         devName = NULL;
     }
     if(devName1) {
         free(devName1);
         devName1 = NULL;
     }
     return NO_ERROR;
}

status_t ALSADevice::startVoiceCall(alsa_handle_t *handle)
{
    char* devName;
    unsigned flags = 0;
    int err = NO_ERROR;

    LOGD("startVoiceCall: handle %p", handle);
    // ASoC multicomponent requires a valid path (frontend/backend) for
    // the device to be opened

    flags = PCM_OUT | PCM_MONO;
    if (deviceName(handle, flags, &devName) < 0) {
        LOGE("Failed to get pcm device node");
        return NO_INIT;
    }
    handle->handle = pcm_open(flags, (char*)devName);
    if (!handle->handle) {
        LOGE("startVoiceCall: could not open PCM device");
        goto Error;
    }

    handle->handle->flags = flags;
    err = setHardwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("startVoiceCall: setHardwareParams failed");
        goto Error;
    }

    err = setSoftwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("startVoiceCall: setSoftwareParams failed");
        goto Error;
    }

    err = pcm_prepare(handle->handle);
    if(err != NO_ERROR) {
        LOGE("startVoiceCall: pcm_prepare failed");
        goto Error;
    }

    if (ioctl(handle->handle->fd, SNDRV_PCM_IOCTL_START)) {
        LOGE("startVoiceCall:SNDRV_PCM_IOCTL_START failed\n");
        goto Error;
    }

    // Store the PCM playback device pointer in rxHandle
    handle->rxHandle = handle->handle;
    free(devName);

    // Open PCM capture device
    flags = PCM_IN | PCM_MONO;
    if (deviceName(handle, flags, &devName) < 0) {
        LOGE("Failed to get pcm device node");
        goto Error;
    }
    handle->handle = pcm_open(flags, (char*)devName);
    if (!handle->handle) {
        free(devName);
        goto Error;
    }

    handle->handle->flags = flags;
    err = setHardwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("startVoiceCall: setHardwareParams failed");
        goto Error;
    }

    err = setSoftwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("startVoiceCall: setSoftwareParams failed");
        goto Error;
    }

    err = pcm_prepare(handle->handle);
    if(err != NO_ERROR) {
        LOGE("startVoiceCall: pcm_prepare failed");
        goto Error;
    }

    if (ioctl(handle->handle->fd, SNDRV_PCM_IOCTL_START)) {
        LOGE("startVoiceCall:SNDRV_PCM_IOCTL_START failed\n");
        goto Error;
    }
    if(devName) {
        free(devName);
        devName = NULL;
    }
    return NO_ERROR;

Error:
    LOGE("startVoiceCall: Failed to initialize ALSA device '%s'", devName);
    if(devName) {
        free(devName);
        devName = NULL;
    }
    close(handle);
    return NO_INIT;
}

status_t ALSADevice::startFm(alsa_handle_t *handle)
{
    int err = NO_ERROR;

    err = startLoopback(handle);

    if(err == NO_ERROR)
        setFmVolume(fmVolume);

    return err;
}

status_t ALSADevice::startLoopback(alsa_handle_t *handle)
{
    char *devName;
    unsigned flags = 0;
    int err = NO_ERROR;

    LOGE("s_start_fm: handle %p", handle);

    // ASoC multicomponent requires a valid path (frontend/backend) for
    // the device to be opened

    flags = PCM_OUT | PCM_STEREO;
    if (deviceName(handle, flags, &devName) < 0) {
        LOGE("Failed to get pcm device node");
        goto Error;
    }
    handle->handle = pcm_open(flags, (char*)devName);
    if (!handle->handle) {
        LOGE("s_start_fm: could not open PCM device");
        goto Error;
    }

    handle->handle->flags = flags;
    err = setHardwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("s_start_fm: setHardwareParams failed");
        goto Error;
    }

    err = setSoftwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("s_start_fm: setSoftwareParams failed");
        goto Error;
    }

    err = pcm_prepare(handle->handle);
    if(err != NO_ERROR) {
        LOGE("s_start_fm: setSoftwareParams failed");
        goto Error;
    }

    if (ioctl(handle->handle->fd, SNDRV_PCM_IOCTL_START)) {
        LOGE("s_start_fm: SNDRV_PCM_IOCTL_START failed\n");
        goto Error;
    }

    // Store the PCM playback device pointer in rxHandle
    handle->rxHandle = handle->handle;
    free(devName);

    // Open PCM capture device
    flags = PCM_IN | PCM_STEREO;
    if (deviceName(handle, flags, &devName) < 0) {
        LOGE("Failed to get pcm device node");
        goto Error;
    }
    handle->handle = pcm_open(flags, (char*)devName);
    if (!handle->handle) {
        goto Error;
    }

    handle->handle->flags = flags;
    err = setHardwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("s_start_fm: setHardwareParams failed");
        goto Error;
    }

    err = setSoftwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("s_start_fm: setSoftwareParams failed");
        goto Error;
    }

    err = pcm_prepare(handle->handle);
    if(err != NO_ERROR) {
        LOGE("s_start_fm: pcm_prepare failed");
        goto Error;
    }

    if (ioctl(handle->handle->fd, SNDRV_PCM_IOCTL_START)) {
        LOGE("s_start_fm: SNDRV_PCM_IOCTL_START failed\n");
        goto Error;
    }
    if(devName) {
        free(devName);
        devName = NULL;
    }
    return NO_ERROR;

Error:
    if(devName) {
        free(devName);
        devName = NULL;
    }
    close(handle);
    return NO_INIT;
}

status_t ALSADevice::setChannelStatus(unsigned char *channelStatus)
{
    struct snd_aes_iec958 iec958;
    status_t err = NO_ERROR;
    memcpy(iec958.status, channelStatus,24);
    unsigned int ptr = (unsigned int)&iec958;
    setMixerControl("IEC958 Playback PCM Stream",ptr,0);

    return err;
}

status_t ALSADevice::setFmVolume(int value)
{
    status_t err = NO_ERROR;
    setMixerControl("Internal FM RX Volume",value,0);
    fmVolume = value;

    return err;
}

status_t ALSADevice::setLpaVolume(int value)
{
    status_t err = NO_ERROR;
    setMixerControl("LPA RX Volume",value,0);

    return err;
}

void ALSADevice::setDeviceList(ALSAHandleList *mParentDeviceList)
{
    mDeviceList = mParentDeviceList;
}

status_t ALSADevice::start(alsa_handle_t *handle)
{
    status_t err = NO_ERROR;
    bool dtsTranscode = false;
    char spdifFormat[128];
    char hdmiFormat[128];

    property_get("mpq.audio.spdif.format",spdifFormat,"0");
    property_get("mpq.audio.hdmi.format",hdmiFormat,"0");
    if (!strncmp(spdifFormat,"dts",sizeof(spdifFormat)) ||
        !strncmp(hdmiFormat,"dts",sizeof(hdmiFormat)))
        dtsTranscode = true;

    if(!handle->handle) {
        LOGE("No active PCM driver to start");
        return err;
    }

    err = pcm_prepare(handle->handle);

    return err;
}

status_t ALSADevice::close(alsa_handle_t *handle)
{
    int ret;
    status_t err = NO_ERROR;
     struct pcm *h = handle->rxHandle;

    handle->rxHandle = 0;
    LOGD("close: handle %p h %p", handle, h);
    if (h) {
        LOGV("close rxHandle\n");
        err = pcm_close(h);
        if(err != NO_ERROR) {
            LOGE("close: pcm_close failed for rxHandle with err %d", err);
        }
    }

    h = handle->handle;
    handle->handle = 0;

    if (h) {
          LOGV("close handle h %p\n", h);
        err = pcm_close(h);
        if(err != NO_ERROR) {
            LOGE("close: pcm_close failed for handle with err %d", err);
        }
        disableDevice(handle);
    } else if((!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER,
                                  strlen(SND_USE_CASE_VERB_HIFI_LOW_POWER))) ||
              (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_LPA,
                                  strlen(SND_USE_CASE_MOD_PLAY_LPA)))) {
        disableDevice(handle);
    }

    return err;
}

/*
    this is same as close, but don't discard
    the device/mode info. This way we can still
    close the device, hit idle and power-save, reopen the pcm
    for the same device/mode after resuming
*/
status_t ALSADevice::standby(alsa_handle_t *handle)
{
    int ret;
    status_t err = NO_ERROR;  
    struct pcm *h = handle->rxHandle;
    handle->rxHandle = 0;
    LOGD("s_standby: handle %p h %p", handle, h);
    if (h) {
        LOGE("s_standby  rxHandle\n");
        err = pcm_close(h);
        if(err != NO_ERROR) {
            LOGE("s_standby: pcm_close failed for rxHandle with err %d", err);
        }
    }

    h = handle->handle;
    handle->handle = 0;

    if (h) {
          LOGE("s_standby handle h %p\n", h);
        err = pcm_close(h);
        if(err != NO_ERROR) {
            LOGE("s_standby: pcm_close failed for handle with err %d", err);
        }
        disableDevice(handle);
    } else if((!strncmp(handle->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER,
                                  strlen(SND_USE_CASE_VERB_HIFI_LOW_POWER))) ||
              (!strncmp(handle->useCase, SND_USE_CASE_MOD_PLAY_LPA,
                                  strlen(SND_USE_CASE_MOD_PLAY_LPA)))) {
        disableDevice(handle);
    }

    return err;
}

status_t ALSADevice::route(uint32_t devices, int mode)
{
    status_t status = NO_ERROR;

    LOGD("s_route: devices 0x%x in mode %d", devices, mode);
    callMode = mode;
    switchDevice(devices, mode);
    return status;
}

int  ALSADevice::getUseCaseType(const char *useCase)
{
    LOGE("use case is %s\n", useCase);
    if (!strncmp(useCase, SND_USE_CASE_VERB_HIFI,
           strlen(SND_USE_CASE_VERB_HIFI)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER,
           strlen(SND_USE_CASE_VERB_HIFI_LOW_POWER)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL,
           strlen(SND_USE_CASE_VERB_HIFI_TUNNEL)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_HIFI2,
           strlen(SND_USE_CASE_VERB_HIFI2)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL2,
           strlen(SND_USE_CASE_VERB_HIFI_TUNNEL2)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_HIFI3,
           strlen(SND_USE_CASE_VERB_HIFI3)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_DIGITAL_RADIO,
           strlen(SND_USE_CASE_VERB_DIGITAL_RADIO)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_PLAY_MUSIC,
           strlen(SND_USE_CASE_MOD_PLAY_MUSIC)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_PLAY_LPA,
           strlen(SND_USE_CASE_MOD_PLAY_LPA)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL,
           strlen(SND_USE_CASE_MOD_PLAY_TUNNEL)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_PLAY_MUSIC2,
           strlen(SND_USE_CASE_MOD_PLAY_MUSIC2)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL2,
           strlen(SND_USE_CASE_MOD_PLAY_TUNNEL2)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_PLAY_MUSIC3,
           strlen(SND_USE_CASE_MOD_PLAY_MUSIC3)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_PLAY_FM,
           strlen(SND_USE_CASE_MOD_PLAY_FM))) {
        return USECASE_TYPE_RX;
    } else if (!strncmp(useCase, SND_USE_CASE_VERB_HIFI_REC,
           strlen(SND_USE_CASE_VERB_HIFI_REC)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_HIFI_REC2,
           strlen(SND_USE_CASE_VERB_HIFI_REC2)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_HIFI_REC_COMPRESSED,
           strlen(SND_USE_CASE_VERB_HIFI_REC_COMPRESSED)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_FM_REC,
           strlen(SND_USE_CASE_VERB_FM_REC)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_FM_A2DP_REC,
           strlen(SND_USE_CASE_VERB_FM_A2DP_REC)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC,
           strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC2,
           strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC2)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC_COMPRESSED,
           strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC_COMPRESSED)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_CAPTURE_FM,
           strlen(SND_USE_CASE_MOD_CAPTURE_FM)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_CAPTURE_A2DP_FM,
           strlen(SND_USE_CASE_MOD_CAPTURE_A2DP_FM))) {
        return USECASE_TYPE_TX;
    } else if (!strncmp(useCase, SND_USE_CASE_VERB_VOICECALL,
           strlen(SND_USE_CASE_VERB_VOICECALL)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_IP_VOICECALL,
           strlen(SND_USE_CASE_VERB_IP_VOICECALL)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_DL_REC,
           strlen(SND_USE_CASE_VERB_DL_REC)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_UL_DL_REC,
           strlen(SND_USE_CASE_VERB_UL_DL_REC)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_PLAY_VOICE,
           strlen(SND_USE_CASE_MOD_PLAY_VOICE)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_PLAY_VOIP,
           strlen(SND_USE_CASE_MOD_PLAY_VOIP)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_CAPTURE_VOICE_DL,
           strlen(SND_USE_CASE_MOD_CAPTURE_VOICE_DL)) ||
        !strncmp(useCase, SND_USE_CASE_MOD_CAPTURE_VOICE_UL_DL,
           strlen(SND_USE_CASE_MOD_CAPTURE_VOICE_UL_DL)) ) {
        return (USECASE_TYPE_RX | USECASE_TYPE_TX);
  } else {
       LOGE("unknown use case %s\n", useCase);
        return 0;
  }
}

void ALSADevice::disableDevice(alsa_handle_t *handle)
{
    bool disableRxDevice = true;
    bool disableTxDevice = true;

    char *rxDeviceToDisable=NULL, *txDeviceToDisable=NULL;
    char *rxDevice=NULL, *txDevice=NULL;

    getDevices(handle->activeDevice, handle->mode, &rxDeviceToDisable, &txDeviceToDisable);
    for(ALSAHandleList::iterator it = mDeviceList->begin(); it != mDeviceList->end(); ++it) {
        if(it->useCase != NULL) {
            if(strcmp(it->useCase,handle->useCase)) {
                if((&(*it)) != handle && handle->activeDevice && it->activeDevice && (it->activeDevice & handle->activeDevice)) {
                    LOGD("disableRxDevice - false ");
                    disableRxDevice = false;
                    disableTxDevice = false;
                }
            }
        }
    }

    if(disableRxDevice && rxDeviceToDisable) {
        if(handle->devices & AudioSystem::DEVICE_OUT_SPDIF) {
            char *tempRxDevice = NULL;
            tempRxDevice = getUCMDevice(handle->devices & ~AudioSystem::DEVICE_OUT_SPDIF,0);
            if(tempRxDevice != NULL) {
                snd_use_case_set_case(handle->ucMgr, "_disdev",tempRxDevice,handle->useCase);
                free(tempRxDevice);
            }
        }
        snd_use_case_set_case(handle->ucMgr, "_disdev",rxDeviceToDisable,handle->useCase);
    }
    if(disableTxDevice && txDeviceToDisable) {
        snd_use_case_set_case(handle->ucMgr, "_disdev",txDeviceToDisable,handle->useCase);
    }
    handle->activeDevice = 0;

    if(rxDeviceToDisable != NULL) {
        free(rxDeviceToDisable);
        rxDeviceToDisable = NULL;
    }
    if(txDeviceToDisable != NULL) {
        free(txDeviceToDisable);
        txDeviceToDisable = NULL;
    }
    if(rxDevice != NULL) {
        free(rxDevice);
        rxDevice = NULL;
    }
    if(txDevice != NULL) {
        free(txDevice);
        txDevice = NULL;
    }
}

char* ALSADevice::getUCMDevice(uint32_t devices, int input)
{
    if (!input) {
        if (!(mDevSettingsFlag & TTY_OFF) &&
            (callMode == AudioSystem::MODE_IN_CALL) &&
            ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
             (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE) ||
             (devices & AudioSystem::DEVICE_OUT_ANC_HEADSET) ||
             (devices & AudioSystem::DEVICE_OUT_ANC_HEADPHONE))) {
             if (mDevSettingsFlag & TTY_VCO) {
                 return strdup(SND_USE_CASE_DEV_TTY_HEADSET_RX);
             } else if (mDevSettingsFlag & TTY_FULL) {
                 return strdup(SND_USE_CASE_DEV_TTY_FULL_RX);
             } else if (mDevSettingsFlag & TTY_HCO) {
                 return strdup(SND_USE_CASE_DEV_EARPIECE); /* HANDSET RX */
             }
        } else if( (devices & AudioSystem::DEVICE_OUT_SPEAKER) &&
                   (devices & AudioSystem::DEVICE_OUT_SPDIF) &&
                   ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
                    (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE) ) ) {
            if (mDevSettingsFlag & ANC_FLAG) {
                return strdup(SND_USE_CASE_DEV_SPDIF_SPEAKER_ANC_HEADSET);
            } else {
                return strdup(SND_USE_CASE_DEV_SPDIF_SPEAKER_HEADSET);
            }
        } else if( (devices & AudioSystem::DEVICE_OUT_SPEAKER) &&
                   (devices & AudioSystem::DEVICE_OUT_PROXY) &&
                   ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
                    (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE) ) ) {
            if (mDevSettingsFlag & ANC_FLAG) {
                return strdup(SND_USE_CASE_DEV_PROXY_RX_SPEAKER_ANC_HEADSET);
            } else {
                return strdup(SND_USE_CASE_DEV_PROXY_RX_SPEAKER_HEADSET);
            }
        } else if ((devices & AudioSystem::DEVICE_OUT_SPEAKER) &&
            ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
            (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE))) {
            if (mDevSettingsFlag & ANC_FLAG) {
                return strdup(SND_USE_CASE_DEV_SPEAKER_ANC_HEADSET); /* COMBO SPEAKER+ANC HEADSET RX */
            } else {
                return strdup(SND_USE_CASE_DEV_SPEAKER_HEADSET); /* COMBO SPEAKER+HEADSET RX */
            }
        } else if ((devices & AudioSystem::DEVICE_OUT_SPDIF) &&
                   ((devices & AudioSystem::DEVICE_OUT_ANC_HEADSET)||
                    (devices & AudioSystem::DEVICE_OUT_ANC_HEADPHONE)) ) {
            return strdup(SND_USE_CASE_DEV_SPDIF_ANC_HEADSET);
        } else if ((devices & AudioSystem::DEVICE_OUT_PROXY) &&
                   ((devices & AudioSystem::DEVICE_OUT_ANC_HEADSET)||
                    (devices & AudioSystem::DEVICE_OUT_ANC_HEADPHONE)) ) {
            return strdup(SND_USE_CASE_DEV_PROXY_RX_ANC_HEADSET);
        } else if ((devices & AudioSystem::DEVICE_OUT_SPEAKER) &&
            ((devices & AudioSystem::DEVICE_OUT_ANC_HEADSET) ||
            (devices & AudioSystem::DEVICE_OUT_ANC_HEADPHONE))) {
            return strdup(SND_USE_CASE_DEV_SPEAKER_ANC_HEADSET); /* COMBO SPEAKER+ANC HEADSET RX */
        } else if ((devices & AudioSystem::DEVICE_OUT_SPEAKER) &&
                 (devices & AudioSystem::DEVICE_OUT_FM_TX)) {
            return strdup(SND_USE_CASE_DEV_SPEAKER_FM_TX); /* COMBO SPEAKER+FM_TX RX */
        } else if ((devices & AudioSystem::DEVICE_OUT_SPEAKER) &&
                 (devices & AudioSystem::DEVICE_OUT_SPDIF)) {
            return strdup(SND_USE_CASE_DEV_SPDIF_SPEAKER); /* COMBO SPEAKER+ SPDIF */
        } else if ((devices & AudioSystem::DEVICE_OUT_EARPIECE) &&
                 (devices & AudioSystem::DEVICE_OUT_SPDIF)) {
            return strdup(SND_USE_CASE_DEV_SPDIF_HANDSET); /* COMBO EARPIECE + SPDIF */
        } else if ((devices & AudioSystem::DEVICE_OUT_SPEAKER) &&
                 (devices & AudioSystem::DEVICE_OUT_PROXY)) {
            return strdup(SND_USE_CASE_DEV_PROXY_RX_SPEAKER); /* COMBO SPEAKER + PROXY RX */
        } else if ((devices & AudioSystem::DEVICE_OUT_EARPIECE) &&
                 (devices & AudioSystem::DEVICE_OUT_PROXY)) {
            return strdup(SND_USE_CASE_DEV_PROXY_RX_HANDSET); /* COMBO EARPIECE + PROXY RX */
        } else if (devices & AudioSystem::DEVICE_OUT_EARPIECE) {
            return strdup(SND_USE_CASE_DEV_EARPIECE); /* HANDSET RX */
        } else if (devices & AudioSystem::DEVICE_OUT_SPEAKER) {
            return strdup(SND_USE_CASE_DEV_SPEAKER); /* SPEAKER RX */
        } else if ((devices & AudioSystem::DEVICE_OUT_SPDIF) &&
                   ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
                    (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE))) {
            if (mDevSettingsFlag & ANC_FLAG) {
                return strdup(SND_USE_CASE_DEV_SPDIF_ANC_HEADSET);
            } else {
                return strdup(SND_USE_CASE_DEV_SPDIF_HEADSET);
            }
        } else if ((devices & AudioSystem::DEVICE_OUT_PROXY) &&
                   ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
                    (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE))) {
            if (mDevSettingsFlag & ANC_FLAG) {
                return strdup(SND_USE_CASE_DEV_PROXY_RX_ANC_HEADSET);
            } else {
                return strdup(SND_USE_CASE_DEV_PROXY_RX_HEADSET);
            }
        } else if ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
                   (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE)) {
            if (mDevSettingsFlag & ANC_FLAG) {
                return strdup(SND_USE_CASE_DEV_ANC_HEADSET); /* ANC HEADSET RX */
            } else {
                return strdup(SND_USE_CASE_DEV_HEADPHONES); /* HEADSET RX */
            }
        } else if ((devices & AudioSystem::DEVICE_OUT_ANC_HEADSET) ||
                   (devices & AudioSystem::DEVICE_OUT_ANC_HEADPHONE)) {
            return strdup(SND_USE_CASE_DEV_ANC_HEADSET); /* ANC HEADSET RX */
        } else if ((devices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO) ||
                  (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET) ||
                  (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT)) {
            if (btsco_samplerate == BTSCO_RATE_16KHZ)
                return strdup(SND_USE_CASE_DEV_BTSCO_WB_RX); /* BTSCO RX*/
            else
                return strdup(SND_USE_CASE_DEV_BTSCO_NB_RX); /* BTSCO RX*/
        } else if ((devices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP) ||
                   (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES) ||
                   (devices & AudioSystem::DEVICE_OUT_DIRECTOUTPUT) ||
                   (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER)) {
            /* Nothing to be done, use current active device */
            return strdup(curRxUCMDevice);
        } else if ((devices & AudioSystem::DEVICE_OUT_SPDIF) &&
                   (devices & AudioSystem::DEVICE_OUT_SPEAKER)) {
            return strdup(SND_USE_CASE_DEV_SPDIF_SPEAKER);
        } else if ((devices & AudioSystem::DEVICE_OUT_SPDIF) &&
                   (devices & AudioSystem::DEVICE_OUT_AUX_DIGITAL)) {
            return strdup(SND_USE_CASE_DEV_HDMI_SPDIF);
        } else if (devices & AudioSystem::DEVICE_OUT_AUX_DIGITAL) {
            return strdup(SND_USE_CASE_DEV_HDMI); /* HDMI RX */
        } else if (devices & AudioSystem::DEVICE_OUT_PROXY) {
            return strdup(SND_USE_CASE_DEV_PROXY_RX); /* PROXY RX */
        } else if (devices & AudioSystem::DEVICE_OUT_FM_TX) {
            return strdup(SND_USE_CASE_DEV_FM_TX); /* FM Tx */
        } else if (devices & AudioSystem::DEVICE_OUT_SPDIF) {
            return strdup(SND_USE_CASE_DEV_SPDIF);
        } else if (devices & AudioSystem::DEVICE_OUT_DEFAULT) {
            return strdup(SND_USE_CASE_DEV_SPEAKER); /* SPEAKER RX */
        } else {
            LOGD("No valid output device: %u", devices);
        }
    } else {
        if (!(mDevSettingsFlag & TTY_OFF) &&
            (callMode == AudioSystem::MODE_IN_CALL) &&
            ((devices & AudioSystem::DEVICE_IN_WIRED_HEADSET) ||
             (devices & AudioSystem::DEVICE_IN_ANC_HEADSET))) {
             if (mDevSettingsFlag & TTY_HCO) {
                 return strdup(SND_USE_CASE_DEV_TTY_HEADSET_TX);
             } else if (mDevSettingsFlag & TTY_FULL) {
                 return strdup(SND_USE_CASE_DEV_TTY_FULL_TX);
             } else if (mDevSettingsFlag & TTY_VCO) {
                 if (!strncmp(mic_type, "analog", 6)) {
                     return strdup(SND_USE_CASE_DEV_HANDSET); /* HANDSET TX */
                 } else {
                     return strdup(SND_USE_CASE_DEV_LINE); /* BUILTIN-MIC TX */
                 }
             }
        } else if (devices & AudioSystem::DEVICE_IN_BUILTIN_MIC) {
            if (!strncmp(mic_type, "analog", 6)) {
                return strdup(SND_USE_CASE_DEV_HANDSET); /* HANDSET TX */
            } else {
                if (mDevSettingsFlag & DMIC_FLAG) {
                    if (fluence_mode == FLUENCE_MODE_ENDFIRE) {
                        return strdup(SND_USE_CASE_DEV_DUAL_MIC_ENDFIRE); /* DUALMIC EF TX */
                    } else if (fluence_mode == FLUENCE_MODE_BROADSIDE) {
                        return strdup(SND_USE_CASE_DEV_DUAL_MIC_BROADSIDE); /* DUALMIC BS TX */
                    }
                } else if (mDevSettingsFlag & QMIC_FLAG){
                    return strdup(SND_USE_CASE_DEV_QUAD_MIC);
                } else {
                    return strdup(SND_USE_CASE_DEV_LINE); /* BUILTIN-MIC TX */
                }
            }
        } else if (devices & AudioSystem::DEVICE_IN_AUX_DIGITAL) {
            return strdup(SND_USE_CASE_DEV_HDMI_TX); /* HDMI TX */
        } else if ((devices & AudioSystem::DEVICE_IN_WIRED_HEADSET) ||
                   (devices & AudioSystem::DEVICE_IN_ANC_HEADSET)) {
            return strdup(SND_USE_CASE_DEV_HEADSET); /* HEADSET TX */
        } else if (devices & AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
             if (btsco_samplerate == BTSCO_RATE_16KHZ)
                 return strdup(SND_USE_CASE_DEV_BTSCO_WB_TX); /* BTSCO TX*/
             else
                 return strdup(SND_USE_CASE_DEV_BTSCO_NB_TX); /* BTSCO TX*/
        } else if (devices & AudioSystem::DEVICE_IN_DEFAULT) {
            if (!strncmp(mic_type, "analog", 6)) {
                return strdup(SND_USE_CASE_DEV_HANDSET); /* HANDSET TX */
            } else {
                if (mDevSettingsFlag & DMIC_FLAG) {
                    if (fluence_mode == FLUENCE_MODE_ENDFIRE) {
                        return strdup(SND_USE_CASE_DEV_SPEAKER_DUAL_MIC_ENDFIRE); /* DUALMIC EF TX */
                    } else if (fluence_mode == FLUENCE_MODE_BROADSIDE) {
                        return strdup(SND_USE_CASE_DEV_SPEAKER_DUAL_MIC_BROADSIDE); /* DUALMIC BS TX */
                    }
                } else if (mDevSettingsFlag & QMIC_FLAG){
                    return strdup(SND_USE_CASE_DEV_QUAD_MIC);
                } else {
                    return strdup(SND_USE_CASE_DEV_LINE); /* BUILTIN-MIC TX */
                }
            }
        } else if ((devices & AudioSystem::DEVICE_IN_COMMUNICATION) ||
                   (devices & AudioSystem::DEVICE_IN_FM_RX) ||
                   (devices & AudioSystem::DEVICE_IN_FM_RX_A2DP) ||
                   (devices & AudioSystem::DEVICE_IN_VOICE_CALL)) {
            /* Nothing to be done, use current active device */
            return strdup(curTxUCMDevice);
        } else if ((devices & AudioSystem::DEVICE_IN_COMMUNICATION) ||
                   (devices & AudioSystem::DEVICE_IN_AMBIENT) ||
                   (devices & AudioSystem::DEVICE_IN_BACK_MIC) ||
                   (devices & AudioSystem::DEVICE_IN_AUX_DIGITAL)) {
            LOGI("No proper mapping found with UCM device list, setting default");
            if (!strncmp(mic_type, "analog", 6)) {
                return strdup(SND_USE_CASE_DEV_HANDSET); /* HANDSET TX */
            } else {
                return strdup(SND_USE_CASE_DEV_LINE); /* BUILTIN-MIC TX */
            }
        } else {
            LOGD("No valid input device: %u", devices);
        }
    }
    return NULL;
}

void ALSADevice::setVoiceVolume(int vol)
{
    LOGD("setVoiceVolume: volume %d", vol);
    setMixerControl("Voice Rx Volume", vol, 0);
}

void ALSADevice::setVoipVolume(int vol)
{
    LOGD("setVoipVolume: volume %d", vol);
    setMixerControl("Voip Rx Volume", vol, 0);
}
void ALSADevice::setMicMute(int state)
{
    LOGD("setMicMute: state %d", state);
    setMixerControl("Voice Tx Mute", state, 0);
}

void ALSADevice::setVoipMicMute(int state)
{
    LOGD("setVoipMicMute: state %d", state);
    setMixerControl("Voip Tx Mute", state, 0);
}

void ALSADevice::setBtscoRate(int rate)
{
    btsco_samplerate = rate;
}

void ALSADevice::enableWideVoice(bool flag)
{
    LOGD("enableWideVoice: flag %d", flag);
    if(flag == true) {
        setMixerControl("Widevoice Enable", 1, 0);
    } else {
        setMixerControl("Widevoice Enable", 0, 0);
    }
}

void ALSADevice::enableFENS(bool flag)
{
    LOGD("enableFENS: flag %d", flag);
    if(flag == true) {
        setMixerControl("FENS Enable", 1, 0);
    } else {
        setMixerControl("FENS Enable", 0, 0);
    }
}

void ALSADevice::setFlags(uint32_t flags)
{
    LOGV("setFlags: flags %d", flags);
    mDevSettingsFlag = flags;
}

status_t ALSADevice::getMixerControl(const char *name, unsigned int &value, int index)
{
    struct mixer_ctl *ctl;

    if (!mMixer) {
        LOGE("Control not initialized");
        return NO_INIT;
    }

    ctl =  mixer_get_control(mMixer, name, index);
    if (!ctl)
        return BAD_VALUE;

    mixer_ctl_get(ctl, &value);
    return NO_ERROR;
}

status_t ALSADevice::setMixerControl(const char *name, unsigned int value, int index)
{
    struct mixer_ctl *ctl;
    int ret = 0;
    LOGD("set:: name %s value %d index %d", name, value, index);
    if (!mMixer) {
        LOGE("Control not initialized");
        return NO_INIT;
    }

    // ToDo: Do we need to send index here? Right now it works with 0
    ctl = mixer_get_control(mMixer, name, 0);
    if(ctl == NULL) {
        LOGE("Could not get the mixer control");
        return BAD_VALUE;
    }
    ret = mixer_ctl_set(ctl, value);
    return (ret < 0) ? BAD_VALUE : NO_ERROR;
}

status_t ALSADevice::setMixerControl(const char *name, const char *value)
{
    struct mixer_ctl *ctl;
    int ret = 0;
    LOGD("set:: name %s value %s", name, value);

    if (!mMixer) {
        LOGE("Control not initialized");
        return NO_INIT;
    }

    ctl = mixer_get_control(mMixer, name, 0);
    if(ctl == NULL) {
        LOGE("Could not get the mixer control");
        return BAD_VALUE;
    }
    ret = mixer_ctl_select(ctl, value);
    return (ret < 0) ? BAD_VALUE : NO_ERROR;
}

int32_t ALSADevice::get_linearpcm_channel_status(uint32_t sampleRate,
                                                 unsigned char *channel_status)
{
    int32_t status = 0;
    unsigned char bit_index;
    memset(channel_status,0,24);
    bit_index = 0;
    /* block start bit in preamble bit 3 */
    set_bits(channel_status, 1, 1, &bit_index);

    //linear pcm
    bit_index = 1;
    set_bits(channel_status, 1, 0, &bit_index);

    bit_index = 24;
    switch (sampleRate) {
        case 8000:
           set_bits(channel_status, 4, 0x09, &bit_index);
           break;
        case 11025:
           set_bits(channel_status, 4, 0x0A, &bit_index);
           break;
        case 12000:
           set_bits(channel_status, 4, 0x0B, &bit_index);
           break;
        case 16000:
           set_bits(channel_status, 4, 0x0E, &bit_index);
           break;
        case 22050:
           set_bits(channel_status, 4, 0x02, &bit_index);
           break;
        case 24000:
           set_bits(channel_status, 4, 0x06, &bit_index);
           break;
        case 32000: // 1100 in 24..27
           set_bits(channel_status, 4, 0x0C, &bit_index);
           break;
        case 44100: // 0000 in 24..27
           break;
        case 48000: // 0100 in 24..27
            set_bits(channel_status, 4, 0x04, &bit_index);
            break;
        case 88200: // 0001 in 24..27
           set_bits(channel_status, 4, 0x01, &bit_index);
           break;
        case 96000: // 0101 in 24..27
            set_bits(channel_status, 4, 0x05, &bit_index);
            break;
        case 176400: // 0011 in 24..27
            set_bits(channel_status, 4, 0x03, &bit_index);
            break;
        case 192000: // 0111 in 24..27
            set_bits(channel_status, 4, 0x07, &bit_index);
            break;
        default:
            LOGV("Invalid sample_rate %u\n", sampleRate);
            status = -1;
            break;
    }
    return status;
}

int32_t ALSADevice::get_compressed_channel_status(void *audio_stream_data,
                                                  uint32_t audio_frame_size,
                                                  unsigned char *channel_status,
                                                  enum audio_parser_code_type codec_type)
                                                  // codec_type - AUDIO_PARSER_CODEC_AC3
                                                  //            - AUDIO_PARSER_CODEC_DTS
{
    unsigned char *streamPtr;
    streamPtr = (unsigned char *)audio_stream_data;

    if(init_audio_parser(streamPtr, audio_frame_size, codec_type) == -1)
    {
        LOGE("init audio parser failed");
        return -1;
    }
    get_channel_status(channel_status, codec_type);
    return 0;
}

status_t ALSADevice::setPcmVolume(int value)
{
    status_t err = NO_ERROR;

    err = setMixerControl("HIFI2 RX Volume",value,0);
    if(err) {
        LOGE("setPcmVolume - HIFI2 error = %d",err);
    }

    return err;
}

status_t ALSADevice::setCompressedVolume(int value)
{
    status_t err = NO_ERROR;

    err = setMixerControl("COMPRESSED RX Volume",value,0);
    if(err) {
        LOGE("setCompressedVolume = error = %d",err);
    }

    return err;
}

status_t ALSADevice::setPlaybackFormat(const char *value, int device)
{
    status_t err = NO_ERROR;
    if (device == AudioSystem::DEVICE_OUT_SPDIF)
        err = setMixerControl("SEC RX Format",value);
    else if(device == AudioSystem::DEVICE_OUT_AUX_DIGITAL)
        err = setMixerControl("HDMI RX Format",value);
    if(err) {
        LOGE("setPlaybackFormat error = %d",err);
    }

    return err;
}

status_t ALSADevice::setCaptureFormat(const char *value)
{
    status_t err = NO_ERROR;

    err = setMixerControl("MI2S TX Format",value);

    if(err) {
        LOGE("setPlaybackFormat error = %d",err);
    }

    return err;
}

status_t ALSADevice::setWMAParams(alsa_handle_t *handle, int params[], int size)
{
    status_t err = NO_ERROR;
    if (size > sizeof(mWMA_params)/sizeof(mWMA_params[0])) {
        LOGE("setWMAParams too many params error");
        return BAD_VALUE;
    }
    for (int i = 0; i < size; i++)
        mWMA_params[i] = params[i];
    return err;
}

int ALSADevice::getALSABufferSize(alsa_handle_t *handle) {

    int format = 2;

    switch (handle->format) {
        case SNDRV_PCM_FORMAT_S8:
            format = 1;
        break;
        case SNDRV_PCM_FORMAT_S16_LE:
            format = 2;
        break;
        case SNDRV_PCM_FORMAT_S24_LE:
            format = 3;
        break;
        default:
           format = 2;
        break;
    }
    LOGD("getALSABufferSize - handle->channels = %d,  handle->sampleRate = %d,\
            format = %d",handle->channels, handle->sampleRate,format);

    LOGD("buff size is %d",((PCM_BUFFER_DURATION *  handle->channels\
            *  handle->sampleRate * format) / 1000000));
    int bufferSize = ((PCM_BUFFER_DURATION *  handle->channels
            *  handle->sampleRate * format) / 1000000);


    //Check for power of 2
    if (bufferSize & (bufferSize-1)) {

        bufferSize -= 1;
        for (uint32_t i=1; i<sizeof(bufferSize -1)*CHAR_BIT; i<<=1)
                bufferSize = bufferSize | bufferSize >> i;
        bufferSize += 1;
        LOGV("Not power of 2 - buff size is = %d",bufferSize);
    }
    else {
        LOGV("power of 2");
    }
    if(bufferSize < MULTI_CHANNEL_MIN_PERIOD_SIZE)
        bufferSize = MULTI_CHANNEL_MIN_PERIOD_SIZE;
    if(bufferSize >  MULTI_CHANNEL_MAX_PERIOD_SIZE)
        bufferSize = MULTI_CHANNEL_MAX_PERIOD_SIZE;

    return bufferSize;
}

status_t ALSADevice::setHDMIChannelCount()
{
    status_t err = NO_ERROR;

    err = setMixerControl("HDMI_RX Channels","Two");
    if(err) {
        LOGE("setHDMIChannelCount error = %d",err);
    }
    return err;
}

void ALSADevice::getDevices(uint32_t devices, uint32_t mode, char **rxDevice, char **txDevice)
{
    LOGV("%s: device %d", __FUNCTION__, devices);

    if ((mode == AudioSystem::MODE_IN_CALL)  || (mode == AudioSystem::MODE_IN_COMMUNICATION)) {
        if ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
            (devices & AudioSystem::DEVICE_IN_WIRED_HEADSET)) {
            devices = devices | (AudioSystem::DEVICE_OUT_WIRED_HEADSET |
                      AudioSystem::DEVICE_IN_WIRED_HEADSET);
        } else if (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE) {
            devices = devices | (AudioSystem::DEVICE_OUT_WIRED_HEADPHONE |
                      AudioSystem::DEVICE_IN_BUILTIN_MIC);
        } else if ((devices & AudioSystem::DEVICE_OUT_EARPIECE) ||
                  (devices & AudioSystem::DEVICE_IN_BUILTIN_MIC)) {
            devices = devices | (AudioSystem::DEVICE_IN_BUILTIN_MIC |
                      AudioSystem::DEVICE_OUT_EARPIECE);
        } else if (devices & AudioSystem::DEVICE_OUT_SPEAKER) {
            devices = devices | (AudioSystem::DEVICE_IN_DEFAULT |
                       AudioSystem::DEVICE_OUT_SPEAKER);
        } else if ((devices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO) ||
                   (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET) ||
                   (devices & AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET)) {
            devices = devices | (AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET |
                      AudioSystem::DEVICE_OUT_BLUETOOTH_SCO);
        } else if ((devices & AudioSystem::DEVICE_OUT_ANC_HEADSET) ||
                   (devices & AudioSystem::DEVICE_IN_ANC_HEADSET)) {
            devices = devices | (AudioSystem::DEVICE_OUT_ANC_HEADSET |
                      AudioSystem::DEVICE_IN_ANC_HEADSET);
        } else if (devices & AudioSystem::DEVICE_OUT_ANC_HEADPHONE) {
            devices = devices | (AudioSystem::DEVICE_OUT_ANC_HEADPHONE |
                      AudioSystem::DEVICE_IN_BUILTIN_MIC);
        } else if (devices & AudioSystem::DEVICE_OUT_AUX_DIGITAL) {
            devices = devices | (AudioSystem::DEVICE_OUT_AUX_DIGITAL |
                      AudioSystem::DEVICE_IN_AUX_DIGITAL);
        } else if (devices & AudioSystem::DEVICE_OUT_PROXY) {
            devices = devices | (AudioSystem::DEVICE_OUT_PROXY |
                      AudioSystem::DEVICE_IN_BUILTIN_MIC);
        }
    }

    *rxDevice = getUCMDevice(devices & AudioSystem::DEVICE_OUT_ALL, 0);
    *txDevice = getUCMDevice(devices & AudioSystem::DEVICE_IN_ALL, 1);

    return;
}

void ALSADevice::setUseCase(alsa_handle_t *handle, bool bIsUseCaseSet)
{
    char *rxDevice = NULL, *txDevice = NULL;
    getDevices(handle->devices, handle->mode, &rxDevice, &txDevice);

    if(rxDevice != NULL) {
        if(bIsUseCaseSet)
            snd_use_case_set_case(handle->ucMgr, "_verb", handle->useCase, rxDevice);
        else
            snd_use_case_set_case(handle->ucMgr, "_enamod", handle->useCase, rxDevice);
        if(rxDevice) {
            free(rxDevice);
            rxDevice = NULL;
        }
    }
    if(txDevice != NULL) {
        if(bIsUseCaseSet)
            snd_use_case_set_case(handle->ucMgr, "_verb", handle->useCase, txDevice);
        else
            snd_use_case_set_case(handle->ucMgr, "_enamod", handle->useCase, txDevice);
        if(txDevice) {
           free(txDevice);
           txDevice = NULL;
        }
    }
}

status_t ALSADevice::exitReadFromProxy()
{
    LOGV("exitReadFromProxy");
    mProxyParams.mExitRead = true;
    if(mProxyParams.mPfdProxy[1].fd != -1) {
        uint64_t writeValue = KILL_A2DP_THREAD;
        LOGD("Writing to mPfdProxy[1].fd %d",mProxyParams.mPfdProxy[1].fd);
        write(mProxyParams.mPfdProxy[1].fd, &writeValue, sizeof(uint64_t));
    }
    return NO_ERROR;
}

void ALSADevice::resetProxyVariables() {

    mProxyParams.mAvail = 0;
    mProxyParams.mFrames = 0;
    mProxyParams.mX.frames = 0;
    if(mProxyParams.mPfdProxy[1].fd != -1) {
        sys_close::lib_close(mProxyParams.mPfdProxy[1].fd);
        mProxyParams.mPfdProxy[1].fd = -1;
    }
}

ssize_t  ALSADevice::readFromProxy(void **captureBuffer , ssize_t *bufferSize) {

    status_t err = NO_ERROR;
    int err_poll = 0;
    initProxyParams();
    err = startProxy();
    if(err) {
        LOGE("ReadFromProxy-startProxy returned err = %d", err);
        *captureBuffer = NULL;
        *bufferSize = 0;
        return err;
    }
    struct pcm * capture_handle = (struct pcm *)mProxyParams.mProxyPcmHandle;

    while(!mProxyParams.mExitRead) {
        LOGV("Calling sync_ptr(proxy");
        err = sync_ptr(capture_handle);
        if(err == EPIPE) {
               LOGE("Failed in sync_ptr \n");
               /* we failed to make our window -- try to restart */
               capture_handle->underruns++;
               capture_handle->running = 0;
               capture_handle->start = 0;
               continue;
        }else if(err != NO_ERROR){
                LOGE("Error: Sync ptr returned %d", err);
                break;
        }

        mProxyParams.mAvail = pcm_avail(capture_handle);
        LOGV("avail is = %d frames = %ld, avai_min = %d\n",\
                      mProxyParams.mAvail,  mProxyParams.mFrames,(int)capture_handle->sw_p->avail_min);
        if (mProxyParams.mAvail < capture_handle->sw_p->avail_min) {
            err_poll = poll(mProxyParams.mPfdProxy, NUM_FDS, TIMEOUT_INFINITE);
            if (mProxyParams.mPfdProxy[1].revents & POLLIN) {
                LOGV("Event on userspace fd");
            }
            if ((mProxyParams.mPfdProxy[1].revents & POLLERR) ||
                    (mProxyParams.mPfdProxy[1].revents & POLLNVAL)) {
                LOGV("POLLERR or INVALID POLL");
                err = BAD_VALUE;
                break;
            }
            if((mProxyParams.mPfdProxy[0].revents & POLLERR) ||
                    (mProxyParams.mPfdProxy[0].revents & POLLNVAL)) {
                LOGV("POLLERR or INVALID POLL on zero");
                err = BAD_VALUE;
                break;
            }
            if (mProxyParams.mPfdProxy[0].revents & POLLIN) {
                LOGV("POLLIN on zero");
            }
            LOGV("err_poll = %d",err_poll);
            continue;
        }
        break;
    }
    if(err != NO_ERROR) {
        LOGE("Reading from proxy failed = err = %d", err);
        *captureBuffer = NULL;
        *bufferSize = 0;
        return err;
    }
    if (mProxyParams.mX.frames > mProxyParams.mAvail)
        mProxyParams.mFrames = mProxyParams.mAvail;
    void *data  = dst_address(capture_handle);
    //TODO: Return a pointer to AudioHardware
    if(mProxyParams.mCaptureBuffer == NULL)
        mProxyParams.mCaptureBuffer =  malloc(mProxyParams.mCaptureBufferSize);
    memcpy(mProxyParams.mCaptureBuffer, (char *)data,
             mProxyParams.mCaptureBufferSize);
    mProxyParams.mX.frames -= mProxyParams.mFrames;
    capture_handle->sync_ptr->c.control.appl_ptr += mProxyParams.mFrames;
    capture_handle->sync_ptr->flags = 0;
    LOGV("Calling sync_ptr for proxy after sync");
    err = sync_ptr(capture_handle);
    if(err == EPIPE) {
        LOGV("Failed in sync_ptr \n");
        capture_handle->running = 0;
        err = sync_ptr(capture_handle);
    }
    if(err != NO_ERROR ) {
        LOGE("Error: Sync ptr end returned %d", err);
        *captureBuffer = NULL;
        *bufferSize = 0;
        return err;
    }
    *captureBuffer = mProxyParams.mCaptureBuffer;
    *bufferSize = mProxyParams.mCaptureBufferSize;
    return err;
}

void ALSADevice::initProxyParams() {
    if(mProxyParams.mPfdProxy[1].fd == -1) {
        LOGV("Allocating A2Dp poll fd");
        mProxyParams.mPfdProxy[0].fd = mProxyParams.mProxyPcmHandle->fd;
        mProxyParams.mPfdProxy[0].events = (POLLIN | POLLERR | POLLNVAL);
        LOGV("Allocated A2DP poll fd");
        mProxyParams.mPfdProxy[1].fd = eventfd(0,0);
        mProxyParams.mPfdProxy[1].events = (POLLIN | POLLERR | POLLNVAL);
        mProxyParams.mFrames = (mProxyParams.mProxyPcmHandle->flags & PCM_MONO) ?
            (mProxyParams.mProxyPcmHandle->period_size / 2) :
            (mProxyParams.mProxyPcmHandle->period_size / 4);
        mProxyParams.mX.frames = (mProxyParams.mProxyPcmHandle->flags & PCM_MONO) ?
            (mProxyParams.mProxyPcmHandle->period_size / 2) :
            (mProxyParams.mProxyPcmHandle->period_size / 4);
    }
}

status_t ALSADevice::startProxy() {

    status_t err = NO_ERROR;
    struct pcm * capture_handle = (struct pcm *)mProxyParams.mProxyPcmHandle;
    while(1) {
        if (!capture_handle->start) {
            if(ioctl(capture_handle->fd, SNDRV_PCM_IOCTL_START)) {
                err = -errno;
                if (errno == EPIPE) {
                   LOGV("Failed in SNDRV_PCM_IOCTL_START\n");
                   /* we failed to make our window -- try to restart */
                   capture_handle->underruns++;
                   capture_handle->running = 0;
                   capture_handle->start = 0;
                   continue;
                } else {
                   LOGE("IGNORE - IOCTL_START failed for proxy err: %d \n", errno);
                   err = NO_ERROR;
                   break;
                }
           } else {
               LOGD(" Proxy Driver started(IOCTL_START Success)\n");
               break;
           }
       }
       else {
           LOGV("Proxy Already started break out of condition");
           break;
       }
   }
   LOGV("startProxy - Proxy started");
   capture_handle->start = 1;
   capture_handle->sync_ptr->flags = SNDRV_PCM_SYNC_PTR_APPL |
               SNDRV_PCM_SYNC_PTR_AVAIL_MIN;
   return err;
}

status_t ALSADevice::openProxyDevice()
{
    struct snd_pcm_hw_params *params = NULL;
    struct snd_pcm_sw_params *sparams = NULL;
    int flags = (DEBUG_ON | PCM_MMAP| PCM_STEREO | PCM_IN);

    LOGV("openProxyDevice");
    mProxyParams.mProxyPcmHandle = pcm_open(flags, PROXY_CAPTURE_DEVICE_NAME);
    if (!pcm_ready(mProxyParams.mProxyPcmHandle)) {
        LOGE("Opening proxy device failed");
        goto bail;
    }
    LOGV("Proxy device opened successfully: mProxyPcmHandle %p", mProxyParams.mProxyPcmHandle);
    mProxyParams.mProxyPcmHandle->channels = AFE_PROXY_CHANNEL_COUNT;
    mProxyParams.mProxyPcmHandle->rate     = AFE_PROXY_SAMPLE_RATE;
    mProxyParams.mProxyPcmHandle->flags    = flags;
    mProxyParams.mProxyPcmHandle->period_size = AFE_PROXY_PERIOD_SIZE;

    params = (struct snd_pcm_hw_params*) calloc(1,sizeof(struct snd_pcm_hw_params));
    if (!params) {
         goto bail;
    }

    param_init(params);

    param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS,
            (mProxyParams.mProxyPcmHandle->flags & PCM_MMAP)?
            SNDRV_PCM_ACCESS_MMAP_INTERLEAVED
            : SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
            SNDRV_PCM_FORMAT_S16_LE);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT,
            SNDRV_PCM_SUBFORMAT_STD);
    param_set_min(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
            mProxyParams.mProxyPcmHandle->period_size);
    param_set_int(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_FRAME_BITS,
            mProxyParams.mProxyPcmHandle->channels - 1 ? 32 : 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_CHANNELS,
            mProxyParams.mProxyPcmHandle->channels);
    param_set_int(params, SNDRV_PCM_HW_PARAM_RATE,
            mProxyParams.mProxyPcmHandle->rate);

    param_set_hw_refine(mProxyParams.mProxyPcmHandle, params);

    if (param_set_hw_params(mProxyParams.mProxyPcmHandle, params)) {
        LOGE("Failed to set hardware params on Proxy device");
        goto bail;
    }

    mProxyParams.mProxyPcmHandle->buffer_size = pcm_buffer_size(params);
    mProxyParams.mProxyPcmHandle->period_size = pcm_period_size(params);
    mProxyParams.mProxyPcmHandle->period_cnt  =
            mProxyParams.mProxyPcmHandle->buffer_size /
            mProxyParams.mProxyPcmHandle->period_size;
    LOGV("Capture - period_size (%d)",\
            mProxyParams.mProxyPcmHandle->period_size);
    LOGV("Capture - buffer_size (%d)",\
            mProxyParams.mProxyPcmHandle->buffer_size);
    LOGV("Capture - period_cnt  (%d)\n",\
            mProxyParams.mProxyPcmHandle->period_cnt);
    sparams = (struct snd_pcm_sw_params*) calloc(1,sizeof(struct snd_pcm_sw_params));
    if (!sparams) {
        LOGE("Failed to allocated software params for Proxy device");
        goto bail;
    }

   sparams->tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
   sparams->period_step = 1;
   sparams->avail_min = (mProxyParams.mProxyPcmHandle->flags & PCM_MONO) ?
           mProxyParams.mProxyPcmHandle->period_size/2
           : mProxyParams.mProxyPcmHandle->period_size/4;
   sparams->start_threshold = 1;
   sparams->stop_threshold = mProxyParams.mProxyPcmHandle->buffer_size;
   sparams->xfer_align = (mProxyParams.mProxyPcmHandle->flags & PCM_MONO) ?
           mProxyParams.mProxyPcmHandle->period_size/2
           : mProxyParams.mProxyPcmHandle->period_size/4; /* needed for old kernels */
   sparams->silence_size = 0;
   sparams->silence_threshold = 0;

   if (param_set_sw_params(mProxyParams.mProxyPcmHandle, sparams)) {
        LOGE("Failed to set software params on Proxy device");
        goto bail;
   }
   mmap_buffer(mProxyParams.mProxyPcmHandle);

   if (pcm_prepare(mProxyParams.mProxyPcmHandle)) {
       LOGE("Failed to pcm_prepare on Proxy device");
       goto bail;
   }
   mProxyParams.mProxyState = proxy_params::EProxyOpened;
   return NO_ERROR;

bail:
   if(mProxyParams.mProxyPcmHandle)  {
       pcm_close(mProxyParams.mProxyPcmHandle);
       mProxyParams.mProxyPcmHandle = NULL;
   }
   mProxyParams.mProxyState = proxy_params::EProxyClosed;
   return NO_INIT;
}

status_t ALSADevice::closeProxyDevice() {
    status_t err = NO_ERROR;
    if(mProxyParams.mProxyPcmHandle) {
        pcm_close(mProxyParams.mProxyPcmHandle);
        mProxyParams.mProxyPcmHandle = NULL;
    }
    resetProxyVariables();
    mProxyParams.mProxyState = proxy_params::EProxyClosed;
    mProxyParams.mExitRead = false;
    return err;
}

bool ALSADevice::isProxyDeviceOpened() {

   //TODO : Add some intelligence to return appropriate value
   if(mProxyParams.mProxyState == proxy_params::EProxyOpened ||
           mProxyParams.mProxyState == proxy_params::EProxyCapture ||
           mProxyParams.mProxyState == proxy_params::EProxySuspended)
       return true;
   return false;
}

bool ALSADevice::isProxyDeviceSuspended() {

   if(mProxyParams.mProxyState == proxy_params::EProxySuspended)
        return true;
   return false;
}

bool ALSADevice::suspendProxy() {

   status_t err = NO_ERROR;
   if(mProxyParams.mProxyState == proxy_params::EProxyOpened ||
           mProxyParams.mProxyState == proxy_params::EProxyCapture) {
       mProxyParams.mProxyState = proxy_params::EProxySuspended;
   }
   else {
       LOGE("Proxy already suspend or closed, in state = %d",\
                mProxyParams.mProxyState);
   }
   return err;
}

bool ALSADevice::resumeProxy() {

   status_t err = NO_ERROR;
   struct pcm *capture_handle= mProxyParams.mProxyPcmHandle;
   LOGD("resumeProxy mProxyParams.mProxyState = %d, capture_handle =%p",\
           mProxyParams.mProxyState, capture_handle);
   if((mProxyParams.mProxyState == proxy_params::EProxyOpened ||
           mProxyParams.mProxyState == proxy_params::EProxySuspended) &&
           capture_handle != NULL) {
       LOGV("pcm_prepare from Resume");
       capture_handle->start = 0;
       err = pcm_prepare(capture_handle);
       if(err != OK) {
           LOGE("IGNORE: PCM Prepare - capture failed err = %d", err);
       }
       err = startProxy();
       if(err) {
           LOGE("IGNORE:startProxy returned error = %d", err);
       }
       mProxyParams.mProxyState = proxy_params::EProxyCapture;
       err = sync_ptr(capture_handle);
       if (err) {
           LOGE("IGNORE: sync ptr from resumeProxy returned error = %d", err);
       }
       LOGV("appl_ptr= %d", (int)capture_handle->sync_ptr->c.control.appl_ptr);
   }
   else {
        LOGE("resume Proxy ignored in invalid state - ignore");
        if(mProxyParams.mProxyState == proxy_params::EProxyClosed ||
                capture_handle == NULL) {
            LOGE("resumeProxy = BAD_VALUE");
            err = BAD_VALUE;
            return err;
        }
   }
   return NO_ERROR;
}

void ALSADevice::setUseCase(alsa_handle_t *handle, bool bIsUseCaseSet, char *device)
{
    if(bIsUseCaseSet)
        snd_use_case_set_case(handle->ucMgr, "_verb", handle->useCase, device);
    else
        snd_use_case_set_case(handle->ucMgr, "_enamod", handle->useCase, device);
}

status_t ALSADevice::openCapture(alsa_handle_t *handle,
                                 bool isMmapMode,
                                 bool isCompressed)
{
    char *devName = NULL;
    unsigned flags = PCM_IN | DEBUG_ON;
    int err = NO_ERROR;

    close(handle);

    LOGD("openCapture: handle %p", handle);

    if (handle->channels == 1)
        flags |= PCM_MONO;
    else if (handle->channels == 4)
        flags |= PCM_QUAD;
    else if (handle->channels == 6)
        flags |= PCM_5POINT1;
    else
        flags |= PCM_STEREO;

    if(isMmapMode)
        flags |= PCM_MMAP;
    else
        flags |= PCM_NMMAP;

    if (deviceName(handle, flags, &devName) < 0) {
        LOGE("Failed to get pcm device node: %s", devName);
        return NO_INIT;
    }
    if(devName != NULL)
        handle->handle = pcm_open(flags, (char*)devName);
    LOGE("s_open: opening ALSA device '%s'", devName);

    if(devName != NULL)
        free(devName);

    if (!handle->handle) {
        LOGE("s_open: Failed to initialize ALSA device '%s'", devName);
        return NO_INIT;
    }

    handle->handle->flags = flags;

    LOGD("setting hardware parameters");
    err = setCaptureHardwareParams(handle, isCompressed);
    if (err == NO_ERROR) {
        LOGD("setting software parameters");
        err = setCaptureSoftwareParams(handle, isCompressed);
    }
    if(err != NO_ERROR) {
        LOGE("Set HW/SW params failed: Closing the pcm stream");
        standby(handle);
        return err;
    }

    if(mmap_buffer(handle->handle)) {
        LOGE("Failed to mmap the buffer");
        standby(handle);
        return NO_INIT;
    }

    if (pcm_prepare(handle->handle)) {
        LOGE("Failed to pcm_prepare on caoture stereo device");
        standby(handle);
        return NO_INIT;
    }

    return NO_ERROR;
}


status_t ALSADevice::setCaptureHardwareParams(alsa_handle_t *handle, bool isCompressed)
{
    struct snd_pcm_hw_params *params;
    struct snd_compr_caps compr_cap;
    struct snd_compr_params compr_params;

    int32_t minPeroid, maxPeroid;
    unsigned long bufferSize, reqBuffSize;
    unsigned int periodTime, bufferTime;

    int status = 0;
    status_t err = NO_ERROR;
    unsigned int requestedRate = handle->sampleRate;
    int format = handle->format;

    reqBuffSize = handle->bufferSize;
    if (isCompressed) {
        if (ioctl(handle->handle->fd, SNDRV_COMPRESS_GET_CAPS, &compr_cap)) {
            LOGE("SNDRV_COMPRESS_GET_CAPS, failed Error no %d \n", errno);
            err = -errno;
            return err;
        }

        minPeroid = compr_cap.min_fragment_size;
        maxPeroid = compr_cap.max_fragment_size;
        handle->channels = 2;
//NOTE:
// channels = 2 would set 1 MI2S line, greater than 2 will set more than
// 1 MI2S lines
        LOGV("Min peroid size = %d , Maximum Peroid size = %d",\
            minPeroid, maxPeroid);
        if (ioctl(handle->handle->fd, SNDRV_COMPRESS_SET_PARAMS,
                  &compr_params)) {
            LOGE("SNDRV_COMPRESS_SET_PARAMS,failed Error no %d \n", errno);
            err = -errno;
            return err;
        }
    }

    params = (snd_pcm_hw_params*) calloc(1, sizeof(struct snd_pcm_hw_params));
    if (!params) {
        LOGE("Failed to allocate ALSA hardware parameters!");
        return NO_INIT;
    }

    LOGD("setHardwareParamsCapture: reqBuffSize %d channels %d sampleRate %d",
         (int) reqBuffSize, handle->channels, handle->sampleRate);

    param_init(params);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS,
                   (handle->handle->flags & PCM_MMAP) ?
                       SNDRV_PCM_ACCESS_MMAP_INTERLEAVED :
                       SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
                   SNDRV_PCM_FORMAT_S16_LE);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT,
                   SNDRV_PCM_SUBFORMAT_STD);
    param_set_int(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
                  handle->bufferSize);
    param_set_int(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_FRAME_BITS,
                  handle->channels* 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_CHANNELS,
                  handle->channels);
    param_set_int(params, SNDRV_PCM_HW_PARAM_RATE,
                  handle->sampleRate);
    param_set_hw_refine(handle->handle, params);
    if (param_set_hw_params(handle->handle, params)) {
        LOGE("Failed to set hardware params on stereo capture device");
        return NO_INIT;
    }
    handle->handle->buffer_size = pcm_buffer_size(params);
    handle->handle->period_size = pcm_period_size(params);
    handle->handle->period_cnt  = handle->handle->buffer_size /
                                     handle->handle->period_size;
    LOGV("period_size %d, period_cnt %d", handle->handle->period_size,
              handle->handle->period_cnt);
    handle->handle->rate = handle->sampleRate;
    handle->handle->channels = handle->channels;
    handle->periodSize = handle->handle->period_size;
    handle->bufferSize = handle->handle->period_size;

    return NO_ERROR;
}

status_t ALSADevice::setCaptureSoftwareParams(alsa_handle_t *handle,
                                              bool isCompressed)
{
    struct snd_pcm_sw_params* params;
    struct pcm* pcm = handle->handle;

    unsigned long periodSize = pcm->period_size;
    unsigned flags = pcm->flags;

    params = (snd_pcm_sw_params*) calloc(1, sizeof(struct snd_pcm_sw_params));
    if (!params) {
        LOGE("Failed to allocate ALSA software parameters!");
        return NO_INIT;
    }

    if(isCompressed)
	params->tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
//NOTE: move this to TIME STAMP mode for compressed
    else
        params->tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    params->period_step = 1;
    if (flags & PCM_MONO) {
        params->avail_min = pcm->period_size/2;
        params->xfer_align = pcm->period_size/2;
    } else if (flags & PCM_QUAD) {
        params->avail_min = pcm->period_size/8;
        params->xfer_align = pcm->period_size/8;
    } else if (flags & PCM_5POINT1) {
        params->avail_min = pcm->period_size/12;
        params->xfer_align = pcm->period_size/12;
    } else {
        params->avail_min = pcm->period_size/4;
        params->xfer_align = pcm->period_size/4;
    }

    params->start_threshold = 1;
    params->stop_threshold = INT_MAX;
    params->silence_threshold = 0;
    params->silence_size = 0;

    if (param_set_sw_params(handle->handle, params)) {
        LOGE("cannot set sw params");
        return NO_INIT;
    }
    return NO_ERROR;
}

}
