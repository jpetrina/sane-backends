/* sane - Scanner Access Now Easy.
   This file implements a SANE backend for V4L2 video devices.

   This file is part of the SANE package.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 3 of the License, or (at your
   option) any later version.
 */

#define BUILD 6

#include "../include/sane/config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <libv4l2.h>

#define BACKEND_NAME v4l
#include "../include/sane/sanei_backend.h"
#include "../include/sane/sanei_config.h"
#include "../include/sane/saneopts.h"
#include "v4l.h"

#define V4L_CONFIG_FILE "v4l.conf"

static const SANE_Device **devlist;
static V4L_Device *first_dev;
static V4L_Scanner *first_handle;
static int num_devices;

void sane_cancel (SANE_Handle handle);

static const SANE_String_Const mode_list[] = {
  SANE_VALUE_SCAN_MODE_GRAY, SANE_VALUE_SCAN_MODE_COLOR, NULL
};

static const SANE_Range u8_range = { 0, 255, 0 };

static void
free_channels (V4L_Scanner *s)
{
  int i;
  for (i = 0; i < MAX_CHANNELS; i++)
    {
      free ((void *) s->channel[i]);
      s->channel[i] = NULL;
    }
}

static V4L_Scanner *
find_handle (SANE_Handle handle)
{
  V4L_Scanner *s;
  for (s = first_handle; s; s = s->next)
    if (s == handle)
      return s;
  return NULL;
}

static int
xioctl (int fd, unsigned long request, void *arg)
{
  int result;
  do
    result = v4l2_ioctl (fd, request, arg);
  while (result == -1 && errno == EINTR);
  return result;
}

static SANE_Status
query_capability (int fd, struct v4l2_capability *cap)
{
  if (xioctl (fd, VIDIOC_QUERYCAP, cap) == -1)
    return SANE_STATUS_INVAL;
  if (!(cap->capabilities & V4L2_CAP_VIDEO_CAPTURE)
      || !(cap->capabilities & (V4L2_CAP_READWRITE | V4L2_CAP_STREAMING)))
    return SANE_STATUS_UNSUPPORTED;
  return SANE_STATUS_GOOD;
}

static SANE_Status
attach (const char *devname)
{
  V4L_Device *dev;
  struct v4l2_capability cap;
  int fd;

  for (dev = first_dev; dev; dev = dev->next)
    if (strcmp (dev->sane.name, devname) == 0)
      return SANE_STATUS_GOOD;

  fd = v4l2_open (devname, O_RDWR);
  if (fd == -1)
    return SANE_STATUS_INVAL;
  memset (&cap, 0, sizeof (cap));
  if (query_capability (fd, &cap) != SANE_STATUS_GOOD)
    {
      v4l2_close (fd);
      return SANE_STATUS_UNSUPPORTED;
    }
  v4l2_close (fd);

  dev = calloc (1, sizeof (*dev));
  if (!dev)
    return SANE_STATUS_NO_MEM;
  dev->sane.name = strdup (devname);
  dev->sane.model = strdup ((const char *) cap.card);
  if (!dev->sane.name || !dev->sane.model)
    {
      free ((void *) dev->sane.name);
      free ((void *) dev->sane.model);
      free (dev);
      return SANE_STATUS_NO_MEM;
    }
  dev->sane.vendor = "Video4Linux2";
  dev->sane.type = "video capture device";
  dev->next = first_dev;
  first_dev = dev;
  num_devices++;
  return SANE_STATUS_GOOD;
}

static void
set_default_format (V4L_Scanner *s)
{
  memset (&s->format, 0, sizeof (s->format));
  s->format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl (s->fd, VIDIOC_G_FMT, &s->format) == -1)
    {
      s->format.fmt.pix.width = 640;
      s->format.fmt.pix.height = 480;
      s->format.fmt.pix.pixelformat = V4L2_PIX_FMT_GREY;
      s->format.fmt.pix.field = V4L2_FIELD_ANY;
    }
}

