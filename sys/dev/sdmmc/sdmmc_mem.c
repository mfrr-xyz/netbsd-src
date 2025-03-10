/*	$NetBSD: sdmmc_mem.c,v 1.77 2024/10/24 10:50:31 skrll Exp $	*/
/*	$OpenBSD: sdmmc_mem.c,v 1.10 2009/01/09 10:55:22 jsg Exp $	*/

/*
 * Copyright (c) 2006 Uwe Stuehler <uwe@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*-
 * Copyright (C) 2007, 2008, 2009, 2010 NONAKA Kimihiro <nonaka@netbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Routines for SD/MMC memory cards. */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: sdmmc_mem.c,v 1.77 2024/10/24 10:50:31 skrll Exp $");

#ifdef _KERNEL_OPT
#include "opt_sdmmc.h"
#endif

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/bitops.h>
#include <sys/evcnt.h>

#include <dev/sdmmc/sdmmcchip.h>
#include <dev/sdmmc/sdmmcreg.h>
#include <dev/sdmmc/sdmmcvar.h>

#ifdef SDMMC_DEBUG
#define DPRINTF(s)	do { printf s; } while (/*CONSTCOND*/0)
#else
#define DPRINTF(s)	do {} while (/*CONSTCOND*/0)
#endif

typedef struct { uint32_t _bits[512/32]; } __packed __aligned(4) sdmmc_bitfield512_t;

static int sdmmc_mem_sd_init(struct sdmmc_softc *, struct sdmmc_function *);
static int sdmmc_mem_mmc_init(struct sdmmc_softc *, struct sdmmc_function *);
static int sdmmc_mem_send_cid(struct sdmmc_softc *, sdmmc_response *);
static int sdmmc_mem_send_csd(struct sdmmc_softc *, struct sdmmc_function *,
    sdmmc_response *);
static int sdmmc_mem_send_scr(struct sdmmc_softc *, struct sdmmc_function *,
    uint32_t *scr);
static int sdmmc_mem_decode_scr(struct sdmmc_softc *, struct sdmmc_function *);
static int sdmmc_mem_send_ssr(struct sdmmc_softc *, struct sdmmc_function *,
    sdmmc_bitfield512_t *);
static int sdmmc_mem_decode_ssr(struct sdmmc_softc *, struct sdmmc_function *,
    sdmmc_bitfield512_t *);
static int sdmmc_mem_decode_general_info(struct sdmmc_softc *,
    struct sdmmc_function * ,const uint8_t *);
static int sdmmc_mem_pef_enable_cache(struct sdmmc_softc *,
    struct sdmmc_function *);
static int sdmmc_mem_send_cxd_data(struct sdmmc_softc *, int, void *, size_t);
static int sdmmc_mem_read_extr_single(struct sdmmc_softc *, struct sdmmc_function *,
    uint8_t, uint8_t, uint32_t, uint16_t, void *);
static int sdmmc_mem_write_extr_single(struct sdmmc_softc *, struct sdmmc_function *,
    uint8_t, uint8_t, uint32_t, uint8_t, bool);
static int sdmmc_set_bus_width(struct sdmmc_function *, int);
static int sdmmc_mem_sd_switch(struct sdmmc_function *, int, int, int, sdmmc_bitfield512_t *);
static int sdmmc_mem_mmc_switch(struct sdmmc_function *, uint8_t, uint8_t,
    uint8_t, bool);
static int sdmmc_mem_signal_voltage(struct sdmmc_softc *, int);
static int sdmmc_mem_spi_read_ocr(struct sdmmc_softc *, uint32_t, uint32_t *);
static int sdmmc_mem_single_read_block(struct sdmmc_function *, uint32_t,
    u_char *, size_t);
static int sdmmc_mem_single_write_block(struct sdmmc_function *, uint32_t,
    u_char *, size_t);
static int sdmmc_mem_single_segment_dma_read_block(struct sdmmc_function *,
    uint32_t, u_char *, size_t);
static int sdmmc_mem_single_segment_dma_write_block(struct sdmmc_function *,
    uint32_t, u_char *, size_t);
static int sdmmc_mem_read_block_subr(struct sdmmc_function *, bus_dmamap_t,
    uint32_t, u_char *, size_t);
static int sdmmc_mem_write_block_subr(struct sdmmc_function *, bus_dmamap_t,
    uint32_t, u_char *, size_t);

static const struct {
	const char *name;
	int v;
	int freq;
} switch_group0_functions[] = {
	/* Default/SDR12 */
	{ "Default/SDR12",	 0,			 25000 },

	/* High-Speed/SDR25 */
	{ "High-Speed/SDR25",	SMC_CAPS_SD_HIGHSPEED,	 50000 },

	/* SDR50 */
	{ "SDR50",		SMC_CAPS_UHS_SDR50,	100000 },

	/* SDR104 */
	{ "SDR104",		SMC_CAPS_UHS_SDR104,	208000 },

	/* DDR50 */
	{ "DDR50",		SMC_CAPS_UHS_DDR50,	 50000 },
};

static const int sdmmc_mmc_timings[] = {
	[EXT_CSD_HS_TIMING_LEGACY]	= 26000,
	[EXT_CSD_HS_TIMING_HIGHSPEED]	= 52000,
	[EXT_CSD_HS_TIMING_HS200]	= 200000
};

/*
 * Initialize SD/MMC memory cards and memory in SDIO "combo" cards.
 */
int
sdmmc_mem_enable(struct sdmmc_softc *sc)
{
	uint32_t host_ocr;
	uint32_t card_ocr;
	uint32_t new_ocr;
	uint32_t ocr = 0;
	int error;

	SDMMC_LOCK(sc);

	/* Set host mode to SD "combo" card or SD memory-only. */
	CLR(sc->sc_flags, SMF_UHS_MODE);
	SET(sc->sc_flags, SMF_SD_MODE|SMF_MEM_MODE);

	if (ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE))
		sdmmc_spi_chip_initialize(sc->sc_spi_sct, sc->sc_sch);

	/* Reset memory (*must* do that before CMD55 or CMD1). */
	sdmmc_go_idle_state(sc);

	if (ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE)) {
		/* Check SD Ver.2 */
		error = sdmmc_mem_send_if_cond(sc, 0x1aa, &card_ocr);
		if (error == 0 && card_ocr == 0x1aa)
			SET(ocr, MMC_OCR_HCS);
	}

	/*
	 * Read the SD/MMC memory OCR value by issuing CMD55 followed
	 * by ACMD41 to read the OCR value from memory-only SD cards.
	 * MMC cards will not respond to CMD55 or ACMD41 and this is
	 * how we distinguish them from SD cards.
	 */
mmc_mode:
	error = sdmmc_mem_send_op_cond(sc,
	    ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE) ? ocr : 0, &card_ocr);
	if (error) {
		if (ISSET(sc->sc_flags, SMF_SD_MODE) &&
		    !ISSET(sc->sc_flags, SMF_IO_MODE)) {
			/* Not a SD card, switch to MMC mode. */
			DPRINTF(("%s: switch to MMC mode\n", SDMMCDEVNAME(sc)));
			CLR(sc->sc_flags, SMF_SD_MODE);
			goto mmc_mode;
		}
		if (!ISSET(sc->sc_flags, SMF_SD_MODE)) {
			DPRINTF(("%s: couldn't read memory OCR\n",
			    SDMMCDEVNAME(sc)));
			goto out;
		} else {
			/* Not a "combo" card. */
			CLR(sc->sc_flags, SMF_MEM_MODE);
			error = 0;
			goto out;
		}
	}
	if (ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE)) {
		/* get card OCR */
		error = sdmmc_mem_spi_read_ocr(sc, ocr, &card_ocr);
		if (error) {
			DPRINTF(("%s: couldn't read SPI memory OCR\n",
			    SDMMCDEVNAME(sc)));
			goto out;
		}
	}

	/* Set the lowest voltage supported by the card and host. */
	host_ocr = sdmmc_chip_host_ocr(sc->sc_sct, sc->sc_sch);
	error = sdmmc_set_bus_power(sc, host_ocr, card_ocr);
	if (error) {
		DPRINTF(("%s: couldn't supply voltage requested by card\n",
		    SDMMCDEVNAME(sc)));
		goto out;
	}

	DPRINTF(("%s: host_ocr 0x%08x\n", SDMMCDEVNAME(sc), host_ocr));
	DPRINTF(("%s: card_ocr 0x%08x\n", SDMMCDEVNAME(sc), card_ocr));

	host_ocr &= card_ocr; /* only allow the common voltages */
	if (!ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE)) {
		if (ISSET(sc->sc_flags, SMF_SD_MODE)) {
			/* Tell the card(s) to enter the idle state (again). */
			sdmmc_go_idle_state(sc);
			/* Check SD Ver.2 */
			error = sdmmc_mem_send_if_cond(sc, 0x1aa, &card_ocr);
			if (error == 0 && card_ocr == 0x1aa)
				SET(ocr, MMC_OCR_HCS);

			if (sdmmc_chip_host_ocr(sc->sc_sct, sc->sc_sch) & MMC_OCR_S18A)
				SET(ocr, MMC_OCR_S18A);
		} else {
			SET(ocr, MMC_OCR_ACCESS_MODE_SECTOR);
		}
	}
	host_ocr |= ocr;

	/* Send the new OCR value until all cards are ready. */
	error = sdmmc_mem_send_op_cond(sc, host_ocr, &new_ocr);
	if (error) {
		DPRINTF(("%s: couldn't send memory OCR\n", SDMMCDEVNAME(sc)));
		goto out;
	}

	if (ISSET(sc->sc_flags, SMF_SD_MODE) && ISSET(new_ocr, MMC_OCR_S18A)) {
		/*
		 * Card and host support low voltage mode, begin switch
		 * sequence.
		 */
		struct sdmmc_command cmd;
		memset(&cmd, 0, sizeof(cmd));
		cmd.c_arg = 0;
		cmd.c_flags = SCF_CMD_AC | SCF_RSP_R1;
		cmd.c_opcode = SD_VOLTAGE_SWITCH;
		DPRINTF(("%s: switching card to 1.8V\n", SDMMCDEVNAME(sc)));
		error = sdmmc_mmc_command(sc, &cmd);
		if (error) {
			DPRINTF(("%s: voltage switch command failed\n",
			    SDMMCDEVNAME(sc)));
			goto out;
		}

		error = sdmmc_mem_signal_voltage(sc, SDMMC_SIGNAL_VOLTAGE_180);
		if (error) {
			DPRINTF(("%s: voltage change on host failed\n",
			    SDMMCDEVNAME(sc)));
			goto out;
		}

		SET(sc->sc_flags, SMF_UHS_MODE);
	}

out:
	SDMMC_UNLOCK(sc);

	return error;
}

static int
sdmmc_mem_signal_voltage(struct sdmmc_softc *sc, int signal_voltage)
{
	int error;

	/*
	 * Stop the clock
	 */
	error = sdmmc_chip_bus_clock(sc->sc_sct, sc->sc_sch,
	    SDMMC_SDCLK_OFF, false);
	if (error)
		goto out;

	delay(1000);

	/*
	 * Card switch command was successful, update host controller
	 * signal voltage setting.
	 */
	DPRINTF(("%s: switching host to %s\n", SDMMCDEVNAME(sc),
	    signal_voltage == SDMMC_SIGNAL_VOLTAGE_180 ? "1.8V" : "3.3V"));
	error = sdmmc_chip_signal_voltage(sc->sc_sct,
	    sc->sc_sch, signal_voltage);
	if (error)
		goto out;

	delay(5000);

	/*
	 * Switch to SDR12 timing
	 */
	error = sdmmc_chip_bus_clock(sc->sc_sct, sc->sc_sch, 25000,
	    false);
	if (error)
		goto out;

	delay(1000);

out:
	return error;
}

/*
 * Read the CSD and CID from all cards and assign each card a unique
 * relative card address (RCA).  CMD2 is ignored by SDIO-only cards.
 */
