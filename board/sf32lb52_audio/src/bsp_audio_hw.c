/****************************************************************************
 * vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/src/bsp_audio_hw.c
 *
 * Hardware bring-up for the SF32LB52 on-chip audio codec.
 *
 * Every register value below is transcribed from the SiFli-SDK reference
 * drivers (drv_audcodec_m.c / drv_audprc.c, SF32LB52X "_m" variants) so the
 * analog power sequencing and clock dividers match the vendor-validated
 * anti-pop behaviour:
 *
 *   capture : MIC -> AUDCODEC ADC_CH0 -> AUDPRC RX_CH0 -> DMA (P2M)
 *   playback: DMA -> AUDPRC TX_CH0 -> AUDCODEC DAC_CH0 -> AW8155 PA (PA10)
 *
 * 16K/48K sample rate families are derived from the 48MHz XTAL
 * (44.1K would require the audio PLL - not supported in this driver).
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

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/cache.h>

#include "bf0_hal.h"            /* HAL_Delay_us, PMU/RCC/DMA HAL, hwp_* */
#include "drv_io.h"             /* BSP_GPIO_Set */
#include "dma_config.h"         /* AUDCODEC/AUDPRC DMA request & channel macros */

#include "bsp_audio_hw.h"

/* Array element count (board code has no access to the drivers-internal nitems). */
#define HW_NARRAY(a) (sizeof(a) / sizeof((a)[0]))

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Speaker PA enable pin (AU_PA_EN), configured as GPIO in bsp_pinmux.c */

#define SF32LB52_AUDIO_PA_PIN        10
#define SF32LB52_AUDIO_PA_PORTA      1

/* Default microphone gain (codec ADC PGA) and playback level, in dB.
 * Values match the SDK defaults (g_adc_volume=12, music level 15 ~= -4dB). */

#define SF32LB52_MIC_GAIN_DEFAULT_DB 12
#define SF32LB52_PLAYBACK_DB_DEFAULT (-4)

/* HAL volume functions are implemented in bf0_hal_audcodec_m.c /
 * bf0_hal_audprc.c but not declared in their headers. */

extern HAL_StatusTypeDef HAL_AUDCODEC_Config_ADCPath_Volume(
    AUDCODEC_HandleTypeDef *hacodec, int channel, int volume);
extern HAL_StatusTypeDef HAL_AUDPRC_Config_DACPath_Volume(
    AUDPRC_HandleTypeDef *haprc, int channel, int volume);
extern HAL_StatusTypeDef HAL_AUDPRC_Config_ADCPath_Volume(
    AUDPRC_HandleTypeDef *haprc, int channel, int volume);

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* AUDPRC clock table entry: 48MHz XTAL / sample rate divider */

