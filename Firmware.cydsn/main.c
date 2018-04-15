/*
 *
 * Copyright (C) 2016 DMA <dma@ya.ru>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include "../dma_core/exp.h"
#include "../dma_core/globals.h"
#include "../dma_core/PSoC_USB.h"
#include "../dma_core/pipeline.h"
#include "../dma_core/scan.h"
#include "../dma_core/sup_serial.h"
#include <project.h>

CY_ISR(BootIRQ_ISR) { Boot_Load(); }

CY_ISR(Timer_ISR) {
  tick++;
  systime++;
}

int main() {
  ILO_Trim_Start();
  ILO_Trim_BeginTrimming();
  CyGlobalIntEnable; /* Enable global interrupts. */
  BootIRQ_StartEx(BootIRQ_ISR);
  SysTimer_Start();
  TimerIRQ_StartEx(Timer_ISR);
  
  load_config();

  power_state = DEVSTATE_FULL_THROTTLE;
  status_register = 0;
  FORCE_BIT(status_register, C2DEVSTATUS_SETUP_MODE, NOT_A_KEYBOARD);
  serial_init();
  usb_init();
  apply_config();
  scan_start(); // We are starting in full power - must do that initial kick
  for (;;) {
#ifdef DEBUG_STATE_MACHINE
      PIN_DEBUG(2, power_state)
#endif
    switch (power_state) {
    case DEVSTATE_FULL_THROTTLE:
      //power_state = DEVSTATE_SHUTDOWN_REQUEST;
//      CyPins_SetPin(ExpHdr_2);
      if (tick) {
        exp_tick(tick);
        tick = 0;
        if (sanity_check_timer > 0) {
          scan_sanity_check();
        }
        usb_tick();
        serial_tick();
        if (TEST_BIT(status_register, C2DEVSTATUS_MATRIX_MONITOR)) {
          report_matrix_readouts();
        } else if (TEST_BIT(status_register, C2DEVSTATUS_OUTPUT_ENABLED)) {
          pipeline_process();
        }
      }
      // Timer ISR will wake us up.
      CyPmAltAct(PM_ALT_ACT_TIME_NONE, PM_ALT_ACT_SRC_NONE);
//      CyPins_ClearPin(ExpHdr_2);
      break;
    case DEVSTATE_PREPARING_TO_SLEEP:
      if (tick) {
        tick = 0;
        // Changes power state!
        usb_check_power();
      }
      break;
    case DEVSTATE_SLEEP:
      // We're supposed to be in deeper sleep so not ever getting here.
      // But if not - keep things warm, sleep the CPU.
      CyPmAltAct(PM_ALT_ACT_TIME_NONE, PM_ALT_ACT_SRC_NONE);
      break;
    case DEVSTATE_WATCH:
      if (tick > SUSPEND_SYSTIMER_DIVISOR) {
        tick = 0;
        scan_start();
        if (pipeline_process_wakeup()) {
          usb_send_wakeup();
        }
      }
      CyPmAltAct(PM_ALT_ACT_TIME_NONE, PM_ALT_ACT_SRC_NONE);
      break;
    case DEVSTATE_SUSPENDING:
      usb_nap();
      break;
    case DEVSTATE_RESUMING:
      // only from deep sleep - RWU goes straight to full throttle.
      tick = 0;
      usb_wake();
      break;
    case DEVSTATE_SHUTDOWN_REQUEST:
      //xprintf("shutdown");
      scan_nap();
      serial_nap();
      SysTimer_Sleep();
      //usb_nap();
      CyPins_SetPin(ExpHdr_2);
      CyPmSaveClocks();
      //CyPmAltAct(PM_SLEEP_TIME_NONE, PM_ALT_ACT_SRC_I2C|PM_ALT_ACT_SRC_PICU|PM_ALT_ACT_SRC_CTW);
      CyPmSleep(PM_SLEEP_TIME_NONE, PM_SLEEP_SRC_I2C|PM_SLEEP_SRC_PICU);
      CyPmRestoreClocks();
      CyDelay(50);
      CyPins_ClearPin(ExpHdr_2);
//      CyPins_SetPin(ExpHdr_2);
//      CyDelay(50);
//      CyPins_ClearPin(ExpHdr_2);
//      CyDelay(50);
      //CyGlobalIntEnable;
      SysTimer_Wakeup();
      serial_wake();
      Sup_Pdu_t buf;
      buf.command = 4;
      buf.data = 5;
      serial_send(&buf);
      //usb_wake();
      scan_wake();
      //xprintf("restart");
      power_state = DEVSTATE_FULL_THROTTLE;
      break;
    default:
#ifdef DEBUG_STATE_MACHINE
      PIN_DEBUG(2, DEVSTATE_MAX)
#endif
      // Stray interrupt?
      // We'd better stay awake and sort it out next iteration.
      break;
    }
  }
}

/* [] END OF FILE */
