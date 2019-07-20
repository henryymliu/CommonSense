/*
 *
 * Copyright (C) 2016-2017 DMA <dma@ya.ru>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <project.h>
#include <string.h>

#include "exp.h"
#include "scan.h"
#include "pipeline.h"
#include "PSoC_USB.h"
#include "sup_serial.h"

uint8_t tap_usb_sc;
uint32_t tap_deadline;
uint_fast16_t saved_macro_ptr;

inline void process_layerMods(uint8_t flags, uint8_t keycode) {
  // codes A8-AB - momentary selection(Fn), AC-AF - permanent(LLck)
  if (keycode & 0x04) {
    // LLck. Keydown flips the bit, keyup is ignored.
    if (flags & KEY_UP_MASK) {
      return;
    }
    FLIP_BIT(layerMods, (keycode & 0x03) + LAYER_MODS_SHIFT);
  } else if ((flags & KEY_UP_MASK) == 0) {
    // Fn Press
    SET_BIT(layerMods, (keycode & 0x03) + LAYER_MODS_SHIFT);
  } else {
    // Fn Release
    CLEAR_BIT(layerMods, (keycode & 0x03) + LAYER_MODS_SHIFT);
  }
  // Figure layer condition
  for (uint8_t i = 0; i < sizeof(config.layerConditions); i++) {
    if (layerMods == (config.layerConditions[i] & 0xf0)) {
      currentLayer = config.layerConditions[i] & 0x0f;
      break;
    }
  }
#ifdef DEBUG_PIPELINE
  xprintf("L@%d: %02x %02x -> %d", systime, flags, keycode, currentLayer);
#endif
}

/*
 * Data structure: [scancode][flags][data length][macro data]
 */
inline uint_fast16_t lookup_macro(uint8_t flags, uint8_t keycode) {
  uint_fast16_t ptr = 0;
  do {
#if USBQUEUE_RELEASED_MASK != MACRO_TYPE_ONKEYUP
#error Please rewrite check below - it is no longer valid
#endif
    if (config.macros[ptr] == keycode &&
        (flags & USBQUEUE_RELEASED_MASK) ==
            (config.macros[ptr + 1] & MACRO_TYPE_ONKEYUP)) {
      return ptr;
    } else {
      ptr += config.macros[ptr + 2] + 3;
    }
  } while (ptr < sizeof config.macros &&
           config.macros[ptr] != EMPTY_FLASH_BYTE);
  return MACRO_NOT_FOUND;
}

inline void queue_usbcode(uint32_t time, uint8_t flags, uint8_t keycode) {
  // Trick - even if current buffer position is empty, write to the next one.
  // It may be empty because reader already processed it.
  USBQueue_wpos = KEYCODE_BUFFER_NEXT(USBQueue_wpos);
  // Make sure not to overwrite pending events - TODO think how not to fall into
  // infinite loop if buffer full.
  while (USBQueue[USBQueue_wpos].keycode != USBCODE_NOEVENT) {
    USBQueue_wpos = KEYCODE_BUFFER_NEXT(USBQueue_wpos);
  }
  USBQueue[USBQueue_wpos].sysTime = time;
  USBQueue[USBQueue_wpos].flags = flags;
  USBQueue[USBQueue_wpos].keycode = keycode;
}

inline void play_macro(uint_fast16_t start) {
  uint8_t *mptr = &config.macros[start] + 3;
  uint8_t *macro_end = mptr + config.macros[start + 2];
#ifdef DEBUG_PIPELINE
  xprintf("PM@%d: %d, sz: %d", systime, start, config.macros[start + 2]);
#endif
  uint32_t now = systime;
  uint_fast16_t delay;
  uint8_t keyflags;
  while (mptr <= macro_end) {
    delay = config.delayLib[(*mptr >> 2) & 0x0f];
    switch (*mptr >> 6) {
    // Check first 2 bits - macro command
    case 0: // TypeOneKey
      // Press+release, timing from delayLib
      mptr++;
      // FIXME possible to queue USB_NOEVENT.
      // This will most likely trigger exp. header, but may be as bad as
      // infinite loop.
      queue_usbcode(now, 0, *mptr);
      now += delay;
      queue_usbcode(now, USBQUEUE_RELEASED_MASK, *mptr);
      break;
    case 1: // ChangeMods - currently PressKey and ReleaseKey
      /*
       * Initial plans for this were to be "change modifiers". Binary format - 
       * 2 bits for (PressMod/ReleaseMod/ToggleMod/ForceMod)
       * then 4 bits not used, then a byte of standard modflags.
       * Now, though, it's 
       * [4 bits delay, 1 bit direction, 1 bit reserved, 1 byte scancode]
       */
      keyflags =
          (*mptr & MACRO_KEY_UPDOWN_RELEASE) ? USBQUEUE_RELEASED_MASK : 0;
      mptr++;
      queue_usbcode(now, keyflags, *mptr);
      now += delay;
      break;
    case 2: // Mods stack manipulation
      // 2 bits - PushMods, PopMods, RevertMods.
      // Not used currently.
      break;
    case 3: // Wait
      /* 4 bits for delay. Initially had special value of 
       * 0 = "Wait for trigger key release", others from delayLib.
       * But since now we have separate triggers on press and release - all
       * values are from delayLib.
       */
      now += delay;
      break;
    default:
      return;
    }
    mptr++;
  }
}

