/*
 * Copyright (c) 2026 Jonathan Elliot Peace <jep@alphabetiq.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "I2S.h"

#include <string.h>
#include <zephyr/devicetree.h>

/*
 * The whole implementation is conditional on the variant wiring an I2S
 * controller into zephyr,user. On variants without one (e.g. boards
 * whose SoC dtsi has no i2s node) this TU compiles empty and a sketch
 * referencing I2S fails at link with an undefined `I2S`.
 */
#if defined(CONFIG_I2S) && DT_NODE_HAS_PROP(DT_PATH(zephyr_user), i2ses)

/*
 * Blocks queued before START. Matching the driver's ring depth gives
 * the largest jitter cushion; it must never exceed the ring depth, or
 * the pre-START writes would block on a ring that only drains after
 * START.
 */
#ifdef CONFIG_I2S_BCM2835_TX_BLOCK_COUNT
static constexpr uint32_t kTxPreload = CONFIG_I2S_BCM2835_TX_BLOCK_COUNT;
#else
static constexpr uint32_t kTxPreload = 2;
#endif

namespace arduino {

ZephyrI2S::ZephyrI2S(const struct device *dev)
	: dev_(dev), begun_(false), slab_ready_(false), tx_started_(false), tx_queued_(0),
	  tx_fill_(0), rx_started_(false), rx_pos_(0), rx_len_(0) {
}

int ZephyrI2S::begin(int mode, long sampleRate, int bitsPerSample) {
	if (begun_) {
		end();
	}

	/* The PCM block is hardwired to frame-packed Philips I2S, 16-bit
	 * stereo, controller clocking; anything else is not expressible.
	 */
	if (mode != I2S_PHILIPS_MODE || bitsPerSample != 16 || sampleRate <= 0) {
		return 0;
	}

	if (!slab_ready_) {
		if (k_mem_slab_init(&slab_, slab_buf_, kBlockSize, kSlabBlocks) != 0) {
			return 0;
		}
		slab_ready_ = true;
	}

	/* The variant defers the controller's init; this runs the driver
	 * init now, which also muxes the PCM pins (pinctrl "default").
	 */
	if (zephyr::arduino::init_dev_apply_pinctrl(dev_) < 0) {
		return 0;
	}

	memset(&cfg_tx_, 0, sizeof(cfg_tx_));
	cfg_tx_.word_size = 16;
	cfg_tx_.channels = 2;
	cfg_tx_.format = I2S_FMT_DATA_FORMAT_I2S;
	cfg_tx_.options = I2S_OPT_BIT_CLK_CONT;
	cfg_tx_.frame_clk_freq = (uint32_t)sampleRate;
	cfg_tx_.mem_slab = &slab_;
	cfg_tx_.block_size = kBlockSize;
	/* Bounded so a stalled stream surfaces as a dropped block, not a
	 * sketch that hangs in write() forever.
	 */
	cfg_tx_.timeout = 1000;

	cfg_rx_ = cfg_tx_;
	/* Non-blocking reads: available()/read() poll. */
	cfg_rx_.timeout = 0;

	if (i2s_configure(dev_, I2S_DIR_TX, &cfg_tx_) != 0) {
		return 0;
	}
	if (i2s_configure(dev_, I2S_DIR_RX, &cfg_rx_) != 0) {
		return 0;
	}

	tx_started_ = false;
	tx_queued_ = 0;
	tx_fill_ = 0;
	rx_started_ = false;
	rx_pos_ = 0;
	rx_len_ = 0;
	begun_ = true;
	return 1;
}

void ZephyrI2S::end() {
	if (!begun_) {
		return;
	}

	(void)i2s_trigger(dev_, I2S_DIR_TX, I2S_TRIGGER_DROP);
	(void)i2s_trigger(dev_, I2S_DIR_RX, I2S_TRIGGER_DROP);

	/* frame_clk_freq = 0 tears both streams down to NOT_READY and
	 * stops the PCM bit clock.
	 */
	cfg_tx_.frame_clk_freq = 0;
	(void)i2s_configure(dev_, I2S_DIR_BOTH, &cfg_tx_);

	begun_ = false;
	tx_started_ = false;
	tx_queued_ = 0;
	tx_fill_ = 0;
	rx_started_ = false;
	rx_pos_ = 0;
	rx_len_ = 0;
}

int ZephyrI2S::submitBlock(size_t len) {
	int ret = i2s_buf_write(dev_, tx_stage_, len);

	if (ret == -EIO && tx_started_) {
		/* TX underran (the sketch was outpaced by the DMA): the
		 * driver latches ERROR and halts. Recover and re-queue;
		 * playback resumes once the preload refills.
		 */
		(void)i2s_trigger(dev_, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
		tx_started_ = false;
		tx_queued_ = 0;
		ret = i2s_buf_write(dev_, tx_stage_, len);
	}

	tx_fill_ = 0;
	if (ret != 0) {
		return ret;
	}

	tx_queued_++;
	if (!tx_started_ && tx_queued_ >= kTxPreload) {
		if (i2s_trigger(dev_, I2S_DIR_TX, I2S_TRIGGER_START) != 0) {
			return -EIO;
		}
		tx_started_ = true;
	}
	return 0;
}

size_t ZephyrI2S::write(uint8_t data) {
	return write(&data, 1);
}

size_t ZephyrI2S::write(const uint8_t *buffer, size_t size) {
	if (!begun_) {
		return 0;
	}

	size_t done = 0;

	while (done < size) {
		size_t n = MIN(kBlockSize - tx_fill_, size - done);

		memcpy(&tx_stage_[tx_fill_], buffer + done, n);
		tx_fill_ += n;
		done += n;

		if (tx_fill_ == kBlockSize && submitBlock(kBlockSize) != 0) {
			break;
		}
	}
	return done;
}

size_t ZephyrI2S::write(int16_t sample) {
	return write((const uint8_t *)&sample, sizeof(sample)) / sizeof(sample);
}

size_t ZephyrI2S::write(int32_t sample) {
	/* 16-bit hardware: truncate, matching begin()'s only accepted
	 * sample width.
	 */
	return write((int16_t)sample);
}

void ZephyrI2S::flush() {
	if (!begun_) {
		return;
	}
	if (tx_fill_ > 0 && submitBlock(tx_fill_) != 0) {
		return;
	}
	if (!tx_started_ && tx_queued_ > 0 &&
	    i2s_trigger(dev_, I2S_DIR_TX, I2S_TRIGGER_START) == 0) {
		tx_started_ = true;
	}
}

int ZephyrI2S::availableForWrite() {
	if (!begun_) {
		return 0;
	}
	return (int)(kBlockSize - tx_fill_);
}

bool ZephyrI2S::refill() {
	if (rx_pos_ < rx_len_) {
		return true;
	}

	if (!rx_started_) {
		if (i2s_trigger(dev_, I2S_DIR_RX, I2S_TRIGGER_START) != 0) {
			return false;
		}
		rx_started_ = true;
	}

	size_t size = 0;
	int ret = i2s_buf_read(dev_, rx_stage_, &size);

	if (ret == -EIO) {
		/* RX overran: recover; reception restarts on the next
		 * call.
		 */
		(void)i2s_trigger(dev_, I2S_DIR_RX, I2S_TRIGGER_PREPARE);
		rx_started_ = false;
		return false;
	}
	if (ret != 0) {
		return false;
	}

	rx_pos_ = 0;
	rx_len_ = size;
	return true;
}

int ZephyrI2S::available() {
	if (!begun_) {
		return 0;
	}
	if (rx_pos_ >= rx_len_) {
		(void)refill();
	}
	return (int)(rx_len_ - rx_pos_);
}

int ZephyrI2S::read() {
	int16_t sample;

	if (read(&sample, sizeof(sample)) != (int)sizeof(sample)) {
		return -1;
	}
	return sample;
}

int ZephyrI2S::peek() {
	if (!begun_ || !refill() || rx_len_ - rx_pos_ < sizeof(int16_t)) {
		return -1;
	}

	int16_t sample;

	memcpy(&sample, &rx_stage_[rx_pos_], sizeof(sample));
	return sample;
}

int ZephyrI2S::read(void *buffer, size_t size) {
	if (!begun_) {
		return 0;
	}

	uint8_t *out = (uint8_t *)buffer;
	size_t done = 0;

	while (done < size && refill()) {
		size_t n = MIN(rx_len_ - rx_pos_, size - done);

		memcpy(out + done, &rx_stage_[rx_pos_], n);
		rx_pos_ += n;
		done += n;
	}
	return (int)done;
}

} // namespace arduino

arduino::ZephyrI2S I2S(DEVICE_DT_GET(DT_PHANDLE_BY_IDX(DT_PATH(zephyr_user), i2ses, 0)));

#endif /* CONFIG_I2S && zephyr,user i2ses */
