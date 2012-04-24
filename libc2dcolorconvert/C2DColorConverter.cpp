/* copyright (c) 2012, code aurora forum. all rights reserved.
 *
 * redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * neither the name of code aurora forum, inc. nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * this software is provided "as is" and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability, fitness for a particular purpose and non-infringement
 * are disclaimed.  in no event shall the copyright owner or contributors
 * be liable for any direct, indirect, incidental, special, exemplary, or
 * consequential damages (including, but not limited to, procurement of
 * substitute goods or services; loss of use, data, or profits; or
 * business interruption) however caused and on any theory of liability,
 * whether in contract, strict liability, or tort (including negligence
 * or otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */
/*--------------------------------------------------------------------------
Copyright (c) 2012 Code Aurora Forum. All rights reserved.
--------------------------------------------------------------------------*/

#include <C2DColorConverter.h>
#include <arm_neon.h>
#include <stdlib.h>
#include <fcntl.h>
#include <linux/msm_kgsl.h>
#include <sys/ioctl.h>
#include <utils/Log.h>
#include <MediaDebug.h>
#include <dlfcn.h>

#define LOG_TAG "C2DColorConvert"
#define ALIGN( num, to ) (((num) + (to-1)) & (~(to-1)))
#define ALIGN8K 8192
#define ALIGN2K 2048
#define ALIGN128 128
#define ALIGN32 32
#define ALIGN16 16

//-----------------------------------------------------
namespace android {

class C2DColorConverter : public C2DColorConverterBase {

public:
    C2DColorConverter(size_t srcWidth, size_t srcHeight, size_t dstWidth, size_t dstHeight, ColorConvertFormat srcFormat, ColorConvertFormat dstFormat, size_t srcSize, size_t dstSize, int32_t flags);

protected:
    virtual ~C2DColorConverter();
    virtual int convertC2D(int srcFd, void * srcData, int dstFd, void * dstData);

private:
    virtual bool isYUVSurface(ColorConvertFormat format);
    virtual void *getDummySurfaceDef(ColorConvertFormat format, size_t width, size_t height, bool isSource);
    virtual C2D_STATUS updateYUVSurfaceDef(int fd, void * data, bool isSource);
    virtual C2D_STATUS updateRGBSurfaceDef(int fd, void * data, bool isSource);
    virtual uint32_t getC2DFormat(ColorConvertFormat format);
    virtual size_t calcStride(ColorConvertFormat format, size_t width);
    virtual size_t calcYSize(ColorConvertFormat format, size_t width, size_t height);
    virtual void *getMappedGPUAddr(int bufFD, void *bufPtr, size_t bufLen);
    virtual bool unmapGPUAddr(uint32_t gAddr);

    void *mC2DLibHandle;
    LINK_c2dCreateSurface mC2DCreateSurface;
    LINK_c2dUpdateSurface mC2DUpdateSurface;
    LINK_c2dReadSurface mC2DReadSurface;
    LINK_c2dDraw mC2DDraw;
    LINK_c2dFlush mC2DFlush;
    LINK_c2dFinish mC2DFinish;
    LINK_c2dWaitTimestamp mC2DWaitTimestamp;
    LINK_c2dDestroySurface mC2DDestroySurface;

    int32_t mKgslFd;
    uint32_t mSrcSurface, mDstSurface;
    void * mSrcSurfaceDef;
    void * mDstSurfaceDef;

    C2D_OBJECT mBlit;
    size_t mSrcWidth;
    size_t mSrcHeight;
    size_t mDstWidth;
    size_t mDstHeight;
    size_t mSrcSize;
    size_t mDstSize;
    size_t mSrcYSize;
    size_t mDstYSize;
    enum ColorConvertFormat mSrcFormat;
    enum ColorConvertFormat mDstFormat;
    int32_t mFlags;

