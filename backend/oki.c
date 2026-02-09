/*
 * sane - Scanner Access Now Easy.
 * Copyright (C) 2012 Oki Data Corporation
 * Copyright (C) 2026 SANE Developers
 *
 * This file is part of the SANE package.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * As a special exception, the authors of SANE give permission for
 * additional uses of the libraries contained in this release of SANE.
 *
 * The exception is that, if you link a SANE library with other files
 * to produce an executable, this does not by itself cause the
 * resulting executable to be covered by the GNU General Public
 * License.  Your use of that executable is in no way restricted on
 * account of linking the SANE library code into it.
 *
 * This exception does not, however, invalidate any other reasons why
 * the executable file might be covered by the GNU General Public
 * License.
 *
 * If you submit changes to SANE to the maintainers to be included in
 * a subsequent release, you agree by submitting the changes that
 * those changes may be distributed with this exception intact.
 *
 * If you write modifications of your own for SANE, it is your choice
 * whether to permit this exception to apply to your modifications.
 * If you do not wish that, delete this exception notice.
 *
 * OKI MFP scanner backend.
 *
 * Communicates with OKI MFP scanners over USB using the PCScan
 * protocol. Protocol details derived from analysis of the vendor's
 * driver library.
 */

#include "../include/sane/config.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "../include/sane/sane.h"
#include "../include/sane/sanei.h"
#include "../include/sane/saneopts.h"
#include "../include/sane/sanei_config.h"
#include "../include/sane/sanei_usb.h"
#define BACKEND_NAME	oki
#include "../include/sane/sanei_backend.h"

/* ====================================================================
 * USB vendor ID
 * ==================================================================== */

#define OKI_VID  0x06BC

/* ====================================================================
 * Hardware spec types (geometry limits)
 * ==================================================================== */

enum oki_spec {
    SPEC_FX750,       /* A4 flatbed 216x297mm, Legal ADF 216x356mm */
    SPEC_FN2967LFB,   /* Legal flatbed 216x356mm, Legal ADF 216x356mm */
    SPEC_FX756,       /* A3 flatbed+ADF 297x420mm */
    SPEC_PX736,       /* A3 flatbed+ADF 297x420mm, USB-only */
};

struct oki_model {
    SANE_Word pid;
    const char *model_name;
    enum oki_spec spec;
};

/* Flatbed geometry per spec (mm) */
struct oki_geometry {
    double fb_w, fb_h;   /* flatbed width x height */
    double adf_w, adf_h; /* ADF width x height */
};

static const struct oki_geometry spec_geom[] = {
    [SPEC_FX750]     = { 216.0, 297.0, 216.0, 356.0 },
    [SPEC_FN2967LFB] = { 216.0, 356.0, 216.0, 356.0 },
    [SPEC_FX756]     = { 297.0, 420.0, 297.0, 420.0 },
    [SPEC_PX736]     = { 297.0, 420.0, 297.0, 420.0 },
};

/* ====================================================================
 * USB VID/PID device table (from driver.conf)
 * ==================================================================== */

