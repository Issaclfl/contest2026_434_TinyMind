/****************************************************************************
 * vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/src/bsp_audio.c
 *
 * NuttX audio lower-half driver for the SF32LB52 on-chip audio codec
 * (AUDCODEC analog path + AUDPRC digital routing).
 *
 * Hardware path (SF32LB52-DevKit-LCD):
 *   capture: onboard analog MEMS mic -> AUDCODEC ADC -> AUDPRC -> DMA -> RAM
 *   playback: RAM -> DMA -> AUDPRC -> AUDCODEC DAC -> Class-D PA (PA10) -> spk
 *
 * M1: framework integration (audio_ops_s plumbing, /dev/audio0).
 * M2: hardware bring-up via bsp_audio_hw.c (clocks, routing, anti-pop).
 * M3: DMA buffer streaming in enqueuebuffer().
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.  See the License for the specific language governing
 * permissions and limitations under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/kmalloc.h>
#include <nuttx/audio/audio.h>

#include "bsp_audio.h"
#include "bsp_audio_hw.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Supported capture formats (M1 report; M3 narrows to what we program). */

#define SF32LB52_AUDIO_SAMPRATES                                                \
  (AUDIO_SAMP_RATE_8K | AUDIO_SAMP_RATE_16K | AUDIO_SAMP_RATE_44K |             \
   AUDIO_SAMP_RATE_48K)

/* NuttX FEATURE gain scale is 0..1000; our PGA range is 0..30dB. */

