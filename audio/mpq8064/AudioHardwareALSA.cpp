/* AudioHardwareALSA.cpp
 **
 ** Copyright 2008-2010 Wind River Systems
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

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>

#define LOG_TAG "AudioHardwareALSA"
//#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>
#include <sys/resource.h>
#include <pthread.h>
#include <sys/prctl.h>

#include "AudioHardwareALSA.h"

//#define OUTPUT_BUFFER_LOG
#ifdef OUTPUT_BUFFER_LOG
    FILE *outputBufferFile1;
    char outputfilename [50] = "/data/output_proxy";
    static int number = 0;
#endif

extern "C"
{
    //
    // Function for dlsym() to look up for creating a new AudioHardwareInterface.
    //
    android_audio_legacy::AudioHardwareInterface *createAudioHardware(void) {
        return android_audio_legacy::AudioHardwareALSA::create();
    }
}         // extern "C"

namespace android_audio_legacy
{

// ----------------------------------------------------------------------------

AudioHardwareInterface *AudioHardwareALSA::create() {
    return new AudioHardwareALSA();
}

AudioHardwareALSA::AudioHardwareALSA() :
    mALSADevice(0),mVoipStreamCount(0),mVoipMicMute(false)
{
    FILE *fp;
    char soundCardInfo[200];
    int err = 0;
    int codec_rev = 2;
    LOGD("hw_get_module(ALSA_HARDWARE_MODULE_ID) returned err %d", err);
    mALSADevice = new ALSADevice();
    mDeviceList.clear();
    mIsVoiceCallActive = 0;
    mIsFmActive = 0;
    mDevSettingsFlag = 0;
    mDevSettingsFlag |= TTY_OFF;
    mBluetoothVGS = false;

    if((fp = fopen("/proc/asound/cards","r")) == NULL) {
        LOGE("Cannot open /proc/asound/cards file to get sound card info");
    } else {
        while((fgets(soundCardInfo, sizeof(soundCardInfo), fp) != NULL)) {
            LOGV("SoundCardInfo %s", soundCardInfo);
            if (strstr(soundCardInfo, "msm8960-tabla1x-snd-card")) {
                codec_rev = 1;
                break;
            }
        }
        fclose(fp);
    }

    if (codec_rev == 1) {
        LOGV("Detected tabla 1.x sound card");
        snd_use_case_mgr_open(&mUcMgr, "snd_soc_msm");
    } else {
        LOGV("Detected tabla 2.x sound card");
        snd_use_case_mgr_open(&mUcMgr, "snd_soc_msm_2x");
    }

    if (mUcMgr < 0) {
        LOGE("Failed to open ucm instance: %d", errno);
    } else {
        LOGI("ucm instance opened: %u", (unsigned)mUcMgr);
    }
    mALSADevice->setDeviceList(&mDeviceList);
    mRouteAudioToA2dp = false;
    mA2dpDevice = NULL;
    mA2dpStream = NULL;
    mA2DPActiveUseCases = A2DPNone;
    mIsA2DPEnabled = false;
    mKillA2DPThread = false;
    mA2dpThreadAlive = false;
    mA2dpThread = NULL;
}

AudioHardwareALSA::~AudioHardwareALSA()
{

    stopA2dpPlayback_l(mA2DPActiveUseCases);

    if (mUcMgr != NULL) {
        LOGD("closing ucm instance: %u", (unsigned)mUcMgr);
        snd_use_case_mgr_close(mUcMgr);
    }
    if (mALSADevice) {
        delete mALSADevice;
    }
    for(ALSAHandleList::iterator it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
        it->useCase[0] = 0;
        mDeviceList.erase(it);
    }
}

status_t AudioHardwareALSA::initCheck()
{
    if (!mALSADevice)
        return NO_INIT;

    return NO_ERROR;
}

status_t AudioHardwareALSA::setVoiceVolume(float v)
{
    LOGD("setVoiceVolume(%f)\n", v);
    if (v < 0.0) {
        LOGW("setVoiceVolume(%f) under 0.0, assuming 0.0\n", v);
        v = 0.0;
    } else if (v > 1.0) {
        LOGW("setVoiceVolume(%f) over 1.0, assuming 1.0\n", v);
        v = 1.0;
    }

    int newMode = mode();
    LOGD("setVoiceVolume  newMode %d",newMode);
    int vol = lrint(v * 100.0);

    // Voice volume levels from android are mapped to driver volume levels as follows.
    // 0 -> 5, 20 -> 4, 40 ->3, 60 -> 2, 80 -> 1, 100 -> 0
    // So adjust the volume to get the correct volume index in driver
    vol = 100 - vol;

    // ToDo: Send mixer command only when voice call is active
    if(mALSADevice) {
        if(newMode == AudioSystem::MODE_IN_COMMUNICATION) {
            mALSADevice->setVoipVolume(vol);
        } else {
            mALSADevice->setVoiceVolume(vol);
        }
    }

    return NO_ERROR;
}

static const float dBPerStep = 0.5f;
static const float dBConvert = -dBPerStep * 2.302585093f / 20.0f;
static const float dBConvertInverse = 1.0f / dBConvert;

status_t  AudioHardwareALSA::setFmVolume(float value)
{
    status_t status = NO_ERROR;

    value = value?(value+0.005):value;
    int vol = value ? 100 - int(dBConvertInverse * log(value) + 0.5) : 0;

    if (vol > 100)
        vol = 100;
    else if (vol < 0)
        vol = 0;

    LOGD("setFmVolume(%f)\n", value);
    LOGD("Setting FM volume to %d (available range is 0 to 100)\n", vol);

    mALSADevice->setFmVolume(vol);

    return status;
}

status_t AudioHardwareALSA::setMasterVolume(float volume)
{
    return NO_ERROR;
}

status_t AudioHardwareALSA::setMode(int mode)
{
    status_t status = NO_ERROR;

    if (mode != mMode) {
        status = AudioHardwareBase::setMode(mode);
    }

    return status;
}

status_t AudioHardwareALSA::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key;
    String8 value;
    status_t status = NO_ERROR;
    int device;
    int btRate;
    LOGD("setParameters() %s", keyValuePairs.string());

    key = String8(TTY_MODE_KEY);
    if (param.get(key, value) == NO_ERROR) {
        mDevSettingsFlag &= TTY_CLEAR;
        if (value == "full") {
            mDevSettingsFlag |= TTY_FULL;
        } else if (value == "hco") {
            mDevSettingsFlag |= TTY_HCO;
        } else if (value == "vco") {
            mDevSettingsFlag |= TTY_VCO;
        } else {
            mDevSettingsFlag |= TTY_OFF;
        }
        LOGI("Changed TTY Mode=%s", value.string());
        mALSADevice->setFlags(mDevSettingsFlag);
        if(mMode != AudioSystem::MODE_IN_CALL){
           return NO_ERROR;
        }
        doRouting(0);
    }

    key = String8(FLUENCE_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "quadmic") {
            mDevSettingsFlag |= QMIC_FLAG;
            mDevSettingsFlag &= (~DMIC_FLAG);
            LOGV("Fluence quadMic feature Enabled");
        } else if (value == "dualmic") {
            mDevSettingsFlag |= DMIC_FLAG;
            mDevSettingsFlag &= (~QMIC_FLAG);
            LOGV("Fluence dualmic feature Enabled");
        } else if (value == "none") {
            mDevSettingsFlag &= (~DMIC_FLAG);
            mDevSettingsFlag &= (~QMIC_FLAG);
            LOGV("Fluence feature Disabled");
        }
        mALSADevice->setFlags(mDevSettingsFlag);
        doRouting(0);
    }

    key = String8(ANC_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "true") {
            LOGV("Enabling ANC setting in the setparameter\n");
            mDevSettingsFlag |= ANC_FLAG;
        } else {
            LOGV("Disabling ANC setting in the setparameter\n");
            mDevSettingsFlag &= (~ANC_FLAG);
        }
        mALSADevice->setFlags(mDevSettingsFlag);
        doRouting(0);
    }

    key = String8(AudioParameter::keyRouting);
    if (param.getInt(key, device) == NO_ERROR) {
        // Ignore routing if device is 0.
        if(device) {
            doRouting(device);
        }
        param.remove(key);
    }

    key = String8(BT_SAMPLERATE_KEY);
    if (param.getInt(key, btRate) == NO_ERROR) {
        mALSADevice->setBtscoRate(btRate);
        param.remove(key);
    }

    key = String8(BTHEADSET_VGS);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "on") {
            mBluetoothVGS = true;
        } else {
            mBluetoothVGS = false;
        }
    }

    key = String8(WIDEVOICE_KEY);
    if (param.get(key, value) == NO_ERROR) {
        bool flag = false;
        if (value == "true") {
            flag = true;
        }
        if(mALSADevice) {
            mALSADevice->enableWideVoice(flag);
        }
        param.remove(key);
    }

    key = String8(FENS_KEY);
    if (param.get(key, value) == NO_ERROR) {
        bool flag = false;
        if (value == "true") {
            flag = true;
        }
        if(mALSADevice) {
            mALSADevice->enableFENS(flag);
        }
        param.remove(key);
    }

    key = String8(SPDIF_FORMAT_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if(value == "lpcm" || value == "ac3" || value == "dts")
            strlcpy(mSpdifOutputFormat,value,sizeof(mSpdifOutputFormat));
        else
            strlcpy(mSpdifOutputFormat,"lpcm",sizeof(mSpdifOutputFormat));
        param.remove(key);
    }

    key = String8(HDMI_FORMAT_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if(value == "lpcm" || value == "ac3" || value == "dts")
            strlcpy(mHdmiOutputFormat,value,sizeof(mHdmiOutputFormat));
        else
            strlcpy(mHdmiOutputFormat,"lpcm",sizeof(mHdmiOutputFormat));
        param.remove(key);
    }

    if (param.size()) {
        status = BAD_VALUE;
    }
    return status;
}

String8 AudioHardwareALSA::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    String8 value;

    String8 key = String8(DUALMIC_KEY);
    if (param.get(key, value) == NO_ERROR) {
        value = String8("false");
        param.add(key, value);
    }

    key = String8(FLUENCE_KEY);
    if (param.get(key, value) == NO_ERROR) {
	if ((mDevSettingsFlag & QMIC_FLAG) &&
                               (mDevSettingsFlag & ~DMIC_FLAG))
            value = String8("quadmic");
	else if ((mDevSettingsFlag & DMIC_FLAG) &&
                                (mDevSettingsFlag & ~QMIC_FLAG))
            value = String8("dualmic");
	else if ((mDevSettingsFlag & ~DMIC_FLAG) &&
                                (mDevSettingsFlag & ~QMIC_FLAG))
            value = String8("none");
        param.add(key, value);
    }

    key = String8("Fm-radio");
    if ( param.get(key,value) == NO_ERROR ) {
        if ( mIsFmActive ) {
            param.addInt(String8("isFMON"), true );
        }
    }

    key = String8(BTHEADSET_VGS);
    if (param.get(key, value) == NO_ERROR) {
        if(mBluetoothVGS)
           param.addInt(String8("isVGS"), true);
    }

    LOGV("AudioHardwareALSA::getParameters() %s", param.toString().string());
    return param.toString();
}

status_t AudioHardwareALSA::doRouting(int device)
{
    Mutex::Autolock autoLock(mLock);
    int newMode = mode();
    bool bIsUseCaseSet = false;
    if ((device == AudioSystem::DEVICE_IN_VOICE_CALL) ||
        (device == AudioSystem::DEVICE_IN_FM_RX) ||
        (device == AudioSystem::DEVICE_OUT_DIRECTOUTPUT) ||
        (device == AudioSystem::DEVICE_IN_COMMUNICATION)) {
        LOGV("Ignoring routing for FM/INCALL/VOIP recording");
        return NO_ERROR;
    }
    LOGV("device = 0x%x,mCurDevice 0x%x", device, mCurDevice);
    if (device == 0)
        device = mCurDevice;
    LOGV("doRouting: device %d newMode %d mIsVoiceCallActive %d mIsFmActive %d",
          device, newMode, mIsVoiceCallActive, mIsFmActive);
    if((newMode == AudioSystem::MODE_IN_CALL) && (mIsVoiceCallActive == 0)) {
        // Start voice call
        unsigned long bufferSize = DEFAULT_BUFFER_SIZE;
        alsa_handle_t alsa_handle;
        char *use_case;
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
                                          strlen(SND_USE_CASE_VERB_INACTIVE)))) {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_VOICECALL, sizeof(alsa_handle.useCase));
            bIsUseCaseSet = true;
        } else {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_VOICE, sizeof(alsa_handle.useCase));
        }
        if(use_case) {
            free(use_case);
            use_case = NULL;
        }

        for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;
        alsa_handle.module = mALSADevice;
        alsa_handle.bufferSize = bufferSize;
        alsa_handle.devices = device;
        alsa_handle.activeDevice = device;
        alsa_handle.handle = 0;
        alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
        alsa_handle.channels = VOICE_CHANNEL_MODE;
        alsa_handle.sampleRate = VOICE_SAMPLING_RATE;
        alsa_handle.latency = VOICE_LATENCY;
        alsa_handle.rxHandle = 0;
        alsa_handle.mode = mode();
        alsa_handle.ucMgr = mUcMgr;
        mIsVoiceCallActive = 1;
        mDeviceList.push_back(alsa_handle);
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        LOGV("Enabling voice call");
        mALSADevice->setUseCase(&(*it), bIsUseCaseSet);
        mALSADevice->startVoiceCall(&(*it));
    } else if(newMode == AudioSystem::MODE_NORMAL && mIsVoiceCallActive == 1) {
        // End voice call
        for(ALSAHandleList::iterator it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
            if((!strncmp(it->useCase, SND_USE_CASE_VERB_VOICECALL,
                                strlen(SND_USE_CASE_VERB_VOICECALL))) ||
               (!strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_VOICE,
                               strlen(SND_USE_CASE_MOD_PLAY_VOICE)))) {
                LOGV("Disabling voice call");
                mALSADevice->close(&(*it));
        // DO WE HAVE TO DO A ROUTE HERE ? REASON - is it for speaker Mic to speaker
                mALSADevice->route((uint32_t)device, newMode);
                mDeviceList.erase(it);
                break;
            }
        }
        mIsVoiceCallActive = 0;
    } else if(device & AudioSystem::DEVICE_OUT_FM && mIsFmActive == 0) {
        // Start FM Radio on current active device
        unsigned long bufferSize = FM_BUFFER_SIZE;
        alsa_handle_t alsa_handle;
        char *use_case;
        LOGV("Start FM");
        if (device & AudioSystem::DEVICE_OUT_ALL_A2DP) {
            device &= ~AudioSystem::DEVICE_OUT_ALL_A2DP;
            device |=  AudioSystem::DEVICE_OUT_PROXY;
            mRouteAudioToA2dp = true;
            LOGV("Routing FM to proxy now");
        }
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
                                         strlen(SND_USE_CASE_VERB_INACTIVE)))) {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_DIGITAL_RADIO, sizeof(alsa_handle.useCase));
            bIsUseCaseSet = true;
        } else {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_FM, sizeof(alsa_handle.useCase));
        }
        if(use_case) {
            free(use_case);
            use_case = NULL;
        }

        for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;
        alsa_handle.module = mALSADevice;
        alsa_handle.bufferSize = bufferSize;
        alsa_handle.devices = device;
        alsa_handle.activeDevice= device;
        alsa_handle.handle = 0;
        alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
        alsa_handle.channels = DEFAULT_CHANNEL_MODE;
        alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
        alsa_handle.latency = VOICE_LATENCY;
        alsa_handle.rxHandle = 0;
        alsa_handle.mode = mode();
        alsa_handle.ucMgr = mUcMgr;
        mIsFmActive = 1;
        mDeviceList.push_back(alsa_handle);
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;

        mALSADevice->setUseCase(&(*it), bIsUseCaseSet);
        if((device & AudioSystem::DEVICE_OUT_PROXY) && mRouteAudioToA2dp) {
            LOGD("FM A2DP writer needs to be disabled in frameworks");
            LOGD("doRouting-startA2dpPlayback_l-FM");
            status_t err = startA2dpPlayback_l(AudioHardwareALSA::A2DPFm);
            if(err) {
                LOGW("startA2dpPlayback for FM failed err = %d", err);
                stopA2dpPlayback_l(AudioHardwareALSA::A2DPFm);
                alsa_handle.devices = mCurDevice;
                alsa_handle.activeDevice = mCurDevice;
                mALSADevice->setUseCase(&(*it), bIsUseCaseSet);
                return err;
            }
        }
        mALSADevice->startFm(&(*it));
    } else if(!(device & AudioSystem::DEVICE_OUT_FM) && mIsFmActive == 1) {
        // Stop FM Radio
        LOGV("Stop FM");
        if(!device & AudioSystem::DEVICE_OUT_ALL_A2DP && mRouteAudioToA2dp == true) {
            device &= ~ AudioSystem::DEVICE_OUT_PROXY;
            LOGD("doRouting-stopA2dpPlayback_l-FM");
            status_t err = stopA2dpPlayback_l(AudioHardwareALSA::A2DPFm);
            if(err) {
                LOGW("stopA2dpPlayback_l for FM failed err = %d", err);
                return err;
            }
            mRouteAudioToA2dp = false;
        }
        for(ALSAHandleList::iterator it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
            if((!strncmp(it->useCase, SND_USE_CASE_VERB_DIGITAL_RADIO,
                                strlen(SND_USE_CASE_VERB_DIGITAL_RADIO))) ||
              (!strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_FM,
                               strlen(SND_USE_CASE_MOD_PLAY_FM)))) {
                mALSADevice->close(&(*it));
        // DO WE HAVE TO DO A ROUTE HERE ? REASON ?
                mALSADevice->route((uint32_t)device, newMode);
                mDeviceList.erase(it);
                break;
            }
        }
        mIsFmActive = 0;
    } else if (device & AudioSystem::DEVICE_OUT_PROXY &&
            mRouteAudioToA2dp == true && device != mCurDevice)  {
        LOGV(" A2DP Enabled - Routing everything to proxy now");
        mALSADevice->route((uint32_t)device, newMode);
        status_t err = NO_ERROR;
        LOGD("doRouting-startA2dpPlayback_l-A2DPHardwareOutput-enable");
        err = startA2dpPlayback_l(AudioHardwareALSA::A2DPHardwareOutput);
        if(err) {
            LOGW("startA2dpPlayback_l for hardware output failed err = %d", err);
            stopA2dpPlayback_l(AudioHardwareALSA::A2DPHardwareOutput);
            mRouteAudioToA2dp = false;
            mALSADevice->route((uint32_t)mCurDevice, newMode);
            return err;
        }
    } else if(!(device & AudioSystem::DEVICE_OUT_PROXY) &&
        mRouteAudioToA2dp == true && device != mCurDevice) {
        LOGV(" A2DP Disable on hardware output");
        LOGD("doRouting-stopA2dpPlayback_l-A2DPHardwareOutput-disable");
        status_t err = stopA2dpPlayback_l(AudioHardwareALSA::A2DPHardwareOutput);
        if(err) {
            LOGW("stop A2dp playback for hardware output failed = %d", err);
            return err;
        }
        mRouteAudioToA2dp = false;
        mALSADevice->route((uint32_t)device, newMode);
    } else {
        // ToDo: to use snd_use_case_set_case API here
        // HANDLING ONLY USE CASE WHICH ARE NOT AT OTHER DOROUTING
        // CONFIRM ?
        if(device != mCurDevice)
            mALSADevice->route((uint32_t)device, newMode);
    }
    mCurDevice = device;
    return NO_ERROR;
}

AudioBroadcastStream *
AudioHardwareALSA::openBroadcastStream(uint32_t  devices,
                                       int      format,
                                       uint32_t channels,
                                       uint32_t sampleRate,
                                       uint32_t audioSource,
                                       status_t *status)
{
    Mutex::Autolock autoLock(mLock);
    status_t err = BAD_VALUE;
    AudioBroadcastStreamALSA *out = 0;

    // 1. Check for validity of the output devices
    if(devices == 0) {
        LOGE("openBroadcastStream:: No output device specified");
        if (status) *status = err;
        return out;
    }
#if 0
    // 2. Check if the output devices contain SPDIF also
    if(devices & AudioSystem::DEVICE_OUT_SPDIF && mIsSpdifDeviceBusy) {
        LOGW("SPDIF device is busy, removing the SPDIF from the list ");
        devices &= ~AudioSystem::DEVICE_OUT_SPDIF;
    }        
#endif
    out = new AudioBroadcastStreamALSA(this, devices, format, channels, 
                                       sampleRate, audioSource, &err);
    if(err != OK) {
        delete out;
        out = NULL;
    }
    if (status) *status = err;
    return out;
}

void
AudioHardwareALSA::closeBroadcastStream(AudioBroadcastStream* out)
{
    delete out;
}

AudioStreamOut *
AudioHardwareALSA::openOutputStream(uint32_t devices,
                                    int *format,
                                    uint32_t *channels,
                                    uint32_t *sampleRate,
                                    status_t *status)
{
    bool bIsUseCaseSet = false;
    Mutex::Autolock autoLock(mLock);
    LOGD("openOutputStream: devices 0x%x channels %d sampleRate %d",
         devices, *channels, *sampleRate);

    status_t err = BAD_VALUE;
    AudioStreamOutALSA *out = 0;
    ALSAHandleList::iterator it;

    if (devices & (devices - 1)) {
        if (status) *status = err;
        LOGE("openOutputStream called with bad devices");
        return out;
    }

    if(devices & AudioSystem::DEVICE_OUT_ALL_A2DP) {
        LOGE("Set Capture from proxy true");
        devices &= ~AudioSystem::DEVICE_OUT_ALL_A2DP;
        devices |=  AudioSystem::DEVICE_OUT_PROXY;
        mRouteAudioToA2dp = true;

    }
    if(devices == AudioSystem::DEVICE_OUT_DIRECTOUTPUT) {
        bool voipstream_active = false;
        for(it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
                if((!strncmp(it->useCase, SND_USE_CASE_VERB_IP_VOICECALL,
                                    strlen(SND_USE_CASE_VERB_IP_VOICECALL))) ||
                   (!strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_VOIP,
                                    strlen(SND_USE_CASE_MOD_PLAY_VOIP)))) {
                    LOGD("openOutput:  it->rxHandle %d it->handle %d",it->rxHandle,it->handle);
                    voipstream_active = true;
                    break;
                }
        }

      if(voipstream_active == false) {
         mVoipStreamCount = 0;
         mVoipMicMute = false;
         alsa_handle_t alsa_handle;
         unsigned long bufferSize;
         if(*sampleRate == VOIP_SAMPLING_RATE_8K) {
             bufferSize = VOIP_BUFFER_SIZE_8K;
         }
         else if(*sampleRate == VOIP_SAMPLING_RATE_16K) {
             bufferSize = VOIP_BUFFER_SIZE_16K;
         }
         else {
             LOGE("unsupported samplerate %d for voip",*sampleRate);
             if (status) *status = err;
                 return out;
          }
          alsa_handle.module = mALSADevice;
          alsa_handle.bufferSize = bufferSize;
          alsa_handle.devices = devices;
          alsa_handle.activeDevice = devices;
          alsa_handle.handle = 0;
          alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
          alsa_handle.channels = VOIP_DEFAULT_CHANNEL_MODE;
          alsa_handle.sampleRate = *sampleRate;
          alsa_handle.latency = VOIP_PLAYBACK_LATENCY;
          alsa_handle.rxHandle = 0;
          alsa_handle.mode = AudioSystem::MODE_IN_COMMUNICATION;
          alsa_handle.ucMgr = mUcMgr;
          char *use_case;
          snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
          if ((use_case == NULL) || (!strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
                                         strlen(SND_USE_CASE_VERB_INACTIVE)))) {
              strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_IP_VOICECALL, sizeof(alsa_handle.useCase));
              bIsUseCaseSet = true;
          } else {
              strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_VOIP, sizeof(alsa_handle.useCase));
          }
          if(use_case) {
              free(use_case);
              use_case = NULL;
          }
          mDeviceList.push_back(alsa_handle);
          it = mDeviceList.end();
          it--;
          LOGV("openoutput: mALSADevice->route useCase %s mCurDevice %d mVoipStreamCount %d mode %d", it->useCase,mCurDevice,mVoipStreamCount, mode());
          mALSADevice->setUseCase(&(*it), bIsUseCaseSet);
          err = mALSADevice->startVoipCall(&(*it));
          if (err) {
              LOGE("Device open failed");
              return NULL;
          }
      }
      out = new AudioStreamOutALSA(this, &(*it));
      err = out->set(format, channels, sampleRate, devices);
      if(err == NO_ERROR) {
          mVoipStreamCount++;   //increment VoipstreamCount only if success
          LOGD("openoutput mVoipStreamCount %d",mVoipStreamCount);
      }
      if (status) *status = err;
      return out;
    } else {
      alsa_handle_t alsa_handle;
      unsigned long bufferSize = DEFAULT_BUFFER_SIZE;

      for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
          bufferSize &= ~b;

      alsa_handle.module = mALSADevice;
      alsa_handle.bufferSize = bufferSize;
      alsa_handle.devices = devices;
      alsa_handle.activeDevice = devices;
      alsa_handle.handle = 0;
      alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
      alsa_handle.channels = DEFAULT_CHANNEL_MODE;
      alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
      alsa_handle.latency = PLAYBACK_LATENCY;
      alsa_handle.rxHandle = 0;
      alsa_handle.mode = mode();
      alsa_handle.ucMgr = mUcMgr;

      char *use_case;
      snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
      if ((use_case == NULL) || (!strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
                                     strlen(SND_USE_CASE_VERB_INACTIVE)))) {
          strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI, sizeof(alsa_handle.useCase));
          bIsUseCaseSet = true;
      } else {
          strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_MUSIC, sizeof(alsa_handle.useCase));
      }
      if(use_case) {
          free(use_case);
          use_case = NULL;
      }
      mDeviceList.push_back(alsa_handle);
      ALSAHandleList::iterator it = mDeviceList.end();
      it--;
      LOGD("useCase %s", it->useCase);
      mALSADevice->setUseCase(&(*it),bIsUseCaseSet);
      err = mALSADevice->open(&(*it));
      if (err) {
          LOGE("Device open failed");
      } else {
          out = new AudioStreamOutALSA(this, &(*it));
          err = out->set(format, channels, sampleRate, devices);
      }

      if (status) *status = err;
      return out;
    }
}

void
AudioHardwareALSA::closeOutputStream(AudioStreamOut* out)
{
    delete out;
}

AudioStreamOut *
AudioHardwareALSA::openOutputSession(uint32_t devices,
                                     int *format,
                                     status_t *status,
                                     int sessionId,
                                     uint32_t samplingRate, 
                                     uint32_t channels)
{
    Mutex::Autolock autoLock(mLock);
    LOGD("openOutputSession");
    status_t err = BAD_VALUE;
    AudioSessionOutALSA *out = new AudioSessionOutALSA(this, devices, *format, channels,
                                                       samplingRate, sessionId, &err);
    if(err != NO_ERROR) {
        delete out;
        out = NULL;
    }
    if (status) *status = err;
    return out;
}

void
AudioHardwareALSA::closeOutputSession(AudioStreamOut* out)
{
    delete out;
}

AudioStreamIn *
AudioHardwareALSA::openInputStream(uint32_t devices,
                                   int *format,
                                   uint32_t *channels,
                                   uint32_t *sampleRate,
                                   status_t *status,
                                   AudioSystem::audio_in_acoustics acoustics)
{
    Mutex::Autolock autoLock(mLock);
    char *use_case;
    bool bIsUseCaseSet = false;
    int newMode = mode();
    uint32_t route_devices;

    status_t err = BAD_VALUE;
    AudioStreamInALSA *in = 0;
    ALSAHandleList::iterator it;

    LOGD("openInputStream: devices 0x%x channels %d sampleRate %d", devices, *channels, *sampleRate);
    if (devices & (devices - 1)) {
        if (status) *status = err;
        return in;
    }

    if((devices == AudioSystem::DEVICE_IN_COMMUNICATION) )  {
        bool voipstream_active = false;
        for(it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
                if((!strncmp(it->useCase, SND_USE_CASE_VERB_IP_VOICECALL,
                          strlen(SND_USE_CASE_VERB_IP_VOICECALL))) ||
                   (!strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_VOIP,
                          strlen(SND_USE_CASE_MOD_PLAY_VOIP)))) {
                    LOGD("openInput:  it->rxHandle %d it->handle %d",it->rxHandle,it->handle);
                    voipstream_active = true;
                    break;
                }
        }
        if(voipstream_active == false) {
           mVoipStreamCount = 0;
           mVoipMicMute = false;
           alsa_handle_t alsa_handle;
           unsigned long bufferSize;
           if(*sampleRate == VOIP_SAMPLING_RATE_8K) {
               bufferSize = VOIP_BUFFER_SIZE_8K;
           }
           else if(*sampleRate == VOIP_SAMPLING_RATE_16K) {
               bufferSize = VOIP_BUFFER_SIZE_16K;
           }
           else {
               LOGE("unsupported samplerate %d for voip",*sampleRate);
               if (status) *status = err;
               return in;
           }
           alsa_handle.module = mALSADevice;
           alsa_handle.bufferSize = bufferSize;
           alsa_handle.devices = devices;
           alsa_handle.activeDevice = devices;
           alsa_handle.handle = 0;
           alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
           alsa_handle.channels = VOIP_DEFAULT_CHANNEL_MODE;
           alsa_handle.sampleRate = *sampleRate;
           alsa_handle.latency = VOIP_RECORD_LATENCY;
           alsa_handle.rxHandle = 0;
           alsa_handle.mode = AudioSystem::MODE_IN_COMMUNICATION;
           alsa_handle.ucMgr = mUcMgr;
           snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
           if ((use_case != NULL) && (strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
                                          strlen(SND_USE_CASE_VERB_INACTIVE)))) {
                strcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_VOIP);
                bIsUseCaseSet = false;
           } else {
                strcpy(alsa_handle.useCase, SND_USE_CASE_VERB_IP_VOICECALL);
           }
           if(use_case) {
               free(use_case);
               use_case = NULL;
           }
           mDeviceList.push_back(alsa_handle);
           it = mDeviceList.end();
           it--;
           mALSADevice->setUseCase(&(*it), bIsUseCaseSet);
           if(sampleRate) {
               it->sampleRate = *sampleRate;
           }
           if(channels)
               it->channels = AudioSystem::popCount(*channels);
           err = mALSADevice->startVoipCall(&(*it));
           if (err) {
               LOGE("Error opening pcm input device");
               return NULL;
           }
        }
        in = new AudioStreamInALSA(this, &(*it), acoustics);
        err = in->set(format, channels, sampleRate, devices);
        if(err == NO_ERROR) {
            mVoipStreamCount++;   //increment VoipstreamCount only if success
            LOGD("OpenInput mVoipStreamCount %d",mVoipStreamCount);
        }
        LOGE("openInput: After Get alsahandle");
        if (status) *status = err;
        return in;
      } else
      {
        for(ALSAHandleList::iterator itDev = mDeviceList.begin();
              itDev != mDeviceList.end(); ++itDev)
        {
            if((0 == strncmp(itDev->useCase, SND_USE_CASE_VERB_HIFI_REC, MAX_UC_LEN))
              ||(0 == strncmp(itDev->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC, MAX_UC_LEN))
              ||(0 == strncmp(itDev->useCase, SND_USE_CASE_MOD_CAPTURE_FM, MAX_UC_LEN))
              ||(0 == strncmp(itDev->useCase, SND_USE_CASE_VERB_FM_REC, MAX_UC_LEN)))
            {
                LOGD("Input stream already exists, new stream not permitted: useCase:%s, devices:0x%x, module:%p", itDev->useCase, itDev->devices, itDev->module);
                return in;
            }
        }
        alsa_handle_t alsa_handle;
        unsigned long bufferSize = DEFAULT_IN_BUFFER_SIZE;

        alsa_handle.module = mALSADevice;
        alsa_handle.bufferSize = bufferSize;
        alsa_handle.devices = devices;
        alsa_handle.activeDevice = devices;
        alsa_handle.handle = 0;
        alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
        alsa_handle.channels = VOICE_CHANNEL_MODE;
        alsa_handle.sampleRate = android::AudioRecord::DEFAULT_SAMPLE_RATE;
        alsa_handle.latency = RECORD_LATENCY;
        alsa_handle.rxHandle = 0;
        alsa_handle.mode = newMode;
        alsa_handle.ucMgr = mUcMgr;
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case != NULL) && (strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
                                      strlen(SND_USE_CASE_VERB_INACTIVE)))) {
            if ((devices == AudioSystem::DEVICE_IN_VOICE_CALL) &&
                (newMode == AudioSystem::MODE_IN_CALL)) {
                LOGD("openInputStream: into incall recording, channels %d", *channels);
                mIncallMode = *channels;
                if ((*channels & AudioSystem::CHANNEL_IN_VOICE_UPLINK) &&
                    (*channels & AudioSystem::CHANNEL_IN_VOICE_DNLINK)) {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_VOICE_UL_DL,
                            sizeof(alsa_handle.useCase));
                } else if (*channels & AudioSystem::CHANNEL_IN_VOICE_DNLINK) {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_VOICE_DL,
                            sizeof(alsa_handle.useCase));
                }
            } else if((devices == AudioSystem::DEVICE_IN_FM_RX)) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_FM, sizeof(alsa_handle.useCase));
            } else if(devices == AudioSystem::DEVICE_IN_FM_RX_A2DP) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_A2DP_FM, sizeof(alsa_handle.useCase));
            } else {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC, sizeof(alsa_handle.useCase));
            }
        } else {
            if ((devices == AudioSystem::DEVICE_IN_VOICE_CALL) &&
                (newMode == AudioSystem::MODE_IN_CALL)) {
                LOGD("openInputStream: incall recording, channels %d", *channels);
                mIncallMode = *channels;
                if ((*channels & AudioSystem::CHANNEL_IN_VOICE_UPLINK) &&
                    (*channels & AudioSystem::CHANNEL_IN_VOICE_DNLINK)) {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_UL_DL_REC, sizeof(alsa_handle.useCase));
                } else if (*channels & AudioSystem::CHANNEL_IN_VOICE_DNLINK) {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_DL_REC, sizeof(alsa_handle.useCase));
                }
            } else if(devices == AudioSystem::DEVICE_IN_FM_RX) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_FM_REC, sizeof(alsa_handle.useCase));
            } else if (devices == AudioSystem::DEVICE_IN_FM_RX_A2DP) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_FM_A2DP_REC, sizeof(alsa_handle.useCase));
            } else {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_REC, sizeof(alsa_handle.useCase));
            }
            bIsUseCaseSet = true;
        }
        if(use_case) {
            free(use_case);
            use_case = NULL;
        }
        mDeviceList.push_back(alsa_handle);
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        if (devices == AudioSystem::DEVICE_IN_VOICE_CALL){
           /* Add current devices info to devices to do route */
            route_devices = devices | mCurDevice;
        } else {
            route_devices = devices;
        }
        it->devices = route_devices;
        // NOTE: NEED CLARIFICATION to update route_device in the handle ???
        mALSADevice->setUseCase(&(*it),bIsUseCaseSet);
        if(sampleRate) {
            it->sampleRate = *sampleRate;
        }
        if(channels) {
            it->channels = AudioSystem::popCount((*channels) &
                      (AudioSystem::CHANNEL_IN_STEREO |
                       AudioSystem::CHANNEL_IN_MONO));
            LOGD("channels %d", it->channels);
        }
        err = mALSADevice->open(&(*it));
        if (err) {
           LOGE("Error opening pcm input device");
        } else {
           in = new AudioStreamInALSA(this, &(*it), acoustics);
           err = in->set(format, channels, sampleRate, devices);
        }
        if (status) *status = err;
        return in;
      }
}

