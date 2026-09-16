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
#include <nuttx/queue.h>
#include <nuttx/irq.h>

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

  /* M3 streaming state.  pendq holds the ap_buffers the upper half has
   * handed us; they are serviced one staging half at a time from the DMA
   * interrupt, so pendq is touched from both task and interrupt context and
   * every access is made inside a critical section. */

  dq_queue_t pendq;               /* Pending ap_buffers (FIFO) */
  uint32_t   overruns;            /* Halves dropped because pendq ran dry */
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

/* M3 streaming helpers */

static void sf32lb52_stage_callback(uint8_t dir, uint8_t half);
static uint32_t sf32lb52_serve_half(FAR struct sf32lb52_audio_s *priv,
                                    uint8_t half);
static void sf32lb52_prime_playback(FAR struct sf32lb52_audio_s *priv);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* This is a single-instance board driver (the codec, the PA enable line and
 * the staging area are all singletons, and reserve()/release() enforce one
 * session), so the hw layer's stage callback - which carries no context -
 * resolves back to the one instance through this pointer. */

static FAR struct sf32lb52_audio_s *g_audio_priv;

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
 * Streaming (M3)
 ****************************************************************************/

/****************************************************************************
 * Name: sf32lb52_serve_half
 *
 * Description:
 *   Move one staging half to or from the upper half.  Runs in interrupt
 *   context; the hw layer has already handled the D-cache for this half
 *   (invalidate for capture, clean for playback).
 *
 *   Exactly one ap_buffer is consumed per call, which is what keeps the
 *   staging geometry (one half == one upper-half buffer) honest, and why
 *   sf32lb52_configure() insists on CONFIG_AUDIO_BUFFER_NUMBYTES buffers.
 *
 * Input Parameters:
 *   priv - Driver state
 *   half - Staging half to service (0 or 1)
 *
 * Returned Value:
 *   Bytes moved, or 0 if no buffer was waiting.
 *
 ****************************************************************************/

static uint32_t sf32lb52_serve_half(FAR struct sf32lb52_audio_s *priv,
                                    uint8_t half)
{
  FAR uint8_t *stage = sf32lb52_audio_hw_stage_pointer(half);
  FAR struct ap_buffer_s *apb;
  irqstate_t flags;
  uint32_t n;

  DEBUGASSERT(stage != NULL);

  /* up_irq_save() rather than enter_critical_section(): with
   * CONFIG_SCHED_CRITMONITOR_MAXTIME_CSECTION undefined the preprocessor
   * treats it as 0, so spinlock.h declares enter_critical_section() as an
   * external function while sched/irq/irq_csection.c only builds it for SMP
   * - linking a non-SMP kernel that calls it fails.  This is a plain
   * single-core board driver, so the arch primitive is both correct and
   * sufficient. */

  flags = up_irq_save();

  /* dq_entry is the first member of ap_buffer_s, so the queue node pointer
   * the queue hands back is the buffer itself. */

  apb = (FAR struct ap_buffer_s *)dq_remfirst(&priv->pendq);
  up_irq_restore(flags);

  if (apb == NULL)
    {
      /* The upper half is not keeping up.  Capture has to drop the audio;
       * playback has to keep the DMA fed or it would repeat the previous
       * half, so silence is substituted. */

      priv->overruns++;

      if (priv->dir == SF32LB52_AUDIO_PLAYBACK)
        {
          memset(stage, 0, SF32LB52_AUDIO_STAGE_BYTES);
        }

      return 0;
    }

  n = apb->nmaxbytes;
  if (n > SF32LB52_AUDIO_STAGE_BYTES)
    {
      n = SF32LB52_AUDIO_STAGE_BYTES;
    }

  if (priv->dir == SF32LB52_AUDIO_CAPTURE)
    {
      memcpy(apb->samp, stage, n);
      apb->nbytes  = n;
      apb->curbyte = 0;
    }
  else
    {
      memcpy(stage, apb->samp, n);

      if (n < SF32LB52_AUDIO_STAGE_BYTES)
        {
          memset(stage + n, 0, SF32LB52_AUDIO_STAGE_BYTES - n);
        }
    }

  if (priv->dev.upper != NULL)
    {
      priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK);
    }

  return n;
}

/****************************************************************************
 * Name: sf32lb52_stage_callback
 *
 * Description:
 *   DMA interrupt entry point; the hw layer calls this once per staging
 *   half.  It must not block: one bounded memcpy plus the upper-half
 *   callback, which NuttX documents as callable from an interrupt handler.
 *
 ****************************************************************************/

static void sf32lb52_stage_callback(uint8_t dir, uint8_t half)
{
  FAR struct sf32lb52_audio_s *priv = g_audio_priv;

  if (priv == NULL || dir != priv->dir ||
      half >= SF32LB52_AUDIO_STAGE_HALVES)
    {
      return;
    }

  sf32lb52_serve_half(priv, half);
}

/****************************************************************************
 * Name: sf32lb52_prime_playback
 *
 * Description:
 *   Fill every staging half from queued buffers before the transmit DMA
 *   starts, so the DAC has real frames from the first sample.  Halves left
 *   over when the queue runs dry stay silent - the anti-pop sequence mutes
 *   the output for the first ~110ms anyway - and the interrupt path takes
 *   over from there.
 *
 ****************************************************************************/