#define SF32LB52_GAIN_SCALE_MAX   1000
#define SF32LB52_GAIN_DB_MAX      30

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sf32lb52_audio_s
{
  struct audio_lowerhalf_s dev;   /* Terminating "base class", must be first */
  uint32_t samprate;              /* Configured sample rate (Hz) */
  uint8_t  nchannels;             /* Configured channel count */
  uint8_t  bpsamp;                /* Configured bits per sample */
  uint8_t  dir;                   /* enum sf32lb52_audio_dir_e */
  bool     configured;            /* True after a successful configure */
  bool     running;               /* True while the stream is active */
  bool     reserved;              /* Single-session reservation flag */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int sf32lb52_getcaps(FAR struct audio_lowerhalf_s *dev, int type,
                            FAR struct audio_caps_s *caps);
static int sf32lb52_configure(FAR struct audio_lowerhalf_s *dev,
                              FAR const struct audio_caps_s *caps);
static int sf32lb52_shutdown(FAR struct audio_lowerhalf_s *dev);
static int sf32lb52_start(FAR struct audio_lowerhalf_s *dev);
#ifndef CONFIG_AUDIO_EXCLUDE_STOP
static int sf32lb52_stop(FAR struct audio_lowerhalf_s *dev);
#endif
#ifndef CONFIG_AUDIO_EXCLUDE_PAUSE_RESUME
static int sf32lb52_pause(FAR struct audio_lowerhalf_s *dev);
static int sf32lb52_resume(FAR struct audio_lowerhalf_s *dev);
#endif
static int sf32lb52_enqueuebuffer(FAR struct audio_lowerhalf_s *dev,
                                  FAR struct ap_buffer_s *apb);
static int sf32lb52_cancelbuffer(FAR struct audio_lowerhalf_s *dev,
                                 FAR struct ap_buffer_s *apb);
static int sf32lb52_ioctl(FAR struct audio_lowerhalf_s *dev, int cmd,
                          unsigned long arg);
static int sf32lb52_reserve(FAR struct audio_lowerhalf_s *dev);
static int sf32lb52_release(FAR struct audio_lowerhalf_s *dev);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct audio_ops_s g_sf32lb52_audio_ops =
{
  .getcaps       = sf32lb52_getcaps,
  .configure     = sf32lb52_configure,
  .shutdown      = sf32lb52_shutdown,
  .start         = sf32lb52_start,
#ifndef CONFIG_AUDIO_EXCLUDE_STOP
  .stop          = sf32lb52_stop,
#endif
#ifndef CONFIG_AUDIO_EXCLUDE_PAUSE_RESUME
  .pause         = sf32lb52_pause,
  .resume        = sf32lb52_resume,
#endif
  .allocbuffer   = NULL,          /* M3: DMA-aligned buffers */
  .freebuffer    = NULL,
  .enqueuebuffer = sf32lb52_enqueuebuffer,
  .cancelbuffer  = sf32lb52_cancelbuffer,
  .ioctl         = sf32lb52_ioctl,
  .read          = NULL,
  .write         = NULL,
  .reserve       = sf32lb52_reserve,
  .release       = sf32lb52_release,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sf32lb52_getcaps
 *
 * Description:
 *   Report device capabilities.  Called by the upper-half on AUDIOIOC ioctl
 *   from applications.  Fill conventions follow audio_null.c.
 *
 ****************************************************************************/

static int sf32lb52_getcaps(FAR struct audio_lowerhalf_s *dev, int type,
                            FAR struct audio_caps_s *caps)
{
  int ret = -ENOTTY;

  /* Validate the structure */

  if (caps == NULL || caps->ac_len < sizeof(struct audio_caps_s))
    {
      return -EINVAL;
    }

  switch (type)
    {
      case AUDIO_TYPE_QUERY:                 /* Overall device query */
        if (caps->ac_subtype == AUDIO_TYPE_QUERY)
          {
            /* Report we are an input (capture) device supporting PCM */

            caps->ac_channels = 2;         /* 1 min .. 2 max channels */
            caps->ac_format.hw = 1 << (AUDIO_FMT_PCM - 1);
            caps->ac_controls.b[0] = AUDIO_TYPE_INPUT;
            ret = caps->ac_len;
          }
        break;

      case AUDIO_TYPE_INPUT:                 /* Input device detail query */
        caps->ac_channels = 2;
        caps->ac_format.hw = 1 << (AUDIO_FMT_PCM - 1);
        caps->ac_controls.hw[0] = SF32LB52_AUDIO_SAMPRATES;
        ret = caps->ac_len;
        break;

      case AUDIO_TYPE_FEATURE:               /* Feature unit query */
        if (caps->ac_subtype == AUDIO_FU_UNDEF)
          {
            /* Report input gain (mic PGA) support */

            caps->ac_controls.b[0] = AUDIO_FU_INP_GAIN;
            ret = caps->ac_len;
          }
        break;

      default:
        break;
    }

  return ret;
}

/****************************************************************************
 * Name: sf32lb52_configure
 *
 * Description:
 *   Bind the driver to the requested mode: store the format and program the
 *   hardware routing/clocks through the hw layer (no streaming yet).
 *
 ****************************************************************************/

static int sf32lb52_configure(FAR struct audio_lowerhalf_s *dev,
                              FAR const struct audio_caps_s *caps)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;
  int ret;

  switch (caps->ac_type)
    {
      case AUDIO_TYPE_INPUT:
        {
          uint32_t samprate = caps->ac_controls.hw[0];
          uint8_t bpsamp = caps->ac_controls.b[2];
          uint8_t nchannels = caps->ac_channels & 0x0f;

          if (samprate == 0 || bpsamp == 0 || nchannels == 0)
            {
              return -EINVAL;
            }

          ret = sf32lb52_audio_hw_config_capture(samprate, nchannels,
                                                 bpsamp);
          if (ret != OK)
            {
              return ret;
            }

          priv->samprate = samprate;
          priv->bpsamp = bpsamp;
          priv->nchannels = nchannels;
          priv->dir = SF32LB52_AUDIO_CAPTURE;
          priv->configured = true;

          _info("configure: rate=%lu ch=%u bits=%u\n",
                (unsigned long)samprate, nchannels, bpsamp);
        }
        break;

      case AUDIO_TYPE_OUTPUT:
        {
          uint32_t samprate = caps->ac_controls.hw[0];
          uint8_t bpsamp = caps->ac_controls.b[2];
          uint8_t nchannels = caps->ac_channels & 0x0f;

          if (samprate == 0 || bpsamp == 0 || nchannels == 0)
            {
              return -EINVAL;
            }

          ret = sf32lb52_audio_hw_config_playback(samprate, nchannels,
                                                  bpsamp);
          if (ret != OK)
            {
              return ret;
            }

          priv->samprate = samprate;
          priv->bpsamp = bpsamp;
          priv->nchannels = nchannels;
          priv->dir = SF32LB52_AUDIO_PLAYBACK;
          priv->configured = true;

          _info("configure: rate=%lu ch=%u bits=%u\n",
                (unsigned long)samprate, nchannels, bpsamp);
        }
        break;

      case AUDIO_TYPE_FEATURE:
        {
          uint16_t fu = caps->ac_subtype;

          if (fu == AUDIO_FU_INP_GAIN)
            {
              uint32_t scale = caps->ac_controls.hw[0];

              if (scale > SF32LB52_GAIN_SCALE_MAX)
                {
                  scale = SF32LB52_GAIN_SCALE_MAX;
                }

              ret = sf32lb52_audio_hw_set_mic_gain(
                (int)(scale * SF32LB52_GAIN_DB_MAX /
                      SF32LB52_GAIN_SCALE_MAX));
              if (ret != OK)
                {
                  return ret;
                }
            }
        }
        break;

      default:
        return -ENOTTY;
    }

  return OK;
}

/****************************************************************************
 * Name: sf32lb52_shutdown
 ****************************************************************************/

static int sf32lb52_shutdown(FAR struct audio_lowerhalf_s *dev)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;

  _info("shutdown\n");

#ifndef CONFIG_AUDIO_EXCLUDE_STOP
  if (priv->running)
    {
      sf32lb52_stop(dev);
    }
#endif

  /* Make sure both directions are torn down regardless of state. */

  sf32lb52_audio_hw_stop(SF32LB52_AUDIO_CAPTURE);
  sf32lb52_audio_hw_stop(SF32LB52_AUDIO_PLAYBACK);
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_start
 ****************************************************************************/

static int sf32lb52_start(FAR struct audio_lowerhalf_s *dev)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;
  int ret;

  if (!priv->configured)
    {
      return -EAGAIN;
    }

  if (priv->dir == SF32LB52_AUDIO_CAPTURE)
    {
      /* M2: bring the analog path up with DMA gated (buf = NULL).  M3
       * passes the first ap_buffer here instead, enabling the stream. */

      ret = sf32lb52_audio_hw_start(SF32LB52_AUDIO_CAPTURE, NULL, 0);
      if (ret != OK)
        {
          return ret;
        }
    }
  else
    {
      /* Playback streams are started from enqueuebuffer() once the first
       * data buffer is available (M3); starting without data would just
       * push silence through the anti-pop sequence. */

      _info("playback starts with the first buffer (M3)\n");
      return -ENOSYS;
    }

  priv->running = true;
  return OK;
}

