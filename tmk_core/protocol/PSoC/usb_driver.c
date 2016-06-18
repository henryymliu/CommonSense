/*
 *
 * Copyright (C) 2016 DMA <dma@ya.ru>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation. 
*/
#include <stdio.h>
#include <project.h>
#include <stdbool.h>
#include "globals.h"
#include "host.h"
#include "led.h"
#include "usb_driver.h"
#include "wait.h"
#include "print.h"
#include "c2/c2_protocol.h"
#include "CyFlash.h"

host_driver_t kbd_driver = {
    keyboard_leds,
    send_keyboard,
    send_mouse,
    send_system,
    send_consumer
};

host_driver_t *psoc_driver(void){
    return &kbd_driver;
}

void report_status(void)
{
    outbox.response_type = C2RESPONSE_STATUS;
    outbox.payload[0] = 0;
    outbox.payload[0] |= (status_register.emergency_stop << C2DEVSTASTUS_EMERGENCY);
    outbox.payload[0] |= (status_register.matrix_output << C2DEVSTASTUS_MATRIXOUTPUT);
    outbox.payload[1] = DEVICE_VER_MAJOR;
    outbox.payload[2] = DEVICE_VER_MINOR;
    EEPROM_UpdateTemperature();
    outbox.payload[3] = dieTemperature[0];
    outbox.payload[4] = dieTemperature[1];
    usb_send();
}

void receive_config_block(void){
    // TODO define offset via transfer block size and packet size
    memcpy(config.raw + (inbox.payload[0] * CONFIG_TRANSFER_BLOCK_SIZE), inbox.payload+31, CONFIG_TRANSFER_BLOCK_SIZE);
    outbox.response_type = C2RESPONSE_CONFIG;
    outbox.payload[0] = inbox.payload[0];
    usb_send();
}

void send_config_block(void){
    outbox.response_type = C2RESPONSE_CONFIG;
    outbox.payload[0] = inbox.payload[0];
    memcpy(outbox.payload + 31, config.raw + (inbox.payload[0] * CONFIG_TRANSFER_BLOCK_SIZE), CONFIG_TRANSFER_BLOCK_SIZE);
    usb_send();
}

void save_config(void){
    EEPROM_Start();
    CyDelayUs(5);
    EEPROM_UpdateTemperature();
    xprintf("Updating EEPROM GO!");
    uint16 bytes_modified = 0;
    for(uint16 i = 0; i < EEPROM_BYTESIZE; i++)
        if(config.raw[i] != EEPROM_ReadByte(i)) {
            EEPROM_WriteByte(config.raw[i], i);
            bytes_modified++;
        }
    EEPROM_Stop();
    xprintf("Written %d bytes!", bytes_modified);
}

void load_config(void){
    EEPROM_Start();
    CyDelayUs(5);
    // Copypaste from EEPROM.c/EEPROM_ReadByte! Use with causion!
    uint8 interruptState;
    interruptState = CyEnterCriticalSection();
    /* Request access to EEPROM for reading.
    This is needed to reserve PHUB for read operation from EEPROM */
    CyEEPROM_ReadReserve();
    memcpy(config.raw, (void *)CYDEV_EE_BASE, CYDEV_EE_SIZE);
    /* Release EEPROM array */
    CyEEPROM_ReadRelease();
    CyExitCriticalSection(interruptState);
    EEPROM_Stop();
}

void process_msg(void)
{
    memset(outbox.raw, 0x00, sizeof(outbox));
    switch (inbox.command) {
    case C2CMD_EWO:
        status_register.emergency_stop = inbox.payload[0];
        xprintf("EWO signal received: %d", inbox.payload[0]);
        break;
    case C2CMD_GET_STATUS:
        report_status();
        break;
    case C2CMD_ENTER_BOOTLOADER:
        xprintf("Jumping to bootloader..");
        Boot_Load(); //Does not return, no need for break
    case C2CMD_UPLOAD_CONFIG:
        receive_config_block();
        break;
    case C2CMD_DOWNLOAD_CONFIG:
        send_config_block();
        break;
    case C2CMD_COMMIT:
        save_config();
        break;
    case C2CMD_ROLLBACK:
        xprintf("Resetting..");
        CySoftwareReset(); //Does not return, no need for break.
    case C2CMD_GET_MATRIX_STATE:
        status_register.matrix_output = inbox.payload[0];
        break;
    default:
        break;
    }
    acknowledge_command();
}
void usb_init(void)
{
    /* Start USB operation with 5-V operation. */
    USB_Start(0u, USB_5V_OPERATION);

    /* Wait for device to enumerate */
    while (0u == USB_GetConfiguration())
    {
        wait_ms(100);
    }
    host_set_driver(psoc_driver());
    // Start listening!
    USB_EnableOutEP(INBOX_EP);


}
/*******************************************************************************
 * Host driver 
 ******************************************************************************/
static uint8_t keyboard_led_status;
uint8_t keyboard_leds(void)
{
    return ~keyboard_led_status;
}

void USB_EP_8_ISR_ExitCallback(void)
{
    USB_ReadOutEP(INBOX_EP, inbox.raw, USB_GetEPCount(INBOX_EP));
    message_for_you_in_the_lobby = true;
}

void acknowledge_command(void)
{
    message_for_you_in_the_lobby = false;
    USB_EnableOutEP(INBOX_EP);
}

void usb_send(void)
{
    USB_LoadInEP(OUTBOX_EP, outbox.raw, sizeof(outbox.raw));
//    /* Wait for ACK after loading data. */
    while (0u == USB_GetEPAckState(OUTBOX_EP))
    {
    }
}

void send_keyboard(report_keyboard_t *report)
{
    USB_LoadInEP(1, (uint8_t *)report, sizeof(report_keyboard_t));
    /* Wait for ACK after loading data. */
    while (0u == USB_GetEPAckState(1))
    {
    }
 }

void send_mouse(report_mouse_t *report)
{
}

/* extra report structure */
typedef struct {
  uint8_t report_id;
  uint16_t usage;
} __attribute__ ((packed)) report_extra_t; 

void send_extrakeys(uint8_t report_id, uint16_t data)
{
  report_extra_t report = {
    .report_id = report_id,
    .usage = data
  };

    USB_LoadInEP(1, (uint8_t *)&report, sizeof(report_extra_t));
    /* Wait for ACK after loading data. */
    while (0u == USB_GetEPAckState(1))
    {
    }
}

void send_system(uint16_t data)
{
    send_extrakeys(2, data);
}

void send_consumer(uint16_t data)
{
    send_extrakeys(3, data);
 }
