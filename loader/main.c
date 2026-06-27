/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zephyr/sys/printk.h"
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sketch);

#include <zephyr/kernel.h>
#include <zephyr/llext/llext.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/logging/log_ctrl.h>

#include <stdlib.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/uart/cdc_acm.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>

#if defined(CONFIG_ARDUINO_SKETCH_LOADER_FS)
#include <zephyr/fs/fs.h>
#include <zephyr/llext/fs_loader.h>
#if defined(CONFIG_ARDUINO_SKETCH_LOADER_CDC_UPLOAD)
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>
#endif
#else
#include <zephyr/storage/flash_map.h>
#include <zephyr/llext/buf_loader.h>
#include <zephyr/devicetree/fixed-partitions.h>
#endif

#define HEADER_LEN 16

struct sketch_header_v1 {
	uint8_t ver;    // @ 0x07
	uint32_t len;   // @ 0x08
	uint16_t magic; // @ 0x0c
	uint8_t flags;  // @ 0x0e
} __attribute__((packed));

#define SKETCH_FLAG_DEBUG        0x01
#define SKETCH_FLAG_LINKED       0x02
#define SKETCH_FLAG_IMMEDIATE    0x04
#define SKETCH_FLAG_WAIT_FOR_APP 0x08

#define SKETCH_RAM_BUFFER_LEN 131072

/* Need to replicate logic from zephyrSerial.h to avoid C++ here */
#define ZARD_FIRST_SERIAL_IS_SERIALUSB                                                             \
	DT_NODE_HAS_PROP(DT_PATH(zephyr_user), cdc_acm_serial) &&                                      \
		(CONFIG_USB_CDC_ACM || CONFIG_USBD_CDC_ACM_CLASS)
#if ZARD_FIRST_SERIAL_IS_SERIALUSB
const struct device *const usb_dev =
	DEVICE_DT_GET(DT_PHANDLE_BY_IDX(DT_PATH(zephyr_user), cdc_acm_serial, 0));

#if CONFIG_USB_DEVICE_STACK_NEXT
#include <zephyr/usb/usbd.h>
struct usbd_context *usbd_init_device(usbd_msg_cb_t msg_cb);