void
sdmmc_mem_scan(struct sdmmc_softc *sc)
{
	sdmmc_response resp;
	struct sdmmc_function *sf;
	uint16_t next_rca;
	int error;
	int retry;

	SDMMC_LOCK(sc);

	/*
	 * CMD2 is a broadcast command understood by SD cards and MMC
	 * cards.  All cards begin to respond to the command, but back
	 * off if another card drives the CMD line to a different level.
	 * Only one card will get its entire response through.  That
	 * card remains silent once it has been assigned a RCA.
	 */
	for (retry = 0; retry < 100; retry++) {
		error = sdmmc_mem_send_cid(sc, &resp);
		if (error) {
			if (!ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE) &&
			    error == ETIMEDOUT) {
				/* No more cards there. */
				break;
			}
			DPRINTF(("%s: couldn't read CID\n", SDMMCDEVNAME(sc)));
			break;
		}

		/* In MMC mode, find the next available RCA. */
		next_rca = 1;
		if (!ISSET(sc->sc_flags, SMF_SD_MODE)) {
			SIMPLEQ_FOREACH(sf, &sc->sf_head, sf_list)
				next_rca++;
		}

		/* Allocate a sdmmc_function structure. */
		sf = sdmmc_function_alloc(sc);
		sf->rca = next_rca;

		/*
		 * Remember the CID returned in the CMD2 response for
		 * later decoding.
		 */
		memcpy(sf->raw_cid, resp, sizeof(sf->raw_cid));

		/*
		 * Silence the card by assigning it a unique RCA, or
		 * querying it for its RCA in the case of SD.
		 */
		if (!ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE)) {
			if (sdmmc_set_relative_addr(sc, sf) != 0) {
				aprint_error_dev(sc->sc_dev,
				    "couldn't set mem RCA\n");
				sdmmc_function_free(sf);
				break;
			}
		}

		/*
		 * If this is a memory-only card, the card responding
		 * first becomes an alias for SDIO function 0.
		 */
		if (sc->sc_fn0 == NULL)
			sc->sc_fn0 = sf;

		SIMPLEQ_INSERT_TAIL(&sc->sf_head, sf, sf_list);

		/* only one function in SPI mode */
		if (ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE))
			break;
	}

	if (!ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE))
		/* Go to Data Transfer Mode, if possible. */
		sdmmc_chip_bus_rod(sc->sc_sct, sc->sc_sch, 0);

	/*
	 * All cards are either inactive or awaiting further commands.
	 * Read the CSDs and decode the raw CID for each card.
	 */
	SIMPLEQ_FOREACH(sf, &sc->sf_head, sf_list) {
		error = sdmmc_mem_send_csd(sc, sf, &resp);
		if (error) {
			SET(sf->flags, SFF_ERROR);
			continue;
		}

		if (sdmmc_decode_csd(sc, resp, sf) != 0 ||
		    sdmmc_decode_cid(sc, sf->raw_cid, sf) != 0) {
			SET(sf->flags, SFF_ERROR);
			continue;
		}

#ifdef SDMMC_DEBUG
		printf("%s: CID: ", SDMMCDEVNAME(sc));
		sdmmc_print_cid(&sf->cid);
#endif
	}

	SDMMC_UNLOCK(sc);
}

int
sdmmc_decode_csd(struct sdmmc_softc *sc, sdmmc_response resp,
    struct sdmmc_function *sf)
{
	/* TRAN_SPEED(2:0): transfer rate exponent */
	static const int speed_exponent[8] = {
		100 *    1,	/* 100 Kbits/s */
		  1 * 1000,	/*   1 Mbits/s */
		 10 * 1000,	/*  10 Mbits/s */
		100 * 1000,	/* 100 Mbits/s */
		         0,
		         0,
		         0,
		         0,
	};
	/* TRAN_SPEED(6:3): time mantissa */
	static const int speed_mantissa[16] = {
		0, 10, 12, 13, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 70, 80,
	};
	struct sdmmc_csd *csd = &sf->csd;
	int e, m;

	if (ISSET(sc->sc_flags, SMF_SD_MODE)) {
		/*
		 * CSD version 1.0 corresponds to SD system
		 * specification version 1.0 - 1.10. (SanDisk, 3.5.3)
		 */
		csd->csdver = SD_CSD_CSDVER(resp);
		switch (csd->csdver) {
		case SD_CSD_CSDVER_2_0:
			DPRINTF(("%s: SD Ver.2.0\n", SDMMCDEVNAME(sc)));
			SET(sf->flags, SFF_SDHC);
			csd->capacity = SD_CSD_V2_CAPACITY(resp);
			csd->read_bl_len = SD_CSD_V2_BL_LEN;
			break;

		case SD_CSD_CSDVER_1_0:
			DPRINTF(("%s: SD Ver.1.0\n", SDMMCDEVNAME(sc)));
			csd->capacity = SD_CSD_CAPACITY(resp);
			csd->read_bl_len = SD_CSD_READ_BL_LEN(resp);
			break;

		default:
			aprint_error_dev(sc->sc_dev,
			    "unknown SD CSD structure version 0x%x\n",
			    csd->csdver);
			return 1;
		}

		csd->mmcver = SD_CSD_MMCVER(resp);
		csd->write_bl_len = SD_CSD_WRITE_BL_LEN(resp);
		csd->r2w_factor = SD_CSD_R2W_FACTOR(resp);
		e = SD_CSD_SPEED_EXP(resp);
		m = SD_CSD_SPEED_MANT(resp);
		csd->tran_speed = speed_exponent[e] * speed_mantissa[m] / 10;
		csd->ccc = SD_CSD_CCC(resp);
	} else {
		csd->csdver = MMC_CSD_CSDVER(resp);
		if (csd->csdver == MMC_CSD_CSDVER_1_0) {
			aprint_error_dev(sc->sc_dev,
			    "unknown MMC CSD structure version 0x%x\n",
			    csd->csdver);
			return 1;
		}

		csd->mmcver = MMC_CSD_MMCVER(resp);
		csd->capacity = MMC_CSD_CAPACITY(resp);
		csd->read_bl_len = MMC_CSD_READ_BL_LEN(resp);
		csd->write_bl_len = MMC_CSD_WRITE_BL_LEN(resp);
		csd->r2w_factor = MMC_CSD_R2W_FACTOR(resp);
		e = MMC_CSD_TRAN_SPEED_EXP(resp);
		m = MMC_CSD_TRAN_SPEED_MANT(resp);
		csd->tran_speed = speed_exponent[e] * speed_mantissa[m] / 10;
	}
	if ((1 << csd->read_bl_len) > SDMMC_SECTOR_SIZE)
		csd->capacity *= (1 << csd->read_bl_len) / SDMMC_SECTOR_SIZE;

#ifdef SDMMC_DUMP_CSD
	sdmmc_print_csd(resp, csd);
#endif

	return 0;
}

int
sdmmc_decode_cid(struct sdmmc_softc *sc, sdmmc_response resp,
    struct sdmmc_function *sf)
{
	struct sdmmc_cid *cid = &sf->cid;

	if (ISSET(sc->sc_flags, SMF_SD_MODE)) {
		cid->mid = SD_CID_MID(resp);
		cid->oid = SD_CID_OID(resp);
		SD_CID_PNM_CPY(resp, cid->pnm);
		cid->rev = SD_CID_REV(resp);
		cid->psn = SD_CID_PSN(resp);
		cid->mdt = SD_CID_MDT(resp);
	} else {
		switch(sf->csd.mmcver) {
		case MMC_CSD_MMCVER_1_0:
		case MMC_CSD_MMCVER_1_4:
			cid->mid = MMC_CID_MID_V1(resp);
			MMC_CID_PNM_V1_CPY(resp, cid->pnm);
			cid->rev = MMC_CID_REV_V1(resp);
			cid->psn = MMC_CID_PSN_V1(resp);
			cid->mdt = MMC_CID_MDT_V1(resp);
			break;
		case MMC_CSD_MMCVER_2_0:
		case MMC_CSD_MMCVER_3_1:
		case MMC_CSD_MMCVER_4_0:
			cid->mid = MMC_CID_MID_V2(resp);
			cid->oid = MMC_CID_OID_V2(resp);
			MMC_CID_PNM_V2_CPY(resp, cid->pnm);
			cid->psn = MMC_CID_PSN_V2(resp);
			break;
		default:
			aprint_error_dev(sc->sc_dev, "unknown MMC version %d\n",
			    sf->csd.mmcver);
			return 1;
		}
	}
	return 0;
}

void
sdmmc_print_cid(struct sdmmc_cid *cid)
{

	printf("mid=0x%02x oid=0x%04x pnm=\"%s\" rev=0x%02x psn=0x%08x"
	    " mdt=%03x\n", cid->mid, cid->oid, cid->pnm, cid->rev, cid->psn,
	    cid->mdt);
}

#ifdef SDMMC_DUMP_CSD
void
sdmmc_print_csd(sdmmc_response resp, struct sdmmc_csd *csd)
{

	printf("csdver = %d\n", csd->csdver);
	printf("mmcver = %d\n", csd->mmcver);
	printf("capacity = 0x%08x\n", csd->capacity);
	printf("read_bl_len = %d\n", csd->read_bl_len);
	printf("write_bl_len = %d\n", csd->write_bl_len);
	printf("r2w_factor = %d\n", csd->r2w_factor);
	printf("tran_speed = %d\n", csd->tran_speed);
	printf("ccc = 0x%x\n", csd->ccc);
}
#endif

/*
 * Initialize a SD/MMC memory card.
 */
int
sdmmc_mem_init(struct sdmmc_softc *sc, struct sdmmc_function *sf)
{
	int error = 0;

	SDMMC_LOCK(sc);

	if (!ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE)) {
		error = sdmmc_select_card(sc, sf);
		if (error)
			goto out;
	}

	error = sdmmc_mem_set_blocklen(sc, sf, SDMMC_SECTOR_SIZE);
	if (error)
		goto out;

	if (ISSET(sc->sc_flags, SMF_SD_MODE))
		error = sdmmc_mem_sd_init(sc, sf);
	else
		error = sdmmc_mem_mmc_init(sc, sf);

	if (error != 0)
		SET(sf->flags, SFF_ERROR);

out:
	SDMMC_UNLOCK(sc);

	return error;
}

/*
 * Get or set the card's memory OCR value (SD or MMC).
 */
int
sdmmc_mem_send_op_cond(struct sdmmc_softc *sc, uint32_t ocr, uint32_t *ocrp)
{
	struct sdmmc_command cmd;
	int error;
	int retry;

	/* Don't lock */

	DPRINTF(("%s: sdmmc_mem_send_op_cond: ocr=%#x\n",
	    SDMMCDEVNAME(sc), ocr));

	/*
	 * If we change the OCR value, retry the command until the OCR
	 * we receive in response has the "CARD BUSY" bit set, meaning
	 * that all cards are ready for identification.
	 */
	for (retry = 0; retry < 100; retry++) {
		memset(&cmd, 0, sizeof(cmd));
		cmd.c_arg = !ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE) ?
		    ocr : (ocr & MMC_OCR_HCS);
		cmd.c_flags = SCF_CMD_BCR | SCF_RSP_R3 | SCF_RSP_SPI_R1
		    | SCF_TOUT_OK;

		if (ISSET(sc->sc_flags, SMF_SD_MODE)) {
			cmd.c_opcode = SD_APP_OP_COND;
			error = sdmmc_app_command(sc, NULL, &cmd);
		} else {
			cmd.c_opcode = MMC_SEND_OP_COND;
			error = sdmmc_mmc_command(sc, &cmd);
		}
		if (error)
			break;

		if (ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE)) {
			if (!ISSET(MMC_SPI_R1(cmd.c_resp), R1_SPI_IDLE))
				break;
		} else {
			if (ISSET(MMC_R3(cmd.c_resp), MMC_OCR_MEM_READY) ||
			    ocr == 0)
				break;
		}

		error = ETIMEDOUT;
		sdmmc_pause(10000, NULL);
	}
	if (ocrp != NULL) {
		if (error == 0 &&
		    !ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE)) {
			*ocrp = MMC_R3(cmd.c_resp);
		} else {
			*ocrp = ocr;
		}
	}
	DPRINTF(("%s: sdmmc_mem_send_op_cond: error=%d, ocr=%#x\n",
	    SDMMCDEVNAME(sc), error, MMC_R3(cmd.c_resp)));
	return error;
}

