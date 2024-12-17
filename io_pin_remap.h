#ifndef io_pin_remap_h
#define io_pin_remap_h

#include <stdint.h>

// SDCARD Slot
#define BOARD_HAS_SDMMC
#define SDMMC_D2             12  // SDMMC Data2
#define SDMMC_D3             13  // SDMMC Data3 / SPI CS
#define SDMMC_CMD            15  // SDMMC CMD   / SPI MOSI
#define SDMMC_CLK            14  // SDMMC CLK   / SPI SCK
#define SDMMC_D0              2  // SDMMC Data0 / SPI MISO
#define SDMMC_D1              4  // SDMMC Data1
#define BOARD_MAX_SDMMC_FREQ SDMMC_FREQ_DEFAULT

#define ASYNCWEBSERVER_REGEX

#endif /* io_pin_remap_h */
