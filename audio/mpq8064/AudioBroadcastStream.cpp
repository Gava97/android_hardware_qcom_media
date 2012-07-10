/* AudioBroadcastStreamALSA.cpp
 **
 ** Copyright 2008-2009 Wind River Systems
 ** Copyright (c) 2011, Code Aurora Forum. All rights reserved.
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

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>

#define LOG_TAG "AudioBroadcastStreamALSA"
#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>

#include <linux/ioctl.h>

#include "AudioHardwareALSA.h"
#include <sys/prctl.h>
#include <sys/resource.h>
#include <pthread.h>
#include <linux/unistd.h>

#define COMPRE_CAPTURE_HEADER_SIZE (sizeof(struct snd_compr_audio_info))

namespace sys_broadcast {
    ssize_t lib_write(int fd, const void *buf, size_t count) {
        return write(fd, buf, count);
    }
    ssize_t lib_close(int fd) {
        return close(fd);
    }
};

namespace android_audio_legacy
{

// ----------------------------------------------------------------------------

AudioBroadcastStreamALSA::AudioBroadcastStreamALSA(AudioHardwareALSA *parent,
                                                   uint32_t  devices,
                                                   int      format,
                                                   uint32_t channels,
                                                   uint32_t sampleRate,
                                                   uint32_t audioSource,
                                                   status_t *status)
{
    /* ------------------------------------------------------------------------
    Description: Initialize Flags and device handles
    -------------------------------------------------------------------------*/
    initialization();

    /* ------------------------------------------------------------------------
    Description: set config parameters from input arguments and error handling
    -------------------------------------------------------------------------*/
    mParent         = parent;
    mDevices        = devices;
    mInputFormat    = format;
    mSampleRate     = sampleRate;
    mChannels       = channels;
    mAudioSource    = audioSource;
    mALSADevice     = mParent->mALSADevice;
    mUcMgr          = mParent->mUcMgr;

    LOGD("devices:%d, format:%d, channels:%d, sampleRate:%d, audioSource:%d",
        devices, format, channels, sampleRate, audioSource);

    if(!(devices & AudioSystem::DEVICE_OUT_ALL) ||
       (mSampleRate == 0) || ((mChannels < 1) && (mChannels > 6)) ||
       ((audioSource != QCOM_AUDIO_SOURCE_DIGITAL_BROADCAST_MAIN_AD) &&
        (audioSource != QCOM_AUDIO_SOURCE_DIGITAL_BROADCAST_MAIN_ONLY) &&
        (audioSource != QCOM_AUDIO_SOURCE_ANALOG_BROADCAST) &&
        (audioSource != QCOM_AUDIO_SOURCE_HDMI_IN ))) {
        LOGE("invalid config");
        *status = BAD_VALUE;
        return;
    }
    if(audioSource == QCOM_AUDIO_SOURCE_HDMI_IN) {
        if((format != QCOM_BROADCAST_AUDIO_FORMAT_LPCM) &&
           (format != QCOM_BROADCAST_AUDIO_FORMAT_COMPRESSED) &&
           (format != QCOM_BROADCAST_AUDIO_FORMAT_COMPRESSED_HBR)) {
            LOGE("invalid config");
            *status = BAD_VALUE;
            return;
        }
    } else if(audioSource == QCOM_AUDIO_SOURCE_ANALOG_BROADCAST) {
        if((format != QCOM_BROADCAST_AUDIO_FORMAT_LPCM) &&
           (mChannels != 2)) {
            LOGE("invalid config");
            *status = BAD_VALUE;
            return;
        }
    } else {
        if(!(format & AudioSystem::MAIN_FORMAT_MASK)) {
            LOGE("invalid config");
            *status = BAD_VALUE;
            return;
        }
    }
    /* ------------------------------------------------------------------------
    Description: set the output device format
    -------------------------------------------------------------------------*/
    updateOutputFormat();

    /* ------------------------------------------------------------------------
    Description: Set appropriate flags based on the configuration parameters
    -------------------------------------------------------------------------*/
    *status = openCapturingAndRoutingDevices();

    if(*status != NO_ERROR) {
        LOGE("Could not open the capture and routing devices");
        *status = BAD_VALUE;
         return;
    }
    LOGV("mRouteAudioToA2dp = %d", mRouteAudioToA2dp);
    if (mRouteAudioToA2dp) {
        *status = mParent->startA2dpPlayback_l(
                                AudioHardwareALSA::A2DPBroadcast);
        LOGV("startA2dpPlayback for broadcast returned = %d", *status);
        if(*status != NO_ERROR)
            *status = BAD_VALUE;
    }

    return;
}

AudioBroadcastStreamALSA::~AudioBroadcastStreamALSA()
{
    LOGV("Destructor");
    Mutex::Autolock autoLock(mLock);

    if (mRouteAudioToA2dp) {
        status_t status = mParent->stopA2dpPlayback_l(
                                AudioHardwareALSA::A2DPBroadcast);
        LOGV("stopA2dpPlayback_l for broadcast returned %d", status);
        mRouteAudioToA2dp = false;
    }

    mSkipWrite = true;
    mWriteCv.signal();

    exitFromCaptureThread();

    if(mPcmTxHandle)
        closeDevice(mPcmTxHandle);

    if(mCompreTxHandle)
        closeDevice(mCompreTxHandle);

    exitFromPlaybackThread();

    if(mPcmRxHandle)
        closeDevice(mPcmRxHandle);

    if(mCompreRxHandle)
        closeDevice(mCompreRxHandle);

    if(mMS11Decoder)
        delete mMS11Decoder;

    if(mBitstreamSM)
        delete mBitstreamSM;

    initialization();

    for(ALSAHandleList::iterator it = mParent->mDeviceList.begin();
            it != mParent->mDeviceList.end(); ++it) {
        if((!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC_COMPRESSED,
                strlen(SND_USE_CASE_VERB_HIFI_REC_COMPRESSED))) ||
           (!strncmp(it->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC_COMPRESSED,
                strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC_COMPRESSED))) ||
           (!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC2,
                 strlen(SND_USE_CASE_VERB_HIFI_REC2))) ||
           (!strncmp(it->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC2,
                 strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC2))) ||
           (!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI3,
                 strlen(SND_USE_CASE_VERB_HIFI3))) ||
           (!strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_MUSIC3,
                 strlen(SND_USE_CASE_MOD_PLAY_MUSIC3))) ||
           (!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL2,
                 strlen(SND_USE_CASE_VERB_HIFI_TUNNEL2))) ||
           (!strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL2,
                 strlen(SND_USE_CASE_MOD_PLAY_TUNNEL2)))) {
            mParent->mDeviceList.erase(it);
        }
    }
}

status_t AudioBroadcastStreamALSA::setParameters(const String8& keyValuePairs)
{
    LOGV("setParameters");
    Mutex::Autolock autoLock(mLock);
    status_t status = NO_ERROR;
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key = String8(AudioParameter::keyRouting);
    int device;
    if (param.getInt(key, device) == NO_ERROR) {
        // Ignore routing if device is 0.
        LOGD("setParameters(): keyRouting with device %d", device);
        mDevices = device;
        if(device) {
            //ToDo: Call device setting UCM API here
//            doRouting(device);
        }
        param.remove(key);
    }

    return status;
}

String8 AudioBroadcastStreamALSA::getParameters(const String8& keys)
{
    LOGV("getParameters");
    Mutex::Autolock autoLock(mLock);
    AudioParameter param = AudioParameter(keys);
    String8 value;
    String8 key = String8(AudioParameter::keyRouting);

    if (param.get(key, value) == NO_ERROR) {
        param.addInt(key, (int)mDevices);
    }

    LOGV("getParameters() %s", param.toString().string());
    return param.toString();
}

status_t AudioBroadcastStreamALSA::start(int64_t absTimeToStart)
{
    LOGV("start");
    Mutex::Autolock autoLock(mLock);
    status_t status = NO_ERROR;
    // 1. Set the absolute time stamp
    // ToDo: We need the ioctl from driver to set the time stamp
    
    // 2. Signal the driver to start rendering data
    if (ioctl(mPcmRxHandle->handle->fd, SNDRV_PCM_IOCTL_START)) {
        LOGE("start:SNDRV_PCM_IOCTL_START failed\n");
        status = BAD_VALUE;
    }
    return status;
}