    int mError;
};

C2DColorConverter::C2DColorConverter(size_t srcWidth, size_t srcHeight, size_t dstWidth, size_t dstHeight, ColorConvertFormat srcFormat, ColorConvertFormat dstFormat, size_t srcSize, size_t dstSize, int32_t flags)
{
     mError = 0;
     mC2DLibHandle = dlopen("libC2D2.so", RTLD_NOW);
     if (!mC2DLibHandle) {
         LOGE("FATAL ERROR: could not dlopen libc2d2.so: %s", dlerror());
         mError = -1;
         return;
     }
     mC2DCreateSurface = (LINK_c2dCreateSurface)dlsym(mC2DLibHandle, "c2dCreateSurface");
     mC2DUpdateSurface = (LINK_c2dUpdateSurface)dlsym(mC2DLibHandle, "c2dUpdateSurface");
     mC2DReadSurface = (LINK_c2dReadSurface)dlsym(mC2DLibHandle, "c2dReadSurface");
     mC2DDraw = (LINK_c2dDraw)dlsym(mC2DLibHandle, "c2dDraw");
     mC2DFlush = (LINK_c2dFlush)dlsym(mC2DLibHandle, "c2dFlush");
     mC2DFinish = (LINK_c2dFinish)dlsym(mC2DLibHandle, "c2dFinish");
     mC2DWaitTimestamp = (LINK_c2dWaitTimestamp)dlsym(mC2DLibHandle, "c2dWaitTimestamp");
     mC2DDestroySurface = (LINK_c2dDestroySurface)dlsym(mC2DLibHandle, "c2dDestroySurface");

     if (!mC2DCreateSurface || !mC2DUpdateSurface || !mC2DReadSurface
        || !mC2DDraw || !mC2DFlush || !mC2DFinish || !mC2DWaitTimestamp
        || !mC2DDestroySurface) {
         LOGE("%s: dlsym ERROR", __FUNCTION__);
         mError = -1;
         return;
     }

    mSrcWidth = srcWidth;
    mSrcHeight = srcHeight;
    mDstWidth = dstWidth;
    mDstHeight = dstHeight;
    mSrcFormat = srcFormat;
    mDstFormat = dstFormat;
    mSrcSize = srcSize;
    mDstSize = dstSize;
    mSrcYSize = calcYSize(srcFormat, srcWidth, srcHeight);
    mDstYSize = calcYSize(dstFormat, dstWidth, dstHeight);

    mFlags = flags; // can be used for rotation
    mKgslFd = open("/dev/kgsl-2d0", O_RDWR | O_SYNC);
    if (mKgslFd < 0) {
        LOGE("Cannot open device kgsl-2d0, trying kgsl-3d0\n");
        mKgslFd = open("/dev/kgsl-3d0", O_RDWR | O_SYNC);
        if (mKgslFd < 0) {
            LOGE("Failed to open device kgsl-3d0\n");
            mError = -1;
            return;
        }
    }

    mSrcSurfaceDef = getDummySurfaceDef(srcFormat, srcWidth, srcHeight, true);
    mDstSurfaceDef = getDummySurfaceDef(dstFormat, dstWidth, dstHeight, false);

    memset((void*)&mBlit,0,sizeof(C2D_OBJECT));
    mBlit.source_rect.x = 0 << 16;
    mBlit.source_rect.y = 0 << 16;
    mBlit.source_rect.width = srcWidth << 16;
    mBlit.source_rect.height = srcHeight << 16;
    mBlit.target_rect.x = 0 << 16;
    mBlit.target_rect.y = 0 << 16;
    mBlit.target_rect.width = dstWidth << 16;
    mBlit.target_rect.height = dstHeight << 16;
    mBlit.config_mask = C2D_ALPHA_BLEND_NONE | C2D_NO_BILINEAR_BIT | C2D_NO_ANTIALIASING_BIT | C2D_TARGET_RECT_BIT;
    mBlit.surface_id = mSrcSurface;
}

C2DColorConverter::~C2DColorConverter()
{
    if (mError) {
        if (mC2DLibHandle) {
            dlclose(mC2DLibHandle);
        }
        return;
    }

    mC2DDestroySurface(mDstSurface);
    mC2DDestroySurface(mSrcSurface);
    if (isYUVSurface(mSrcFormat)) {
        delete ((C2D_YUV_SURFACE_DEF *)mSrcSurfaceDef);
    } else {
        delete ((C2D_RGB_SURFACE_DEF *)mSrcSurfaceDef);
    }

    if (isYUVSurface(mDstFormat)) {
        delete ((C2D_YUV_SURFACE_DEF *)mDstSurfaceDef);
    } else {
        delete ((C2D_RGB_SURFACE_DEF *)mDstSurfaceDef);
    }

    dlclose(mC2DLibHandle);
    close(mKgslFd);
}

int C2DColorConverter::convertC2D(int srcFd, void * srcData, int dstFd, void * dstData)
{
    C2D_STATUS ret;

    if (mError) {
        LOGE("C2D library initialization failed\n");
        return mError;
    }

    if ((srcFd < 0) || (dstFd < 0) || (srcData == NULL) || (dstData == NULL)) {
        LOGE("Incorrect input parameters\n");
        return -1;
    }

    if (isYUVSurface(mSrcFormat)) {
        ret = updateYUVSurfaceDef(srcFd, srcData, true);
    } else {
        ret = updateRGBSurfaceDef(srcFd, srcData, true);
    }

    if (ret != C2D_STATUS_OK) {
        LOGE("Update src surface def failed\n");
        return -ret;
    }

    if (isYUVSurface(mDstFormat)) {
        ret = updateYUVSurfaceDef(dstFd, dstData, false);
    } else {
        ret = updateRGBSurfaceDef(dstFd, dstData, false);
    }

    if (ret != C2D_STATUS_OK) {
        LOGE("Update dst surface def failed\n");
        return -ret;
    }

    mBlit.surface_id = mSrcSurface;
    ret = mC2DDraw(mDstSurface, C2D_TARGET_ROTATE_0, 0, 0, 0, &mBlit, 1);
    mC2DFinish(mDstSurface);

    bool unmappedSrcSuccess;
    if (isYUVSurface(mSrcFormat)) {
        unmappedSrcSuccess = unmapGPUAddr((uint32_t)((C2D_YUV_SURFACE_DEF *)mSrcSurfaceDef)->phys0);
    } else {
        unmappedSrcSuccess = unmapGPUAddr((uint32_t)((C2D_RGB_SURFACE_DEF *)mSrcSurfaceDef)->phys);
    }

    bool unmappedDstSuccess;
    if (isYUVSurface(mDstFormat)) {
        unmappedDstSuccess = unmapGPUAddr((uint32_t)((C2D_YUV_SURFACE_DEF *)mDstSurfaceDef)->phys0);
    } else {
        unmappedDstSuccess = unmapGPUAddr((uint32_t)((C2D_RGB_SURFACE_DEF *)mDstSurfaceDef)->phys);
    }

    if (ret != C2D_STATUS_OK) {
        LOGE("C2D Draw failed\n");
        return -ret; //c2d err values are positive
    } else {
        if (!unmappedSrcSuccess || !unmappedDstSuccess) {
            LOGE("unmapping GPU address failed\n");
            return -1;
        }
        return ret;
    }
}

bool C2DColorConverter::isYUVSurface(ColorConvertFormat format)
{
    switch (format) {
        case YCbCr420Tile:
        case YCbCr420SP:
        case YCbCr420P:
        case YCrCb420P:
            return true;
        case RGB565:
        default:
            return false;
    }
}

void* C2DColorConverter::getDummySurfaceDef(ColorConvertFormat format, size_t width, size_t height, bool isSource)
{
    if (isYUVSurface(format)) {
        C2D_YUV_SURFACE_DEF * surfaceDef = new C2D_YUV_SURFACE_DEF;
        surfaceDef->format = getC2DFormat(format);
        surfaceDef->width = width;
        surfaceDef->height = height;
        surfaceDef->plane0 = (void *)0xaaaaaaaa;
        surfaceDef->phys0 = (void *)0xaaaaaaaa;
        surfaceDef->stride0 = calcStride(format, width);
        surfaceDef->plane1 = (void *)0xaaaaaaaa;
        surfaceDef->phys1 = (void *)0xaaaaaaaa;
        surfaceDef->stride1 = calcStride(format, width);
        mC2DCreateSurface(isSource ? &mSrcSurface : &mDstSurface, isSource ? C2D_SOURCE : C2D_TARGET,
                        (C2D_SURFACE_TYPE)(C2D_SURFACE_YUV_HOST | C2D_SURFACE_WITH_PHYS),
                        &(*surfaceDef));
        return ((void *)surfaceDef);
    } else {
        C2D_RGB_SURFACE_DEF * surfaceDef = new C2D_RGB_SURFACE_DEF;
        surfaceDef->format = getC2DFormat(format);
        surfaceDef->width = width;
        surfaceDef->height = height;
        surfaceDef->buffer = (void *)0xaaaaaaaa;
        surfaceDef->phys = (void *)0xaaaaaaaa;
        surfaceDef->stride = calcStride(format, width);
        mC2DCreateSurface(isSource ? &mSrcSurface : &mDstSurface, isSource ? C2D_SOURCE : C2D_TARGET,
                        (C2D_SURFACE_TYPE)(C2D_SURFACE_RGB_HOST | C2D_SURFACE_WITH_PHYS),
                        &(*surfaceDef));
        return ((void *)surfaceDef);
    }
}

C2D_STATUS C2DColorConverter::updateYUVSurfaceDef(int fd, void * data, bool isSource)
{
    if (isSource) {
        C2D_YUV_SURFACE_DEF * srcSurfaceDef = (C2D_YUV_SURFACE_DEF *)mSrcSurfaceDef;
        srcSurfaceDef->plane0 = data;
        srcSurfaceDef->phys0  = getMappedGPUAddr(fd, data, mSrcSize);
        srcSurfaceDef->plane1 = (uint8_t *)data + mSrcYSize;
        srcSurfaceDef->phys1  = (uint8_t *)srcSurfaceDef->phys0 + mSrcYSize;
        return mC2DUpdateSurface(mSrcSurface, C2D_SOURCE,
                        (C2D_SURFACE_TYPE)(C2D_SURFACE_YUV_HOST | C2D_SURFACE_WITH_PHYS),
                        &(*srcSurfaceDef));
    } else {
        C2D_YUV_SURFACE_DEF * dstSurfaceDef = (C2D_YUV_SURFACE_DEF *)mDstSurfaceDef;
        dstSurfaceDef->plane0 = data;
        dstSurfaceDef->phys0  = getMappedGPUAddr(fd, data, mDstSize);
        dstSurfaceDef->plane1 = (uint8_t *)data + mDstYSize;
        dstSurfaceDef->phys1  = (uint8_t *)dstSurfaceDef->phys0 + mDstYSize;
        return mC2DUpdateSurface(mDstSurface, C2D_TARGET,
                        (C2D_SURFACE_TYPE)(C2D_SURFACE_YUV_HOST | C2D_SURFACE_WITH_PHYS),
                        &(*dstSurfaceDef));
    }
}

C2D_STATUS C2DColorConverter::updateRGBSurfaceDef(int fd, void * data, bool isSource)
{
    if (isSource) {
        C2D_RGB_SURFACE_DEF * srcSurfaceDef = (C2D_RGB_SURFACE_DEF *)mSrcSurfaceDef;
        srcSurfaceDef->buffer = data;
        srcSurfaceDef->phys = getMappedGPUAddr(fd, data, mSrcSize);
        return  mC2DUpdateSurface(mSrcSurface, C2D_SOURCE,
                        (C2D_SURFACE_TYPE)(C2D_SURFACE_RGB_HOST | C2D_SURFACE_WITH_PHYS),
                        &(*srcSurfaceDef));
    } else {
        C2D_RGB_SURFACE_DEF * dstSurfaceDef = (C2D_RGB_SURFACE_DEF *)mDstSurfaceDef;
        dstSurfaceDef->buffer = data;
        dstSurfaceDef->phys = getMappedGPUAddr(fd, data, mDstSize);
        return mC2DUpdateSurface(mDstSurface, C2D_TARGET,
                        (C2D_SURFACE_TYPE)(C2D_SURFACE_RGB_HOST | C2D_SURFACE_WITH_PHYS),
                        &(*dstSurfaceDef));
    }
}

uint32_t C2DColorConverter::getC2DFormat(ColorConvertFormat format)
{
    switch (format) {
        case RGB565:
            return C2D_COLOR_FORMAT_565_RGB;
        case YCbCr420Tile:
            return (C2D_COLOR_FORMAT_420_NV12 | C2D_FORMAT_MACROTILED);
        case YCbCr420SP:
            return C2D_COLOR_FORMAT_420_NV12;
        case YCbCr420P:
            return C2D_COLOR_FORMAT_420_I420;
        case YCrCb420P:
            return C2D_COLOR_FORMAT_420_YV12;
        default:
            return -1;
    }
}

size_t C2DColorConverter::calcStride(ColorConvertFormat format, size_t width)
{
    switch (format) {
        case RGB565:
            return ALIGN(width, ALIGN32) * 2; // RGB565 has width as twice
        case YCbCr420Tile:
            return ALIGN(width, ALIGN128);
        case YCbCr420SP:
        case YCbCr420P:
            return width;
        case YCrCb420P:
            return ALIGN(width, ALIGN16);
        default:
            return -1;
    }
}

size_t C2DColorConverter::calcYSize(ColorConvertFormat format, size_t width, size_t height)
{
    switch (format) {
        case YCbCr420SP:
            return ALIGN((width * height), ALIGN2K);
        case YCbCr420P:
            return width * height;
        case YCrCb420P:
            return ALIGN(width, ALIGN16) * height;
        case YCbCr420Tile:
            return ALIGN(ALIGN(width, ALIGN128) * ALIGN(height, ALIGN32), ALIGN8K);
        default:
            return -1;
    }
}
/*
 * Tells GPU to map given buffer and returns a physical address of mapped buffer
 */
void * C2DColorConverter::getMappedGPUAddr(int bufFD, void *bufPtr, size_t bufLen)
{
    struct kgsl_map_user_mem param;
    param.fd = bufFD;
    param.offset = 0;
    param.len = bufLen;
    param.hostptr = (unsigned int)bufPtr;
    param.memtype = KGSL_USER_MEM_TYPE_ION;
    param.reserved = 0;
    param.gpuaddr = 0;

    if (!ioctl(mKgslFd, IOCTL_KGSL_MAP_USER_MEM, &param, sizeof(param))) {
        return (void *)param.gpuaddr;
    }
    LOGE("mapping failed");
    return NULL;
}

bool C2DColorConverter::unmapGPUAddr(uint32_t gAddr)
{
   int rc = 0;
   struct kgsl_sharedmem_free param;
   memset(&param, 0, sizeof(param));
   param.gpuaddr = gAddr;

   rc = ioctl(mKgslFd, IOCTL_KGSL_SHAREDMEM_FREE, (void *)&param,
     sizeof(param));
   if (rc < 0) {
     LOGE("%s: IOCTL_KGSL_SHAREDMEM_FREE failed rc = %d\n", __func__, rc);
     return false;
   }
   return true;
}

extern "C" C2DColorConverterBase* createC2DColorConverter(size_t srcWidth, size_t srcHeight, size_t dstWidth, size_t dstHeight, ColorConvertFormat srcFormat, ColorConvertFormat dstFormat, size_t srcSize, size_t dstSize, int32_t flags)
{
    return new C2DColorConverter(srcWidth, srcHeight, dstWidth, dstHeight, srcFormat, dstFormat, srcSize, dstSize, flags);
}

extern "C" void destroyC2DColorConverter(C2DColorConverterBase* C2DCC)
{
    delete C2DCC;
}

}