static void
update_parameters (V4L_Scanner *s)
{
  s->params.format = (s->format.fmt.pix.pixelformat == V4L2_PIX_FMT_GREY)
    ? SANE_FRAME_GRAY : SANE_FRAME_RGB;
  s->params.last_frame = SANE_TRUE;
  s->params.pixels_per_line = s->format.fmt.pix.width;
  s->params.lines = s->format.fmt.pix.height;
  s->params.bytes_per_line = s->format.fmt.pix.bytesperline;
  if (!s->params.bytes_per_line)
    s->params.bytes_per_line = s->params.pixels_per_line
      * ((s->params.format == SANE_FRAME_GRAY) ? 1 : 3);
  s->params.depth = 8;
}

static int
query_control (V4L_Scanner *s, unsigned id, SANE_Word *value)
{
  struct v4l2_queryctrl query;
  struct v4l2_control control;
  memset (&query, 0, sizeof (query));
  query.id = id;
  if (xioctl (s->fd, VIDIOC_QUERYCTRL, &query) == -1
      || (query.flags & V4L2_CTRL_FLAG_DISABLED))
    return -1;
  memset (&control, 0, sizeof (control));
  control.id = id;
  if (xioctl (s->fd, VIDIOC_G_CTRL, &control) == -1)
    return -1;
  *value = query.maximum == query.minimum ? 0
    : (control.value - query.minimum) * 255
      / (query.maximum - query.minimum);
  return 0;
}

static SANE_Status
set_control (V4L_Scanner *s, unsigned id, SANE_Word value)
{
  struct v4l2_queryctrl query;
  struct v4l2_control control;
  memset (&query, 0, sizeof (query));
  query.id = id;
  if (xioctl (s->fd, VIDIOC_QUERYCTRL, &query) == -1
      || (query.flags & V4L2_CTRL_FLAG_DISABLED))
    return SANE_STATUS_UNSUPPORTED;
  memset (&control, 0, sizeof (control));
  control.id = id;
  control.value = query.minimum
    + value * (query.maximum - query.minimum) / 255;
  return xioctl (s->fd, VIDIOC_S_CTRL, &control) == -1
    ? SANE_STATUS_INVAL : SANE_STATUS_GOOD;
}