static const struct oki_model oki_models[] = {
    /* fx750_fn2967 (A4 flatbed) */
    { 0x02BD, "MB461",        SPEC_FX750 },
    { 0x02BE, "MB471",        SPEC_FX750 },
    { 0x02BF, "MB491",        SPEC_FX750 },
    { 0x02CA, "ES4161 MFP",   SPEC_FX750 },
    { 0x02CB, "ES4191 MFP",   SPEC_FX750 },
    { 0x02D4, "MPS4200mb",    SPEC_FX750 },
    { 0x02EE, "MB451",        SPEC_FX750 },
    { 0x02EF, "MB441",        SPEC_FX750 },
    /* MC351 range */
    { 0x026F, "MC351",        SPEC_FX750 },
    { 0x0270, "MC351",        SPEC_FX750 },
    { 0x0271, "MC351",        SPEC_FX750 },
    { 0x0272, "MC351",        SPEC_FX750 },
    { 0x0273, "MC351",        SPEC_FX750 },
    { 0x0274, "ES3451 MFP",   SPEC_FX750 },
    { 0x0275, "MC351",        SPEC_FX750 },
    { 0x0276, "MC351",        SPEC_FX750 },
    { 0x0277, "MC351",        SPEC_FX750 },
    { 0x0278, "MC351",        SPEC_FX750 },
    /* MC361 range */
    { 0x0279, "MC361",        SPEC_FX750 },
    { 0x027A, "MC361",        SPEC_FX750 },
    { 0x027B, "MC361",        SPEC_FX750 },
    { 0x027C, "MC361",        SPEC_FX750 },
    { 0x027D, "MC361",        SPEC_FX750 },
    { 0x027E, "ES3461 MFP",   SPEC_FX750 },
    { 0x027F, "MC361",        SPEC_FX750 },
    { 0x0280, "MC361",        SPEC_FX750 },
    { 0x0281, "MC361",        SPEC_FX750 },
    { 0x0282, "MC361",        SPEC_FX750 },
    /* MC561 range */
    { 0x0283, "MC561",        SPEC_FX750 },
    { 0x0284, "MC561",        SPEC_FX750 },
    { 0x0285, "MC561",        SPEC_FX750 },
    { 0x0286, "MC561",        SPEC_FX750 },
    { 0x0287, "CX2731 MFP",   SPEC_FX750 },
    { 0x0288, "ES5461 MFP",   SPEC_FX750 },
    { 0x0289, "ES5461 MFP",   SPEC_FX750 },
    { 0x028A, "MC561",        SPEC_FX750 },
    { 0x028B, "MC561",        SPEC_FX750 },
    { 0x028C, "MC561",        SPEC_FX750 },
    /* MC352/362/562 */
    { 0x02E7, "MC352",        SPEC_FX750 },
    { 0x02E8, "MC362",        SPEC_FX750 },
    { 0x02E9, "MC562",        SPEC_FX750 },
    { 0x02EA, "ES3452 MFP",   SPEC_FX750 },
    { 0x02EB, "ES5462 MFP",   SPEC_FX750 },
    /* MB472/492/562 */
    { 0x034D, "MB472",        SPEC_FX750 },
    { 0x034E, "MB492",        SPEC_FX750 },
    { 0x034F, "MB562",        SPEC_FX750 },
    { 0x0350, "ES4192 MFP",   SPEC_FX750 },
    { 0x0351, "ES5162 MFP",   SPEC_FX750 },
    { 0x0377, "ES4172LP MFP", SPEC_FX750 },
    { 0x0378, "ES5162LP MFP", SPEC_FX750 },
    /* MC332/342 */
    { 0x0323, "MC332",        SPEC_FX750 },
    { 0x0324, "MC342",        SPEC_FX750 },
    { 0x0325, "MPS2731mc",    SPEC_FX750 },
    /* MC363/563/573 */
    { 0x0382, "MC363",        SPEC_FX750 },
    { 0x0383, "MC563",        SPEC_FX750 },
    { 0x0384, "ES5463 MFP",   SPEC_FX750 },
    { 0x0394, "MC573",        SPEC_FX750 },
    { 0x0395, "ES5473 MFP",   SPEC_FX750 },

    /* px736 (A3, USB-only) */
    { 0x01DB, "MC860",        SPEC_PX736 },
    { 0x01DC, "MC860",        SPEC_PX736 },
    { 0x01DD, "MC860",        SPEC_PX736 },
    { 0x01DE, "MC860",        SPEC_PX736 },
    { 0x01DF, "CX2633 MFP",   SPEC_PX736 },
    { 0x01E0, "ES8460 MFP",   SPEC_PX736 },
    { 0x021F, "ES8460 MFP",   SPEC_PX736 },
    { 0x02B4, "MC861",        SPEC_PX736 },
    { 0x02B5, "ES8461 MFP",   SPEC_PX736 },
    { 0x02B6, "MC851",        SPEC_PX736 },
    { 0x02B7, "ES8451 MFP",   SPEC_PX736 },
    { 0x02F7, "MC862",        SPEC_PX736 },
    { 0x02F8, "MC852",        SPEC_PX736 },
    { 0x02F9, "ES8462 MFP",   SPEC_PX736 },

    /* fn2967lfb (Legal flatbed) */
    { 0x02FE, "MB491+LP MFP", SPEC_FN2967LFB },
    { 0x02FF, "MB461+LP MFP", SPEC_FN2967LFB },
    { 0x0300, "MPS4700mb",    SPEC_FN2967LFB },

    /* fx756 (A3 flatbed+ADF) */
    { 0x035B, "MC853",        SPEC_FX756 },
    { 0x035C, "MC863",        SPEC_FX756 },
    { 0x035D, "MC873",        SPEC_FX756 },
    { 0x035E, "MC883",        SPEC_FX756 },
    { 0x035F, "ES8453 MFP",   SPEC_FX756 },
    { 0x0360, "ES8463 MFP",   SPEC_FX756 },
    { 0x0361, "ES8473 MFP",   SPEC_FX756 },
    { 0x0362, "ES8483 MFP",   SPEC_FX756 },

    { 0, NULL, 0 }
};

/* ====================================================================
 * PCScan protocol definitions
 * ==================================================================== */

/* VERIFY: opcodes and byte layouts need hardware confirmation. */

enum pcscan_opcode {
    PCSCAN_INQUIRY       = 0x01,
    PCSCAN_SET_WINDOW    = 0x02,
    PCSCAN_READ_DATA     = 0x03,
    PCSCAN_SEND_DATA     = 0x04,
    PCSCAN_SCAN          = 0x05,
    PCSCAN_MEDIA_CHECK   = 0x06,
    PCSCAN_REQUEST_SENSE = 0x07,
    PCSCAN_CALIBRATION   = 0x08,
    PCSCAN_CANCEL        = 0x09,
    PCSCAN_JOB_END       = 0x0A,
    PCSCAN_ACCESS_REQUEST= 0x0B,
    PCSCAN_INQUIRY_V2    = 0x0D,
};

/* SCAN_PARAMETER_BLOCK size (SET_WINDOW payload) */
/* VERIFY: exact size may vary; 49 bytes from constructor analysis */
#define SPB_SIZE  49

/* READ_DATA response header: 4 bytes status + image data */
#define READ_HDR_SIZE  4

/* USB I/O timeout (ms) */
#define USB_TIMEOUT  10000

/* Max read chunk from device */
#define MAX_READ_SIZE  65536

/* ====================================================================
 * SANE options
 * ==================================================================== */

enum oki_option {
    OPT_NUM_OPTS = 0,
    OPT_STD_GROUP,
    OPT_MODE,
    OPT_RESOLUTION,
    OPT_SOURCE,
    OPT_GEO_GROUP,
    OPT_TL_X,
    OPT_TL_Y,
    OPT_BR_X,
    OPT_BR_Y,
    NUM_OPTIONS
};

