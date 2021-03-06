/*
 * Copyright (C) 2008-2010 Felipe Contreras
 *
 * Author: Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1, a copy of which is found in LICENSE included in the
 * packaging of this file.
 */

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>

#include <linux/fb.h>
#include <linux/omapfb.h>

#include "omapfb.h"
#include "log.h"

#define ROUND_UP(num, scale) (((num) + ((scale) - 1)) & ~((scale) - 1))

static void *parent_class;

#ifndef GST_DISABLE_GST_DEBUG
GstDebugCategory *omapfb_debug;
#endif

struct page {
	unsigned yoffset;
	void *buf;
	bool used;
};

struct gst_omapfb_sink {
	GstBaseSink parent;

	struct fb_var_screeninfo varinfo;
	struct fb_var_screeninfo overlay_info;
	struct omapfb_mem_info mem_info;
	struct omapfb_plane_info plane_info;

	int overlay_fd;
	unsigned char *framebuffer;
	bool enabled;
	bool manual_update;

	struct page *pages;
	int nr_pages;
	struct page *cur_page;
	struct page *old_page;
};

struct gst_omapfb_sink_class {
	GstBaseSinkClass parent_class;
};

static GstCaps *
generate_sink_template(void)
{
	GstCaps *caps;
	GstStructure *struc;

	caps = gst_caps_new_empty();

	struc = gst_structure_new("video/x-raw-yuv",
			"width", GST_TYPE_INT_RANGE, 16, 4096,
			"height", GST_TYPE_INT_RANGE, 16, 4096,
			"framerate", GST_TYPE_FRACTION_RANGE, 0, 1, 30, 1,
			NULL);

	{
		GValue list;
		GValue val;

		list.g_type = val.g_type = 0;

		g_value_init(&list, GST_TYPE_LIST);
		g_value_init(&val, GST_TYPE_FOURCC);

#if 0
		gst_value_set_fourcc(&val, GST_MAKE_FOURCC('I', '4', '2', '0'));
		gst_value_list_append_value(&list, &val);

		gst_value_set_fourcc(&val, GST_MAKE_FOURCC('Y', 'U', 'Y', '2'));
		gst_value_list_append_value(&list, &val);

		gst_value_set_fourcc(&val, GST_MAKE_FOURCC('U', 'Y', 'V', 'Y'));
		gst_value_list_append_value(&list, &val);
#else
		gst_value_set_fourcc(&val, GST_MAKE_FOURCC('U', 'Y', 'V', 'Y'));
		gst_value_list_append_value(&list, &val);
#endif

		gst_structure_set_value(struc, "format", &list);

		g_value_unset(&val);
		g_value_unset(&list);
	}

	gst_caps_append_structure(caps, struc);

	return caps;
}

static void
update(struct gst_omapfb_sink *self)
{
	struct omapfb_update_window update_window;
	unsigned x, y, w, h;

	x = y = 0;
	w = self->varinfo.xres;
	h = self->varinfo.yres;

	update_window.x = x;
	update_window.y = y;
	update_window.width = w;
	update_window.height = h;
	update_window.format = 0;
	update_window.out_x = 0;
	update_window.out_y = 0;
	update_window.out_width = w;
	update_window.out_height = h;

	ioctl(self->overlay_fd, OMAPFB_UPDATE_WINDOW, &update_window);
}

static struct page *get_page(struct gst_omapfb_sink *self)
{
	struct page *page = NULL;
	int i;
	for (i = 0; i < self->nr_pages; i++) {
		if (&self->pages[i] == self->cur_page)
			continue;
		if (&self->pages[i] == self->old_page)
			continue;
		if (!self->pages[i].used) {
			page = &self->pages[i];
			break;
		}
	}
	/* old page needs a vsync */
	if (!page && self->old_page && !self->old_page->used)
		page = self->old_page;
	if (page)
		page->used = true;
	return page;
}