int usb_enable(usb_dc_status_callback status_cb) {
	int err;
	struct usbd_context *_usbd = usbd_init_device(NULL);
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
#endif

#if CONFIG_SHELL
static int enable_shell_usb(void) {
	bool log_backend = CONFIG_SHELL_BACKEND_SERIAL_LOG_LEVEL > 0;
	uint32_t level = (CONFIG_SHELL_BACKEND_SERIAL_LOG_LEVEL > LOG_LEVEL_DBG) ?
						 CONFIG_LOG_MAX_LEVEL :
						 CONFIG_SHELL_BACKEND_SERIAL_LOG_LEVEL;
	static const struct shell_backend_config_flags cfg_flags = SHELL_DEFAULT_BACKEND_CONFIG_FLAGS;

	shell_init(shell_backend_uart_get_ptr(), usb_dev, cfg_flags, log_backend, level);

	return 0;
}
#endif
#endif

#ifdef CONFIG_USERSPACE
K_THREAD_STACK_DEFINE(llext_stack, CONFIG_MAIN_STACK_SIZE);
struct k_thread llext_thread;

void llext_entry(void *arg0, void *arg1, void *arg2) {
	void (*fn)(struct llext_loader *, struct llext *) = arg0;
	fn(arg1, arg2);
}
#endif /* CONFIG_USERSPACE */

#if !defined(CONFIG_ARDUINO_SKETCH_LOADER_FS)
/* Export Flash parameters for use by core building scripts */
__attribute__((retain)) const uintptr_t sketch_base_addr =
	DT_REG_ADDR(DT_GPARENT(DT_NODELABEL(user_sketch))) + DT_REG_ADDR(DT_NODELABEL(user_sketch));
__attribute__((retain)) const uintptr_t sketch_max_size = DT_REG_SIZE(DT_NODELABEL(user_sketch));

/* Determine maximum size of the loader application */
#if DT_HAS_FIXED_PARTITION_LABEL(image_0) /* "image_0" partition size */
#define LOADER_MAX_SIZE DT_REG_SIZE(DT_NODE_BY_FIXED_PARTITION_LABEL(image_0))
#elif CONFIG_FLASH_LOAD_SIZE > 0 /* forced value from Kconfig */
#define LOADER_MAX_SIZE CONFIG_FLASH_LOAD_SIZE
#elif CONFIG_FLASH_LOAD_OFFSET /* heuristic: size of Flash minus load offset */
#define LOADER_MAX_SIZE (DT_REG_SIZE(DT_NODELABEL(flash0)) - CONFIG_FLASH_LOAD_OFFSET)
#else /* default: size of whole Flash */
#define LOADER_MAX_SIZE DT_REG_SIZE(DT_NODELABEL(flash0))
#endif
__attribute__((retain)) const uintptr_t loader_max_size = LOADER_MAX_SIZE;

struct backup_store {
	uint32_t wait_for_app_magic;
};
volatile __stm32_backup_sram_section struct backup_store backup;
#endif /* !CONFIG_ARDUINO_SKETCH_LOADER_FS */

__maybe_unused static int loader(const struct shell *sh) {
#if defined(CONFIG_ARDUINO_SKETCH_LOADER_FS)
	const char *path = CONFIG_ARDUINO_SKETCH_LOADER_FS_PATH;
	struct llext_fs_loader fs_loader = LLEXT_FS_LOADER(path);
	struct llext_loader *ldr = &fs_loader.loader;
	struct llext_load_param ldr_parm = LLEXT_LOAD_PARAM_DEFAULT;
	struct llext *ext;
	int res;

	printk("Loading sketch from %s\n", path);
	res = llext_load(ldr, "sketch", &ext, &ldr_parm);
	if (res) {
		printk("Failed to load %s, rc %d\n", path, res);
		return res;
	}

	void (*main_fn)() = llext_find_sym(&ext->exp_tab, "main");
	if (!main_fn) {
		printk("Failed to find main\n");
		return -ENOENT;
	}

	llext_bootstrap(ext, main_fn, NULL);
	return 0;
#else
	const struct flash_area *fa;
	int rc;

	/* Test that attempting to open a disabled flash area fails */
	rc = flash_area_open(FIXED_PARTITION_ID(user_sketch), &fa);
	if (rc) {
		printk("Failed to open flash area, rc %d\n", rc);
		return rc;
	}

	uintptr_t base_addr =
		DT_REG_ADDR(DT_GPARENT(DT_NODELABEL(user_sketch))) + DT_REG_ADDR(DT_NODELABEL(user_sketch));

	char header[HEADER_LEN];
	rc = flash_area_read(fa, 0, header, sizeof(header));
	if (rc) {
		printk("Failed to read header, rc %d\n", rc);
		return rc;
	}

	bool sketch_valid = true;
	struct sketch_header_v1 *sketch_hdr = (struct sketch_header_v1 *)(header + 7);
	if (sketch_hdr->ver != 0x1 || sketch_hdr->magic != 0x2341) {
		printk("Invalid sketch header\n");
		sketch_valid = false;
		// This is not a valid sketch, but try to start a shell anyway
	}

#if ZARD_FIRST_SERIAL_IS_SERIALUSB
	int debug = (!sketch_valid) || (sketch_hdr->flags & SKETCH_FLAG_DEBUG);
#if CONFIG_SHELL
	if (strcmp(k_thread_name_get(k_current_get()), "main") == 0) {
		// disables default shell on UART
		shell_uninit(shell_backend_uart_get_ptr(), NULL);
		// enables USB and starts the shell
		usb_enable(NULL);
		int dtr;
		do {
			// wait for the serial port to open
			uart_line_ctrl_get(usb_dev, UART_LINE_CTRL_DTR, &dtr);
			k_sleep(K_MSEC(100));
		} while (!dtr);
		enable_shell_usb();
	}
#elif CONFIG_LOG
#if !CONFIG_USB_DEVICE_INITIALIZE_AT_BOOT
	if (debug) {
		usb_enable(NULL);
	}
#endif
	for (int i = 0; i < log_backend_count_get(); i++) {
		const struct log_backend *backend;
		backend = log_backend_get(i);
		log_backend_init(backend);
		log_backend_enable(backend, backend->cb->ctx, CONFIG_LOG_DEFAULT_LEVEL);
		if (!debug) {
			break;
		}
	}
#endif
#endif

#if defined(CONFIG_BOARD_ARDUINO_UNO_Q)
	void matrixBegin(void);
	void matrixEnd(void);
	void matrixPlay(uint8_t *buf, uint32_t len);
	void matrixSetGrayscaleBits(uint8_t _max);
	void matrixGrayscaleWrite(uint8_t *buf);
#include "bootanimation.h"

	uint8_t *_bootanimation = (uint8_t *)bootanimation;
	size_t _bootanimation_len = bootanimation_len;
	uint8_t *_bootanimation_end = (uint8_t *)bootanimation_end;
	size_t _bootanimation_end_len = bootanimation_end_len;

	__attribute__((packed)) struct bootanimation_user_data {
		size_t magic; // must be 0xBA for bootanimation
		size_t len_loop;
		size_t len_end;
		size_t empty;
		char buf_loop;
	};

	backup.wait_for_app_magic = 0;

	uintptr_t bootanimation_addr = DT_REG_ADDR(DT_GPARENT(DT_NODELABEL(bootanimation))) +
								   DT_REG_ADDR(DT_NODELABEL(bootanimation));

	struct bootanimation_user_data *user_bootanimation =
		(struct bootanimation_user_data *)bootanimation_addr;
	if (user_bootanimation->magic == 0xBA) {
		_bootanimation = &(user_bootanimation->buf_loop);
		_bootanimation_len = user_bootanimation->len_loop;
		_bootanimation_end_len = user_bootanimation->len_end;
		_bootanimation_end = _bootanimation + user_bootanimation->len_loop;
	}

	if ((!sketch_valid) || !(sketch_hdr->flags & SKETCH_FLAG_IMMEDIATE)) {
		// Start the bootanimation while waiting for the MPU to boot
		const struct gpio_dt_spec spec =
			GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), control_gpios, 0);

		gpio_pin_configure_dt(&spec, GPIO_INPUT | GPIO_PULL_DOWN);
		k_sleep(K_MSEC(200));
		if (gpio_pin_get_dt(&spec) == 0) {
			matrixBegin();
			matrixSetGrayscaleBits(8);
			while (gpio_pin_get_dt(&spec) == 0) {
				matrixPlay(_bootanimation, _bootanimation_len);
			}
			matrixPlay(_bootanimation_end, _bootanimation_end_len);
			uint8_t _framebuffer[104] = {0};
			matrixGrayscaleWrite(_framebuffer);
			k_sleep(K_MSEC(10));
			matrixEnd();
		}

		if (sketch_hdr->flags & SKETCH_FLAG_WAIT_FOR_APP) {
			while (backup.wait_for_app_magic == 0) {
				k_sleep(K_MSEC(100));
			}
		}
	}