static SANE_Status
init_options (V4L_Scanner *s)
{
  int i;
  memset (s->opt, 0, sizeof (s->opt));
  memset (s->val, 0, sizeof (s->val));
  for (i = 0; i < NUM_OPTIONS; i++)
    {
      s->opt[i].size = sizeof (SANE_Word);
      s->opt[i].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    }

  s->opt[OPT_NUM_OPTS] = (SANE_Option_Descriptor) {
    SANE_NAME_NUM_OPTIONS, SANE_TITLE_NUM_OPTIONS, SANE_DESC_NUM_OPTIONS,
    SANE_TYPE_INT, SANE_UNIT_NONE, sizeof (SANE_Word), SANE_CAP_SOFT_DETECT,
    SANE_CONSTRAINT_NONE, { 0 }
  };
  s->val[OPT_NUM_OPTS].w = NUM_OPTIONS;
  s->opt[OPT_MODE_GROUP] = (SANE_Option_Descriptor) {
    "mode-group", "Scan Mode", "", SANE_TYPE_GROUP, SANE_UNIT_NONE,
    0, 0, SANE_CONSTRAINT_NONE, { 0 }
  };
  s->opt[OPT_MODE] = (SANE_Option_Descriptor) {
    SANE_NAME_SCAN_MODE, SANE_TITLE_SCAN_MODE, SANE_DESC_SCAN_MODE,
    SANE_TYPE_STRING, SANE_UNIT_NONE, 6,
    SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
    SANE_CONSTRAINT_STRING_LIST, { .string_list = mode_list }
  };
  s->val[OPT_MODE].s = strdup (SANE_VALUE_SCAN_MODE_GRAY);
  if (!s->val[OPT_MODE].s)
    return SANE_STATUS_NO_MEM;

  s->opt[OPT_CHANNEL] = (SANE_Option_Descriptor) {
    "channel", "Channel", "Selects the V4L2 input channel",
    SANE_TYPE_STRING, SANE_UNIT_NONE, 1,
    SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
    SANE_CONSTRAINT_STRING_LIST, { .string_list = s->channel }
  };
  if (s->channel[0])
    {
      s->val[OPT_CHANNEL].s = strdup (s->channel[0]);
      if (!s->val[OPT_CHANNEL].s)
        return SANE_STATUS_NO_MEM;
    }
  else
    s->opt[OPT_CHANNEL].cap |= SANE_CAP_INACTIVE;

  s->opt[OPT_GEOMETRY_GROUP] = (SANE_Option_Descriptor) {
    "geometry-group", "Geometry", "", SANE_TYPE_GROUP, SANE_UNIT_NONE,
    0, SANE_CAP_ADVANCED, SANE_CONSTRAINT_NONE, { 0 }
  };
  for (i = OPT_TL_X; i <= OPT_BR_Y; i++)
    s->opt[i].cap |= SANE_CAP_INACTIVE;
  s->opt[OPT_ENHANCEMENT_GROUP] = (SANE_Option_Descriptor) {
    "enhancement-group", "Enhancement", "", SANE_TYPE_GROUP,
    SANE_UNIT_NONE, 0, 0, SANE_CONSTRAINT_NONE, { 0 }
  };
  s->opt[OPT_BRIGHTNESS] = (SANE_Option_Descriptor) {
    SANE_NAME_BRIGHTNESS, SANE_TITLE_BRIGHTNESS, SANE_DESC_BRIGHTNESS,
    SANE_TYPE_INT, SANE_UNIT_NONE, sizeof (SANE_Word),
    SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT, SANE_CONSTRAINT_RANGE,
    { .range = &u8_range }
  };
  s->opt[OPT_HUE] = s->opt[OPT_BRIGHTNESS];
  s->opt[OPT_HUE].name = SANE_NAME_HUE;
  s->opt[OPT_HUE].title = SANE_TITLE_HUE;
  s->opt[OPT_HUE].desc = SANE_DESC_HUE;
  s->opt[OPT_COLOR] = s->opt[OPT_BRIGHTNESS];
  s->opt[OPT_COLOR].name = "color";
  s->opt[OPT_COLOR].title = "Picture color";
  s->opt[OPT_COLOR].desc = "Sets the picture's color.";
  s->opt[OPT_CONTRAST] = s->opt[OPT_BRIGHTNESS];
  s->opt[OPT_CONTRAST].name = SANE_NAME_CONTRAST;
  s->opt[OPT_CONTRAST].title = SANE_TITLE_CONTRAST;
  s->opt[OPT_CONTRAST].desc = SANE_DESC_CONTRAST;
  s->opt[OPT_WHITE_LEVEL] = s->opt[OPT_BRIGHTNESS];
  s->opt[OPT_WHITE_LEVEL].name = SANE_NAME_WHITE_LEVEL;
  s->opt[OPT_WHITE_LEVEL].title = SANE_TITLE_WHITE_LEVEL;
  s->opt[OPT_WHITE_LEVEL].desc = SANE_DESC_WHITE_LEVEL;
  query_control (s, V4L2_CID_BRIGHTNESS, &s->val[OPT_BRIGHTNESS].w);
  query_control (s, V4L2_CID_HUE, &s->val[OPT_HUE].w);
  query_control (s, V4L2_CID_SATURATION, &s->val[OPT_COLOR].w);
  query_control (s, V4L2_CID_CONTRAST, &s->val[OPT_CONTRAST].w);
  query_control (s, V4L2_CID_WHITENESS, &s->val[OPT_WHITE_LEVEL].w);
  return SANE_STATUS_GOOD;
}

static void
cleanup_capture (V4L_Scanner *s)
{
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (s->streaming)
    {
      xioctl (s->fd, VIDIOC_STREAMOFF, &type);
      s->streaming = SANE_FALSE;
    }
  if (s->buffer && s->is_mmap)
    v4l2_munmap (s->buffer, s->buffer_size);
  else if (s->buffer)
    free (s->buffer);
  s->buffer = NULL;
  s->buffer_size = 0;
  s->frame_size = 0;
  s->offset = 0;
  s->scanning = SANE_FALSE;
}