void
AudioHardwareALSA::closeInputStream(AudioStreamIn* in)
{
    delete in;
}

status_t AudioHardwareALSA::setMicMute(bool state)
{
    int newMode = mode();
    LOGD("setMicMute  newMode %d",newMode);
    if(newMode == AudioSystem::MODE_IN_COMMUNICATION) {
        if (mVoipMicMute != state) {
             mVoipMicMute = state;
            LOGD("setMicMute: mVoipMicMute %d", mVoipMicMute);
            if(mALSADevice) {
                mALSADevice->setVoipMicMute(state);
            }
        }
    } else {
        if (mMicMute != state) {
              mMicMute = state;
              LOGD("setMicMute: mMicMute %d", mMicMute);
              if(mALSADevice) {
                 mALSADevice->setMicMute(state);
              }
        }
    }
    return NO_ERROR;
}

status_t AudioHardwareALSA::getMicMute(bool *state)
{
    int newMode = mode();
    if(newMode == AudioSystem::MODE_IN_COMMUNICATION) {
        *state = mVoipMicMute;
    } else {
        *state = mMicMute;
    }
    return NO_ERROR;
}

status_t AudioHardwareALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

size_t AudioHardwareALSA::getInputBufferSize(uint32_t sampleRate, int format, int channelCount)
{
    size_t bufferSize;
    if (format != AudioSystem::PCM_16_BIT) {
         LOGW("getInputBufferSize bad format: %d", format);
         return 0;
    }
    if(sampleRate == 16000) {
        bufferSize = DEFAULT_IN_BUFFER_SIZE * 2 * channelCount;
    } else if(sampleRate < 44100) {
        bufferSize = DEFAULT_IN_BUFFER_SIZE * channelCount;
    } else {
        bufferSize = DEFAULT_IN_BUFFER_SIZE * 12;
    }
    return bufferSize;
}