static gboolean
setup(struct gst_omapfb_sink *self, GstCaps *caps)
{
	GstStructure *structure;
	int width, height;
	int update_mode;
	struct omapfb_color_key color_key;
	size_t framesize;
	int par_n, par_d;
	unsigned out_width, out_height;

	structure = gst_caps_get_structure(caps, 0);

	gst_structure_get_int(structure, "width", &width);
	gst_structure_get_int(structure, "height", &height);
	if (!gst_structure_get_fraction(structure, "pixel-aspect-ratio", &par_n, &par_d))
		par_n = par_d = 1;

	self->plane_info.enabled = 0;
	if (ioctl(self->overlay_fd, OMAPFB_SETUP_PLANE, &self->plane_info)) {
		pr_err(self, "could not disable plane");
		return false;
	}

	framesize = GST_ROUND_UP_2(width) * height * 2;

	self->mem_info.type = OMAPFB_MEMTYPE_SDRAM;
	self->mem_info.size = framesize * self->nr_pages;

	if (ioctl(self->overlay_fd, OMAPFB_SETUP_MEM, &self->mem_info)) {
		pr_err(self, "could not setup memory info");
		return false;
	}

	self->framebuffer = mmap(NULL, self->mem_info.size, PROT_WRITE, MAP_SHARED, self->overlay_fd, 0);
	if (self->framebuffer == MAP_FAILED) {
		pr_err(self, "memory map failed");
		return false;
	}

	self->overlay_info.xres = width;
	self->overlay_info.yres = height;
	self->overlay_info.xres_virtual = self->overlay_info.xres;
	self->overlay_info.yres_virtual = self->overlay_info.yres * self->nr_pages;

	self->overlay_info.xoffset = 0;
	self->overlay_info.yoffset = 0;
	self->overlay_info.nonstd = OMAPFB_COLOR_YUV422;

	pr_info(self, "vscreen info: width=%u, height=%u",
			self->overlay_info.xres, self->overlay_info.yres);

	if (ioctl(self->overlay_fd, FBIOPUT_VSCREENINFO, &self->overlay_info)) {
		pr_err(self, "could not set screen info");
		return false;
	}

	color_key.key_type = OMAPFB_COLOR_KEY_DISABLED;
	if (ioctl(self->overlay_fd, OMAPFB_SET_COLOR_KEY, &color_key))
		pr_err(self, "could not disable color key");

	/* scale to width */
	out_width = self->varinfo.xres;
	out_height = (height * par_d * self->varinfo.xres + width * par_n / 2) / (width * par_n);
	if (out_height > self->varinfo.yres) {
		/* scale to height */
		out_height = self->varinfo.yres;
		out_width = (width * par_n * self->varinfo.yres + height * par_d / 2) / (height * par_d);
	}
	out_width = ROUND_UP(out_width, 2);
	out_height = ROUND_UP(out_height, 2);

	self->plane_info.enabled = 1;
	self->plane_info.pos_x = (self->varinfo.xres - out_width) / 2;
	self->plane_info.pos_y = (self->varinfo.yres - out_height) / 2;
	self->plane_info.out_width = out_width;
	self->plane_info.out_height = out_height;

	pr_info(self, "output info: %dx%d, offset: %d,%d",
			out_width, out_height,
			self->plane_info.pos_x, self->plane_info.pos_y);
	pr_info(self, "plane info: %ux%u",
			self->varinfo.xres, self->varinfo.yres);

	if (ioctl(self->overlay_fd, OMAPFB_SETUP_PLANE, &self->plane_info)) {
		pr_err(self, "could not setup plane");
		return false;
	}

	self->enabled = true;

	update_mode = OMAPFB_MANUAL_UPDATE;
	ioctl(self->overlay_fd, OMAPFB_SET_UPDATE_MODE, &update_mode);
	self->manual_update = (update_mode == OMAPFB_MANUAL_UPDATE);

	self->pages = calloc(self->nr_pages, sizeof(*self->pages));

	int i;
	for (i = 0; i < self->nr_pages; i++) {
		self->pages[i].yoffset = i * self->overlay_info.yres;
		self->pages[i].buf = self->framebuffer + (i * framesize);
	}
	self->cur_page = &self->pages[0];

	return true;
}

static GstFlowReturn
buffer_alloc(GstBaseSink *base, guint64 offset, guint size, GstCaps *caps, GstBuffer **buf)
{
	struct gst_omapfb_sink *self = (struct gst_omapfb_sink *)base;
	GstBuffer *buffer;
	struct page *page;

	if (!self->enabled && !setup(self, caps))
		goto missing;

	page = get_page(self);
	if (!page)
		goto missing;

	buffer = gst_buffer_new();
	GST_BUFFER_DATA(buffer) = page->buf;
	GST_BUFFER_SIZE(buffer) = size;
	gst_buffer_set_caps(buffer, caps);

	*buf = buffer;

	if (page == self->old_page)
		ioctl(self->overlay_fd, OMAPFB_WAITFORVSYNC);

	return GST_FLOW_OK;
missing:
	*buf = NULL;
	return GST_FLOW_OK;
}

static gboolean
setcaps(GstBaseSink *base, GstCaps *caps)
{
	struct gst_omapfb_sink *self = (struct gst_omapfb_sink *)base;
	if (self->enabled)
		return true;
	return setup(self, caps);
}

