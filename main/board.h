/* Board-specific hardware configuration
 * Toggle XIAO external antenna switching and pin definitions here.
 *
 * To enable external antenna selection, set ENABLE_XIAO_EXTERNAL_ANT to 1.
 * The code will drive the FM8625H switch pins as follows (per board docs):
 *   - GPIO3 = LOW
 *   - GPIO14 = HIGH
 */
#pragma once

#include "driver/gpio.h"

/* Enable (1) or disable (0) automatic external antenna switching in firmware */
#ifndef ENABLE_XIAO_EXTERNAL_ANT
#define ENABLE_XIAO_EXTERNAL_ANT 1
#endif

/* Pin definitions for antenna switch (XIAO ESP32-C6) */
#ifndef XIAO_ANT_GPIO_SEL0
#define XIAO_ANT_GPIO_SEL0 GPIO_NUM_3
#endif

#ifndef XIAO_ANT_GPIO_SEL1
#define XIAO_ANT_GPIO_SEL1 GPIO_NUM_14
#endif