#ifndef CONFIG_AUDIO_EXCLUDE_STOP
/****************************************************************************
 * Name: sf32lb52_stop
 ****************************************************************************/

static int sf32lb52_stop(FAR struct audio_lowerhalf_s *dev)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;

  _info("stop\n");

  sf32lb52_audio_hw_stop(priv->dir);
  priv->running = false;
  return OK;
}
#endif

#ifndef CONFIG_AUDIO_EXCLUDE_PAUSE_RESUME
/****************************************************************************
 * Name: sf32lb52_pause
 ****************************************************************************/

static int sf32lb52_pause(FAR struct audio_lowerhalf_s *dev)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;

  _info("pause\n");
  sf32lb52_audio_hw_stop(priv->dir);
  priv->running = false;
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_resume
 ****************************************************************************/

static int sf32lb52_resume(FAR struct audio_lowerhalf_s *dev)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;

  _info("resume\n");
  priv->running = true;
  return OK;
}
#endif

/****************************************************************************
 * Name: sf32lb52_enqueuebuffer
 *
 * Description:
 *   Non-blocking buffer enqueue.  M2 stub: no DMA backend yet, so reject
 *   cleanly instead of silently swallowing buffers; the upper-half will
 *   surface the error to the caller.
 *
 *   M3 replaces this with: append to pendq, arm/refresh the RX DMA, and
 *   start playback streams on their first buffer.
 *
 * Input Parameters:
 *   dev - Lower-half device
 *   apb - The audio pipeline buffer to enqueue
 *
 * Returned Value:
 *   OK on success (buffer accepted); -ENOSYS until the DMA backend lands.
 *
 ****************************************************************************/