status_t AudioBroadcastStreamALSA::mute(bool mute)
{
    LOGV("mute");
    Mutex::Autolock autoLock(mLock);
    status_t status = NO_ERROR;
    uint32_t volume;
    if(mute) {
        // Set the volume to 0 to mute the stream
        volume = 0;
    } else {
        // Set the volume back to current volume
        volume = mStreamVol;
    }
    if(mPcmRxHandle) {
        if((!strncmp(mPcmRxHandle->useCase, SND_USE_CASE_VERB_HIFI3,
                     strlen(SND_USE_CASE_VERB_HIFI3)) ||
            !strncmp(mPcmRxHandle->useCase, SND_USE_CASE_MOD_PLAY_MUSIC3,
                     strlen(SND_USE_CASE_MOD_PLAY_MUSIC3)))) {
            status = mPcmRxHandle->module->setPcmVolume(volume);
        }
    } else if(mCompreRxHandle) {
        if(!strncmp(mCompreRxHandle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL2,
                    strlen(SND_USE_CASE_VERB_HIFI_TUNNEL2)) ||
           !strncmp(mCompreRxHandle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL2,
                    strlen(SND_USE_CASE_MOD_PLAY_TUNNEL2)) &&
           (mRouteCompreToSpdif == false)) {
            status = mCompreRxHandle->module->setCompressedVolume(volume);
        }
    }
    return OK;
}

status_t AudioBroadcastStreamALSA::setVolume(float left, float right)
{
    LOGV("setVolume");
    Mutex::Autolock autoLock(mLock);
    float volume;
    status_t status = NO_ERROR;

    volume = (left + right) / 2;
    if (volume < 0.0) {
        LOGD("setVolume(%f) under 0.0, assuming 0.0\n", volume);
        volume = 0.0;
    } else if (volume > 1.0) {
        LOGD("setVolume(%f) over 1.0, assuming 1.0\n", volume);
        volume = 1.0;
    }
    mStreamVol = lrint((volume * 100.0)+0.5);

    LOGD("Setting broadcast stream volume to %d \
                 (available range is 0 to 100)\n", mStreamVol);
    if(mPcmRxHandle) {
        if((!strncmp(mPcmRxHandle->useCase, SND_USE_CASE_VERB_HIFI3,
                     strlen(SND_USE_CASE_VERB_HIFI3)) ||
            !strncmp(mPcmRxHandle->useCase, SND_USE_CASE_MOD_PLAY_MUSIC3,
                     strlen(SND_USE_CASE_MOD_PLAY_MUSIC3)))) {
            status = mPcmRxHandle->module->setPcmVolume(mStreamVol);
        }
    } else if(mCompreRxHandle) {
        if(!strncmp(mCompreRxHandle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL2,
                    strlen(SND_USE_CASE_VERB_HIFI_TUNNEL2)) ||
           !strncmp(mCompreRxHandle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL2,
                    strlen(SND_USE_CASE_MOD_PLAY_TUNNEL2)) &&
           (mRouteCompreToSpdif == false)) {
            status = mCompreRxHandle->module->setCompressedVolume(mStreamVol);
        }
    }
    return status;
}

//NOTE:
// This will be used for Digital Broadcast usecase. This function will serve
// as a wrapper to handle the buffering of both Main and Associated data in
// case of dual decode use case.
ssize_t AudioBroadcastStreamALSA::write(const void *buffer, size_t bytes, 
                                        int64_t timestamp, int audiotype)
{
    LOGV("write");
    Mutex::Autolock autoLock(mLock);
    int period_size;
    char *use_case;

    LOGV("write:: buffer %p, bytes %d", buffer, bytes);
    if (!mPowerLock) {
        acquire_wake_lock (PARTIAL_WAKE_LOCK, "AudioOutLock");
        mPowerLock = true;
    }

    snd_pcm_sframes_t n;
    size_t            sent = 0;
    status_t          err;

    // 1. Check if MS11 decoder instance is present and if present we need to
    //    preserve the data and supply it to MS 11 decoder.
    if(mMS11Decoder != NULL) {
    }

    // 2. Get the output data from MS11 decoder and write to PCM driver
    if(mPcmRxHandle && mRoutePcmAudio) {
        int write_pending = bytes;
        period_size = mPcmRxHandle->periodSize;
        do {
            if (write_pending < period_size) {
                LOGE("write:: We should not be here !!!");
                write_pending = period_size;
            }
            n = pcm_write(mPcmRxHandle->handle,
                     (char *)buffer + sent,
                      period_size);
            if (n == -EBADFD) {
                // Somehow the stream is in a bad state. The driver probably
                // has a bug and snd_pcm_recover() doesn't seem to handle this.
                mPcmRxHandle->module->open(mPcmRxHandle);
            }
            else if (n < 0) {
                // Recovery is part of pcm_write. TODO split is later.
                LOGE("pcm_write returned n < 0");
                return static_cast<ssize_t>(n);
            }
            else {
                mFrameCount += n;
                sent += static_cast<ssize_t>((period_size));
                write_pending -= period_size;
            }
        } while ((mPcmRxHandle->handle) && (sent < bytes));
    }

    return bytes;
}

status_t AudioBroadcastStreamALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

status_t AudioBroadcastStreamALSA::standby()
{
    LOGD("standby");
    Mutex::Autolock autoLock(mLock);

    if (mRouteAudioToA2dp) {
        status_t status = mParent->stopA2dpPlayback_l(
                                     AudioHardwareALSA::A2DPBroadcast);
        if(status) {
            LOGE("stopA2dpPlayback_l from standby returned = %d", status);
            return status;
        }
        mRouteAudioToA2dp = false;
    }

    if(mPcmRxHandle) {
        mPcmRxHandle->module->standby(mPcmRxHandle);
    }

    if(mCompreRxHandle) {
        mCompreRxHandle->module->standby(mCompreRxHandle);
    }

    if(mPcmTxHandle) {
        mPcmTxHandle->module->standby(mPcmTxHandle);
    }
    if(mCompreTxHandle) {
        mCompreTxHandle->module->standby(mCompreTxHandle);
    }
    if (mPowerLock) {
        release_wake_lock ("AudioBroadcastLock");
        mPowerLock = false;
    }

    mFrameCount = 0;

    return NO_ERROR;
}

#define USEC_TO_MSEC(x) ((x + 999) / 1000)

uint32_t AudioBroadcastStreamALSA::latency() const
{
    // Android wants latency in milliseconds.
    return USEC_TO_MSEC (mPcmRxHandle->latency);
}

// return the number of audio frames written by the audio dsp to DAC since
// the output has exited standby
status_t AudioBroadcastStreamALSA::getRenderPosition(uint32_t *dspFrames)
{
    *dspFrames = mFrameCount;
    return NO_ERROR;
}

/*******************************************************************************
Description: update sample rate and channel info based on format
*******************************************************************************/
void AudioBroadcastStreamALSA::updateSampleRateChannelMode()
{
//NOTE: For AAC, the output of MS11 is 48000 for the sample rates greater than
//      24000. The samples rates <= 24000 will be at their native sample rate
//      and channel mode
//      For AC3, the PCM output is at its native sample rate if the decoding is
//      single decode usecase for MS11.
//      Since, SPDIF is limited to PCM stereo for LPCM format, output is
//      stereo always.
    if(mFormat == AudioSystem::AAC || mFormat == AudioSystem::HE_AAC_V1 ||
       mFormat == AudioSystem::HE_AAC_V2) {
        if(mSampleRate > 24000) {
            mSampleRate = DEFAULT_SAMPLING_RATE;
            mChannels = DEFAULT_CHANNEL_MODE;
//NOTE: handle the multi channel PCM- do not update the channel mode to 2,
//      if Multi channel stereo to be supported
        }
    } else if(mFormat == AudioSystem::AC3 || mFormat == AudioSystem::AC3_PLUS) {
        if(mChannels > 2)
            mChannels = DEFAULT_CHANNEL_MODE;
    }
}

