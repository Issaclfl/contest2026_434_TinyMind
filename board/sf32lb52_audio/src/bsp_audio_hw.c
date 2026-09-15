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

/* HAL handles.  Codec-side DMA handles are configured but never started:
 * audio data always flows through AUDPRC (the SDK 52d board config does the
 * same - codec DMA Kconfigs stay off). */

static AUDPRC_HandleTypeDef g_audprc;
static AUDCODEC_HandleTypeDef g_audcodec;

static DMA_HandleTypeDef g_audprc_hdma_tx;      /* HAL_AUDPRC_TX_CH0 */
static DMA_HandleTypeDef g_audprc_hdma_rx;      /* HAL_AUDPRC_RX_CH0 */

static DMA_HandleTypeDef g_codec_hdma[HAL_AUDCODEC_INSTANC_CNT];

static bool g_hw_ready;
static bool g_stream_on[2];
static int  g_mic_gain_db = SF32LB52_MIC_GAIN_DEFAULT_DB;

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
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sf32lb52_audio_hw_init
 ****************************************************************************/

int sf32lb52_audio_hw_init(void)
{
  int ret;
  int i;

  /* 1. Power and bus clocks (SDK: bf0_pll_calibration + rt_bf0_audio_*_init) */

  HAL_PMU_EnableAudio(1);
  HAL_RCC_EnableModule(RCC_MOD_AUDCODEC);
  HAL_RCC_EnableModule(RCC_MOD_AUDPRC);

  /* 2. Codec handle.  DMA handles are attached (zeroed, like the SDK leaves
   *    its unused channels) so HAL_AUDCODEC_Init() can iterate them, but
   *    codec-side DMA is never started: audio data always flows through
   *    AUDPRC (dma_config.h only routes codec ADC0 anyway). */

  memset(&g_audcodec, 0, sizeof(g_audcodec));
  g_audcodec.Instance = hwp_audcodec;

  for (i = 0; i < (int)HAL_AUDCODEC_INSTANC_CNT; i++)
    {
      g_audcodec.hdma[i] = &g_codec_hdma[i];
      memset(&g_codec_hdma[i], 0, sizeof(g_codec_hdma[i]));
    }

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

  ret = HAL_AUDCODEC_Config_RChanel(&g_audcodec, HAL_AUDCODEC_ADC_CH0,
                                    &g_audcodec.Init.adc_cfg);
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

  ret = HAL_AUDCODEC_Config_TChanel(&g_audcodec, HAL_AUDCODEC_DAC_CH0,
                                    &g_audcodec.Init.dac_cfg);
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

  if (dir == SF32LB52_AUDIO_CAPTURE)
    {
      /* SDK start_rx order: codec analog path first (MICBIAS settle 20ms),
       * then the digital enables, DMA last. */

      HAL_AUDCODEC_Config_Analog_ADCPath(
        (FAR AUDCODE_ADC_CLK_CONFIG_TYPE *)g_audcodec.Init.adc_cfg.adc_clk);
      __HAL_AUDCODEC_ADC_ENABLE(&g_audcodec);
      __HAL_AUDPRC_ADCPATH_ENABLE(&g_audprc);

      if (buf != NULL && len > 0)
        {
          ret = HAL_AUDPRC_Receive_DMA(&g_audprc, buf, len,
                                       HAL_AUDPRC_RX_CH0);
          if (ret != HAL_OK)
            {
              _err("Receive_DMA failed: %d\n", ret);
              return -EIO;
            }
        }

      __HAL_AUDPRC_ENABLE(&g_audprc);     /* AUDPRC total enable, last */

      g_stream_on[SF32LB52_AUDIO_CAPTURE] = true;
      _info("capture started (dma=%s)\n", buf != NULL ? "on" : "gated");
      return OK;
    }

  if (dir == SF32LB52_AUDIO_PLAYBACK)
    {
      if (buf == NULL || len == 0)
        {
          return -EINVAL;
        }

      /* SDK start_tx order (anti-pop):
       * mute -> AUDPRC (ch cfg + DMA + enable) -> codec digital enable ->
       * DAC analog stepwise power-up -> 10ms -> PA -> 100ms -> unmute. */

      HAL_AUDCODEC_Config_DACPath(&g_audcodec, 1);        /* muted */

      ret = HAL_AUDPRC_Transmit_DMA(&g_audprc, buf, len,
                                    HAL_AUDPRC_TX_CH0);
      if (ret != HAL_OK)
        {
          _err("Transmit_DMA failed: %d\n", ret);
          return -EIO;
        }

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
      _info("playback started\n");
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

      HAL_AUDCODEC_DMAStop(&g_audprc, HAL_AUDPRC_TX_CH0);

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
