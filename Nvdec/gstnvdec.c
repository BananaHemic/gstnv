/*
 * Copyright (C) 2017 Ericsson AB. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstnvdec.h"

#include <gst/gl/gstglfuncs.h>
#include <cudaGL.h>

// According to the NVCodec sample, 20 is the min for h264
#define NUM_SURFACES_H264 20
#define NUM_SURFACES_H265 20
#define NUM_SURFACES_MPEG 20
#define NUM_SURFACES_JPEG 1

typedef enum
{
  GST_NVDEC_QUEUE_ITEM_TYPE_SEQUENCE,
  GST_NVDEC_QUEUE_ITEM_TYPE_DECODE,
  GST_NVDEC_QUEUE_ITEM_TYPE_DISPLAY
} GstNvDecQueueItemType;

enum
{
    PROP_0,
    PROP_CTX,
    PROP_LOCK
};

typedef struct _GstNvDecQueueItem
{
  GstNvDecQueueItemType type;
  gpointer data;
} GstNvDecQueueItem;

#if USE_GL
typedef struct _GstNvDecCudaGraphicsResourceInfo
{
  GstGLContext *gl_context;
  GstNvDecCudaContext *cuda_context;
  CUgraphicsResource resource;
} GstNvDecCudaGraphicsResourceInfo;
#endif

GST_DEBUG_CATEGORY_STATIC (gst_nvdec_debug_category);
#define GST_CAT_DEFAULT gst_nvdec_debug_category

//G_DEFINE_TYPE (GstNvDecCudaContext, gst_nvdec_cuda_context, G_TYPE_OBJECT);

static gboolean gst_nvdec_start (GstVideoDecoder * decoder);
static gboolean gst_nvdec_stop (GstVideoDecoder * decoder);
static gboolean gst_nvdec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state);
static GstFlowReturn gst_nvdec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame);
static void gst_nvdec_set_context (GstElement * element, GstContext * context);
#if USE_GL
static gboolean gst_nvdec_decide_allocation (GstVideoDecoder * decoder,
    GstQuery * query);
static gboolean gst_nvdec_src_query (GstVideoDecoder * decoder,
    GstQuery * query);
#endif
static gboolean gst_nvdec_flush (GstVideoDecoder * decoder);
static GstFlowReturn gst_nvdec_drain (GstVideoDecoder * decoder);
static void gst_nvdec_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_nvdec_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

static GstStaticPadTemplate gst_nvdec_sink_template =
    GST_STATIC_PAD_TEMPLATE (GST_VIDEO_DECODER_SINK_NAME,
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264, stream-format=byte-stream, alignment=au; "
        "video/x-h265, stream-format=byte-stream, alignment=au; "
        "video/mpeg, mpegversion={ 1, 2, 4 }, systemstream=false; "
        "image/jpeg")
    );

#if !USE_GL
static GstStaticPadTemplate gst_nvdec_src_template =
GST_STATIC_PAD_TEMPLATE (GST_VIDEO_DECODER_SRC_NAME,
    GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE("NV12"))
    );
#else
static GstStaticPadTemplate gst_nvdec_src_template =
GST_STATIC_PAD_TEMPLATE (GST_VIDEO_DECODER_SRC_NAME,
    GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
        GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, "NV12") ", texture-target=2D;"
        GST_VIDEO_CAPS_MAKE("NV12"))
    );
#endif

G_DEFINE_TYPE_WITH_CODE (GstNvDec, gst_nvdec, GST_TYPE_VIDEO_DECODER,
    GST_DEBUG_CATEGORY_INIT (gst_nvdec_debug_category, "nvdec", 0,
        "Debug category for the nvdec element"));


static inline gboolean
cuda_OK (CUresult result)
{
  const gchar *error_name, *error_text;

  if (result != CUDA_SUCCESS) {
    cuGetErrorName (result, &error_name);
    cuGetErrorString (result, &error_text);
    GST_WARNING ("CUDA call failed: %s, %s", error_name, error_text);
    return FALSE;
  }

  return TRUE;
}
static const char * GetVideoChromaFormatString(cudaVideoChromaFormat eChromaFormat) {
  static struct {
    cudaVideoChromaFormat eChromaFormat;
    const char *name;
  } aChromaFormatName[] = {
    { cudaVideoChromaFormat_Monochrome, "YUV 400 (Monochrome)" },
  { cudaVideoChromaFormat_420,        "YUV 420" },
  { cudaVideoChromaFormat_422,        "YUV 422" },
  { cudaVideoChromaFormat_444,        "YUV 444" },
  };

  if (eChromaFormat >= 0 && eChromaFormat < sizeof(aChromaFormatName) / sizeof(aChromaFormatName[0])) {
    return aChromaFormatName[eChromaFormat].name;
  }
  return "Unknown";
}
static const char * GetVideoCodecString(cudaVideoCodec eCodec) {
  static struct {
    cudaVideoCodec eCodec;
    const char *name;
  } aCodecName[] = {
    { cudaVideoCodec_MPEG1,     "MPEG-1" },
  { cudaVideoCodec_MPEG2,     "MPEG-2" },
  { cudaVideoCodec_MPEG4,     "MPEG-4 (ASP)" },
  { cudaVideoCodec_VC1,       "VC-1/WMV" },
  { cudaVideoCodec_H264,      "AVC/H.264" },
  { cudaVideoCodec_JPEG,      "M-JPEG" },
  { cudaVideoCodec_H264_SVC,  "H.264/SVC" },
  { cudaVideoCodec_H264_MVC,  "H.264/MVC" },
  { cudaVideoCodec_HEVC,      "H.265/HEVC" },
  { cudaVideoCodec_VP8,       "VP8" },
  { cudaVideoCodec_VP9,       "VP9" },
  { cudaVideoCodec_NumCodecs, "Invalid" },
  { cudaVideoCodec_YUV420,    "YUV  4:2:0" },
  { cudaVideoCodec_YV12,      "YV12 4:2:0" },
  { cudaVideoCodec_NV12,      "NV12 4:2:0" },
  { cudaVideoCodec_YUYV,      "YUYV 4:2:2" },
  { cudaVideoCodec_UYVY,      "UYVY 4:2:2" },
  };

  if (eCodec >= 0 && eCodec <= cudaVideoCodec_NumCodecs) {
    return aCodecName[eCodec].name;
  }
  for (int i = cudaVideoCodec_NumCodecs + 1; i < sizeof(aCodecName) / sizeof(aCodecName[0]); i++) {
    if (eCodec == aCodecName[i].eCodec) {
      return aCodecName[eCodec].name;
    }
  }
  return "Unknown";
}

#if USE_GL
static void
register_cuda_resource (GstGLContext * context, gpointer * args)
{
  GstMemory *mem = GST_MEMORY_CAST (args[0]);
  GstNvDecCudaGraphicsResourceInfo *cgr_info =
      (GstNvDecCudaGraphicsResourceInfo *) args[1];
  GstMapInfo map_info = GST_MAP_INFO_INIT;
  guint texture_id;

  if (!cuda_OK (cuvidCtxLock (cgr_info->cuda_context->lock, 0)))
    GST_WARNING ("failed to lock CUDA context");

  if (gst_memory_map (mem, &map_info, GST_MAP_READ | GST_MAP_GL)) {
    texture_id = *(guint *) map_info.data;

    if (!cuda_OK (cuGraphicsGLRegisterImage (&cgr_info->resource, texture_id,
                GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD)))
      GST_WARNING ("failed to register texture with CUDA");

    gst_memory_unmap (mem, &map_info);
  } else
    GST_WARNING ("failed to map memory");

  if (!cuda_OK (cuvidCtxUnlock (cgr_info->cuda_context->lock, 0)))
    GST_WARNING ("failed to unlock CUDA context");
}

static void
unregister_cuda_resource (GstGLContext * context,
    GstNvDecCudaGraphicsResourceInfo * cgr_info)
{
  if (!cuda_OK (cuvidCtxLock (cgr_info->cuda_context->lock, 0)))
    GST_WARNING ("failed to lock CUDA context");

  if (!cuda_OK (cuGraphicsUnregisterResource ((const CUgraphicsResource)
              cgr_info->resource)))
    GST_WARNING ("failed to unregister resource");

  if (!cuda_OK (cuvidCtxUnlock (cgr_info->cuda_context->lock, 0)))
    GST_WARNING ("failed to unlock CUDA context");
}

static void
free_cgr_info (GstNvDecCudaGraphicsResourceInfo * cgr_info)
{
  gst_gl_context_thread_add (cgr_info->gl_context,
      (GstGLContextThreadFunc) unregister_cuda_resource, cgr_info);
  gst_object_unref (cgr_info->gl_context);
  g_object_unref (cgr_info->cuda_context);
  g_slice_free (GstNvDecCudaGraphicsResourceInfo, cgr_info);
}

static CUgraphicsResource
ensure_cuda_graphics_resource (GstMemory * mem,
    GstNvDecCudaContext * cuda_context)
{
  static GQuark quark = 0;
  GstNvDecCudaGraphicsResourceInfo *cgr_info;
  gpointer args[2];

  if (!gst_is_gl_base_memory (mem)) {
    GST_WARNING ("memory is not GL base memory");
    return NULL;
  }

  if (!quark)
    quark = g_quark_from_static_string ("GstNvDecCudaGraphicsResourceInfo");

  cgr_info = gst_mini_object_get_qdata (GST_MINI_OBJECT (mem), quark);
  if (!cgr_info) {
    cgr_info = g_slice_new (GstNvDecCudaGraphicsResourceInfo);
    cgr_info->gl_context =
        gst_object_ref (GST_GL_BASE_MEMORY_CAST (mem)->context);
    cgr_info->cuda_context = g_object_ref (cuda_context);
    args[0] = mem;
    args[1] = cgr_info;
    gst_gl_context_thread_add (cgr_info->gl_context,
        (GstGLContextThreadFunc) register_cuda_resource, args);
    gst_mini_object_set_qdata (GST_MINI_OBJECT (mem), quark, cgr_info,
        (GDestroyNotify) free_cgr_info);
  }

  return cgr_info->resource;
}
#endif

static void
gst_nvdec_class_init (GstNvDecClass * klass)
{
  GstVideoDecoderClass *video_decoder_class = GST_VIDEO_DECODER_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gst_element_class_add_static_pad_template (element_class,
      &gst_nvdec_sink_template);
  gst_element_class_add_static_pad_template (element_class,
      &gst_nvdec_src_template);

  gst_element_class_set_static_metadata (element_class, "NVDEC video decoder",
      "Decoder/Video", "NVDEC video decoder",
      "Ericsson AB, http://www.ericsson.com");

  gobject_class->set_property = gst_nvdec_set_property;
  gobject_class->get_property = gst_nvdec_get_property;

  video_decoder_class->start = GST_DEBUG_FUNCPTR (gst_nvdec_start);
  video_decoder_class->stop = GST_DEBUG_FUNCPTR (gst_nvdec_stop);
  video_decoder_class->set_format = GST_DEBUG_FUNCPTR (gst_nvdec_set_format);
  video_decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_nvdec_handle_frame);
#if USE_GL
  video_decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_nvdec_decide_allocation);
  video_decoder_class->src_query = GST_DEBUG_FUNCPTR (gst_nvdec_src_query);
#endif
  video_decoder_class->drain = GST_DEBUG_FUNCPTR (gst_nvdec_drain);
  video_decoder_class->flush = GST_DEBUG_FUNCPTR (gst_nvdec_flush);

#if USE_GL
  element_class->set_context = GST_DEBUG_FUNCPTR (gst_nvdec_set_context);
#endif

  g_object_class_install_property (gobject_class, PROP_CTX,
      g_param_spec_uint64 ("context", "context",
          "Cuda Context", 0, G_MAXUINT64, 0,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_LOCK,
      g_param_spec_uint64 ("lock", "lock",
          "Cuda Context Lock", 0, G_MAXUINT64, 0,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
}

static void
gst_nvdec_init (GstNvDec * nvdec)
{
  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (nvdec), TRUE);
  gst_video_decoder_set_needs_format (GST_VIDEO_DECODER (nvdec), TRUE);
  nvdec->did_make_context = FALSE;
}

static gboolean
parser_sequence_callback (GstNvDec * nvdec, CUVIDEOFORMAT * format)
{
  GstNvDecQueueItem *item;
  guint width, height;
  CUVIDDECODECREATEINFO create_info = { 0, };
  gboolean ret = TRUE;

  width = format->display_area.right - format->display_area.left;
  height = format->display_area.bottom - format->display_area.top;
  //GST_DEBUG ("Parser callback");
  GST_DEBUG_OBJECT (nvdec, "width: %u, height: %u", width, height);

  if (!nvdec->decoder || (nvdec->width != width || nvdec->height != height)) {
    if (!cuda_OK (cuvidCtxLock (nvdec->lock, 0))) {
      GST_ERROR_OBJECT (nvdec, "failed to lock CUDA context");
      return FALSE;
    }

    if (nvdec->decoder) {
      GST_DEBUG_OBJECT (nvdec, "destroying decoder");
      if (!cuda_OK (cuvidDestroyDecoder (nvdec->decoder))) {
        GST_ERROR_OBJECT (nvdec, "failed to destroy decoder");
        ret = FALSE;
      } else
        nvdec->decoder = NULL;
    }

    GST_DEBUG_OBJECT (nvdec, "creating decoder");
    create_info.ulWidth = width;
    create_info.ulHeight = height;
    create_info.ulNumDecodeSurfaces = nvdec->num_decode_surfaces;
    create_info.CodecType = format->codec;
    create_info.ChromaFormat = format->chroma_format;
    //create_info.ulCreationFlags = cudaVideoCreate_Default;
    create_info.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    create_info.display_area.left = format->display_area.left;
    create_info.display_area.top = format->display_area.top;
    create_info.display_area.right = format->display_area.right;
    create_info.display_area.bottom = format->display_area.bottom;
    create_info.OutputFormat = cudaVideoSurfaceFormat_NV12;
    create_info.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    create_info.ulTargetWidth = width;
    create_info.ulTargetHeight = height;
    create_info.ulNumOutputSurfaces = 1;
    create_info.vidLock = nvdec->lock;
    create_info.target_rect.left = 0;
    create_info.target_rect.top = 0;
    create_info.target_rect.right = width;
    create_info.target_rect.bottom = height;

    if (nvdec->decoder)
      GST_WARNING_OBJECT(nvdec, "Already have decoder?");

    cuCtxPushCurrent(nvdec->context);
    if (nvdec->decoder
        || !cuda_OK (cuvidCreateDecoder (&nvdec->decoder, &create_info))) {
      GST_ERROR_OBJECT (nvdec, "failed to create decoder");
      ret = FALSE;
    }
    else {
        GST_DEBUG_OBJECT (nvdec, "created decoder");
    }
    cuCtxPopCurrent(NULL);

    if (!cuda_OK (cuvidCtxUnlock (nvdec->lock, 0))) {
      GST_ERROR_OBJECT (nvdec, "failed to unlock CUDA context");
      ret = FALSE;
    }
  }

  item = g_slice_new (GstNvDecQueueItem);
  item->type = GST_NVDEC_QUEUE_ITEM_TYPE_SEQUENCE;
  item->data = g_memdup (format, sizeof (CUVIDEOFORMAT));
  g_async_queue_push (nvdec->decode_queue, item);

  return ret;
}

static gboolean
parser_decode_callback (GstNvDec * nvdec, CUVIDPICPARAMS * params)
{
  GstNvDecQueueItem *item;
  //GST_DEBUG ("decode callback");

  GST_DEBUG_OBJECT (nvdec, "decoded picture index: %u", params->CurrPicIdx);

  if (!cuda_OK (cuvidCtxLock (nvdec->lock, 0)))
    GST_WARNING_OBJECT (nvdec, "failed to lock CUDA context");

  if (!cuda_OK (cuvidDecodePicture (nvdec->decoder, params)))
    GST_WARNING_OBJECT (nvdec, "failed to decode picture");

  if (!cuda_OK (cuvidCtxUnlock (nvdec->lock, 0)))
    GST_WARNING_OBJECT (nvdec, "failed to unlock CUDA context");

  item = g_slice_new (GstNvDecQueueItem);
  item->type = GST_NVDEC_QUEUE_ITEM_TYPE_DECODE;
  item->data = g_memdup (params, sizeof (CUVIDPICPARAMS));
  ((CUVIDPICPARAMS *) item->data)->pBitstreamData = NULL;
  ((CUVIDPICPARAMS *) item->data)->pSliceDataOffsets = NULL;
  g_async_queue_push (nvdec->decode_queue, item);

  return TRUE;
}

static gboolean
parser_display_callback (GstNvDec * nvdec, CUVIDPARSERDISPINFO * dispinfo)
{
  GstNvDecQueueItem *item;
  //GST_DEBUG ("display callback");

  GST_DEBUG_OBJECT (nvdec, "display picture index: %u", dispinfo->picture_index);

  item = g_slice_new (GstNvDecQueueItem);
  item->type = GST_NVDEC_QUEUE_ITEM_TYPE_DISPLAY;
  item->data = g_memdup (dispinfo, sizeof (CUVIDPARSERDISPINFO));
  g_async_queue_push (nvdec->decode_queue, item);

  return TRUE;
}

static gboolean
gst_nvdec_start (GstVideoDecoder * decoder)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);
  g_print("Start\n");

  if (nvdec->context == NULL) {

      GST_DEBUG_OBJECT (nvdec, "creating CUDA context");
      if (!cuda_OK (cuInit (0))) {
          GST_ERROR ("failed to init CUDA");
          return FALSE;
      }

      CUdevice cuDevice;
      if (!cuda_OK (cuDeviceGet (&cuDevice, 0)))
          GST_ERROR ("Failed to get device");

      // Uses 0th device
      if (!cuda_OK (cuCtxCreate (&nvdec->context, CU_CTX_SCHED_AUTO, cuDevice)))
          GST_ERROR ("failed to create CUDA context");
      nvdec->did_make_context = TRUE;
      GST_DEBUG ("Made ctx, size %i %" G_GUINT64_FORMAT " device #%i", sizeof (CUcontext), nvdec->context, cuDevice);
  }
  else {
      CUcontext current;
      cuCtxGetCurrent (&current);

      if (current == nvdec->context)
          GST_DEBUG ("Ctx is current");
      else
          GST_DEBUG ("Ctx is not current we have %" G_GUINT64_FORMAT, current);

      if (!cuda_OK (cuCtxPushCurrent (nvdec->context))) {
          GST_ERROR ("Failed pushing provided context");
          return FALSE;
      }
      GST_DEBUG ("Using provided context of %" G_GUINT64_FORMAT, nvdec->context);
      cuCtxGetCurrent (&current);
      if (current == nvdec->context)
          GST_DEBUG ("Ctx is current");
      else
          GST_WARNING ("Ctx is still not current we have " G_GUINT64_FORMAT, current);
  }

  //if (!cuda_OK (cuStreamCreate (&(nvdec->cudaStream), CU_STREAM_NON_BLOCKING)))
  if (!cuda_OK (cuStreamCreate (&(nvdec->cudaStream), CU_STREAM_DEFAULT)))
      GST_ERROR ("Failed to create the cuda stream");
  GST_DEBUG ("Made cuda stream");

  unsigned int version = 0;
  cuCtxGetApiVersion (nvdec->context, &version);
  GST_DEBUG ("Using version %u", version);

  if (!cuda_OK (cuCtxPopCurrent (NULL)))
      GST_ERROR ("failed to pop current CUDA context");


  if (nvdec->lock == NULL) {
      if (!cuda_OK (cuvidCtxLockCreate (&nvdec->lock, nvdec->context)))
          GST_ERROR ("failed to create CUDA context lock");
      GST_DEBUG ("Made lock with size %i", sizeof (nvdec->lock));
      nvdec->did_make_lock = TRUE;
  }
  else {
      GST_DEBUG ("Using provided lock");
  }

  nvdec->decode_queue = g_async_queue_new ();

  if (!nvdec->context || !nvdec->lock) {
    GST_ERROR_OBJECT (nvdec, "failed to create CUDA context or lock");
    return FALSE;
  }
  return TRUE;
}

static gboolean
maybe_destroy_decoder_and_parser (GstNvDec * nvdec)
{
  gboolean ret = TRUE;

  if (!cuda_OK (cuvidCtxLock (nvdec->lock, 0))) {
    GST_ERROR_OBJECT (nvdec, "failed to lock CUDA context");
    return FALSE;
  }

  if (nvdec->decoder) {
    GST_DEBUG_OBJECT (nvdec, "destroying decoder");
    ret = cuda_OK (cuvidDestroyDecoder (nvdec->decoder));
    if (ret)
      nvdec->decoder = NULL;
    else
      GST_ERROR_OBJECT (nvdec, "failed to destroy decoder");
  }

  if (!cuda_OK (cuvidCtxUnlock (nvdec->lock, 0))) {
    GST_ERROR_OBJECT (nvdec, "failed to unlock CUDA context");
    return FALSE;
  }

  if (nvdec->parser) {
    GST_DEBUG_OBJECT (nvdec, "destroying parser");
    if (!cuda_OK (cuvidDestroyVideoParser (nvdec->parser))) {
      GST_ERROR_OBJECT (nvdec, "failed to destroy parser");
      return FALSE;
    }
    nvdec->parser = NULL;
  }

  return ret;
}

static gboolean
gst_nvdec_stop (GstVideoDecoder * decoder)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);
  GstNvDecQueueItem *item;

  GST_DEBUG_OBJECT (nvdec, "stop");

  if (!maybe_destroy_decoder_and_parser (nvdec))
    return FALSE;

  if (nvdec->lock && nvdec->did_make_lock) {
    GST_DEBUG ("destroying CUDA context lock");
    if (cuda_OK (cuvidCtxLockDestroy (nvdec->lock)))
      nvdec->lock = NULL;
    else
      GST_ERROR ("failed to destroy CUDA context lock");
    nvdec->did_make_lock = FALSE;
  }

  if (nvdec->cudaStream) {
      GST_DEBUG ("Destroying cuda stream");
      if (cuda_OK (cuStreamDestroy (nvdec->cudaStream)))
          nvdec->cudaStream = NULL;
      else
          GST_ERROR ("Failed to destroy the cuda stream");
  }

  if (nvdec->context && nvdec->did_make_context) {
    GST_DEBUG ("destroying CUDA context");
    if (cuda_OK (cuCtxDestroy (nvdec->context))) {
        nvdec->context = NULL;
        nvdec->did_make_context = FALSE;
    } else
      GST_ERROR ("failed to destroy CUDA context");
  }

#if USE_GL
  if (nvdec->gl_context) {
    gst_object_unref (nvdec->gl_context);
    nvdec->gl_context = NULL;
  }

  if (nvdec->other_gl_context) {
    gst_object_unref (nvdec->other_gl_context);
    nvdec->other_gl_context = NULL;
  }

  if (nvdec->gl_display) {
    gst_object_unref (nvdec->gl_display);
    nvdec->gl_display = NULL;
  }
#endif
  if (nvdec->input_state) {
    gst_video_codec_state_unref (nvdec->input_state);
    nvdec->input_state = NULL;
  }

  if (nvdec->decode_queue) {
    if (g_async_queue_length (nvdec->decode_queue) > 0) {
      GST_INFO_OBJECT (nvdec, "decode queue not empty");

      while ((item = g_async_queue_try_pop (nvdec->decode_queue))) {
        g_free (item->data);
        g_slice_free (GstNvDecQueueItem, item);
      }
    }
    g_async_queue_unref (nvdec->decode_queue);
    nvdec->decode_queue = NULL;
  }
  g_list_free_full (nvdec->decode_frames_pending_drop, (GDestroyNotify) gst_video_codec_frame_unref);
  g_list_free_full (nvdec->display_frames_pending_drop, (GDestroyNotify) gst_video_codec_frame_unref);

  return TRUE;
}

static gboolean
gst_nvdec_set_format (GstVideoDecoder * decoder, GstVideoCodecState * state)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);

  GstStructure *s;
  const gchar *caps_name;
  gint mpegversion = 0;
  CUVIDPARSERPARAMS parser_params = { 0, };

  GST_DEBUG_OBJECT (nvdec, "set format");
  //g_print("Set format\n");

  if (nvdec->input_state)
    gst_video_codec_state_unref (nvdec->input_state);

  nvdec->input_state = gst_video_codec_state_ref (state);

  if (!maybe_destroy_decoder_and_parser(nvdec)) {
    GST_WARNING_OBJECT(nvdec, "maybe destroy failed\n");
    return FALSE;
  }

  s = gst_caps_get_structure (state->caps, 0);
  caps_name = gst_structure_get_name (s);
  GST_DEBUG_OBJECT (nvdec, "codec is %s", caps_name);

  if (!g_strcmp0 (caps_name, "video/mpeg")) {
    if (gst_structure_get_int (s, "mpegversion", &mpegversion)) {
      switch (mpegversion) {
        case 1:
          parser_params.CodecType = cudaVideoCodec_MPEG1;
          break;
        case 2:
          parser_params.CodecType = cudaVideoCodec_MPEG2;
          break;
        case 4:
          parser_params.CodecType = cudaVideoCodec_MPEG4;
          break;
      }
    }
    nvdec->num_decode_surfaces = NUM_SURFACES_MPEG;
    if (!mpegversion) {
      GST_ERROR_OBJECT (nvdec, "could not get MPEG version");
      return FALSE;
    }
  } else if (!g_strcmp0 (caps_name, "video/x-h264")) {
    parser_params.CodecType = cudaVideoCodec_H264;
    nvdec->num_decode_surfaces = NUM_SURFACES_H264;
  } else if (!g_strcmp0 (caps_name, "image/jpeg")) {
    parser_params.CodecType = cudaVideoCodec_JPEG;
    nvdec->num_decode_surfaces = NUM_SURFACES_JPEG;
  } else if (!g_strcmp0 (caps_name, "video/x-h265")) {
    parser_params.CodecType = cudaVideoCodec_HEVC;
    nvdec->num_decode_surfaces = NUM_SURFACES_H265;
  } else {
    GST_ERROR_OBJECT (nvdec, "failed to determine codec type");
    return FALSE;
  }

  parser_params.ulMaxNumDecodeSurfaces = nvdec->num_decode_surfaces;
  parser_params.ulErrorThreshold = 100;
  parser_params.ulMaxDisplayDelay = 0;
  parser_params.ulClockRate = GST_SECOND;
  parser_params.pUserData = nvdec;
  parser_params.pfnSequenceCallback =
      (PFNVIDSEQUENCECALLBACK) parser_sequence_callback;
  parser_params.pfnDecodePicture =
      (PFNVIDDECODECALLBACK) parser_decode_callback;
  parser_params.pfnDisplayPicture =
      (PFNVIDDISPLAYCALLBACK) parser_display_callback;

  CUVIDDECODECAPS decodecaps;
  memset(&decodecaps, 0, sizeof(decodecaps));
  decodecaps.eCodecType = parser_params.CodecType;
  decodecaps.eChromaFormat = cudaVideoChromaFormat_420; // TODO support 4:4:4 output for jpeg 
  decodecaps.nBitDepthMinus8 = 0; //TODO support 10 bit

  if (!cuda_OK (cuCtxPushCurrent (nvdec->context))) {
      return FALSE;
  }
  if (!cuda_OK (cuvidGetDecoderCaps (&decodecaps))) {
      GST_ERROR_OBJECT (nvdec, "Failed to get decode caps");
      return FALSE;
  }
  if (!cuda_OK (cuCtxPopCurrent (NULL)))
      return FALSE;

  if (!decodecaps.bIsSupported) {
    GST_ERROR_OBJECT(nvdec, "Format not supported! chroma: %s codec: %s",
      GetVideoChromaFormatString(decodecaps.eChromaFormat), GetVideoCodecString(decodecaps.eCodecType));
    return FALSE;
  }
  else {
      GST_DEBUG_OBJECT (nvdec, "Format is supported");
  }


  GST_DEBUG_OBJECT (nvdec, "creating parser");
  if (!cuda_OK (cuvidCreateVideoParser (&nvdec->parser, &parser_params))) {
    GST_ERROR_OBJECT (nvdec, "failed to create parser");
    return FALSE;
  }
  else {
    GST_DEBUG_OBJECT (nvdec, "Parser created");
  }

  GST_DEBUG_OBJECT (nvdec, "Set format worked");
  return TRUE;
}

#if USE_GL
static void
copy_video_frame_to_gl_textures (GstGLContext * context, gpointer * args)
{
  GstNvDec *nvdec = GST_NVDEC (args[0]);
  CUVIDPARSERDISPINFO *dispinfo = (CUVIDPARSERDISPINFO *) args[1];
  CUgraphicsResource *resources = (CUgraphicsResource *) args[2];
  guint num_resources = GPOINTER_TO_UINT (args[3]);
  CUVIDPROCPARAMS proc_params = { 0, };
  CUdeviceptr dptr;
  CUarray array;
  guint pitch, i;
  CUDA_MEMCPY2D mcpy2d = { 0, };

  GST_LOG_OBJECT (nvdec, "picture index: %u", dispinfo->picture_index);

  proc_params.progressive_frame = dispinfo->progressive_frame;
  proc_params.top_field_first = dispinfo->top_field_first;
  proc_params.unpaired_field = dispinfo->repeat_first_field == -1;

  if (!cuda_OK (cuvidCtxLock (nvdec->cuda_context->lock, 0))) {
    GST_WARNING_OBJECT (nvdec, "failed to lock CUDA context");
    return;
  }

  if (!cuda_OK (cuvidMapVideoFrame (nvdec->decoder, dispinfo->picture_index,
              &dptr, &pitch, &proc_params))) {
    GST_WARNING_OBJECT (nvdec, "failed to map CUDA video frame");
    goto unlock_cuda_context;
  }

  if (!cuda_OK (cuGraphicsMapResources (num_resources, resources, NULL))) {
    GST_WARNING_OBJECT (nvdec, "failed to map CUDA resources");
    goto unmap_video_frame;
  }

  mcpy2d.srcMemoryType = CU_MEMORYTYPE_DEVICE;
  mcpy2d.srcPitch = pitch;
  mcpy2d.dstMemoryType = CU_MEMORYTYPE_ARRAY;
  mcpy2d.dstPitch = nvdec->width;
  mcpy2d.WidthInBytes = nvdec->width;

  for (i = 0; i < num_resources; i++) {
    if (!cuda_OK (cuGraphicsSubResourceGetMappedArray (&array, resources[i], 0,
                0))) {
      GST_WARNING_OBJECT (nvdec, "failed to map CUDA array");
      break;
    }

    mcpy2d.srcDevice = dptr + (i * pitch * nvdec->height);
    mcpy2d.dstArray = array;
    mcpy2d.Height = nvdec->height / (i + 1);

    if (!cuda_OK (cuMemcpy2D (&mcpy2d)))
      GST_WARNING_OBJECT (nvdec, "memcpy to mapped array failed");
  }

  if (!cuda_OK (cuGraphicsUnmapResources (num_resources, resources, NULL)))
    GST_WARNING_OBJECT (nvdec, "failed to unmap CUDA resources");

unmap_video_frame:
  if (!cuda_OK (cuvidUnmapVideoFrame (nvdec->decoder, dptr)))
    GST_WARNING_OBJECT (nvdec, "failed to unmap CUDA video frame");

unlock_cuda_context:
  if (!cuda_OK (cuvidCtxUnlock (nvdec->cuda_context->lock, 0)))
    GST_WARNING_OBJECT (nvdec, "failed to unlock CUDA context");
}
#endif

static void
copy_video_frame_to_system (GstNvDec * nvdec, CUVIDPARSERDISPINFO * dispinfo, guint8 * dst_host) {
  CUVIDPROCPARAMS proc_params = { 0, };
  CUdeviceptr dptr;
  guint pitch;
  CUDA_MEMCPY2D mcpy2d = { 0, };

  GST_LOG_OBJECT (nvdec, "copying picture index: %u", dispinfo->picture_index);

  proc_params.progressive_frame = dispinfo->progressive_frame;
  proc_params.top_field_first = dispinfo->top_field_first;
  proc_params.unpaired_field = dispinfo->repeat_first_field == -1;
  proc_params.output_stream = nvdec->cudaStream;

  if (!cuda_OK (cuvidCtxLock (nvdec->lock, 0))) {
    GST_WARNING_OBJECT (nvdec, "failed to lock CUDA context");
    return;
  }

  if (!cuda_OK (cuvidMapVideoFrame (nvdec->decoder, dispinfo->picture_index,
              &dptr, &pitch, &proc_params))) {
    GST_WARNING_OBJECT (nvdec, "failed to map CUDA video frame");
    goto unlock_cuda_context;
  }

  mcpy2d.srcMemoryType = CU_MEMORYTYPE_DEVICE;
  mcpy2d.srcDevice = dptr;
  mcpy2d.srcPitch = pitch;
  mcpy2d.dstMemoryType = CU_MEMORYTYPE_HOST;
  mcpy2d.dstPitch = nvdec->stride;
  mcpy2d.dstHost = dst_host;
  mcpy2d.WidthInBytes = nvdec->width;
  //mcpy2d.Height = nvdec->height;
  mcpy2d.Height = nvdec->height + nvdec->height / 2;
  GST_DEBUG ("Copying %i pitch to %i pitch", mcpy2d.srcPitch, mcpy2d.dstPitch);

  cuCtxPushCurrent (nvdec->context);
  //if (!cuda_OK (cuStreamSynchronize (nvdec->cudaStream))) {
      //GST_WARNING_OBJECT (nvdec, "Failed to syncronize the cuda stream");
  //}
  // Copy the Y and UV planes
  //if (!cuda_OK (cuMemcpy2D(&mcpy2d))){
  if (!cuda_OK (cuMemcpy2DAsync (&mcpy2d, nvdec->cudaStream))){
    GST_WARNING_OBJECT (nvdec, "memcpy to mapped array failed Y");
  }
  if (!cuda_OK (cuStreamSynchronize (nvdec->cudaStream))) {
      GST_WARNING_OBJECT (nvdec, "Failed to syncronize the cuda stream");
  }
  /*
  CUDA_MEMCPY2D v2 = mcpy2d;
  // Copy the UV
  v2.dstPitch = nvdec->uv_stride;
  v2.srcDevice = (CUdeviceptr)(dptr + mcpy2d.srcPitch * mcpy2d.Height);
  v2.dstHost = (void*)(dst_host + mcpy2d.dstPitch * mcpy2d.Height);
  v2.Height = nvdec->height / 2; //TODO this assumes 420, which might not be the case for jpeg!

  //if (!cuda_OK (cuMemcpy2D(&mcpy2d))) {
  if (!cuda_OK (cuMemcpy2DAsync (&v2, nvdec->cudaStream))) {
    GST_WARNING_OBJECT (nvdec, "memcpy to mapped array failed UV");
  }

  if (!cuda_OK (cuStreamSynchronize (nvdec->cudaStream))) {
      GST_WARNING_OBJECT (nvdec, "Failed to syncronize the cuda stream");
  }
  */
  cuCtxPopCurrent (NULL);

  if (!cuda_OK (cuvidUnmapVideoFrame (nvdec->decoder, dptr)))
    GST_WARNING_OBJECT (nvdec, "failed to unmap CUDA video frame");