static int sf32lb52_enqueuebuffer(FAR struct audio_lowerhalf_s *dev,
                                  FAR struct ap_buffer_s *apb)
{
  _info("enqueuebuffer: no DMA backend yet (M3)\n");
  return -ENOSYS;
}

/****************************************************************************
 * Name: sf32lb52_cancelbuffer
 ****************************************************************************/

static int sf32lb52_cancelbuffer(FAR struct audio_lowerhalf_s *dev,
                                 FAR struct ap_buffer_s *apb)
{
  _info("cancelbuffer\n");

  /* M3: drop apb from pendq if it is queued. */

  return OK;
}

/****************************************************************************
 * Name: sf32lb52_ioctl
 ****************************************************************************/

static int sf32lb52_ioctl(FAR struct audio_lowerhalf_s *dev, int cmd,
                          unsigned long arg)
{
  switch (cmd)
    {
      case AUDIOIOC_GETBUFFERINFO:
        {
          FAR struct ap_buffer_info_s *info =
            (FAR struct ap_buffer_info_s *)((uintptr_t)arg);

          if (info == NULL)
            {
              return -EFAULT;
            }

          /* 16 kHz, 16-bit, mono: 8 KiB is 256 ms per buffer.  Two
           * buffers are enough for the upper-half to double-buffer. */

          info->buffer_size = CONFIG_AUDIO_BUFFER_NUMBYTES;
          info->nbuffers = CONFIG_AUDIO_NUM_BUFFERS;
        }
        return OK;

      case AUDIOIOC_REGISTERMQ:
      case AUDIOIOC_UNREGISTERMQ:
        /* The upper-half handles these before reaching us. */

        return OK;

      default:
        return -ENOTTY;
    }
}

/****************************************************************************
 * Name: sf32lb52_reserve
 ****************************************************************************/

static int sf32lb52_reserve(FAR struct audio_lowerhalf_s *dev)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;

  if (priv->reserved)
    {
      return -EBUSY;
    }

  priv->reserved = true;
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_release
 ****************************************************************************/

static int sf32lb52_release(FAR struct audio_lowerhalf_s *dev)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;

  priv->reserved = false;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sf32lb52_audio_initialize
 *
 * Description:
 *   Instantiate the on-chip audio lower-half driver.  See bsp_audio.h.
 *
 ****************************************************************************/

FAR struct audio_lowerhalf_s *sf32lb52_audio_initialize(void)
{
  FAR struct sf32lb52_audio_s *priv;
  int ret;

  priv = (FAR struct sf32lb52_audio_s *)
    kmm_zalloc(sizeof(struct sf32lb52_audio_s));
  if (priv == NULL)
    {
      _err("ERROR: failed to allocate audio driver state\n");
      return NULL;
    }

  priv->dev.ops = &g_sf32lb52_audio_ops;
  priv->samprate = 16000;
  priv->nchannels = 1;
  priv->bpsamp = 16;
  priv->dir = SF32LB52_AUDIO_CAPTURE;

  /* M2: power/clock/routing bring-up so the hardware is ready before the
   * first configure/start. */

  ret = sf32lb52_audio_hw_init();
  if (ret != OK)
    {
      _err("ERROR: audio hw init failed: %d\n", ret);
      kmm_free(priv);
      return NULL;
    }

  return &priv->dev;
}