status_t AudioHardwareALSA::startA2dpPlayback(uint32_t activeUsecase) {

    Mutex::Autolock autoLock(mLock);
    status_t err = startA2dpPlayback_l(activeUsecase);
    if(err) {
        LOGE("startA2dpPlayback_l  = %d", err);
    }
    return err;
}
status_t AudioHardwareALSA::startA2dpPlayback_l(uint32_t activeUsecase) {

    LOGV("startA2dpPlayback_l");
    status_t err = NO_ERROR;
    if(!getA2DPActiveUseCases_l() && !mIsA2DPEnabled) {
        setA2DPActiveUseCases_l(activeUsecase);
        {
            Mutex::Autolock autolock1(mA2dpMutex);
            err = mALSADevice->openProxyDevice();
            if(err) {
                LOGE("openProxyDevice failed = %d", err);
            }

            err = openA2dpOutput();
            if(err) {
                LOGE("openA2DPOutput failed = %d",err);
                return err;
            }

            mKillA2DPThread = false;
            err = pthread_create(&mA2dpThread, (const pthread_attr_t *) NULL,
                    a2dpThreadWrapper,
                    this);
            if(err) {
                LOGE("thread create failed = %d", err);
                return err;
            }
            mA2dpThreadAlive = true;
            mIsA2DPEnabled = true;

#ifdef OUTPUT_BUFFER_LOG
    sprintf(outputfilename, "%s%d%s", outputfilename, number,".pcm");
    outputBufferFile1 = fopen (outputfilename, "ab");
    number++;
#endif

        }
    }
    setA2DPActiveUseCases_l(activeUsecase);
    {
        //Mutex::Autolock autolock1(mA2dpMutex);
        mALSADevice->resumeProxy();
    }
    LOGV("A2DP signal");
    mA2dpCv.signal();
    return err;
}