/* Scan mode: protocol values 0=Color, 1=Gray, 2=Lineart */
static const SANE_String_Const mode_list[] = {
    SANE_VALUE_SCAN_MODE_COLOR,
    SANE_VALUE_SCAN_MODE_GRAY,
    SANE_VALUE_SCAN_MODE_LINEART,
    NULL
};

/* Source: protocol 1=Flatbed, 2=ADF */
static const SANE_String_Const source_list[] = {
    SANE_I18N("Flatbed"),
    SANE_I18N("ADF"),
    NULL
};

/* Resolution word list: count + values */
static const SANE_Word res_list[] = {
    7,   /* count */
    75, 100, 150, 200, 300, 400, 600
};

/* ====================================================================
 * Device state
 * ==================================================================== */

struct oki_device {
    struct oki_device *next;
    SANE_Device sane;                /* name, vendor, model, type */
    SANE_String name;                /* allocated device name */
    int dn;                          /* sanei_usb device number, -1 if closed */
    const struct oki_model *model;

    SANE_Bool scanning, canceled;
    SANE_Parameters params;
    int scan_mode;                   /* 0=Color, 1=Gray, 2=Lineart */

    /* SANE options */
    SANE_Option_Descriptor opt[NUM_OPTIONS];
    SANE_Word val_num_opts;
    SANE_Word val_resolution;
    char val_mode[32];
    char val_source[32];
    SANE_Fixed val_tl_x, val_tl_y, val_br_x, val_br_y;
    SANE_Range range_x, range_y;

    /* I/O buffers */
    uint8_t cmd_buf[512];
    uint8_t data_buf[MAX_READ_SIZE];
    size_t data_len;    /* valid bytes in data_buf */
    size_t data_pos;    /* current read position */
    SANE_Bool eof_flag; /* device signaled end of data */
};

/* Global state */
static struct oki_device *first_dev = NULL;
static int num_devices = 0;
static const SANE_Device **devlist = NULL;

/* ====================================================================
 * Helpers: big-endian wire format
 * ==================================================================== */