/*******************************************************************************
Description: Initialize Flags and device handles
*******************************************************************************/
void AudioBroadcastStreamALSA::initialization()
{
    mFrameCount        = 0;
    mFormat            = AudioSystem::INVALID_FORMAT;
    mSampleRate        = DEFAULT_SAMPLING_RATE;
    mChannels          = DEFAULT_CHANNEL_MODE;
    mBufferSize        = DEFAULT_BUFFER_SIZE;
    mStreamVol         = 0x2000;
    mPowerLock         = false;

    // device handles
    mPcmRxHandle       = NULL;
    mCompreRxHandle    = NULL;
    mPcmTxHandle       = NULL;
    mCompreTxHandle    = NULL;
    mCaptureHandle     = NULL;

    // MS11
    mMS11Decoder       = NULL;
    mBitstreamSM       = NULL;

    // capture and routing Flags
    mCapturePCMFromDSP       = false;
    mCaptureCompressedFromDSP= false;
    mRoutePCMStereoToDSP     = false;
    mUseMS11Decoder          = false;
    mUseTunnelDecoder        = false;
    mRoutePcmToSpdif         = false;
    mRoutePcmToHdmi          = false;
    mRouteCompreToSpdif      = false;
    mRouteCompreToHdmi       = false;
    mRoutePcmAudio           = false;
    mRoutingSetupDone        = false;
    mSignalToSetupRoutingPath = false;
    mInputBufferSize         = 0;
    mInputBufferCount        = 0;
    mMinBytesReqToDecode     = 0;
    mChannelStatusSet        = false;

    mAacConfigDataSet        = false;
    mWMAConfigDataSet        = false;

    // Thread
    mCaptureThread           = NULL;
    mKillCaptureThread       = false;
    mCaptureThreadAlive      = false;
    mExitReadCapturePath     = false;
    mCapturefd               = -1;
    mAvail                   = 0;
    mFrames                  = 0;
    mX.buf                   = NULL;
    mX.frames                = 0;
    mX.result                = 0;

    mPlaybackThread          = false;
    mKillPlaybackThread      = false;
    mPlaybackThreadAlive     = false;
    mPlaybackfd              = -1;
    // playback controls
    mSkipWrite               = false;
    mPlaybackReachedEOS      = false;
    isSessionPaused          = false;
    mObserver                = NULL;


    // Proxy
    mRouteAudioToA2dp        = false;

    return;
}


/*******************************************************************************
Description: set the output device format
*******************************************************************************/
void AudioBroadcastStreamALSA::updateOutputFormat()
{
    char value[128];
    property_get("mpq.audio.spdif.format",value,"0");
    if (!strncmp(value,"lpcm",sizeof(value)) ||
        !strncmp(value,"ac3",sizeof(value)) ||
        !strncmp(value,"dts",sizeof(value)))
        strlcpy(mSpdifOutputFormat, value, sizeof(mSpdifOutputFormat));
    else
        strlcpy(mSpdifOutputFormat, "lpcm", sizeof(mSpdifOutputFormat));

    property_get("mpq.audio.hdmi.format",value,"0");
    if (!strncmp(value,"lpcm",sizeof(value)) ||
        !strncmp(value,"ac3",sizeof(value)) ||
	!strncmp(value,"dts",sizeof(value)))
        strlcpy(mHdmiOutputFormat, value, sizeof(mHdmiOutputFormat));
    else
        strlcpy(mHdmiOutputFormat, "lpcm", sizeof(mHdmiOutputFormat));

    LOGV("mSpdifOutputFormat: %s", mSpdifOutputFormat);
    LOGV("mHdmiOutputFormat: %s", mHdmiOutputFormat);
    return;
}

/*******************************************************************************
Description: Set appropriate flags based on the configuration parameters
*******************************************************************************/
void AudioBroadcastStreamALSA::setCaptureFlagsBasedOnConfig()
{
    /*-------------------------------------------------------------------------
                    Set the capture and playback flags
    -------------------------------------------------------------------------*/
    // 1. Validate the audio source type
    if(mAudioSource == QCOM_AUDIO_SOURCE_ANALOG_BROADCAST) {
        mCapturePCMFromDSP = true;
    } else if(mAudioSource == QCOM_AUDIO_SOURCE_HDMI_IN) {
//NOTE:
// The format will be changed from decoder format to the format specified by
// ADV driver through TVPlayer
        if( mInputFormat == QCOM_BROADCAST_AUDIO_FORMAT_LPCM) {
            mCapturePCMFromDSP = true;
        } else {
            mCaptureCompressedFromDSP = true;
           // ToDo: What about mixing main and AD in this case?
           // Should we pull back both the main and AD decoded data and mix
           // using MS11 decoder?
        }
    }
}

void AudioBroadcastStreamALSA::setRoutingFlagsBasedOnConfig()
{
    if(mAudioSource == QCOM_AUDIO_SOURCE_ANALOG_BROADCAST) {
        mRoutePCMStereoToDSP = true;
    } else if(mAudioSource == QCOM_AUDIO_SOURCE_HDMI_IN ||
              mAudioSource == QCOM_AUDIO_SOURCE_DIGITAL_BROADCAST_MAIN_ONLY ||
              mAudioSource == QCOM_AUDIO_SOURCE_DIGITAL_BROADCAST_MAIN_AD) {
//NOTE:
// The format will be changed from decoder format to the format specified by
// DSP
        if((mFormat == AudioSystem::PCM_16_BIT) ||
           (mInputFormat == QCOM_BROADCAST_AUDIO_FORMAT_LPCM)) {
            if(mChannels <= 2) {
                mRoutePCMStereoToDSP = true;
            } else {
                mUseMS11Decoder = true;
            }
        } else {
            if(mFormat == AudioSystem::AC3 || mFormat == AudioSystem::AC3_PLUS ||
               mFormat == AudioSystem::AAC || mFormat == AudioSystem::HE_AAC_V1 ||
               mFormat == AudioSystem::HE_AAC_V2) {
                mUseMS11Decoder = true;
            } else {
                mUseTunnelDecoder = true;
            }
        }
    }
    /*-------------------------------------------------------------------------
                    Set the device routing flags
    -------------------------------------------------------------------------*/
    if(!strncmp(mSpdifOutputFormat,"lpcm",sizeof(mSpdifOutputFormat)) ||
       !strncmp(mSpdifOutputFormat,"dts",sizeof(mSpdifOutputFormat)) ) {
        if(mDevices & AudioSystem::DEVICE_OUT_SPDIF)
            mRoutePcmToSpdif = true;
    }
    if(!strncmp(mHdmiOutputFormat,"lpcm",sizeof(mHdmiOutputFormat)) ||
       !strncmp(mHdmiOutputFormat,"dts",sizeof(mHdmiOutputFormat)) ) {
        if(mDevices & AudioSystem::DEVICE_OUT_AUX_DIGITAL)
            mRoutePcmToHdmi = true;
    }
    if(!strncmp(mSpdifOutputFormat,"ac3",sizeof(mSpdifOutputFormat)) &&
       !mUseTunnelDecoder) {
        if(mDevices & AudioSystem::DEVICE_OUT_SPDIF)
            if(mSampleRate > 24000 && mFormat != AUDIO_FORMAT_PCM_16_BIT)
                mRouteCompreToSpdif = true;
            else
                mRoutePcmToSpdif = true;
    }
    if(!strncmp(mHdmiOutputFormat,"ac3",sizeof(mHdmiOutputFormat)) &&
       !mUseTunnelDecoder) {
        if(mDevices & AudioSystem::DEVICE_OUT_AUX_DIGITAL)
            if(mSampleRate > 24000 && mFormat != AUDIO_FORMAT_PCM_16_BIT)
                mRouteCompreToHdmi = true;
            else
                mRoutePcmToHdmi = true;
    }
    if(mRoutePcmToSpdif || mRoutePcmToHdmi || mRoutePCMStereoToDSP ||
       ((mDevices & ~AudioSystem::DEVICE_OUT_SPDIF) &&
       (mDevices & ~AudioSystem::DEVICE_OUT_AUX_DIGITAL))) {
        mRoutePcmAudio = true;
    }
    if(mUseTunnelDecoder)
        mRoutePcmAudio = false;

    return;
}