#endif

	size_t sketch_buf_len = sketch_hdr->len;

	if (sketch_hdr->flags & SKETCH_FLAG_LINKED) {
#ifdef CONFIG_BOARD_ARDUINO_PORTENTA_C33
#if CONFIG_MPU
		barrier_dmem_fence_full();
#endif
#if CONFIG_DCACHE
		barrier_dsync_fence_full();
#endif
#if CONFIG_ICACHE
		barrier_isync_fence_full();
#endif
#endif

		extern struct k_heap llext_heap;
		typedef void (*entry_point_t)(struct k_heap *heap, size_t heap_size);
		entry_point_t entry_point = (entry_point_t)(base_addr + HEADER_LEN + 1);
		entry_point(&llext_heap, llext_heap.heap.init_bytes);
		// should never reach here
		for (;;) {
			k_sleep(K_FOREVER);
		}
	}

#if defined(CONFIG_LLEXT_STORAGE_WRITABLE)
	uint8_t *sketch_buf = k_aligned_alloc(4096, sketch_buf_len);

	if (!sketch_buf) {
		printk("Unable to allocate %d bytes\n", sketch_buf_len);
		return -ENOMEM;
	}

	rc = flash_area_read(fa, 0, sketch_buf, sketch_buf_len);
	if (rc) {
		printk("Failed to read sketch area, rc %d\n", rc);
		return rc;
	}