unlock_cuda_context:
  if (!cuda_OK (cuvidCtxUnlock (nvdec->lock, 0)))
    GST_WARNING_OBJECT (nvdec, "failed to unlock CUDA context");
}

static GstFlowReturn
handle_pending_frames (GstNvDec * nvdec)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (nvdec);
  GList *pending_frames, *list, *tmp;
  GstVideoCodecFrame *pending_frame;
  guint frame_number;
  GstClockTime latency = 0;
  GstNvDecQueueItem *item;
  CUVIDEOFORMAT *format;
  GstVideoCodecState *state;
  guint width, height, fps_n, fps_d;
  CUVIDPICPARAMS *decode_params;
  CUVIDPARSERDISPINFO *dispinfo;
#if USE_GL
  CUgraphicsResource *resources;
  gpointer args[4];
  guint i, num_resources;
  GstMemory *mem;
#endif
  GstFlowReturn ret = GST_FLOW_OK;
  GST_DEBUG ("In pending frames");

  /* find the oldest unused, unfinished frame */
  pending_frames = list = gst_video_decoder_get_frames (decoder);

  /* Print all waiting frames
  for (GList *pnd = list; pnd != NULL; pnd = pnd->next) {
    GstVideoCodecFrame *frame = pnd->data;
    guint num =
        GPOINTER_TO_UINT (gst_video_codec_frame_get_user_data (frame));
    GST_DEBUG ("%" G_GUINT32_FORMAT ": %" GST_TIME_FORMAT,
        num, GST_TIME_ARGS(frame->pts));
  }
  */

  // Get the first frame with a 0 frame number
  // Also measure the latency
  for (; pending_frames; pending_frames = pending_frames->next) {
    pending_frame = pending_frames->data;
    frame_number =
        GPOINTER_TO_UINT (gst_video_codec_frame_get_user_data (pending_frame));
    if (!frame_number)
        break;
    latency += pending_frame->duration;
  }
  // TODO currently we won't iterate if there are no pending
  // frames waiting for decode. Even if there are
  // frames waiting to be displayed or frames
  // that are ready to be dropped

  // Keep iterating until we error, or have no more pending frames
  while (ret == GST_FLOW_OK && pending_frames
      && (item =
          (GstNvDecQueueItem *) g_async_queue_try_pop (nvdec->decode_queue))) {
    switch (item->type) {
      case GST_NVDEC_QUEUE_ITEM_TYPE_SEQUENCE:
        GST_DEBUG ("Sequence");
        if (!nvdec->decoder) {
          GST_ERROR_OBJECT (nvdec, "no decoder");
          ret = GST_FLOW_ERROR;
          break;
        }

        format = (CUVIDEOFORMAT *) item->data;
        width = format->display_area.right - format->display_area.left;
        height = format->display_area.bottom - format->display_area.top;
        fps_n = format->frame_rate.numerator;
        fps_d = MAX (1, format->frame_rate.denominator);
        GST_DEBUG ("Sequence A");

        if (!gst_pad_has_current_caps (GST_VIDEO_DECODER_SRC_PAD (decoder))
            || width != nvdec->width || height != nvdec->height
            || fps_n != nvdec->fps_n || fps_d != nvdec->fps_d) {
          GST_DEBUG ("Sequence B");
          nvdec->width = width;
          nvdec->height = height;
          nvdec->fps_n = fps_n;
          nvdec->fps_d = fps_d;

          state = gst_video_decoder_set_output_state (decoder,
              GST_VIDEO_FORMAT_NV12, nvdec->width, nvdec->height,
              nvdec->input_state);
          GST_DEBUG ("Sequence C");
          state->caps = gst_caps_new_simple ("video/x-raw",
              "format", G_TYPE_STRING, "NV12",
              "width", G_TYPE_INT, nvdec->width,
              "height", G_TYPE_INT, nvdec->height,
              "framerate", GST_TYPE_FRACTION, nvdec->fps_n, nvdec->fps_d,
              "interlace-mode", G_TYPE_STRING, format->progressive_sequence
              ? "progressive" : "interleaved",
              "texture-target", G_TYPE_STRING, "2D", NULL);
          nvdec->stride = state->info.stride[0];
          GST_DEBUG ("Stride is %i", nvdec->stride);
          GST_DEBUG ("Sequence D");
#if USE_GL
          gst_caps_set_features (state->caps, 0,
              gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, NULL));
#endif
          gst_video_codec_state_unref (state);

          GST_DEBUG ("Sequence E");
          if (!gst_video_decoder_negotiate (decoder)) {
              GST_DEBUG ("Sequence F");
            GST_WARNING_OBJECT (nvdec, "failed to negotiate with downstream");
            ret = GST_FLOW_NOT_NEGOTIATED;
            GST_WARNING ("Not Negotiated\n");
            break;
          }

          GST_DEBUG ("Negotiated");
        }
        break;

      case GST_NVDEC_QUEUE_ITEM_TYPE_DECODE:
        // If we have a decode item, then use the oldest
        // frame without a frame number
        decode_params = (CUVIDPICPARAMS *) item->data;

        // First check if this is a frame that needs to be dropped
        if (nvdec->decode_frames_pending_drop != NULL) {
            //guint len = g_list_length (nvdec->decode_frames_pending_drop);
            pending_frame = nvdec->decode_frames_pending_drop->data;
            nvdec->decode_frames_pending_drop = g_list_remove (nvdec->decode_frames_pending_drop, pending_frame);
            //GST_DEBUG_OBJECT (nvdec, "Will drop decode frame, len: %" G_GUINT32_FORMAT, len);
            // Add this frame to the list of display frames ready to be dropped
            nvdec->display_frames_pending_drop = g_list_append (nvdec->display_frames_pending_drop, pending_frame);
        }
        else {
            pending_frame = pending_frames->data;
            pending_frames = pending_frames->next;
        }

        frame_number = decode_params->CurrPicIdx + 1;
        GST_DEBUG ("Decode %" G_GUINT32_FORMAT, frame_number);

        gst_video_codec_frame_set_user_data (pending_frame,
            GUINT_TO_POINTER (frame_number), NULL);

        if (decode_params->intra_pic_flag)
          GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT (pending_frame);

        if (!GST_CLOCK_TIME_IS_VALID (pending_frame->duration)) {
          pending_frame->duration =
              nvdec->fps_n ? GST_SECOND * nvdec->fps_d / nvdec->fps_n : 0;
        }
        latency += pending_frame->duration;
        GST_DEBUG_OBJECT (nvdec, "Done with decode");

        break;

      case GST_NVDEC_QUEUE_ITEM_TYPE_DISPLAY:
          // If we get a display item, find the previously
          // set pending frame using the frame number
        dispinfo = (CUVIDPARSERDISPINFO *) item->data;
        GST_DEBUG ("Display %" G_GUINT32_FORMAT, dispinfo->picture_index + 1);
        pending_frame = NULL;

        // First, try to find the display frame in the display frames ready to be dropped
        for (tmp = nvdec->display_frames_pending_drop;
            !pending_frame && tmp;
            tmp = tmp->next) {
            frame_number =
                GPOINTER_TO_UINT (gst_video_codec_frame_get_user_data
                (tmp->data));
            //GST_DEBUG ("drop display #%" G_GUINT32_FORMAT, frame_number);
            if (frame_number == dispinfo->picture_index + 1)
                pending_frame = tmp->data;
        }
        // If we found the frame, remove it from the list
        if (pending_frame) {
            GST_DEBUG_OBJECT (nvdec, "Using dropped frame");
            nvdec->display_frames_pending_drop = g_list_remove (nvdec->display_frames_pending_drop, pending_frame);
            // We're now done with this frame, so just unref
            gst_video_codec_frame_unref (pending_frame);
            break;
        }

        // If we didn't find the frame in the dropping display list, check
        // pending_frames
        for (pending_frame = NULL, tmp = list; !pending_frame && tmp;
            tmp = tmp->next) {
            frame_number =
                GPOINTER_TO_UINT (gst_video_codec_frame_get_user_data
                (tmp->data));
            if (frame_number == dispinfo->picture_index + 1)
                pending_frame = tmp->data;
        }

        // If we still did not find the frame, then an error has likely occurred
        if (!pending_frame) {
            GST_WARNING_OBJECT (nvdec, "no frame with number %u",
                dispinfo->picture_index + 1);
            break;
        }

        // Make sure the timestamps are the same
        if (dispinfo->timestamp != pending_frame->pts) {
            GST_WARNING_OBJECT (nvdec,
                "timestamp mismatch, frame: %" GST_TIME_FORMAT
                " nvdisp: %" GST_TIME_FORMAT,
                GST_TIME_ARGS (pending_frame->pts),
                GST_TIME_ARGS (dispinfo->timestamp));
          //pending_frame->pts = dispinfo->timestamp;
        }
          GST_DEBUG_OBJECT (nvdec,
              "displaying ts: %" GST_TIME_FORMAT,
              GST_TIME_ARGS (pending_frame->pts));

        if (latency > nvdec->min_latency) {
          nvdec->min_latency = latency;
          gst_video_decoder_set_latency (decoder, nvdec->min_latency,
              nvdec->min_latency);
          GST_DEBUG_OBJECT (nvdec, "latency: %" GST_TIME_FORMAT,
              GST_TIME_ARGS (latency));
        }
        latency -= pending_frame->duration;

        ret = gst_video_decoder_allocate_output_frame (decoder, pending_frame);
        if (ret != GST_FLOW_OK) {
          GST_WARNING_OBJECT (nvdec, "failed to allocate output frame");
          break;
        }

#if USE_GL
        num_resources = gst_buffer_n_memory (pending_frame->output_buffer);
        resources = g_new (CUgraphicsResource, num_resources);

        for (i = 0; i < num_resources; i++) {
          mem = gst_buffer_get_memory (pending_frame->output_buffer, i);
          resources[i] =
              ensure_cuda_graphics_resource (mem, nvdec->cuda_context);
          GST_MINI_OBJECT_FLAG_SET (mem,
              GST_GL_BASE_MEMORY_TRANSFER_NEED_DOWNLOAD);
          gst_memory_unref (mem);
        }

        args[0] = nvdec;
        args[1] = dispinfo;
        args[2] = resources;
        args[3] = GUINT_TO_POINTER (num_resources);
        gst_gl_context_thread_add (nvdec->gl_context,
            (GstGLContextThreadFunc) copy_video_frame_to_gl_textures, args);
        g_free (resources);
#endif
        GstMapInfo map = GST_MAP_INFO_INIT;
        if (!gst_buffer_map (pending_frame->output_buffer, &map, GST_MAP_WRITE)) {
            GST_WARNING_OBJECT (nvdec, "Failed to map for display!");
            break;
        }
        GST_DEBUG ("Copying %d bytes to system", (int)map.size);
        copy_video_frame_to_system (nvdec, dispinfo, map.data);
        gst_buffer_unmap (pending_frame->output_buffer, &map);

        if (!dispinfo->progressive_frame) {
          GST_BUFFER_FLAG_SET (pending_frame->output_buffer,
              GST_VIDEO_BUFFER_FLAG_INTERLACED);

          if (dispinfo->top_field_first) {
            GST_BUFFER_FLAG_SET (pending_frame->output_buffer,
                GST_VIDEO_BUFFER_FLAG_TFF);
          }
          if (dispinfo->repeat_first_field == -1) {
            GST_BUFFER_FLAG_SET (pending_frame->output_buffer,
                GST_VIDEO_BUFFER_FLAG_ONEFIELD);
          } else {
            GST_BUFFER_FLAG_SET (pending_frame->output_buffer,
                GST_VIDEO_BUFFER_FLAG_RFF);
          }
        }
        list = g_list_remove (list, pending_frame);
        ret = gst_video_decoder_finish_frame (decoder, pending_frame);
        if (ret != GST_FLOW_OK)
          GST_INFO_OBJECT (nvdec, "failed to finish frame");

        break;

      default:
        g_assert_not_reached ();
    }

    g_free (item->data);
    g_slice_free (GstNvDecQueueItem, item);
  }

  // getting the pending frames from the base class refs them,
  // so we unref everything here
  g_list_free_full (list, (GDestroyNotify) gst_video_codec_frame_unref);

  //g_print("Done handling frame %s\n", gst_flow_get_name(ret));
  GST_DEBUG ("pending frames done");
  return ret;
}