static inline void
put_be16 (uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static inline void
put_be32 (uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

/* ====================================================================
 * USB I/O helpers
 * ==================================================================== */

/* Send a PCScan command and optionally read a response.
 * cmd_buf[0] = opcode, followed by param_len bytes of parameters.
 * If resp is non-NULL, read up to *resp_len bytes into it. */
static SANE_Status
oki_cmd (struct oki_device *dev, uint8_t opcode,
         const uint8_t *param, size_t param_len,
         uint8_t *resp, size_t *resp_len)
{
    SANE_Status status;
    size_t wsize;

    /* build command: opcode + params */
    dev->cmd_buf[0] = opcode;
    if (param && param_len > 0)
    {
        if (param_len > sizeof (dev->cmd_buf) - 1)
            return SANE_STATUS_NO_MEM;
        memcpy (dev->cmd_buf + 1, param, param_len);
    }
    wsize = 1 + param_len;

    DBG (4, "oki_cmd: opcode=0x%02x param_len=%zu\n", opcode, param_len);

    status = sanei_usb_write_bulk (dev->dn, dev->cmd_buf, &wsize);
    if (status != SANE_STATUS_GOOD)
    {
        DBG (1, "oki_cmd: write_bulk failed: %s\n",
             sane_strstatus (status));
        return status;
    }

    if (resp && resp_len && *resp_len > 0)
    {
        status = sanei_usb_read_bulk (dev->dn, resp, resp_len);
        if (status != SANE_STATUS_GOOD)
        {
            DBG (1, "oki_cmd: read_bulk failed: %s\n",
                 sane_strstatus (status));
            return status;
        }
        DBG (4, "oki_cmd: read %zu bytes\n", *resp_len);
    }

    return SANE_STATUS_GOOD;
}

/* Send ACCESS_REQUEST (0x0B) -- claim scanner for scanning */
static SANE_Status
oki_access_request (struct oki_device *dev)
{
    uint8_t resp[64];
    size_t rlen = sizeof (resp);

    DBG (3, "oki_access_request\n");
    /* VERIFY: ACCESS_REQUEST may need additional param bytes */
    return oki_cmd (dev, PCSCAN_ACCESS_REQUEST, NULL, 0, resp, &rlen);
}

/* Send SET_WINDOW (0x02) -- configure scan parameters */
static SANE_Status
oki_set_window (struct oki_device *dev)
{
    uint8_t spb[SPB_SIZE];
    uint8_t resp[64];
    size_t rlen = sizeof (resp);
    double tl_x_mm, tl_y_mm, br_x_mm, br_y_mm, w_mm, h_mm;
    uint16_t res;

    DBG (3, "oki_set_window: mode=%d res=%d\n",
         dev->scan_mode, dev->val_resolution);

    memset (spb, 0, sizeof (spb));

    res = (uint16_t) dev->val_resolution;

    tl_x_mm = SANE_UNFIX (dev->val_tl_x);
    tl_y_mm = SANE_UNFIX (dev->val_tl_y);
    br_x_mm = SANE_UNFIX (dev->val_br_x);
    br_y_mm = SANE_UNFIX (dev->val_br_y);
    if (tl_x_mm > br_x_mm) { double t = tl_x_mm; tl_x_mm = br_x_mm; br_x_mm = t; }
    if (tl_y_mm > br_y_mm) { double t = tl_y_mm; tl_y_mm = br_y_mm; br_y_mm = t; }
    w_mm = br_x_mm - tl_x_mm;
    h_mm = br_y_mm - tl_y_mm;

    /* VERIFY: all offsets below are from constructor disassembly */
    put_be16 (spb + 0x00, res);                       /* XResolution */
    put_be16 (spb + 0x02, res);                       /* YResolution */
    put_be16 (spb + 0x04, (uint16_t)(tl_x_mm));      /* TopLeftX */
    put_be16 (spb + 0x06, (uint16_t)(tl_y_mm));      /* TopLeftY */
    put_be16 (spb + 0x08, (uint16_t)(w_mm));          /* Width */
    put_be16 (spb + 0x0A, (uint16_t)(h_mm));          /* Height */
    spb[0x10] = (uint8_t) dev->scan_mode;             /* ColorMode */
    spb[0x12] = 0x01;                                 /* BitsPerSample/PlaneSeq */
    spb[0x14] = 0x64;                                 /* Brightness (100) */
    spb[0x15] = 0x80;                                 /* Contrast (128) */
    spb[0x16] = 0x64;                                 /* Sharpness (100) */

    /* Source: 0=Auto, 1=Flatbed, 2=ADF */
    if (strcmp (dev->val_source, "ADF") == 0)
        spb[0x17] = 0x02;
    else
        spb[0x17] = 0x01;

    spb[0x18] = 0x00;   /* Duplex off */
    spb[0x1A] = 0x3C;   /* VERIFY: unknown (60) */
    spb[0x1B] = 0x64;   /* VERIFY: unknown (100) */

    return oki_cmd (dev, PCSCAN_SET_WINDOW, spb, sizeof (spb), resp, &rlen);
}

/* Send SCAN (0x05) -- start scanning */
static SANE_Status
oki_start_scan (struct oki_device *dev)
{
    uint8_t resp[64];
    size_t rlen = sizeof (resp);

    DBG (3, "oki_start_scan\n");
    return oki_cmd (dev, PCSCAN_SCAN, NULL, 0, resp, &rlen);
}

/* Send READ_DATA (0x03) -- read a chunk of image data.
 * Returns image data in dev->data_buf, length in dev->data_len.
 * Sets dev->eof_flag when device signals end of data. */
static SANE_Status
oki_read_data (struct oki_device *dev)
{
    SANE_Status status;
    size_t wsize, rsize;
    uint8_t req[4];

    /* VERIFY: READ_DATA may need a requested-length parameter */
    put_be32 (req, MAX_READ_SIZE);
    wsize = 1 + sizeof (req);
    dev->cmd_buf[0] = PCSCAN_READ_DATA;
    memcpy (dev->cmd_buf + 1, req, sizeof (req));

    status = sanei_usb_write_bulk (dev->dn, dev->cmd_buf, &wsize);
    if (status != SANE_STATUS_GOOD)
    {
        DBG (1, "oki_read_data: write failed: %s\n",
             sane_strstatus (status));
        return status;
    }

    rsize = sizeof (dev->data_buf);
    status = sanei_usb_read_bulk (dev->dn, dev->data_buf, &rsize);
    if (status != SANE_STATUS_GOOD)
    {
        DBG (1, "oki_read_data: read failed: %s\n",
             sane_strstatus (status));
        return status;
    }

    /* VERIFY: the response format. We assume:
     * - First READ_HDR_SIZE bytes are a status/length header
     * - If rsize <= READ_HDR_SIZE, no more data (EOF)
     * - Image data starts at offset READ_HDR_SIZE */
    if (rsize <= READ_HDR_SIZE)
    {
        DBG (3, "oki_read_data: EOF (got %zu bytes)\n", rsize);
        dev->data_len = 0;
        dev->data_pos = 0;
        dev->eof_flag = SANE_TRUE;
        return SANE_STATUS_GOOD;
    }

    /* shift image data to start of buffer */
    dev->data_len = rsize - READ_HDR_SIZE;
    memmove (dev->data_buf, dev->data_buf + READ_HDR_SIZE, dev->data_len);
    dev->data_pos = 0;

    DBG (4, "oki_read_data: got %zu bytes of image data\n", dev->data_len);
    return SANE_STATUS_GOOD;
}

/* Send CANCEL (0x09) */
static SANE_Status
oki_cancel_scan (struct oki_device *dev)
{
    DBG (3, "oki_cancel_scan\n");
    return oki_cmd (dev, PCSCAN_CANCEL, NULL, 0, NULL, NULL);
}

/* Send JOB_END (0x0A) */
static SANE_Status
oki_job_end (struct oki_device *dev)
{
    DBG (3, "oki_job_end\n");
    return oki_cmd (dev, PCSCAN_JOB_END, NULL, 0, NULL, NULL);
}

/* ====================================================================
 * Model lookup
 * ==================================================================== */

static const struct oki_model *
find_model (SANE_Word pid)
{
    const struct oki_model *m;
    for (m = oki_models; m->model_name; m++)
    {
        if (m->pid == pid)
            return m;
    }
    return NULL;
}

/* ====================================================================
 * Option setup
 * ==================================================================== */

static int
mode_to_proto (const char *mode)
{
    if (strcmp (mode, SANE_VALUE_SCAN_MODE_COLOR) == 0)
        return 0;
    if (strcmp (mode, SANE_VALUE_SCAN_MODE_GRAY) == 0)
        return 1;
    if (strcmp (mode, SANE_VALUE_SCAN_MODE_LINEART) == 0)
        return 2;
    return -1;
}

static void
init_options (struct oki_device *dev)
{
    SANE_Option_Descriptor *od;
    const struct oki_geometry *geo;

    memset (dev->opt, 0, sizeof (dev->opt));

    geo = &spec_geom[dev->model->spec];

    /* Option count */
    od = &dev->opt[OPT_NUM_OPTS];
    od->name  = SANE_NAME_NUM_OPTIONS;
    od->title = SANE_TITLE_NUM_OPTIONS;
    od->desc  = SANE_DESC_NUM_OPTIONS;
    od->type  = SANE_TYPE_INT;
    od->cap   = SANE_CAP_SOFT_DETECT;
    od->size  = sizeof (SANE_Word);
    dev->val_num_opts = NUM_OPTIONS;

    /* Standard group */
    od = &dev->opt[OPT_STD_GROUP];
    od->name  = "";
    od->title = SANE_I18N("Standard");
    od->desc  = "";
    od->type  = SANE_TYPE_GROUP;
    od->cap   = 0;
    od->size  = 0;

    /* Mode */
    od = &dev->opt[OPT_MODE];
    od->name  = SANE_NAME_SCAN_MODE;
    od->title = SANE_TITLE_SCAN_MODE;
    od->desc  = SANE_DESC_SCAN_MODE;
    od->type  = SANE_TYPE_STRING;
    od->cap   = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    od->size  = 32;
    od->constraint_type = SANE_CONSTRAINT_STRING_LIST;
    od->constraint.string_list = mode_list;
    strcpy (dev->val_mode, SANE_VALUE_SCAN_MODE_COLOR);

    /* Resolution */
    od = &dev->opt[OPT_RESOLUTION];
    od->name  = SANE_NAME_SCAN_RESOLUTION;
    od->title = SANE_TITLE_SCAN_RESOLUTION;
    od->desc  = SANE_DESC_SCAN_RESOLUTION;
    od->type  = SANE_TYPE_INT;
    od->unit  = SANE_UNIT_DPI;
    od->cap   = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    od->size  = sizeof (SANE_Word);
    od->constraint_type = SANE_CONSTRAINT_WORD_LIST;
    od->constraint.word_list = res_list;
    dev->val_resolution = 150;

    /* Source */
    od = &dev->opt[OPT_SOURCE];
    od->name  = SANE_NAME_SCAN_SOURCE;
    od->title = SANE_TITLE_SCAN_SOURCE;
    od->desc  = SANE_DESC_SCAN_SOURCE;
    od->type  = SANE_TYPE_STRING;
    od->cap   = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    od->size  = 32;
    od->constraint_type = SANE_CONSTRAINT_STRING_LIST;
    od->constraint.string_list = source_list;
    strcpy (dev->val_source, "Flatbed");

    /* Geometry group */
    od = &dev->opt[OPT_GEO_GROUP];
    od->name  = "";
    od->title = SANE_I18N("Geometry");
    od->desc  = "";
    od->type  = SANE_TYPE_GROUP;
    od->cap   = 0;
    od->size  = 0;

    /* Set geometry ranges from spec */
    dev->range_x.min   = SANE_FIX (0.0);
    dev->range_x.max   = SANE_FIX (geo->fb_w);
    dev->range_x.quant = SANE_FIX (1.0);

    dev->range_y.min   = SANE_FIX (0.0);
    dev->range_y.max   = SANE_FIX (geo->fb_h);
    dev->range_y.quant = SANE_FIX (1.0);

    /* tl-x */
    od = &dev->opt[OPT_TL_X];
    od->name  = SANE_NAME_SCAN_TL_X;
    od->title = SANE_TITLE_SCAN_TL_X;
    od->desc  = SANE_DESC_SCAN_TL_X;
    od->type  = SANE_TYPE_FIXED;
    od->unit  = SANE_UNIT_MM;
    od->cap   = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    od->size  = sizeof (SANE_Fixed);
    od->constraint_type = SANE_CONSTRAINT_RANGE;
    od->constraint.range = &dev->range_x;
    dev->val_tl_x = SANE_FIX (0.0);

    /* tl-y */
    od = &dev->opt[OPT_TL_Y];
    od->name  = SANE_NAME_SCAN_TL_Y;
    od->title = SANE_TITLE_SCAN_TL_Y;
    od->desc  = SANE_DESC_SCAN_TL_Y;
    od->type  = SANE_TYPE_FIXED;
    od->unit  = SANE_UNIT_MM;
    od->cap   = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    od->size  = sizeof (SANE_Fixed);
    od->constraint_type = SANE_CONSTRAINT_RANGE;
    od->constraint.range = &dev->range_y;
    dev->val_tl_y = SANE_FIX (0.0);

    /* br-x */
    od = &dev->opt[OPT_BR_X];
    od->name  = SANE_NAME_SCAN_BR_X;
    od->title = SANE_TITLE_SCAN_BR_X;
    od->desc  = SANE_DESC_SCAN_BR_X;
    od->type  = SANE_TYPE_FIXED;
    od->unit  = SANE_UNIT_MM;
    od->cap   = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    od->size  = sizeof (SANE_Fixed);
    od->constraint_type = SANE_CONSTRAINT_RANGE;
    od->constraint.range = &dev->range_x;
    dev->val_br_x = SANE_FIX (geo->fb_w);

    /* br-y */
    od = &dev->opt[OPT_BR_Y];
    od->name  = SANE_NAME_SCAN_BR_Y;
    od->title = SANE_TITLE_SCAN_BR_Y;
    od->desc  = SANE_DESC_SCAN_BR_Y;
    od->type  = SANE_TYPE_FIXED;
    od->unit  = SANE_UNIT_MM;
    od->cap   = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    od->size  = sizeof (SANE_Fixed);
    od->constraint_type = SANE_CONSTRAINT_RANGE;
    od->constraint.range = &dev->range_y;
    dev->val_br_y = SANE_FIX (geo->fb_h);
}

/* Compute SANE_Parameters from current option values */
static void
compute_params (struct oki_device *dev)
{
    int mode;
    double res, tl_x, tl_y, br_x, br_y, w_mm, h_mm;
    uint32_t w_px, h_px;

    mode = mode_to_proto (dev->val_mode);
    if (mode < 0)
        mode = 0;

    res = (double) dev->val_resolution;
    tl_x = SANE_UNFIX (dev->val_tl_x);
    tl_y = SANE_UNFIX (dev->val_tl_y);
    br_x = SANE_UNFIX (dev->val_br_x);
    br_y = SANE_UNFIX (dev->val_br_y);

    if (tl_x > br_x) { double t = tl_x; tl_x = br_x; br_x = t; }
    if (tl_y > br_y) { double t = tl_y; tl_y = br_y; br_y = t; }

    w_mm = br_x - tl_x;
    h_mm = br_y - tl_y;
    w_px = (uint32_t)(w_mm * res / 25.4);
    h_px = (uint32_t)(h_mm * res / 25.4);

    dev->params.last_frame = SANE_TRUE;
    dev->params.lines = (SANE_Int) h_px;
    dev->params.pixels_per_line = (SANE_Int) w_px;

    switch (mode)
    {
    case 0: /* Color */
        dev->params.format = SANE_FRAME_RGB;
        dev->params.depth = 8;
        dev->params.bytes_per_line = dev->params.pixels_per_line * 3;
        break;
    case 1: /* Gray */
        dev->params.format = SANE_FRAME_GRAY;
        dev->params.depth = 8;
        dev->params.bytes_per_line = dev->params.pixels_per_line;
        break;
    case 2: /* Lineart */
        dev->params.format = SANE_FRAME_GRAY;
        dev->params.depth = 1;
        dev->params.bytes_per_line = (dev->params.pixels_per_line + 7) / 8;
        break;
    default: /* not reached; mode is clamped above */
        dev->params.format = SANE_FRAME_RGB;
        dev->params.depth = 8;
        dev->params.bytes_per_line = dev->params.pixels_per_line * 3;
        break;
    }
}

/* ====================================================================
 * Device attachment (called during sane_get_devices / sane_init)
 * ==================================================================== */

static SANE_Status
attach_one (const char *devname)
{
    struct oki_device *dev;
    SANE_Word vid, pid;
    SANE_Status status;
    const struct oki_model *model;
    SANE_Int dn;

    DBG (3, "attach_one: %s\n", devname);

    /* check if already attached */
    for (dev = first_dev; dev; dev = dev->next)
    {
        if (strcmp (dev->sane.name, devname) == 0)
        {
            DBG (3, "attach_one: already attached\n");
            return SANE_STATUS_GOOD;
        }
    }

    /* open temporarily to get VID/PID */
    status = sanei_usb_open (devname, &dn);
    if (status != SANE_STATUS_GOOD)
    {
        DBG (1, "attach_one: open failed: %s\n", sane_strstatus (status));
        return status;
    }

    status = sanei_usb_get_vendor_product (dn, &vid, &pid);
    sanei_usb_close (dn);

    if (status != SANE_STATUS_GOOD)
    {
        DBG (1, "attach_one: get_vendor_product failed\n");
        return status;
    }

    if (vid != OKI_VID)
    {
        DBG (3, "attach_one: not an OKI device (vid=0x%04x)\n", vid);
        return SANE_STATUS_GOOD;
    }

    model = find_model (pid);
    if (!model)
    {
        DBG (3, "attach_one: unknown OKI PID 0x%04x\n", pid);
        return SANE_STATUS_GOOD;
    }

    dev = calloc (1, sizeof (*dev));
    if (!dev)
        return SANE_STATUS_NO_MEM;

    dev->dn = -1;
    dev->model = model;
    dev->name = strdup (devname);
    dev->sane.name   = dev->name;
    dev->sane.vendor = "OKI";
    dev->sane.model  = model->model_name;
    dev->sane.type   = "multi-function peripheral";

    dev->next = first_dev;
    first_dev = dev;
    num_devices++;

    DBG (2, "attach_one: attached %s (%s) pid=0x%04x spec=%d\n",
         model->model_name, devname, pid, model->spec);

    return SANE_STATUS_GOOD;
}

/* ====================================================================
 * SANE API implementation
 * ==================================================================== */

SANE_Status
sane_init (SANE_Int *version_code, SANE_Auth_Callback authorize)
{
    FILE *fp;
    char config_line[PATH_MAX];

    (void) authorize;
    DBG_INIT ();
    DBG (2, "sane_init: oki backend (direct USB)\n");

    if (version_code)
        *version_code = SANE_VERSION_CODE (SANE_CURRENT_MAJOR,
                                           SANE_CURRENT_MINOR, 0);

    sanei_usb_init ();

    fp = sanei_config_open ("oki.conf");
    if (fp)
    {
        while (sanei_config_read (config_line, sizeof (config_line), fp))
        {
            if (config_line[0] == '#' || config_line[0] == '\0')
                continue;

            DBG (4, "sane_init: config line: %s\n", config_line);
            sanei_usb_attach_matching_devices (config_line, attach_one);
        }
        fclose (fp);
    }
    else
    {
        const struct oki_model *m;
        DBG (3, "sane_init: no config file, probing all known PIDs\n");
        for (m = oki_models; m->model_name; m++)
            sanei_usb_find_devices (OKI_VID, m->pid, attach_one);
    }

    return SANE_STATUS_GOOD;
}

void
sane_exit (void)
{
    struct oki_device *dev, *next;

    DBG (2, "sane_exit\n");

    for (dev = first_dev; dev; dev = next)
    {
        next = dev->next;
        if (dev->dn >= 0)
            sanei_usb_close (dev->dn);
        free (dev->name);
        free (dev);
    }
    first_dev = NULL;
    num_devices = 0;

    free (devlist);
    devlist = NULL;

    sanei_usb_exit ();
}

SANE_Status
sane_get_devices (const SANE_Device ***device_list,
                  SANE_Bool local_only)
{
    struct oki_device *dev;
    int i;

    (void) local_only;
    DBG (2, "sane_get_devices\n");

    free (devlist);
    devlist = calloc (num_devices + 1, sizeof (SANE_Device *));
    if (!devlist)
        return SANE_STATUS_NO_MEM;

    for (dev = first_dev, i = 0; dev; dev = dev->next, i++)
        devlist[i] = &dev->sane;
    devlist[i] = NULL;

    *device_list = devlist;
    return SANE_STATUS_GOOD;
}

SANE_Status
sane_open (SANE_String_Const devicename, SANE_Handle *handle)
{
    struct oki_device *dev;
    SANE_Status status;

    DBG (2, "sane_open: %s\n", devicename);

    /* empty name = open first device */
    if (!devicename[0])
    {
        dev = first_dev;
        if (!dev)
            return SANE_STATUS_INVAL;
    }
    else
    {
        for (dev = first_dev; dev; dev = dev->next)
        {
            if (strcmp (dev->sane.name, devicename) == 0)
                break;
        }
        if (!dev)
        {
            /* try attaching directly */
            status = attach_one (devicename);
            if (status != SANE_STATUS_GOOD)
                return status;
            /* find it */
            for (dev = first_dev; dev; dev = dev->next)
            {
                if (strcmp (dev->sane.name, devicename) == 0)
                    break;
            }
            if (!dev)
                return SANE_STATUS_INVAL;
        }
    }

    /* open USB connection */
    status = sanei_usb_open (dev->sane.name, &dev->dn);
    if (status != SANE_STATUS_GOOD)
    {
        DBG (1, "sane_open: usb_open failed: %s\n", sane_strstatus (status));
        return status;
    }

    sanei_usb_set_timeout (USB_TIMEOUT);
    status = sanei_usb_claim_interface (dev->dn, 0);
    if (status != SANE_STATUS_GOOD)
    {
        DBG (1, "sane_open: claim_interface failed: %s\n",
             sane_strstatus (status));
        /* non-fatal on some platforms */
    }

    init_options (dev);

    dev->scanning = SANE_FALSE;
    dev->canceled = SANE_FALSE;

    *handle = dev;
    return SANE_STATUS_GOOD;
}

void
sane_close (SANE_Handle handle)
{
    struct oki_device *dev = handle;

    DBG (2, "sane_close\n");

    if (dev->scanning)
    {
        oki_cancel_scan (dev);
        oki_job_end (dev);
    }

    if (dev->dn >= 0)
    {
        sanei_usb_release_interface (dev->dn, 0);
        sanei_usb_close (dev->dn);
        dev->dn = -1;
    }
    dev->scanning = SANE_FALSE;
}

const SANE_Option_Descriptor *
sane_get_option_descriptor (SANE_Handle handle, SANE_Int option)
{
    struct oki_device *dev = handle;

    if (option < 0 || option >= NUM_OPTIONS)
        return NULL;

    return &dev->opt[option];
}

SANE_Status
sane_control_option (SANE_Handle handle, SANE_Int option,
                     SANE_Action action, void *value, SANE_Int *info)
{
    struct oki_device *dev = handle;

    if (option < 0 || option >= NUM_OPTIONS)
        return SANE_STATUS_INVAL;

    if (info)
        *info = 0;

    if (action == SANE_ACTION_GET_VALUE)
    {
        switch (option)
        {
        case OPT_NUM_OPTS:
            *(SANE_Word *) value = dev->val_num_opts;
            break;
        case OPT_MODE:
            strcpy ((char *) value, dev->val_mode);
            break;
        case OPT_RESOLUTION:
            *(SANE_Word *) value = dev->val_resolution;
            break;
        case OPT_SOURCE:
            strcpy ((char *) value, dev->val_source);
            break;
        case OPT_TL_X:
            *(SANE_Fixed *) value = dev->val_tl_x;
            break;
        case OPT_TL_Y:
            *(SANE_Fixed *) value = dev->val_tl_y;
            break;
        case OPT_BR_X:
            *(SANE_Fixed *) value = dev->val_br_x;
            break;
        case OPT_BR_Y:
            *(SANE_Fixed *) value = dev->val_br_y;
            break;
        default:
            return SANE_STATUS_INVAL;
        }
        return SANE_STATUS_GOOD;
    }

    if (action == SANE_ACTION_SET_VALUE)
    {
        if (dev->scanning)
            return SANE_STATUS_DEVICE_BUSY;

        switch (option)
        {
        case OPT_MODE:
            strncpy (dev->val_mode, (const char *) value,
                     sizeof (dev->val_mode) - 1);
            dev->val_mode[sizeof (dev->val_mode) - 1] = '\0';
            if (info)
                *info |= SANE_INFO_RELOAD_PARAMS;
            break;
        case OPT_RESOLUTION:
            dev->val_resolution = *(SANE_Word *) value;
            if (info)
                *info |= SANE_INFO_RELOAD_PARAMS;
            break;
        case OPT_SOURCE:
            strncpy (dev->val_source, (const char *) value,
                     sizeof (dev->val_source) - 1);
            dev->val_source[sizeof (dev->val_source) - 1] = '\0';
            if (info)
                *info |= SANE_INFO_RELOAD_PARAMS;
            break;
        case OPT_TL_X:
            dev->val_tl_x = *(SANE_Fixed *) value;
            if (info)
                *info |= SANE_INFO_RELOAD_PARAMS;
            break;
        case OPT_TL_Y:
            dev->val_tl_y = *(SANE_Fixed *) value;
            if (info)
                *info |= SANE_INFO_RELOAD_PARAMS;
            break;
        case OPT_BR_X:
            dev->val_br_x = *(SANE_Fixed *) value;
            if (info)
                *info |= SANE_INFO_RELOAD_PARAMS;
            break;
        case OPT_BR_Y:
            dev->val_br_y = *(SANE_Fixed *) value;
            if (info)
                *info |= SANE_INFO_RELOAD_PARAMS;
            break;
        default:
            return SANE_STATUS_INVAL;
        }
        return SANE_STATUS_GOOD;
    }

    return SANE_STATUS_INVAL;
}

SANE_Status
sane_get_parameters (SANE_Handle handle, SANE_Parameters *params)
{
    struct oki_device *dev = handle;

    DBG (5, "sane_get_parameters\n");

    if (!dev->scanning)
        compute_params (dev);

    if (params)
        *params = dev->params;

    return SANE_STATUS_GOOD;
}

SANE_Status
sane_start (SANE_Handle handle)
{
    struct oki_device *dev = handle;
    SANE_Status status;

    DBG (2, "sane_start\n");

    if (dev->dn < 0)
        return SANE_STATUS_INVAL;

    dev->scan_mode = mode_to_proto (dev->val_mode);
    if (dev->scan_mode < 0)
        return SANE_STATUS_INVAL;

    dev->canceled = SANE_FALSE;
    dev->eof_flag = SANE_FALSE;
    dev->data_len = 0;
    dev->data_pos = 0;

    /* access request */
    status = oki_access_request (dev);
    if (status != SANE_STATUS_GOOD)
    {
        DBG (1, "sane_start: access_request failed\n");
        return status;
    }

    /* set scan parameters */
    status = oki_set_window (dev);
    if (status != SANE_STATUS_GOOD)
    {
        DBG (1, "sane_start: set_window failed\n");
        oki_job_end (dev);
        return status;
    }

    /* start scanning */
    status = oki_start_scan (dev);
    if (status != SANE_STATUS_GOOD)
    {
        DBG (1, "sane_start: start_scan failed\n");
        oki_job_end (dev);
        return status;
    }

    compute_params (dev);
    dev->scanning = SANE_TRUE;

    return SANE_STATUS_GOOD;
}

SANE_Status
sane_read (SANE_Handle handle, SANE_Byte *data,
           SANE_Int max_length, SANE_Int *length)
{
    struct oki_device *dev = handle;
    SANE_Status status;
    size_t to_copy;

    *length = 0;

    if (dev->canceled)
    {
        dev->scanning = SANE_FALSE;
        return SANE_STATUS_CANCELLED;
    }

    if (!dev->scanning)
        return SANE_STATUS_INVAL;

    /* if we have buffered data, copy from it */
    if (dev->data_pos < dev->data_len)
    {
        to_copy = dev->data_len - dev->data_pos;
        if (to_copy > (size_t) max_length)
            to_copy = (size_t) max_length;

        memcpy (data, dev->data_buf + dev->data_pos, to_copy);
        dev->data_pos += to_copy;
        *length = (SANE_Int) to_copy;
        return SANE_STATUS_GOOD;
    }

    /* no buffered data -- check if we've seen EOF */
    if (dev->eof_flag)
    {
        dev->scanning = SANE_FALSE;
        return SANE_STATUS_EOF;
    }

    /* read more data from device */
    status = oki_read_data (dev);
    if (status != SANE_STATUS_GOOD)
    {
        dev->scanning = SANE_FALSE;
        return status;
    }

    /* oki_read_data may have set eof_flag with no data */
    if (dev->data_len == 0 && dev->eof_flag)
    {
        dev->scanning = SANE_FALSE;
        return SANE_STATUS_EOF;
    }

    /* copy newly read data */
    to_copy = dev->data_len;
    if (to_copy > (size_t) max_length)
        to_copy = (size_t) max_length;

    memcpy (data, dev->data_buf, to_copy);
    dev->data_pos = to_copy;
    *length = (SANE_Int) to_copy;

    return SANE_STATUS_GOOD;
}

void
sane_cancel (SANE_Handle handle)
{
    struct oki_device *dev = handle;

    DBG (2, "sane_cancel\n");

    dev->canceled = SANE_TRUE;

    if (dev->scanning && dev->dn >= 0)
    {
        oki_cancel_scan (dev);
        oki_job_end (dev);
    }

    dev->scanning = SANE_FALSE;
}

SANE_Status
sane_set_io_mode (SANE_Handle handle, SANE_Bool non_blocking)
{
    (void) handle;
    if (non_blocking)
        return SANE_STATUS_UNSUPPORTED;
    return SANE_STATUS_GOOD;
}

SANE_Status
sane_get_select_fd (SANE_Handle handle, SANE_Int *fd)
{
    (void) handle;
    (void) fd;
    return SANE_STATUS_UNSUPPORTED;
}
