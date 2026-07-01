/*
 * Copyright (c) 2026 Jonathan Elliot Peace <jep@alphabetiq.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * No board-specific Arduino macros are needed at this stage. v1 scope
 * (Blink + USB-CDC Serial) means no Wire/SPI bindings; `analogRead`/
 * `analogWrite` are intentionally unsupported -- there is no ADC on
 * this SoC and no PWM driver is bound to the BCM PWM block in Zephyr
 * yet. Pin-map additions belong here when sketches need them.
 */