int
sdmmc_mem_send_if_cond(struct sdmmc_softc *sc, uint32_t ocr, uint32_t *ocrp)
{
	struct sdmmc_command cmd;
	int error;

	/* Don't lock */

	memset(&cmd, 0, sizeof(cmd));
	cmd.c_arg = ocr;
	cmd.c_flags = SCF_CMD_BCR | SCF_RSP_R7 | SCF_RSP_SPI_R7 | SCF_TOUT_OK;
	cmd.c_opcode = SD_SEND_IF_COND;

	error = sdmmc_mmc_command(sc, &cmd);
	if (error == 0 && ocrp != NULL) {
		if (ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE)) {
			*ocrp = MMC_SPI_R7(cmd.c_resp);
		} else {
			*ocrp = MMC_R7(cmd.c_resp);
		}
		DPRINTF(("%s: sdmmc_mem_send_if_cond: error=%d, ocr=%#x\n",
		    SDMMCDEVNAME(sc), error, *ocrp));
	}
	return error;
}

/*
 * Set the read block length appropriately for this card, according to
 * the card CSD register value.
 */
int
sdmmc_mem_set_blocklen(struct sdmmc_softc *sc, struct sdmmc_function *sf,
   int block_len)
{
	struct sdmmc_command cmd;
	int error;

	/* Don't lock */

	memset(&cmd, 0, sizeof(cmd));
	cmd.c_opcode = MMC_SET_BLOCKLEN;
	cmd.c_arg = block_len;
	cmd.c_flags = SCF_CMD_AC | SCF_RSP_R1 | SCF_RSP_SPI_R1;

	error = sdmmc_mmc_command(sc, &cmd);

	DPRINTF(("%s: sdmmc_mem_set_blocklen: read_bl_len=%d sector_size=%d\n",
	    SDMMCDEVNAME(sc), 1 << sf->csd.read_bl_len, block_len));

	return error;
}

/* make 512-bit BE quantity __bitfield()-compatible */
static void
sdmmc_be512_to_bitfield512(sdmmc_bitfield512_t *buf) {
	size_t i;
	uint32_t tmp0, tmp1;
	const size_t bitswords = __arraycount(buf->_bits);
	for (i = 0; i < bitswords/2; i++) {
		tmp0 = buf->_bits[i];
		tmp1 = buf->_bits[bitswords - 1 - i];
		buf->_bits[i] = be32toh(tmp1);
		buf->_bits[bitswords - 1 - i] = be32toh(tmp0);
	}
}

static int
sdmmc_mem_select_transfer_mode(struct sdmmc_softc *sc, int support_func)
{
	if (ISSET(sc->sc_flags, SMF_UHS_MODE)) {
		if (ISSET(sc->sc_caps, SMC_CAPS_UHS_SDR104) &&
		    ISSET(support_func, 1 << SD_ACCESS_MODE_SDR104)) {
			return SD_ACCESS_MODE_SDR104;
		}
		if (ISSET(sc->sc_caps, SMC_CAPS_UHS_DDR50) &&
		    ISSET(support_func, 1 << SD_ACCESS_MODE_DDR50)) {
			return SD_ACCESS_MODE_DDR50;
		}
		if (ISSET(sc->sc_caps, SMC_CAPS_UHS_SDR50) &&
		    ISSET(support_func, 1 << SD_ACCESS_MODE_SDR50)) {
			return SD_ACCESS_MODE_SDR50;
		}
	}
	if (ISSET(sc->sc_caps, SMC_CAPS_SD_HIGHSPEED) &&
	    ISSET(support_func, 1 << SD_ACCESS_MODE_SDR25)) {
		return SD_ACCESS_MODE_SDR25;
	}
	return SD_ACCESS_MODE_SDR12;
}

static int
sdmmc_mem_execute_tuning(struct sdmmc_softc *sc, struct sdmmc_function *sf)
{
	int timing = -1;

	if (ISSET(sc->sc_flags, SMF_SD_MODE)) {
		if (!ISSET(sc->sc_flags, SMF_UHS_MODE))
			return 0;

		switch (sf->csd.tran_speed) {
		case 100000:
			timing = SDMMC_TIMING_UHS_SDR50;
			break;
		case 208000:
			timing = SDMMC_TIMING_UHS_SDR104;
			break;
		default:
			return 0;
		}
	} else {
		switch (sf->csd.tran_speed) {
		case 200000:
			timing = SDMMC_TIMING_MMC_HS200;
			break;
		default:
			return 0;
		}
	}

	DPRINTF(("%s: execute tuning for timing %d\n", SDMMCDEVNAME(sc),
	    timing));

	return sdmmc_chip_execute_tuning(sc->sc_sct, sc->sc_sch, timing);
}

static int
sdmmc_mem_sd_init(struct sdmmc_softc *sc, struct sdmmc_function *sf)
{
	int support_func, best_func, bus_clock, error, i;
	sdmmc_bitfield512_t status;
	bool ddr = false;

	/* change bus clock */
	bus_clock = uimin(sc->sc_busclk, sf->csd.tran_speed);
	error = sdmmc_chip_bus_clock(sc->sc_sct, sc->sc_sch, bus_clock, false);
	if (error) {
		aprint_error_dev(sc->sc_dev, "can't change bus clock\n");
		return error;
	}

	error = sdmmc_mem_send_scr(sc, sf, sf->raw_scr);
	if (error) {
		aprint_error_dev(sc->sc_dev, "SD_SEND_SCR send failed.\n");
		return error;
	}
	error = sdmmc_mem_decode_scr(sc, sf);
	if (error)
		return error;

	if (ISSET(sc->sc_caps, SMC_CAPS_4BIT_MODE) &&
	    ISSET(sf->scr.bus_width, SCR_SD_BUS_WIDTHS_4BIT)) {
		DPRINTF(("%s: change bus width\n", SDMMCDEVNAME(sc)));
		error = sdmmc_set_bus_width(sf, 4);
		if (error) {
			aprint_error_dev(sc->sc_dev,
			    "can't change bus width (%d bit)\n", 4);
			return error;
		}
		sf->width = 4;
	}

	best_func = 0;
	if (sf->scr.sd_spec >= SCR_SD_SPEC_VER_1_10 &&
	    ISSET(sf->csd.ccc, SD_CSD_CCC_SWITCH)) {
		DPRINTF(("%s: switch func mode 0\n", SDMMCDEVNAME(sc)));
		error = sdmmc_mem_sd_switch(sf, 0, 1, 0, &status);
		if (error) {
			if (error == ENOTSUP) {
				/* Not supported by controller */
				goto skipswitchfuncs;
			} else {
				aprint_error_dev(sc->sc_dev,
				    "switch func mode 0 failed\n");
				return error;
			}
		}

		support_func = SFUNC_STATUS_GROUP(&status, 1);

		if (!ISSET(sc->sc_flags, SMF_UHS_MODE) && support_func & 0x1c) {
			/* XXX UHS-I card started in 1.8V mode, switch now */
			error = sdmmc_mem_signal_voltage(sc,
			    SDMMC_SIGNAL_VOLTAGE_180);
			if (error) {
				aprint_error_dev(sc->sc_dev,
				    "failed to recover UHS card\n");
				return error;
			}
			SET(sc->sc_flags, SMF_UHS_MODE);
		}

		for (i = 0; i < __arraycount(switch_group0_functions); i++) {
			if (!(support_func & (1 << i)))
				continue;
			DPRINTF(("%s: card supports mode %s\n",
			    SDMMCDEVNAME(sc),
			    switch_group0_functions[i].name));
		}

		best_func = sdmmc_mem_select_transfer_mode(sc, support_func);

		DPRINTF(("%s: using mode %s\n", SDMMCDEVNAME(sc),
		    switch_group0_functions[best_func].name));

		if (best_func != 0) {
			DPRINTF(("%s: switch func mode 1(func=%d)\n",
			    SDMMCDEVNAME(sc), best_func));
			error =
			    sdmmc_mem_sd_switch(sf, 1, 1, best_func, &status);
			if (error) {
				aprint_error_dev(sc->sc_dev,
				    "switch func mode 1 failed:"
				    " group 1 function %d(0x%2x)\n",
				    best_func, support_func);
				return error;
			}
			sf->csd.tran_speed =
			    switch_group0_functions[best_func].freq;

			if (best_func == SD_ACCESS_MODE_DDR50)
				ddr = true;

			/* Wait 400KHz x 8 clock (2.5us * 8 + slop) */
			delay(25);
		}
	}
skipswitchfuncs:

	/* update bus clock */
	if (sc->sc_busclk > sf->csd.tran_speed)
		sc->sc_busclk = sf->csd.tran_speed;
	if (sc->sc_busclk != bus_clock || sc->sc_busddr != ddr) {
		/* change bus clock */
		error = sdmmc_chip_bus_clock(sc->sc_sct, sc->sc_sch, sc->sc_busclk,
		    ddr);
		if (error) {
			aprint_error_dev(sc->sc_dev, "can't change bus clock\n");
			return error;
		}

		sc->sc_transfer_mode = switch_group0_functions[best_func].name;
		sc->sc_busddr = ddr;
	}

	/* get card status */
	error = sdmmc_mem_send_ssr(sc, sf, &status);
	if (error) {
		aprint_error_dev(sc->sc_dev, "can't get SD status: %d\n",
		    error);
		return error;
	}
	sdmmc_mem_decode_ssr(sc, sf, &status);

	/* execute tuning (UHS) */
	error = sdmmc_mem_execute_tuning(sc, sf);
	if (error) {
		aprint_error_dev(sc->sc_dev, "can't execute SD tuning\n");
		return error;
	}

	/* detect extended functions */
	if (!ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE) && sf->scr.support_cmd48) {
		uint8_t ginfo[512];
		error = sdmmc_mem_read_extr_single(sc, sf, SD_EXTR_MIO_MEM, 0, 0,
		    sizeof(ginfo), ginfo);
		if (error == 0) {
			sdmmc_mem_decode_general_info(sc, sf, ginfo);
		}
	}

	/* enable card cache if supported */
	if (sf->ssr.cache && sf->ext_sd.pef.valid) {
		error = sdmmc_mem_pef_enable_cache(sc, sf);
		if (error != 0) {
			aprint_error_dev(sc->sc_dev,
			    "can't enable cache: %d", error);
		} else {
			SET(sf->flags, SFF_CACHE_ENABLED);
		}
	}

	return 0;
}

