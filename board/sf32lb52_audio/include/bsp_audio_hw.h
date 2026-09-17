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
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
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
 * Pre-processor Definitions
 ****************************************************************************/

/* Streaming geometry.
 *
 * The AUDPRC DMA is driven in circular mode (the HAL selects it because
 * dest_sel != AUDPRC_TX_TO_MEM), so the HAL gives us half-complete and
 * complete interrupts.  We hand the DMA *one* staging area of two halves,
 * each half being exactly one ap_buffer's worth of audio, and turn each
 * half/complete interrupt into one buffer delivered to / taken from the
 * upper half.
 *
 * Why stage at all instead of pointing the DMA straight at the upper
 * half's ap_buffers:
 *   - the upper half owns buffer allocation, so the driver cannot promise
 *     cache-line alignment or contiguous physical memory for them;
 *   - the AUDPRC DMA runs in circular mode, which does not map onto a
 *     per-buffer one-shot transfer;
 *   - the D-cache is enabled (CONFIG_ARMV8M_DCACHE), so a DMA target that
 *     the CPU also touches needs explicit cache maintenance.
 * Staging confines all three problems to one buffer we own.  The cost is
 * one 8 KiB memcpy per half (32 KiB/s at 16 kHz/16-bit/mono), which is
 * negligible next to the DMA transfer itself.
 */

#define SF32LB52_AUDIO_STAGE_BYTES   CONFIG_AUDIO_BUFFER_NUMBYTES
#define SF32LB52_AUDIO_STAGE_HALVES  2

/* D-cache line size on this part is not exposed as a Kconfig symbol, so
 * over-align: any power of two >= the real line size is safe. */

#define SF32LB52_AUDIO_DMA_ALIGN     64

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum sf32lb52_audio_dir_e
{
  SF32LB52_AUDIO_CAPTURE = 0,   /* MIC -> AUDCODEC ADC -> AUDPRC RX -> DMA */
  SF32LB52_AUDIO_PLAYBACK = 1   /* DMA -> AUDPRC TX -> AUDCODEC DAC -> PA  */
};

/* Streaming callback.
 *
 * Invoked from DMA interrupt context when a staging half has just been
 * filled by the ADC (capture) or has just been drained by the DAC
 * (playback).  The callee must not block; it is expected to move at most
 * one staging half's worth of data.
 *
 *   dir  - SF32LB52_AUDIO_CAPTURE or SF32LB52_AUDIO_PLAYBACK
 *   half - 0 or 1, the staging half that needs servicing
 */

typedef CODE void (*sf32lb52_audio_stage_cb_t)(uint8_t dir, uint8_t half);

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
 * Name: sf32lb52_audio_hw_set_stage_callback
 *
 * Description:
 *   Register the streaming callback (see sf32lb52_audio_stage_cb_t).
 *   Call before sf32lb52_audio_hw_start(); pass NULL to detach.
 *
 ****************************************************************************/

int sf32lb52_audio_hw_set_stage_callback(sf32lb52_audio_stage_cb_t cb);

/****************************************************************************
 * Name: sf32lb52_audio_hw_stage_pointer
 *
 * Description:
 *   Address of staging half 0 or 1, for the streaming callback to read
 *   from (capture) or write to (playback).
 *
 * Input Parameters:
 *   half - 0 or 1
 *
 * Returned Value:
 *   Pointer to SF32LB52_AUDIO_STAGE_BYTES bytes, or NULL if half is invalid.
 *
 ****************************************************************************/

FAR uint8_t *sf32lb52_audio_hw_stage_pointer(uint8_t half);

/****************************************************************************
 * Name: sf32lb52_audio_hw_start
 *
 * Description:
 *   Bring the configured stream up following the SDK anti-pop sequence and
 *   start the circular DMA on the internal staging buffer.
 *
 *   The DMA always runs against the staging area; `buf`/`len` only carry the
 *   upper half's buffer geometry so the caller can assert it matches what
 *   the driver reported through AUDIOIOC_GETBUFFERINFO.  A mismatch is
 *   rejected rather than silently truncated.  NULL/0 is accepted and means
 *   "use whatever the driver was built with".
 *
 * Input Parameters:
 *   dir  - SF32LB52_AUDIO_CAPTURE or SF32LB52_AUDIO_PLAYBACK
 *   buf  - Unused (staging is internal); must be NULL or the caller's buffer
 *   len  - Expected bytes per upper-half buffer; 0 if not asserted
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
 *   Tear the stream down in the exact reverse (anti-pop) order and detach
 *   the DMA interrupt.
 *
 *   NOTE: this is destructive, not a gate.  It clears the AUDPRC channel
 *   config, soft-resets the AUDPRC block (which also drops the sample-rate
 *   divider) and clears the codec channel, so the next
 *   sf32lb52_audio_hw_start() would arm a channel nothing feeds unless
 *   config_capture()/config_playback() is called again first.  sf32lb52_resume()
 *   in bsp_audio.c does exactly that.
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
