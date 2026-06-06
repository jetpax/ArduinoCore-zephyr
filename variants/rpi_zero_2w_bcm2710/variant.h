/*
 * Copyright (c) 2026 Jonathan Elliot Peace <jep@alphabetiq.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * No board-specific Arduino macros are needed at this stage. The
 * digital-pin map, Serial/Wire/SPI bindings, and SD-FAT mount are
 * all expressed in the matching .overlay; `analogRead`/`analogWrite`
 * are intentionally unsupported -- there is no ADC on this SoC and
 * no PWM driver is bound to the BCM PWM block in Zephyr yet.
 */