static int
sdmmc_mem_mmc_init(struct sdmmc_softc *sc, struct sdmmc_function *sf)
{
	int width, value, hs_timing, bus_clock, error;
	uint8_t ext_csd[512];
	uint32_t sectors = 0;
	bool ddr = false;

	sc->sc_transfer_mode = NULL;

	/* change bus clock */
	bus_clock = uimin(sc->sc_busclk, sf->csd.tran_speed);
	error = sdmmc_chip_bus_clock(sc->sc_sct, sc->sc_sch, bus_clock, false);
	if (error) {
		aprint_error_dev(sc->sc_dev, "can't change bus clock\n");
		return error;
	}

	if (sf->csd.mmcver >= MMC_CSD_MMCVER_4_0) {
		error = sdmmc_mem_send_cxd_data(sc,
		    MMC_SEND_EXT_CSD, ext_csd, sizeof(ext_csd));
		if (error) {
			aprint_error_dev(sc->sc_dev,
			    "can't read EXT_CSD (error=%d)\n", error);
			return error;
		}
		if ((sf->csd.csdver == MMC_CSD_CSDVER_EXT_CSD) &&
		    (ext_csd[EXT_CSD_STRUCTURE] > EXT_CSD_STRUCTURE_VER_1_2)) {
			aprint_error_dev(sc->sc_dev,
			    "unrecognised future version (%d)\n",
				ext_csd[EXT_CSD_STRUCTURE]);
			return ENOTSUP;
		}
		sf->ext_csd.rev = ext_csd[EXT_CSD_REV];

		if (ISSET(sc->sc_caps, SMC_CAPS_MMC_HS200) &&
		    ext_csd[EXT_CSD_CARD_TYPE] & EXT_CSD_CARD_TYPE_F_HS200_1_8V) {
			hs_timing = EXT_CSD_HS_TIMING_HS200;
		} else if (ISSET(sc->sc_caps, SMC_CAPS_MMC_DDR52) &&
		    ext_csd[EXT_CSD_CARD_TYPE] & EXT_CSD_CARD_TYPE_F_DDR52_1_8V) {
			hs_timing = EXT_CSD_HS_TIMING_HIGHSPEED;
			ddr = true;
		} else if (ext_csd[EXT_CSD_CARD_TYPE] & EXT_CSD_CARD_TYPE_F_52M) {
			hs_timing = EXT_CSD_HS_TIMING_HIGHSPEED;
		} else if (ext_csd[EXT_CSD_CARD_TYPE] & EXT_CSD_CARD_TYPE_F_26M) {
			hs_timing = EXT_CSD_HS_TIMING_LEGACY;
		} else {
			aprint_error_dev(sc->sc_dev,
			    "unknown CARD_TYPE: 0x%x\n",
			    ext_csd[EXT_CSD_CARD_TYPE]);
			return ENOTSUP;
		}

		if (ISSET(sc->sc_caps, SMC_CAPS_8BIT_MODE)) {
			width = 8;
			value = EXT_CSD_BUS_WIDTH_8;
		} else if (ISSET(sc->sc_caps, SMC_CAPS_4BIT_MODE)) {
			width = 4;
			value = EXT_CSD_BUS_WIDTH_4;
		} else {
			width = 1;
			value = EXT_CSD_BUS_WIDTH_1;
		}

		if (width != 1) {
			error = sdmmc_mem_mmc_switch(sf, EXT_CSD_CMD_SET_NORMAL,
			    EXT_CSD_BUS_WIDTH, value, false);
			if (error == 0)
				error = sdmmc_chip_bus_width(sc->sc_sct,
				    sc->sc_sch, width);
			else {
				DPRINTF(("%s: can't change bus width"
				    " (%d bit)\n", SDMMCDEVNAME(sc), width));
				return error;
			}

			/* XXXX: need bus test? (using by CMD14 & CMD19) */
			delay(10000);
		}
		sf->width = width;

		if (hs_timing == EXT_CSD_HS_TIMING_HIGHSPEED &&
		    !ISSET(sc->sc_caps, SMC_CAPS_MMC_HIGHSPEED)) {
			hs_timing = EXT_CSD_HS_TIMING_LEGACY;
		}

		const int target_timing = hs_timing;
		if (hs_timing != EXT_CSD_HS_TIMING_LEGACY) {
			while (hs_timing >= EXT_CSD_HS_TIMING_LEGACY) {
				error = sdmmc_mem_mmc_switch(sf, EXT_CSD_CMD_SET_NORMAL,
				    EXT_CSD_HS_TIMING, hs_timing, false);
				if (error == 0 || hs_timing == EXT_CSD_HS_TIMING_LEGACY)
					break;
				hs_timing--;
			}
		}
		if (hs_timing != target_timing) {
			aprint_debug_dev(sc->sc_dev,
			    "card failed to switch to timing mode %d, using %d\n",
			    target_timing, hs_timing);
		}

		KASSERT(hs_timing < __arraycount(sdmmc_mmc_timings));
		sf->csd.tran_speed = sdmmc_mmc_timings[hs_timing];

		if (sc->sc_busclk > sf->csd.tran_speed)
			sc->sc_busclk = sf->csd.tran_speed;
		if (sc->sc_busclk != bus_clock) {
			error = sdmmc_chip_bus_clock(sc->sc_sct, sc->sc_sch,
			    sc->sc_busclk, false);
			if (error) {
				aprint_error_dev(sc->sc_dev,
				    "can't change bus clock\n");
				return error;
			}
		}

		if (hs_timing != EXT_CSD_HS_TIMING_LEGACY) {
			error = sdmmc_mem_send_cxd_data(sc,
			    MMC_SEND_EXT_CSD, ext_csd, sizeof(ext_csd));
			if (error) {
				aprint_error_dev(sc->sc_dev,
				    "can't re-read EXT_CSD\n");
				return error;
			}
			if (ext_csd[EXT_CSD_HS_TIMING] != hs_timing) {
				aprint_error_dev(sc->sc_dev,
				    "HS_TIMING set failed\n");
				return EINVAL;
			}
		}

		/*
		 * HS_TIMING must be set to 0x1 before setting BUS_WIDTH
		 * for dual data rate operation
		 */
		if (ddr &&
		    hs_timing == EXT_CSD_HS_TIMING_HIGHSPEED &&
		    width > 1) {
			error = sdmmc_mem_mmc_switch(sf,
			    EXT_CSD_CMD_SET_NORMAL, EXT_CSD_BUS_WIDTH,
			    (width == 8) ? EXT_CSD_BUS_WIDTH_8_DDR :
			      EXT_CSD_BUS_WIDTH_4_DDR, false);
			if (error) {
				DPRINTF(("%s: can't switch to DDR"
				    " (%d bit)\n", SDMMCDEVNAME(sc), width));
				return error;
			}

			delay(10000);

			error = sdmmc_mem_signal_voltage(sc,
			    SDMMC_SIGNAL_VOLTAGE_180);
			if (error) {
				aprint_error_dev(sc->sc_dev,
				    "can't switch signaling voltage\n");
				return error;
			}

			error = sdmmc_chip_bus_clock(sc->sc_sct, sc->sc_sch,
			    sc->sc_busclk, ddr);
			if (error) {
				aprint_error_dev(sc->sc_dev,
				    "can't change bus clock\n");
				return error;
			}

			delay(10000);

			sc->sc_transfer_mode = "DDR52";
			sc->sc_busddr = ddr;
		}

		sectors = ext_csd[EXT_CSD_SEC_COUNT + 0] << 0 |
		    ext_csd[EXT_CSD_SEC_COUNT + 1] << 8  |
		    ext_csd[EXT_CSD_SEC_COUNT + 2] << 16 |
		    ext_csd[EXT_CSD_SEC_COUNT + 3] << 24;
		if (sectors > (2u * 1024 * 1024 * 1024) / 512) {
			SET(sf->flags, SFF_SDHC);
			sf->csd.capacity = sectors;
		}

		if (hs_timing == EXT_CSD_HS_TIMING_HS200) {
			sc->sc_transfer_mode = "HS200";

			/* execute tuning (HS200) */
			error = sdmmc_mem_execute_tuning(sc, sf);
			if (error) {
				aprint_error_dev(sc->sc_dev,
				    "can't execute MMC tuning\n");
				return error;
			}
		}

		if (sf->ext_csd.rev >= 5) {
			sf->ext_csd.rst_n_function =
			    ext_csd[EXT_CSD_RST_N_FUNCTION];
		}

		if (sf->ext_csd.rev >= 6) {
			sf->ext_csd.cache_size =
			    le32dec(&ext_csd[EXT_CSD_CACHE_SIZE]) * 1024;
		}
		if (sf->ext_csd.cache_size > 0) {
			/* eMMC cache present, enable it */
			error = sdmmc_mem_mmc_switch(sf,
			    EXT_CSD_CMD_SET_NORMAL, EXT_CSD_CACHE_CTRL,
			    EXT_CSD_CACHE_CTRL_CACHE_EN, false);
			if (error) {
				aprint_error_dev(sc->sc_dev,
				    "can't enable cache: %d\n", error);
			} else {
				SET(sf->flags, SFF_CACHE_ENABLED);
			}
		}
	} else {
		if (sc->sc_busclk > sf->csd.tran_speed)
			sc->sc_busclk = sf->csd.tran_speed;
		if (sc->sc_busclk != bus_clock) {
			error = sdmmc_chip_bus_clock(sc->sc_sct, sc->sc_sch,
			    sc->sc_busclk, false);
			if (error) {
				aprint_error_dev(sc->sc_dev,
				    "can't change bus clock\n");
				return error;
			}
		}
	}

	return 0;
}

static int
sdmmc_mem_send_cid(struct sdmmc_softc *sc, sdmmc_response *resp)
{
	struct sdmmc_command cmd;
	int error;

	if (!ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE)) {
		memset(&cmd, 0, sizeof cmd);
		cmd.c_opcode = MMC_ALL_SEND_CID;
		cmd.c_flags = SCF_CMD_BCR | SCF_RSP_R2 | SCF_TOUT_OK;

		error = sdmmc_mmc_command(sc, &cmd);
	} else {
		error = sdmmc_mem_send_cxd_data(sc, MMC_SEND_CID, &cmd.c_resp,
		    sizeof(cmd.c_resp));
	}

#ifdef SDMMC_DEBUG
	if (error == 0)
		sdmmc_dump_data("CID", cmd.c_resp, sizeof(cmd.c_resp));
#endif
	if (error == 0 && resp != NULL)
		memcpy(resp, &cmd.c_resp, sizeof(*resp));
	return error;
}

static int
sdmmc_mem_send_csd(struct sdmmc_softc *sc, struct sdmmc_function *sf,
    sdmmc_response *resp)
{
	struct sdmmc_command cmd;
	int error;

	if (!ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE)) {
		memset(&cmd, 0, sizeof cmd);
		cmd.c_opcode = MMC_SEND_CSD;
		cmd.c_arg = MMC_ARG_RCA(sf->rca);
		cmd.c_flags = SCF_CMD_AC | SCF_RSP_R2;

		error = sdmmc_mmc_command(sc, &cmd);
	} else {
		error = sdmmc_mem_send_cxd_data(sc, MMC_SEND_CSD, &cmd.c_resp,
		    sizeof(cmd.c_resp));
	}

#ifdef SDMMC_DEBUG
	if (error == 0)
		sdmmc_dump_data("CSD", cmd.c_resp, sizeof(cmd.c_resp));
#endif
	if (error == 0 && resp != NULL)
		memcpy(resp, &cmd.c_resp, sizeof(*resp));
	return error;
}

static int
sdmmc_mem_send_scr(struct sdmmc_softc *sc, struct sdmmc_function *sf,
    uint32_t *scr)
{
	struct sdmmc_command cmd;
	bus_dma_segment_t ds[1];
	void *ptr = NULL;
	int datalen = 8;
	int rseg;
	int error = 0;

	/* Don't lock */

	if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
		error = bus_dmamem_alloc(sc->sc_dmat, datalen, PAGE_SIZE, 0,
		    ds, 1, &rseg, BUS_DMA_NOWAIT);
		if (error)
			goto out;
		error = bus_dmamem_map(sc->sc_dmat, ds, 1, datalen, &ptr,
		    BUS_DMA_NOWAIT);
		if (error)
			goto dmamem_free;
		error = bus_dmamap_load(sc->sc_dmat, sc->sc_dmap, ptr, datalen,
		    NULL, BUS_DMA_NOWAIT|BUS_DMA_STREAMING|BUS_DMA_READ);
		if (error)
			goto dmamem_unmap;

		bus_dmamap_sync(sc->sc_dmat, sc->sc_dmap, 0, datalen,
		    BUS_DMASYNC_PREREAD);
	} else {
		ptr = malloc(datalen, M_DEVBUF, M_NOWAIT | M_ZERO);
		if (ptr == NULL)
			goto out;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.c_data = ptr;
	cmd.c_datalen = datalen;
	cmd.c_blklen = datalen;
	cmd.c_arg = 0;
	cmd.c_flags = SCF_CMD_ADTC | SCF_CMD_READ | SCF_RSP_R1 | SCF_RSP_SPI_R1;
	cmd.c_opcode = SD_APP_SEND_SCR;
	if (ISSET(sc->sc_caps, SMC_CAPS_DMA))
		cmd.c_dmamap = sc->sc_dmap;

	error = sdmmc_app_command(sc, sf, &cmd);
	if (error == 0) {
		if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
			bus_dmamap_sync(sc->sc_dmat, sc->sc_dmap, 0, datalen,
			    BUS_DMASYNC_POSTREAD);
		}
		memcpy(scr, ptr, datalen);
	}

