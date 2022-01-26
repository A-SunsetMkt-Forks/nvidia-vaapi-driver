#include "export-buf.h"
#include <stdio.h>
#include <ffnvcodec/dynlink_loader.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef __has_include
#  if __has_include(<libdrm/drm.h>)
#    include <libdrm/drm.h>
#  else
#    include <drm/drm.h>
#  endif
#else
#  include <drm/drm.h>
#endif

#ifndef EGL_NV_stream_consumer_eglimage
#define EGL_NV_stream_consumer_eglimage 1
#define EGL_STREAM_CONSUMER_IMAGE_NV      0x3373
#define EGL_STREAM_IMAGE_ADD_NV           0x3374
#define EGL_STREAM_IMAGE_REMOVE_NV        0x3375
#define EGL_STREAM_IMAGE_AVAILABLE_NV     0x3376
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLint num_modifiers, EGLuint64KHR *modifiers, EGLAttrib *attrib_list);
typedef EGLint (EGLAPIENTRYP PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLTime timeout, EGLenum *event, EGLAttrib *aux);
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSTREAMACQUIREIMAGENVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLImage *pImage, EGLSync sync);
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSTREAMRELEASEIMAGENVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLImage image, EGLSync sync);
#ifdef EGL_EGLEXT_PROTOTYPES
EGLAPI EGLBoolean EGLAPIENTRY eglStreamImageConsumerConnectNV (EGLDisplay dpy, EGLStreamKHR stream, EGLint num_modifiers, EGLuint64KHR *modifiers, EGLAttrib *attrib_list);
EGLAPI EGLint EGLAPIENTRY eglQueryStreamConsumerEventNV (EGLDisplay dpy, EGLStreamKHR stream, EGLTime timeout, EGLenum *event, EGLAttrib *aux);
EGLAPI EGLBoolean EGLAPIENTRY eglStreamAcquireImageNV (EGLDisplay dpy, EGLStreamKHR stream, EGLImage *pImage, EGLSync sync);
EGLAPI EGLBoolean EGLAPIENTRY eglStreamReleaseImageNV (EGLDisplay dpy, EGLStreamKHR stream, EGLImage image, EGLSync sync);
#endif
#endif

#ifndef EGL_EXT_device_drm_render_node
#define EGL_DRM_RENDER_NODE_FILE_EXT      0x3377
#endif

static PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC eglQueryStreamConsumerEventNV;
static PFNEGLSTREAMRELEASEIMAGENVPROC eglStreamReleaseImageNV;
static PFNEGLSTREAMACQUIREIMAGENVPROC eglStreamAcquireImageNV;
static PFNEGLEXPORTDMABUFIMAGEMESAPROC eglExportDMABUFImageMESA;
static PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC eglExportDMABUFImageQueryMESA;
static PFNEGLCREATESTREAMKHRPROC eglCreateStreamKHR;
static PFNEGLDESTROYSTREAMKHRPROC eglDestroyStreamKHR;
static PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC eglStreamImageConsumerConnectNV;
static PFNEGLQUERYDEVICESTRINGEXTPROC eglQueryDeviceStringEXT;

static void debug(EGLenum error,const char *command,EGLint messageType,EGLLabelKHR threadLabel,EGLLabelKHR objectLabel,const char* message) {
    LOG("[EGL] %s: %s", command, message);
}

void releaseExporter(NVDriver *drv) {
    LOG("Releasing exporter, %d outstanding frames", drv->numFramesPresented);
    while (drv->numFramesPresented > 0) {
      CUeglFrame eglframe;
      CUresult cuStatus = drv->cu->cuEGLStreamProducerReturnFrame(&drv->cuStreamConnection, &eglframe, NULL);
      if (cuStatus == CUDA_SUCCESS) {
        drv->numFramesPresented--;
        for (int i = 0; i < 3; i++) {
            if (eglframe.frame.pArray[i] != NULL) {
                LOG("Cleaning up CUDA array %p", eglframe.frame.pArray[i]);
                drv->cu->cuArrayDestroy(eglframe.frame.pArray[i]);
                eglframe.frame.pArray[i] = NULL;
            }
        }
      } else {
          break;
      }
    }
    LOG("Done releasing frames");

    NVEGLImage *img = drv->allocatedEGLImages;
    while (img != NULL) {
        LOG("Destroying EGLImage: %p", img->image);
        eglDestroyImage(drv->eglDisplay, img->image);
        NVEGLImage *oldImg = img;
        img = img->next;
        free(oldImg);
    }
    LOG("Done releasing EGLImages");

    if (drv->cuStreamConnection != NULL) {
        drv->cu->cuEGLStreamProducerDisconnect(&drv->cuStreamConnection);
    }

    if (drv->cuStreamConnection != NULL) {
        drv->cu->cuEGLStreamConsumerDisconnect(&drv->cuStreamConnection);
    }

    if (drv->eglDisplay != EGL_NO_DISPLAY) {
        if (drv->eglStream != EGL_NO_STREAM_KHR) {
            eglDestroyStreamKHR(drv->eglDisplay, drv->eglStream);
            drv->eglStream = EGL_NO_STREAM_KHR;
        }
        //TODO terminate the EGLDisplay here, sounds like that could break stuff
        drv->eglDisplay = EGL_NO_DISPLAY;
    }
}