static GstFlowReturn
gst_nvdec_handle_frame (GstVideoDecoder * decoder, GstVideoCodecFrame * frame)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);
  GstMapInfo map_info = GST_MAP_INFO_INIT;
  CUVIDSOURCEDATAPACKET packet = { 0, };

  GST_LOG_OBJECT (nvdec,
      "handling frame ts: %" GST_TIME_FORMAT,
      GST_TIME_ARGS (frame->pts));

  gst_video_codec_frame_set_user_data (frame, GUINT_TO_POINTER (0), NULL);

  if (!gst_buffer_map (frame->input_buffer, &map_info, GST_MAP_READ)) {
    GST_ERROR_OBJECT (nvdec, "failed to map input buffer");
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }

  packet.payload_size = (gulong) map_info.size;
  packet.payload = map_info.data;
  packet.timestamp = frame->pts;
  packet.flags = CUVID_PKT_TIMESTAMP;

  if (GST_BUFFER_IS_DISCONT (frame->input_buffer)) {
      GST_DEBUG_OBJECT (nvdec, "Adding discontinuity");
      packet.flags |= CUVID_PKT_DISCONTINUITY;
  }

  if (!cuda_OK (cuvidParseVideoData (nvdec->parser, &packet)))
    GST_WARNING_OBJECT (nvdec, "parser failed");

  gst_buffer_unmap (frame->input_buffer, &map_info);
  gst_video_codec_frame_unref (frame);

  return handle_pending_frames (nvdec);
}