/*******************************************************************************
Description: open appropriate capture and routing devices
*******************************************************************************/
status_t AudioBroadcastStreamALSA::openCapturingAndRoutingDevices()
{
    int32_t devices = mDevices;
    status_t status = NO_ERROR;
    bool bIsUseCaseSet = false;
    mCaptureHandle = NULL;
    /* ------------------------------------------------------------------------
    Description: Set appropriate Capture flags based on the configuration
                 parameters
    -------------------------------------------------------------------------*/
    setCaptureFlagsBasedOnConfig();

    /*-------------------------------------------------------------------------
                           open capture device
    -------------------------------------------------------------------------*/
    if(mCapturePCMFromDSP) {
        status = openPCMCapturePath();
        if(status != NO_ERROR) {
            LOGE("open capture path for pcm stereo failed");
            return status;
        }
        mCaptureHandle = (alsa_handle_t *) mPcmTxHandle;
    } else if(mCaptureCompressedFromDSP) {
        status = openCompressedCapturePath();
        if(status != NO_ERROR) {
            LOGE("open capture path for compressed failed ");
            return status;
        }
        mCaptureHandle = (alsa_handle_t *) mCompreTxHandle;
    } else {
        LOGD("Capture path not enabled");
    }
    if(mCaptureHandle) {
        status = createCaptureThread();
        if(status != NO_ERROR) {
            LOGE("create captured thread failed");
            return status;
        }
        mCaptureCv.signal();
        LOGD("Capture path setup successful");
    }

    /*-------------------------------------------------------------------------
                wait till first read event to setup the routing path
    -------------------------------------------------------------------------*/
    if(mCapturePCMFromDSP || mCaptureCompressedFromDSP) {
        mSignalToSetupRoutingPath = true;

        Mutex::Autolock autolock(mRoutingSetupMutex);

        mRoutingSetupCv.wait(mRoutingSetupMutex);
    }
    mSignalToSetupRoutingPath = false;

    /* ------------------------------------------------------------------------
    Description: Set appropriate Routing flags based on the configuration
                 parameters
    -------------------------------------------------------------------------*/
    setRoutingFlagsBasedOnConfig();

    /* ------------------------------------------------------------------------
    Description: update sample rate and channel info based on format
    -------------------------------------------------------------------------*/
    updateSampleRateChannelMode();

    /*-------------------------------------------------------------------------
                           open routing device
    -------------------------------------------------------------------------*/
    if(mRoutePcmAudio) {
        status = openPcmDevice(mDevices);
        if(status != NO_ERROR) {
            LOGE("PCM device open failure");
            return status;
        }
    }
    if(mUseTunnelDecoder || mRouteCompreToSpdif || mRouteCompreToHdmi) {
        if(mRouteCompreToSpdif && mRouteCompreToHdmi)
            devices = AudioSystem::DEVICE_OUT_SPDIF |
                         AudioSystem::DEVICE_OUT_AUX_DIGITAL;
        else if(mRouteCompreToSpdif)
            devices = AudioSystem::DEVICE_OUT_SPDIF;
        else if(mRouteCompreToHdmi)
            devices = AudioSystem::DEVICE_OUT_AUX_DIGITAL;
        else
            devices = mDevices;

        if(mFormat != AUDIO_FORMAT_WMA && mFormat != AUDIO_FORMAT_WMA_PRO) {
            status = openTunnelDevice(devices);
            if(status != NO_ERROR) {
                LOGE("Tunnel device open failure");
                return status;
            }
        }
        createPlaybackThread();
    }
    mBitstreamSM = new AudioBitstreamSM;
    if(false == mBitstreamSM->initBitstreamPtr()) {
        LOGE("Unable to allocate Memory for i/p and o/p buffering for MS11");
        delete mBitstreamSM;
        return BAD_VALUE;
    }
    if(mUseMS11Decoder) {
        status = openMS11Instance();
        if(status != NO_ERROR) {
            LOGE("Unable to open MS11 instance succesfully- exiting");
            mUseMS11Decoder = false;
            delete mBitstreamSM;
            return status;
        }
    }
    mRoutingSetupDone = true;
    return NO_ERROR;
}

status_t AudioBroadcastStreamALSA::openPCMCapturePath()
{
    LOGV("openPCMStereoCapturePath");
    bool bIsUseCaseSet = false;
    status_t status = NO_ERROR;
    alsa_handle_t alsa_handle;
    char *use_case;

    alsa_handle.module = mParent->mALSADevice;
    if (mChannels <= DEFAULT_CHANNEL_MODE)
        alsa_handle.bufferSize = DEFAULT_IN_BUFFER_SIZE_BROADCAST_PCM_STEREO;
    else
        alsa_handle.bufferSize = DEFAULT_IN_BUFFER_SIZE_BROADCAST_PCM_MCH;
    alsa_handle.devices = AudioSystem::DEVICE_IN_AUX_DIGITAL;
//NOTE: what is the device ID that has to be set
    alsa_handle.activeDevice = AudioSystem::DEVICE_IN_AUX_DIGITAL;
    alsa_handle.handle = 0;
    alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
    alsa_handle.channels = mChannels;
    alsa_handle.sampleRate = mSampleRate;
    alsa_handle.latency = RECORD_LATENCY;
    alsa_handle.rxHandle = 0;
    alsa_handle.mode = mParent->mode();
    alsa_handle.ucMgr = mParent->mUcMgr;
    alsa_handle.periodSize = alsa_handle.bufferSize;
    snd_use_case_get(mParent->mUcMgr, "_verb", (const char **)&use_case);
    if ((use_case == NULL) ||
        (!strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
             strlen(SND_USE_CASE_VERB_INACTIVE)))) {
        strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_REC2,
                    sizeof(alsa_handle.useCase));
        bIsUseCaseSet = true;
    } else {
        strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC2,
                    sizeof(alsa_handle.useCase));
    }
    if(use_case)
        free(use_case);
    mParent->mDeviceList.push_back(alsa_handle);
    ALSAHandleList::iterator it = mParent->mDeviceList.end(); it--;
    LOGD("useCase %s", it->useCase);
    mPcmTxHandle = &(*it);

    status = mALSADevice->setCaptureFormat("LPCM");
    if(status != NO_ERROR) {
        LOGE("set Capture format failed");
        return BAD_VALUE;
    }
    mALSADevice->setUseCase(mPcmTxHandle, bIsUseCaseSet,"MI2S");
    status = mALSADevice->openCapture(mPcmTxHandle, true, false);

    return status;
}

status_t AudioBroadcastStreamALSA::openCompressedCapturePath()
{
    LOGV("openCompressedCapturePath");
    bool bIsUseCaseSet = false;
    alsa_handle_t alsa_handle;
    char *use_case;
    status_t status = NO_ERROR;

    alsa_handle.module = mParent->mALSADevice;
    alsa_handle.bufferSize = DEFAULT_IN_BUFFER_SIZE_BROADCAST_COMPRESSED;
    alsa_handle.devices = AudioSystem::DEVICE_IN_AUX_DIGITAL;
//NOTE: what is the device ID that has to be set
    alsa_handle.activeDevice = AudioSystem::DEVICE_IN_AUX_DIGITAL;
    alsa_handle.handle = 0;
    alsa_handle.format = mInputFormat;
    if(mInputFormat == QCOM_BROADCAST_AUDIO_FORMAT_COMPRESSED)
        alsa_handle.channels = 2; //This is to set one MI2S line to read data
    else
        alsa_handle.channels = 8; //This is to set four MI2S lines to read data
    alsa_handle.sampleRate = mSampleRate;
    alsa_handle.latency = RECORD_LATENCY;
    alsa_handle.rxHandle = 0;
    alsa_handle.mode = mParent->mode();
    alsa_handle.ucMgr = mParent->mUcMgr;
    alsa_handle.periodSize = DEFAULT_IN_BUFFER_SIZE_BROADCAST_COMPRESSED;

    snd_use_case_get(mParent->mUcMgr, "_verb", (const char **)&use_case);
    if ((use_case == NULL) ||
        (!strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
             strlen(SND_USE_CASE_VERB_INACTIVE)))) {
        strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_REC_COMPRESSED,
                    sizeof(alsa_handle.useCase));
        bIsUseCaseSet = true;
    } else {
        strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC_COMPRESSED,
                    sizeof(alsa_handle.useCase));
    }
    if(use_case) {
        free(use_case);
        use_case = NULL;
    }
    mParent->mDeviceList.push_back(alsa_handle);
    ALSAHandleList::iterator it = mParent->mDeviceList.end(); it--;
    LOGD("useCase %s", it->useCase);
    mCompreTxHandle = &(*it);

    status = mALSADevice->setCaptureFormat("Compr");
    if(status != NO_ERROR) {
        LOGE("set Capture format failed");
        return BAD_VALUE;
    }
    mALSADevice->setUseCase(mCompreTxHandle, bIsUseCaseSet, "MI2S");
    status =  mALSADevice->openCapture(mCompreTxHandle, true, true);
    return status;
}

