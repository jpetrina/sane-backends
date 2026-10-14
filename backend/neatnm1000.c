/* sane-neatnm1000.c --- SANE backend for the Neat NM-1000 (1f44:0001).
 *
 * Copyright (C) 2026 Pedro et al.
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
 * Sheetfed portable CIS (USB 1f44:0001), reverse-engineered from the
 * Windows driver USB traffic; no vendor firmware blob needed. Init
 * scripts (nm1000-ctrl-*.json) plus calibration blobs (nm1000-*-out.bin)
 * install to $(datadir)/sane/neatnm1000/.
 *
 * Protocol notes: IF0 FF/FF/FF, EP 0x81 bulk-IN (image/status), EP 0x02
 * bulk-OUT (cal); EP 0x83 interrupt-IN never used. Ordered EP0 vendor
 * init with cold-arm injection + 3055 paper gate, framed bulk-OUT cal,
 * tail region, 600 poll sweep, post-bulk vendors, paced strip loop
 * (paper-out + lid-dark end, drain-until-clean), verbatim close ritual
 * (post-image only). Five mode/DPI paths: gray 150/300/600, color
 * 150/300. Widths: gray150 1272 (=38160/30), gray300 2548, gray600 5100,
 * color 2548px line-sequential RGB triplets (driver renders planes).
 *
 * Never send CLEAR_FEATURE or USB reset (both wedge/drop the device);
 * fail cleanly with a replug message instead. sanei_usb_open() only
 * (re-)sets the configuration when the device is unconfigured, which
 * is exactly the cold/warm split this firmware needs, so no explicit
 * set_configuration is issued here.
 */
#include "../include/sane/config.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>

#include "../include/sane/sane.h"
#include "../include/sane/sanei.h"
#include "../include/sane/saneopts.h"
#include "../include/sane/sanei_usb.h"

#define BACKEND_NAME neatnm1000
#include "../include/sane/sanei_backend.h"
#include "../include/sane/sanei_config.h"

#ifndef SANE_I18N
#define SANE_I18N(text) text
#endif

#define NEATNM1000_CONFIG_FILE "neatnm1000.conf"
#define NEATNM1000_DATA_SUBDIR "neatnm1000"

#if defined(_WIN32) || defined(HAVE_OS2_H)
# define PATH_SEP       "\\"
#else
# define PATH_SEP       "/"
#endif

/* Debug level from sanei_init_debug */
static SANE_Int debug_level;

/* USB VID/PID (ground truth from descriptors.txt/lsusb). Bulk endpoints
 * are auto-detected by sanei_usb; EP 0x81 bulk-IN (image/status), EP 0x02
 * bulk-OUT (cal). */
#define NM1000_VID 0x1f44
#define NM1000_PID 0x0001

/* Open device (sanei_usb device number, -1 when closed) plus the device
 * name it was opened with (libusb:BBB:AAA form, used for the warm-session
 * stamp). Declared here so sane_open/sane_close can use them. */
static SANE_Int nm_dn = -1;
static char nm_devname[64] = "";
static int nm_open_count = 0;
/* nm_warm: same device as the last scan (no replug since). Warm runs
 * skip the boot handshake and the cold-arm block. Detected via a
 * bus:addr stamp file parsed from the "libusb:BBB:AAA" device name:
 * querying the configuration is useless (the kernel pre-configures the
 * device at enumeration, so it reads configured even on a fresh plug).
 * sanei_usb_open() itself only (re-)sets the configuration when the
 * device is unconfigured, matching the firmware's cold/warm split. */
static int nm_warm;
#define NM_STAMP_PATH "/tmp/.neatnm1000-session"
static void nm_msleep(int ms);
/* 1 if the plugged device matches the last-scanned one, else 0. */
static int nm_same_session(const char *devname)
{
    unsigned bus = 0, addr = 0;
    char cur[32], prev[32] = "";
    if (sscanf(devname, "libusb:%u:%u", &bus, &addr) != 2) return 0;
    snprintf(cur, sizeof(cur), "%u:%u", bus, addr);
    FILE *f = fopen(NM_STAMP_PATH, "r");
    if (f) {
        if (!fgets(prev, sizeof(prev), f)) prev[0] = 0;
        fclose(f);
        prev[strcspn(prev, "\r\n")] = 0;
    }
    return cur[0] && !strcmp(cur, prev);
}
/* Record the plugged device as last-scanned (best-effort). */
static void nm_stamp_session(const char *devname)
{
    unsigned bus = 0, addr = 0;
    if (sscanf(devname, "libusb:%u:%u", &bus, &addr) != 2) return;
    FILE *f = fopen(NM_STAMP_PATH, "w");
    if (f) {
        fprintf(f, "%u:%u\n", bus, addr);
        fclose(f);
        /* World-writable: frontends may run as different users
         * (scanimage vs saned); a private stamp would force cold. */
        chmod(NM_STAMP_PATH, 0666);
    }
}

typedef struct NeatNM1000_Scanner {
    struct NeatNM1000_Scanner *next;
    int open;
    SANE_Int resolution;   /* 150 | 300 | 600 */
    SANE_Bool color;       /* SANE_FALSE=gray, SANE_TRUE=color (300 only) */
    size_t image_size;
    size_t image_read;
    unsigned char *image;  /* rendered RGB or gray, row-packed */
    SANE_Parameters params;
} NeatNM1000_Scanner;

/* Option indices (must match Num_Options order). */
enum { OPT_COUNT = 0, OPT_RESOLUTION, OPT_MODE, OPT_NUM_OPTIONS };

static const SANE_String_Const mode_list[] = {"Gray", "Color", NULL};
static const SANE_Int res_list[] = {3, 150, 300, 600}; /* SANE word-list form */

/* Debug traces via SANE_DEBUG_NEATNM1000 (sanei_init_debug). The
 * protocol trace below uses NM_DBG for transfer-level chatter; user
 * action messages (feed paper, replug) stay on stderr. */
#define NM_DBG(...) DBG(5, __VA_ARGS__)

typedef struct NeatNM1000_Device
{
  struct NeatNM1000_Device *next;
  SANE_Device sane;
} NeatNM1000_Device;

static SANE_Int num_devices;
static NeatNM1000_Device *first_dev;
static NeatNM1000_Scanner *first_handle;
static const SANE_Device **devlist = 0;

/* Attach one device expanded from the config file ("usb VID PID" lines
 * and explicit devnames are both expanded by
 * sanei_usb_attach_matching_devices). Probes the VID/PID, then closes:
 * the device is re-opened by name in sane_open. */