static gboolean
gst_nvdec_flush (GstVideoDecoder * decoder)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);
  GST_DEBUG_OBJECT (nvdec, "flush");

  // Nvidia doesn't let us drop frames that are "in-flight"
  // So to flush we just note the frames that are currently in flight
  // and we drop them once they've been fully decoded
  // we also notify the base class that we're done with the
  // current pending frames

  GList* pending_frames = gst_video_decoder_get_frames (decoder);
  GList* list = pending_frames;
  for (; pending_frames != NULL; pending_frames = pending_frames->next) {
      // Keep track of the frames that are being dropped
      GstVideoCodecFrame* frame = pending_frames->data;
      guint frame_number =
          GPOINTER_TO_UINT (gst_video_codec_frame_get_user_data (frame));

      // If the frame number is 0, this frame is pending decode
      // if it has a frame number, it's pending display
      if (frame_number == 0) {
          nvdec->decode_frames_pending_drop = g_list_append (nvdec->decode_frames_pending_drop, frame);
          GST_DEBUG_OBJECT (nvdec, "Adding to decode drop: %" GST_TIME_FORMAT,
              GST_TIME_ARGS(frame->pts));
      }
      else {
          nvdec->display_frames_pending_drop = g_list_append (nvdec->display_frames_pending_drop, frame);
          GST_DEBUG_OBJECT (nvdec, "Adding to display drop: %" G_GUINT32_FORMAT
              " %" GST_TIME_FORMAT,
              frame_number,
              GST_TIME_ARGS(frame->pts));
      }

      // drop the frame, but keep a ref for until it's fully decoded
      gst_video_codec_frame_ref (frame);
      gst_video_decoder_release_frame (decoder, frame);
  }
  // Clear out our list (GstVideoDecoder gives us a ref'd copy)
  g_list_free (list);

  GST_DEBUG_OBJECT (nvdec, "flushed");
  return TRUE;
}