status_t AudioBroadcastStreamALSA::openPcmDevice(int devices)
{
    char *use_case;
    status_t status = NO_ERROR;

    if(!mRoutePcmToSpdif) {
        devices = devices & ~AudioSystem::DEVICE_OUT_SPDIF;
    }
    if(!mRoutePcmToHdmi) {
        devices = devices & ~AudioSystem::DEVICE_OUT_AUX_DIGITAL;
    }
    if(mRoutePcmToSpdif) {
        if(!strncmp(mSpdifOutputFormat, "lpcm",
              sizeof(mSpdifOutputFormat))) {
            status = mALSADevice->setPlaybackFormat("LPCM",
                         AudioSystem::DEVICE_OUT_SPDIF);
            if(status != NO_ERROR)
                return status;
        } else {
            //ToDo: handle DTS;
        }
    }
    if(mRoutePcmToHdmi) {
        if(!strncmp(mHdmiOutputFormat, "lpcm",
              sizeof(mHdmiOutputFormat))) {
            status = mALSADevice->setPlaybackFormat("LPCM",
                        AudioSystem::DEVICE_OUT_AUX_DIGITAL);
            if (status != NO_ERROR)
                return status;
        } else {
            //ToDo: handle DTS
        }
    }
    snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
    if ((use_case == NULL) ||
        (!strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
            strlen(SND_USE_CASE_VERB_INACTIVE)))) {
        status = openRoutingDevice(SND_USE_CASE_VERB_HIFI3, true,
                     devices);
    } else {
        status = openRoutingDevice(SND_USE_CASE_MOD_PLAY_MUSIC3, false,
                     devices);
    }
    if(use_case)
        free(use_case);
    if(status != NO_ERROR) {
        return status;
    }
    ALSAHandleList::iterator it = mParent->mDeviceList.end(); it--;
    mPcmRxHandle = &(*it);
    mBufferSize = mPcmRxHandle->periodSize;
    return status;
}

status_t AudioBroadcastStreamALSA::openTunnelDevice(int devices)
{
    LOGV("openTunnelDevice");
    char *use_case;
    status_t status = NO_ERROR;

    mInputBufferSize    = TUNNEL_DECODER_BUFFER_SIZE;
    mInputBufferCount   = TUNNEL_DECODER_BUFFER_COUNT;
    if(mRoutePcmToSpdif) {
        if(!strncmp(mSpdifOutputFormat,"lpcm",sizeof(mSpdifOutputFormat))) {
            status = mALSADevice->setPlaybackFormat("LPCM",
                                              AudioSystem::DEVICE_OUT_SPDIF);
            if(status != NO_ERROR)
                return status;
            if(mALSADevice->get_linearpcm_channel_status(mSampleRate,
                                mChannelStatus)) {
                LOGE("channel status set error ");
                return BAD_VALUE;
            }
            mALSADevice->setChannelStatus(mChannelStatus);
            mChannelStatusSet = true;
        } else {
            //ToDo: handle DTS;
        }
    } else if(mRouteCompreToSpdif) {
        status = mALSADevice->setPlaybackFormat("Compr",
                                  AudioSystem::DEVICE_OUT_SPDIF);
        if(status != NO_ERROR)
            return status;
    }
    if(mRoutePcmToHdmi) {
        if(!strncmp(mHdmiOutputFormat,"lpcm",sizeof(mHdmiOutputFormat))) {
            status = mALSADevice->setPlaybackFormat("LPCM",
                                      AudioSystem::DEVICE_OUT_AUX_DIGITAL);
            if (status != NO_ERROR)
               return status;
        } else {
            //ToDo: handle DTS;
        }
    } else if (mRouteCompreToHdmi) {
        status = mALSADevice->setPlaybackFormat("Compr",
                                  AudioSystem::DEVICE_OUT_AUX_DIGITAL);
        if (status != NO_ERROR)
           return status;
    }
    snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
    if ((use_case == NULL) || (!strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
            strlen(SND_USE_CASE_VERB_INACTIVE)))) {
        status = openRoutingDevice(SND_USE_CASE_VERB_HIFI_TUNNEL2, true,
                     devices);
    } else {
        status = openRoutingDevice(SND_USE_CASE_MOD_PLAY_TUNNEL2, false,
                     devices);
    }
    if(use_case)
        free(use_case);
    if(status != NO_ERROR) {
        return status;
    }
    ALSAHandleList::iterator it = mParent->mDeviceList.end(); it--;
    mCompreRxHandle = &(*it);

    //mmap the buffers for playback
    status = mmap_buffer(mCompreRxHandle->handle);
    if(status) {
        LOGE("MMAP buffer failed - playback err = %d", status);
        return status;
    }
    //prepare the driver for playback
    status = pcm_prepare(mCompreRxHandle->handle);
    if (status) {
        LOGE("PCM Prepare failed - playback err = %d", status);
        return status;
    }
    bufferAlloc(mCompreRxHandle);
    mBufferSize = mCompreRxHandle->periodSize;

    return NO_ERROR;
}

status_t AudioBroadcastStreamALSA::openRoutingDevice(char *useCase,
                                  bool bIsUseCase, int devices)
{
    alsa_handle_t alsa_handle;
    status_t status = NO_ERROR;
    LOGD("openRoutingDevice: E");
    alsa_handle.module      = mALSADevice;
    alsa_handle.bufferSize  = mInputBufferSize;
    alsa_handle.devices     = devices;
    alsa_handle.activeDevice= devices;
    alsa_handle.handle      = 0;
    alsa_handle.format      = (mFormat == AUDIO_FORMAT_PCM_16_BIT ?
                                  SNDRV_PCM_FORMAT_S16_LE : mFormat);
    alsa_handle.channels    = mChannels;
    alsa_handle.sampleRate  = mSampleRate;
    alsa_handle.mode        = mParent->mode();
    alsa_handle.latency     = PLAYBACK_LATENCY;
    alsa_handle.rxHandle    = 0;
    alsa_handle.ucMgr       = mUcMgr;
    strlcpy(alsa_handle.useCase, useCase, sizeof(alsa_handle.useCase));

    mALSADevice->setUseCase(&alsa_handle, bIsUseCase);
    status = mALSADevice->open(&alsa_handle);
    if(status != NO_ERROR) {
        LOGE("Could not open the ALSA device for use case %s",
                alsa_handle.useCase);
        mALSADevice->close(&alsa_handle);
    } else{
        mParent->mDeviceList.push_back(alsa_handle);
    }
    LOGD("openRoutingDevice: X");
    return status;
}

status_t AudioBroadcastStreamALSA::openMS11Instance()
{
    int32_t formatMS11;
    mMS11Decoder = new SoftMS11;
    if(mMS11Decoder->initializeMS11FunctionPointers() == false) {
        LOGE("Could not resolve all symbols Required for MS11");
        delete mMS11Decoder;
        return BAD_VALUE;
    }
    if(mFormat == AUDIO_FORMAT_AAC || mFormat == AUDIO_FORMAT_HE_AAC_V1 ||
       mFormat == AUDIO_FORMAT_HE_AAC_V2 || mFormat == AUDIO_FORMAT_AAC_ADIF) {
        if(mFormat == AUDIO_FORMAT_AAC_ADIF)
            mMinBytesReqToDecode = AAC_BLOCK_PER_CHANNEL_MS11*mChannels-1;
        else
            mMinBytesReqToDecode = 0;
        formatMS11 = FORMAT_DOLBY_PULSE_MAIN;
    } else if(mFormat = AUDIO_FORMAT_AC3 || mFormat == AUDIO_FORMAT_AC3_PLUS) {
        formatMS11 = FORMAT_DOLBY_DIGITAL_PLUS_MAIN;
        mMinBytesReqToDecode = 0;
    } else {
        formatMS11 = FORMAT_EXTERNAL_PCM;
    }
    if(mMS11Decoder->setUseCaseAndOpenStream(formatMS11,mChannels,mSampleRate)) {
        LOGE("SetUseCaseAndOpen MS11 failed");
        delete mMS11Decoder;
        return BAD_VALUE;
    }
    mAacConfigDataSet = false;
    return NO_ERROR;
}

/******************************************************************************
                                 THREADS
******************************************************************************/

status_t AudioBroadcastStreamALSA::createCaptureThread()
{
    LOGV("createCaptureThread");
    status_t status = NO_ERROR;

    mKillCaptureThread = false;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    status = pthread_create(&mCaptureThread, (const pthread_attr_t *) NULL,
                            captureThreadWrapper, this);
    if(status) {
        LOGE("create capture thread failed = %d", status);
    } else {
        LOGD("Capture Thread created");
        mCaptureThreadAlive = true;
    }
    return status;
}

status_t AudioBroadcastStreamALSA::createPlaybackThread()
{
    LOGV("createPlaybackThread");
    status_t status = NO_ERROR;

    mKillPlaybackThread = false;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    status = pthread_create(&mPlaybackThread, (const pthread_attr_t *) NULL,
                            playbackThreadWrapper, this);
    if(status) {
        LOGE("create playback thread failed = %d", status);
    } else {
        LOGD("Playback Thread created");
        mPlaybackThreadAlive = true;
    }
    return status;
}

void * AudioBroadcastStreamALSA::captureThreadWrapper(void *me)
{
    static_cast<AudioBroadcastStreamALSA *>(me)->captureThreadEntry();
    return NULL;
}

