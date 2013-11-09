/*
 * Copyright (c) Michael Tharp <gxti@partiallystapled.com>
 *
 * This file is distributed under the terms of the MIT License.
 * See the LICENSE file at the top of this tree, or if it is missing a copy can
 * be found at http://opensource.org/licenses/MIT
 */

#include "common.h"
#include "bootloader.h"
#include "ff.h"
#include "info_table.h"
#include "init.h"
#include "linker.h"
#include "stm32/mmc.h"
#include "stm32/serial.h"
#include "stm32/spi.h"

#define MAIN_STACK 512
OS_STK main_stack[MAIN_STACK];
OS_TID main_tid;

FATFS MMC_FS;
#define MMC_FIRMWARE_FILENAME "ll.hex"

#define JUMP_TOKEN 0xeefc63d2
uint32_t __attribute__((section(".uninit"))) jump_token;
const uint32_t *user_vtor = _user_start;

const info_entry_t info_table[] = {
	{INFO_BOOTVER, VERSION},
	{INFO_HWVER, (void*)HW_VERSION},
	{INFO_END, NULL},
};


static void
reset_and_jump(void) {
	jump_token = JUMP_TOKEN;
	NVIC_SystemReset();
	while (1) {}
}


static void
jump_application(void) {
	typedef void (*func_t)(void);
	func_t entry = (func_t)user_vtor[1];
	if (entry != (void*)0xFFFFFFFF) {
		__set_MSP(user_vtor[0]);
		entry();
	}
}


static void
try_flash(void) {
	int16_t rc;
	FIL fp;
	UINT nread;
	const char *errmsg;
	static uint8_t buf[512];

	SPI3_Dev.cs_pad = SDIO_CS_PAD;
	SPI3_Dev.cs_pin = SDIO_CS_PNUM;
	spi_start(&SPI3_Dev, 0);
	mmc_start();
	GPIO_OFF(SDIO_PWR);
	CoTickDelay(MS2ST(100));

	serial_puts(&Serial1, "Bootloader version: " VERSION "\r\n");
	rc = mmc_connect();
	if (rc == EERR_OK) {
		serial_puts(&Serial1, "SD connected\r\n");
	} else if (rc == EERR_TIMEOUT) {
		serial_puts(&Serial1, "Timed out waiting for SD\r\n");
		return;
	} else {
		serial_puts(&Serial1, "Failed to connect to SD\r\n");
		return;
	}

	serial_puts(&Serial1, "Mounting SD filesystem\r\n");
	if (f_mount(0, &MMC_FS) != FR_OK) {
		serial_puts(&Serial1, "ERROR: Unable to mount filesystem\r\n");
		return;
	}

	serial_puts(&Serial1, "Opening file " MMC_FIRMWARE_FILENAME "\r\n");
	if (f_open(&fp, MMC_FIRMWARE_FILENAME, FA_READ) != FR_OK) {
		serial_puts(&Serial1, "Error opening file, maybe it does not exist\r\n");
		return;
	}

	serial_puts(&Serial1, "Comparing file to current flash contents\r\n");
	bootloader_start();
	while (bootloader_status == BLS_FLASHING) {
		if (f_read(&fp, buf, sizeof(buf), &nread) != FR_OK) {
			serial_puts(&Serial1, "Error reading file\r\n");
			break;
		}
		if (nread == 0) {
			serial_puts(&Serial1, "Error: premature end of file\r\n");
			break;
		}
		errmsg = bootloader_feed(buf, nread);
		if (errmsg != NULL) {
			serial_puts(&Serial1, "Error flashing firmware: ");
			serial_puts(&Serial1, errmsg);
			serial_puts(&Serial1, "\r\n");
			break;
		}
	}

	if (bootloader_status == BLS_DONE) {
		if (bootloader_was_changed()) {
			serial_puts(&Serial1, "New firmware successfully loaded\r\n");
		} else {
			serial_puts(&Serial1, "Firmware is up-to-date\r\n");
		}
	} else {
		serial_puts(&Serial1, "ERROR: Reset to try again or load last known good firmware\r\n");
		HALT();
	}
}


void
main_thread(void *pdata) {
	try_flash();
	if (user_vtor[1] == 0xFFFFFFFF) {
		serial_puts(&Serial1, "No application loaded, trying to load again in 10 seconds\r\n");
		CoTickDelay(S2ST(10));
		NVIC_SystemReset();
	} else {
		serial_puts(&Serial1, "Booting application\r\n");
		CoTickDelay(MS2ST(250));
		reset_and_jump();
	}
}


void
main(void) {
	if (jump_token == JUMP_TOKEN) {
		jump_token = 0;
		jump_application();
	}
	setup_clocks(ONBOARD_CLOCK);
	CoInitOS();
	serial_start(&Serial1, 115200);
	main_tid = CoCreateTask(main_thread, NULL, THREAD_PRIO_MAIN,
			&main_stack[MAIN_STACK-1], MAIN_STACK, "main");
	ASSERT(main_tid != E_CREATE_FAIL);
	CoStartOS();
	while (1) {}
}