static SANE_Status
attach_one_device (const char *devname)
{
  SANE_Word vendor, product;
  SANE_Int dn;
  SANE_Status status;
  NeatNM1000_Device *dev;

  DBG (4, "attach: trying device %s\n", devname);
  for (dev = first_dev; dev; dev = dev->next)
    if (strcmp (dev->sane.name, devname) == 0)
      return SANE_STATUS_GOOD;

  status = sanei_usb_open (devname, &dn);
  if (status != SANE_STATUS_GOOD)
    {
      DBG (1, "attach: sanei_usb_open failed: %s\n",
           sane_strstatus (status));
      return status;
    }
  status = sanei_usb_get_vendor_product (dn, &vendor, &product);
  sanei_usb_close (dn);
  if (status != SANE_STATUS_GOOD && status != SANE_STATUS_UNSUPPORTED)
    {
      DBG (1, "attach: sanei_usb_get_vendor_product failed: %s\n",
           sane_strstatus (status));
      return status;
    }
  if (status != SANE_STATUS_UNSUPPORTED
      && (vendor != NM1000_VID || product != NM1000_PID))
    {
      DBG (4, "attach: ignoring USB device 0x%04x/0x%04x\n", vendor, product);
      return SANE_STATUS_GOOD;
    }

  dev = malloc (sizeof (*dev));
  if (!dev)
    return SANE_STATUS_NO_MEM;
  memset (dev, 0, sizeof (*dev));
  dev->sane.name = strdup (devname);
  dev->sane.vendor = SANE_I18N ("Neat");
  dev->sane.model = SANE_I18N ("NM-1000 Mobile Scanner");
  dev->sane.type = SANE_I18N ("sheetfed portable CIS");
  if (!dev->sane.name)
    {
      free (dev);
      return SANE_STATUS_NO_MEM;
    }
  dev->next = first_dev;
  first_dev = dev;
  num_devices++;
  DBG (4, "attach: NM-1000 at %s\n", devname);
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_init (SANE_Int * version_code, SANE_Auth_Callback authorize)
{
  SANE_Char line[PATH_MAX];
  FILE *fp;

  DBG_INIT ();

#ifdef DBG_LEVEL
  debug_level = DBG_LEVEL;
#else
  debug_level = 0;
#endif

  DBG (2, "SANE neatnm1000 backend version %d.%d from %s\n",
       SANE_CURRENT_MAJOR, SANE_CURRENT_MINOR, PACKAGE_STRING);

  if (version_code)
    *version_code =
      SANE_VERSION_CODE (SANE_CURRENT_MAJOR, SANE_CURRENT_MINOR, 0);

  DBG (4, "sane_init: authorize %s null\n", authorize ? "!=" : "==");
  (void) authorize;

  sanei_usb_init ();

  num_devices = 0;
  first_dev = 0;
  first_handle = 0;
  devlist = 0;
  nm_dn = -1;
  nm_devname[0] = 0;
  nm_open_count = 0;
  nm_warm = 0;

  fp = sanei_config_open (NEATNM1000_CONFIG_FILE);
  if (!fp)
    {
      /* No config file: probe the known VID/PID directly. */
      DBG (3, "sane_init: no %s, probing usb 0x%04x 0x%04x directly\n",
           NEATNM1000_CONFIG_FILE, NM1000_VID, NM1000_PID);
      sanei_usb_attach_matching_devices ("usb 0x1f44 0x0001",
                                         attach_one_device);
      return SANE_STATUS_GOOD;
    }
  DBG (4, "sane_init: reading config file `%s'\n", NEATNM1000_CONFIG_FILE);
  while (sanei_config_read (line, sizeof (line), fp))
    {
      if (line[0] == '#')
        continue;               /* ignore comment lines */
      if (line[0] == '\0')
        continue;               /* ignore empty lines */
      DBG (4, "sane_init: trying to attach `%s'\n", line);
      sanei_usb_attach_matching_devices (line, attach_one_device);
    }
  fclose (fp);
  return SANE_STATUS_GOOD;
}

void
sane_exit (void)
{
  NeatNM1000_Device *dev, *next;

  DBG (4, "sane_exit\n");
  for (dev = first_dev; dev; dev = next)
    {
      next = dev->next;
      free ((void *) dev->sane.name);
      free (dev);
    }
  if (devlist)
    free (devlist);
  devlist = 0;
  first_dev = 0;
  num_devices = 0;
}

SANE_Status
sane_get_devices (const SANE_Device *** device_list, SANE_Bool local_only)
{
  NeatNM1000_Device *dev;
  SANE_Int i;

  DBG (4, "sane_get_devices: %d devices %s\n", num_devices,
       local_only ? "(local only)" : "");
  (void) local_only;
  if (devlist)
    free (devlist);

  devlist = malloc ((num_devices + 1) * sizeof (devlist[0]));
  if (!devlist)
    return SANE_STATUS_NO_MEM;

  i = 0;
  for (dev = first_dev; dev; dev = dev->next)
    devlist[i++] = &dev->sane;
  devlist[i] = 0;

  *device_list = devlist;
  DBG (5, "sane_get_devices: end\n");
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_open (SANE_String_Const devicename, SANE_Handle * handle)
{
  NeatNM1000_Device *dev;
  SANE_Status status;
  NeatNM1000_Scanner *s;

  if (!devicename)
    {
      DBG (1, "sane_open: devicename is null!\n");
      return SANE_STATUS_INVAL;
    }
  if (!handle)
    {
      DBG (1, "sane_open: handle is null!\n");
      return SANE_STATUS_INVAL;
    }
  DBG (4, "sane_open: devicename=%s\n", devicename);

  if (devicename[0])
    {
      for (dev = first_dev; dev; dev = dev->next)
        if (strcmp (dev->sane.name, devicename) == 0)
          break;
      if (!dev)
        {
          status = attach_one_device (devicename);
          if (status != SANE_STATUS_GOOD)
            return status;
          dev = first_dev;
        }
    }
  else
    dev = first_dev;            /* empty devicename: use first device */

  if (!dev)
    {
      DBG (1, "sane_open: %s doesn't seem to exist\n", devicename);
      return SANE_STATUS_INVAL;
    }

  s = malloc (sizeof (*s));
  if (!s)
    return SANE_STATUS_NO_MEM;
  memset (s, 0, sizeof (*s));
  s->open = 1;
  s->resolution = 300;
  s->color = SANE_FALSE;

  /* insert newly opened handle into list of open handles */
  s->next = first_handle;
  first_handle = s;

  if (nm_dn >= 0 && strcmp (nm_devname, dev->sane.name) != 0)
    {
      /* Different device while one is open: hand over. */
      sanei_usb_close (nm_dn);
      nm_dn = -1;
      nm_open_count = 0;
    }
  if (nm_dn < 0)
    {
      status = sanei_usb_open (dev->sane.name, &nm_dn);
      if (status != SANE_STATUS_GOOD)
        {
          DBG (1, "sane_open: couldn't open %s: %s\n", dev->sane.name,
               sane_strstatus (status));
          first_handle = s->next;
          free (s);
          nm_dn = -1;
          return status;
        }
      snprintf (nm_devname, sizeof (nm_devname), "%s", dev->sane.name);
    }
  nm_open_count++;
  /* NOTE: no explicit set_configuration and no CLEAR_FEATURE here.
   * sanei_usb_open() only (re-)sets the configuration when the device
   * is unconfigured, which is exactly the cold/warm split this firmware
   * needs (re-sending it reboots the fw USB engine and confuses the
   * cal-framing gate). Windows never sends CLEAR_FEATURE either;
   * clearing a healthy endpoint desyncs toggles. Never USB-reset a
   * confused NM-1000: a reset drops it off the bus (only a physical
   * replug recovers a deaf device). */
  nm_warm = nm_same_session (nm_devname);
  if (nm_warm)
    DBG (4, "sane_open: same device as last scan: warm session\n");
  nm_msleep (300);

  *handle = s;
  DBG (5, "sane_open: finished (handle=%p)\n", (void *) s);
  return SANE_STATUS_GOOD;
}

void
sane_close (SANE_Handle handle)
{
  NeatNM1000_Scanner *s = (NeatNM1000_Scanner *) handle;
  NeatNM1000_Scanner **prev;

  if (!s)
    return;
  for (prev = &first_handle; *prev; prev = &(*prev)->next)
    if (*prev == s)
      {
        *prev = s->next;
        break;
      }
  s->open = 0;
  free (s->image);
  s->image = NULL;
  s->image_size = s->image_read = 0;
  if (nm_dn >= 0 && --nm_open_count <= 0)
    {
      nm_open_count = 0;
      nm_stamp_session (nm_devname);
      sanei_usb_close (nm_dn);
      nm_dn = -1;
      nm_devname[0] = 0;
    }
  free (s);
  /* NOTE: no USB reset here: a reset re-enumerates the device (new bus
   * address) and breaks subsequent opens without a replug. */
}

const SANE_Option_Descriptor *sane_get_option_descriptor(SANE_Handle h, SANE_Int n)
{
    static SANE_Option_Descriptor opt[OPT_NUM_OPTIONS];
    static char res_name[] = "resolution", mode_name[] = "mode";
    static char res_title[] = "Resolution", mode_title[] = "Mode";
    static char res_desc[] = "Scan resolution in DPI (gray 150/300/600, color 150/300). "
        "Mode+DPI programmed via EP0 64B tables (tbl9 byte19, tbl11 byte47, tbl10 bytes33/35).";
    static char mode_desc[] = "Gray or Color (gray 150/300/600, color 150/300). "
        "Bulk 512B blocks are mode-independent; selector is EP0, not bulk.";
    (void)h;
    if (n < 0 || n >= OPT_NUM_OPTIONS) return NULL;
    memset(&opt[n], 0, sizeof(opt[n]));
    if (n == OPT_COUNT) {
        opt[n].name = ""; opt[n].title = ""; opt[n].desc = "";
        opt[n].type = SANE_TYPE_INT; opt[n].size = sizeof(SANE_Word);
        opt[n].cap = SANE_CAP_SOFT_DETECT;
    } else if (n == OPT_RESOLUTION) {
        opt[n].name = res_name; opt[n].title = res_title; opt[n].desc = res_desc;
        opt[n].type = SANE_TYPE_INT; opt[n].unit = SANE_UNIT_DPI;
        opt[n].size = sizeof(SANE_Word);
        opt[n].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
        opt[n].constraint_type = SANE_CONSTRAINT_WORD_LIST;
        opt[n].constraint.word_list = (SANE_Word *)res_list;
    } else {
        opt[n].name = mode_name; opt[n].title = mode_title; opt[n].desc = mode_desc;
        opt[n].type = SANE_TYPE_STRING; opt[n].size = 6;
        opt[n].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
        opt[n].constraint_type = SANE_CONSTRAINT_STRING_LIST;
        opt[n].constraint.string_list = mode_list;
    }
    return &opt[n];
}

SANE_Status sane_control_option(SANE_Handle h, SANE_Int n, SANE_Action a,
                                void *v, SANE_Int *info)
{
    NeatNM1000_Scanner *s = (NeatNM1000_Scanner *)h;
    if (info) *info = 0;
    if (a == SANE_ACTION_GET_VALUE) {
        if (n == OPT_COUNT) *(SANE_Word *)v = OPT_NUM_OPTIONS;
        else if (n == OPT_RESOLUTION) *(SANE_Word *)v = s->resolution;
        else if (n == OPT_MODE) strcpy((char *)v, s->color ? "Color" : "Gray");
        else return SANE_STATUS_INVAL;
        return SANE_STATUS_GOOD;
    }
    /* SET_VALUE */
    if (n == OPT_RESOLUTION) {
        SANE_Word r = *(SANE_Word *)v;
        if (r != 150 && r != 300 && r != 600) return SANE_STATUS_INVAL;
        if (s->color && r != 150 && r != 300) return SANE_STATUS_INVAL;
        if (!s->color && r != 150 && r != 300 && r != 600) return SANE_STATUS_INVAL;
        if (r == 600 && s->color) return SANE_STATUS_INVAL; /* no color600 */
        s->resolution = r; if (info) *info |= SANE_INFO_RELOAD_PARAMS;
        return SANE_STATUS_GOOD;
    }
    if (n == OPT_MODE) {
        if (!strcmp((char *)v, "Color")) {
            if (s->resolution != 150 && s->resolution != 300) return SANE_STATUS_INVAL;
            s->color = SANE_TRUE;
        } else if (!strcmp((char *)v, "Gray")) s->color = SANE_FALSE;
        else return SANE_STATUS_INVAL;
        if (info) *info |= SANE_INFO_RELOAD_PARAMS;
        return SANE_STATUS_GOOD;
    }
    return SANE_STATUS_INVAL;
}

SANE_Status sane_get_parameters(SANE_Handle h, SANE_Parameters *p)
{
    NeatNM1000_Scanner *s = (NeatNM1000_Scanner *)h;
    /* Widths are exact divisors from pcap analysis; depth 8 gray / 24 color. */
    int w = (s->resolution == 150) ? 1272 : (s->resolution == 600) ? 5100 :
        2548;  /* color300: 2548px line-seq RGB triplets (live-verified) */
    p->format = s->color ? SANE_FRAME_RGB : SANE_FRAME_GRAY;
    p->last_frame = SANE_TRUE;
    p->pixels_per_line = w;
    p->depth = 8;
    p->bytes_per_line = s->color ? w * 3 : w;
    /* Lines unknown until feed completes (sheetfed); report -1 (unknown). */
    p->lines = -1;
    s->params = *p;
    if (p) *p = s->params;
    return SANE_STATUS_GOOD;
}

/* ---- protocol port (pcap-verified full sequence) ----
 * Mirrors nm1000_scan.py: EP0 init from json, bulk OUT with
 * vendor-8200 interleaving, tail region, post-bulk vendor sequence,
 * exact-size paced strip loop with per-strip vendor IN/OUT. */

/* (nm_dn / nm_devname declared at top of file.) */

static void nm_msleep(int ms) { usleep((useconds_t)ms * 1000); }

/* Parse 8-byte setup hex into (bm,req,val,idx,wlen). Returns 0 ok. */
static int nm_parse_setup(const char *hex, uint8_t *bm, uint8_t *req,
                          uint16_t *val, uint16_t *idx, uint16_t *wlen)
{
    unsigned b[8];
    if (sscanf(hex, "%02x%02x%02x%02x%02x%02x%02x%02x",
               &b[0],&b[1],&b[2],&b[3],&b[4],&b[5],&b[6],&b[7]) != 8) return -1;
    *bm = (uint8_t)b[0]; *req = (uint8_t)b[1];
    *val = (uint16_t)(b[2] | (b[3] << 8));
    *idx = (uint16_t)(b[4] | (b[5] << 8));
    *wlen = (uint16_t)(b[6] | (b[7] << 8));
    return 0;
}

/* Hex string -> bytes. Returns byte count (or 0 on error). */
static size_t nm_hex2bin(const char *hex, unsigned char *out, size_t maxlen)
{
    size_t hlen = strlen(hex);
    if (hlen % 2 != 0 || hlen / 2 > maxlen) return 0;
    size_t n = hlen / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        if (sscanf(hex + 2 * i, "%02x", &v) != 1) return 0;
        out[i] = (unsigned char)v;
    }
    return n;
}

/* ---- sanei_usb transport ----
 * All transfers go through the open device nm_dn with a per-call timeout
 * (sanei_usb keeps one global timeout, so it is set before each call).
 * sanei_usb reports timeouts as SANE_STATUS_IO_ERROR with no success/
 * failure split, so bulk reads separate idle-silence from a dead device
 * with a cheap paper-sensor probe: a live firmware answers it. Control
 * transfers return 0 on success, -1 on any failure (the paper gate
 * already retries those). Bulk reads return bytes read, 0 on idle
 * timeout, -1 on hard error. */
static int
nm_ctl_out (uint8_t bm, uint8_t req, uint16_t val, uint16_t idx,
            const unsigned char *pay, int len, int timeout)
{
  SANE_Status st;

  if (nm_dn < 0)
    return -1;
  sanei_usb_set_timeout (timeout);
  st = sanei_usb_control_msg (nm_dn, bm, req, val, idx, len,
                              (SANE_Byte *) pay);
  if (st != SANE_STATUS_GOOD)
    {
      DBG (5, "nm_ctl_out %02x %02x %04x %04x %d: %s\n", bm, req, val, idx,
           len, sane_strstatus (st));
      return -1;
    }
  return 0;
}

static int
nm_ctl_in (uint8_t bm, uint8_t req, uint16_t val, uint16_t idx,
           unsigned char *buf, int len, int timeout)
{
  SANE_Status st;

  if (nm_dn < 0)
    return -1;
  sanei_usb_set_timeout (timeout);
  st = sanei_usb_control_msg (nm_dn, bm, req, val, idx, len, buf);
  if (st != SANE_STATUS_GOOD)
    {
      DBG (5, "nm_ctl_in %02x %02x %04x %04x %d: %s\n", bm, req, val, idx,
           len, sane_strstatus (st));
      return -1;
    }
  return len;
}

/* 1 while the firmware answers vendor INs, 0 otherwise. */
static int
nm_dev_alive (void)
{
  unsigned char tmp[2];

  return nm_ctl_in (0xc0, 0x04, 0x008e, 0x4022, tmp, sizeof (tmp), 300) >= 0;
}

static int
nm_bulk_out (const unsigned char *data, int len, int timeout)
{
  SANE_Status st;
  size_t size;

  if (nm_dn < 0)
    return -1;
  sanei_usb_set_timeout (timeout);
  size = (size_t) len;
  st = sanei_usb_write_bulk (nm_dn, data, &size);
  if (st != SANE_STATUS_GOOD || size != (size_t) len)
    {
      DBG (5, "nm_bulk_out %d: %s (wrote %lu)\n", len, sane_strstatus (st),
           (unsigned long) size);
      return -1;
    }
  return 0;
}

static int
nm_bulk_in (unsigned char *dst, int maxlen, int timeout)
{
  SANE_Status st;
  size_t size;

  if (nm_dn < 0)
    return -1;
  sanei_usb_set_timeout (timeout);
  size = (size_t) maxlen;
  st = sanei_usb_read_bulk (nm_dn, dst, &size);
  if (st == SANE_STATUS_GOOD)
    return (int) size;          /* may be a short packet: one strip */
  if (nm_dev_alive ())
    return 0;                   /* idle silence, not a dead device */
  DBG (1, "nm_bulk_in %d: device stopped answering: %s\n", maxlen,
       sane_strstatus (st));
  return -1;
}

/* Vendor OUT: setup_hex (8B) + payload_hex. Returns 0 ok, -1 fail. */
static int nm_vout(const char *setup_hex,
                   const char *payload_hex, int timeout)
{
    uint8_t bm, req; uint16_t val, idx, wlen;
    unsigned char payload[128];
    if (nm_parse_setup(setup_hex, &bm, &req, &val, &idx, &wlen) != 0) return -1;
    size_t blen = nm_hex2bin(payload_hex, payload, sizeof(payload));
    if (blen == 0 && payload_hex[0] != 0) return -1;
    return nm_ctl_out(bm, req, val, idx, payload, (int)blen, timeout);
}

/* Vendor IN: setup_hex, read len bytes into out. Returns bytes, -1 fail. */
static int nm_vin(const char *setup_hex,
                  unsigned char *out, int len, int timeout)
{
    uint8_t bm, req; uint16_t val, idx, wlen;
    (void)wlen;
    if (nm_parse_setup(setup_hex, &bm, &req, &val, &idx, &wlen) != 0) return -1;
    return nm_ctl_in(bm, req, val, idx, out, len, timeout);
}

/* Bulk OUT splits in captured order (verified against pcaps). */
static const size_t splits_gray150[] = {10808,10808,10808,512,512,512,32,16,2};
static const size_t splits_gray300[] = {10808,10808,10808,512,512,512,32,8,2};
static const size_t splits_color300[] = {10808,10808,10808,512,512,512,32,8,2};
static const size_t splits_gray600[] = {21608,21608,21608,512,512,512,32};

/* Vendor-8200 framing payloads preceding each bulk OUT
 * (setup 4004820001000800, wValue 0x0082 wIndex 0x0001). */
static const char *b8200_gray300[] = {"00400110382a0000","00a00110382a0000","00000210382a0000","0000000100020000","0002000100020000","0004000100020000","0000001020000000","0080001008000000","00c0001002000000"};
static const char *b8200_gray150[] = {"00400110382a0000","00a00110382a0000","00000210382a0000","0000000100020000","0002000100020000","0004000100020000","0000001020000000","0080001010000000","00c0001002000000"};
static const char *b8200_color300[] = {"00400110382a0000","00a00110382a0000","00000210382a0000","0000000100020000","0002000100020000","0004000100020000","0000001020000000","0080001008000000","00c0001002000000"};
static const char *b8200_gray600[] = {"0040011068540000","00c0011068540000","0040021068540000","0000000100020000","0002000100020000","0004000100020000","0000001020000000"};
static const char *b8200_color150[] = {"00400110382a0000","00a00110382a0000","00000210382a0000","0000000100020000","0002000100020000","0004000100020000","0000001020000000","0080001010000000","00c0001002000000"};
/* 600-DPI trailing tails NOT in the 66392 B blob (pcap 11902-11926):
 * framed 8 B b80b filler + framed 2 B dc05. Sent literally after the blob. */
static const char *nm_600_tail8200[] = {"0080001008000000","00c0001002000000"};
static const char *nm_600_tailbulk[] = {"b80bb80bb80bb80b","dc05"};

typedef struct {
    const char *json; const char *bin;
    const size_t *splits; int n_splits;
    const char **b8200;
    const char *post2e;   /* 46B 8300 payload after last bulk */
    const char *post1000; /* 16B 8300 payload */
    const char *tail2800; /* 40B tail-region vendor OUT (after last 512) */
    const char *tail0a00; /* 10B tail-region vendor OUT */
    const char *tail0400[3]; /* 0400 tail-region vendor OUTs (after BULK8) */
    const char *img8200;  /* 8B image framing (setup 4004820000000800) */
    int strip_big;        /* bulk IN dominant strip read (exact size) */
    int strip_small;      /* bulk IN strip tail read (exact size) */
    double cycle;           /* min strip cycle, s (Windows median; overrun
                               re-serves stale lines at 150: use 0.216) */
    size_t max_bytes;     /* safety cap: no-feed streams zeros forever */
    int is600;            /* gray600: 600 poll sweep + frame_first +
                             literal 8/2 tails */
} NmModeCfg;

static const NmModeCfg nm_modes[] = {
    { "nm1000-ctrl-gray150.json", "nm1000-150gray-out.bin",
      splits_gray150, 9, b8200_gray150,
      "02583854396021086454654160206100620069088f00900091009200936e950196a597e00a206a013d003e003f01",
      "02506a043d003e003f01350036013777", "0508c500c600bd00be47c700c800c900ca000414341f3000319f320a338f2c012d2c3d003e003f01", "0900a000250026112794", {"10021158","1204137e","1406158b"}, "0000001010950000", 37888, 272, 0.216, 4*1024*1024, 0 },
    { "nm1000-ctrl-gray300.json", "nm1000-300gray-out.bin",
      splits_gray300, 9, b8200_gray300,
      "0258382a39302104642a651160206100620069048f0090009100920093dc950196a597e00a206a013d003e003f01",
      "02506a043d003e003f013500360137dd", "0508c500c600bd00be47c700c800c900ca000414341f300031a7320a339b2c022d583d003e003f01", "0900a000250026232728", {"10021158","1204137e","1406158b"}, "00000010d4f80000", 63488, 212, 0.090, 8*1024*1024, 0 },
    { "nm1000-ctrl-color300.json", "nm1000-300color-out.bin",
      splits_color300, 9, b8200_color300,
      "0258380e39102104640d65f160206100620069048f009000910092029394950196a597e00a206a013d003e003f01",
      "02506a043d003e003f01350036053799", "0508c500c600bd00be47c700c800c900ca000410341f300031a7320a339b2c022d583d003e003f01", "0900a000250026692778", {"1006116e","120613a0","1406156e"}, "00000010e0ee0000", 60928, 224, 0.038, 40*1024*1024, 0 },
    { "nm1000-ctrl-gray600.json", "nm1000-600gray-out.bin",
      splits_gray600, 7, b8200_gray600,
      "02583817397021046417656b60206100620069048f0090009100920193b8950196a597e00a206a013d003e003f01",
      "02506a043d003e003f01350036073778", "0508c500c600bd00be07c700c800c900ca00041434053000319a321433862c022d583d003e003f01", "0900a000250026462750", {"10031108","120713c2","140c1545"}, "0000001010ef0000", 60928, 272, 0.039, 20*1024*1024, 1 },
    { "nm1000-ctrl-color150.json", "nm1000-150color-out.bin",
      splits_gray150, 9, b8200_color150,
      "0258381c39202108641c650160206100620069088f00900091009201934a950196a597e00a206a013d003e003f01",
      "02506a043d003e003f01350036013765", "0508c500c600bd00be47c700c800c900ca000410341f3000319f320a338f2c012d2c3d003e003f01", "0900a0002500263427bc", {"1006116e","120613a0","1406156e"}, "0000001068fd0000", 64512, 360, 0.216, 16*1024*1024, 0 },
};

static const NmModeCfg *nm_cfg_for(int res, int color)
{
    if (!color) {
        if (res == 150) return &nm_modes[0];
        if (res == 300) return &nm_modes[1];
        if (res == 600) return &nm_modes[3];
    } else if (res == 150) {
        return &nm_modes[4];
    } else if (res == 300) {
        return &nm_modes[2];
    }
    return NULL;
}

/* Small 8300 OUTs after post1000 (pcap order). NOTE: 0810 is sent
 * separately before post1000 above, not part of this list. */
static const char *nm_post_small_out[] =
    {"2010","9f00","0250","0850","a800","01a1","0fff","01e1"};
/* Vendor INs between image strips (setup + length). */
static const struct { const char *setup; int len; } nm_strip_ins[] = {
    {"c0048e0022400200",2},{"c0048e0022014000",64},{"c0048e0022404000",64},
    {"c0048e00227f4000",64},{"c0048e0022be4000",64},{"c0048e0022fd0400",4},
    {"c0048e0022410200",2}
};
/* Vendor INs of the post-bulk sequence (pcap order, no 402002 here). */
static const struct { const char *setup; int len; } nm_post_ins[] = {
    {"c0048e0022014000",64},{"c0048e0022404000",64},
    {"c0048e00227f4000",64},{"c0048e0022be4000",64},
    {"c0048e0022fd0400",4},{"c0048e0022410200",2},
    {"c0048e0022400300",3},{"c0048e0022420400",4}
};

/* ---- op-array init replay with cold-arm injection ----
 * Warm-derived scripts (150/600/color) skip r2 ops 5-22 (6e00-group +
 * 088f probe + zeros + ACK + a940/0600/0b01); a cold device then accepts
 * init but never steps the motor. Parse the JSON into ops, inject the
 * block r2-positioned when missing, replay. Also handles {"op":"bulk"}
 * (gray300's inline probe zeros), which the old streamer skipped. */
typedef struct { int kind; /*0=out,1=in,2=bulk*/ char setup[17]; int len;
                 unsigned char *pay; } NmOp;
static const char *nm_arm_prefix[5] = {
    "c0048e0022014000", "c0048e0022404000", "c0048e00227f4000",
    "c0048e0022be4000", "c0048e0022fd0400"};
static const struct { int kind; const char *setup; const char *pay; int len; }
nm_cold_arm[] = {
    {0,"4004830000000200","6e00",0},{0,"4004830000000200","a700",0},
    {0,"4004830000000200","6f00",0},{0,"4004830000000200","0650",0},
    {0,"4004830000000200","6c00",0},{0,"4004830000000200","6e02",0},
    {0,"4004830000000200","6c00",0},{1,"c0048e0022410200",NULL,2},
    {0,"4004830000000200","0b01",0},{0,"400c8c0010000100","0a",0},
    {0,"400c8c0013000100","0e",0},
    {0,"4004820001000800","088f001004000000",0},{2,NULL,"00000000",4},
    {1,"c00c8e000b000100",NULL,1},{0,"4004830000000200","a940",0},
    {0,"4004830000000600","51043a003b00",0},
    {0,"4004830000000200","0b01",0}};
#define NM_COLD_ARM_N ((int)(sizeof(nm_cold_arm)/sizeof(nm_cold_arm[0])))
/* Forward decls (implemented below sane_start). */
static void nm_close_scan(const NmModeCfg *cfg, int verbose);
static int nm_hexnib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Nth quoted string in blk: *s_out = content, *l_out = length. */
static int nm_hexbyte(char hi, char lo, unsigned char *out)
{
    int h = nm_hexnib(hi), l = nm_hexnib(lo);
    if (h < 0 || l < 0) return -1;
    *out = (unsigned char)((h << 4) | l);
    return 0;
}
/* Parse the ctrl JSON into an op array. Returns op count (<0 on error).
 * kind 0=out (pay/len), 1=in (len only), 2=bulk EP_OUT (pay/len). */
static int nm_parse_script(const char *json_path, NmOp **ops_out)
{
    FILE *f = fopen(json_path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *js = malloc(sz + 1);
    fread(js, 1, sz, f);
    js[sz] = 0;
    fclose(f);
    int cap = 256, n = 0;
    NmOp *ops = malloc(sizeof(NmOp) * cap);
    char *pos = js, *obj;
    while ((obj = strchr(pos, '{')) != NULL) {
        char *end = strchr(obj, '}');
        if (!end) break;
        size_t olen = (size_t)(end - obj) + 1;
        char *blk = malloc(olen + 1);
        memcpy(blk, obj, olen);
        blk[olen] = 0;
        pos = end + 1;
        char *opkey = strstr(blk, "\"op\"");
        NmOp op;
        memset(&op, 0, sizeof(op));
        int ok = 0, kind = -1;
        if (opkey) {
            char *v = strchr(opkey + 4, '"');
            if (v) {
                if (!strncmp(v + 1, "bulk\"", 5)) kind = 2;
                else if (!strncmp(v + 1, "in\"", 3)) kind = 1;
                else if (!strncmp(v + 1, "out\"", 4)) kind = 0;
            }
        }
        if (kind == 2) {
            char *lkey = strstr(blk, "\"len\"");
            char *dkey = strstr(blk, "\"data\"");
            if (lkey && dkey) {
                int blen = atoi(strchr(lkey, ':') + 1);
                char *ds;
                int dl;
                char *dq = strchr(dkey + 6, '"');
                if (dq && blen > 0 && blen <= 65536) {
                    ds = dq + 1;
                    dl = (int)(strchr(ds, '"') - ds);
                    if (dl >= 2 * blen) {
                        op.kind = 2;
                        op.len = blen;
                        op.pay = malloc(blen);
                        ok = 1;
                        for (int i = 0; i < blen; i++)
                            if (nm_hexbyte(ds[2*i], ds[2*i+1],
                                           &op.pay[i]) != 0) {
                                ok = 0;
                                break;
                            }
                    }
                }
            }
        } else {
            char *skey = strstr(blk, "\"setup\"");
            if (skey) {
                char *sq = strchr(skey + 7, '"');
                if (sq && strchr(sq + 1, '"') - (sq + 1) >= 16) {
                    memcpy(op.setup, sq + 1, 16);
                    if (kind == 0) {
                        char *pkey = strstr(blk, "\"payload\"");
                        if (pkey) {
                            char *pq = strchr(pkey + 9, '"');
                            if (pq) {
                                char *pe = strchr(pq + 1, '"');
                                int plen = pe ? (int)(pe - (pq + 1)) : 0;
                                if (plen >= 2 && !(plen % 2)) {
                                    op.kind = 0;
                                    op.len = plen / 2;
                                    op.pay = malloc(op.len);
                                    ok = 1;
                                    for (int i = 0; i < op.len; i++)
                                        if (nm_hexbyte((pq+1)[2*i],
                                                       (pq+1)[2*i+1],
                                                       &op.pay[i]) != 0) {
                                            ok = 0;
                                            break;
                                        }
                                }
                            }
                        }
                    } else if (kind == 1) {
                        char *lkey = strstr(blk, "\"len\"");
                        if (lkey) {
                            int rlen = atoi(strchr(lkey, ':') + 1);
                            if (rlen > 0 && rlen <= 4096) {
                                op.kind = 1;
                                op.len = rlen;
                                ok = 1;
                            }
                        }
                    }
                }
            }
        }
        free(blk);
        if (!ok) {
            free(op.pay);
            continue;
        }
        if (n >= cap) {
            cap *= 2;
            ops = realloc(ops, sizeof(NmOp) * cap);
        }
        ops[n++] = op;
    }
    free(js);
    *ops_out = ops;
    return n;
}
static void nm_free_script(NmOp *ops, int n);
static SANE_Status nm_replay_ops(NmOp *ops,
                                 int a, int b, int paper_wait, int verbose);
static SANE_Status nm_replay_cold_arm_from(int from, int verbose);
static SANE_Status nm_scan_full(const NmModeCfg *cfg,
                                const char *bin_path,
                                unsigned char **out_buf, size_t *out_len,
                                int verbose);

/* Full scan: EP0 init + bulk OUT with 8200 interleaving + post-bulk
 * vendors + exact-size strip loop. Zero image -> SANE_STATUS_NO_DOCS
 * (empty feeder; frontend may re-poll, not a hard failure). */
SANE_Status sane_start(SANE_Handle h)
{
    NeatNM1000_Scanner *s = (NeatNM1000_Scanner *)h;
    const NmModeCfg *cfg = nm_cfg_for(s->resolution, s->color ? 1 : 0);
    if (!cfg) return SANE_STATUS_INVAL;

    /* Resolve data dir: installed location first, then the source
     * tree and the current directory (developer runs). */
    const char *envdir = getenv("NEATNM1000_DATA_DIR");
    char pkgdir[512];
    snprintf(pkgdir, sizeof(pkgdir), "%s%s%s%s%s", PATH_SANE_DATA_DIR,
             PATH_SEP, "sane", PATH_SEP, NEATNM1000_DATA_SUBDIR);
    const char *search_dirs[] = {
        pkgdir,
        envdir ? envdir : "",
        "./backend/neatnm1000/",
        "./",
        NULL
    };
    char json_path[512] = "", bin_path[512] = "";
    for (int i=0; search_dirs[i]; i++) {
        char cand[512]; snprintf(cand,sizeof(cand),"%s%s%s",search_dirs[i],PATH_SEP,cfg->json);
        if (search_dirs[i][0] && access(cand,R_OK)==0) { strcpy(json_path,cand); break; }
    }
    for (int i=0; search_dirs[i]; i++) {
        char cand[512]; snprintf(cand,sizeof(cand),"%s%s%s",search_dirs[i],PATH_SEP,cfg->bin);
        if (search_dirs[i][0] && access(cand,R_OK)==0) { strcpy(bin_path,cand); break; }
    }
    NM_DBG("neatnm1000: json=%s bin=%s\n", json_path, bin_path);
    if (!json_path[0] || !bin_path[0]) {
        fprintf(stderr,"neatnm1000: missing ctrl/bin data (%s/%s)\n", cfg->json, cfg->bin);
        return SANE_STATUS_IO_ERROR;
    }

    /* sane_open owns the USB handle; start without one is a bug. */
    if (nm_dn < 0) {
        DBG (1, "sane_start: no open device\n");
        return SANE_STATUS_IO_ERROR;
    }

    /* Boot handshake (cold only): flush stale image bytes, then poll 8B
     * status on bulk-IN until 8 s quiet (cap 40 s). The feed only arms
     * after the firmware settles (cold r2: reports over ~19 s). Warm
     * sessions skip it (Windows never re-handshakes between scans). */
    if (nm_warm) {
        NM_DBG("neatnm1000: warm session, skipping boot handshake\n");
    } else
    {
        unsigned char dr[65536];
        for (int k = 0; k < 3; k++) {
            if (nm_bulk_in(dr, sizeof(dr), 500) < 0)
                break;
        }
        struct timespec bt0, btn;
        clock_gettime(CLOCK_MONOTONIC, &bt0);
        double last_nz = 0;
        int n_nz = 0;
        for (;;) {
            clock_gettime(CLOCK_MONOTONIC, &btn);
            double el = (btn.tv_sec - bt0.tv_sec) +
                        (btn.tv_nsec - bt0.tv_nsec) / 1e9;
            if (el >= 5.0 && el - last_nz >= 8.0) break;
            if (el >= 40.0) break;
            unsigned char rep[8];
            int x = nm_bulk_in(rep, sizeof(rep), 1200);
            if (x <= 0) continue;
            int live = 0, j;
            for (j = 0; j < x; j++)
                if (rep[j]) { live = 1; break; }
            if (live) {
                last_nz = el;
                n_nz++;
                NM_DBG("neatnm1000: boot status +%.1fs: %02x%02x%02x%02x...\n",
                       el, rep[0], rep[1], rep[2], rep[3]);
            }
        }
        NM_DBG("neatnm1000: boot settled (%d live reports)\n", n_nz);
    }

    /* 1a-c. Parse init script; the arm block lives at cops[5:22] when
     * present (injected for warm-derived scripts, inline for gray300:
     * 6e00 opener + 00000000 probe bulk nearby). Windows warm inits omit
     * it (neat-3x-backtoback.pcap: warm inits identical, zero arm
     * payloads); sending it warm deafens the fw. Probe the 6e00 opener
     * once with a short timeout: accepted = cold (send block), refused =
     * warm (skip block whole). Paper gate on the first three 400200
     * reads (3055 staged, else NO_DOCS). */
    NmOp *cops = NULL;
    int ncops = nm_parse_script(json_path, &cops);
    if (ncops <= 0) {
        fprintf(stderr,"neatnm1000: cannot parse %s\n", json_path);
        return SANE_STATUS_IO_ERROR;
    }
    /* need_arm: warm script (no inline probe bulk) whose head is the
     * 5-prefix-IN group and whose 6th op is not the 6e00 cold opener. */
    int has_probe = 0, head_ok = 0;
    for (int i = 0; i < ncops; i++)
        if (cops[i].kind == 2) has_probe = 1;  /* inline probe bulk */
    if (ncops >= 6) {
        head_ok = 1;
        for (int k = 0; k < 5; k++)
            if (cops[k].kind != 1 ||
                memcmp(cops[k].setup, nm_arm_prefix[k], 16)) head_ok = 0;
    }
    int need_arm = !has_probe && head_ok &&
        !(ncops > 5 && cops[5].kind == 0 && cops[5].pay &&
          cops[5].len == 2 && cops[5].pay[0] == 0x6e &&
          cops[5].pay[1] == 0x00);
    /* has_arm: arm block present at [5:22), injected or inline.
     * (OUT payload "6e00" parses to len 2 BYTES, not 1.) */
    int has_arm = 0;
    if (ncops > 30 && cops[5].kind == 0 && cops[5].pay &&
        cops[5].len == 2 && cops[5].pay[0] == 0x6e &&
        cops[5].pay[1] == 0x00) {
        for (int i = 5; i < 30; i++)
            if (cops[i].kind == 2 && cops[i].len == 4 && cops[i].pay &&
                !cops[i].pay[0] && !cops[i].pay[1] &&
                !cops[i].pay[2] && !cops[i].pay[3]) { has_arm = 1; break; }
    }
    int rest_at = need_arm ? 5 : 0;
    int warmed = 0; /* warm skip taken: gray300 rest is [22:27)+[30:) */
    SANE_Status st;
    fprintf(stderr,"neatnm1000: feed paper fully until it stops...\n");
    if (has_arm && !nm_warm) {
        /* Cold (fresh plug): send the arm block. need_arm replays the
         * separate table; gray300 carries it inline (rest from 5). */
        if (need_arm) {
            st = nm_replay_ops(cops, 0, 5, 0, 1);
            if (st != SANE_STATUS_GOOD) {
                nm_free_script(cops, ncops);
                return st;
            }
            st = nm_replay_cold_arm_from(0, 1);
            if (st != SANE_STATUS_GOOD) {
                nm_free_script(cops, ncops);
                return st;
            }
        }
        rest_at = 5;
        fprintf(stderr,"neatnm1000: feeder state: cold, arming...\n");
    } else if (has_arm) {
        /* Warm (stamp match, Windows verbatim): skip the block whole.
         * The separate arm table (need_arm) is simply not sent; resume
         * at orig[5] (0b09 lead). gray300's inline block occupies
         * cops[5:22]: send its 0b09/8c framing (ops 27-29) up front,
         * then resume at 22 but SKIP the duplicate 27-29 in the rest
         * (rest = [22:27) + [30:)). */
        uint8_t pbm, preq;
        uint16_t pval, pidx, pwlen;
        int prc;
        fprintf(stderr,"neatnm1000: feeder state: warm, "
                "skipping arm block...\n");
        if (!need_arm) {
            static const char *pre_s[] = {"4004830000000200",
                "400c8c0010000100", "400c8c0013000100"};
            static const unsigned char pre_p[][2] =
                {{0x0b, 0x09}, {0x0a}, {0x0e}};
            static const int pre_l[] = {2, 1, 1};
            for (int j = 0; j < 3; j++) {
                int k = 27 + j;
                if (cops[k].kind != 0 ||
                    memcmp(cops[k].setup, pre_s[j], 16) ||
                    cops[k].len != pre_l[j] ||
                    memcmp(cops[k].pay, pre_p[j], pre_l[j])) {
                    fprintf(stderr, "neatnm1000: warm framing moved "
                            "(op%d), aborting\n", k);
                    nm_free_script(cops, ncops);
                    return SANE_STATUS_IO_ERROR;
                }
                if (nm_parse_setup(cops[k].setup, &pbm, &preq, &pval,
                                   &pidx, &pwlen) != 0) {
                    nm_free_script(cops, ncops);
                    return SANE_STATUS_IO_ERROR;
                }
                prc = nm_ctl_out(pbm, preq, pval, pidx, cops[k].pay,
                                 cops[k].len, 2000);
                if (prc < 0) {
                    nm_free_script(cops, ncops);
                    return SANE_STATUS_IO_ERROR;
                }
            }
            rest_at = 22;
            warmed = 1;
        } else {
            rest_at = 5;
        }
    }
    if (warmed) {
        /* gray300 warm: [22:27), skip already-sent dup [27:30). */
        st = nm_replay_ops(cops, rest_at, 27, 1, 1);
        if (st != SANE_STATUS_GOOD) {
            nm_free_script(cops, ncops);
            return st;
        }
        st = nm_replay_ops(cops, 30, ncops, 1, 1);
    } else {
        st = nm_replay_ops(cops, rest_at, ncops, 1, 1);
    }
    nm_free_script(cops, ncops);
    if (st != SANE_STATUS_GOOD) return st;

    /* 2+3. Bulk OUT with 8200 framing, post-bulk vendors, strip loop. */
    unsigned char *buf = NULL; size_t len = 0;
    NM_DBG("neatnm1000: stage bulk+image %s\n", bin_path);
    st = nm_scan_full(cfg, bin_path, &buf, &len, 1);
    if (st == SANE_STATUS_NO_DOCS) {
        fprintf(stderr,"neatnm1000: no document (empty feeder). Feed paper and retry.\n");
        return SANE_STATUS_NO_DOCS;
    }
    if (st != SANE_STATUS_GOOD) { free(buf); return st; }
    if (len == 0) {
        free(buf);
        fprintf(stderr,"neatnm1000: zero bytes -> NO_DOCS (feed paper, retry)\n");
        return SANE_STATUS_NO_DOCS;
    }

    if (len > 0)
        nm_close_scan(cfg, 1);
    /* From here the firmware is post-close: any further start in this
     * session (or process) is warm, even if this open looked cold. */
    nm_warm = 1;
    free(s->image);
    s->image = buf;
    s->image_size = len;
    s->image_read = 0;

    int w = (s->resolution==150)?1272:(s->resolution==600)?5100:2548;
    size_t bpl = s->color ? (size_t)w*3 : (size_t)w;
    if (len % bpl != 0 && !(s->color && len % bpl ==4)) {
        fprintf(stderr,"neatnm1000: warning: raw %zu %% bpl %zu = %zu\n",
                len, bpl, len % bpl);
    }
    return SANE_STATUS_GOOD;
}

/* ---- op-array script parser + replayer (cold-arm aware) ---- */
/* Parse the ctrl JSON into an op array. Returns op count (<0 on error).
 * kind 0=out (pay/len), 1=in (len only), 2=bulk EP_OUT (pay/len). */
static void nm_free_script(NmOp *ops, int n)
{
    for (int i = 0; i < n; i++) free(ops[i].pay);
    free(ops);
}
/* Replay ops[a:b). paper_wait: first three 400200/2 reads wait 3055. */
static SANE_Status nm_replay_ops(NmOp *ops,
                                 int a, int b, int paper_wait, int verbose)
{
    int n_in = 0, n_out = 0, n4002 = 0;
    for (int i = a; i < b; i++) {
        NmOp *op = &ops[i];
        if (op->kind == 2) {
            int rc = nm_bulk_out(op->pay, op->len, 2000);
            if (verbose)
                fprintf(stderr, "  BULK OUT %d -> %d\n", op->len, rc);
            if (rc != 0) return SANE_STATUS_IO_ERROR;
            n_out++;
            nm_msleep(8);
            continue;
        }
        uint8_t bm, req;
        uint16_t val, idx, wlen;
        if (nm_parse_setup(op->setup, &bm, &req, &val, &idx, &wlen) != 0)
            return SANE_STATUS_IO_ERROR;
        if (op->kind == 0) {
            int rc = nm_ctl_out(bm, req, val, idx,
                                op->pay, op->len, 2000);
            if (verbose)
                fprintf(stderr, "  CTRL OUT %s %d -> %d\n", op->setup,
                        op->len, rc);
            if (rc < 0) return SANE_STATUS_IO_ERROR;
            n_out++;
            nm_msleep(8);
            continue;
        }
        int is_paper = (paper_wait && bm == 0xc0 && req == 0x04 &&
                        val == 0x008e && idx == 0x4022 && op->len == 2 &&
                        n4002 < 3);
        unsigned char *buf = malloc(op->len);
        int rc = -1, tries = 0;
        do {
            rc = nm_ctl_in(bm, req, val, idx, buf, op->len, 2000);
            if (!is_paper) break;
            if (rc >= 0 && buf[0] == 0x30 && buf[1] == 0x55) break;
            if (verbose)
                fprintf(stderr, "  paper poll %d: %02x%02x\n", tries,
                        (rc >= 0 ? buf[0] : 0), (rc >= 0 ? buf[1] : 0));
            nm_msleep(1000);
        } while (++tries < 10);
        if (verbose)
            fprintf(stderr, "  CTRL IN %s %d -> %d\n", op->setup,
                    op->len, rc);
        if (is_paper) {
            n4002++;
            if (!(rc >= 0 && buf[0] == 0x30 && buf[1] == 0x55)) {
                fprintf(stderr, "neatnm1000: no document (paper sensor "
                        "not staged). Feed paper and retry.\n");
                free(buf);
                return SANE_STATUS_NO_DOCS;
            }
        }
        free(buf);
        if (rc < 0) return SANE_STATUS_IO_ERROR;
        n_in++;
        nm_msleep(8);
    }
    if (verbose)
        fprintf(stderr, "  ctrl replay [%d:%d): %d OUT + %d IN\n", a, b,
                n_out, n_in);
    return SANE_STATUS_GOOD;
}
/* Cold-arm block (r2 ops 5-22) for warm-derived scripts, from index k. */
static SANE_Status nm_replay_cold_arm_from(int from, int verbose)
{
    for (int i = from; i < NM_COLD_ARM_N; i++) {
        int rc;
        if (nm_cold_arm[i].kind == 2) {
            unsigned char z[4] = {0, 0, 0, 0};
            (void)nm_cold_arm[i].pay;
            rc = nm_bulk_out(z, nm_cold_arm[i].len, 2000);
            if (verbose) fprintf(stderr, "  ARM BULK %d -> %d\n",
                                 nm_cold_arm[i].len, rc);
        } else if (nm_cold_arm[i].kind == 0) {
            rc = nm_vout(nm_cold_arm[i].setup, nm_cold_arm[i].pay,
                         2000);
            if (verbose) fprintf(stderr, "  ARM OUT %s %s -> %d\n",
                                 nm_cold_arm[i].setup,
                                 nm_cold_arm[i].pay, rc);
        } else {
            unsigned char tmp[64];
            rc = nm_vin(nm_cold_arm[i].setup, tmp,
                        nm_cold_arm[i].len, 2000);
            if (verbose) fprintf(stderr, "  ARM IN %s -> %d %02x%02x\n",
                                 nm_cold_arm[i].setup, rc, tmp[0],
                                 nm_cold_arm[i].len > 1 ? tmp[1] : 0);
        }
        if (rc < 0) return SANE_STATUS_IO_ERROR;
        nm_msleep(8);
    }
    return SANE_STATUS_GOOD;
}

/* Bulk OUT calibration with vendor-8200 framing between chunks.
 * The FIRST framing (004001 before the first cal block) is already the
 * tail op of the init JSON replay -- re-sending it here wedges the fw
 * (EP0 goes deaf), so framing starts at i>=1, exactly like the pcap.
 * Timeout on a cal block -> empty feeder. */
static SANE_Status nm_bulk_out_framed(const NmModeCfg *cfg,
                                      const unsigned char *blob, size_t total,
                                      size_t *consumed, int verbose)
{
    size_t off = 0;
    int n_head = cfg->n_splits > 6 ? 6 : cfg->n_splits;
    for (int i = 0; i < n_head; i++) {
        size_t ln = cfg->splits[i];
        if (off + ln > total) return SANE_STATUS_INVAL;
        /* First framing already sent as init-JSON tail -- except gray600,
         * whose JSON ends mid-warmup (use is600 as the frame_first flag). */
        if (i > 0 || cfg->is600) {
            int rc = nm_vout("4004820001000800", cfg->b8200[i], 2000);
            if (rc < 0) {
                if (verbose) fprintf(stderr,"  8200 before bulk %d failed %d\n", i, rc);
                return SANE_STATUS_IO_ERROR;
            }
            nm_msleep(10);
        }
        int brc = nm_bulk_out((unsigned char *)blob + off, (int)ln, 5000);
        if (verbose) fprintf(stderr,"  BULK OUT %d: %zu B -> rc %d\n",
                             i, ln, brc);
        /* Paper was staged (gate passed), so a cal stall is a fw wedge,
         * not NO_DOCS. NOTE: no clear_halt: Windows never sends
         * CLEAR_FEATURE and clearing a healthy endpoint desyncs
         * toggles. */
        if (brc != 0) return SANE_STATUS_IO_ERROR;
        off += ln;
        nm_msleep(10);
    }
    *consumed = off;
    return SANE_STATUS_GOOD;
}

/* Ordered tail region (after last 512, before 2e00): 3 vendor INs,
 * 2800/0a00/1000/9d00 OUTs, framed BULK32, framed BULK8/16, 0600/0400s/
 * 0200s OUTs, framed BULK2. Consumes blob splits[6..]; 600's 8/2 tails
 * are literal (outside its blob). *consumed updated. */
static SANE_Status nm_tail_region(const NmModeCfg *cfg,
                                  const unsigned char *blob, size_t total,
                                  size_t *consumed, int verbose)
{
    unsigned char tmp[64];
    size_t off = *consumed;
    static const char *tail_small[] = {"1000", "9d00"};
    static const char *tail_mid[] =
        {"0d01", "0d00", "0d10", "6b81", "6320"};
    nm_vin("c0048e0022410200", tmp, 2, 2000); nm_msleep(8);
    nm_vin("c00c8e001c000100", tmp, 1, 2000); nm_msleep(8);
    nm_vin("c0048e0022480400", tmp, 4, 2000); nm_msleep(8);
    if (nm_vout("4004830000002800", cfg->tail2800, 2000) < 0)
        return SANE_STATUS_IO_ERROR;
    nm_msleep(8);
    if (nm_vout("4004830000000a00", cfg->tail0a00, 2000) < 0)
        return SANE_STATUS_IO_ERROR;
    nm_msleep(8);
    for (int i = 0; i < 2; i++) {
        if (nm_vout("4004830000000200", tail_small[i], 2000) < 0)
            return SANE_STATUS_IO_ERROR;
        nm_msleep(8);
    }
    /* BULK32 from blob (all modes have splits[6]). */
    if (off + cfg->splits[6] > total) return SANE_STATUS_INVAL;
    if (nm_vout("4004820001000800", cfg->b8200[6], 2000) < 0)
        return SANE_STATUS_IO_ERROR;
    nm_msleep(10);
    {
        if (nm_bulk_out((unsigned char *)blob + off,
                        (int)cfg->splits[6], 5000) != 0)
            return SANE_STATUS_IO_ERROR;
    }
    off += cfg->splits[6];
    nm_msleep(10);
    /* BULK8/16: blob for 9-split modes, literal for 600. */
    if (cfg->is600) {
        unsigned char tb[8];
        size_t blen = nm_hex2bin(nm_600_tailbulk[0], tb, sizeof(tb));
        if (nm_vout("4004820001000800", nm_600_tail8200[0],
                    2000) < 0) return SANE_STATUS_IO_ERROR;
        nm_msleep(10);
        if (nm_bulk_out(tb, (int)blen, 5000) != 0)
            return SANE_STATUS_IO_ERROR;
        nm_msleep(10);
    } else {
        if (nm_vout("4004820001000800", cfg->b8200[7], 2000) < 0)
            return SANE_STATUS_IO_ERROR;
        nm_msleep(10);
        if (off + cfg->splits[7] > total) return SANE_STATUS_INVAL;
        if (nm_bulk_out((unsigned char *)blob + off,
                        (int)cfg->splits[7], 5000) != 0)
            return SANE_STATUS_IO_ERROR;
        off += cfg->splits[7];
        nm_msleep(10);
    }
    if (nm_vout("4004830000000600", "3d003e003f01", 2000) < 0)
        return SANE_STATUS_IO_ERROR;
    nm_msleep(8);
    for (int i = 0; i < 3; i++) {
        if (nm_vout("4004830000000400", cfg->tail0400[i],
                    2000) < 0) return SANE_STATUS_IO_ERROR;
        nm_msleep(8);
    }
    for (int i = 0; i < 5; i++) {
        if (nm_vout("4004830000000200", tail_mid[i], 2000) < 0)
            return SANE_STATUS_IO_ERROR;
        nm_msleep(8);
    }
    /* BULK2: blob for 9-split modes, literal for 600. */
    if (cfg->is600) {
        unsigned char tb[8];
        size_t blen = nm_hex2bin(nm_600_tailbulk[1], tb, sizeof(tb));
        if (nm_vout("4004820001000800", nm_600_tail8200[1],
                    2000) < 0) return SANE_STATUS_IO_ERROR;
        nm_msleep(10);
        if (nm_bulk_out(tb, (int)blen, 5000) != 0)
            return SANE_STATUS_IO_ERROR;
        nm_msleep(10);
    } else {
        if (nm_vout("4004820001000800", cfg->b8200[8], 2000) < 0)
            return SANE_STATUS_IO_ERROR;
        nm_msleep(10);
        if (off + cfg->splits[8] > total) return SANE_STATUS_INVAL;
        if (nm_bulk_out((unsigned char *)blob + off,
                        (int)cfg->splits[8], 5000) != 0)
            return SANE_STATUS_IO_ERROR;
        off += cfg->splits[8];
        nm_msleep(10);
    }
    *consumed = off;
    if (verbose) fprintf(stderr,"  tail region done (blob off %zu/%zu)\n",
                         off, total);
    return SANE_STATUS_GOOD;
}

/* Post-bulk vendor sequence (pcap idx 548-593): 2e, 0f01, 6b01, 4030 poll,
 * 0810, 1000-table, small 8300s, 7 vendor INs + 4030/4240, image 8200. */
static SANE_Status nm_post_bulk(const NmModeCfg *cfg,
                               int verbose)
{
    int rc;
    unsigned char tmp[64];
    /* NOTE: the 0600/0400s/0200s preamble lives in nm_tail_region()
     * (between BULK8 and BULK2, per pcap), not here. */
    rc = nm_vout("4004830000002e00", cfg->post2e, 2000);
    if (rc < 0) return SANE_STATUS_IO_ERROR;
    rc = nm_vout("4004830000000200", "0f01", 2000);
    if (rc < 0) return SANE_STATUS_IO_ERROR;
    rc = nm_vout("4004830000000200", "6b01", 2000);
    if (rc < 0) return SANE_STATUS_IO_ERROR;
    rc = nm_vin("c0048e0022400300", tmp, 3, 1000);
    if (rc < 0) return SANE_STATUS_IO_ERROR;
    rc = nm_vout("4004830000000200", "0810", 2000);
    if (rc < 0) return SANE_STATUS_IO_ERROR;
    rc = nm_vout("4004830000001000", cfg->post1000, 2000);
    if (rc < 0) return SANE_STATUS_IO_ERROR;
    for (int i = 0; i < 8; i++) {
        rc = nm_vout("4004830000000200", nm_post_small_out[i], 2000);
        if (rc < 0) return SANE_STATUS_IO_ERROR;
    }
    for (int i = 0; i < 8; i++) {
        rc = nm_vin(nm_post_ins[i].setup, tmp, nm_post_ins[i].len, 2000);
        if (rc < 0) return SANE_STATUS_IO_ERROR;
    }
    rc = nm_vout("4004820000000800", cfg->img8200, 2000);
    if (rc < 0) return SANE_STATUS_IO_ERROR;
    if (verbose) fprintf(stderr,"  post-bulk vendors ok\n");
    return SANE_STATUS_GOOD;
}

/* Windows-faithful end-of-scan ritual (verbatim 150/300/color;
 * 600 = short tail only). Best-effort: never fails the scan. Without it
 * the fw holds the scan context and the next init wedges. */
static void nm_close_scan(const NmModeCfg *cfg,
                          int verbose)
{
    unsigned char tmp[64];
#define NM_CO(setup, pay) do { nm_vout(setup, pay, 1500); } while (0)
#define NM_CI(setup, ln) do { \
        if (nm_vin(setup, tmp, ln, 1500) >= 0 && verbose) \
            fprintf(stderr, "  close %s -> %02x%02x\n", setup, tmp[0], \
                    (ln) > 1 ? tmp[1] : 0); \
    } while (0)
    static const char *ctbl[] = {
        "01a002700392041005000650081009000a000c000b090d001000110012001300"
        "1400150016001701180419e01a261b001c001d041e101f012010210422502350",
        "24042500260027002c022d582e802f8030003114322733ec343c350036403700"
        "3817396f3d003e003f015211530254055508560b570e588b59005ac05e885f01",
        "6600673f683f69016a046b016e6f6c616f006d00700471067207730874007500"
        "76007700780079fc7a037bff7cff87009d049e009f00a20fa800a940ab30ad00",
    };
    static const char *creg[] = {
        "c0048e0022014000", "c0048e0022404000", "c0048e00227f4000",
        "c0048e0022be4000", "c0048e0022fd0400", "c0048e0022410200",
    };
    static const int cregln[] = {64, 64, 64, 64, 4, 2};
    static const char *couts[] = {"6e6f", "7e00", "0312", "0392", "6f00",
                                  "6d1f", "6b01"};
    if (!cfg->is600) {
        unsigned char b4[4];
        nm_vout("4004820000000800", "0000000104000000", 1500);
        if (nm_bulk_in(b4, 4, 1500) == 4 && verbose)
            fprintf(stderr, "  close BIN4 -> %02x%02x%02x%02x\n", b4[0],
                    b4[1], b4[2], b4[3]);
        NM_CO("4004830000000200", "0250");
        NM_CO("4004830000000200", "20ff");
        NM_CO("4004830000000600", "350036003700");
        NM_CO("4004830000000200", "01e0");
        NM_CI("c0048e0022410200", 2);
        NM_CI("c0048e0022480400", 4);
        NM_CO("4004830000000200", "0a00");
        NM_CO("4004830000000200", "01a0");
        for (int k = 0; k < 6; k++) NM_CI(creg[k], cregln[k]);
        NM_CI("c0048e00224b0400", 4);
        for (int k = 0; k < 6; k++) NM_CI(creg[k], cregln[k]);
        for (int k = 0; k < 7; k++)
            NM_CO("4004830000000200", couts[k]);
        NM_CO("4004830000000200", "0b09");
        NM_CO("400c8c0010000100", "0a");
        NM_CO("400c8c0013000100", "0e");
        for (int k = 0; k < 3; k++)
            NM_CO("4004830000004000", ctbl[k]);
        NM_CO("4004830000000800", "ae3fb800bd00be00");
        NM_CO("4004830000003600",
              "51013a003b3551023a003b8051033a003b0451203a003b8051213a"
              "003b8051223a003b8051283a003bbb51293a003b4b512a3a003b4b");
        NM_CI("c004840006e20400", 4);
        for (int k = 0; k < 6; k++) NM_CI(creg[k], cregln[k]);
        NM_CI("c0048e00220a0200", 2);
    }
    NM_CO("4004830000000200", "0a20");
    NM_CI("c0048e0022400200", 2);
    NM_CO("4004830000000200", "0312");
    NM_CO("4004830000000200", "0392");
    NM_CO("4004830000000200", "6f00");
    NM_CO("4004830000000200", "6d1f");
    NM_CO("4004830000000200", "6b01");
    if (verbose) fprintf(stderr, "  close ritual done\n");
#undef NM_CO
#undef NM_CI
}

/* Strip vendor block between image reads (7 INs + image 8200 OUT).
 * Returns the 400200 paper byte pair packed as int (or -1): 0x7335 =
 * feeding paper-out states are 7355/7055 (done) and 3955 (150-DPI done). */
static int nm_strip_vendors(const NmModeCfg *cfg)
{
    unsigned char tmp[64];
    int paper = -1;
    for (int i = 0; i < 7; i++) {
        if (nm_vin(nm_strip_ins[i].setup, tmp, nm_strip_ins[i].len,
                   800) < 0)
            return -1;
        if (i == 0 && nm_strip_ins[i].len == 2)
            paper = (tmp[0] << 8) | tmp[1];
    }
    nm_vout("4004820000000800", cfg->img8200, 800);
    return paper;
}

/* 600-DPI pre-cal sensor sweep (pcap ops ~208-8738, 704 cycles).
 * Header once, then 704x [8B row-address OUT | 2B OUT | 2B IN | 256B
 * IN]. Bulk-OUT framer setup = 4004820001000800, bulk-IN framer =
 * 4004820000000800. Addresses open-loop (k*0x20); IN data discarded.
 * Live-verified (s600a 5100x3756). */
static int nm_600_out(const char *framer,
                      const unsigned char *data, int len, int timeout)
{
    if (nm_vout("4004820001000800", framer, timeout) < 0) return -1;
    if (nm_bulk_out(data, len, timeout) != 0) return -1;
    return 0;
}
static int nm_600_in(const char *framer,
                     unsigned char *dst, int want, int timeout)
{
    if (nm_vout("4004820000000800", framer, timeout) < 0) return -1;
    int got = 0;
    while (got < want) {
        int r = nm_bulk_in(dst + got, want - got, timeout);
        if (r < 0) return -1;
        if (r == 0) break;      /* idle: short sweep read ends here */
        got += r;
    }
    return got;
}
static SANE_Status nm_poll600(int verbose)
{
    unsigned char tmp[256];
    /* The init script's last op already sent framer 0000000302000000,
     * so complete it first, then the rest verbatim. */
    {
        unsigned char b04[1] = {0x04};
        if (nm_bulk_out(b04, 1, 5000) != 0) return SANE_STATUS_IO_ERROR;
    }
    static const struct { const char *fr; const char *data; } hdr[] = {
        {"0200000302000000", "0100"}, {"0800000302000000", "0208"},
        {"0a00000302000000", "9f9f"}, {"0c00000302000000", "9f9f"},
        {"0400000302000000", "1000"},
    };
    for (int i = 0; i < 5; i++) {
        unsigned char d[2];
        if (nm_hex2bin(hdr[i].data, d, sizeof(d)) != 2)
            return SANE_STATUS_INVAL;
        if (nm_600_out(hdr[i].fr, d, 2, 5000) != 0)
            return SANE_STATUS_IO_ERROR;
    }
    if (nm_600_in("0600000302000000", tmp, 2, 5000) < 0)
        return SANE_STATUS_IO_ERROR;
    {
        unsigned char one[1] = {0x01};
        if (nm_600_out("0600000302000000", one, 1, 5000) != 0)
            return SANE_STATUS_IO_ERROR;
    }
    for (int i = 0; i < 3; i++)
        if (nm_600_in("0e00000302000000", tmp, 2, 5000) < 0)
            return SANE_STATUS_IO_ERROR;
    for (int k = 0; k < 704; k++) {
        unsigned addr = (unsigned)k * 0x20;
        unsigned char pay8[8] = {0x01, 0x00, 0x0f, 0x20,
                                 (unsigned char)(addr & 0xff),
                                 (unsigned char)((addr >> 8) & 0xff),
                                 0x02, 0x03};
        unsigned char data1008[2] = {0x10, 0x08};
        if (nm_600_out("0600000308000000", pay8, 8, 5000) != 0)
            return SANE_STATUS_IO_ERROR;
        if (nm_600_out("0400000302000000", data1008, 2, 5000) != 0)
            return SANE_STATUS_IO_ERROR;
        if (nm_600_in("0600000302000000", tmp, 2, 5000) < 0)
            return SANE_STATUS_IO_ERROR;
        if (nm_600_in("0e00000300010000", tmp, 256, 5000) < 0)
            return SANE_STATUS_IO_ERROR;
        if (verbose && (k < 2 || k % 100 == 0))
            fprintf(stderr, "  poll600 cycle %d/704...\n", k);
    }
    if (verbose) fprintf(stderr, "  poll600 sweep done\n");
    return SANE_STATUS_GOOD;
}

/* Full scan body: framed bulk OUT, post-bulk vendors, strip loop.
 * Exact-size strip pairs (Windows shape); ends on bulk-IN idle timeout,
 * paper-out remainder + lid-dark, then drain-until-clean. */
static SANE_Status nm_scan_full(const NmModeCfg *cfg,
                                const char *bin_path,
                                unsigned char **out_buf, size_t *out_len,
                                int verbose)
{
    FILE *f = fopen(bin_path, "rb");
    if (!f) { if (verbose) fprintf(stderr,"  fopen %s failed\n", bin_path); return SANE_STATUS_IO_ERROR; }
    fseek(f, 0, SEEK_END); long total = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *blob = malloc(total);
    if (fread(blob, 1, total, f) != (size_t)total) { fclose(f); free(blob); return SANE_STATUS_IO_ERROR; }
    fclose(f);

    /* Settle after init (lamp/motor spin-up window, mirrors driver). */
    nm_msleep(2000);

    /* 600-DPI pre-cal sensor sweep before bulk cal (verbatim pcap). */
    if (cfg->is600) {
        SANE_Status pst = nm_poll600(verbose);
        if (pst != SANE_STATUS_GOOD) { free(blob); return pst; }
    }

    size_t consumed = 0;
    SANE_Status st = nm_bulk_out_framed(cfg, blob, total, &consumed,
                                         verbose);
    if (st != SANE_STATUS_GOOD) { free(blob); return st; } /* NO_DOCS out */

    /* Ordered tail region (INs + 2800/0a00/1000/9d00 + framed BULK32/8/2 +
     * 0600/0400s/0200s). Consumes the rest of the blob (600: 32B + literals). */
    st = nm_tail_region(cfg, blob, total, &consumed, verbose);
    free(blob);
    if (st != SANE_STATUS_GOOD) return st;

    st = nm_post_bulk(cfg, verbose);
    if (st != SANE_STATUS_GOOD) return st;

    /* 64 KiB reads: the fw short-packets each strip (~63700 B), so a
     * 64K URB completes on the short packet and self-synchronizes.
     * Exact-size URBs overflow when host/fw bulk toggles disagree
     * (warm runs without set_configuration). Each completed read is
     * one strip for vendors/pacing/paper purposes. */
    size_t cap = 4 * 1024 * 1024, len = 0;
    unsigned char *buf = malloc(cap);
    unsigned char *drain_tmp = malloc(65536);
    int n = 0, paper_out_at = -1, dark_streak = 0;
    const char *ended = NULL;
    struct timespec t0, t1;
    /* First bulk-IN touch: the fw may have a ≤4B status word queued
     * (seen warm) or already flood image. A full 64K buffer never
     * overflows on the first 512B packet; ≤4B is dropped (not image),
     * anything bigger is strip 1, consumed alone below. */
    int have_first = 0;
    size_t first_len = 0;
    {
        if (len + 65536 > cap) {
            cap = (len + 65536) * 2;
            buf = realloc(buf, cap);
        }
        int pxfer = nm_bulk_in(buf + len, 65536, 300);
        if (pxfer > 0) {
            if (pxfer <= 4) {
                if (verbose) fprintf(stderr,"  pre-image status %02x%02x%02x%02x dropped\n",
                                     buf[len], pxfer > 1 ? buf[len+1] : 0,
                                     pxfer > 2 ? buf[len+2] : 0,
                                     pxfer > 3 ? buf[len+3] : 0);
            } else {
                len += (size_t)pxfer;
                have_first = 1;
                first_len = (size_t)pxfer;
                if (verbose) fprintf(stderr,"  pre-image %dB kept as strip 1\n",
                                     pxfer);
            }
        }
    }
    while (!ended && len < cfg->max_bytes) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        size_t s0;
        if (have_first) {
            s0 = len - first_len;
            have_first = 0;
        } else {
            s0 = len;
            if (len + 65536 > cap) {
                cap = (len + 65536) * 2;
                buf = realloc(buf, cap);
            }
            int xfer = nm_bulk_in(buf + len, 65536, 3000);
            if (xfer == 0) {
                if (verbose) fprintf(stderr,"  strip %d: idle timeout, end of image\n", n);
                ended = "idle";
                break;
            }
            if (xfer < 0) {
                fprintf(stderr,"neatnm1000: strip %d bulk read failed\n", n);
                free(buf); free(drain_tmp); return SANE_STATUS_IO_ERROR;
            }
            len += (size_t)xfer;
        }
        if (ended) break;
        n++;
        if (verbose && (n <= 3 || n % 10 == 0))
            fprintf(stderr,"  BULK IN: %zu B in %d strips...\n", len, n);
        int paper = nm_strip_vendors(cfg);
        if (verbose && (n <= 3 || n % 20 == 0 ||
                        paper == 0x7355 || paper == 0x7055 ||
                        paper == 0x7155 || paper == 0x3955))
            fprintf(stderr,"  strip %d: 400200 -> %04x\n", n, paper);
        /* Windows strip pace (150 needs 216ms; overrun re-serves stale). */
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double el = (t1.tv_sec - t0.tv_sec) +
                    (t1.tv_nsec - t0.tv_nsec) / 1e9;
        if (el < cfg->cycle)
            nm_msleep((int)((cfg->cycle - el) * 1000));
        /* Sensor sits ~1in upstream of the head: 7355 = trailing edge
         * at the SENSOR with rows still under the head. Keep the framed
         * loop going; stop 64 strips after paper-out (backstop). Sticky:
         * never reset paper_out_at (status flickers mid-feed). */
        if (paper == 0x7355 || paper == 0x7055 || paper == 0x7155 ||
            paper == 0x3955) {
            if (paper_out_at < 0) {
                paper_out_at = n;
                if (verbose) fprintf(stderr,"  paper-out at strip %d (%04x),"
                                     " reading remainder\n", n, paper);
            } else if (n - paper_out_at >= 64) {
                ended = "paper-out-cap";
                break;
            }
        }
        /* Content end: 3 consecutive lid-dark strips after paper-out =
         * trailing edge fully past the head (7155-closed fw keeps
         * serving stale brights; darkness is the true end). */
        if (paper_out_at >= 0) {
            long m = 0;
            size_t k;
            for (k = s0; k < len; k += 7) m += buf[k];
            m /= (long)((len - s0) / 7 + 1);
            if (m < 50) {
                if (++dark_streak >= 3) {
                    if (verbose) fprintf(stderr,"  lid-dark x3 at strip %d,"
                                         " end of image\n", n);
                    ended = "paper-out";
                    break;
                }
            } else {
                dark_streak = 0;
            }
        }
    }
    if (!ended)
        fprintf(stderr,"neatnm1000: hit %zuMB cap (paper not feeding?)\n",
                cfg->max_bytes / (1024 * 1024));
    if (len > 0) {
        /* Drain the fw end-of-scan queue with plain reads (no 8200s)
         * until 3 consecutive misses (cap 64). Discarded, never appended:
         * post-end bytes are lid repeats or stale brights, and appending
         * shifts width math. Leftover bytes must still be READ or they
         * back-pressure the fw into refusing the next session. */
        int miss = 0;
        size_t drained = 0;
        for (int d = 0; d < 64 && miss < 3; d++) {
            int dg = nm_bulk_in(drain_tmp, 65536, 800);
            if (dg <= 0) { miss++; continue; }
            miss = 0;
            drained += (size_t)dg;
        }
        if (verbose) fprintf(stderr,"  drain: discarded %zu B\n", drained);
    }
    free(drain_tmp);
    if (verbose) fprintf(stderr,"  bulk IN done: %zu B in %d strips (end=%s)\n", len, n, ended ? ended : "cap");
    /* End-of-scan close ritual runs in sane_start() (post-image only). */
    *out_buf = buf; *out_len = len;
    return SANE_STATUS_GOOD;
}

SANE_Status sane_read(SANE_Handle h, SANE_Byte *buf, SANE_Int maxlen, SANE_Int *len)
{
    NeatNM1000_Scanner *s = (NeatNM1000_Scanner *)h;
    size_t left = s->image_size - s->image_read;
    size_t n = (size_t)maxlen < left ? (size_t)maxlen : left;
    memcpy(buf, s->image + s->image_read, n);
    s->image_read += n;
    *len = (SANE_Int)n;
    return n == 0 ? SANE_STATUS_EOF : SANE_STATUS_GOOD;
}

void sane_cancel(SANE_Handle h) { (void)h; }
SANE_Status sane_set_io_mode(SANE_Handle h, SANE_Bool m) { (void)h; (void)m; return SANE_STATUS_GOOD; }
SANE_Status sane_get_select_fd(SANE_Handle h, SANE_Int *fd) { (void)h; (void)fd; return SANE_STATUS_UNSUPPORTED; }

/* NOTE: the sane_neatnm1000_* versioned entry points are generated from
 * backend/stubs.c via -DBACKEND_NAME=neatnm1000; they are not defined
 * by hand here. */