#else
	// Assuming the sketch is stored in the same flash device as the loader
	uint8_t *sketch_buf = (uint8_t *)base_addr;
#endif

#ifdef CONFIG_LLEXT
	struct llext_buf_loader buf_loader = LLEXT_BUF_LOADER(sketch_buf, sketch_buf_len);
	struct llext_loader *ldr = &buf_loader.loader;

	LOG_HEXDUMP_DBG(sketch_buf, 4, "4 byte MAGIC");

	struct llext_load_param ldr_parm = LLEXT_LOAD_PARAM_DEFAULT;
	struct llext *ext;
	int res;

	res = llext_load(ldr, "sketch", &ext, &ldr_parm);
	if (res) {
		printk("Failed to load sketch, rc %d\n", res);
		return res;
	}

	void (*main_fn)() = llext_find_sym(&ext->exp_tab, "main");
	if (!main_fn) {
		printk("Failed to find main function\n");
		return -ENOENT;
	}
#endif

#ifdef CONFIG_USERSPACE
	/*
	 * Due to the number of MPU regions on some parts with MPU (USERSPACE)
	 * enabled we need to always call into the extension from a new dedicated
	 * thread to avoid running out of MPU regions on some parts.
	 *
	 * This is part dependent behavior and certainly on MMU capable parts
	 * this should not be needed! This test however is here to be generic
	 * across as many parts as possible.
	 */
	struct k_mem_domain domain;

	k_mem_domain_init(&domain, 0, NULL);

#ifdef Z_LIBC_PARTITION_EXISTS
	k_mem_domain_add_partition(&domain, &z_libc_partition);
#endif

	res = llext_add_domain(ext, &domain);
	if (res == -ENOSPC) {
		printk("Too many memory partitions for this particular hardware\n");
		return -1;
	}

	k_thread_create(&llext_thread, llext_stack, K_THREAD_STACK_SIZEOF(llext_stack), &llext_entry,
					llext_bootstrap, ext, main_fn, 1, K_INHERIT_PERMS, K_FOREVER);

	k_mem_domain_add_thread(&domain, &llext_thread);

	k_thread_start(&llext_thread);
	k_thread_join(&llext_thread, K_FOREVER);
#else

#ifdef CONFIG_LLEXT
	llext_bootstrap(ext, main_fn, NULL);
#endif

#endif

#endif /* !CONFIG_ARDUINO_SKETCH_LOADER_FS */

	return 0;
}

#if CONFIG_SHELL
SHELL_CMD_REGISTER(sketch, NULL, "Run sketch", loader);
#endif

#if defined(CONFIG_ARDUINO_SKETCH_LOADER_CDC_UPLOAD)
/*
 * Phase-6 in-process CDC upload + swap supervisor (rpi_zero_2w / BCM2710).
 *
 * The loader owns USB: it brings USBD up at boot, runs the sketch in its own
 * thread, and watches the CDC line for the IDE's 1200-bps "enter upload"
 * touch. On the touch it aborts the running sketch, receives a fresh
 * sketch.llext over CDC, writes it to the firmware-owned SD, and starts it --
 * all in-process, no reboot (this SoC has no sys_reboot). Because the firmware
 * owns the SD the entire time, a sketch can still use it for Storage.
 *
 * Wire protocol (host -> device): "PZUP" + <le32 length> + <length bytes>;
 * the device replies "OK\n" or "ERR ...\n" on the same CDC line. Ported from
 * the hardware-proven spike PiZZa/os/Arduino/spikes/cdc-loader.
 */