status_t AudioHardwareALSA::stopA2dpPlayback(uint32_t activeUsecase) {
     Mutex::Autolock autoLock(mLock);
     status_t err = stopA2dpPlayback_l(activeUsecase);
     if(err) {
         LOGE("stopA2dpPlayback = %d", err);
     }
     return err;
}

status_t AudioHardwareALSA::stopA2dpPlayback_l(uint32_t activeUsecase) {

     LOGV("stopA2dpPlayback  = %d", activeUsecase);
     status_t err = NO_ERROR;
     suspendA2dpPlayback_l(activeUsecase);
     {
         Mutex::Autolock autolock1(mA2dpMutex);
         LOGV("stopA2dpPlayback  getA2DPActiveUseCases_l = %d,\
                 mIsA2DPEnabled = %d",\
                 getA2DPActiveUseCases_l(),\
                 mIsA2DPEnabled);

         if(!getA2DPActiveUseCases_l()) {
             mIsA2DPEnabled = false;

             err = stopA2dpThread();
             if(err) {
                 LOGE("stopA2dpOutput = %d" ,err);
             }

             err = closeA2dpOutput();
             if(err) {
                  LOGE("closeA2dpOutput = %d" ,err);
             }

             err = mALSADevice->closeProxyDevice();
             if(err) {
                 LOGE("closeProxyDevice failed = %d", err);
             }

             mA2DPActiveUseCases = 0x0;

#ifdef OUTPUT_BUFFER_LOG
    LOGV("close file output");
    fclose (outputBufferFile1);
#endif
         }
     }
     return err;
}