struct audprc_clk_div_s
{
  uint32_t samprate;
  uint16_t div;                 /* STB divider, adc_div == dac_div */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* HAL handles.  Audio data always flows through AUDPRC (TX0/RX0), so the
 * codec's own DMA channels are deliberately left unattached - see the note
 * in sf32lb52_audio_hw_init(). */

static AUDPRC_HandleTypeDef g_audprc;
static AUDCODEC_HandleTypeDef g_audcodec;

static DMA_HandleTypeDef g_audprc_hdma_tx;      /* HAL_AUDPRC_TX_CH0 */
static DMA_HandleTypeDef g_audprc_hdma_rx;      /* HAL_AUDPRC_RX_CH0 */

static bool g_hw_ready;
static bool g_stream_on[2];
static int  g_mic_gain_db = SF32LB52_MIC_GAIN_DEFAULT_DB;

/* Streaming state (M3).
 *
 * g_stage is the circular DMA target for both directions: two halves of
 * SF32LB52_AUDIO_STAGE_BYTES, one upper-half buffer each.  It is a private
 * buffer rather than the caller's ap_buffer data area so that alignment and
 * D-cache maintenance stay contained in this file - see bsp_audio_hw.h.
 */

static uint8_t g_stage[SF32LB52_AUDIO_STAGE_HALVES *
                       SF32LB52_AUDIO_STAGE_BYTES]
  __attribute__((aligned(SF32LB52_AUDIO_DMA_ALIGN)));

static sf32lb52_audio_stage_cb_t g_stage_cb;
static bool g_dma_on[2];

/* Codec ADC clock table (48MHz XTAL family).
 * Fields: samplerate, clk_src_sel, clk_div, osr_sel, sel_clk_adc_source,
 *         sel_clk_adc, diva_clk_adc, fsp
 * Source: SiFli-SDK drv_audcodec_m.c codec_adc_clk_config_xtal[]. */

static const AUDCODE_ADC_CLK_CONFIG_TYPE g_adc_clk_xtal[] =
{
  { 48000, 0,  5, 0, 0, 1, 5, 0 },
  { 16000, 0, 10, 1, 0, 0, 5, 2 },
};

/* Codec DAC clock table (48MHz XTAL family).
 * Fields: samplerate, clk_src_sel, clk_div, osr_sel, sinc_gain,
 *         sel_clk_dac_source, diva_clk_dac, diva_clk_chop_dac,
 *         divb_clk_chop_dac, diva_clk_chop_bg, diva_clk_chop_refgen,
 *         sel_clk_dac
 * Source: SiFli-SDK drv_audcodec_m.c codec_dac_clk_config_xtal[]. */

static const AUDCODE_DAC_CLK_CONFIG_TYPE g_dac_clk_xtal[] =
{
  { 48000, 0, 1, 0, 0x14d, 0, 5, 4, 2, 20, 20, 0 },
  { 16000, 0, 1, 4, 0x14d, 0, 5, 4, 2, 20, 20, 0 },
};

/* AUDPRC STB clock dividers (48MHz XTAL family).
 * Source: SiFli-SDK drv_audprc.c audprc_clk_cfg_tb_xtal[]. */

static const struct audprc_clk_div_s g_audprc_div_xtal[] =
{
  { 48000, 1000 },
  { 16000, 3000 },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: hw_find_adc_clk / hw_find_dac_clk / hw_find_prc_div
 ****************************************************************************/

static FAR const AUDCODE_ADC_CLK_CONFIG_TYPE *
hw_find_adc_clk(uint32_t samprate)
{
  size_t i;

  for (i = 0; i < HW_NARRAY(g_adc_clk_xtal); i++)
    {
      if (g_adc_clk_xtal[i].samplerate == samprate)
        {
          return &g_adc_clk_xtal[i];
        }
    }

  return NULL;
}

static FAR const AUDCODE_DAC_CLK_CONFIG_TYPE *
hw_find_dac_clk(uint32_t samprate)
{
  size_t i;

  for (i = 0; i < HW_NARRAY(g_dac_clk_xtal); i++)
    {
      if (g_dac_clk_xtal[i].samplerate == samprate)
        {
          return &g_dac_clk_xtal[i];
        }
    }

  return NULL;
}

static FAR const struct audprc_clk_div_s *
hw_find_prc_div(uint32_t samprate)
{
  size_t i;

  for (i = 0; i < HW_NARRAY(g_audprc_div_xtal); i++)
    {
      if (g_audprc_div_xtal[i].samprate == samprate)
        {
          return &g_audprc_div_xtal[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: hw_dma_handle_init
 *
 * Description:
 *   Fill a DMA handle the way the SDK audio drivers do: 32-bit transfers,
 *   circular mode, high priority, no burst.
 *
 ****************************************************************************/

static int hw_dma_handle_init(FAR DMA_HandleTypeDef *hdma,
                              DMA_Channel_TypeDef *instance,
                              uint32_t request, uint32_t direction)
{
  memset(hdma, 0, sizeof(*hdma));

  hdma->Instance = instance;
  hdma->Init.Request = request;
  hdma->Init.Direction = direction;
  hdma->Init.PeriphInc = DMA_PINC_DISABLE;
  hdma->Init.MemInc = DMA_MINC_ENABLE;
  hdma->Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
  hdma->Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
  hdma->Init.Mode = DMA_CIRCULAR;
  hdma->Init.Priority = DMA_PRIORITY_HIGH;
  hdma->Init.BurstSize = 0;

  return HAL_DMA_Init(hdma) == HAL_OK ? OK : -EIO;
}

/****************************************************************************
 * Name: hw_prc_path_defaults
 *
 * Description:
 *   Default (muted) ADC/DAC path configuration for AUDPRC, transcribed from
 *   the SDK bf0_adc_dac_path_cfg_init(): both paths routed to/from the
 *   codec, mixers fed from silence, EQ bypassed.
 *
 ****************************************************************************/

static void hw_prc_path_defaults(void)
{
  FAR AUDPRC_ADCCfgTypeDef *adc = &g_audprc.Init.adc_cfg;
  FAR AUDPRC_DACCfgTypeDef *dac = &g_audprc.Init.dac_cfg;

  memset(adc, 0, sizeof(*adc));
  adc->src_sel = AUDPRC_RX_FROM_CODEC;    /* RX from codec */

  memset(dac, 0, sizeof(*dac));
  dac->dst_sel = AUDPRC_TX_TO_CODEC;      /* DAC path to codec */

  /* The HAL keys the AUDPRC DMA mode off this handle field: anything other
   * than AUDPRC_TX_TO_MEM selects circular mode plus the half-complete
   * handler (bf0_hal_audprc.c:796), which is exactly what the two-half
   * staging scheme depends on.  Set it explicitly instead of relying on the
   * zero-fill matching AUDPRC_TX_TO_CODEC by accident. */

  g_audprc.dest_sel = AUDPRC_TX_TO_CODEC;
  dac->mixrsrc1 = 5;                      /* mixer right aux: silence */
  dac->mixrsrc0 = 1;                      /* mixer right main: TX ch0 */
  dac->mixlsrc1 = 5;                      /* mixer left aux: silence */
  dac->muxrsrc1 = 5;                      /* mux right aux: silence */
  dac->muxrsrc0 = 1;                      /* mux right main: TX ch0 */
  dac->muxlsrc1 = 5;                      /* mux left aux: silence */
  dac->eq_stage = 1;
}

/****************************************************************************
 * Name: hw_set_prc_clock
 *
 * Description:
 *   Select the 48MHz XTAL as AUDPRC clock and apply the sample-rate
 *   divider.  SDK: AUDIO_CTL_OUTPUTSRC -> bf0_audprc_src().
 *
 ****************************************************************************/

static int hw_set_prc_clock(uint32_t samprate)
{
  FAR const struct audprc_clk_div_s *div = hw_find_prc_div(samprate);

  if (div == NULL)
    {
      _err("unsupported audprc sample rate %lu\n", (unsigned long)samprate);
      return -EINVAL;
    }

  g_audprc.Init.adc_div = div->div;
  g_audprc.Init.dac_div = div->div;
  g_audprc.Init.clk_sel = 0;              /* XTAL 48MHz */

  __HAL_AUDPRC_CLK_XTAL(&g_audprc);
  __HAL_AUDPRC_STB_DIV_CLK(&g_audprc, div->div, div->div);
  return OK;
}

/****************************************************************************
 * Streaming plumbing (M3)
 *
 * Interrupt path:
 *   DMAC IRQ -> hw_{rx,tx}_dma_isr -> HAL_DMA_IRQHandler
 *            -> AUDPRC_DMARxCplt / AUDPRC_DMATxCplt   (set up by the HAL)
 *            -> HAL_AUDPRC_Rx{Cplt,HalfCplt}Callback  (weak, overridden here)
 *            -> g_stage_cb(dir, half)
 *
 * Because dest_sel stays AUDPRC_TX_TO_CODEC the HAL runs the DMA in circular
 * mode and installs the half-complete handler, so a two-half staging area
 * yields one interrupt per upper-half buffer in both directions.
 ****************************************************************************/

static void hw_stage_clean(uint8_t half)
{
  FAR uint8_t *p = &g_stage[half * SF32LB52_AUDIO_STAGE_BYTES];

  up_clean_dcache((uintptr_t)p,
                  (uintptr_t)p + SF32LB52_AUDIO_STAGE_BYTES);
}

static void hw_stage_invalidate(uint8_t half)
{
  FAR uint8_t *p = &g_stage[half * SF32LB52_AUDIO_STAGE_BYTES];

  up_invalidate_dcache((uintptr_t)p,
                       (uintptr_t)p + SF32LB52_AUDIO_STAGE_BYTES);
}

/* Capture: the ADC has just filled `half`, so the CPU's view is stale.
 * Invalidate before handing it up, then notify (runs in interrupt context).
 */

static void hw_capture_half_ready(uint8_t half)
{
  hw_stage_invalidate(half);

  if (g_stage_cb != NULL)
    {
      g_stage_cb(SF32LB52_AUDIO_CAPTURE, half);
    }
}

/* Playback: the callee has just refilled `half`, so push it out of the
 * D-cache before the DMA reads it again (a full half-transfer away, but the
 * clean has to happen after the refill, not before).
 */

static void hw_playback_half_drained(uint8_t half)
{
  if (g_stage_cb != NULL)
    {
      g_stage_cb(SF32LB52_AUDIO_PLAYBACK, half);
    }

  hw_stage_clean(half);
}

static int hw_rx_dma_isr(int irq, FAR void *context, FAR void *arg)
{
  HAL_DMA_IRQHandler(&g_audprc_hdma_rx);
  return OK;
}

static int hw_tx_dma_isr(int irq, FAR void *context, FAR void *arg)
{
  HAL_DMA_IRQHandler(&g_audprc_hdma_tx);
  return OK;
}

/* NuttX IRQ numbers are offset by the 16 Cortex-M system exceptions. */

static void hw_dma_irq_attach(uint8_t dir)
{
  if (dir == SF32LB52_AUDIO_CAPTURE)
    {
      irq_attach(AUDPRC_RX0_DMA_IRQ + 16, hw_rx_dma_isr, NULL);
      up_enable_irq(AUDPRC_RX0_DMA_IRQ + 16);
    }
  else
    {
      irq_attach(AUDPRC_TX0_DMA_IRQ + 16, hw_tx_dma_isr, NULL);
      up_enable_irq(AUDPRC_TX0_DMA_IRQ + 16);
    }
}

static void hw_dma_irq_detach(uint8_t dir)
{
  if (dir == SF32LB52_AUDIO_CAPTURE)
    {
      up_disable_irq(AUDPRC_RX0_DMA_IRQ + 16);
      irq_detach(AUDPRC_RX0_DMA_IRQ + 16);
    }
  else
    {
      up_disable_irq(AUDPRC_TX0_DMA_IRQ + 16);
      irq_detach(AUDPRC_TX0_DMA_IRQ + 16);
    }
}

/****************************************************************************
 * HAL callbacks
 *
 * The HAL sets AUDPRC_DMARxCplt / AUDPRC_DMATxCplt as the DMA transfer
 * callbacks and those call these weak functions; overriding them here is the
 * documented user hook (bf0_hal_audprc.c:854..).
 ****************************************************************************/

void HAL_AUDPRC_RxHalfCpltCallback(AUDPRC_HandleTypeDef *haprc, int cid)
{
  hw_capture_half_ready(0);
}

void HAL_AUDPRC_RxCpltCallback(AUDPRC_HandleTypeDef *haprc, int cid)
{
  hw_capture_half_ready(1);
}

void HAL_AUDPRC_TxHalfCpltCallback(AUDPRC_HandleTypeDef *haprc, int cid)
{
  hw_playback_half_drained(0);
}

void HAL_AUDPRC_TxCpltCallback(AUDPRC_HandleTypeDef *haprc, int cid)
{
  hw_playback_half_drained(1);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sf32lb52_audio_hw_init
 ****************************************************************************/

int sf32lb52_audio_hw_init(void)
{
  int ret;

  /* 1. Power and bus clocks (SDK: bf0_pll_calibration + rt_bf0_audio_*_init) */

  HAL_PMU_EnableAudio(1);
  HAL_RCC_EnableModule(RCC_MOD_AUDCODEC);
  HAL_RCC_EnableModule(RCC_MOD_AUDPRC);

  /* 2. Codec handle.
   *
   * The codec's own DMA channels are left NULL on purpose.  Audio data always
   * flows through AUDPRC, and HAL_AUDCODEC_Init() skips NULL entries
   * (bf0_hal_audcodec_m.c:680/685).  Attaching *zeroed* handles here instead
   * makes the HAL call HAL_DMA_Init() on a handle whose Instance is NULL,
   * which trips HAL_ASSERT(IS_DMA_ALL_INSTANCE(...)) - and in this HAL
   * HAL_ASSERT expands to `while (1) {;}` (bf0_hal.h:419), i.e. a silent
   * permanent hang at boot with no output and no reset.
   */

  memset(&g_audcodec, 0, sizeof(g_audcodec));
  g_audcodec.Instance = hwp_audcodec;

  ret = HAL_AUDCODEC_Init(&g_audcodec);
  if (ret != HAL_OK)
    {
      _err("HAL_AUDCODEC_Init failed: %d\n", ret);
      return -EIO;
    }

  /* 3. AUDPRC handle: two DMA handles (TX0 for playback, RX0 for capture)
   *    plus the muted routing defaults, then core init. */

  memset(&g_audprc, 0, sizeof(g_audprc));
  g_audprc.Instance = hwp_audprc;
  g_audprc.Init.clk_div = 3;              /* ASIC value (SDK bf0_audio_init) */
  g_audprc.Init.adc_div = 1;
  g_audprc.Init.dac_div = 1;

  g_audprc.hdma[HAL_AUDPRC_TX_CH0] = &g_audprc_hdma_tx;
  g_audprc.hdma[HAL_AUDPRC_RX_CH0] = &g_audprc_hdma_rx;

  ret = hw_dma_handle_init(&g_audprc_hdma_tx, AUDPRC_TX0_DMA_INSTANCE,
                           AUDPRC_TX0_DMA_REQUEST, DMA_MEMORY_TO_PERIPH);
  if (ret != OK)
    {
      return ret;
    }

  ret = hw_dma_handle_init(&g_audprc_hdma_rx, AUDPRC_RX0_DMA_INSTANCE,
                           AUDPRC_RX0_DMA_REQUEST, DMA_PERIPH_TO_MEMORY);
  if (ret != OK)
    {
      return ret;
    }

  hw_prc_path_defaults();

  ret = HAL_AUDPRC_Init(&g_audprc);
  if (ret != HAL_OK)
    {
      _err("HAL_AUDPRC_Init failed: %d\n", ret);
      return -EIO;
    }

  /* 4. Bandgap + PLL clock domain (HAL_TURN_ON_PLL also runs
   *    HAL_AUCODEC_Refgen_Init, required by the analog paths).  No PLL
   *    frequency programming: 16K/48K families run from the 48MHz XTAL. */

  HAL_TURN_ON_PLL();

  /* 5. Speaker PA off until playback needs it (LCD bring-up may have
   *    raised PA10 as a side effect - force it low here). */

  sf32lb52_audio_hw_pa(false);

  g_hw_ready = true;
  _info("audio hw ready\n");
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_audio_hw_config_capture
 ****************************************************************************/

int sf32lb52_audio_hw_config_capture(uint32_t rate, uint8_t nchannels,
                                     uint8_t bpsamp)
{
  FAR const AUDCODE_ADC_CLK_CONFIG_TYPE *adc_clk;
  AUDPRC_ChnlCfgTypeDef prc_cfg;
  int ret;

  if (!g_hw_ready)
    {
      return -EAGAIN;
    }

  if (bpsamp != 16 || (nchannels != 1 && nchannels != 2))
    {
      _err("unsupported format: %ubit %uch\n", bpsamp, nchannels);
      return -EINVAL;
    }

  adc_clk = hw_find_adc_clk(rate);
  if (adc_clk == NULL)
    {
      _err("unsupported capture rate %lu\n", (unsigned long)rate);
      return -EINVAL;
    }

  /* Codec side: route ADC through AUDPRC (opmode 0), apply the clock row,
   * enable ADC_CH0, then restore the stored mic gain. */

  g_audcodec.Init.adc_cfg.opmode = 0;     /* 0: AUDPRC RX from codec */
  g_audcodec.Init.adc_cfg.adc_clk =
    (FAR AUDCODE_ADC_CLK_CONFIG_TYPE *)adc_clk;

  /* Channel argument is a *relative* index within this direction (0 or 1),
   * not the HAL_AUDCODEC_ChannelTypeDef value: the HAL switches on it with
   * `case 0: / case 1:` and returns HAL_ERROR from `default:` - so passing
   * HAL_AUDCODEC_ADC_CH0 (== 2) fails outright. */

  ret = HAL_AUDCODEC_Config_RChanel(&g_audcodec, 0, &g_audcodec.Init.adc_cfg);
  if (ret != HAL_OK)
    {
      _err("codec Config_RChanel failed: %d\n", ret);
      return -EIO;
    }

  HAL_AUDCODEC_Config_ADCPath_Volume(&g_audcodec, 0, g_mic_gain_db);
  HAL_AUDCODEC_Config_ADCPath_Volume(&g_audcodec, 1, g_mic_gain_db);

  /* AUDPRC side: RX_CH0 from codec, digital gain 0dB, sample clock. */

  memset(&prc_cfg, 0, sizeof(prc_cfg));
  prc_cfg.en = 1;
  prc_cfg.format = 0;                     /* 16 bit */
  prc_cfg.mode = (nchannels == 1) ? 0 : 1;

  HAL_AUDPRC_Config_ADCPath_Volume(&g_audprc, 0, 0);
  HAL_AUDPRC_Config_ADCPath_Volume(&g_audprc, 1, 0);

  __HAL_AUDPRC_ADC_SRC_CODEC(&g_audprc);

  g_audprc.cfg = prc_cfg;

  ret = hw_set_prc_clock(rate);
  if (ret != OK)
    {
      return ret;
    }

  _info("capture config: %luHz %uch %ubit\n",
         (unsigned long)rate, nchannels, bpsamp);
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_audio_hw_config_playback
 ****************************************************************************/

int sf32lb52_audio_hw_config_playback(uint32_t rate, uint8_t nchannels,
                                      uint8_t bpsamp)
{
  FAR const AUDCODE_DAC_CLK_CONFIG_TYPE *dac_clk;
  AUDPRC_ChnlCfgTypeDef prc_cfg;
  int ret;

  if (!g_hw_ready)
    {
      return -EAGAIN;
    }

  if (bpsamp != 16 || (nchannels != 1 && nchannels != 2))
    {
      _err("unsupported format: %ubit %uch\n", bpsamp, nchannels);
      return -EINVAL;
    }

  dac_clk = hw_find_dac_clk(rate);
  if (dac_clk == NULL)
    {
      _err("unsupported playback rate %lu\n", (unsigned long)rate);
      return -EINVAL;
    }

  /* Codec side: DAC fed by AUDPRC (opmode 0), ramp enabled against pops. */

  g_audcodec.Init.dac_cfg.opmode = 0;     /* 0: AUDPRC TX to codec */
  g_audcodec.Init.dac_cfg.dac_clk =
    (FAR AUDCODE_DAC_CLK_CONFIG_TYPE *)dac_clk;

  /* Relative channel index, same convention as Config_RChanel above. */

  ret = HAL_AUDCODEC_Config_TChanel(&g_audcodec, 0, &g_audcodec.Init.dac_cfg);
  if (ret != HAL_OK)
    {
      _err("codec Config_TChanel failed: %d\n", ret);
      return -EIO;
    }

  /* Q15.1 volume: parameter is 2x dB, range -36..54dB. */

  HAL_AUDCODEC_Config_DACPath_Volume(&g_audcodec, 0,
                                     2 * SF32LB52_PLAYBACK_DB_DEFAULT);
  HAL_AUDCODEC_Config_DACPath_Volume(&g_audcodec, 1,
                                     2 * SF32LB52_PLAYBACK_DB_DEFAULT);

  /* AUDPRC side: TX_CH0 to codec, keep mixer muxes silent until start. */

  __HAL_AUDPRC_DAC_DST_CODEC(&g_audprc);

  memset(&prc_cfg, 0, sizeof(prc_cfg));
  prc_cfg.en = 1;
  prc_cfg.format = 0;                     /* 16 bit */
  prc_cfg.mode = (nchannels == 1) ? 0 : 1;

  g_audprc.cfg1 = prc_cfg;

  ret = hw_set_prc_clock(rate);
  if (ret != OK)
    {
      return ret;
    }

  _info("playback config: %luHz %uch %ubit\n",
         (unsigned long)rate, nchannels, bpsamp);
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_audio_hw_start
 ****************************************************************************/

int sf32lb52_audio_hw_start(uint8_t dir, uint8_t *buf, uint32_t len)
{
  int ret;

  if (!g_hw_ready)
    {
      return -EAGAIN;
    }

  /* The DMA always runs against the internal staging area, so the caller's
   * buffer geometry is only ever asserted, never used as the DMA target. */

  if (len != 0 && len != SF32LB52_AUDIO_STAGE_BYTES)
    {
      _err("buffer size %lu != staging %u\n", (unsigned long)len,
           (unsigned)SF32LB52_AUDIO_STAGE_BYTES);
      return -EINVAL;
    }

  if (dir == SF32LB52_AUDIO_CAPTURE)
    {
      /* SDK start_rx order: codec analog path first (MICBIAS settle 20ms),
       * then the digital enables, DMA last. */

      HAL_AUDCODEC_Config_Analog_ADCPath(
        (FAR AUDCODE_ADC_CLK_CONFIG_TYPE *)g_audcodec.Init.adc_cfg.adc_clk);
      __HAL_AUDCODEC_ADC_ENABLE(&g_audcodec);
      __HAL_AUDPRC_ADCPATH_ENABLE(&g_audprc);

      /* Flush the staging area before handing it to the DMA: the ADC writes
       * into it behind the CPU's back, and any dirty line left in the D-cache
       * would be written back over captured audio. */

      up_flush_dcache((uintptr_t)g_stage,
                      (uintptr_t)g_stage + sizeof(g_stage));

      ret = HAL_AUDPRC_Receive_DMA(&g_audprc, g_stage, sizeof(g_stage),
                                   HAL_AUDPRC_RX_CH0);
      if (ret != HAL_OK)
        {
          _err("Receive_DMA failed: %d\n", ret);
          return -EIO;
        }

      hw_dma_irq_attach(SF32LB52_AUDIO_CAPTURE);
      __HAL_AUDPRC_ENABLE(&g_audprc);     /* AUDPRC total enable, last */

      g_stream_on[SF32LB52_AUDIO_CAPTURE] = true;
      g_dma_on[SF32LB52_AUDIO_CAPTURE] = true;
      _info("capture started (staging %u B, circular)\n",
            (unsigned)sizeof(g_stage));
      return OK;
    }

  if (dir == SF32LB52_AUDIO_PLAYBACK)
    {
      /* SDK start_tx order (anti-pop):
       * mute -> AUDPRC (ch cfg + DMA + enable) -> codec digital enable ->
       * DAC analog stepwise power-up -> 10ms -> PA -> 100ms -> unmute.
       *
       * The staging halves were filled by the stage callback before start,
       * so the DAC has valid frames queued while the analog path settles;
       * the unmute at the end is what makes them audible. */

      HAL_AUDCODEC_Config_DACPath(&g_audcodec, 1);        /* muted */

      up_flush_dcache((uintptr_t)g_stage,
                      (uintptr_t)g_stage + sizeof(g_stage));

      ret = HAL_AUDPRC_Transmit_DMA(&g_audprc, g_stage, sizeof(g_stage),
                                    HAL_AUDPRC_TX_CH0);
      if (ret != HAL_OK)
        {
          _err("Transmit_DMA failed: %d\n", ret);
          return -EIO;
        }

      hw_dma_irq_attach(SF32LB52_AUDIO_PLAYBACK);
      __HAL_AUDPRC_ENABLE(&g_audprc);

      __HAL_AUDCODEC_DAC_ENABLE(&g_audcodec);
      HAL_AUDCODEC_Config_DACPath(&g_audcodec, 1);        /* still muted */
      HAL_AUDCODEC_Config_Analog_DACPath(
        (FAR AUDCODE_DAC_CLK_CONFIG_TYPE *)g_audcodec.Init.dac_cfg.dac_clk);

      HAL_Delay_us(10 * 1000);
      sf32lb52_audio_hw_pa(true);
      HAL_Delay_us(100 * 1000);

      HAL_AUDCODEC_Config_DACPath(&g_audcodec, 0);        /* unmute */

      g_stream_on[SF32LB52_AUDIO_PLAYBACK] = true;
      g_dma_on[SF32LB52_AUDIO_PLAYBACK] = true;
      _info("playback started (staging %u B, circular)\n",
            (unsigned)sizeof(g_stage));
      return OK;
    }

  return -EINVAL;
}

/****************************************************************************
 * Name: sf32lb52_audio_hw_stop
 ****************************************************************************/

int sf32lb52_audio_hw_stop(uint8_t dir)
{
  if (dir == SF32LB52_AUDIO_CAPTURE)
    {
      if (!g_stream_on[SF32LB52_AUDIO_CAPTURE])
        {
          return OK;
        }

      /* Reverse of start: AUDPRC first, then codec digital, then analog. */

      HAL_AUDPRC_DMAStop(&g_audprc, HAL_AUDPRC_RX_CH0);

      /* The vendor's DMAStop has its state reset commented out
       * (bf0_hal_audprc.c, "//haprc->State = HAL_AUDPRC_STATE_READY;"),
       * so BUSY_RX survives the session and the next Receive_DMA returns
       * HAL_BUSY forever after.  Clear it here - State is __IO and indexed
       * per channel, and Receive_DMA sets it unconditionally on start. */

      g_audprc.State[HAL_AUDPRC_RX_CH0] = HAL_AUDPRC_STATE_READY;
      hw_dma_irq_detach(SF32LB52_AUDIO_CAPTURE);
      g_dma_on[SF32LB52_AUDIO_CAPTURE] = false;

      __HAL_AUDPRC_ADCPATH_DISABLE(&g_audprc);
      __HAL_AUDPRC_DISABLE(&g_audprc);
      HAL_AUDPRC_Clear_Adc_Channel(&g_audprc);
      __HAL_AUDPRC_SRESET_START(&g_audprc);
      __HAL_AUDPRC_SRESET_STOP(&g_audprc);

      __HAL_AUDCODEC_ADC_DISABLE(&g_audcodec);
      HAL_AUDCODEC_Close_Analog_ADCPath();
      HAL_AUDCODEC_Clear_All_Channel(&g_audcodec, 2);   /* bit1: adc */

      g_stream_on[SF32LB52_AUDIO_CAPTURE] = false;
      _info("capture stopped\n");
      return OK;
    }

  if (dir == SF32LB52_AUDIO_PLAYBACK)
    {
      if (!g_stream_on[SF32LB52_AUDIO_PLAYBACK])
        {
          return OK;
        }

      /* SDK speaker_close order (anti-pop): PA off -> mute -> codec analog
       * down in reverse 10us steps -> channels cleared -> AUDPRC reset. */

      sf32lb52_audio_hw_pa(false);
      HAL_AUDCODEC_Config_DACPath(&g_audcodec, 1);      /* mute */

      /* Stop the AUDPRC TX channel - NOT the codec's DMAStop: that takes a
       * codec handle, and passing an AUDPRC handle to it only "worked"
       * because C does not type-check across the two HALs.  Same vendor
       * state-reset omission as the RX side, so clear BUSY_TX here too. */

      HAL_AUDPRC_DMAStop(&g_audprc, HAL_AUDPRC_TX_CH0);
      g_audprc.State[HAL_AUDPRC_TX_CH0] = HAL_AUDPRC_STATE_READY;
      hw_dma_irq_detach(SF32LB52_AUDIO_PLAYBACK);
      g_dma_on[SF32LB52_AUDIO_PLAYBACK] = false;

      HAL_AUDCODEC_Close_Analog_DACPath();
      __HAL_AUDCODEC_DAC_DISABLE(&g_audcodec);
      HAL_AUDCODEC_Clear_All_Channel(&g_audcodec, 1);   /* bit0: dac */

      __HAL_AUDPRC_DISABLE(&g_audprc);
      HAL_AUDPRC_Clear_All_Channel(&g_audprc);
      __HAL_AUDPRC_SRESET_START(&g_audprc);
      __HAL_AUDPRC_SRESET_STOP(&g_audprc);

      g_stream_on[SF32LB52_AUDIO_PLAYBACK] = false;
      _info("playback stopped\n");
      return OK;
    }

  return -EINVAL;
}

/****************************************************************************
 * Name: sf32lb52_audio_hw_set_stage_callback
 ****************************************************************************/

int sf32lb52_audio_hw_set_stage_callback(sf32lb52_audio_stage_cb_t cb)
{
  g_stage_cb = cb;
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_audio_hw_stage_pointer
 ****************************************************************************/

FAR uint8_t *sf32lb52_audio_hw_stage_pointer(uint8_t half)
{
  if (half >= SF32LB52_AUDIO_STAGE_HALVES)
    {
      return NULL;
    }

  return &g_stage[half * SF32LB52_AUDIO_STAGE_BYTES];
}

/****************************************************************************
 * Name: sf32lb52_audio_hw_set_mic_gain
 ****************************************************************************/

int sf32lb52_audio_hw_set_mic_gain(int gain_db)
{
  if (gain_db < -60 || gain_db > 30)
    {
      return -EINVAL;
    }

  g_mic_gain_db = gain_db;

  if (g_hw_ready)
    {
      HAL_AUDCODEC_Config_ADCPath_Volume(&g_audcodec, 0, gain_db);
      HAL_AUDCODEC_Config_ADCPath_Volume(&g_audcodec, 1, gain_db);
    }

  return OK;
}

/****************************************************************************
 * Name: sf32lb52_audio_hw_pa
 *
 * Description:
 *   AW8155 enable (PA10).  The SDK power-up sequence holds the pin low for
 *   550us, then raises it once to select the default gain mode.
 *
 ****************************************************************************/

void sf32lb52_audio_hw_pa(bool on)
{
  if (on)
    {
      BSP_GPIO_Set(SF32LB52_AUDIO_PA_PIN, 0, SF32LB52_AUDIO_PA_PORTA);
      HAL_Delay_us(550);
      BSP_GPIO_Set(SF32LB52_AUDIO_PA_PIN, 1, SF32LB52_AUDIO_PA_PORTA);
    }
  else
    {
      BSP_GPIO_Set(SF32LB52_AUDIO_PA_PIN, 0, SF32LB52_AUDIO_PA_PORTA);
    }
}