/* Supervisor runs above the sketch so its 30 ms baud poll preempts even a
 * busy-looping sketch (this board's lowest preemptible prio is
 * CONFIG_MAIN_THREAD_PRIORITY == 14, so the sketch cannot go lower; the
 * supervisor goes higher instead). */
#define CDC_SUPERVISOR_PRIO 7
#define CDC_SKETCH_PRIO     CONFIG_MAIN_THREAD_PRIORITY

#define UP_MAGIC "PZUP"
#define SKETCH_PATH CONFIG_ARDUINO_SKETCH_LOADER_FS_PATH
/* Receive into a temp file and rename on success, so an interrupted upload
 * (stall, unplug, power loss) never corrupts the live sketch. */
#define SKETCH_TMP_PATH SKETCH_PATH ".tmp"

/* The CDC ACM device is `usb_dev` (declared above under
 * ZARD_FIRST_SERIAL_IS_SERIALUSB) -- the same node the sketch's `Serial`
 * binds to. */
/* Sized to hold a whole typical sketch so the USB receive never outruns it and
 * the flow-control path (disable/re-enable RX, below) doesn't engage -- that
 * cycling races cdc_acm's in-flight OUT transfer and spams "RX transfer already
 * in progress". Flow control stays as the correctness net for sketches larger
 * than this. 32 KiB is the non-large ring max (UINT16_MAX/2); enabling
 * CONFIG_RING_BUFFER_LARGE to go bigger would change struct ring_buf's ABI,
 * which the Arduino `Serial` embeds -- breaking every prebuilt sketch. So cap
 * here and let flow control handle the rare >32 KiB sketch. */
RING_BUF_DECLARE(cdc_rx_rb, 32767);
/* Set by the ISR when it disables RX on a full ring; cleared by the consumer
 * when it re-enables. Lets cdc_rx_exact re-enable RX only on the paused->active
 * edge instead of every drain (avoids cdc_acm "RX transfer already in progress"
 * spam). */
static volatile bool cdc_rx_paused;

/* CDC RX ISR: drain the FIFO into the loader's ring buffer, but only as far as
 * there is room. When the ring fills we disable the RX IRQ and leave the bytes
 * in the CDC FIFO -- the cdc_acm OUT endpoint then NAKs and the host throttles
 * (flow control). The consumer (cdc_rx_exact) re-enables RX after it drains.
 * Without this, a sketch larger than the ring (8 KiB) overruns it and
 * ring_buf_put drops bytes, so the receive stalls forever waiting for data
 * that was lost. */
static void cdc_rx_isr(const struct device *dev, void *user) {
	ARG_UNUSED(user);
	uart_irq_update(dev);
	while (uart_irq_rx_ready(dev)) {
		uint8_t tmp[64];
		uint32_t space = ring_buf_space_get(&cdc_rx_rb);

		if (space == 0) {
			cdc_rx_paused = true;
			uart_irq_rx_disable(dev);
			break;
		}

		int n = uart_fifo_read(dev, tmp, MIN(space, sizeof(tmp)));

		if (n <= 0) {
			break;
		}
		ring_buf_put(&cdc_rx_rb, tmp, n);
	}
}

/* Take CDC RX from whoever currently holds it (a running sketch's Serial) and
 * route it into the loader's ring buffer for the upload window. */
static void cdc_rx_take(void) {
	uart_irq_rx_disable(usb_dev);
	uart_irq_callback_set(usb_dev, cdc_rx_isr);
	ring_buf_reset(&cdc_rx_rb);
	cdc_rx_paused = false;
	uart_irq_rx_enable(usb_dev);
}