status_t AudioHardwareALSA::openA2dpOutput()
{
    hw_module_t *mod;
    int      format = AUDIO_FORMAT_PCM_16_BIT;
    uint32_t channels = AUDIO_CHANNEL_OUT_STEREO;
    uint32_t sampleRate = AFE_PROXY_SAMPLE_RATE;
    status_t status;
    LOGV("openA2dpOutput");

    int rc = hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID, (const char*)"a2dp",
                                    (const hw_module_t**)&mod);
    if (rc) {
        LOGE("Could not get a2dp hardware module");
        return NO_INIT;
    }

    rc = audio_hw_device_open(mod, &mA2dpDevice);
    if(rc) {
        LOGE("couldn't open a2dp audio hw device");
        return NO_INIT;
    }
    status = mA2dpDevice->open_output_stream(mA2dpDevice, AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP,
                                    &format, &channels, &sampleRate, &mA2dpStream);
    if(status != NO_ERROR) {
        LOGE("Failed to open output stream for a2dp: status %d", status);
    }
    return status;
}

status_t AudioHardwareALSA::closeA2dpOutput()
{
    LOGV("closeA2dpOutput");
    if(!mA2dpDevice){
        LOGE("No Aactive A2dp output found");
        return NO_ERROR;
    }

    mA2dpDevice->close_output_stream(mA2dpDevice, mA2dpStream);
    mA2dpStream = NULL;

    audio_hw_device_close(mA2dpDevice);
    mA2dpDevice = NULL;
    return NO_ERROR;
}

