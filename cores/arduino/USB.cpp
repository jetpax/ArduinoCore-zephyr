/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/uart/cdc_acm.h>
#include <zephyr/usb/usb_device.h>
#include <SerialUSB.h>

#if ZARD_FIRST_SERIAL_IS_SERIALUSB
const struct device *const usb_dev =
	DEVICE_DT_GET(DT_PHANDLE_BY_IDX(DT_PATH(zephyr_user), cdc_acm_serial, 0));

void __attribute__((weak)) _on_1200_bps() {
#ifndef CONFIG_ARDUINO_USB_LOADER_OWNED
	/* Default behaviour: the 1200-bps touch resets into the bootloader.
	 * NVIC_SystemReset is a Cortex-M intrinsic; on loader-owned targets
	 * (e.g. aarch64 BCM2710, which has no such reset) the loader handles
	 * the upload touch itself, so this path is compiled out -- it would
	 * neither link nor apply there. */
	NVIC_SystemReset();
#endif
}

void arduino::SerialUSB_::baudChangeHandler(const struct device *dev, uint32_t rate) {
	(void)dev; // unused
	if (rate == 1200) {
		usb_disable();
		_on_1200_bps();
	}
}

#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
int arduino::SerialUSB_::usb_disable() {
	// To avoid Cannot perform port reset: 1200-bps touch: setting DTR to OFF: protocol error
	k_sleep(K_MSEC(100));
	return usbd_disable(Serial._usbd);
}

void arduino::SerialUSB_::usbd_next_cb(struct usbd_context *const ctx, const struct usbd_msg *msg) {
	if (usbd_can_detect_vbus(ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			usbd_enable(ctx);
		}

		if (msg->type == USBD_MSG_VBUS_REMOVED) {
			usbd_disable(ctx);
		}
	}

	if (msg->type == USBD_MSG_CDC_ACM_LINE_CODING) {
		uint32_t baudrate;
		uart_line_ctrl_get(Serial.uart, UART_LINE_CTRL_BAUD_RATE, &baudrate);
		Serial.baudChangeHandler(nullptr, baudrate);
	}
}

int arduino::SerialUSB_::enable_usb_device_next(void) {
	int err;

	_usbd = usbd_init_device(arduino::SerialUSB_::usbd_next_cb);
	if (_usbd == NULL) {
		return -ENODEV;
	}

	if (!usbd_can_detect_vbus(_usbd)) {
		err = usbd_enable(_usbd);
		if (err) {
			return err;
		}
	}
	return 0;
}
#endif /* defined(CONFIG_USB_DEVICE_STACK_NEXT) */

void arduino::SerialUSB_::begin(unsigned long baudrate, uint16_t config) {
	if (!started) {
#ifndef CONFIG_USB_DEVICE_STACK_NEXT
		usb_enable(NULL);
#ifndef CONFIG_CDC_ACM_DTE_RATE_CALLBACK_SUPPORT
#warning "Can't read CDC baud change, please enable CONFIG_CDC_ACM_DTE_RATE_CALLBACK_SUPPORT"
#else
		cdc_acm_dte_rate_callback_set(usb_dev, SerialUSB_::baudChangeHandler);
#endif
#elif !defined(CONFIG_ARDUINO_USB_LOADER_OWNED)
		enable_usb_device_next();
#endif
		/* When CONFIG_ARDUINO_USB_LOADER_OWNED, the loader has already
		 * brought USBD up and registered the upload-touch callback; the
		 * sketch only needs to attach to the CDC ACM device, which
		 * ZephyrSerial::begin does (configure + RX-enable). */
		ZephyrSerial::begin(baudrate, config);
		started = true;
	}
}

arduino::SerialUSB_::operator bool() {
	uart_line_ctrl_get(uart, UART_LINE_CTRL_DTR, &dtr);
	return dtr;
}

size_t arduino::SerialUSB_::write(const uint8_t *buffer, size_t size) {
	/* No DTR gate: the cdc_acm driver handles host-not-listening on its
	 * own (TX FIFO buffers, eventual NAK). Gating here means Arduino
	 * IDE Serial Monitor on macOS — which doesn't always raise DTR —
	 * sees zero echo from sketches like HelloSerial. operator bool()
	 * still reports DTR so sketches that want "wait for host" can do
	 * `while (!Serial);` explicitly. */
	return arduino::ZephyrSerial::write(buffer, size);
}

void arduino::SerialUSB_::flush() {
	arduino::ZephyrSerial::flush();
}

arduino::SerialUSB_ Serial(usb_dev);
#endif
