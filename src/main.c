/*
 * Copyright (c) Michael Tharp <gxti@partiallystapled.com>
 *
 * This file is distributed under the terms of the MIT License.
 * See the LICENSE file at the top of this tree, or if it is missing a copy can
 * be found at http://opensource.org/licenses/MIT
 */

#include "common.h"
#include "cmdline.h"
#include "eeprom.h"
#include "gps/parser.h"
#include "gps/ublox.h"
#include "info_table.h"
#include "init.h"
#include "logging.h"
#include "ppscapture.h"
#include "tcpip.h"
#include "version.h"
#include "vtimer.h"
#include "stm32/iwdg.h"
#include "stm32/serial.h"
#include "util/queue.h"

#include <string.h>

#define MAIN_STACK 512
OS_STK main_stack[MAIN_STACK];
OS_TID main_tid;
serial_t *const gps_serial = &Serial4;
static int did_startup, did_watchdog;

const info_entry_t info_table[] = {
	{INFO_APPVER, VERSION},
	{INFO_END, NULL},
};


static void
load_eeprom(void) {
	int16_t rc;
	rc = eeprom_read_cfg();
	if (rc != EERR_OK) {
		memset(&cfg, 0, sizeof(cfg));
		cfg.version = CFG_VERSION;
		cli_puts("ERROR: EEPROM is invalid, run 'save' or 'defaults' to clear\r\n");
	}
}


static void
test_reset(void) {
	uint32_t sr = RCC->CSR;
	RCC->CSR = RCC_CSR_RMVF;
	if (sr & (RCC_CSR_WWDGRSTF | RCC_CSR_IWDGRSTF)) {
		cli_puts("\r\n\r\nERROR: Device was reset by watchdog timer!\r\n");
		did_watchdog = 1;
	}
}


void
log_startup(void) {
	if (did_startup) {
		return;
	}
	log_write(LOG_NOTICE, "kernel", "GPS NTP Server version " VERSION " started");
	if (did_watchdog) {
		log_write(LOG_CRIT, "kernel", "Device was previously reset by watchdog timer!");
	}
	did_startup = 1;
}


static void
main_thread(void *pdata) {
	uint8_t err;
	int16_t val;
	uint32_t flags;
	load_eeprom();
	tcpip_start();
	test_reset();
	cli_banner();
	ublox_configure(gps_serial);
	cl_enabled = 0;
	while (1) {
		flags = CoWaitForMultipleFlags(0
				| 1 << Serial1.rx_q.flag
				| 1 << gps_serial->rx_q.flag
				, OPT_WAIT_ANY, S2ST(1), &err);
		if (err != E_OK && err != E_TIMEOUT) {
			HALT();
		}
		if (flags & (1 << Serial1.rx_q.flag)) {
			while ((val = serial_get(&Serial1, TIMEOUT_NOBLOCK)) >= 0) {
				cli_feed(val);
			}
		}
		if (flags & (1 << gps_serial->rx_q.flag)) {
			while ((val = serial_get(gps_serial, TIMEOUT_NOBLOCK)) >= 0) {
				gps_byte_received(val);
			}
		}
	}
}


void
main(void) {
	setup_clocks(ONBOARD_CLOCK);
	iwdg_start(4, 0xFFF);
	CoInitOS();
	serial_start(&Serial1, 115200);
	serial_start(gps_serial, 57600);
	log_start(&Serial1);
	log_sethostname("gps-ntp");
	cli_set_output(&Serial1);
	cl_enabled = 1;
	ppscapture_start();
	vtimer_start();
	main_tid = CoCreateTask(main_thread, NULL, THREAD_PRIO_MAIN,
			&main_stack[MAIN_STACK-1], MAIN_STACK, "main");
	ASSERT(main_tid != E_CREATE_FAIL);
	CoStartOS();
	while (1) {}
}