void * AudioBroadcastStreamALSA::playbackThreadWrapper(void *me)
{
    static_cast<AudioBroadcastStreamALSA *>(me)->playbackThreadEntry();
    return NULL;
}

void AudioBroadcastStreamALSA::allocateCapturePollFd()
{
    LOGV("allocateCapturePollFd");
    unsigned flags  = mCaptureHandle->handle->flags;
    struct pcm* pcm = mCaptureHandle->handle;

    if(mCapturefd == -1) {
        mCapturePfd[0].fd     = mCaptureHandle->handle->fd;
        mCapturePfd[0].events = POLLIN | POLLERR | POLLNVAL;
        mCapturefd            = eventfd(0,0);
        mCapturePfd[1].fd     = mCapturefd;
        mCapturePfd[1].events = POLLIN | POLLERR | POLLNVAL;

        if (flags & PCM_MONO) {
            mFrames   = pcm->period_size/2;
            mX.frames = pcm->period_size/2;
        } else if (flags & PCM_QUAD) {
            mFrames   = pcm->period_size/8;
            mX.frames = pcm->period_size/8;
        } else if (flags & PCM_5POINT1) {
            mFrames   = pcm->period_size/12;
            mX.frames = pcm->period_size/12;
        } else {
            mFrames   = pcm->period_size/4;
            mX.frames = pcm->period_size/4;
        }
    }
}

void AudioBroadcastStreamALSA::allocatePlaybackPollFd()
{
    LOGV("allocatePlaybackPollFd");
    if(mPlaybackfd == -1) {
        mPlaybackPfd[0].fd     = mCompreRxHandle->handle->timer_fd;
        mPlaybackPfd[0].events = POLLIN | POLLERR | POLLNVAL;
        mPlaybackfd            = eventfd(0,0);
        mPlaybackPfd[1].fd     = mPlaybackfd;
        mPlaybackPfd[1].events = POLLIN | POLLERR | POLLNVAL;
    }
}

status_t AudioBroadcastStreamALSA::startCapturePath()
{
    LOGV("startCapturePath");
    status_t status = NO_ERROR;
    struct pcm * capture_handle = (struct pcm *)mCaptureHandle->handle;

    while(1) {
        if(!capture_handle->start) {
            if(ioctl(capture_handle->fd, SNDRV_PCM_IOCTL_START)) {
                status = -errno;
                if (errno == EPIPE) {
                    LOGV("Failed in SNDRV_PCM_IOCTL_START\n");
                    /* we failed to make our window -- try to restart */
                    capture_handle->underruns++;
                    capture_handle->running = 0;
                    capture_handle->start = 0;
                    continue;
                } else {
                    LOGE("IOCTL_START failed for proxy err: %d \n", errno);
                    return status;
                }
           } else {
               LOGD(" Capture Driver started(IOCTL_START Success)\n");
               capture_handle->start = 1;
               break;
           }
       } else {
           LOGV("Capture driver Already started break out of condition");
           break;
       }
   }
   capture_handle->sync_ptr->flags = SNDRV_PCM_SYNC_PTR_APPL |
                                     SNDRV_PCM_SYNC_PTR_AVAIL_MIN;
   return status;
}

void AudioBroadcastStreamALSA::captureThreadEntry()
{
    LOGV("captureThreadEntry");
    androidSetThreadPriority(gettid(), ANDROID_PRIORITY_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"Broadcast Capture Thread", 0, 0, 0);

    uint32_t frameSize = 0;
    status_t status = NO_ERROR;
    ssize_t size = 0;
    resetCapturePathVariables();
    char tempBuffer[mCaptureHandle->handle->period_size];

    LOGV("mKillCaptureThread = %d", mKillCaptureThread);
    while(!mKillCaptureThread && (mCaptureHandle != NULL)) {
        {
            Mutex::Autolock autolock(mCaptureMutex);

            size = readFromCapturePath(tempBuffer);
            if(size <= 0) {
                LOGE("capturePath returned size = %d", size);
                if(mCaptureHandle) {
                    status = pcm_prepare(mCaptureHandle->handle);
                    if(status != NO_ERROR)
                        LOGE("DEVICE_OUT_DIRECTOUTPUT: pcm_prepare failed");
                }
                continue;
            } else if (mSignalToSetupRoutingPath ==  true) {
                LOGD("Setup the Routing path based on the format from DSP");
                if(mInputFormat == QCOM_BROADCAST_AUDIO_FORMAT_LPCM)
                    mFormat = AudioSystem::PCM_16_BIT;
                else
                    mFormat = AudioSystem::AC3;
//NOTE:
// Fill the format from the meta data from DSP. Right now hardcoded to AC3
// as support is available only for AC3 and without meta data

//NOTE:
// Read the format from the buffer and update mFormat so that the routing
// path can be set accordingly
                mRoutingSetupCv.signal();
                mSignalToSetupRoutingPath = false;
            } else if(mRoutingSetupDone == false) {
                LOGD("Routing Setup is not ready. Dropping the samples");
                continue;
            } else {
                char *bufPtr = tempBuffer;
//NOTE: Each buffer contains valida data in length based on framesize which
//      can be less than the buffer size frm DSP to kernel. The valid length
//      prefixed to the buffer from driver to HAL. Hence, frame length is
//      extracted to read valid data from the buffer. Framelength is of 32 bit.
                if (mCaptureCompressedFromDSP) {
                    frameSize =  (uint32_t)(tempBuffer[3] << 24) +
                                 (uint32_t)(tempBuffer[2] << 16) +
                                 (uint32_t)(tempBuffer[1] << 8) +
                                 (uint32_t)(tempBuffer[0] << 0);
                    bufPtr += COMPRE_CAPTURE_HEADER_SIZE;
                } else {
                    frameSize = size;
                }
                LOGV("frameSize: %d", frameSize);
                write_l(bufPtr, frameSize);
            }
        }
    }

    resetCapturePathVariables();
    mCaptureThreadAlive = false;
    LOGD("Capture Thread is dying");
}

ssize_t AudioBroadcastStreamALSA::readFromCapturePath(char *buffer)
{
    LOGV("readFromCapturePath");
    status_t status = NO_ERROR;
    int err_poll = 0;

    allocateCapturePollFd();
    status = startCapturePath();
    if(status != NO_ERROR) {
        LOGE("start capture path fail");
        return status;
    }
    struct pcm * capture_handle = (struct pcm *)mCaptureHandle->handle;

    while(!mExitReadCapturePath) {
        status = sync_ptr(capture_handle);
        if(status == EPIPE) {
            LOGE("Failed in sync_ptr \n");
            /* we failed to make our window -- try to restart */
            capture_handle->underruns++;
            capture_handle->running = 0;
            capture_handle->start = 0;
            continue;
        } else if(status != NO_ERROR){
            LOGE("Error: Sync ptr returned %d", status);
            break;
        }
        mAvail = pcm_avail(capture_handle);
        LOGV("avail is = %d frames = %ld, avai_min = %d\n",\
              mAvail, mFrames,(int)capture_handle->sw_p->avail_min);

        if (mAvail < capture_handle->sw_p->avail_min) {
            err_poll = poll(mCapturePfd, NUM_FDS, TIMEOUT_INFINITE);

            if (mCapturePfd[1].revents & POLLIN)
                LOGV("Event on userspace fd");

            if ((mCapturePfd[1].revents & POLLERR) ||
                (mCapturePfd[1].revents & POLLNVAL) ||
                (mCapturePfd[0].revents & POLLERR) ||
                (mCapturePfd[0].revents & POLLNVAL)) {
                LOGV("POLLERR or INVALID POLL");
                status = BAD_VALUE;
                break;
            }

            if (mCapturePfd[0].revents & POLLIN)
                LOGV("POLLIN on zero");

            LOGV("err_poll = %d",err_poll);
            continue;
        }
        break;
    }
    if(status != NO_ERROR) {
        LOGE("Reading from Capture path failed = err = %d", status);
        return status;
    }
    if (mX.frames > mAvail)
        mFrames = mAvail;

    void *data  = dst_address(capture_handle);

    if(data != NULL)
        memcpy(buffer, (char *)data, capture_handle->period_size);

    mX.frames -= mFrames;
    capture_handle->sync_ptr->c.control.appl_ptr += mFrames;
    capture_handle->sync_ptr->flags = 0;

    status = sync_ptr(capture_handle);
    if(status == EPIPE) {
        if(status != NO_ERROR ) {
            LOGE("Error: Sync ptr end returned %d", status);
            return status;
        }
    }
    return capture_handle->period_size;
}