out:
	if (ptr != NULL) {
		if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
			bus_dmamap_unload(sc->sc_dmat, sc->sc_dmap);
dmamem_unmap:
			bus_dmamem_unmap(sc->sc_dmat, ptr, datalen);
dmamem_free:
			bus_dmamem_free(sc->sc_dmat, ds, rseg);
		} else {
			free(ptr, M_DEVBUF);
		}
	}
	DPRINTF(("%s: sdmem_mem_send_scr: error = %d\n", SDMMCDEVNAME(sc),
	    error));

#ifdef SDMMC_DEBUG
	if (error == 0)
		sdmmc_dump_data("SCR", scr, datalen);
#endif
	return error;
}

static int
sdmmc_mem_decode_scr(struct sdmmc_softc *sc, struct sdmmc_function *sf)
{
	sdmmc_response resp;
	int ver;

	memset(resp, 0, sizeof(resp));
	/*
	 * Change the raw-scr received from the DMA stream to resp.
	 */
	resp[0] = be32toh(sf->raw_scr[1]) >> 8;		// LSW
	resp[1] = be32toh(sf->raw_scr[0]);		// MSW
	resp[0] |= (resp[1] & 0xff) << 24;
	resp[1] >>= 8;

	ver = SCR_STRUCTURE(resp);
	sf->scr.sd_spec = SCR_SD_SPEC(resp);
	if (sf->scr.sd_spec == 2) {
		sf->scr.sd_spec3 = SCR_SD_SPEC3(resp);
		if (sf->scr.sd_spec3) {
			sf->scr.sd_spec4 = SCR_SD_SPEC4(resp);
		}
	}
	sf->scr.bus_width = SCR_SD_BUS_WIDTHS(resp);
	if (sf->scr.sd_spec4) {
		sf->scr.support_cmd48 = SCR_CMD_SUPPORT_CMD48(resp);
	}

	DPRINTF(("%s: sdmmc_mem_decode_scr: %08x%08x ver=%d, spec=%d,%d,%d, bus width=%d\n",
	    SDMMCDEVNAME(sc), resp[1], resp[0],
	    ver, sf->scr.sd_spec, sf->scr.sd_spec3, sf->scr.sd_spec4, sf->scr.bus_width));

	if (ver != 0 && ver != 1) {
		DPRINTF(("%s: unknown structure version: %d\n",
		    SDMMCDEVNAME(sc), ver));
		return EINVAL;
	}
	return 0;
}

static int
sdmmc_mem_send_ssr(struct sdmmc_softc *sc, struct sdmmc_function *sf,
    sdmmc_bitfield512_t *ssr)
{
	struct sdmmc_command cmd;
	bus_dma_segment_t ds[1];
	void *ptr = NULL;
	int datalen = 64;
	int rseg;
	int error = 0;

	/* Don't lock */

	if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
		error = bus_dmamem_alloc(sc->sc_dmat, datalen, PAGE_SIZE, 0,
		    ds, 1, &rseg, BUS_DMA_NOWAIT);
		if (error)
			goto out;
		error = bus_dmamem_map(sc->sc_dmat, ds, 1, datalen, &ptr,
		    BUS_DMA_NOWAIT);
		if (error)
			goto dmamem_free;
		error = bus_dmamap_load(sc->sc_dmat, sc->sc_dmap, ptr, datalen,
		    NULL, BUS_DMA_NOWAIT|BUS_DMA_STREAMING|BUS_DMA_READ);
		if (error)
			goto dmamem_unmap;

		bus_dmamap_sync(sc->sc_dmat, sc->sc_dmap, 0, datalen,
		    BUS_DMASYNC_PREREAD);
	} else {
		ptr = malloc(datalen, M_DEVBUF, M_NOWAIT | M_ZERO);
		if (ptr == NULL)
			goto out;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.c_data = ptr;
	cmd.c_datalen = datalen;
	cmd.c_blklen = datalen;
	cmd.c_arg = 0;
	cmd.c_flags = SCF_CMD_ADTC | SCF_CMD_READ | SCF_RSP_R1 | SCF_RSP_SPI_R1;
	cmd.c_opcode = SD_APP_SD_STATUS;
	if (ISSET(sc->sc_caps, SMC_CAPS_DMA))
		cmd.c_dmamap = sc->sc_dmap;

	error = sdmmc_app_command(sc, sf, &cmd);
	if (error == 0) {
		if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
			bus_dmamap_sync(sc->sc_dmat, sc->sc_dmap, 0, datalen,
			    BUS_DMASYNC_POSTREAD);
		}
		memcpy(ssr, ptr, datalen);
	}

out:
	if (ptr != NULL) {
		if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
			bus_dmamap_unload(sc->sc_dmat, sc->sc_dmap);
dmamem_unmap:
			bus_dmamem_unmap(sc->sc_dmat, ptr, datalen);
dmamem_free:
			bus_dmamem_free(sc->sc_dmat, ds, rseg);
		} else {
			free(ptr, M_DEVBUF);
		}
	}
	DPRINTF(("%s: sdmem_mem_send_ssr: error = %d\n", SDMMCDEVNAME(sc),
	    error));

	if (error == 0)
		sdmmc_be512_to_bitfield512(ssr);

#ifdef SDMMC_DEBUG
	if (error == 0)
		sdmmc_dump_data("SSR", ssr, datalen);
#endif
	return error;
}

static int
sdmmc_mem_decode_ssr(struct sdmmc_softc *sc, struct sdmmc_function *sf,
    sdmmc_bitfield512_t *ssr_bitfield)
{
	uint32_t *ssr = (uint32_t *)ssr_bitfield;
	int speed_class_val, bus_width_val;

	const int bus_width = SSR_DAT_BUS_WIDTH(ssr);
	const int speed_class = SSR_SPEED_CLASS(ssr);
	const int uhs_speed_grade = SSR_UHS_SPEED_GRADE(ssr);
	const int video_speed_class = SSR_VIDEO_SPEED_CLASS(ssr);
	const int app_perf_class = SSR_APP_PERF_CLASS(ssr);
	const uint64_t perf_enhance = SSR_PERFORMANCE_ENHANCE(ssr);

	switch (speed_class) {
	case SSR_SPEED_CLASS_0:	speed_class_val = 0; break;
	case SSR_SPEED_CLASS_2: speed_class_val = 2; break;
	case SSR_SPEED_CLASS_4: speed_class_val = 4; break;
	case SSR_SPEED_CLASS_6: speed_class_val = 6; break;
	case SSR_SPEED_CLASS_10: speed_class_val = 10; break;
	default: speed_class_val = -1; break;
	}

	switch (bus_width) {
	case SSR_DAT_BUS_WIDTH_1: bus_width_val = 1; break;
	case SSR_DAT_BUS_WIDTH_4: bus_width_val = 4; break;
	default: bus_width_val = -1;
	}

	if (ISSET(perf_enhance, SSR_PERFORMANCE_ENHANCE_CACHE)) {
		sf->ssr.cache = true;
	}

	/*
	 * Log card status
	 */
	device_printf(sc->sc_dev, "SD card status:");
	if (bus_width_val != -1)
		printf(" %d-bit", bus_width_val);
	else
		printf(" unknown bus width");
	if (speed_class_val != -1)
		printf(", C%d", speed_class_val);
	if (uhs_speed_grade)
		printf(", U%d", uhs_speed_grade);
	if (video_speed_class)
		printf(", V%d", video_speed_class);
	if (app_perf_class)
		printf(", A%d", app_perf_class);
	if (ISSET(perf_enhance, SSR_PERFORMANCE_ENHANCE_CACHE))
		printf(", Cache");
	if (ISSET(perf_enhance, SSR_PERFORMANCE_ENHANCE_HOST_MAINT|
				SSR_PERFORMANCE_ENHANCE_CARD_MAINT)) {
		printf(", %s self-maintenance",
		       perf_enhance == SSR_PERFORMANCE_ENHANCE_HOST_MAINT ? "Host" :
		       perf_enhance == SSR_PERFORMANCE_ENHANCE_CARD_MAINT ? "Card" :
		       "Host/Card");
	}
	printf("\n");

	return 0;
}

static int
sdmmc_mem_decode_general_info(struct sdmmc_softc *sc, struct sdmmc_function *sf,
    const uint8_t *ginfo)
{
	uint16_t len = SD_GENERAL_INFO_HDR_LENGTH(ginfo);
	unsigned num_ext = SD_GENERAL_INFO_HDR_NUM_EXT(ginfo);
	unsigned index = SD_GENERAL_INFO_EXT_FIRST;
	unsigned ext;

	DPRINTF(("%s: sdmmc_mem_decode_general_info: rev=%u, len=%u, num_ext=%u\n",
		SDMMCDEVNAME(sc), SD_GENERAL_INFO_HDR_REVISION(ginfo),
		len, num_ext));

	/*
	 * General Information Length can span more than one page, but for
	 * now just parse the first one.
	 */
	len = uimin(SDMMC_SECTOR_SIZE, len);

	for (ext = 0; ext < num_ext && index < len && index != 0; ext++) {
		uint16_t sfc = SD_EXTENSION_INFO_SFC(ginfo, index);
		unsigned num_reg = SD_EXTENSION_INFO_NUM_REG(ginfo, index);
		uint32_t reg;

		if (num_reg == 0) {
			goto next_ext;
		}
		reg = SD_EXTENSION_INFO_REG(ginfo, index, 0);

		DPRINTF(("%s: sdmmc_mem_decode_general_info: sfc=0x%04x, reg=0x%08x\n",
			SDMMCDEVNAME(sc), sfc, reg));

		switch (sfc) {
		case SD_SFC_PEF:
			sf->ext_sd.pef.valid = true;
			sf->ext_sd.pef.fno =
			    SD_EXTENSION_INFO_REG_FNO(reg);
			sf->ext_sd.pef.start_addr = 
			    SD_EXTENSION_INFO_REG_START_ADDR(reg);
			break;
		}

next_ext:
		index = SD_EXTENSION_INFO_NEXT(ginfo, index);
	}

	return 0;
}

static int
sdmmc_mem_pef_enable_cache(struct sdmmc_softc *sc,
    struct sdmmc_function *sf)
{
	uint8_t data[512];
	int error;

	error = sdmmc_mem_read_extr_single(sc, sf, SD_EXTR_MIO_MEM,
	    sf->ext_sd.pef.fno, sf->ext_sd.pef.start_addr,
	    sizeof(data), data);
	if (error != 0) {
		return error;
	}

	if (SD_PEF_CACHE_ENABLE(data)) {
		/* Cache is already enabled. */
		return 0;
	}

	error = sdmmc_mem_write_extr_single(sc, sf, SD_EXTR_MIO_MEM,
	    sf->ext_sd.pef.fno,
	    sf->ext_sd.pef.start_addr + SD_PEF_CACHE_ENABLE_OFFSET, 1,
	    false);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "setting cache enable failed: %d\n", error);
		return error;
	}

	device_printf(sc->sc_dev, "cache enabled\n");

	return 0;
}

static int
sdmmc_mem_send_cxd_data(struct sdmmc_softc *sc, int opcode, void *data,
    size_t datalen)
{
	struct sdmmc_command cmd;
	bus_dma_segment_t ds[1];
	void *ptr = NULL;
	int rseg;
	int error = 0;

	if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
		error = bus_dmamem_alloc(sc->sc_dmat, datalen, PAGE_SIZE, 0, ds,
		    1, &rseg, BUS_DMA_NOWAIT);
		if (error)
			goto out;
		error = bus_dmamem_map(sc->sc_dmat, ds, 1, datalen, &ptr,
		    BUS_DMA_NOWAIT);
		if (error)
			goto dmamem_free;
		error = bus_dmamap_load(sc->sc_dmat, sc->sc_dmap, ptr, datalen,
		    NULL, BUS_DMA_NOWAIT|BUS_DMA_STREAMING|BUS_DMA_READ);
		if (error)
			goto dmamem_unmap;

		bus_dmamap_sync(sc->sc_dmat, sc->sc_dmap, 0, datalen,
		    BUS_DMASYNC_PREREAD);
	} else {
		ptr = malloc(datalen, M_DEVBUF, M_NOWAIT | M_ZERO);
		if (ptr == NULL)
			goto out;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.c_data = ptr;
	cmd.c_datalen = datalen;
	cmd.c_blklen = datalen;
	cmd.c_opcode = opcode;
	cmd.c_arg = 0;
	cmd.c_flags = SCF_CMD_ADTC | SCF_CMD_READ | SCF_RSP_SPI_R1;
	if (opcode == MMC_SEND_EXT_CSD)
		SET(cmd.c_flags, SCF_RSP_R1);
	else
		SET(cmd.c_flags, SCF_RSP_R2);
	if (ISSET(sc->sc_caps, SMC_CAPS_DMA))
		cmd.c_dmamap = sc->sc_dmap;

	error = sdmmc_mmc_command(sc, &cmd);
	if (error == 0) {
		if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
			bus_dmamap_sync(sc->sc_dmat, sc->sc_dmap, 0, datalen,
			    BUS_DMASYNC_POSTREAD);
		}
		memcpy(data, ptr, datalen);
#ifdef SDMMC_DEBUG
		sdmmc_dump_data("CXD", data, datalen);
#endif
	}

