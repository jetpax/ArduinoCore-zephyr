/*
 * Copyright (c) 2026 Jonathan Elliot Peace <jep@alphabetiq.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <Arduino.h>
#include <api/Stream.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyrPinctrl.h>

/*
 * Classic Arduino I2S API (ArduinoCore-samd I2S library shape) over the
 * Zephyr i2s driver API. Controller (master) clocking only; the stream
 * format is 16-bit stereo Philips I2S.
 */
typedef enum {
	I2S_PHILIPS_MODE,
	I2S_RIGHT_JUSTIFIED_MODE,
	I2S_LEFT_JUSTIFIED_MODE,
} i2s_mode_t;

namespace arduino {

class ZephyrI2S : public Stream {
public:
	/*
	 * One block = 1024 bytes = 256 stereo 16-bit frames (~5.8 ms at
	 * 44.1 kHz). Must stay within the driver's [FIFO size, max block]
	 * window; i2s_configure() rejects it otherwise.
	 */
	static constexpr size_t kBlockSize = 1024;
	static constexpr uint32_t kSlabBlocks = 4;

	ZephyrI2S(const struct device *dev);

	/* Returns 1 on success, 0 on failure. Only I2S_PHILIPS_MODE with
	 * bitsPerSample = 16 is supported by the hardware.
	 */
	int begin(int mode, long sampleRate, int bitsPerSample);
	void end();

	/* Stream: RX side. read()/peek() return one signed 16-bit sample
	 * (not a byte), -1 when the receive queue is empty; available()
	 * returns buffered bytes. Reception starts on the first call.
	 */
	virtual int available();
	virtual int read();
	virtual int peek();
	int read(void *buffer, size_t size);

	/* Print: TX side. One write() is one 16-bit channel slot; a stereo
	 * frame is two writes (left, right). Samples accumulate into a
	 * block which is queued when full; transmission starts once the
	 * driver ring is preloaded. write() blocks when the ring is full
	 * (backpressure paces the sketch at the sample rate).
	 */
	virtual size_t write(uint8_t data);
	virtual size_t write(const uint8_t *buffer, size_t size);
	using Print::write;
	size_t write(int16_t sample);
	size_t write(int32_t sample);

	/* Queue any partially filled block and start transmission without
	 * waiting for the ring preload.
	 */
	virtual void flush();
	virtual int availableForWrite();

private:
	int submitBlock(size_t len);
	bool refill();

	const struct device *dev_;

	/* k_mem_slab links free blocks through a void * at each block
	 * start: buffer alignment and block size must be multiples of
	 * the pointer size.
	 */
	struct k_mem_slab slab_;
	char __aligned(sizeof(void *)) slab_buf_[kSlabBlocks * kBlockSize];

	struct i2s_config cfg_tx_;
	struct i2s_config cfg_rx_;

	bool begun_;
	bool slab_ready_;

	bool tx_started_;
	uint32_t tx_queued_;
	size_t tx_fill_;
	uint8_t tx_stage_[kBlockSize];

	bool rx_started_;
	size_t rx_pos_;
	size_t rx_len_;
	uint8_t rx_stage_[kBlockSize];
};

} // namespace arduino

extern arduino::ZephyrI2S I2S;