static GstFlowReturn
gst_nvdec_drain (GstVideoDecoder * decoder)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);
  //CUVIDSOURCEDATAPACKET packet = { 0, };
  GST_DEBUG_OBJECT (nvdec, "draining decoder");
  //packet.payload_size = 0;
  //packet.payload = NULL;
  //packet.flags = CUVID_PKT_ENDOFSTREAM;

  //if (nvdec->parser && !cuda_OK (cuvidParseVideoData (nvdec->parser, &packet)))
    //GST_WARNING_OBJECT (nvdec, "parser failed");

  GstFlowReturn ret = handle_pending_frames (nvdec);
  GST_DEBUG_OBJECT (nvdec, "decoder drained");
  return ret;
}

void gst_nvdec_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
    GstNvDec *nvdec = GST_NVDEC (object);
    switch (prop_id) {
    case PROP_CTX:
        nvdec->did_make_context = FALSE;
        uint64_t ctx_uint = g_value_get_uint64 (value);
        nvdec->context = (CUcontext)ctx_uint; //TODO this looks real fucking dangerous...
        break;
    case PROP_LOCK:
        nvdec->did_make_lock = FALSE;
        uint64_t lock_uint = g_value_get_uint64 (value);
        nvdec->lock = (CUvideoctxlock)lock_uint; //TODO this looks real fucking dangerous...
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

void gst_nvdec_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
    GstNvDec *nvdec = GST_NVDEC (object);
    switch (prop_id) {
    case PROP_CTX:
        g_value_set_uint64 (value, (guint64)nvdec->context);
        //TODO this looks real fucking dangerous...
        break;
    case PROP_LOCK:
        g_value_set_uint64 (value, (guint64)nvdec->lock);
        //TODO this looks real fucking dangerous...
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

#if USE_GL
static gboolean
gst_nvdec_decide_allocation (GstVideoDecoder * decoder, GstQuery * query)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);
  GstCaps *outcaps;
  GstBufferPool *pool = NULL;
  guint n, size, min, max;
  GstVideoInfo vinfo = { 0, };
  GstStructure *config;

  GST_DEBUG_OBJECT (nvdec, "decide allocation");

  if (!gst_gl_ensure_element_data (nvdec, &nvdec->gl_display,
          &nvdec->other_gl_context)) {
    GST_ERROR_OBJECT (nvdec, "failed to ensure OpenGL display");
    return FALSE;
  }

  if (!gst_gl_query_local_gl_context (GST_ELEMENT (decoder), GST_PAD_SRC,
          &nvdec->gl_context)) {
    GST_INFO_OBJECT (nvdec, "failed to query local OpenGL context");
    if (nvdec->gl_context)
      gst_object_unref (nvdec->gl_context);
    nvdec->gl_context =
        gst_gl_display_get_gl_context_for_thread (nvdec->gl_display, NULL);
    if (!nvdec->gl_context
        || !gst_gl_display_add_context (nvdec->gl_display, nvdec->gl_context)) {
      if (nvdec->gl_context)
        gst_object_unref (nvdec->gl_context);
      if (!gst_gl_display_create_context (nvdec->gl_display,
              nvdec->other_gl_context, &nvdec->gl_context, NULL)) {
        GST_ERROR_OBJECT (nvdec, "failed to create OpenGL context");
        return FALSE;
      }
      if (!gst_gl_display_add_context (nvdec->gl_display, nvdec->gl_context)) {
        GST_ERROR_OBJECT (nvdec,
            "failed to add the OpenGL context to the display");
        return FALSE;
      }
    }
  }

  gst_query_parse_allocation (query, &outcaps, NULL);
  n = gst_query_get_n_allocation_pools (query);
  if (n > 0) {
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);
    if (!GST_IS_GL_BUFFER_POOL (pool)) {
      gst_object_unref (pool);
      pool = NULL;
    }
  }

  if (!pool) {
    pool = gst_gl_buffer_pool_new (nvdec->gl_context);

    if (outcaps)
      gst_video_info_from_caps (&vinfo, outcaps);
    size = (guint) vinfo.size;
    min = max = 0;
  }

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, outcaps, size, min, max);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  gst_buffer_pool_set_config (pool, config);
  if (n > 0)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
  else
    gst_query_add_allocation_pool (query, pool, size, min, max);
  gst_object_unref (pool);

  return GST_VIDEO_DECODER_CLASS (gst_nvdec_parent_class)->decide_allocation
      (decoder, query);
}
#endif