static void reconnect(NVDriver *drv) {
    LOG("Reconnecting to stream");
    eglInitialize(drv->eglDisplay, NULL, NULL);
    if (drv->cuStreamConnection != NULL) {
        drv->cu->cuEGLStreamConsumerDisconnect(&drv->cuStreamConnection);
    }
    if (drv->eglStream != EGL_NO_STREAM_KHR) {
        eglDestroyStreamKHR(drv->eglDisplay, drv->eglStream);
    }
//    EGLint streamAttribs[] = { EGL_STREAM_FIFO_LENGTH_KHR, 4, EGL_NONE };
//    drv->eglStream = eglCreateStreamKHR(drv->eglDisplay, streamAttribs);
    drv->eglStream = eglCreateStreamKHR(drv->eglDisplay, NULL);
    if (drv->eglStream == EGL_NO_STREAM_KHR) {
        LOG("Unable to create EGLStream");
        return;
    }
    if (!eglStreamImageConsumerConnectNV(drv->eglDisplay, drv->eglStream, 0, 0, NULL)) {
        LOG("Unable to connect EGLImage stream consumer");
        return;
    }
    CHECK_CUDA_RESULT(drv->cu->cuEGLStreamProducerConnect(&drv->cuStreamConnection, drv->eglStream, 1024, 1024));
    drv->numFramesPresented = 0;
}

static int findCudaDisplay(EGLDisplay *eglDisplay) {
    PFNEGLQUERYDEVICESEXTPROC eglQueryDevicesEXT = (PFNEGLQUERYDEVICESEXTPROC) eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLQUERYDEVICEATTRIBEXTPROC eglQueryDeviceAttribEXT = (PFNEGLQUERYDEVICEATTRIBEXTPROC) eglGetProcAddress("eglQueryDeviceAttribEXT");

    *eglDisplay = EGL_NO_DISPLAY;

    if (eglQueryDevicesEXT == NULL || eglQueryDeviceAttribEXT == NULL) {
        return 0;
    }

    EGLDeviceEXT devices[8];
    EGLint num_devices;
    if(!eglQueryDevicesEXT(8, devices, &num_devices)) {
        return 0;
    }

    LOG("Found %d EGL devices", num_devices);
    for (int i = 0; i < num_devices; i++) {
        EGLAttrib attr;
        if (eglQueryDeviceAttribEXT(devices[i], EGL_CUDA_DEVICE_NV, &attr)) {
            LOG("Got EGL_CUDA_DEVICE_NV value '%d' from device %d", attr, i);
            //TODO: currently we're hardcoding the CUDA device to 0, so only create the display on that device
            if (attr == 0) {
                //attr contains the cuda device id
                //we need to check to see if modeset=1 has been passed to the nvidia_drm driver
                //since you need to be root to read the parameter out of /sys, we'll have to find the
                //DRM device file, open it and issue an ioctl to see if the ASYNC_PAGE_FLIP cap is set
                const char* drmDeviceFile = eglQueryDeviceStringEXT(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);
                if (drmDeviceFile == NULL) {
                    LOG("Unable to retrieve render node, falling back to device node");
                    drmDeviceFile = eglQueryDeviceStringEXT(devices[i], EGL_DRM_DEVICE_FILE_EXT);
                }
                LOG("Checking device file: %s", drmDeviceFile);
                if (drmDeviceFile != NULL) {
                    int fd = open(drmDeviceFile, O_RDONLY);
                    if (fd > 0) {
                        //this ioctl should fail if modeset=0
                        struct drm_get_cap caps = { .capability = DRM_CAP_DUMB_BUFFER };
                        int ret = ioctl(fd, DRM_IOCTL_GET_CAP, &caps);
                        LOG("Got DRM_IOCTL_GET_CAP ioctl response: %d %d", ret, caps.value);
                        close(fd);
                        if (ret != 0) {
                            //the modeset parameter is set to 0
                            LOG("ERROR: This driver requires the nvidia_drm.modeset kernel module parameter set to 1");
                            return -1;
                        }
                    } else {
                        LOG("Unable to check nvidia_drm modeset setting");
                    }
                } else {
                    LOG("Unable to retrieve DRM device file");
                }

                *eglDisplay = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);
                return 1;
            }
        }
    }

    return 0;
}

