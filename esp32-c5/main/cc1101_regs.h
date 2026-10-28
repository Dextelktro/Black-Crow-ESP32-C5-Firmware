/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

/* ============================================================
 * TI CC1101 register map, command strobes and status registers.
 *
 * Addresses follow the CC1101 datasheet (SWRS061). This header is
 * shared by the low-level driver (cc1101.c) and the Sub-GHz preset
 * tables (subghz_presets.c), whose register/value pairs reference
 * the CC1101_* names below exactly as the Flipper firmware does, so
 * ported presets stay byte-for-byte faithful.
 *
 * NOTE: status registers (0x30-0x3D) collide with the command
 * strobe opcodes of the same value. They are told apart on the SPI
 * bus by the burst bit: a status register is read with the header
 * (addr | READ | BURST); the same opcode without the burst bit is a
 * command strobe. The driver provides separate helpers for each.
 * ============================================================ */

/* ---- SPI header bits ---- */
#define CC1101_WRITE_SINGLE 0x00
#define CC1101_WRITE_BURST  0x40
#define CC1101_READ_SINGLE  0x80
#define CC1101_READ_BURST   0xC0

/* ---- Configuration registers (0x00-0x2E) ---- */
#define CC1101_IOCFG2   0x00  /* GDO2 output pin configuration */
#define CC1101_IOCFG1   0x01  /* GDO1 output pin configuration */
#define CC1101_IOCFG0   0x02  /* GDO0 output pin configuration */
#define CC1101_FIFOTHR  0x03  /* RX FIFO and TX FIFO thresholds */
#define CC1101_SYNC1    0x04  /* Sync word, high byte */
#define CC1101_SYNC0    0x05  /* Sync word, low byte */
#define CC1101_PKTLEN   0x06  /* Packet length */
#define CC1101_PKTCTRL1 0x07  /* Packet automation control */
#define CC1101_PKTCTRL0 0x08  /* Packet automation control */
#define CC1101_ADDR     0x09  /* Device address */
#define CC1101_CHANNR   0x0A  /* Channel number */
#define CC1101_FSCTRL1  0x0B  /* Frequency synthesizer control */
#define CC1101_FSCTRL0  0x0C  /* Frequency synthesizer control */
#define CC1101_FREQ2    0x0D  /* Frequency control word, high byte */
#define CC1101_FREQ1    0x0E  /* Frequency control word, middle byte */
#define CC1101_FREQ0    0x0F  /* Frequency control word, low byte */
#define CC1101_MDMCFG4  0x10  /* Modem configuration */
#define CC1101_MDMCFG3  0x11  /* Modem configuration */
#define CC1101_MDMCFG2  0x12  /* Modem configuration */
#define CC1101_MDMCFG1  0x13  /* Modem configuration */
#define CC1101_MDMCFG0  0x14  /* Modem configuration */
#define CC1101_DEVIATN  0x15  /* Modem deviation setting */
#define CC1101_MCSM2    0x16  /* Main Radio Control State Machine config */
#define CC1101_MCSM1    0x17  /* Main Radio Control State Machine config */
#define CC1101_MCSM0    0x18  /* Main Radio Control State Machine config */
#define CC1101_FOCCFG   0x19  /* Frequency Offset Compensation config */
#define CC1101_BSCFG    0x1A  /* Bit Synchronization configuration */
#define CC1101_AGCCTRL2 0x1B  /* AGC control */
#define CC1101_AGCCTRL1 0x1C  /* AGC control */
#define CC1101_AGCCTRL0 0x1D  /* AGC control */
#define CC1101_WOREVT1  0x1E  /* High byte Event0 timeout */
#define CC1101_WOREVT0  0x1F  /* Low byte Event0 timeout */
#define CC1101_WORCTRL  0x20  /* Wake On Radio control */
#define CC1101_FREND1   0x21  /* Front end RX configuration */
#define CC1101_FREND0   0x22  /* Front end TX configuration */
#define CC1101_FSCAL3   0x23  /* Frequency synthesizer calibration */
#define CC1101_FSCAL2   0x24  /* Frequency synthesizer calibration */
#define CC1101_FSCAL1   0x25  /* Frequency synthesizer calibration */
#define CC1101_FSCAL0   0x26  /* Frequency synthesizer calibration */
#define CC1101_RCCTRL1  0x27  /* RC oscillator configuration */
#define CC1101_RCCTRL0  0x28  /* RC oscillator configuration */
#define CC1101_FSTEST   0x29  /* Frequency synthesizer cal control */
#define CC1101_PTEST    0x2A  /* Production test */
#define CC1101_AGCTEST  0x2B  /* AGC test */
#define CC1101_TEST2    0x2C  /* Various test settings */
#define CC1101_TEST1    0x2D  /* Various test settings */
#define CC1101_TEST0    0x2E  /* Various test settings */

/* ---- Multi-byte / FIFO / PA table (special addresses) ---- */
#define CC1101_PATABLE  0x3E  /* PA table (8 bytes, burst) */
#define CC1101_FIFO     0x3F  /* RX/TX FIFO */

/* ---- Command strobes (0x30-0x3D, no burst bit) ---- */
#define CC1101_STROBE_SRES    0x30  /* Reset chip */
#define CC1101_STROBE_SFSTXON 0x31  /* Enable and calibrate synth */
#define CC1101_STROBE_SXOFF   0x32  /* Turn off crystal oscillator */
#define CC1101_STROBE_SCAL    0x33  /* Calibrate synth and disable */
#define CC1101_STROBE_SRX     0x34  /* Enable RX */
#define CC1101_STROBE_STX     0x35  /* Enable TX */
#define CC1101_STROBE_SIDLE   0x36  /* Exit RX/TX, enter IDLE */
#define CC1101_STROBE_SWOR    0x38  /* Start Wake-on-Radio */
#define CC1101_STROBE_SPWD    0x39  /* Power down when CSn goes high */
#define CC1101_STROBE_SFRX    0x3A  /* Flush RX FIFO */
#define CC1101_STROBE_SFTX    0x3B  /* Flush TX FIFO */
#define CC1101_STROBE_SWORRST 0x3C  /* Reset real time clock */
#define CC1101_STROBE_SNOP    0x3D  /* No operation */

/* ---- Status registers (0x30-0x3D, require burst bit on read) ---- */
#define CC1101_STATUS_PARTNUM    0x30
#define CC1101_STATUS_VERSION    0x31
#define CC1101_STATUS_FREQEST    0x32
#define CC1101_STATUS_LQI        0x33
#define CC1101_STATUS_RSSI       0x34
#define CC1101_STATUS_MARCSTATE  0x35
#define CC1101_STATUS_WORTIME1   0x36
#define CC1101_STATUS_WORTIME0   0x37
#define CC1101_STATUS_PKTSTATUS  0x38
#define CC1101_STATUS_VCO_VC_DAC 0x39
#define CC1101_STATUS_TXBYTES    0x3A
#define CC1101_STATUS_RXBYTES    0x3B

/* MARCSTATE values of interest */
#define CC1101_MARC_STATE_IDLE 0x01
#define CC1101_MARC_STATE_RX   0x0D
#define CC1101_MARC_STATE_TX   0x13

/* Crystal frequency of the module (Flipper/common CC1101 boards use 26 MHz;
 * the FSCTRL1 IF comment in the Flipper preset tables confirms 26 MHz). */
#define CC1101_XOSC_HZ 26000000UL