out:
	if (ptr != NULL) {
		if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
			bus_dmamap_unload(sc->sc_dmat, sc->sc_dmap);
dmamem_unmap:
			bus_dmamem_unmap(sc->sc_dmat, ptr, datalen);
dmamem_free:
			bus_dmamem_free(sc->sc_dmat, ds, rseg);
		} else {
			free(ptr, M_DEVBUF);
		}
	}
	return error;
}

static int
sdmmc_mem_read_extr_single(struct sdmmc_softc *sc, struct sdmmc_function *sf,
    uint8_t mio, uint8_t fno, uint32_t addr, uint16_t datalen, void *data)
{
	struct sdmmc_command cmd;
	bus_dma_segment_t ds[1];
	void *ptr = NULL;
	int rseg;
	int error = 0;

	if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
		error = bus_dmamem_alloc(sc->sc_dmat, datalen, PAGE_SIZE, 0, ds,
		    1, &rseg, BUS_DMA_NOWAIT);
		if (error)
			goto out;
		error = bus_dmamem_map(sc->sc_dmat, ds, 1, datalen, &ptr,
		    BUS_DMA_NOWAIT);
		if (error)
			goto dmamem_free;
		error = bus_dmamap_load(sc->sc_dmat, sc->sc_dmap, ptr, datalen,
		    NULL, BUS_DMA_NOWAIT|BUS_DMA_STREAMING|BUS_DMA_READ);
		if (error)
			goto dmamem_unmap;

		bus_dmamap_sync(sc->sc_dmat, sc->sc_dmap, 0, datalen,
		    BUS_DMASYNC_PREREAD);
	} else {
		ptr = data;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.c_data = ptr;
	cmd.c_datalen = datalen;
	cmd.c_blklen = SDMMC_SECTOR_SIZE;
	cmd.c_opcode = SD_READ_EXTR_SINGLE;
	cmd.c_arg = __SHIFTIN((uint32_t)mio, SD_EXTR_MIO) |
		    __SHIFTIN((uint32_t)fno, SD_EXTR_FNO) |
		    __SHIFTIN(addr, SD_EXTR_ADDR) |
		    __SHIFTIN(datalen - 1, SD_EXTR_LEN);
	cmd.c_flags = SCF_CMD_ADTC | SCF_CMD_READ | SCF_RSP_R1;
	if (ISSET(sc->sc_caps, SMC_CAPS_DMA))
		cmd.c_dmamap = sc->sc_dmap;

	error = sdmmc_mmc_command(sc, &cmd);
	if (error == 0) {
		if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
			bus_dmamap_sync(sc->sc_dmat, sc->sc_dmap, 0, datalen,
			    BUS_DMASYNC_POSTREAD);
			memcpy(data, ptr, datalen);
		}
#ifdef SDMMC_DEBUG
		sdmmc_dump_data("EXT", data, datalen);
#endif
	}

out:
	if (ptr != NULL) {
		if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
			bus_dmamap_unload(sc->sc_dmat, sc->sc_dmap);
dmamem_unmap:
			bus_dmamem_unmap(sc->sc_dmat, ptr, datalen);
dmamem_free:
			bus_dmamem_free(sc->sc_dmat, ds, rseg);
		}
	}
	return error;
}

static int
sdmmc_mem_write_extr_single(struct sdmmc_softc *sc, struct sdmmc_function *sf,
    uint8_t mio, uint8_t fno, uint32_t addr, uint8_t value, bool poll)
{
	struct sdmmc_command cmd;
	bus_dma_segment_t ds[1];
	uint8_t buf[512];
	uint16_t buflen = sizeof(buf);
	void *ptr = NULL;
	int rseg;
	int error = 0;

	if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
		error = bus_dmamem_alloc(sc->sc_dmat, buflen, PAGE_SIZE, 0, ds,
		    1, &rseg, BUS_DMA_NOWAIT);
		if (error)
			goto out;
		error = bus_dmamem_map(sc->sc_dmat, ds, 1, buflen, &ptr,
		    BUS_DMA_NOWAIT);
		if (error)
			goto dmamem_free;
		error = bus_dmamap_load(sc->sc_dmat, sc->sc_dmap, ptr, buflen,
		    NULL, BUS_DMA_NOWAIT|BUS_DMA_STREAMING|BUS_DMA_WRITE);
		if (error)
			goto dmamem_unmap;

		memset(ptr, 0, buflen);
		*(uint8_t *)ptr = value;

		bus_dmamap_sync(sc->sc_dmat, sc->sc_dmap, 0, buflen,
		    BUS_DMASYNC_PREWRITE);
	} else {
		buf[0] = value;
		ptr = buf;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.c_data = ptr;
	cmd.c_datalen = buflen;
	cmd.c_blklen = SDMMC_SECTOR_SIZE;
	cmd.c_opcode = SD_WRITE_EXTR_SINGLE;
	cmd.c_arg = __SHIFTIN((uint32_t)mio, SD_EXTR_MIO) |
		    __SHIFTIN((uint32_t)fno, SD_EXTR_FNO) |
		    __SHIFTIN(addr, SD_EXTR_ADDR) |
		    __SHIFTIN(0, SD_EXTR_LEN);
	cmd.c_flags = SCF_CMD_ADTC | SCF_RSP_R1;
	if (ISSET(sc->sc_caps, SMC_CAPS_DMA))
		cmd.c_dmamap = sc->sc_dmap;

	error = sdmmc_mmc_command(sc, &cmd);
	if (error == 0) {
		if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
			bus_dmamap_sync(sc->sc_dmat, sc->sc_dmap, 0, buflen,
			    BUS_DMASYNC_POSTWRITE);
		}
	}

out:
	if (ptr != NULL) {
		if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
			bus_dmamap_unload(sc->sc_dmat, sc->sc_dmap);
dmamem_unmap:
			bus_dmamem_unmap(sc->sc_dmat, ptr, buflen);
dmamem_free:
			bus_dmamem_free(sc->sc_dmat, ds, rseg);
		}
	}

	if (!error) {
		do {
			memset(&cmd, 0, sizeof(cmd));
			cmd.c_opcode = MMC_SEND_STATUS;
			cmd.c_arg = MMC_ARG_RCA(sf->rca);
			cmd.c_flags = SCF_CMD_AC | SCF_RSP_R1 | SCF_RSP_SPI_R2 |
				      SCF_TOUT_OK;
			if (poll) {
				cmd.c_flags |= SCF_POLL;
			}
			error = sdmmc_mmc_command(sc, &cmd);
			if (error)
				break;
			/* XXX time out */
		} while (!ISSET(MMC_R1(cmd.c_resp), MMC_R1_READY_FOR_DATA));

		if (error) {
			aprint_error_dev(sc->sc_dev,
			    "error waiting for data ready after ext write : %d\n",
			    error);
		}
	}

	return error;
}

static int
sdmmc_set_bus_width(struct sdmmc_function *sf, int width)
{
	struct sdmmc_softc *sc = sf->sc;
	struct sdmmc_command cmd;
	int error;

	if (ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE))
		return ENODEV;

	memset(&cmd, 0, sizeof(cmd));
	cmd.c_opcode = SD_APP_SET_BUS_WIDTH;
	cmd.c_flags = SCF_RSP_R1 | SCF_CMD_AC;

	switch (width) {
	case 1:
		cmd.c_arg = SD_ARG_BUS_WIDTH_1;
		break;

	case 4:
		cmd.c_arg = SD_ARG_BUS_WIDTH_4;
		break;

	default:
		return EINVAL;
	}

	error = sdmmc_app_command(sc, sf, &cmd);
	if (error == 0)
		error = sdmmc_chip_bus_width(sc->sc_sct, sc->sc_sch, width);
	return error;
}

static int
sdmmc_mem_sd_switch(struct sdmmc_function *sf, int mode, int group,
    int function, sdmmc_bitfield512_t *status)
{
	struct sdmmc_softc *sc = sf->sc;
	struct sdmmc_command cmd;
	bus_dma_segment_t ds[1];
	void *ptr = NULL;
	int gsft, rseg, error = 0;
	const int statlen = 64;

	if (sf->scr.sd_spec >= SCR_SD_SPEC_VER_1_10 &&
	    !ISSET(sf->csd.ccc, SD_CSD_CCC_SWITCH))
		return EINVAL;

	if (group <= 0 || group > 6 ||
	    function < 0 || function > 15)
		return EINVAL;

	gsft = (group - 1) << 2;

	if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
		error = bus_dmamem_alloc(sc->sc_dmat, statlen, PAGE_SIZE, 0, ds,
		    1, &rseg, BUS_DMA_NOWAIT);
		if (error)
			goto out;
		error = bus_dmamem_map(sc->sc_dmat, ds, 1, statlen, &ptr,
		    BUS_DMA_NOWAIT);
		if (error)
			goto dmamem_free;
		error = bus_dmamap_load(sc->sc_dmat, sc->sc_dmap, ptr, statlen,
		    NULL, BUS_DMA_NOWAIT|BUS_DMA_STREAMING|BUS_DMA_READ);
		if (error)
			goto dmamem_unmap;

		bus_dmamap_sync(sc->sc_dmat, sc->sc_dmap, 0, statlen,
		    BUS_DMASYNC_PREREAD);
	} else {
		ptr = malloc(statlen, M_DEVBUF, M_NOWAIT | M_ZERO);
		if (ptr == NULL)
			goto out;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.c_data = ptr;
	cmd.c_datalen = statlen;
	cmd.c_blklen = statlen;
	cmd.c_opcode = SD_SEND_SWITCH_FUNC;
	cmd.c_arg = ((uint32_t)!!mode << 31) |
	    (function << gsft) | (0x00ffffff & ~(0xf << gsft));
	cmd.c_flags = SCF_CMD_ADTC | SCF_CMD_READ | SCF_RSP_R1 | SCF_RSP_SPI_R1;
	if (ISSET(sc->sc_caps, SMC_CAPS_DMA))
		cmd.c_dmamap = sc->sc_dmap;

	error = sdmmc_mmc_command(sc, &cmd);
	if (error == 0) {
		if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
			bus_dmamap_sync(sc->sc_dmat, sc->sc_dmap, 0, statlen,
			    BUS_DMASYNC_POSTREAD);
		}
		memcpy(status, ptr, statlen);
	}

out:
	if (ptr != NULL) {
		if (ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
			bus_dmamap_unload(sc->sc_dmat, sc->sc_dmap);
dmamem_unmap:
			bus_dmamem_unmap(sc->sc_dmat, ptr, statlen);
dmamem_free:
			bus_dmamem_free(sc->sc_dmat, ds, rseg);
		} else {
			free(ptr, M_DEVBUF);
		}
	}

	if (error == 0)
		sdmmc_be512_to_bitfield512(status);

	return error;
}

