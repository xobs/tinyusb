/* 
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This file is part of the TinyUSB stack.
 */

#include "tusb_option.h"

// #if TUSB_OPT_DEVICE_ENABLED && (CFG_TUSB_MCU == OPT_MCU_FOMU_EPTRI)
#if 1

#include "device/dcd.h"
#include "dcd_fomu.h"
#include "csr.h"
#include "irq.h"
void fomu_error(uint32_t line);
void mputs(const char *str);
void mputln(const char *str);

//--------------------------------------------------------------------+
// SIE Command
//--------------------------------------------------------------------+

static uint8_t volatile out_buffer_length[16];
static uint8_t volatile * out_buffer[16];
static uint8_t volatile out_buffer_max[16];

static volatile bool tx_in_progress;
static volatile uint8_t tx_ep;
static volatile uint16_t tx_len;

//--------------------------------------------------------------------+
// PIPE HELPER
//--------------------------------------------------------------------+

static void finish_tx(void) {
    // // Don't allow requeueing -- only queue more data if the system is idle.
    // if (!(usb_in_status_read() & 2)) {
    //     return;
    // }

    // Don't send empty data
    if (!tx_in_progress) {
        return;
    }

    tx_in_progress = 0;
    dcd_event_xfer_complete(0, tx_ep, tx_len, XFER_RESULT_SUCCESS, true);
    return;
}

static void process_rx(bool in_isr) {
    // If there isn't any data in the FIFO, don't do anything.
    if (!(usb_out_status_read() & 1))
        return;

    uint8_t out_ep = (usb_out_status_read() >> 2) & 0xf;
    uint32_t total_read = 0;
    uint32_t current_offset = out_buffer_length[out_ep];
    while (usb_out_status_read() & 1) {
      uint8_t c = usb_out_data_read();
      total_read++;
      if (out_buffer_length[out_ep] < out_buffer_max[out_ep])
        out_buffer[out_ep][current_offset++] = c;
    }

    // Strip off the CRC16
    total_read -= 2;
    out_buffer_length[out_ep] += total_read;
    if (out_buffer_length[out_ep] > out_buffer_max[out_ep])
      out_buffer_length[out_ep] = out_buffer_max[out_ep];

    if (out_buffer_max[out_ep] == out_buffer_length[out_ep]) {
      out_buffer[out_ep] = NULL;
      dcd_event_xfer_complete(0, tu_edpt_addr(out_ep, TUSB_DIR_OUT), out_buffer_length[out_ep], XFER_RESULT_SUCCESS, in_isr);
    }

    // Acknowledge having received the data
    usb_out_ctrl_write(2);
}

//--------------------------------------------------------------------+
// CONTROLLER API
//--------------------------------------------------------------------+

// Initializes the USB peripheral for device mode and enables it.
void dcd_init(uint8_t rhport)
{
  (void) rhport;

  usb_pullup_out_write(0);
  usb_address_write(0);
  usb_out_ctrl_write(0);

  usb_setup_ev_enable_write(0);
  usb_in_ev_enable_write(0);
  usb_out_ev_enable_write(0);

  // Reset the IN handler
  usb_in_ctrl_write(0x20);

  // Reset the SETUP handler
  usb_setup_ctrl_write(0x04);

  // Reset the OUT handler
  usb_out_ctrl_write(0x04);

  // Enable all event handlers and clear their contents
  usb_setup_ev_pending_write(usb_setup_ev_pending_read());
  usb_in_ev_pending_write(usb_in_ev_pending_read());
  usb_out_ev_pending_write(usb_out_ev_pending_read());
  usb_setup_ev_enable_write(3);
  usb_in_ev_enable_write(1);
  usb_out_ev_enable_write(1);

  // Accept incoming data by default.
  usb_out_ctrl_write(2);

  // Turn on the external pullup
  usb_pullup_out_write(1);

  dcd_event_bus_signal(0, DCD_EVENT_BUS_RESET, false);
}

// Enables or disables the USB device interrupt(s). May be used to
// prevent concurrency issues when mutating data structures shared
// between main code and the interrupt handler.
void dcd_int_enable(uint8_t rhport)
{
  (void) rhport;
	irq_setmask(irq_getmask() | (1 << USB_INTERRUPT));
}

void dcd_int_disable(uint8_t rhport)
{
  (void) rhport;
  irq_setmask(irq_getmask() & ~(1 << USB_INTERRUPT));
}

// Called when the device is given a new bus address.
void dcd_set_address(uint8_t rhport, uint8_t dev_addr)
{
  (void)rhport;
  // Set address and then acknowledge the SETUP packet
  usb_address_write(dev_addr);

  // ACK the transfer (sets the address)
  usb_setup_ctrl_write(2);
}

// Called when the device received SET_CONFIG request, you can leave this
// empty if your peripheral does not require any specific action.
void dcd_set_config(uint8_t rhport, uint8_t config_num)
{
  (void) rhport;
  (void) config_num;
}

// Called to remote wake up host when suspended (e.g hid keyboard)
void dcd_remote_wakeup(uint8_t rhport)
{
  (void) rhport;
}

//--------------------------------------------------------------------+
// DCD Endpoint Port
//--------------------------------------------------------------------+
bool dcd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const * p_endpoint_desc)
{
  (void) rhport;

  if (p_endpoint_desc->bmAttributes.xfer == TUSB_XFER_ISOCHRONOUS)
    return false; // Not supported

  return true;
}