inline scancode_t read_scancode(void) {
  if (scancodes_rpos == scancodes_wpos) {
    // Nothing to read. Return empty value AKA "Pressed nokey".
    scancode_t result;
    result.flags = 0;
    result.scancode = COMMONSENSE_NOKEY;
    return result;
  }
  // Skip empty elements that might be there
  while (scancodes[scancodes_rpos].scancode == COMMONSENSE_NOKEY &&
        (scancodes[scancodes_rpos].flags & USBQUEUE_RELEASED_MASK) == 0
  ) {
    scancodes_rpos = SCANCODES_NEXT(scancodes_rpos);
  }
  // MOVE the value from ring buffer to output buffer. Mark source as empty.
  scancode_t scancode = scancodes[scancodes_rpos];
  scancodes[scancodes_rpos].flags = 0;
  scancodes[scancodes_rpos].scancode = COMMONSENSE_NOKEY;
#ifdef MATRIX_LEVELS_DEBUG
  xprintf("sc: %d %d @ %d ms, lvl %d/%d", scancode & KEY_UP_MASK,
          scancode & SCANCODE_MASK, systime,
          level_buffer[scancodes_rpos],
          level_buffer_inst[scancodes_rpos]);
#endif
  return scancode;
}


inline void process_real_key(void) {
  scancode_t sc;
  uint8_t usb_sc;
  // Real keys are processed there. So modifiers can be processed right away,
  // not buffered.
  sc = process_scancode_buffer();
  if (sc.scancode == COMMONSENSE_NOKEY) {
    if ((sc.flags & KEY_UP_MASK)) {
      if (!USBQUEUE_IS_EMPTY) {
        push_back_scancode(sc);
      } else {
        /*
         * This is "All keys are up" signal, sun keyboard-style.
         * It is used to deal with stuck keys. Those appear due to layers - suppose
         * you press the key, then switch layer which has another key at that SC
         * position. When you release the key - non-existent key release is
         * generated, which is not that bad, but first key is stuck forever.
         */
        reset_reports();
        serial_reset_reports();
      }
    }
    return;
  }
  if (TEST_BIT(status_register, C2DEVSTATUS_SETUP_MODE)) {
    outbox.response_type = C2RESPONSE_SCANCODE;
    outbox.payload[0] = sc.flags;
    outbox.payload[1] = sc.scancode;
    usb_send_c2();
    return;
  }
  // Resolve USB keycode using current active layers
  for (uint8_t i = currentLayer; i >= 0; i--) {
    usb_sc = config.layers[i][sc.scancode];
    if (usb_sc != USBCODE_TRANSPARENT) {
      break;
    }
  }
  // xprintf("SC->KC: %d -> %d", sc, usb_sc);
  if (usb_sc < USBCODE_A) {
    if (usb_sc == USBCODE_EXP_TOGGLE  && !(sc.flags & USBQUEUE_RELEASED_MASK)) {
      exp_toggle();
    }
    // Dead key.
    return;
  }
  if ((usb_sc & 0xf8) == 0xa8) {
    process_layerMods(sc.scancode, usb_sc);
    return;
    /*
    TODO resolve problem where pressed mod keys are missing on the new layer.
     -> Do they get stuck?
     -> Do they automatically release?
    */
  }
  uint8_t keyflags = sc.flags | USBQUEUE_REAL_KEY_MASK;
  uint_fast16_t macro_ptr = lookup_macro(keyflags, usb_sc);
  bool do_play = (macro_ptr != MACRO_NOT_FOUND);
  bool do_queue = !do_play; // eat the macro-producing code.
  if (do_play && (sc.flags & USBQUEUE_RELEASED_MASK) &&
      (config.macros[macro_ptr + 1] & MACRO_TYPE_TAP)) {
    // Tap macro. Check if previous event was this key down and it's not too
    // late.
    do_queue = true;
    if ((usb_sc != pipeline_prev_usbkey) ||
        (pipeline_prev_usbkey_time + config.delayLib[DELAYS_TAP] < systime)) {
      // Nope!
      do_play = false;
    }
  }
  pipeline_prev_usbkey = usb_sc;
  pipeline_prev_usbkey_time = systime;
  /*
  NOTE: queue_usbcode skips non-zero cells in the buffer. Think what to do on
  overflow. NOTE2: we still want to maintain order? Otherwise linked list is
  probably what's doctor ordered (though expensive at 4B per pointer plus memory
  management)
  */
  if (do_queue)
    queue_usbcode(systime, keyflags, usb_sc);
  if (do_play)
    play_macro(macro_ptr);
  return;
}