/* Block until exactly len bytes have been pulled from the ring buffer. After
 * freeing space, re-enable RX in case the ISR disabled it on a full ring
 * (see cdc_rx_isr -- flow control for sketches larger than the ring). */
static void cdc_rx_exact(uint8_t *dst, size_t len) {
	size_t got = 0;

	while (got < len) {
		uint32_t r = ring_buf_get(&cdc_rx_rb, dst + got, len - got);

		got += r;
		if (r > 0) {
			if (cdc_rx_paused) {
				cdc_rx_paused = false;
				uart_irq_rx_enable(usb_dev);
			}
		} else {
			k_msleep(2);
		}
	}
}

static void cdc_print(const char *s) {
	for (; *s != '\0'; s++) {
		uart_poll_out(usb_dev, (uint8_t)*s);
	}
}

/* Receive one framed sketch over CDC and write it to the SD card. */
static int cdc_recv_sketch(void) {
	uint8_t hdr[8];

	cdc_rx_exact(hdr, sizeof(hdr));
	if (memcmp(hdr, UP_MAGIC, 4) != 0) {
		LOG_ERR("upload: bad magic %02x %02x %02x %02x", hdr[0], hdr[1], hdr[2], hdr[3]);
		cdc_print("ERR magic\n");
		return -EINVAL;
	}

	uint32_t len = sys_get_le32(&hdr[4]);

	LOG_INF("upload: receiving %u bytes -> %s", len, SKETCH_PATH);

	struct fs_file_t f;

	fs_file_t_init(&f);
	int rc = fs_open(&f, SKETCH_TMP_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);

	if (rc) {
		LOG_ERR("upload: fs_open(%s) = %d", SKETCH_TMP_PATH, rc);
		cdc_print("ERR open\n");
		return rc;
	}

	uint32_t left = len;
	uint8_t buf[256];

	while (left > 0) {
		uint32_t chunk = MIN(left, sizeof(buf));

		cdc_rx_exact(buf, chunk);
		ssize_t w = fs_write(&f, buf, chunk);

		if (w < 0) {
			LOG_ERR("upload: fs_write = %d", (int)w);
			fs_close(&f);
			(void)fs_unlink(SKETCH_TMP_PATH);
			cdc_print("ERR write\n");
			return (int)w;
		}
		left -= chunk;
	}

	fs_close(&f);

	/* The new sketch is fully on disk now; swap it in. Up to here only the
	 * temp file was touched, so an interrupted receive left the live sketch
	 * intact. (FATFS fs_rename fails if the target exists -- unlink first.) */
	(void)fs_unlink(SKETCH_PATH);
	rc = fs_rename(SKETCH_TMP_PATH, SKETCH_PATH);
	if (rc) {
		LOG_ERR("upload: rename %s -> %s = %d", SKETCH_TMP_PATH, SKETCH_PATH, rc);
		cdc_print("ERR rename\n");
		return rc;
	}

	LOG_INF("upload: wrote %u bytes to %s", len, SKETCH_PATH);
	cdc_print("OK\n");
	return 0;
}

/* The loaded sketch runs in its own thread so the loader keeps control to
 * watch the CDC for the upload touch while the sketch loops forever. */
static struct llext *cdc_cur_ext;
static K_THREAD_STACK_DEFINE(cdc_sketch_stack, 16384);
static struct k_thread cdc_sketch_thread;
static bool cdc_sketch_active;

static void cdc_sketch_entry(void *entry, void *b, void *c) {
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	llext_bootstrap(cdc_cur_ext, (llext_entry_fn_t)entry, NULL);
}