void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr)
{
  (void) rhport;
  if (tu_edpt_dir(ep_addr) == TUSB_DIR_OUT)
    usb_out_stall_write((1 << CSR_USB_OUT_STALL_STALL_OFFSET) | tu_edpt_number(ep_addr));
  else {
    usb_in_ctrl_write((1 << CSR_USB_IN_CTRL_STALL_OFFSET) | tu_edpt_number(ep_addr));
  }
}

void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr)
{
  (void) rhport;
  if (tu_edpt_dir(ep_addr) == TUSB_DIR_OUT)
    usb_out_stall_write((0 << CSR_USB_OUT_STALL_STALL_OFFSET) | tu_edpt_number(ep_addr));
  // IN endpoints will get unstalled when more data is written.
}

__attribute__((used))
uint8_t *last_tx_buffer;
__attribute__((used))
uint16_t last_tx_bytes;

bool dcd_edpt_xfer (uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes)
{
  (void)rhport;

  if (tu_edpt_dir(ep_addr) == TUSB_DIR_IN) {
    // These sorts of transfers are handled in hardware
    if ((tu_edpt_number(ep_addr) == 0) && (total_bytes == 0) && (buffer == 0)) {
      dcd_event_xfer_complete(0, ep_addr, total_bytes, XFER_RESULT_SUCCESS, false);
      return true;
    }
    uint32_t offset;
    // Wait for the tx pipe to free up
    while (tx_in_progress)
      ;
    tx_in_progress = 1;
    tx_ep = ep_addr;
    tx_len = total_bytes;

    for (offset = 0; offset < total_bytes; offset++) {
        usb_in_data_write(buffer[offset]);
    }
    // Updating the epno queues the data
    usb_in_ctrl_write(tu_edpt_number(ep_addr) & 0xf);
    last_tx_buffer = buffer;
    last_tx_bytes = total_bytes;
  }
  else if (tu_edpt_dir(ep_addr) == TUSB_DIR_OUT) {

    // Wait for the rx pipe to free up
    while (out_buffer[tu_edpt_number(ep_addr)])
      ;
    out_buffer_max[tu_edpt_number(ep_addr)] = total_bytes;
    out_buffer[tu_edpt_number(ep_addr)] = buffer;
    out_buffer_length[tu_edpt_number(ep_addr)] = 0;
    process_rx(false);
  }
  return true;
}

//--------------------------------------------------------------------+
// ISR
//--------------------------------------------------------------------+

void hal_dcd_isr(uint8_t rhport)
{
  uint8_t setup_pending   = usb_setup_ev_pending_read();
  uint8_t in_pending      = usb_in_ev_pending_read();
  uint8_t out_pending     = usb_out_ev_pending_read();
  usb_setup_ev_pending_write(setup_pending);
  usb_in_ev_pending_write(in_pending);
  usb_out_ev_pending_write(out_pending);

  // This event means a bus reset occurred.  Reset everything, and
  // abandon any further processing.
  if (setup_pending & 2) {
    usb_setup_ctrl_write(1 << CSR_USB_SETUP_CTRL_RESET_OFFSET);
    usb_in_ctrl_write(1 << CSR_USB_IN_CTRL_RESET_OFFSET);
    usb_out_ctrl_write(1 << CSR_USB_OUT_CTRL_RESET_OFFSET);

    dcd_event_bus_signal(0, DCD_EVENT_BUS_RESET, true);
    return;
  }

  // An "IN" transaction just completed.
  // Note that due to the way tinyusb's callback system is implemented,
  // we must handle IN and OUT packets before we handle SETUP packets.
  // This ensures that any responses to SETUP packets aren't overwritten.
  // For example, oftentimes a host will request part of a descriptor
  // to begin with, then make a subsequent request.  If we don't handle
  // the IN packets first, then the second request will be truncated.
  if (in_pending) {
    finish_tx();
  } 

  // An "OUT" transaction just completed so we have new data.
  // (But only if we can accept the data)
  if (out_pending) {
    process_rx(true);
  }

  // We got a SETUP packet.  Copy it to the setup buffer and clear
  // the "pending" bit.
  if (setup_pending & 1) {
    // Setup packets are always 8 bytes, plus two bytes
    // of crc16
    uint8_t setup_packet[10];
    uint32_t setup_length = 0;

    if (!(usb_setup_status_read() & 1))
      fomu_error(__LINE__);

    while (usb_setup_status_read() & 1) {
      uint8_t c = usb_setup_data_read();
      if (setup_length < sizeof(setup_packet))
        setup_packet[setup_length] = c;
      setup_length++;
    }

    // If we have 10 bytes, that's a full SETUP packet plus CRC16.
    // Otherwise, it was an RX error.
    if (setup_length == 10) {
      dcd_event_setup_received(rhport, setup_packet, true);
      // Acknowledge the packet, so long as it isn't a SET_ADDRESS
      // packet.  If it is, leave it unacknowledged and we'll do this
      // in the `dcd_set_address` function instead.
      if (!((setup_packet[0] == 0x00) && (setup_packet[1] == 0x05)))
        usb_setup_ctrl_write(2);
    }
    else {
      fomu_error(__LINE__);
    }
  }
}

#endif