status_t AudioHardwareALSA::stopA2dpThread()
{
    LOGV("stopA2dpThread");
    status_t err = NO_ERROR;
    if (!mA2dpThreadAlive) {
        LOGD("Return - thread not live");
        return NO_ERROR;
    }
    mKillA2DPThread = true;
    err = mALSADevice->exitReadFromProxy();
    if(err) {
        LOGE("exitReadFromProxy failed = %d", err);
    }
    mA2dpCv.signal();
    int ret = pthread_join(mA2dpThread,NULL);
    LOGD("a2dp thread killed = %d", ret);
    return err;
}

void *AudioHardwareALSA::a2dpThreadWrapper(void *me) {
    static_cast<AudioHardwareALSA *>(me)->a2dpThreadFunc();
    return NULL;
}

void AudioHardwareALSA::a2dpThreadFunc() {
    if(!mA2dpStream) {
        LOGE("No valid a2dp output stream found");
        return;
    }
    if(!mALSADevice->isProxyDeviceOpened()) {
        LOGE("No valid mProxyPcmHandle found");
        return;
    }

    pid_t tid  = gettid();
    androidSetThreadPriority(tid, ANDROID_PRIORITY_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"A2DPThread", 0, 0, 0);

    int ionBufCount = 0;
    uint32_t bytesWritten = 0;
    uint32_t numBytesRemaining = 0;
    uint32_t bytesAvailInBuffer = 0;
    void  *data;
    int err = NO_ERROR;
    ssize_t size = 0;

    mALSADevice->resetProxyVariables();

    LOGV("mKillA2DPThread = %d", mKillA2DPThread);
    while(!mKillA2DPThread) {

        {
            Mutex::Autolock autolock1(mA2dpMutex);
            if (!mA2dpStream || !mIsA2DPEnabled ||
                    !mALSADevice->isProxyDeviceOpened() ||
                    (mALSADevice->isProxyDeviceSuspended()) ||
                    (err != NO_ERROR)) {
                LOGD("A2DPThreadEntry:: mIsA2DPEnabled %d,\
                        proxy opened = %d, proxy suspended = %d,err =%d,\
                        mA2dpStream = %p", mIsA2DPEnabled,\
                        mALSADevice->isProxyDeviceOpened(),\
                        mALSADevice->isProxyDeviceSuspended(),err,mA2dpStream);
                LOGD("A2DPThreadEntry:: Waiting on mA2DPCv");
                mA2dpCv.wait(mA2dpMutex);
                LOGD("A2DPThreadEntry:: received signal to wake up");
                mA2dpMutex.unlock();
                continue;
            }
        }
        err = mALSADevice->readFromProxy(&data, &size);
        if(err < 0) {
           LOGE("ALSADevice readFromProxy returned err = %d,data = %p,\
                    size = %d", err, data, size);
           continue;
        }

#ifdef OUTPUT_BUFFER_LOG
    if (outputBufferFile1)
    {
        fwrite (data,1,size,outputBufferFile1);
    }
#endif
        void *copyBuffer = data;
        numBytesRemaining = size;
        while (err == OK && (numBytesRemaining  > 0) && !mKillA2DPThread
                && mIsA2DPEnabled ) {
            {
                Mutex::Autolock autolock1(mA2dpMutex);
                bytesAvailInBuffer = mA2dpStream->common.get_buffer_size(&mA2dpStream->common);
            }
            uint32_t writeLen = bytesAvailInBuffer > numBytesRemaining ?
                    numBytesRemaining : bytesAvailInBuffer;
            LOGV("Writing %d bytes to A2DP ", writeLen);
            {
                Mutex::Autolock autolock1(mA2dpMutex);
                bytesWritten = mA2dpStream->write(mA2dpStream,copyBuffer, writeLen);
            }
            LOGV("bytesWritten = %d",bytesWritten);
            //Need to check warning here - void used in arithmetic
            copyBuffer = (char *)copyBuffer + bytesWritten;
            numBytesRemaining -= bytesWritten;
            LOGV("@_@bytes To write2:%d",numBytesRemaining);
        }
    }

    mALSADevice->resetProxyVariables();
    mA2dpThreadAlive = false;
    LOGD("A2DP Thread is dying");
}