/* Load SKETCH_PATH and start it in a thread. */
static int cdc_start_sketch(void) {
	struct llext_fs_loader fsl = LLEXT_FS_LOADER(SKETCH_PATH);
	struct llext_load_param parm = LLEXT_LOAD_PARAM_DEFAULT;
	int rc = llext_load(&fsl.loader, "sketch", &cdc_cur_ext, &parm);

	if (rc) {
		LOG_ERR("llext_load(%s) = %d", SKETCH_PATH, rc);
		cdc_cur_ext = NULL;
		return rc;
	}

	void *main_fn = llext_find_sym(&cdc_cur_ext->exp_tab, "main");

	if (main_fn == NULL) {
		LOG_ERR("sketch has no 'main'");
		llext_unload(&cdc_cur_ext);
		cdc_cur_ext = NULL;
		return -ENOENT;
	}

	k_thread_create(&cdc_sketch_thread, cdc_sketch_stack,
			K_THREAD_STACK_SIZEOF(cdc_sketch_stack), cdc_sketch_entry, main_fn,
			NULL, NULL, CDC_SKETCH_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&cdc_sketch_thread, "sketch");
	cdc_sketch_active = true;
	LOG_INF("sketch started");
	return 0;
}

/* Abort the running sketch and free it -- all in-process, no reboot. */
static void cdc_stop_sketch(void) {
	if (cdc_sketch_active) {
		k_thread_abort(&cdc_sketch_thread);
		cdc_sketch_active = false;
	}
	if (cdc_cur_ext != NULL) {
		int rc = llext_unload(&cdc_cur_ext);

		LOG_INF("sketch stopped + unloaded (rc %d)", rc);
		cdc_cur_ext = NULL;
	}
}

static uint32_t cdc_baud(void) {
	uint32_t baud = 0;

	(void)uart_line_ctrl_get(usb_dev, UART_LINE_CTRL_BAUD_RATE, &baud);
	return baud;
}

static void cdc_upload_supervisor(void) {
	if (!device_is_ready(usb_dev)) {
		LOG_ERR("CDC device not ready");
		return;
	}

	/* The loader owns USB bring-up (usb_enable() does usbd_init_device +
	 * usbd_enable; defined above under ZARD_FIRST_SERIAL_IS_SERIALUSB). */
	if (usb_enable(NULL) != 0) {
		LOG_ERR("USB device enable failed");
		return;
	}

	/* Hold CDC RX until a sketch's Serial.begin() takes it over, so a first
	 * upload works on a blank/recovery boot before any sketch runs. */
	cdc_rx_take();

	LOG_INF("PiZZa CDC loader ready (firmware owns SD; in-process swap, no reboot)");

	k_thread_priority_set(k_current_get(), CDC_SUPERVISOR_PRIO);

	/* Auto-run the sketch already on the SD (POR persistence). */
	struct fs_dirent ent;

	if (fs_stat(SKETCH_PATH, &ent) == 0) {
		LOG_INF("auto-loading %s (%u bytes)", SKETCH_PATH, (unsigned int)ent.size);
		cdc_start_sketch();
	} else {
		LOG_INF("no %s yet -- upload one", SKETCH_PATH);
	}

	/* Watch the CDC for the IDE's 1200-bps "enter upload" touch while the
	 * sketch loops in its own thread; on the touch, swap in-process. */
	uint32_t prev = cdc_baud();

	for (;;) {
		uint32_t baud = cdc_baud();

		if (baud == 1200 && prev != 1200) {
			LOG_INF("1200-bps touch -> upload mode (in-process swap)");
			cdc_stop_sketch();
			cdc_rx_take();
			if (cdc_recv_sketch() == 0) {
				cdc_start_sketch();
			}
			prev = cdc_baud();
			continue;
		}
		prev = baud;
		k_msleep(30);
	}
}
#endif /* CONFIG_ARDUINO_SKETCH_LOADER_CDC_UPLOAD */

int main(void) {
#if defined(CONFIG_ARDUINO_SKETCH_LOADER_CDC_UPLOAD)
	cdc_upload_supervisor();
#else
	loader(NULL);
#endif
	return 0;
}