#if USE_GL
static gboolean
gst_nvdec_src_query (GstVideoDecoder * decoder, GstQuery * query)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
      if (gst_gl_handle_context_query (GST_ELEMENT (decoder), query,
              nvdec->gl_display, nvdec->gl_context, nvdec->other_gl_context))
        return TRUE;
      break;
    default:
      break;
  }

  return GST_VIDEO_DECODER_CLASS (gst_nvdec_parent_class)->src_query (decoder,
      query);
}
#endif

#if USE_GL
static void
gst_nvdec_set_context (GstElement * element, GstContext * context)
{
  GstNvDec *nvdec = GST_NVDEC (element);
  GST_DEBUG_OBJECT (nvdec, "set context");

  gst_gl_handle_set_context (element, context, &nvdec->gl_display,
      &nvdec->other_gl_context);

  GST_ELEMENT_CLASS (gst_nvdec_parent_class)->set_context (element, context);
}
#endif

static gboolean
plugin_init(GstPlugin * plugin)
{
    //TODO check if NVDEC is supported before registering
  GST_DEBUG_CATEGORY_INIT(gst_nvdec_debug_category, "nvdec",
    0, "Template nvdec");

  // Don't register this device if the user doesn't have an nvidia gpu
  // nvcuvid is included in the display driver and is the dll for nvdec
  //if (LoadLibraryA ("nvcuvid.dll") == NULL)
      //return TRUE;

  return gst_element_register(plugin, "nvdec", GST_RANK_PRIMARY + 1,
    GST_TYPE_NVDEC);
}

#ifndef PACKAGE
#define PACKAGE "someapp"
#endif

GST_PLUGIN_DEFINE(
  GST_VERSION_MAJOR,
  GST_VERSION_MINOR,
  nvidia,
  "Nvidia hardware decoder",
  plugin_init,
  "0.0.1",
  "BSD",
  "here",
  "me"
)