static SANE_Status
start_capture (V4L_Scanner *s)
{
  struct v4l2_requestbuffers req;
  struct v4l2_buffer buf;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  size_t size;
  ssize_t result;

  cleanup_capture (s);
  memset (&req, 0, sizeof (req));
  req.count = 1;
  req.type = type;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl (s->fd, VIDIOC_REQBUFS, &req) == -1 || req.count < 1)
    {
      size = s->format.fmt.pix.sizeimage;
      if (!size)
        size = (size_t) s->format.fmt.pix.bytesperline
          * s->format.fmt.pix.height;
      s->buffer = malloc (size);
      if (!s->buffer)
        return SANE_STATUS_NO_MEM;
      s->buffer_size = size;
      s->is_mmap = SANE_FALSE;
      result = v4l2_read (s->fd, s->buffer, size);
      if (result <= 0)
        {
          cleanup_capture (s);
          return SANE_STATUS_IO_ERROR;
        }
      s->frame_size = (size_t) result;
    }
  else
    {
      memset (&buf, 0, sizeof (buf));
      buf.type = type;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = 0;
      if (xioctl (s->fd, VIDIOC_QUERYBUF, &buf) == -1)
        return SANE_STATUS_IO_ERROR;
      s->buffer_size = buf.length;
      s->buffer = v4l2_mmap (NULL, buf.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, s->fd, buf.m.offset);
      if (s->buffer == MAP_FAILED)
        {
          s->buffer = NULL;
          return SANE_STATUS_IO_ERROR;
        }
      s->is_mmap = SANE_TRUE;
      if (xioctl (s->fd, VIDIOC_QBUF, &buf) == -1
          || xioctl (s->fd, VIDIOC_STREAMON, &type) == -1)
        {
          cleanup_capture (s);
          return SANE_STATUS_IO_ERROR;
        }
      s->streaming = SANE_TRUE;
      if (xioctl (s->fd, VIDIOC_DQBUF, &buf) == -1)
        {
          cleanup_capture (s);
          return SANE_STATUS_IO_ERROR;
        }
      s->frame_size = buf.bytesused < s->buffer_size
        ? buf.bytesused : s->buffer_size;
    }
  s->scanning = SANE_TRUE;
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_init (SANE_Int *version_code, SANE_Auth_Callback authorize)
{
  char line[PATH_MAX];
  FILE *fp;
  char *end;
  (void) authorize;
  DBG (3, "sane_init\n");
  if (version_code)
    *version_code = SANE_VERSION_CODE (SANE_CURRENT_MAJOR, SANE_CURRENT_MINOR, BUILD);
  fp = sanei_config_open (V4L_CONFIG_FILE);
  if (!fp)
    return attach ("/dev/video0");
  while (sanei_config_read (line, sizeof (line), fp))
    {
      if (line[0] == '#')
        continue;
      end = line;
      while (*end && *end != '#' && *end != ' ' && *end != '\t')
        end++;
      *end = '\0';
      if (*line)
        attach (line);
    }
  fclose (fp);
  return SANE_STATUS_GOOD;
}

void
sane_exit (void)
{
  V4L_Device *dev, *next;
  while (first_handle)
    sane_close (first_handle);
  free (devlist);
  devlist = NULL;
  for (dev = first_dev; dev; dev = next)
    {
      next = dev->next;
      free ((void *) dev->sane.name);
      free ((void *) dev->sane.model);
      free (dev);
    }
  first_dev = NULL;
  num_devices = 0;
}