bool initExporter(NVDriver *drv) {
    static const EGLAttrib debugAttribs[] = {EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE, EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE, EGL_NONE};

    eglQueryStreamConsumerEventNV = (PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC) eglGetProcAddress("eglQueryStreamConsumerEventNV");
    eglStreamReleaseImageNV = (PFNEGLSTREAMRELEASEIMAGENVPROC) eglGetProcAddress("eglStreamReleaseImageNV");
    eglStreamAcquireImageNV = (PFNEGLSTREAMACQUIREIMAGENVPROC) eglGetProcAddress("eglStreamAcquireImageNV");
    eglExportDMABUFImageMESA = (PFNEGLEXPORTDMABUFIMAGEMESAPROC) eglGetProcAddress("eglExportDMABUFImageMESA");
    eglExportDMABUFImageQueryMESA = (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC) eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    eglCreateStreamKHR = (PFNEGLCREATESTREAMKHRPROC) eglGetProcAddress("eglCreateStreamKHR");
    eglDestroyStreamKHR = (PFNEGLDESTROYSTREAMKHRPROC) eglGetProcAddress("eglDestroyStreamKHR");
    eglStreamImageConsumerConnectNV = (PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC) eglGetProcAddress("eglStreamImageConsumerConnectNV");
    eglQueryDeviceStringEXT = (PFNEGLQUERYDEVICESTRINGEXTPROC) eglGetProcAddress("eglQueryDeviceStringEXT");

    PFNEGLDEBUGMESSAGECONTROLKHRPROC eglDebugMessageControlKHR = (PFNEGLDEBUGMESSAGECONTROLKHRPROC) eglGetProcAddress("eglDebugMessageControlKHR");

    int ret = findCudaDisplay(&drv->eglDisplay);
    if (ret == 1) {
        LOG("Got EGLDisplay from CUDA device");
    } else if (ret == 0) {
        LOG("Falling back to using default EGLDisplay");
        drv->eglDisplay = eglGetDisplay(NULL);
    } else if (ret == -1) {
        return false;
    }
    if (!eglInitialize(drv->eglDisplay, NULL, NULL)) {
        LOG("Unable to initialise EGL for display: %p", drv->eglDisplay);
        return false;
    }
    //setup debug logging
    eglDebugMessageControlKHR(debug, debugAttribs);

//    drv->eglContext = eglCreateContext(drv->eglDisplay, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, NULL);
//    eglMakeCurrent(drv->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, drv->eglContext);
    reconnect(drv);

    return true;
}