void  AudioBroadcastStreamALSA::playbackThreadEntry()
{
    int err_poll = 0;
    int avail = 0;
    struct pcm * local_handle = (struct pcm *)mCompreRxHandle->handle;
    androidSetThreadPriority(gettid(), ANDROID_PRIORITY_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"HAL Audio EventThread", 0, 0, 0);

    mPlaybackMutex.lock();
    LOGV("PlaybackThreadEntry wait for signal \n");
    mPlaybackCv.wait(mPlaybackMutex);
    LOGV("PlaybackThreadEntry ready to work \n");
    mPlaybackMutex.unlock();

    if(!mKillPlaybackThread)
        allocatePlaybackPollFd();

    while(!mKillPlaybackThread && ((err_poll = poll(mPlaybackPfd, NUM_FDS, -1)) >=0)) {
        if (err_poll == EINTR)
            LOGE("Timer is intrrupted");

        if (mPlaybackPfd[1].revents & POLLIN) {
            uint64_t u;
            read(mPlaybackfd, &u, sizeof(uint64_t));
            LOGV("POLLIN event occured on the d, value written to %llu",
                    (unsigned long long)u);
            mPlaybackPfd[1].revents = 0;
            if (u == SIGNAL_PLAYBACK_THREAD) {
                mPlaybackReachedEOS = true;
                continue;
            }
        }
        if ((mPlaybackPfd[1].revents & POLLERR) ||
            (mPlaybackPfd[1].revents & POLLNVAL))
            LOGE("POLLERR or INVALID POLL");

        struct snd_timer_tread rbuf[4];
        read(local_handle->timer_fd, rbuf, sizeof(struct snd_timer_tread) * 4 );
        if((mPlaybackPfd[0].revents & POLLERR) ||
           (mPlaybackPfd[0].revents & POLLNVAL))
            continue;

        if (mPlaybackPfd[0].revents & POLLIN && !mKillPlaybackThread) {
            mPlaybackPfd[0].revents = 0;
            LOGV("After an event occurs");
            {
                if (mInputMemFilledQueue.empty()) {
                    LOGV("Filled queue is empty");
                    continue;
                }
                mInputMemResponseMutex.lock();
                BuffersAllocated buf = *(mInputMemFilledQueue.begin());
                mInputMemFilledQueue.erase(mInputMemFilledQueue.begin());
                LOGV("mInputMemFilledQueue %d", mInputMemFilledQueue.size());
                if (mInputMemFilledQueue.empty() && mPlaybackReachedEOS) {
                    LOGE("Queue Empty");
                    //post the EOS To Player
//NOTE: In Broadcast stream, EOS is not available yet. This can be for
//      furture use
                    if (mObserver)
                        mObserver->postEOS(0);
                }
                mInputMemResponseMutex.unlock();

                mInputMemRequestMutex.lock();

                mInputMemEmptyQueue.push_back(buf);

                mInputMemRequestMutex.unlock();
                mWriteCv.signal();
            }
        }
    }
    mPlaybackThreadAlive = false;
    resetPlaybackPathVariables();
    LOGD("Playback event Thread is dying.");
    return;
}

void AudioBroadcastStreamALSA::exitFromCaptureThread()
{
    LOGV("exitFromCapturePath");
    if (!mCaptureThreadAlive)
        return;

    mExitReadCapturePath = true;
    mKillCaptureThread = true;
    if(mCapturefd != -1) {
        uint64_t writeValue = KILL_CAPTURE_THREAD;
        LOGD("Writing to mCapturefd %d",mCapturefd);
        sys_broadcast::lib_write(mCapturefd, &writeValue, sizeof(uint64_t));
    }
    mCaptureCv.signal();
    pthread_join(mCaptureThread,NULL);
    LOGD("Capture thread killed");

    resetCapturePathVariables();
    return;
}

void AudioBroadcastStreamALSA::exitFromPlaybackThread()
{
    LOGV("exitFromPlaybackPath");
    if (!mPlaybackThreadAlive)
        return;

    mKillPlaybackThread = true;
    if(mPlaybackfd != -1) {
        LOGE("Writing to mPlaybackfd %d",mPlaybackfd);
        uint64_t writeValue = KILL_PLAYBACK_THREAD;
        sys_broadcast::lib_write(mPlaybackfd, &writeValue, sizeof(uint64_t));
    }
    mPlaybackCv.signal();
    pthread_join(mPlaybackThread,NULL);
    LOGD("Playback thread killed");

    resetPlaybackPathVariables();
    return;
}

void AudioBroadcastStreamALSA::resetCapturePathVariables()
{
    LOGV("resetCapturePathVariables");
    mAvail = 0;
    mFrames = 0;
    mX.frames = 0;
    if(mCapturefd != -1) {
        sys_broadcast::lib_close(mCapturefd);
        mCapturefd = -1;
    }
}

void AudioBroadcastStreamALSA::resetPlaybackPathVariables()
{
    LOGV("resetPlaybackPathVariables");
    if(mPlaybackfd != -1) {
        sys_broadcast::lib_close(mPlaybackfd);
        mPlaybackfd = -1;
    }
}
/******************************************************************************\
                      End - Thread
******************************************************************************/


/******************************************************************************\
                      Close capture and routing devices
******************************************************************************/
status_t AudioBroadcastStreamALSA::closeDevice(alsa_handle_t *pHandle)
{
    status_t status = NO_ERROR;
    LOGV("closeDevice: useCase %s", pHandle->useCase);
    if(pHandle) {
        status = mALSADevice->close(pHandle);
    }
    pHandle = NULL;
    return status;
}

void AudioBroadcastStreamALSA::bufferAlloc(alsa_handle_t *handle)
{
    void  *mem_buf = NULL;
    int i = 0;

    struct pcm * local_handle = (struct pcm *)handle->handle;
    int32_t nSize = local_handle->period_size;
    LOGV("number of input buffers = %d", mInputBufferCount);
    LOGV("memBufferAlloc calling with required size %d", nSize);
    for (i = 0; i < mInputBufferCount; i++) {
        mem_buf = (int32_t *)local_handle->addr + (nSize * i/sizeof(int));
        BuffersAllocated buf(mem_buf, nSize);
        memset(buf.memBuf, 0x0, nSize);
        mInputMemEmptyQueue.push_back(buf);
        mInputBufPool.push_back(buf);
        LOGD("MEM that is allocated - buffer is %x", (unsigned int)mem_buf);
    }
}

void AudioBroadcastStreamALSA::bufferDeAlloc()
{
    while (!mInputBufPool.empty()) {
        List<BuffersAllocated>::iterator it = mInputBufPool.begin();
        BuffersAllocated &memBuffer = *it;
        LOGD("Removing input buffer from Buffer Pool ");
        mInputBufPool.erase(it);
    }
}