SANE_Status
sane_get_devices (const SANE_Device ***device_list, SANE_Bool local_only)
{
  V4L_Device *dev;
  int i = 0;
  (void) local_only;
  if (!device_list)
    return SANE_STATUS_INVAL;
  free (devlist);
  devlist = calloc ((size_t) num_devices + 1, sizeof (*devlist));
  if (!devlist)
    return SANE_STATUS_NO_MEM;
  for (dev = first_dev; dev; dev = dev->next)
    devlist[i++] = &dev->sane;
  *device_list = devlist;
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_open (SANE_String_Const devname, SANE_Handle *handle)
{
  V4L_Device *dev;
  V4L_Scanner *s;
  struct v4l2_input input;
  int fd, i;

  if (!devname || !handle)
    return SANE_STATUS_INVAL;
  for (dev = first_dev; dev; dev = dev->next)
    if (strcmp (dev->sane.name, devname) == 0)
      break;
  if (!dev)
    return SANE_STATUS_INVAL;
  fd = v4l2_open (devname, O_RDWR);
  if (fd == -1)
    return SANE_STATUS_INVAL;
  s = calloc (1, sizeof (*s));
  if (!s)
    {
      v4l2_close (fd);
      return SANE_STATUS_NO_MEM;
    }
  s->fd = fd;
  s->devicename = devname;
  if (query_capability (fd, &s->capability) != SANE_STATUS_GOOD)
    goto fail;
  for (i = 0; i < MAX_CHANNELS; i++)
    {
      memset (&input, 0, sizeof (input));
      input.index = i;
      if (xioctl (fd, VIDIOC_ENUMINPUT, &input) == -1)
        break;
      s->channel[i] = strdup ((const char *) input.name);
      if (!s->channel[i])
        goto fail;
    }
  set_default_format (s);
  if (init_options (s) != SANE_STATUS_GOOD)
    goto fail;
  update_parameters (s);
  s->next = first_handle;
  first_handle = s;
  *handle = s;
  return SANE_STATUS_GOOD;

fail:
  free_channels (s);
  free ((void *) s->val[OPT_MODE].s);
  free ((void *) s->val[OPT_CHANNEL].s);
  v4l2_close (fd);
  free (s);
  return SANE_STATUS_INVAL;
}

void
sane_close (SANE_Handle handle)
{
  V4L_Scanner **p;
  V4L_Scanner *s = find_handle (handle);
  if (!s)
    return;
  sane_cancel (handle);
  for (p = &first_handle; *p && *p != s; p = &(*p)->next)
    ;
  if (*p)
    *p = s->next;
  free_channels (s);
  free ((void *) s->val[OPT_MODE].s);
  free ((void *) s->val[OPT_CHANNEL].s);
  v4l2_close (s->fd);
  free (s);
}

const SANE_Option_Descriptor *
sane_get_option_descriptor (SANE_Handle handle, SANE_Int option)
{
  V4L_Scanner *s = find_handle (handle);
  if (!s || option < 0 || option >= NUM_OPTIONS)
    return NULL;
  return &s->opt[option];
}

SANE_Status
sane_control_option (SANE_Handle handle, SANE_Int option, SANE_Action action,
                     void *value, SANE_Int *info)
{
  V4L_Scanner *s = find_handle (handle);
  char *copy;
  SANE_Word *word;
  if (!s || !value || option < 0 || option >= NUM_OPTIONS)
    return SANE_STATUS_INVAL;
  if (info)
    *info = 0;
  if (action == SANE_ACTION_GET_VALUE)
    {
      if (option == OPT_MODE || option == OPT_CHANNEL)
        strcpy (value, s->val[option].s ? s->val[option].s : "");
      else if (option == OPT_NUM_OPTS || option >= OPT_BRIGHTNESS)
        *(SANE_Word *) value = s->val[option].w;
      else
        return SANE_STATUS_INVAL;
      return SANE_STATUS_GOOD;
    }
  if (action != SANE_ACTION_SET_VALUE
      || !SANE_OPTION_IS_SETTABLE (s->opt[option].cap))
    return SANE_STATUS_INVAL;
  if (option == OPT_MODE || option == OPT_CHANNEL)
    {
      copy = strdup (value);
      if (!copy)
        return SANE_STATUS_NO_MEM;
      if (option == OPT_CHANNEL)
        {
          int i;
          struct v4l2_input input;
          for (i = 0; i < MAX_CHANNELS && s->channel[i]; i++)
            if (strcmp (s->channel[i], copy) == 0)
              {
                memset (&input, 0, sizeof (input));
                input.index = i;
                if (xioctl (s->fd, VIDIOC_S_INPUT, &input) == -1)
                  {
                    free (copy);
                    return SANE_STATUS_INVAL;
                  }
                free ((void *) s->val[option].s);
                s->val[option].s = copy;
                return SANE_STATUS_GOOD;
              }
          free (copy);
          return SANE_STATUS_INVAL;
        }
      free ((void *) s->val[option].s);
      s->val[option].s = copy;
      if (info)
        *info |= SANE_INFO_RELOAD_PARAMS;
      return SANE_STATUS_GOOD;
    }
  word = value;
  if (option < OPT_BRIGHTNESS || option > OPT_WHITE_LEVEL)
    return SANE_STATUS_INVAL;
  switch (option)
    {
    case OPT_BRIGHTNESS: return set_control (s, V4L2_CID_BRIGHTNESS, *word);
    case OPT_HUE: return set_control (s, V4L2_CID_HUE, *word);
    case OPT_COLOR: return set_control (s, V4L2_CID_SATURATION, *word);
    case OPT_CONTRAST: return set_control (s, V4L2_CID_CONTRAST, *word);
    case OPT_WHITE_LEVEL: return set_control (s, V4L2_CID_WHITENESS, *word);
    default: return SANE_STATUS_INVAL;
    }
}

SANE_Status
sane_get_parameters (SANE_Handle handle, SANE_Parameters *params)
{
  V4L_Scanner *s = find_handle (handle);
  if (!s || !params)
    return SANE_STATUS_INVAL;
  update_parameters (s);
  *params = s->params;
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_start (SANE_Handle handle)
{
  V4L_Scanner *s = find_handle (handle);
  struct v4l2_format format;
  __u32 pixel_format;
  if (!s)
    return SANE_STATUS_INVAL;
  pixel_format = (s->val[OPT_MODE].s
                  && strcmp (s->val[OPT_MODE].s, SANE_VALUE_SCAN_MODE_COLOR) == 0)
    ? V4L2_PIX_FMT_RGB24 : V4L2_PIX_FMT_GREY;
  format = s->format;
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.pixelformat = pixel_format;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  if (xioctl (s->fd, VIDIOC_S_FMT, &format) == -1)
    return SANE_STATUS_INVAL;
  if (format.fmt.pix.pixelformat != pixel_format)
    return SANE_STATUS_UNSUPPORTED;
  s->format = format;
  update_parameters (s);
  return start_capture (s);
}

SANE_Status
sane_read (SANE_Handle handle, SANE_Byte *buf, SANE_Int max_len, SANE_Int *lenp)
{
  V4L_Scanner *s = find_handle (handle);
  size_t remaining, count;
  if (!s || !buf || !lenp || max_len < 0)
    return SANE_STATUS_INVAL;
  if (!s->scanning || s->offset >= s->frame_size)
    {
      *lenp = 0;
      return SANE_STATUS_EOF;
    }
  remaining = s->frame_size - s->offset;
  count = remaining < (size_t) max_len ? remaining : (size_t) max_len;
  memcpy (buf, (unsigned char *) s->buffer + s->offset, count);
  s->offset += count;
  *lenp = (SANE_Int) count;
  return SANE_STATUS_GOOD;
}

void
sane_cancel (SANE_Handle handle)
{
  V4L_Scanner *s = find_handle (handle);
  if (s)
    cleanup_capture (s);
}

SANE_Status
sane_set_io_mode (SANE_Handle __sane_unused__ handle, SANE_Bool non_blocking)
{
  return non_blocking ? SANE_STATUS_UNSUPPORTED : SANE_STATUS_GOOD;
}

SANE_Status
sane_get_select_fd (SANE_Handle __sane_unused__ handle,
                    SANE_Int __sane_unused__ *fd)
{
  return SANE_STATUS_UNSUPPORTED;
}