static int
sdmmc_mem_mmc_switch(struct sdmmc_function *sf, uint8_t set, uint8_t index,
    uint8_t value, bool poll)
{
	struct sdmmc_softc *sc = sf->sc;
	struct sdmmc_command cmd;
	int error;

	memset(&cmd, 0, sizeof(cmd));
	cmd.c_opcode = MMC_SWITCH;
	cmd.c_arg = (MMC_SWITCH_MODE_WRITE_BYTE << 24) |
	    (index << 16) | (value << 8) | set;
	cmd.c_flags = SCF_RSP_SPI_R1B | SCF_RSP_R1B | SCF_CMD_AC;

	if (poll)
		cmd.c_flags |= SCF_POLL;

	error = sdmmc_mmc_command(sc, &cmd);
	if (error)
		return error;

	if (index == EXT_CSD_FLUSH_CACHE || (index == EXT_CSD_HS_TIMING && value >= 2)) {
		do {
			memset(&cmd, 0, sizeof(cmd));
			cmd.c_opcode = MMC_SEND_STATUS;
			if (!ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE))
				cmd.c_arg = MMC_ARG_RCA(sf->rca);
			cmd.c_flags = SCF_CMD_AC | SCF_RSP_R1 | SCF_RSP_SPI_R2;
			if (poll)
				cmd.c_flags |= SCF_POLL;
			error = sdmmc_mmc_command(sc, &cmd);
			if (error)
				break;
			if (ISSET(MMC_R1(cmd.c_resp), MMC_R1_SWITCH_ERROR)) {
				aprint_error_dev(sc->sc_dev, "switch error\n");
				return EINVAL;
			}
			/* XXX time out */
		} while (!ISSET(MMC_R1(cmd.c_resp), MMC_R1_READY_FOR_DATA));

		if (error) {
			aprint_error_dev(sc->sc_dev,
			    "error waiting for data ready after switch command: %d\n",
			    error);
			return error;
		}
	}

	return 0;
}

/*
 * SPI mode function
 */
static int
sdmmc_mem_spi_read_ocr(struct sdmmc_softc *sc, uint32_t hcs, uint32_t *card_ocr)
{
	struct sdmmc_command cmd;
	int error;

	memset(&cmd, 0, sizeof(cmd));
	cmd.c_opcode = MMC_READ_OCR;
	cmd.c_arg = hcs ? MMC_OCR_HCS : 0;
	cmd.c_flags = SCF_RSP_SPI_R3;

	error = sdmmc_mmc_command(sc, &cmd);
	if (error == 0 && card_ocr != NULL)
		*card_ocr = cmd.c_resp[1];
	DPRINTF(("%s: sdmmc_mem_spi_read_ocr: error=%d, ocr=%#x\n",
	    SDMMCDEVNAME(sc), error, cmd.c_resp[1]));
	return error;
}

/*
 * read/write function
 */
/* read */
static int
sdmmc_mem_single_read_block(struct sdmmc_function *sf, uint32_t blkno,
    u_char *data, size_t datalen)
{
	struct sdmmc_softc *sc = sf->sc;
	int error = 0;
	int i;

	KASSERT((datalen % SDMMC_SECTOR_SIZE) == 0);
	KASSERT(!ISSET(sc->sc_caps, SMC_CAPS_DMA));

	for (i = 0; i < datalen / SDMMC_SECTOR_SIZE; i++) {
		error = sdmmc_mem_read_block_subr(sf, sc->sc_dmap, blkno + i,
		    data + i * SDMMC_SECTOR_SIZE, SDMMC_SECTOR_SIZE);
		if (error)
			break;
	}
	return error;
}

/*
 * Simulate multi-segment dma transfer.
 */
static int
sdmmc_mem_single_segment_dma_read_block(struct sdmmc_function *sf,
    uint32_t blkno, u_char *data, size_t datalen)
{
	struct sdmmc_softc *sc = sf->sc;
	bool use_bbuf = false;
	int error = 0;
	int i;

	for (i = 0; i < sc->sc_dmap->dm_nsegs; i++) {
		size_t len = sc->sc_dmap->dm_segs[i].ds_len;
		if ((len % SDMMC_SECTOR_SIZE) != 0) {
			use_bbuf = true;
			break;
		}
	}
	if (use_bbuf) {
		bus_dmamap_sync(sc->sc_dmat, sf->bbuf_dmap, 0, datalen,
		    BUS_DMASYNC_PREREAD);

		error = sdmmc_mem_read_block_subr(sf, sf->bbuf_dmap,
		    blkno, data, datalen);
		if (error) {
			bus_dmamap_unload(sc->sc_dmat, sf->bbuf_dmap);
			return error;
		}

		bus_dmamap_sync(sc->sc_dmat, sf->bbuf_dmap, 0, datalen,
		    BUS_DMASYNC_POSTREAD);

		/* Copy from bounce buffer */
		memcpy(data, sf->bbuf, datalen);

		return 0;
	}

	for (i = 0; i < sc->sc_dmap->dm_nsegs; i++) {
		size_t len = sc->sc_dmap->dm_segs[i].ds_len;

		error = bus_dmamap_load(sc->sc_dmat, sf->sseg_dmap,
		    data, len, NULL, BUS_DMA_NOWAIT|BUS_DMA_READ);
		if (error)
			return error;

		bus_dmamap_sync(sc->sc_dmat, sf->sseg_dmap, 0, len,
		    BUS_DMASYNC_PREREAD);

		error = sdmmc_mem_read_block_subr(sf, sf->sseg_dmap,
		    blkno, data, len);
		if (error) {
			bus_dmamap_unload(sc->sc_dmat, sf->sseg_dmap);
			return error;
		}

		bus_dmamap_sync(sc->sc_dmat, sf->sseg_dmap, 0, len,
		    BUS_DMASYNC_POSTREAD);

		bus_dmamap_unload(sc->sc_dmat, sf->sseg_dmap);

		blkno += len / SDMMC_SECTOR_SIZE;
		data += len;
	}
	return 0;
}

static int
sdmmc_mem_read_block_subr(struct sdmmc_function *sf, bus_dmamap_t dmap,
    uint32_t blkno, u_char *data, size_t datalen)
{
	struct sdmmc_softc *sc = sf->sc;
	struct sdmmc_command cmd;
	int error;

	if (!ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE)) {
		error = sdmmc_select_card(sc, sf);
		if (error)
			goto out;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.c_data = data;
	cmd.c_datalen = datalen;
	cmd.c_blklen = SDMMC_SECTOR_SIZE;
	cmd.c_opcode = (cmd.c_datalen / cmd.c_blklen) > 1 ?
	    MMC_READ_BLOCK_MULTIPLE : MMC_READ_BLOCK_SINGLE;
	cmd.c_arg = blkno;
	if (!ISSET(sf->flags, SFF_SDHC))
		cmd.c_arg <<= SDMMC_SECTOR_SIZE_SB;
	cmd.c_flags = SCF_CMD_ADTC | SCF_CMD_READ | SCF_RSP_R1 | SCF_RSP_SPI_R1;
	if (ISSET(sf->flags, SFF_SDHC))
		cmd.c_flags |= SCF_XFER_SDHC;
	if (ISSET(sc->sc_caps, SMC_CAPS_DMA))
		cmd.c_dmamap = dmap;

	sc->sc_ev_xfer.ev_count++;

	error = sdmmc_mmc_command(sc, &cmd);
	if (error) {
		sc->sc_ev_xfer_error.ev_count++;
		goto out;
	}

	const u_int counter = __builtin_ctz(cmd.c_datalen);
	if (counter >= 9 && counter <= 16) {
		sc->sc_ev_xfer_aligned[counter - 9].ev_count++;
	} else {
		sc->sc_ev_xfer_unaligned.ev_count++;
	}

	if (!ISSET(sc->sc_caps, SMC_CAPS_AUTO_STOP)) {
		if (cmd.c_opcode == MMC_READ_BLOCK_MULTIPLE) {
			memset(&cmd, 0, sizeof cmd);
			cmd.c_opcode = MMC_STOP_TRANSMISSION;
			cmd.c_arg = MMC_ARG_RCA(sf->rca);
			cmd.c_flags = SCF_CMD_AC | SCF_RSP_R1B | SCF_RSP_SPI_R1B;
			error = sdmmc_mmc_command(sc, &cmd);
			if (error)
				goto out;
		}
	}

	if (!ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE)) {
		do {
			memset(&cmd, 0, sizeof(cmd));
			cmd.c_opcode = MMC_SEND_STATUS;
			if (!ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE))
				cmd.c_arg = MMC_ARG_RCA(sf->rca);
			cmd.c_flags = SCF_CMD_AC | SCF_RSP_R1 | SCF_RSP_SPI_R2;
			error = sdmmc_mmc_command(sc, &cmd);
			if (error)
				break;
			/* XXX time out */
		} while (!ISSET(MMC_R1(cmd.c_resp), MMC_R1_READY_FOR_DATA));
	}

out:
	return error;
}

int
sdmmc_mem_read_block(struct sdmmc_function *sf, uint32_t blkno, u_char *data,
    size_t datalen)
{
	struct sdmmc_softc *sc = sf->sc;
	int error;

	SDMMC_LOCK(sc);
	mutex_enter(&sc->sc_mtx);

	if (ISSET(sc->sc_caps, SMC_CAPS_SINGLE_ONLY)) {
		error = sdmmc_mem_single_read_block(sf, blkno, data, datalen);
		goto out;
	}

	if (!ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
		error = sdmmc_mem_read_block_subr(sf, sc->sc_dmap, blkno, data,
		    datalen);
		goto out;
	}

	/* DMA transfer */
	error = bus_dmamap_load(sc->sc_dmat, sc->sc_dmap, data, datalen, NULL,
	    BUS_DMA_NOWAIT|BUS_DMA_READ);
	if (error)
		goto out;

#ifdef SDMMC_DEBUG
	printf("data=%p, datalen=%zu\n", data, datalen);
	for (int i = 0; i < sc->sc_dmap->dm_nsegs; i++) {
		printf("seg#%d: addr=%#lx, size=%#lx\n", i,
		    (u_long)sc->sc_dmap->dm_segs[i].ds_addr,
		    (u_long)sc->sc_dmap->dm_segs[i].ds_len);
	}
#endif

	if (sc->sc_dmap->dm_nsegs > 1
	    && !ISSET(sc->sc_caps, SMC_CAPS_MULTI_SEG_DMA)) {
		error = sdmmc_mem_single_segment_dma_read_block(sf, blkno,
		    data, datalen);
		goto unload;
	}

	bus_dmamap_sync(sc->sc_dmat, sc->sc_dmap, 0, datalen,
	    BUS_DMASYNC_PREREAD);

	error = sdmmc_mem_read_block_subr(sf, sc->sc_dmap, blkno, data,
	    datalen);
	if (error)
		goto unload;

	bus_dmamap_sync(sc->sc_dmat, sc->sc_dmap, 0, datalen,
	    BUS_DMASYNC_POSTREAD);
unload:
	bus_dmamap_unload(sc->sc_dmat, sc->sc_dmap);

out:
	mutex_exit(&sc->sc_mtx);
	SDMMC_UNLOCK(sc);

	return error;
}

/* write */
static int
sdmmc_mem_single_write_block(struct sdmmc_function *sf, uint32_t blkno,
    u_char *data, size_t datalen)
{
	struct sdmmc_softc *sc = sf->sc;
	int error = 0;
	int i;

	KASSERT((datalen % SDMMC_SECTOR_SIZE) == 0);
	KASSERT(!ISSET(sc->sc_caps, SMC_CAPS_DMA));

	for (i = 0; i < datalen / SDMMC_SECTOR_SIZE; i++) {
		error = sdmmc_mem_write_block_subr(sf, sc->sc_dmap, blkno + i,
		    data + i * SDMMC_SECTOR_SIZE, SDMMC_SECTOR_SIZE);
		if (error)
			break;
	}
	return error;
}

/*
 * Simulate multi-segment dma transfer.
 */
