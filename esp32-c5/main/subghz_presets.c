#include "subghz_presets.h"

#include <string.h>
#include <strings.h>

#include "cc1101.h"
#include "cc1101_regs.h"

/* Register tables are verbatim from Flipper firmware
 * lib/subghz/devices/cc1101_configs.c (dev branch), terminated by {0,0}. */

static const cc1101_reg_pair_t s_ook_270_regs[] = {
    {CC1101_IOCFG0, 0x0D},   /* GDO0 = async serial data out/in */
    {CC1101_FIFOTHR, 0x47},
    {CC1101_PKTCTRL0, 0x32}, /* async serial mode */
    {CC1101_FSCTRL1, 0x06},
    {CC1101_MDMCFG0, 0x00},
    {CC1101_MDMCFG1, 0x00},
    {CC1101_MDMCFG2, 0x30},  /* ASK/OOK, no preamble/sync */
    {CC1101_MDMCFG3, 0x32},
    {CC1101_MDMCFG4, 0x67},  /* RX BW 270.833 kHz */
    {CC1101_MCSM0, 0x18},
    {CC1101_FOCCFG, 0x18},
    {CC1101_AGCCTRL0, 0x40},
    {CC1101_AGCCTRL1, 0x00},
    {CC1101_AGCCTRL2, 0x03},
    {CC1101_WORCTRL, 0xFB},
    {CC1101_FREND0, 0x11},
    {CC1101_FREND1, 0xB6},
    {0, 0},
};

static const cc1101_reg_pair_t s_ook_650_regs[] = {
    {CC1101_IOCFG0, 0x0D},
    {CC1101_FIFOTHR, 0x07},
    {CC1101_PKTCTRL0, 0x32},
    {CC1101_FSCTRL1, 0x06},
    {CC1101_MDMCFG0, 0x00},
    {CC1101_MDMCFG1, 0x00},
    {CC1101_MDMCFG2, 0x30},
    {CC1101_MDMCFG3, 0x32},
    {CC1101_MDMCFG4, 0x17},  /* RX BW 650 kHz */
    {CC1101_MCSM0, 0x18},
    {CC1101_FOCCFG, 0x18},
    {CC1101_AGCCTRL0, 0x91},
    {CC1101_AGCCTRL1, 0x00},
    {CC1101_AGCCTRL2, 0x07},
    {CC1101_WORCTRL, 0xFB},
    {CC1101_FREND0, 0x11},
    {CC1101_FREND1, 0xB6},
    {0, 0},
};

static const cc1101_reg_pair_t s_2fsk_238_regs[] = {
    {CC1101_IOCFG0, 0x0D},
    {CC1101_FSCTRL1, 0x06},
    {CC1101_PKTCTRL0, 0x32},
    {CC1101_PKTCTRL1, 0x04},
    {CC1101_MDMCFG0, 0x00},
    {CC1101_MDMCFG1, 0x02},
    {CC1101_MDMCFG2, 0x04},  /* 2-FSK, no preamble/sync */
    {CC1101_MDMCFG3, 0x83},
    {CC1101_MDMCFG4, 0x67},
    {CC1101_DEVIATN, 0x04},  /* deviation 2.380 kHz */
    {CC1101_MCSM0, 0x18},
    {CC1101_FOCCFG, 0x16},
    {CC1101_AGCCTRL0, 0x91},
    {CC1101_AGCCTRL1, 0x00},
    {CC1101_AGCCTRL2, 0x07},
    {CC1101_WORCTRL, 0xFB},
    {CC1101_FREND0, 0x10},
    {CC1101_FREND1, 0x56},
    {0, 0},
};

static const cc1101_reg_pair_t s_2fsk_476_regs[] = {
    {CC1101_IOCFG0, 0x0D},
    {CC1101_FSCTRL1, 0x06},
    {CC1101_PKTCTRL0, 0x32},
    {CC1101_PKTCTRL1, 0x04},
    {CC1101_MDMCFG0, 0x00},
    {CC1101_MDMCFG1, 0x02},
    {CC1101_MDMCFG2, 0x04},
    {CC1101_MDMCFG3, 0x83},
    {CC1101_MDMCFG4, 0x67},
    {CC1101_DEVIATN, 0x47},  /* deviation 47.607 kHz */
    {CC1101_MCSM0, 0x18},
    {CC1101_FOCCFG, 0x16},
    {CC1101_AGCCTRL0, 0x91},
    {CC1101_AGCCTRL1, 0x00},
    {CC1101_AGCCTRL2, 0x07},
    {CC1101_WORCTRL, 0xFB},
    {CC1101_FREND0, 0x10},
    {CC1101_FREND1, 0x56},
    {0, 0},
};

static const subghz_preset_t s_presets[SUBGHZ_PRESET_COUNT] = {
    [SUBGHZ_PRESET_OOK_270] = {
        .name = "ook270",
        .flipper_name = "FuriHalSubGhzPresetOok270Async",
        .regs = s_ook_270_regs,
        .patable = {0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        .fm = false,
    },
    [SUBGHZ_PRESET_OOK_650] = {
        .name = "ook650",
        .flipper_name = "FuriHalSubGhzPresetOok650Async",
        .regs = s_ook_650_regs,
        .patable = {0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        .fm = false,
    },
    [SUBGHZ_PRESET_2FSK_238] = {
        .name = "2fsk238",
        .flipper_name = "FuriHalSubGhzPreset2FSKDev238Async",
        .regs = s_2fsk_238_regs,
        .patable = {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        .fm = true,
    },
    [SUBGHZ_PRESET_2FSK_476] = {
        .name = "2fsk476",
        .flipper_name = "FuriHalSubGhzPreset2FSKDev476Async",
        .regs = s_2fsk_476_regs,
        .patable = {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        .fm = true,
    },
};

const subghz_preset_t *subghz_preset_get(subghz_preset_id_t id)
{
    if (id < 0 || id >= SUBGHZ_PRESET_COUNT) return NULL;
    return &s_presets[id];
}

const subghz_preset_t *subghz_preset_from_name(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < SUBGHZ_PRESET_COUNT; i++) {
        if (strcasecmp(name, s_presets[i].name) == 0 ||
            strcmp(name, s_presets[i].flipper_name) == 0) {
            return &s_presets[i];
        }
    }
    return NULL;
}

esp_err_t subghz_preset_apply(const subghz_preset_t *preset)
{
    if (!preset || !preset->regs) return ESP_ERR_INVALID_ARG;
    for (const cc1101_reg_pair_t *p = preset->regs; p->reg != 0; p++) {
        esp_err_t err = cc1101_write_reg(p->reg, p->val);
        if (err != ESP_OK) return err;
    }
    return cc1101_write_patable(preset->patable);
}