int exportCudaPtr(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch, int *fourcc, int *fds, int *offsets, int *strides, uint64_t *mods, int *bppOut) {
//    EGLDisplay oldDisplay = eglGetCurrentDisplay();
//    EGLContext oldContext = eglGetCurrentContext();
//    EGLSurface oldReadSurface = eglGetCurrentSurface(EGL_READ);
//    EGLSurface oldDrawSurface = eglGetCurrentSurface(EGL_DRAW);
//    eglMakeCurrent(drv->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, drv->eglContext);

    // If there is a frame presented before we check if consumer
    // is done with it using cuEGLStreamProducerReturnFrame.
    //LOG("outstanding frames: %d", numFramesPresented);
    CUeglFrame eglframe = {
        .frame = {
            .pArray = {0, 0, 0}
        }
    };
    //TODO if we ever have more than 1 frame returned a frame, we'll leak that memory
    while (drv->numFramesPresented > 0) {
      //LOG("waiting for returned frame: %lx %d", drv->cuStreamConnection, drv->numFramesPresented);
      CUresult cuStatus = drv->cu->cuEGLStreamProducerReturnFrame(&drv->cuStreamConnection, &eglframe, NULL);
      if (cuStatus == CUDA_ERROR_LAUNCH_TIMEOUT) {
        //LOG("timeout with %d outstanding", drv->numFramesPresented);
        break;
      } else if (cuStatus != CUDA_SUCCESS) {
        CHECK_CUDA_RESULT(cuStatus);
      } else {
        LOG("returned frame %dx%d %p %p", eglframe.width, eglframe.height, eglframe.frame.pArray[0], eglframe.frame.pArray[1]);
        drv->numFramesPresented--;
      }
    }

    uint32_t width = surface->width;
    uint32_t height = surface->height;

    //check if the frame size if different and release the arrays
    //TODO figure out how to get the EGLimage freed aswell
    if (eglframe.width != width || eglframe.height != height) {
        if (eglframe.frame.pArray[0] != NULL) {
            drv->cu->cuArrayDestroy(eglframe.frame.pArray[0]);
            eglframe.frame.pArray[0] = NULL;
        }
        if (eglframe.frame.pArray[1] != NULL) {
            drv->cu->cuArrayDestroy(eglframe.frame.pArray[1]);
            eglframe.frame.pArray[1] = NULL;
        }
    }
    eglframe.width = width;
    eglframe.height = height;
    eglframe.depth = 1;
    eglframe.pitch = 0;
    eglframe.planeCount = 2;
    eglframe.numChannels = 1;
    eglframe.frameType = CU_EGL_FRAME_TYPE_ARRAY;

    int bpp = 1;
    if (surface->format == cudaVideoSurfaceFormat_NV12) {
        eglframe.eglColorFormat = drv->useCorrectNV12Format ? CU_EGL_COLOR_FORMAT_YUV420_SEMIPLANAR :
                                                              CU_EGL_COLOR_FORMAT_YVU420_SEMIPLANAR;
        eglframe.cuFormat = CU_AD_FORMAT_UNSIGNED_INT8;
    } else if (surface->format == cudaVideoSurfaceFormat_P016) {
        //TODO not working, produces this error in mpv:
        //EGL_BAD_MATCH error: In eglCreateImageKHR: requested LINUX_DRM_FORMAT is not supported
        //this error seems to be coming from the NVIDIA EGL driver
        //this might be caused by the DRM_FORMAT_*'s in nvExportSurfaceHandle
        if (surface->bitDepth == 10) {
            eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_Y10V10U10_420_SEMIPLANAR;
        } else if (surface->bitDepth == 12) {
            eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_Y12V12U12_420_SEMIPLANAR;
        } else {
            LOG("Unknown bitdepth");
        }
        eglframe.cuFormat = CU_AD_FORMAT_UNSIGNED_INT16;
        bpp = 2;
    }
    *bppOut = bpp;

    //TODO in theory this should work, but the application attempting to bind that texture gets the following error:
    //GL_INVALID_OPERATION error generated. <image> and <target> are incompatible
    //eglframe.frameType = CU_EGL_FRAME_TYPE_PITCH;
    //eglframe.pitch = pitch;
    //eglframe.frame.pPitch[0] = (void*) ptr;
    //eglframe.frame.pPitch[1] = (void*) ptr + (height*pitch);

    //reuse the arrays if we can
    //creating new arrays will cause a new EGLimage to be created and we'll eventually run out of resources
    if (eglframe.frame.pArray[0] == NULL) {
        CUDA_ARRAY3D_DESCRIPTOR arrDesc = {
            .Width = width,
            .Height = height,
            .Depth = 0,
            .NumChannels = 1,
            .Flags = 0,
            .Format = eglframe.cuFormat
        };
        CHECK_CUDA_RESULT(drv->cu->cuArray3DCreate(&eglframe.frame.pArray[0], &arrDesc));
    }
    if (eglframe.frame.pArray[1] == NULL) {
        CUDA_ARRAY3D_DESCRIPTOR arr2Desc = {
            .Width = width >> 1,
            .Height = height >> 1,
            .Depth = 0,
            .NumChannels = 2,
            .Flags = 0,
            .Format = eglframe.cuFormat
        };
        CHECK_CUDA_RESULT(drv->cu->cuArray3DCreate(&eglframe.frame.pArray[1], &arr2Desc));
    }
    if (ptr != 0) {
        CUDA_MEMCPY2D cpy = {
            .srcMemoryType = CU_MEMORYTYPE_DEVICE,
            .srcDevice = ptr,
            .srcPitch = pitch,
            .dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .dstArray = eglframe.frame.pArray[0],
            .Height = height,
            .WidthInBytes = width * bpp
        };
        CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy));
        CUDA_MEMCPY2D cpy2 = {
            .srcMemoryType = CU_MEMORYTYPE_DEVICE,
            .srcDevice = ptr,
            .srcY = height,
            .srcPitch = pitch,
            .dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .dstArray = eglframe.frame.pArray[1],
            .Height = height >> 1,
            .WidthInBytes = width * bpp
        };
        CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy2));
    }

    LOG("presenting frame %dx%d %p %p", eglframe.width, eglframe.height, eglframe.frame.pArray[0], eglframe.frame.pArray[1]);
    CUresult ret = drv->cu->cuEGLStreamProducerPresentFrame( &drv->cuStreamConnection, eglframe, NULL );
    if (ret == CUDA_ERROR_UNKNOWN) {
        reconnect(drv);
        CHECK_CUDA_RESULT(drv->cu->cuEGLStreamProducerPresentFrame( &drv->cuStreamConnection, eglframe, NULL ));
    }

    drv->numFramesPresented++;

    while (1) {
        EGLenum event = 0;
        EGLAttrib aux = 0;
        //check for the next event
        if (eglQueryStreamConsumerEventNV(drv->eglDisplay, drv->eglStream, 0, &event, &aux) != EGL_TRUE) {
            break;
        }

        if (event == EGL_STREAM_IMAGE_ADD_NV) {
            EGLImage image = eglCreateImage(drv->eglDisplay, EGL_NO_CONTEXT, EGL_STREAM_CONSUMER_IMAGE_NV, drv->eglStream, NULL);
            NVEGLImage* nvEglImage = (NVEGLImage*) calloc(1, sizeof(NVEGLImage));
            LOG("Adding frame from EGLStream: %p", image);
            nvEglImage->image = image;
            nvEglImage->next = drv->allocatedEGLImages;
            drv->allocatedEGLImages = nvEglImage;
        } else if (event == EGL_STREAM_IMAGE_AVAILABLE_NV) {
            EGLImage img;
            //somehow we get here with the previous frame, not the next one
            if (!eglStreamAcquireImageNV(drv->eglDisplay, drv->eglStream, &img, EGL_NO_SYNC_NV)) {
                LOG("eglStreamAcquireImageNV failed");
                return 0;
            }

            LOG("Acquired image from EGLStream: %p", img);

            int planes = 0;
            if (!eglExportDMABUFImageQueryMESA(drv->eglDisplay, img, fourcc, &planes, mods)) {
                LOG("eglExportDMABUFImageQueryMESA failed");
                return 0;
            }

            //LOG("eglExportDMABUFImageQueryMESA: %p %.4s (%x) planes:%d mods:%lx %lx", img, (char*)fourcc, *fourcc, planes, mods[0], mods[1]);

            EGLBoolean r = eglExportDMABUFImageMESA(drv->eglDisplay, img, fds, strides, offsets);

            if (!r) {
                LOG("Unable to export image");
                return 0;
            }
            //LOG("eglExportDMABUFImageMESA: %d %d %d %d, strides: %d %d %d %d, offsets: %d %d %d %d",
            //        fds[0], fds[1], fds[2], fds[3],
            //        strides[0], strides[1], strides[2], strides[3],
            //        offsets[0], offsets[1], offsets[2], offsets[3]);

            r = eglStreamReleaseImageNV(drv->eglDisplay, drv->eglStream, img, EGL_NO_SYNC_NV);
            if (!r) {
                LOG("Unable to release image");
                return 0;
            }
        } else if (event == EGL_STREAM_IMAGE_REMOVE_NV) {
            LOG("Removing image from EGLStream, eglDestroyImage: %p", (EGLImage) aux);
            eglDestroyImage(drv->eglDisplay, (EGLImage) aux);
        } else {
            LOG("Unhandled event: %X", event);
        }
    }

//    if (oldDisplay != EGL_NO_DISPLAY) {
//        eglMakeCurrent(oldDisplay, oldReadSurface, oldDrawSurface, oldContext);
//    }
    return 1;
}