static void sf32lb52_prime_playback(FAR struct sf32lb52_audio_s *priv)
{
  FAR uint8_t *stage;
  uint8_t half;

  for (half = 0; half < SF32LB52_AUDIO_STAGE_HALVES; half++)
    {
      stage = sf32lb52_audio_hw_stage_pointer(half);
      if (stage != NULL)
        {
          memset(stage, 0, SF32LB52_AUDIO_STAGE_BYTES);
        }
    }

  for (half = 0; half < SF32LB52_AUDIO_STAGE_HALVES; half++)
    {
      if (sf32lb52_serve_half(priv, half) == 0)
        {
          break;
        }
    }
}

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

  if (priv->dir == SF32LB52_AUDIO_PLAYBACK)
    {
      /* Feed the staging area from whatever the upper half queued before
       * start(); the transmit DMA then runs against it continuously. */

      sf32lb52_prime_playback(priv);
    }

  /* Capture needs no priming: the ADC starts filling the staging halves as
   * soon as the DMA is armed, and the upper half's buffers are consumed by
   * the interrupt path as they arrive. */

  ret = sf32lb52_audio_hw_start(priv->dir, NULL, 0);
  if (ret != OK)
    {
      return ret;
    }

  priv->overruns = 0;
  priv->running  = true;

  _info("start: dir=%u\n", priv->dir);
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
  FAR struct ap_buffer_s *apb;
  irqstate_t flags;

  _info("stop: overruns=%lu\n", (unsigned long)priv->overruns);

  sf32lb52_audio_hw_stop(priv->dir);
  priv->running = false;

  /* Give back whatever the upper half is still waiting on, otherwise it
   * blocks on buffers that will never complete.  nbytes = 0 marks them as
   * carrying no data. */

  for (; ; )
    {
      flags = up_irq_save();
      apb = (FAR struct ap_buffer_s *)dq_remfirst(&priv->pendq);
      up_irq_restore(flags);

      if (apb == NULL)
        {
          break;
        }

      apb->nbytes = 0;

      if (priv->dev.upper != NULL)
        {
          priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK);
        }
    }

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
 *   Non-blocking buffer enqueue: the buffer goes on pendq and is serviced
 *   later from the DMA interrupt, one staging half at a time.  Nothing is
 *   moved here, so this returns immediately as the upper half expects.
 *
 * Input Parameters:
 *   dev - Lower-half device
 *   apb - The audio pipeline buffer to enqueue
 *
 * Returned Value:
 *   OK on success; -EINVAL if the buffer geometry does not match what
 *   AUDIOIOC_GETBUFFERINFO reported.
 *
 ****************************************************************************/

static int sf32lb52_enqueuebuffer(FAR struct audio_lowerhalf_s *dev,
                                  FAR struct ap_buffer_s *apb)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;
  irqstate_t flags;

  if (apb == NULL || apb->samp == NULL || apb->nmaxbytes == 0)
    {
      return -EINVAL;
    }

  /* One staging half carries exactly one buffer, so the geometry advertised
   * through AUDIOIOC_GETBUFFERINFO has to hold or the stream would be
   * silently truncated. */

  if (apb->nmaxbytes != SF32LB52_AUDIO_STAGE_BYTES)
    {
      _err("enqueue: nmaxbytes=%lu, staging half is %u\n",
           (unsigned long)apb->nmaxbytes,
           (unsigned)SF32LB52_AUDIO_STAGE_BYTES);
      return -EINVAL;
    }

  flags = up_irq_save();
  dq_addlast(&apb->dq_entry, &priv->pendq);
  up_irq_restore(flags);

  return OK;
}

/****************************************************************************
 * Name: sf32lb52_cancelbuffer
 ****************************************************************************/

static int sf32lb52_cancelbuffer(FAR struct audio_lowerhalf_s *dev,
                                 FAR struct ap_buffer_s *apb)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;
  irqstate_t flags;
  bool removed;

  if (apb == NULL)
    {
      return -EINVAL;
    }

  /* dq_rem() is a statement macro, so membership is tested first. */

  flags = up_irq_save();

  removed = dq_inqueue(&apb->dq_entry, &priv->pendq);
  if (removed)
    {
      dq_rem(&apb->dq_entry, &priv->pendq);
    }

  up_irq_restore(flags);

  _info("cancelbuffer: %s\n", removed ? "removed" : "was not queued");
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

  dq_init(&priv->pendq);

  /* M2: power/clock/routing bring-up so the hardware is ready before the
   * first configure/start. */

  ret = sf32lb52_audio_hw_init();
  if (ret != OK)
    {
      _err("ERROR: audio hw init failed: %d\n", ret);
      kmm_free(priv);
      return NULL;
    }

  /* M3: let the DMA interrupt path hand staging halves back to us. */

  sf32lb52_audio_hw_set_stage_callback(sf32lb52_stage_callback);
  g_audio_priv = priv;

  return &priv->dev;
}