/******************************************************************************
                               route output
******************************************************************************/
ssize_t AudioBroadcastStreamALSA::write_l(char *buffer, size_t bytes)
{
    if(bytes == 0)
        return bytes;

    snd_pcm_sframes_t n;
    size_t            sent = 0;
    status_t          status;
    int               period_size;
    char              *use_case;

    if ((mUseTunnelDecoder) && (mWMAConfigDataSet == false) &&
        (mFormat == AUDIO_FORMAT_WMA || mFormat == AUDIO_FORMAT_WMA_PRO)) {
        LOGV("Configuring the WMA params");
        status_t err = mALSADevice->setWMAParams(mCompreRxHandle,
                           (int *)buffer, bytes/sizeof(int));
        if (err) {
            LOGE("WMA param config failed");
            return BAD_VALUE;
        }
        err = openTunnelDevice(mDevices);
        if (err) {
            LOGE("opening of tunnel device failed");
            return BAD_VALUE;
        }
        mWMAConfigDataSet = true;
        return bytes;
    }
    if (mUseTunnelDecoder && mCompreRxHandle) {
        period_size = mCompreRxHandle->periodSize;

        mBitstreamSM->copyBitsreamToInternalBuffer((char *)buffer, bytes);

        if((mBitstreamSM->sufficientBitstreamToDecode(period_size) == true) ||
                (bytes == 0)) {
            if(bytes == 0) {
                bytes = mBitstreamSM->getInputBufferWritePtr() -
                            mBitstreamSM->getInputBufferPtr();
            } else {
                bytes = period_size;
            }
            writeToCompressedDriver((char *)mBitstreamSM->getInputBufferPtr(),
                                            bytes);
            mBitstreamSM->copyResidueBitstreamToStart(bytes);
        }
    } else if(mMS11Decoder != NULL) {
        if(mFormat == AUDIO_FORMAT_AAC ||
           mFormat == AUDIO_FORMAT_HE_AAC_V1 ||
           mFormat == AUDIO_FORMAT_AAC_ADIF ||
           mFormat == AUDIO_FORMAT_HE_AAC_V2) {
            if(mAacConfigDataSet == false) {
                if(mMS11Decoder->setAACConfig((unsigned char *)buffer,
                                     bytes) == true)
                    mAacConfigDataSet = true;
                return bytes;
            }
        }

        bool    continueDecode=false;
        size_t  bytesConsumedInDecode = 0;
        size_t  copyBytesMS11 = 0;
        char    *bufPtr;
        uint32_t outSampleRate=mSampleRate,outChannels=mChannels;

        mBitstreamSM->copyBitsreamToInternalBuffer((char *)buffer, bytes);

        do
        {
            continueDecode=false;
            if(mBitstreamSM->sufficientBitstreamToDecode(mMinBytesReqToDecode)
                                 == true) {
                bufPtr = mBitstreamSM->getInputBufferPtr();
                copyBytesMS11 = mBitstreamSM->bitStreamBufSize();

                mMS11Decoder->copyBitstreamToMS11InpBuf(bufPtr,copyBytesMS11);
                bytesConsumedInDecode = mMS11Decoder->streamDecode(
                                            &outSampleRate, &outChannels);
                mBitstreamSM->copyResidueBitstreamToStart(bytesConsumedInDecode);
            }

            if( (mSampleRate != outSampleRate) || (mChannels != outChannels)) {
                mSampleRate = outSampleRate;
                mChannels = outChannels;
                if(mPcmRxHandle && mRoutePcmAudio) {
                    status_t status = closeDevice(mPcmRxHandle);
                    if(status != NO_ERROR)
                        break;
                    status = openPcmDevice(mDevices);
                    if(status != NO_ERROR)
                        break;
                }
                mChannelStatusSet = false;
            }

            // copy the output of MS11 to HAL internal buffers for PCM and SPDIF
            if(mRoutePcmAudio) {
                bufPtr=mBitstreamSM->getOutputBufferWritePtr(PCM_2CH_OUT);
                copyBytesMS11 = mMS11Decoder->copyOutputFromMS11Buf(PCM_2CH_OUT,bufPtr);
                mBitstreamSM->setOutputBufferWritePtr(PCM_2CH_OUT,copyBytesMS11);
            }
            if(mRouteCompreToSpdif || mRouteCompreToHdmi) {
                bufPtr=mBitstreamSM->getOutputBufferWritePtr(SPDIF_OUT);
                copyBytesMS11 = mMS11Decoder->copyOutputFromMS11Buf(SPDIF_OUT,bufPtr);
                mBitstreamSM->setOutputBufferWritePtr(SPDIF_OUT,copyBytesMS11);
            }
            if(copyBytesMS11)
                continueDecode = true;

            if(mChannelStatusSet == false) {
                if(mRoutePcmToSpdif) {
                    if(!strncmp(mSpdifOutputFormat,"lpcm",
                                sizeof(mSpdifOutputFormat))) {
                        if(mALSADevice->get_linearpcm_channel_status(mSampleRate,
                                            mChannelStatus)) {
                            LOGE("channel status set error ");
                            return BAD_VALUE;
                        }
                        mALSADevice->setChannelStatus(mChannelStatus);
                    }
                } else if(mRouteCompreToSpdif) {
                    if(mALSADevice->get_compressed_channel_status(
                                   mBitstreamSM->getOutputBufferPtr(SPDIF_OUT),
                                   copyBytesMS11,
                                   mChannelStatus,AUDIO_PARSER_CODEC_AC3)) {
                        LOGE("channel status set error ");
                        return BAD_VALUE;
                    }
                    mALSADevice->setChannelStatus(mChannelStatus);
                }
                mChannelStatusSet = true;
            }

            if(mPcmRxHandle && mRoutePcmAudio) {
                period_size = mPcmRxHandle->periodSize;
                while(mBitstreamSM->sufficientSamplesToRender(PCM_2CH_OUT,
                                       period_size) == true) {
                    n = pcm_write(mPcmRxHandle->handle,
                             mBitstreamSM->getOutputBufferPtr(PCM_2CH_OUT),
                             period_size);
                    LOGE("pcm_write returned with %d", n);
                    if(n < 0) {
                        // Recovery is part of pcm_write. TODO split is later.
                        LOGE("pcm_write returned n < 0");
                        return static_cast<ssize_t>(n);
                    } else {
                        mFrameCount++;
                        sent += static_cast<ssize_t>((period_size));
                        mBitstreamSM->copyResidueOutputToStart(PCM_2CH_OUT,
                                          period_size);
                    }
                }
            }
            if(mCompreRxHandle && (mRouteCompreToSpdif||mRouteCompreToHdmi)) {
                period_size = mCompreRxHandle->periodSize;
                while(mBitstreamSM->sufficientSamplesToRender(SPDIF_OUT,
                                        period_size) == true) {
                    n = writeToCompressedDriver(
                            mBitstreamSM->getOutputBufferPtr(SPDIF_OUT),
                            period_size);
                    LOGV("pcm_write returned with %d", n);
                    if (n < 0) {
                        // Recovery is part of pcm_write. TODO split is later.
                        LOGE("pcm_write returned n < 0");
                        return static_cast<ssize_t>(n);
                    } else {
                        mBitstreamSM->copyResidueOutputToStart(SPDIF_OUT,
                                          period_size);
                    }
                }
            }
	} while( (continueDecode == true) &&
                 (mBitstreamSM->sufficientBitstreamToDecode(mMinBytesReqToDecode)
                      == true));
    } else {

        if(mPcmRxHandle && mRoutePcmAudio) {
            period_size = mPcmRxHandle->periodSize;

            mBitstreamSM->copyBitsreamToInternalBuffer((char *)buffer, bytes);

            while((mPcmRxHandle->handle) &&
                  (mBitstreamSM->sufficientBitstreamToDecode(period_size)
                                     == true)) {
                LOGE("Calling pcm_write - periodSize: %d",period_size);
                n = pcm_write(mPcmRxHandle->handle,
                              mBitstreamSM->getInputBufferPtr(),
                              period_size);
                LOGE("pcm_write returned with %d", n);
                if(n < 0) {
                    LOGE("pcm_write returned n < 0");
                    return static_cast<ssize_t>(n);
                }
                else {
                    mFrameCount++;
                    sent += static_cast<ssize_t>((period_size));
                    mBitstreamSM->copyResidueBitstreamToStart(period_size);
                }
            }
        }
    }
    return sent;
}

int32_t AudioBroadcastStreamALSA::writeToCompressedDriver(char *buffer, int bytes)
{
    LOGV("writeToCompressedDriver");
    int n = 0;
    mPlaybackCv.signal();
    mInputMemRequestMutex.lock();

    LOGV("write Empty Queue size() = %d, Filled Queue size() = %d ",
              mInputMemEmptyQueue.size(), mInputMemFilledQueue.size());
    if (mInputMemEmptyQueue.empty()) {
        LOGV("Write: waiting on mWriteCv");
        mLock.unlock();
        mWriteCv.wait(mInputMemRequestMutex);
        mLock.lock();
        if (mSkipWrite) {
            mSkipWrite = false;
            mInputMemRequestMutex.unlock();
            return 0;
        }
        LOGV("Write: received a signal to wake up");
    }

    List<BuffersAllocated>::iterator it = mInputMemEmptyQueue.begin();
    BuffersAllocated buf = *it;
    mInputMemEmptyQueue.erase(it);

    mInputMemRequestMutex.unlock();

    memcpy(buf.memBuf, buffer, bytes);

    buf.bytesToWrite = bytes;
    mInputMemResponseMutex.lock();
    mInputMemFilledQueue.push_back(buf);
    mInputMemResponseMutex.unlock();

    pcm * local_handle = (struct pcm *)mCompreRxHandle->handle;
    if(bytes != 0) {
        LOGV("PCM write start");
        n = pcm_write(local_handle, buf.memBuf, local_handle->period_size);
        LOGV("PCM write complete");
    }
    if (bytes < local_handle->period_size) {
        LOGD("Last buffer case");
        uint64_t writeValue = SIGNAL_PLAYBACK_THREAD;
        sys_broadcast::lib_write(mPlaybackfd, &writeValue, sizeof(uint64_t));

        //TODO : Is this code reqd - start seems to fail?
        if (ioctl(local_handle->fd, SNDRV_PCM_IOCTL_START) < 0)
            LOGE("AUDIO Start failed");
        else
            local_handle->start = 1;
    }

    return n;
}

}       // namespace android_audio_legacy