static gboolean
start(GstBaseSink *base)
{
	struct gst_omapfb_sink *self = (struct gst_omapfb_sink *)base;
	int fd;

	self->nr_pages = 4;
	self->cur_page = self->old_page = NULL;

	fd = open("/dev/fb0", O_RDWR);

	if (fd == -1) {
		pr_err(self, "could not open framebuffer");
		return false;
	}

	if (ioctl(fd, FBIOGET_VSCREENINFO, &self->varinfo)) {
		pr_err(self, "could not get screen info");
		close(fd);
		return false;
	}

	if (close(fd)) {
		pr_err(self, "could not close framebuffer");
		return false;
	}

	self->overlay_fd = open("/dev/fb1", O_RDWR);

	if (self->overlay_fd == -1) {
		pr_err(self, "could not open overlay");
		return false;
	}

	if (ioctl(self->overlay_fd, FBIOGET_VSCREENINFO, &self->overlay_info)) {
		pr_err(self, "could not get overlay screen info");
		return false;
	}

	if (ioctl(self->overlay_fd, OMAPFB_QUERY_PLANE, &self->plane_info)) {
		pr_err(self, "could not query plane info");
		return false;
	}

	return true;
}

static gboolean
stop(GstBaseSink *base)
{
	struct gst_omapfb_sink *self = (struct gst_omapfb_sink *)base;

	if (self->enabled) {
		self->plane_info.enabled = 0;

		if (ioctl(self->overlay_fd, OMAPFB_SETUP_PLANE, &self->plane_info)) {
			pr_err(self, "could not disable plane");
			return false;
		}
	}

	if (munmap(self->framebuffer, self->mem_info.size)) {
		pr_err(self, "could not unmap");
		return false;
	}

	if (close(self->overlay_fd)) {
		pr_err(self, "could not close overlay");
		return false;
	}

	return true;
}

static GstFlowReturn
render(GstBaseSink *base, GstBuffer *buffer)
{
	struct gst_omapfb_sink *self = (struct gst_omapfb_sink *)base;
	struct page *page = NULL;
	int i;

	for (i = 0; i < self->nr_pages; i++)
		if (self->pages[i].buf == GST_BUFFER_DATA(buffer)) {
			page = &self->pages[i];
			break;
		}

	if (!page) {
		page = get_page(self);
		if (!page)
			page = self->cur_page; /* not ok, but last resort */
		memcpy(page->buf, GST_BUFFER_DATA(buffer), GST_BUFFER_SIZE(buffer));
	}

	if (page != self->cur_page) {
		self->overlay_info.yoffset = page->yoffset;
		ioctl(self->overlay_fd, FBIOPAN_DISPLAY, &self->overlay_info);
	}

	if (self->manual_update)
		update(self);

	self->old_page = self->cur_page;
	self->cur_page = page;
	page->used = false;

	return GST_FLOW_OK;
}

static void
class_init(void *g_class, void *class_data)
{
	GstBaseSinkClass *base_sink_class;

	base_sink_class = g_class;

	parent_class = g_type_class_ref(GST_OMAPFB_SINK_TYPE);

	base_sink_class->set_caps = setcaps;
	base_sink_class->buffer_alloc = buffer_alloc;
	base_sink_class->start = start;
	base_sink_class->stop = stop;
	base_sink_class->render = render;
	base_sink_class->preroll = render;
}

static void
base_init(void *g_class)
{
	GstElementClass *element_class = g_class;
	GstPadTemplate *template;

	gst_element_class_set_details_simple(element_class,
			"Linux OMAP framebuffer sink",
			"Sink/Video",
			"Renders video with omapfb",
			"Felipe Contreras");

	template = gst_pad_template_new("sink", GST_PAD_SINK,
			GST_PAD_ALWAYS,
			generate_sink_template());

	gst_element_class_add_pad_template(element_class, template);

	gst_object_unref(template);
}

GType
gst_omapfb_sink_get_type(void)
{
	static GType type;

	if (G_UNLIKELY(type == 0)) {
		GTypeInfo type_info = {
			.class_size = sizeof(struct gst_omapfb_sink_class),
			.class_init = class_init,
			.base_init = base_init,
			.instance_size = sizeof(struct gst_omapfb_sink),
		};

		type = g_type_register_static(GST_TYPE_BASE_SINK, "GstOmapFbSink", &type_info, 0);
	}

	return type;
}

static gboolean
plugin_init(GstPlugin *plugin)
{
#ifndef GST_DISABLE_GST_DEBUG
	omapfb_debug = _gst_debug_category_new("omapfb", 0, "omapfb");
#endif

	if (!gst_element_register(plugin, "omapfbsink", GST_RANK_SECONDARY, GST_OMAPFB_SINK_TYPE))
		return false;

	return true;
}

GstPluginDesc gst_plugin_desc = {
	.major_version = GST_VERSION_MAJOR,
	.minor_version = GST_VERSION_MINOR,
	.name = "omapfb",
	.description = (gchar *) "Linux OMAP framebuffer",
	.plugin_init = plugin_init,
	.version = VERSION,
	.license = "LGPL",
	.source = "source",
	.package = "package",
	.origin = "origin",
};