static int
sdmmc_mem_single_segment_dma_write_block(struct sdmmc_function *sf,
    uint32_t blkno, u_char *data, size_t datalen)
{
	struct sdmmc_softc *sc = sf->sc;
	bool use_bbuf = false;
	int error = 0;
	int i;

	for (i = 0; i < sc->sc_dmap->dm_nsegs; i++) {
		size_t len = sc->sc_dmap->dm_segs[i].ds_len;
		if ((len % SDMMC_SECTOR_SIZE) != 0) {
			use_bbuf = true;
			break;
		}
	}
	if (use_bbuf) {
		/* Copy to bounce buffer */
		memcpy(sf->bbuf, data, datalen);

		bus_dmamap_sync(sc->sc_dmat, sf->bbuf_dmap, 0, datalen,
		    BUS_DMASYNC_PREWRITE);

		error = sdmmc_mem_write_block_subr(sf, sf->bbuf_dmap,
		    blkno, data, datalen);
		if (error) {
			bus_dmamap_unload(sc->sc_dmat, sf->bbuf_dmap);
			return error;
		}

		bus_dmamap_sync(sc->sc_dmat, sf->bbuf_dmap, 0, datalen,
		    BUS_DMASYNC_POSTWRITE);

		return 0;
	}

	for (i = 0; i < sc->sc_dmap->dm_nsegs; i++) {
		size_t len = sc->sc_dmap->dm_segs[i].ds_len;

		error = bus_dmamap_load(sc->sc_dmat, sf->sseg_dmap,
		    data, len, NULL, BUS_DMA_NOWAIT|BUS_DMA_WRITE);
		if (error)
			return error;

		bus_dmamap_sync(sc->sc_dmat, sf->sseg_dmap, 0, len,
		    BUS_DMASYNC_PREWRITE);

		error = sdmmc_mem_write_block_subr(sf, sf->sseg_dmap,
		    blkno, data, len);
		if (error) {
			bus_dmamap_unload(sc->sc_dmat, sf->sseg_dmap);
			return error;
		}

		bus_dmamap_sync(sc->sc_dmat, sf->sseg_dmap, 0, len,
		    BUS_DMASYNC_POSTWRITE);

		bus_dmamap_unload(sc->sc_dmat, sf->sseg_dmap);

		blkno += len / SDMMC_SECTOR_SIZE;
		data += len;
	}

	return error;
}

static int
sdmmc_mem_write_block_subr(struct sdmmc_function *sf, bus_dmamap_t dmap,
    uint32_t blkno, u_char *data, size_t datalen)
{
	struct sdmmc_softc *sc = sf->sc;
	struct sdmmc_command cmd;
	int error;

	if (!ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE)) {
		error = sdmmc_select_card(sc, sf);
		if (error)
			goto out;
	}

	const int nblk = howmany(datalen, SDMMC_SECTOR_SIZE);
	if (ISSET(sc->sc_flags, SMF_SD_MODE) && nblk > 1) {
		/* Set the number of write blocks to be pre-erased */
		memset(&cmd, 0, sizeof(cmd));
		cmd.c_opcode = SD_APP_SET_WR_BLK_ERASE_COUNT;
		cmd.c_flags = SCF_RSP_R1 | SCF_RSP_SPI_R1 | SCF_CMD_AC;
		cmd.c_arg = nblk;
		error = sdmmc_app_command(sc, sf, &cmd);
		if (error)
			goto out;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.c_data = data;
	cmd.c_datalen = datalen;
	cmd.c_blklen = SDMMC_SECTOR_SIZE;
	cmd.c_opcode = (cmd.c_datalen / cmd.c_blklen) > 1 ?
	    MMC_WRITE_BLOCK_MULTIPLE : MMC_WRITE_BLOCK_SINGLE;
	cmd.c_arg = blkno;
	if (!ISSET(sf->flags, SFF_SDHC))
		cmd.c_arg <<= SDMMC_SECTOR_SIZE_SB;
	cmd.c_flags = SCF_CMD_ADTC | SCF_RSP_R1;
	if (ISSET(sf->flags, SFF_SDHC))
		cmd.c_flags |= SCF_XFER_SDHC;
	if (ISSET(sc->sc_caps, SMC_CAPS_DMA))
		cmd.c_dmamap = dmap;

	sc->sc_ev_xfer.ev_count++;

	error = sdmmc_mmc_command(sc, &cmd);
	if (error) {
		sc->sc_ev_xfer_error.ev_count++;
		goto out;
	}

	const u_int counter = __builtin_ctz(cmd.c_datalen);
	if (counter >= 9 && counter <= 16) {
		sc->sc_ev_xfer_aligned[counter - 9].ev_count++;
	} else {
		sc->sc_ev_xfer_unaligned.ev_count++;
	}

	if (!ISSET(sc->sc_caps, SMC_CAPS_AUTO_STOP)) {
		if (cmd.c_opcode == MMC_WRITE_BLOCK_MULTIPLE) {
			memset(&cmd, 0, sizeof(cmd));
			cmd.c_opcode = MMC_STOP_TRANSMISSION;
			cmd.c_flags = SCF_CMD_AC | SCF_RSP_R1B | SCF_RSP_SPI_R1B;
			error = sdmmc_mmc_command(sc, &cmd);
			if (error)
				goto out;
		}
	}

	if (!ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE)) {
		do {
			memset(&cmd, 0, sizeof(cmd));
			cmd.c_opcode = MMC_SEND_STATUS;
			if (!ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE))
				cmd.c_arg = MMC_ARG_RCA(sf->rca);
			cmd.c_flags = SCF_CMD_AC | SCF_RSP_R1 | SCF_RSP_SPI_R2;
			error = sdmmc_mmc_command(sc, &cmd);
			if (error)
				break;
			/* XXX time out */
		} while (!ISSET(MMC_R1(cmd.c_resp), MMC_R1_READY_FOR_DATA));
	}

out:
	return error;
}

int
sdmmc_mem_write_block(struct sdmmc_function *sf, uint32_t blkno, u_char *data,
    size_t datalen)
{
	struct sdmmc_softc *sc = sf->sc;
	int error;

	SDMMC_LOCK(sc);
	mutex_enter(&sc->sc_mtx);

	if (ISSET(sc->sc_flags, SMF_SD_MODE) &&
	    sdmmc_chip_write_protect(sc->sc_sct, sc->sc_sch)) {
		aprint_normal_dev(sc->sc_dev, "write-protected\n");
		error = EIO;
		goto out;
	}

	if (ISSET(sc->sc_caps, SMC_CAPS_SINGLE_ONLY)) {
		error = sdmmc_mem_single_write_block(sf, blkno, data, datalen);
		goto out;
	}

	if (!ISSET(sc->sc_caps, SMC_CAPS_DMA)) {
		error = sdmmc_mem_write_block_subr(sf, sc->sc_dmap, blkno, data,
		    datalen);
		goto out;
	}

	/* DMA transfer */
	error = bus_dmamap_load(sc->sc_dmat, sc->sc_dmap, data, datalen, NULL,
	    BUS_DMA_NOWAIT|BUS_DMA_WRITE);
	if (error)
		goto out;

#ifdef SDMMC_DEBUG
	aprint_normal_dev(sc->sc_dev, "%s: data=%p, datalen=%zu\n",
	    __func__, data, datalen);
	for (int i = 0; i < sc->sc_dmap->dm_nsegs; i++) {
		aprint_normal_dev(sc->sc_dev,
		    "%s: seg#%d: addr=%#lx, size=%#lx\n", __func__, i,
		    (u_long)sc->sc_dmap->dm_segs[i].ds_addr,
		    (u_long)sc->sc_dmap->dm_segs[i].ds_len);
	}
#endif

	if (sc->sc_dmap->dm_nsegs > 1
	    && !ISSET(sc->sc_caps, SMC_CAPS_MULTI_SEG_DMA)) {
		error = sdmmc_mem_single_segment_dma_write_block(sf, blkno,
		    data, datalen);
		goto unload;
	}

	bus_dmamap_sync(sc->sc_dmat, sc->sc_dmap, 0, datalen,
	    BUS_DMASYNC_PREWRITE);

	error = sdmmc_mem_write_block_subr(sf, sc->sc_dmap, blkno, data,
	    datalen);
	if (error)
		goto unload;

	bus_dmamap_sync(sc->sc_dmat, sc->sc_dmap, 0, datalen,
	    BUS_DMASYNC_POSTWRITE);
unload:
	bus_dmamap_unload(sc->sc_dmat, sc->sc_dmap);

out:
	mutex_exit(&sc->sc_mtx);
	SDMMC_UNLOCK(sc);

	return error;
}

int
sdmmc_mem_discard(struct sdmmc_function *sf, uint32_t sblkno, uint32_t eblkno)
{
	struct sdmmc_softc *sc = sf->sc;
	struct sdmmc_command cmd;
	int error;

	if (ISSET(sc->sc_caps, SMC_CAPS_SPI_MODE))
		return ENODEV;	/* XXX not tested */

	if (eblkno < sblkno)
		return EINVAL;

	SDMMC_LOCK(sc);
	mutex_enter(&sc->sc_mtx);

	/* Set the address of the first write block to be erased */
	memset(&cmd, 0, sizeof(cmd));
	cmd.c_opcode = ISSET(sc->sc_flags, SMF_SD_MODE) ?
	    SD_ERASE_WR_BLK_START : MMC_TAG_ERASE_GROUP_START;
	cmd.c_arg = sblkno;
	if (!ISSET(sf->flags, SFF_SDHC))
		cmd.c_arg <<= SDMMC_SECTOR_SIZE_SB;
	cmd.c_flags = SCF_CMD_AC | SCF_RSP_R1;
	error = sdmmc_mmc_command(sc, &cmd);
	if (error)
		goto out;

	/* Set the address of the last write block to be erased */
	memset(&cmd, 0, sizeof(cmd));
	cmd.c_opcode = ISSET(sc->sc_flags, SMF_SD_MODE) ?
	    SD_ERASE_WR_BLK_END : MMC_TAG_ERASE_GROUP_END;
	cmd.c_arg = eblkno;
	if (!ISSET(sf->flags, SFF_SDHC))
		cmd.c_arg <<= SDMMC_SECTOR_SIZE_SB;
	cmd.c_flags = SCF_CMD_AC | SCF_RSP_R1;
	error = sdmmc_mmc_command(sc, &cmd);
	if (error)
		goto out;

	/* Start the erase operation */
	memset(&cmd, 0, sizeof(cmd));
	cmd.c_opcode = MMC_ERASE;
	cmd.c_flags = SCF_CMD_AC | SCF_RSP_R1B;
	error = sdmmc_mmc_command(sc, &cmd);
	if (error)
		goto out;

out:
	mutex_exit(&sc->sc_mtx);
	SDMMC_UNLOCK(sc);

#ifdef SDMMC_DEBUG
	device_printf(sc->sc_dev, "discard blk %u-%u error %d\n",
	    sblkno, eblkno, error);
#endif

	return error;
}

int
sdmmc_mem_flush_cache(struct sdmmc_function *sf, bool poll)
{
	struct sdmmc_softc *sc = sf->sc;
	int error;

	if (!ISSET(sf->flags, SFF_CACHE_ENABLED))
		return 0;

	SDMMC_LOCK(sc);
	mutex_enter(&sc->sc_mtx);

	if (ISSET(sc->sc_flags, SMF_SD_MODE)) {
		KASSERT(sf->ext_sd.pef.valid);
		error = sdmmc_mem_write_extr_single(sc, sf, SD_EXTR_MIO_MEM,
		    sf->ext_sd.pef.fno,
		    sf->ext_sd.pef.start_addr + SD_PEF_CACHE_FLUSH_OFFSET, 1,
		    poll);
		if (error == 0) {
			uint8_t data[512];

			error = sdmmc_mem_read_extr_single(sc, sf, SD_EXTR_MIO_MEM,
			    sf->ext_sd.pef.fno, sf->ext_sd.pef.start_addr,
			    sizeof(data), data);
			if (error == 0 && SD_PEF_CACHE_FLUSH(data) != 0) {
				device_printf(sc->sc_dev, "cache flush failed\n");
			}
		}
	} else {
		error = sdmmc_mem_mmc_switch(sf,
		    EXT_CSD_CMD_SET_NORMAL, EXT_CSD_FLUSH_CACHE,
		    EXT_CSD_FLUSH_CACHE_FLUSH, poll);
	}

	mutex_exit(&sc->sc_mtx);
	SDMMC_UNLOCK(sc);

#ifdef SDMMC_DEBUG
	device_printf(sc->sc_dev, "flush cache error %d\n", error);
#endif

	return error;
}