#define NO_COOLDOWN USBQueue[pos].flags |= USBQUEUE_RELEASED_MASK;
/*
    Idea: not move readpos until keycode after it is processed.
    So, readpos to writepos would be the working area.

    TODO: maintain bitmap of currently pressed keys to release them on reset and
   for better KRO handling.
    TODO: implement out of order key release?
    NOTE ^ ^^ very rare situations where this is needed.
 */
inline void update_reports(void) {
  if (cooldown_timer > 0) {
    // Slow down! Delay 0 controls update rate.
    // we should be called every millisecond - so setting delay0 to 10 will
    // essentially makes it 100Hz keyboard with latency of 1kHz one.
    cooldown_timer--;
    return;
  }
  if (USBQUEUE_IS_EMPTY) {
    return;
  }
  // If there's change - find first non-empty buffer cell.
  // This results in infinite loop on empty buffer - must take care not to queue
  // NOEVENTs.
  while (USBQueue[USBQueue_rpos].keycode == USBCODE_NOEVENT) {
    USBQueue_rpos = KEYCODE_BUFFER_NEXT(USBQueue_rpos);
  }
  // xprintf("USB queue %d - %d", USBQueue_rpos, USBQueue_wpos);
  uint8_t pos = USBQueue_rpos;
  do {
    if (USBQueue[pos].keycode != USBCODE_NOEVENT &&
        USBQueue[pos].sysTime <= systime) {
      if (USBQueue[pos].keycode < USBCODE_A) {
        // side effect - key transparent till the bottom will toggle exp. header
        // But it should not ever be put on queue!
        exp_toggle();
        NO_COOLDOWN
      }
      // Codes you want filtered from reports MUST BE ABOVE THIS LINE!
      // -> Think of special code for collectively settings mods!
      else if (USBQueue[pos].keycode >= 0xe8) {
        update_consumer_report(&USBQueue[pos]);
      } else if (USBQueue[pos].keycode >= 0xa5 &&
                 USBQueue[pos].keycode <= 0xa7) {
        update_system_report(&USBQueue[pos]);
      } else {
        switch (output_direction) {
          case OUTPUT_DIRECTION_USB:
            update_keyboard_report(&USBQueue[pos]);
            break;
          case OUTPUT_DIRECTION_SERIAL:
            update_serial_keyboard_report(&USBQueue[pos]);
            break;
          default:
            break;
        }

      }
      if ((USBQueue[pos].flags & USBQUEUE_RELEASED_MASK) == 0) {
        // We only throttle keypresses. Key release doesn't slow us down -
        // minimum duration is guaranteed by fact that key release goes after
        // key press and keypress triggers cooldown.
        cooldown_timer = config.delayLib[DELAYS_EVENT]; // Actual update
                                                        // happened - reset
                                                        // cooldown.
        exp_keypress(
            USBQueue[pos].keycode); // Let the downstream filter by keycode
      }
      USBQueue[pos].keycode = USBCODE_NOEVENT;
      if (pos == USBQueue_rpos && pos != USBQueue_wpos) {
        // Only if we're at first position. Previous cells may contain future
        // actions otherwise! Also if not the last item - we don't want to
        // overrun the buffer.
        USBQueue_rpos = KEYCODE_BUFFER_NEXT(pos);
      }
      break;
    }
  } while (pos != USBQueue_wpos);
}

inline void pipeline_process(void) {
  if (false) {
    /*
            playing non-game macro
                if wait is blocking:
                    if SCB has macro SC release:
                        set that SCB pos to 0
                        play after-wait part of macro to USBCB w/proper
       timestamps
    */
  } else {
    process_real_key();
  }
  update_reports();
}

inline bool pipeline_process_wakeup(void) {
  scancode_t sc = process_scancode_buffer();
  if (sc.scancode == COMMONSENSE_NOKEY) {
    return false;
  }
  if ((sc.flags & KEY_UP_MASK) == 0) {
    return true;
  }
  return false;
}

void pipeline_init(void) {
  // Pipeline is worked on in main loop only, no point disabling IRQs to avoid
  // preemption.
  scan_reset();
  USBQueue_rpos = 0;
  USBQueue_wpos = 0;
  cooldown_timer = 0;
  memset(USBQueue, USBCODE_NOEVENT, sizeof USBQueue);
}