void AudioHardwareALSA::setA2DPActiveUseCases_l(uint32_t activeUsecase)
{
   mA2DPActiveUseCases |= activeUsecase;
   LOGD("mA2DPActiveUseCases = %u, activeUsecase = %u", mA2DPActiveUseCases, activeUsecase);
}

uint32_t AudioHardwareALSA::getA2DPActiveUseCases_l()
{
   LOGD("getA2DPActiveUseCases_l: mA2DPActiveUseCases = %u", mA2DPActiveUseCases);
   return mA2DPActiveUseCases;
}

void AudioHardwareALSA::clearA2DPActiveUseCases_l(uint32_t activeUsecase) {

   mA2DPActiveUseCases &= ~activeUsecase;
   LOGD("clear - mA2DPActiveUseCases = %u, activeUsecase = %u", mA2DPActiveUseCases, activeUsecase);

}

bool  AudioHardwareALSA::suspendA2dpPlayback(uint32_t activeUsecase) {

    Mutex::Autolock autoLock(mLock);
    suspendA2dpPlayback_l(activeUsecase);
    return NO_ERROR;
}

bool  AudioHardwareALSA::suspendA2dpPlayback_l(uint32_t activeUsecase) {

    Mutex::Autolock autolock1(mA2dpMutex);
    LOGD("suspendA2dpPlayback_l activeUsecase = %d, mIsA2DPEnabled = %d",\
            activeUsecase, mIsA2DPEnabled);
    clearA2DPActiveUseCases_l(activeUsecase);
    if(!getA2DPActiveUseCases_l() && mIsA2DPEnabled)
        return mALSADevice->suspendProxy();
    return NO_ERROR;
}

}       // namespace android_audio_legacy
