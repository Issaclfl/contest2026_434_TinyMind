/****************************************************************************
 * vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/include/bsp_audio_hw.h
 *
 * Hardware bring-up layer for the SF32LB52 on-chip audio codec
 * (AUDCODEC analog path + AUDPRC digital routing).
 *
 * Porting reference: SiFli-SDK drv_audcodec_m.c / drv_audprc.c
 * (SF32LB52X uses the "_m" single-instance codec; 16K/48K families run
 * from the 48MHz XTAL directly, no PLL frequency programming needed).
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

#ifndef __BOARDS_SF32LB52_SF32LB52_DEVKIT_LCD_INCLUDE_BSP_AUDIO_HW_H
#define __BOARDS_SF32LB52_SF32LB52_DEVKIT_LCD_INCLUDE_BSP_AUDIO_HW_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum sf32lb52_audio_dir_e
{
  SF32LB52_AUDIO_CAPTURE = 0,   /* MIC -> AUDCODEC ADC -> AUDPRC RX -> DMA */
  SF32LB52_AUDIO_PLAYBACK = 1   /* DMA -> AUDPRC TX -> AUDCODEC DAC -> PA  */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: sf32lb52_audio_hw_init
 *
 * Description:
 *   One-time power/clock/HAL bring-up:
 *   PMU audio domain -> RCC AUDCODEC/AUDPRC -> DMA handles (AUDPRC TX0/RX0)
 *   -> HAL_AUDPRC_Init (routing defaults, all muted) -> HAL_AUDCODEC_Init
 *   -> bandgap/PLL clock domain (HAL_TURN_ON_PLL, needed by Refgen)
 *   -> speaker PA (PA10) default off.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   OK on success; negative errno on failure.
 *
 ****************************************************************************/

int sf32lb52_audio_hw_init(void);

/****************************************************************************
 * Name: sf32lb52_audio_hw_config_capture
 ****************************************************************************/

int sf32lb52_audio_hw_config_capture(uint32_t rate, uint8_t nchannels,
                                     uint8_t bpsamp);

/****************************************************************************
 * Name: sf32lb52_audio_hw_config_playback
 ****************************************************************************/

int sf32lb52_audio_hw_config_playback(uint32_t rate, uint8_t nchannels,
                                      uint8_t bpsamp);

/****************************************************************************
 * Name: sf32lb52_audio_hw_start
 *
 * Description:
 *   Bring the configured stream up following the SDK anti-pop sequence.
 *
 * Input Parameters:
 *   dir  - SF32LB52_AUDIO_CAPTURE or SF32LB52_AUDIO_PLAYBACK
 *   buf  - DMA target/source buffer; NULL keeps the stream gated at the
 *          AUDPRC level (hardware on, no DMA service) - M3 passes the
 *          ap_buffer data area here.
 *   len  - Buffer size in bytes (must be word aligned); 0 if buf is NULL.
 *
 * Returned Value:
 *   OK on success; negative errno on failure.
 *
 ****************************************************************************/

int sf32lb52_audio_hw_start(uint8_t dir, uint8_t *buf, uint32_t len);

/****************************************************************************
 * Name: sf32lb52_audio_hw_stop
 *
 * Description:
 *   Tear the stream down in the exact reverse (anti-pop) order.
 *
 * Input Parameters:
 *   dir - Stream direction to stop.
 *
 * Returned Value:
 *   OK on success; negative errno on failure.
 *
 ****************************************************************************/

int sf32lb52_audio_hw_stop(uint8_t dir);

/****************************************************************************
 * Name: sf32lb52_audio_hw_set_mic_gain
 *
 * Description:
 *   Set the codec ADC PGA gain (microphone volume) in dB, -60..30.
 *
 ****************************************************************************/

int sf32lb52_audio_hw_set_mic_gain(int gain_db);

/****************************************************************************
 * Name: sf32lb52_audio_hw_pa
 *
 * Description:
 *   Control the AW8155 speaker amplifier enable line (PA10), including the
 *   550us mode-pulse sequence required after power-up.
 *
 ****************************************************************************/

void sf32lb52_audio_hw_pa(bool on);

#endif /* __BOARDS_SF32LB52_SF32LB52_DEVKIT_LCD_INCLUDE_BSP_AUDIO_HW_H */
