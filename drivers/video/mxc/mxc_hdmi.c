/*
 * Copyright (C) 2011-2014 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */
/*
 * SH-Mobile High-Definition Multimedia Interface (HDMI) driver
 * for SLISHDMI13T and SLIPHDMIT IP cores
 *
 * Copyright (C) 2010, Guennadi Liakhovetski <g.liakhovetski@gmx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/uaccess.h>
#include <linux/cpufreq.h>
#include <linux/firmware.h>
#include <linux/kthread.h>
#include <linux/regulator/driver.h>
#include <linux/fsl_devices.h>
#include <linux/ipu.h>
#include <linux/regmap.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of_device.h>
#include <linux/string.h>

#include <linux/firmware.h>
#include <linux/console.h>
#include <linux/types.h>

#include "../fbdev/edid.h"
#include <video/mxc_edid.h>
#include <video/mxc_hdmi.h>
#include "mxc_dispdrv.h"

#include <linux/mfd/mxc-hdmi-core.h>

#include <linux/hdmi.h>

#define DISPDRV_HDMI	"hdmi"
#define HDMI_EDID_LEN		512

/* status codes for reading edid */
#define HDMI_EDID_SUCCESS	0
#define HDMI_EDID_FAIL		-1
#define HDMI_EDID_SAME		-2
#define HDMI_EDID_NO_MODES	-3

#define NUM_CEA_VIDEO_MODES	64
#define DEFAULT_VIDEO_MODE	16 /* 1080P */

#define RGB			0
#define YCBCR444		1
#define YCBCR422_16BITS		2
#define YCBCR422_8BITS		3
#define XVYCC444            4

#define ALIGN2(x, a)		( ALIGN(x, a) != x ? ALIGN(x, a) - a : x )

/*
 * We follow a flowchart which is in the "Synopsys DesignWare Courses
 * HDMI Transmitter Controller User Guide, 1.30a", section 3.1
 * (dwc_hdmi_tx_user.pdf)
 *
 * Below are notes that say "HDMI Initialization Step X"
 * These correspond to the flowchart.
 */

/*
 * We are required to configure VGA mode before reading edid
 * in HDMI Initialization Step B
 */
static const struct fb_videomode vga_mode = {
	/* 640x480 @ 60 Hz, 31.5 kHz hsync */
	NULL, 60, 640, 480, 39721, 48, 16, 33, 10, 96, 2, 0,
	FB_VMODE_NONINTERLACED | FB_VMODE_ASPECT_4_3, FB_MODE_IS_VESA,
};

enum hdmi_datamap {
	RGB444_8B = 0x01,
	RGB444_10B = 0x03,
	RGB444_12B = 0x05,
	RGB444_16B = 0x07,
	YCbCr444_8B = 0x09,
	YCbCr444_10B = 0x0B,
	YCbCr444_12B = 0x0D,
	YCbCr444_16B = 0x0F,
	YCbCr422_8B = 0x16,
	YCbCr422_10B = 0x14,
	YCbCr422_12B = 0x12,
};

enum hdmi_mxc_colorimetry {
	eITU601,
	eITU709,
};

struct hdmi_vmode {
	bool mDVI;
	bool mHSyncPolarity;
	bool mVSyncPolarity;
	bool mInterlaced;
	bool mDataEnablePolarity;

	unsigned int mPixelClock;
	unsigned int mPixelRepetitionInput;
	unsigned int mPixelRepetitionOutput;
};

struct hdmi_data_info {
	unsigned int enc_in_format;
	unsigned int enc_out_format;
	unsigned int enc_color_depth;
	unsigned int colorimetry;
	unsigned int pix_repet_factor;
	unsigned int hdcp_enable;
	unsigned int rgb_out_enable;
	unsigned int rgb_quant_range;
	unsigned int enable_3d;
	unsigned int enable_fract;
	char *edid_filename;
	struct hdmi_vmode video_mode;
};

struct hdmi_phy_reg_config {
	/* HDMI PHY register config for pass HCT */
	u16 reg_vlev;
	u16 reg_cksymtx;
	u16 reg_txterm;
};

enum hotplug_state {
	HDMI_HOTPLUG_DISCONNECTED,
	HDMI_HOTPLUG_CONNECTED_NO_EDID,
	HDMI_HOTPLUG_CONNECTED_DVI,
	HDMI_HOTPLUG_CONNECTED_HDMI,
};

struct mxc_hdmi {
	struct platform_device *pdev;
	struct platform_device *core_pdev;
	struct mxc_dispdrv_handle *disp_mxc_hdmi;
	struct fb_info *fbi;
	struct clk *hdmi_isfr_clk;
	struct clk *hdmi_iahb_clk;
	struct clk *mipi_core_clk;
	struct timer_list jitter_timer;
	struct work_struct hotplug_work;
	struct work_struct hdmi_connected;
	struct delayed_work hdcp_hdp_work;

	struct notifier_block nb;

	struct hdmi_data_info hdmi_data;
	int vic;
	struct mxc_edid_cfg edid_cfg;
	u8 edid[HDMI_EDID_LEN];
	bool fb_reg;
	enum hotplug_state hp_state;
	u8 blank;
	bool dft_mode_set;
	char *dft_mode_str;
	int default_bpp;
	u8 latest_intr_stat;
	u8 plug_event;
	u8 plug_mask;
	bool irq_enabled;
	spinlock_t irq_lock;
	struct mutex m_lock;
	bool phy_enabled;
	struct fb_videomode default_mode;
	struct fb_var_screeninfo previous_non_vga_mode;
	bool requesting_vga_for_initialization;

	int *gpr_base;
	int *gpr_hdmi_base;
	int *gpr_sdma_base;
	int cpu_type;
	int cpu_version;
	struct hdmi_phy_reg_config phy_config;

	struct pinctrl *pinctrl;
};

static int hdmi_major;
static struct class *hdmi_class;

struct i2c_client *hdmi_i2c;
struct mxc_hdmi *g_hdmi;

static bool hdmi_inited;
static bool hdcp_init;

extern const struct fb_videomode mxc_cea_mode[64];
extern void mxc_hdmi_cec_handle(u32 cec_stat);

extern int mxcfb_blank(int blank, struct fb_info *info);

static void mxc_hdmi_setup(struct mxc_hdmi *hdmi, unsigned long event);
static void hdmi_enable_overflow_interrupts(void);
static void hdmi_disable_overflow_interrupts(void);
static void mxc_hdmi_edid_rebuild_modelist(struct mxc_hdmi *hdmi);
static void mxc_hdmi_default_edid_cfg(struct mxc_hdmi *hdmi);
static void mxc_hdmi_default_modelist(struct mxc_hdmi *hdmi);
static void mxc_hdmi_set_mode(struct mxc_hdmi *hdmi, int edid_status);
static unsigned int getRGBQuantRange(struct mxc_hdmi *hdmi);

static char *rgb_quant_range = "auto";
module_param(rgb_quant_range, charp, S_IRUGO);
MODULE_PARM_DESC(rgb_quant_range, "RGB Quant Range (auto, default, limited, full)");

static bool ignore_edid = 0;
module_param(ignore_edid, bool, S_IRUGO);
MODULE_PARM_DESC(ignore_edid, "Ignore EDID (default=0)");

static char *ext_edid = "";
module_param(ext_edid, charp, S_IRUGO);
MODULE_PARM_DESC(ext_edid, "file name to load EDID from");

static char *enable_3d = "1";
module_param(enable_3d, charp, S_IRUGO);
MODULE_PARM_DESC(enable_3d, "3D modes enabled (0/1)");

static char *enable_fract = "1";
module_param(enable_fract, charp, S_IRUGO);
MODULE_PARM_DESC(enable_fract, "Fractional modes enabled (0/1)");

static struct platform_device_id imx_hdmi_devtype[] = {
	{
		.name = "hdmi-imx6DL",
		.driver_data = IMX6DL_HDMI,
	}, {
		.name = "hdmi-imx6Q",
		.driver_data = IMX6Q_HDMI,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(platform, imx_hdmi_devtype);

static const struct of_device_id imx_hdmi_dt_ids[] = {
	{ .compatible = "fsl,imx6dl-hdmi-video", .data = &imx_hdmi_devtype[IMX6DL_HDMI], },
	{ .compatible = "fsl,imx6q-hdmi-video", .data = &imx_hdmi_devtype[IMX6Q_HDMI], },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx_hdmi_dt_ids);

static inline int cpu_is_imx6dl(struct mxc_hdmi *hdmi)
{
	return hdmi->cpu_type == IMX6DL_HDMI;
}

static inline void get_refresh_str(struct fb_videomode *m, char *refresh)
{
	snprintf(refresh, 10, "%u.%uHz", m->refresh - (int)(m->vmode & FB_VMODE_FRACTIONAL ? 1 : 0),
				m->refresh * (int)(m->vmode & FB_VMODE_FRACTIONAL ? 999 : 1000) % 1000);
}

static void dump_fb_videomode(struct fb_videomode *m)
{
	char refresh[10];

	get_refresh_str(m, refresh);
	pr_debug("fb_videomode = %ux%u%c-%s (%ups/%lukHz) %u %u %u %u %u %u %u %u %u\n",
		m->xres, m->yres, m->vmode & FB_VMODE_INTERLACED ? 'i' : 'p',
		refresh, m->pixclock, mxcPICOS2KHZ(m->pixclock, m->vmode), m->left_margin,
		m->right_margin, m->upper_margin, m->lower_margin,
		m->hsync_len, m->vsync_len, m->sync, m->vmode, m->flag);
}

static ssize_t mxc_hdmi_show_name(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mxc_hdmi *hdmi = dev_get_drvdata(dev);

	strcpy(buf, hdmi->fbi->fix.id);
	sprintf(buf+strlen(buf), "\n");

	return strlen(buf);
}

static DEVICE_ATTR(fb_name, S_IRUGO, mxc_hdmi_show_name, NULL);

static ssize_t mxc_hdmi_show_state(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mxc_hdmi *hdmi = dev_get_drvdata(dev);

	switch (hdmi->hp_state)
	{
	case HDMI_HOTPLUG_CONNECTED_HDMI:
		strcpy(buf, "plugin HDMI\n");
		break;
	case HDMI_HOTPLUG_CONNECTED_DVI:
		strcpy(buf, "plugin DVI\n");
		break;
	case HDMI_HOTPLUG_CONNECTED_NO_EDID: 
		strcpy(buf, "plugin NO EDID\n");
		break;
	case HDMI_HOTPLUG_DISCONNECTED:
	default:
		strcpy(buf, "plugout\n");
	}

	return strlen(buf);
}

static DEVICE_ATTR(cable_state, S_IRUGO, mxc_hdmi_show_state, NULL);

static ssize_t mxc_hdmi_show_edid(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mxc_hdmi *hdmi = dev_get_drvdata(dev);
	int i, j, len = 0;

	for (j = 0; j < HDMI_EDID_LEN/16; j++) {
		for (i = 0; i < 16; i++)
			len += sprintf(buf+len, "0x%02X ",
					hdmi->edid[j*16 + i]);
		len += sprintf(buf+len, "\n");
	}

	return len;
}

static DEVICE_ATTR(edid, S_IRUGO, mxc_hdmi_show_edid, NULL);

static ssize_t mxc_hdmi_show_rgb_out_enable(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mxc_hdmi *hdmi = dev_get_drvdata(dev);

	if (hdmi->hdmi_data.rgb_out_enable == true)
		strcpy(buf, "RGB out\n");
	else
		strcpy(buf, "YCbCr out\n");

	return strlen(buf);
}

static ssize_t mxc_hdmi_store_rgb_out_enable(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mxc_hdmi *hdmi = dev_get_drvdata(dev);
	unsigned long value;
	int ret;

	ret = kstrtol(buf, 10, &value);
	if (ret)
		return ret;

	hdmi->hdmi_data.rgb_out_enable = value;

	/* Reconfig HDMI for output color space change */
	mxc_hdmi_setup(hdmi, 0);

	return count;
}

static DEVICE_ATTR(rgb_out_enable, S_IRUGO | S_IWUSR,
				mxc_hdmi_show_rgb_out_enable,
				mxc_hdmi_store_rgb_out_enable);

static ssize_t mxc_hdmi_show_rgb_quant_range(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mxc_hdmi *hdmi = dev_get_drvdata(dev);
	int n;

	switch (getRGBQuantRange(hdmi)) {
	case HDMI_FC_AVICONF2_RGB_QUANT_LIMITED_RANGE: 
		strcpy(buf, "limited\n");
		break;
	case HDMI_FC_AVICONF2_RGB_QUANT_FULL_RANGE: 
		strcpy(buf, "full\n");
		break;
	case HDMI_FC_AVICONF2_RGB_QUANT_DEFAULT: 
	default:
		strcpy(buf, "default\n");
		break;
	};

	n = strlen(buf);

	if (hdmi->hdmi_data.rgb_quant_range == HDMI_FC_AVICONF2_RGB_QUANT_MASK) {
		strcpy(buf + n - 1, " (auto)\n");
		n += 7;
	}

	return n;
}

static ssize_t mxc_hdmi_store_rgb_quant_range(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mxc_hdmi *hdmi = dev_get_drvdata(dev);
	int ret = count;

	if (sysfs_streq("limited", buf)) {
		hdmi->hdmi_data.rgb_quant_range = HDMI_FC_AVICONF2_RGB_QUANT_LIMITED_RANGE;
	} else if (sysfs_streq("full", buf)) {
		hdmi->hdmi_data.rgb_quant_range = HDMI_FC_AVICONF2_RGB_QUANT_FULL_RANGE;
	} else if (sysfs_streq("default", buf)) {
		hdmi->hdmi_data.rgb_quant_range = HDMI_FC_AVICONF2_RGB_QUANT_DEFAULT;
	} else if (sysfs_streq("auto", buf)) {
		hdmi->hdmi_data.rgb_quant_range = HDMI_FC_AVICONF2_RGB_QUANT_MASK;
	} else {
		ret = -EINVAL;
		goto out;
	}

	/* Reconfig HDMI for output RGB Quant Range change if using RGB out */
	if(hdmi->hdmi_data.rgb_out_enable)
		mxc_hdmi_setup(hdmi, 0);
out:
	return ret;
}

static DEVICE_ATTR(rgb_quant_range, S_IRUGO | S_IWUSR,
				mxc_hdmi_show_rgb_quant_range,
				mxc_hdmi_store_rgb_quant_range);

static ssize_t mxc_hdmi_show_enable_3d(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mxc_hdmi *hdmi = dev_get_drvdata(dev);

	switch (hdmi->hdmi_data.enable_3d) {
	case 0:
		strcpy(buf, "disabled\n");
		break;
	default:
		strcpy(buf, "enabled\n");
		break;
	};

	return strlen(buf);
}

static ssize_t mxc_hdmi_store_enable_3d(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mxc_hdmi *hdmi = dev_get_drvdata(dev);
	int ret = count;

	if (sysfs_streq("disable", buf)) {
		hdmi->hdmi_data.enable_3d = 0;
	} else if (sysfs_streq("0", buf)) {
		hdmi->hdmi_data.enable_3d = 0;
	} else {
		hdmi->hdmi_data.enable_3d = 1;
	}

	mxc_hdmi_edid_rebuild_modelist(hdmi);
	if (hdmi->hp_state > HDMI_HOTPLUG_DISCONNECTED)
		mxc_hdmi_set_mode(hdmi, HDMI_EDID_SUCCESS);

	return ret;
}

static DEVICE_ATTR(enable_3d, S_IRUGO | S_IWUSR,
				mxc_hdmi_show_enable_3d,
				mxc_hdmi_store_enable_3d);

static ssize_t mxc_hdmi_show_enable_fract(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mxc_hdmi *hdmi = dev_get_drvdata(dev);

	switch (hdmi->hdmi_data.enable_fract) {
	case 0:
		strcpy(buf, "disabled\n");
		break;
	default:
		strcpy(buf, "enabled\n");
		break;
	};

	return strlen(buf);
}

static ssize_t mxc_hdmi_store_enable_fract(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mxc_hdmi *hdmi = dev_get_drvdata(dev);
	int ret = count;

	if (sysfs_streq("disable", buf)) {
		hdmi->hdmi_data.enable_fract = 0;
	} else if (sysfs_streq("0", buf)) {
		hdmi->hdmi_data.enable_fract = 0;
	} else {
		hdmi->hdmi_data.enable_fract = 1;
	}

	mxc_hdmi_edid_rebuild_modelist(hdmi);
	if (hdmi->hp_state > HDMI_HOTPLUG_DISCONNECTED)
		mxc_hdmi_set_mode(hdmi, HDMI_EDID_SUCCESS);

	return ret;
}

static DEVICE_ATTR(enable_fract, S_IRUGO | S_IWUSR,
				mxc_hdmi_show_enable_fract,
				mxc_hdmi_store_enable_fract);

static ssize_t mxc_hdmi_show_hdcp_enable(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mxc_hdmi *hdmi = dev_get_drvdata(dev);

	if (hdmi->hdmi_data.hdcp_enable == false)
		strcpy(buf, "hdcp disable\n");
	else
		strcpy(buf, "hdcp enable\n");

	return strlen(buf);

}

static ssize_t mxc_hdmi_store_hdcp_enable(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mxc_hdmi *hdmi = dev_get_drvdata(dev);
	char event_string[32];
	char *envp[] = { event_string, NULL };
	unsigned long value;
	int ret;

	ret = kstrtol(buf, 10, &value);
	if (ret)
		return ret;

	hdmi->hdmi_data.hdcp_enable = value;

	/* Reconfig HDMI for HDCP */
	mxc_hdmi_setup(hdmi, 0);

	if (hdmi->hdmi_data.hdcp_enable == false) {
		sprintf(event_string, "EVENT=hdcpdisable");
		kobject_uevent_env(&hdmi->pdev->dev.kobj, KOBJ_CHANGE, envp);
	} else {
		sprintf(event_string, "EVENT=hdcpenable");
		kobject_uevent_env(&hdmi->pdev->dev.kobj, KOBJ_CHANGE, envp);
	}

	return count;

}

static DEVICE_ATTR(hdcp_enable, S_IRUGO | S_IWUSR,
			mxc_hdmi_show_hdcp_enable, mxc_hdmi_store_hdcp_enable);

/*!
 * this submodule is responsible for the video data synchronization.
 * for example, for RGB 4:4:4 input, the data map is defined as
 *			pin{47~40} <==> R[7:0]
 *			pin{31~24} <==> G[7:0]
 *			pin{15~8}  <==> B[7:0]
 */
static void hdmi_video_sample(struct mxc_hdmi *hdmi)
{
	int color_format = 0;
	u8 val;

	if (hdmi->hdmi_data.enc_in_format == RGB) {
		if (hdmi->hdmi_data.enc_color_depth == 8)
			color_format = 0x01;
		else if (hdmi->hdmi_data.enc_color_depth == 10)
			color_format = 0x03;
		else if (hdmi->hdmi_data.enc_color_depth == 12)
			color_format = 0x05;
		else if (hdmi->hdmi_data.enc_color_depth == 16)
			color_format = 0x07;
		else
			return;
	} else if (hdmi->hdmi_data.enc_in_format == YCBCR444) {
		if (hdmi->hdmi_data.enc_color_depth == 8)
			color_format = 0x09;
		else if (hdmi->hdmi_data.enc_color_depth == 10)
			color_format = 0x0B;
		else if (hdmi->hdmi_data.enc_color_depth == 12)
			color_format = 0x0D;
		else if (hdmi->hdmi_data.enc_color_depth == 16)
			color_format = 0x0F;
		else
			return;
	} else if (hdmi->hdmi_data.enc_in_format == YCBCR422_8BITS) {
		if (hdmi->hdmi_data.enc_color_depth == 8)
			color_format = 0x16;
		else if (hdmi->hdmi_data.enc_color_depth == 10)
			color_format = 0x14;
		else if (hdmi->hdmi_data.enc_color_depth == 12)
			color_format = 0x12;
		else
			return;
	}

	val = HDMI_TX_INVID0_INTERNAL_DE_GENERATOR_DISABLE |
		((color_format << HDMI_TX_INVID0_VIDEO_MAPPING_OFFSET) &
		HDMI_TX_INVID0_VIDEO_MAPPING_MASK);
	hdmi_writeb(val, HDMI_TX_INVID0);

	/* Enable TX stuffing: When DE is inactive, fix the output data to 0 */
	val = HDMI_TX_INSTUFFING_BDBDATA_STUFFING_ENABLE |
		HDMI_TX_INSTUFFING_RCRDATA_STUFFING_ENABLE |
		HDMI_TX_INSTUFFING_GYDATA_STUFFING_ENABLE;
	hdmi_writeb(val, HDMI_TX_INSTUFFING);
	hdmi_writeb(0x0, HDMI_TX_GYDATA0);
	hdmi_writeb(0x0, HDMI_TX_GYDATA1);
	hdmi_writeb(0x0, HDMI_TX_RCRDATA0);
	hdmi_writeb(0x0, HDMI_TX_RCRDATA1);
	hdmi_writeb(0x0, HDMI_TX_BCBDATA0);
	hdmi_writeb(0x0, HDMI_TX_BCBDATA1);
}

static unsigned int getRGBQuantRange(struct mxc_hdmi *hdmi)
{
	if (hdmi->hdmi_data.rgb_quant_range != HDMI_FC_AVICONF2_RGB_QUANT_MASK)
	      return hdmi->hdmi_data.rgb_quant_range;

	return hdmi->edid_cfg.cea_rgb_range_selectable ?
		HDMI_FC_AVICONF2_RGB_QUANT_FULL_RANGE : HDMI_FC_AVICONF2_RGB_QUANT_DEFAULT;
}

static int isColorSpaceConversion(struct mxc_hdmi *hdmi)
{
	unsigned int rgb_quant_range = getRGBQuantRange(hdmi);

	return (hdmi->hdmi_data.enc_in_format != hdmi->hdmi_data.enc_out_format) ||
		(hdmi->hdmi_data.enc_out_format == RGB &&
		  ((rgb_quant_range == HDMI_FC_AVICONF2_RGB_QUANT_LIMITED_RANGE) ||
		   (rgb_quant_range == HDMI_FC_AVICONF2_RGB_QUANT_DEFAULT && hdmi->vic > 1)));
}

static int isColorSpaceDecimation(struct mxc_hdmi *hdmi)
{
	return ((hdmi->hdmi_data.enc_out_format == YCBCR422_8BITS) &&
		(hdmi->hdmi_data.enc_in_format == RGB ||
		hdmi->hdmi_data.enc_in_format == YCBCR444));
}

static int isColorSpaceInterpolation(struct mxc_hdmi *hdmi)
{
	return ((hdmi->hdmi_data.enc_in_format == YCBCR422_8BITS) &&
		(hdmi->hdmi_data.enc_out_format == RGB
		|| hdmi->hdmi_data.enc_out_format == YCBCR444));
}

/*!
 * update the color space conversion coefficients.
 */
static void update_csc_coeffs(struct mxc_hdmi *hdmi)
{
	unsigned short csc_coeff[3][4];
	unsigned int csc_scale = 1;
	u8 val;
	bool coeff_selected = false;

	if (isColorSpaceConversion(hdmi)) { /* csc needed */
		if (hdmi->hdmi_data.enc_out_format == RGB) {
			if (hdmi->hdmi_data.enc_in_format == RGB) {
				csc_coeff[0][0] = 0x1b80;
				csc_coeff[0][1] = 0x0000;
				csc_coeff[0][2] = 0x0000;
				csc_coeff[0][3] = 0x0020;

				csc_coeff[1][0] = 0x0000;
				csc_coeff[1][1] = 0x1b80;
				csc_coeff[1][2] = 0x0000;
				csc_coeff[1][3] = 0x0020;

				csc_coeff[2][0] = 0x0000;
				csc_coeff[2][1] = 0x0000;
				csc_coeff[2][2] = 0x1b80;
				csc_coeff[2][3] = 0x0020;

				csc_scale = 1;
				coeff_selected = true;
			} else if (hdmi->hdmi_data.colorimetry == eITU601) {
				csc_coeff[0][0] = 0x2000;
				csc_coeff[0][1] = 0x6926;
				csc_coeff[0][2] = 0x74fd;
				csc_coeff[0][3] = 0x010e;

				csc_coeff[1][0] = 0x2000;
				csc_coeff[1][1] = 0x2cdd;
				csc_coeff[1][2] = 0x0000;
				csc_coeff[1][3] = 0x7e9a;

				csc_coeff[2][0] = 0x2000;
				csc_coeff[2][1] = 0x0000;
				csc_coeff[2][2] = 0x38b4;
				csc_coeff[2][3] = 0x7e3b;

				csc_scale = 1;
				coeff_selected = true;
			} else if (hdmi->hdmi_data.colorimetry == eITU709) {
				csc_coeff[0][0] = 0x2000;
				csc_coeff[0][1] = 0x7106;
				csc_coeff[0][2] = 0x7a02;
				csc_coeff[0][3] = 0x00a7;

				csc_coeff[1][0] = 0x2000;
				csc_coeff[1][1] = 0x3264;
				csc_coeff[1][2] = 0x0000;
				csc_coeff[1][3] = 0x7e6d;

				csc_coeff[2][0] = 0x2000;
				csc_coeff[2][1] = 0x0000;
				csc_coeff[2][2] = 0x3b61;
				csc_coeff[2][3] = 0x7e25;

				csc_scale = 1;
				coeff_selected = true;
			}
		} else if (hdmi->hdmi_data.enc_in_format == RGB) {
			if (hdmi->hdmi_data.colorimetry == eITU601) {
				csc_coeff[0][0] = 0x2591;
				csc_coeff[0][1] = 0x1322;
				csc_coeff[0][2] = 0x074b;
				csc_coeff[0][3] = 0x0000;

				csc_coeff[1][0] = 0x6535;
				csc_coeff[1][1] = 0x2000;
				csc_coeff[1][2] = 0x7acc;
				csc_coeff[1][3] = 0x0200;

				csc_coeff[2][0] = 0x6acd;
				csc_coeff[2][1] = 0x7534;
				csc_coeff[2][2] = 0x2000;
				csc_coeff[2][3] = 0x0200;

				csc_scale = 0;
				coeff_selected = true;
			} else if (hdmi->hdmi_data.colorimetry == eITU709) {
				csc_coeff[0][0] = 0x2dc5;
				csc_coeff[0][1] = 0x0d9b;
				csc_coeff[0][2] = 0x049e;
				csc_coeff[0][3] = 0x0000;

				csc_coeff[1][0] = 0x62f0;
				csc_coeff[1][1] = 0x2000;
				csc_coeff[1][2] = 0x7d11;
				csc_coeff[1][3] = 0x0200;

				csc_coeff[2][0] = 0x6756;
				csc_coeff[2][1] = 0x78ab;
				csc_coeff[2][2] = 0x2000;
				csc_coeff[2][3] = 0x0200;

				csc_scale = 0;
				coeff_selected = true;
			}
		}
	}

	if (!coeff_selected) {
		csc_coeff[0][0] = 0x2000;
		csc_coeff[0][1] = 0x0000;
		csc_coeff[0][2] = 0x0000;
		csc_coeff[0][3] = 0x0000;

		csc_coeff[1][0] = 0x0000;
		csc_coeff[1][1] = 0x2000;
		csc_coeff[1][2] = 0x0000;
		csc_coeff[1][3] = 0x0000;

		csc_coeff[2][0] = 0x0000;
		csc_coeff[2][1] = 0x0000;
		csc_coeff[2][2] = 0x2000;
		csc_coeff[2][3] = 0x0000;

		csc_scale = 1;
	}

	/* Update CSC parameters in HDMI CSC registers */
	hdmi_writeb((unsigned char)(csc_coeff[0][0] & 0xFF),
		HDMI_CSC_COEF_A1_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[0][0] >> 8),
		HDMI_CSC_COEF_A1_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[0][1] & 0xFF),
		HDMI_CSC_COEF_A2_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[0][1] >> 8),
		HDMI_CSC_COEF_A2_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[0][2] & 0xFF),
		HDMI_CSC_COEF_A3_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[0][2] >> 8),
		HDMI_CSC_COEF_A3_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[0][3] & 0xFF),
		HDMI_CSC_COEF_A4_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[0][3] >> 8),
		HDMI_CSC_COEF_A4_MSB);

	hdmi_writeb((unsigned char)(csc_coeff[1][0] & 0xFF),
		HDMI_CSC_COEF_B1_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[1][0] >> 8),
		HDMI_CSC_COEF_B1_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[1][1] & 0xFF),
		HDMI_CSC_COEF_B2_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[1][1] >> 8),
		HDMI_CSC_COEF_B2_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[1][2] & 0xFF),
		HDMI_CSC_COEF_B3_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[1][2] >> 8),
		HDMI_CSC_COEF_B3_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[1][3] & 0xFF),
		HDMI_CSC_COEF_B4_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[1][3] >> 8),
		HDMI_CSC_COEF_B4_MSB);

	hdmi_writeb((unsigned char)(csc_coeff[2][0] & 0xFF),
		HDMI_CSC_COEF_C1_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[2][0] >> 8),
		HDMI_CSC_COEF_C1_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[2][1] & 0xFF),
		HDMI_CSC_COEF_C2_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[2][1] >> 8),
		HDMI_CSC_COEF_C2_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[2][2] & 0xFF),
		HDMI_CSC_COEF_C3_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[2][2] >> 8),
		HDMI_CSC_COEF_C3_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[2][3] & 0xFF),
		HDMI_CSC_COEF_C4_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[2][3] >> 8),
		HDMI_CSC_COEF_C4_MSB);

	val = hdmi_readb(HDMI_CSC_SCALE);
	val &= ~HDMI_CSC_SCALE_CSCSCALE_MASK;
	val |= csc_scale & HDMI_CSC_SCALE_CSCSCALE_MASK;
	hdmi_writeb(val, HDMI_CSC_SCALE);
}

static void hdmi_video_csc(struct mxc_hdmi *hdmi)
{
	int color_depth = 0;
	int interpolation = HDMI_CSC_CFG_INTMODE_DISABLE;
	int decimation = HDMI_CSC_CFG_DECMODE_DISABLE;
	u8 val;

	/* YCC422 interpolation to 444 mode */
	if (isColorSpaceInterpolation(hdmi))
		interpolation = HDMI_CSC_CFG_INTMODE_CHROMA_INT_FORMULA1;
	else if (isColorSpaceDecimation(hdmi))
		decimation = HDMI_CSC_CFG_DECMODE_CHROMA_INT_FORMULA3;

	if (hdmi->hdmi_data.enc_color_depth == 8)
		color_depth = HDMI_CSC_SCALE_CSC_COLORDE_PTH_24BPP;
	else if (hdmi->hdmi_data.enc_color_depth == 10)
		color_depth = HDMI_CSC_SCALE_CSC_COLORDE_PTH_30BPP;
	else if (hdmi->hdmi_data.enc_color_depth == 12)
		color_depth = HDMI_CSC_SCALE_CSC_COLORDE_PTH_36BPP;
	else if (hdmi->hdmi_data.enc_color_depth == 16)
		color_depth = HDMI_CSC_SCALE_CSC_COLORDE_PTH_48BPP;
	else
		return;

	/*configure the CSC registers */
	hdmi_writeb(interpolation | decimation, HDMI_CSC_CFG);
	val = hdmi_readb(HDMI_CSC_SCALE);
	val &= ~HDMI_CSC_SCALE_CSC_COLORDE_PTH_MASK;
	val |= color_depth;
	hdmi_writeb(val, HDMI_CSC_SCALE);

	update_csc_coeffs(hdmi);
}

/*!
 * HDMI video packetizer is used to packetize the data.
 * for example, if input is YCC422 mode or repeater is used,
 * data should be repacked this module can be bypassed.
 */
static void hdmi_video_packetize(struct mxc_hdmi *hdmi)
{
	unsigned int color_depth = 0;
	unsigned int remap_size = HDMI_VP_REMAP_YCC422_16bit;
	unsigned int output_select = HDMI_VP_CONF_OUTPUT_SELECTOR_PP;
	struct hdmi_data_info *hdmi_data = &hdmi->hdmi_data;
	u8 val;

	if (hdmi_data->enc_out_format == RGB
		|| hdmi_data->enc_out_format == YCBCR444) {
		if (hdmi_data->enc_color_depth == 0)
			output_select = HDMI_VP_CONF_OUTPUT_SELECTOR_BYPASS;
		else if (hdmi_data->enc_color_depth == 8) {
			color_depth = 4;
			output_select = HDMI_VP_CONF_OUTPUT_SELECTOR_BYPASS;
		} else if (hdmi_data->enc_color_depth == 10)
			color_depth = 5;
		else if (hdmi_data->enc_color_depth == 12)
			color_depth = 6;
		else if (hdmi_data->enc_color_depth == 16)
			color_depth = 7;
		else
			return;
	} else if (hdmi_data->enc_out_format == YCBCR422_8BITS) {
		if (hdmi_data->enc_color_depth == 0 ||
			hdmi_data->enc_color_depth == 8)
			remap_size = HDMI_VP_REMAP_YCC422_16bit;
		else if (hdmi_data->enc_color_depth == 10)
			remap_size = HDMI_VP_REMAP_YCC422_20bit;
		else if (hdmi_data->enc_color_depth == 12)
			remap_size = HDMI_VP_REMAP_YCC422_24bit;
		else
			return;
		output_select = HDMI_VP_CONF_OUTPUT_SELECTOR_YCC422;
	} else
		return;

	/* HDMI not support deep color,
	 * because IPU MAX support color depth is 24bit */
	color_depth = 0;

	/* set the packetizer registers */
	val = ((color_depth << HDMI_VP_PR_CD_COLOR_DEPTH_OFFSET) &
		HDMI_VP_PR_CD_COLOR_DEPTH_MASK) |
		((hdmi_data->pix_repet_factor <<
		HDMI_VP_PR_CD_DESIRED_PR_FACTOR_OFFSET) &
		HDMI_VP_PR_CD_DESIRED_PR_FACTOR_MASK);
	hdmi_writeb(val, HDMI_VP_PR_CD);

	val = hdmi_readb(HDMI_VP_STUFF);
	val &= ~HDMI_VP_STUFF_PR_STUFFING_MASK;
	val |= HDMI_VP_STUFF_PR_STUFFING_STUFFING_MODE;
	hdmi_writeb(val, HDMI_VP_STUFF);

	/* Data from pixel repeater block */
	if (hdmi_data->pix_repet_factor > 1) {
		val = hdmi_readb(HDMI_VP_CONF);
		val &= ~(HDMI_VP_CONF_PR_EN_MASK |
			HDMI_VP_CONF_BYPASS_SELECT_MASK);
		val |= HDMI_VP_CONF_PR_EN_ENABLE |
			HDMI_VP_CONF_BYPASS_SELECT_PIX_REPEATER;
		hdmi_writeb(val, HDMI_VP_CONF);
	} else { /* data from packetizer block */
		val = hdmi_readb(HDMI_VP_CONF);
		val &= ~(HDMI_VP_CONF_PR_EN_MASK |
			HDMI_VP_CONF_BYPASS_SELECT_MASK);
		val |= HDMI_VP_CONF_PR_EN_DISABLE |
			HDMI_VP_CONF_BYPASS_SELECT_VID_PACKETIZER;
		hdmi_writeb(val, HDMI_VP_CONF);
	}

	val = hdmi_readb(HDMI_VP_STUFF);
	val &= ~HDMI_VP_STUFF_IDEFAULT_PHASE_MASK;
	val |= 1 << HDMI_VP_STUFF_IDEFAULT_PHASE_OFFSET;
	hdmi_writeb(val, HDMI_VP_STUFF);

	hdmi_writeb(remap_size, HDMI_VP_REMAP);

	if (output_select == HDMI_VP_CONF_OUTPUT_SELECTOR_PP) {
		val = hdmi_readb(HDMI_VP_CONF);
		val &= ~(HDMI_VP_CONF_BYPASS_EN_MASK |
			HDMI_VP_CONF_PP_EN_ENMASK |
			HDMI_VP_CONF_YCC422_EN_MASK);
		val |= HDMI_VP_CONF_BYPASS_EN_DISABLE |
			HDMI_VP_CONF_PP_EN_ENABLE |
			HDMI_VP_CONF_YCC422_EN_DISABLE;
		hdmi_writeb(val, HDMI_VP_CONF);
	} else if (output_select == HDMI_VP_CONF_OUTPUT_SELECTOR_YCC422) {
		val = hdmi_readb(HDMI_VP_CONF);
		val &= ~(HDMI_VP_CONF_BYPASS_EN_MASK |
			HDMI_VP_CONF_PP_EN_ENMASK |
			HDMI_VP_CONF_YCC422_EN_MASK);
		val |= HDMI_VP_CONF_BYPASS_EN_DISABLE |
			HDMI_VP_CONF_PP_EN_DISABLE |
			HDMI_VP_CONF_YCC422_EN_ENABLE;
		hdmi_writeb(val, HDMI_VP_CONF);
	} else if (output_select == HDMI_VP_CONF_OUTPUT_SELECTOR_BYPASS) {
		val = hdmi_readb(HDMI_VP_CONF);
		val &= ~(HDMI_VP_CONF_BYPASS_EN_MASK |
			HDMI_VP_CONF_PP_EN_ENMASK |
			HDMI_VP_CONF_YCC422_EN_MASK);
		val |= HDMI_VP_CONF_BYPASS_EN_ENABLE |
			HDMI_VP_CONF_PP_EN_DISABLE |
			HDMI_VP_CONF_YCC422_EN_DISABLE;
		hdmi_writeb(val, HDMI_VP_CONF);
	} else {
		return;
	}

	val = hdmi_readb(HDMI_VP_STUFF);
	val &= ~(HDMI_VP_STUFF_PP_STUFFING_MASK |
		HDMI_VP_STUFF_YCC422_STUFFING_MASK);
	val |= HDMI_VP_STUFF_PP_STUFFING_STUFFING_MODE |
		HDMI_VP_STUFF_YCC422_STUFFING_STUFFING_MODE;
	hdmi_writeb(val, HDMI_VP_STUFF);

	val = hdmi_readb(HDMI_VP_CONF);
	val &= ~HDMI_VP_CONF_OUTPUT_SELECTOR_MASK;
	val |= output_select;
	hdmi_writeb(val, HDMI_VP_CONF);
}

#if 0
/* Force a fixed color screen */
static void hdmi_video_force_output(struct mxc_hdmi *hdmi, unsigned char force)
{
	u8 val;

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	if (force) {
		hdmi_writeb(0x00, HDMI_FC_DBGTMDS2);   /* R */
		hdmi_writeb(0x00, HDMI_FC_DBGTMDS1);   /* G */
		hdmi_writeb(0xFF, HDMI_FC_DBGTMDS0);   /* B */
		val = hdmi_readb(HDMI_FC_DBGFORCE);
		val |= HDMI_FC_DBGFORCE_FORCEVIDEO;
		hdmi_writeb(val, HDMI_FC_DBGFORCE);
	} else {
		val = hdmi_readb(HDMI_FC_DBGFORCE);
		val &= ~HDMI_FC_DBGFORCE_FORCEVIDEO;
		hdmi_writeb(val, HDMI_FC_DBGFORCE);
		hdmi_writeb(0x00, HDMI_FC_DBGTMDS2);   /* R */
		hdmi_writeb(0x00, HDMI_FC_DBGTMDS1);   /* G */
		hdmi_writeb(0x00, HDMI_FC_DBGTMDS0);   /* B */
	}
}
#endif

static inline void hdmi_phy_test_clear(struct mxc_hdmi *hdmi,
						unsigned char bit)
{
	u8 val = hdmi_readb(HDMI_PHY_TST0);
	val &= ~HDMI_PHY_TST0_TSTCLR_MASK;
	val |= (bit << HDMI_PHY_TST0_TSTCLR_OFFSET) &
		HDMI_PHY_TST0_TSTCLR_MASK;
	hdmi_writeb(val, HDMI_PHY_TST0);
}

static inline void hdmi_phy_test_enable(struct mxc_hdmi *hdmi,
						unsigned char bit)
{
	u8 val = hdmi_readb(HDMI_PHY_TST0);
	val &= ~HDMI_PHY_TST0_TSTEN_MASK;
	val |= (bit << HDMI_PHY_TST0_TSTEN_OFFSET) &
		HDMI_PHY_TST0_TSTEN_MASK;
	hdmi_writeb(val, HDMI_PHY_TST0);
}

static inline void hdmi_phy_test_clock(struct mxc_hdmi *hdmi,
						unsigned char bit)
{
	u8 val = hdmi_readb(HDMI_PHY_TST0);
	val &= ~HDMI_PHY_TST0_TSTCLK_MASK;
	val |= (bit << HDMI_PHY_TST0_TSTCLK_OFFSET) &
		HDMI_PHY_TST0_TSTCLK_MASK;
	hdmi_writeb(val, HDMI_PHY_TST0);
}

static inline void hdmi_phy_test_din(struct mxc_hdmi *hdmi,
						unsigned char bit)
{
	hdmi_writeb(bit, HDMI_PHY_TST1);
}

static inline void hdmi_phy_test_dout(struct mxc_hdmi *hdmi,
						unsigned char bit)
{
	hdmi_writeb(bit, HDMI_PHY_TST2);
}

static bool hdmi_phy_wait_i2c_done(struct mxc_hdmi *hdmi, int msec)
{
	unsigned char val = 0;
	val = hdmi_readb(HDMI_IH_I2CMPHY_STAT0) & 0x3;
	while (val == 0) {
		udelay(1000);
		if (msec-- == 0)
			return false;
		val = hdmi_readb(HDMI_IH_I2CMPHY_STAT0) & 0x3;
	}
	return true;
}

static void hdmi_phy_i2c_write(struct mxc_hdmi *hdmi, unsigned short data,
			      unsigned char addr)
{
	hdmi_writeb(0xFF, HDMI_IH_I2CMPHY_STAT0);
	hdmi_writeb(addr, HDMI_PHY_I2CM_ADDRESS_ADDR);
	hdmi_writeb((unsigned char)(data >> 8),
		HDMI_PHY_I2CM_DATAO_1_ADDR);
	hdmi_writeb((unsigned char)(data >> 0),
		HDMI_PHY_I2CM_DATAO_0_ADDR);
	hdmi_writeb(HDMI_PHY_I2CM_OPERATION_ADDR_WRITE,
		HDMI_PHY_I2CM_OPERATION_ADDR);
	hdmi_phy_wait_i2c_done(hdmi, 1000);
}

#if 0
static unsigned short hdmi_phy_i2c_read(struct mxc_hdmi *hdmi,
					unsigned char addr)
{
	unsigned short data;
	unsigned char msb = 0, lsb = 0;
	hdmi_writeb(0xFF, HDMI_IH_I2CMPHY_STAT0);
	hdmi_writeb(addr, HDMI_PHY_I2CM_ADDRESS_ADDR);
	hdmi_writeb(HDMI_PHY_I2CM_OPERATION_ADDR_READ,
		HDMI_PHY_I2CM_OPERATION_ADDR);
	hdmi_phy_wait_i2c_done(hdmi, 1000);
	msb = hdmi_readb(HDMI_PHY_I2CM_DATAI_1_ADDR);
	lsb = hdmi_readb(HDMI_PHY_I2CM_DATAI_0_ADDR);
	data = (msb << 8) | lsb;
	return data;
}

static int hdmi_phy_i2c_write_verify(struct mxc_hdmi *hdmi, unsigned short data,
				     unsigned char addr)
{
	unsigned short val = 0;
	hdmi_phy_i2c_write(hdmi, data, addr);
	val = hdmi_phy_i2c_read(hdmi, addr);
	return (val == data);
}
#endif

static bool  hdmi_edid_wait_i2c_done(struct mxc_hdmi *hdmi, int msec)
{
    unsigned char val = 0;
    val = hdmi_readb(HDMI_IH_I2CM_STAT0) & 0x2;
    while (val == 0) {

		udelay(1000);
		if (msec-- == 0) {
			dev_dbg(&hdmi->pdev->dev,
					"HDMI EDID i2c operation time out!!\n");
			return false;
		}
		val = hdmi_readb(HDMI_IH_I2CM_STAT0) & 0x2;
	}
	return true;
}

static u8 hdmi_edid_i2c_read(struct mxc_hdmi *hdmi,
					u8 addr, u8 blockno)
{
	u8 spointer = blockno / 2;
	u8 edidaddress = ((blockno % 2) * 0x80) + addr;
	u8 data;

	hdmi_writeb(0xFF, HDMI_IH_I2CM_STAT0);
	hdmi_writeb(edidaddress, HDMI_I2CM_ADDRESS);
	hdmi_writeb(spointer, HDMI_I2CM_SEGADDR);
	if (spointer == 0)
		hdmi_writeb(HDMI_I2CM_OPERATION_READ,
			HDMI_I2CM_OPERATION);
	else
		hdmi_writeb(HDMI_I2CM_OPERATION_READ_EXT,
			HDMI_I2CM_OPERATION);

	hdmi_edid_wait_i2c_done(hdmi, 30);
	data = hdmi_readb(HDMI_I2CM_DATAI);
	hdmi_writeb(0xFF, HDMI_IH_I2CM_STAT0);
	return data;
}


/* "Power-down enable (active low)"
 * That mean that power up == 1! */
static void mxc_hdmi_phy_enable_power(u8 enable)
{
	hdmi_mask_writeb(enable, HDMI_PHY_CONF0,
			HDMI_PHY_CONF0_PDZ_OFFSET,
			HDMI_PHY_CONF0_PDZ_MASK);
}

static void mxc_hdmi_phy_enable_tmds(u8 enable)
{
	hdmi_mask_writeb(enable, HDMI_PHY_CONF0,
			HDMI_PHY_CONF0_ENTMDS_OFFSET,
			HDMI_PHY_CONF0_ENTMDS_MASK);
}

static void mxc_hdmi_phy_gen2_pddq(u8 enable)
{
	hdmi_mask_writeb(enable, HDMI_PHY_CONF0,
			HDMI_PHY_CONF0_GEN2_PDDQ_OFFSET,
			HDMI_PHY_CONF0_GEN2_PDDQ_MASK);
}

static void mxc_hdmi_phy_gen2_txpwron(u8 enable)
{
	hdmi_mask_writeb(enable, HDMI_PHY_CONF0,
			HDMI_PHY_CONF0_GEN2_TXPWRON_OFFSET,
			HDMI_PHY_CONF0_GEN2_TXPWRON_MASK);
}

#if 0
static void mxc_hdmi_phy_gen2_enhpdrxsense(u8 enable)
{
	hdmi_mask_writeb(enable, HDMI_PHY_CONF0,
			HDMI_PHY_CONF0_GEN2_ENHPDRXSENSE_OFFSET,
			HDMI_PHY_CONF0_GEN2_ENHPDRXSENSE_MASK);
}
#endif

static void mxc_hdmi_phy_sel_data_en_pol(u8 enable)
{
	hdmi_mask_writeb(enable, HDMI_PHY_CONF0,
			HDMI_PHY_CONF0_SELDATAENPOL_OFFSET,
			HDMI_PHY_CONF0_SELDATAENPOL_MASK);
}

static void mxc_hdmi_phy_sel_interface_control(u8 enable)
{
	hdmi_mask_writeb(enable, HDMI_PHY_CONF0,
			HDMI_PHY_CONF0_SELDIPIF_OFFSET,
			HDMI_PHY_CONF0_SELDIPIF_MASK);
}

static int hdmi_phy_configure(struct mxc_hdmi *hdmi, unsigned char pRep,
			      unsigned char cRes, int cscOn)
{
	u8 val;
	u8 msec;

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	/* color resolution 0 is 8 bit colour depth */
	if (cRes == 0)
		cRes = 8;

	if (pRep != 0)
		return false;
	else if (cRes != 8 && cRes != 12)
		return false;

	/* Enable csc path */
	if (cscOn)
		val = HDMI_MC_FLOWCTRL_FEED_THROUGH_OFF_CSC_IN_PATH;
	else
		val = HDMI_MC_FLOWCTRL_FEED_THROUGH_OFF_CSC_BYPASS;

	hdmi_writeb(val, HDMI_MC_FLOWCTRL);

	/* gen2 tx power off */
	mxc_hdmi_phy_gen2_txpwron(0);

	/* gen2 pddq */
	mxc_hdmi_phy_gen2_pddq(1);

	/* PHY reset */
	hdmi_writeb(HDMI_MC_PHYRSTZ_DEASSERT, HDMI_MC_PHYRSTZ);
	hdmi_writeb(HDMI_MC_PHYRSTZ_ASSERT, HDMI_MC_PHYRSTZ);

	hdmi_writeb(HDMI_MC_HEACPHY_RST_ASSERT, HDMI_MC_HEACPHY_RST);

	hdmi_phy_test_clear(hdmi, 1);
	hdmi_writeb(HDMI_PHY_I2CM_SLAVE_ADDR_PHY_GEN2,
			HDMI_PHY_I2CM_SLAVE_ADDR);
	hdmi_phy_test_clear(hdmi, 0);

	if (hdmi->hdmi_data.video_mode.mPixelClock < 0) {
		dev_dbg(&hdmi->pdev->dev, "Pixel clock (%d) must be positive\n",
			hdmi->hdmi_data.video_mode.mPixelClock);
		return false;
	}

	if (hdmi->hdmi_data.video_mode.mPixelClock <= 45250000) {
		switch (cRes) {
		case 8:
			/* PLL/MPLL Cfg */
			hdmi_phy_i2c_write(hdmi, 0x01e0, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x0000, 0x15);  /* GMPCTRL */
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x21e1, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x0000, 0x15);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x41e2, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x0000, 0x15);
			break;
		default:
			return false;
		}
	} else if (hdmi->hdmi_data.video_mode.mPixelClock <= 92500000) {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x0140, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x0005, 0x15);
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x2141, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x0005, 0x15);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x4142, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x0005, 0x15);
		default:
			return false;
		}
	} else if (hdmi->hdmi_data.video_mode.mPixelClock <= 148500000) {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x00a0, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x000a, 0x15);
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x20a1, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x000a, 0x15);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x40a2, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x000a, 0x15);
		default:
			return false;
		}
	} else {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x00a0, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x000a, 0x15);
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x2001, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x000f, 0x15);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x4002, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x000f, 0x15);
		default:
			return false;
		}
	}

	if (hdmi->hdmi_data.video_mode.mPixelClock <= 54000000) {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x091c, 0x10);  /* CURRCTRL */
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x091c, 0x10);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x06dc, 0x10);
			break;
		default:
			return false;
		}
	} else if (hdmi->hdmi_data.video_mode.mPixelClock <= 58400000) {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x091c, 0x10);
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x06dc, 0x10);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x06dc, 0x10);
			break;
		default:
			return false;
		}
	} else if (hdmi->hdmi_data.video_mode.mPixelClock <= 72000000) {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x06dc, 0x10);
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x06dc, 0x10);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x091c, 0x10);
			break;
		default:
			return false;
		}
	} else if (hdmi->hdmi_data.video_mode.mPixelClock <= 74250000) {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x06dc, 0x10);
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x0b5c, 0x10);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x091c, 0x10);
			break;
		default:
			return false;
		}
	} else if (hdmi->hdmi_data.video_mode.mPixelClock <= 118800000) {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x091c, 0x10);
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x091c, 0x10);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x06dc, 0x10);
			break;
		default:
			return false;
		}
	} else if (hdmi->hdmi_data.video_mode.mPixelClock <= 216000000) {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x06dc, 0x10);
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x0b5c, 0x10);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x091c, 0x10);
			break;
		default:
			return false;
		}
	} else {
		dev_err(&hdmi->pdev->dev,
				"Pixel clock %d - unsupported by HDMI\n",
				hdmi->hdmi_data.video_mode.mPixelClock);
		return false;
	}

	hdmi_phy_i2c_write(hdmi, 0x0000, 0x13);  /* PLLPHBYCTRL */
	hdmi_phy_i2c_write(hdmi, 0x0006, 0x17);
	/* RESISTANCE TERM 133Ohm Cfg */
	hdmi_phy_i2c_write(hdmi, 0x0005, 0x19);  /* TXTERM */
	/* PREEMP Cgf 0.00 */
	hdmi_phy_i2c_write(hdmi, 0x800d, 0x09);  /* CKSYMTXCTRL */
	/* TX/CK LVL 10 */
	hdmi_phy_i2c_write(hdmi, 0x01ad, 0x0E);  /* VLEVCTRL */

	/* Board specific setting for PHY register 0x09, 0x0e to pass HCT */
	if (hdmi->phy_config.reg_cksymtx != 0)
		hdmi_phy_i2c_write(hdmi, hdmi->phy_config.reg_cksymtx, 0x09);

	if (hdmi->phy_config.reg_vlev >= 0 && hdmi->phy_config.reg_vlev <= 1023)
		hdmi_phy_i2c_write(hdmi, hdmi->phy_config.reg_vlev, 0x0E);

	/* TXTERM */
	if (hdmi->phy_config.reg_txterm >= 0 &&
			hdmi->phy_config.reg_txterm <= 7)
		hdmi_phy_i2c_write(hdmi, hdmi->phy_config.reg_txterm, 0x19);

	/* REMOVE CLK TERM */
	hdmi_phy_i2c_write(hdmi, 0x8000, 0x05);  /* CKCALCTRL */

	if (hdmi->hdmi_data.video_mode.mPixelClock > 148500000) {
			hdmi_phy_i2c_write(hdmi, 0x800b, 0x09);
			hdmi_phy_i2c_write(hdmi, 0x0129, 0x0E);
	}

	mxc_hdmi_phy_enable_power(1);

	/* toggle TMDS enable */
	mxc_hdmi_phy_enable_tmds(0);
	mxc_hdmi_phy_enable_tmds(1);

	/* gen2 tx power on */
	mxc_hdmi_phy_gen2_txpwron(1);
	mxc_hdmi_phy_gen2_pddq(0);

	/*Wait for PHY PLL lock */
	msec = 4;
	val = hdmi_readb(HDMI_PHY_STAT0) & HDMI_PHY_TX_PHY_LOCK;
	while (val == 0) {
		udelay(1000);
		if (msec-- == 0) {
			dev_dbg(&hdmi->pdev->dev, "PHY PLL not locked\n");
			return false;
		}
		val = hdmi_readb(HDMI_PHY_STAT0) & HDMI_PHY_TX_PHY_LOCK;
	}

	return true;
}

static void mxc_hdmi_phy_init(struct mxc_hdmi *hdmi)
{
	int i;
	bool cscon = false;

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	/* Never do phy init if pixel clock is gated.
	 * Otherwise HDMI PHY will get messed up and generate an overflow
	 * interrupt that can't be cleared or detected by accessing the
	 * status register. */
	if (!hdmi->fb_reg ||
	    hdmi->hp_state == HDMI_HOTPLUG_DISCONNECTED ||
            hdmi->blank != FB_BLANK_UNBLANK)
		return;

	/*check csc whether needed activated in HDMI mode */
	cscon = (isColorSpaceConversion(hdmi) &&
		 (hdmi->hp_state == HDMI_HOTPLUG_CONNECTED_HDMI));

	/* HDMI Phy spec says to do the phy initialization sequence twice */
	for (i = 0 ; i < 2 ; i++) {
		mxc_hdmi_phy_sel_data_en_pol(1);
		mxc_hdmi_phy_sel_interface_control(0);
		mxc_hdmi_phy_enable_tmds(0);
		mxc_hdmi_phy_enable_power(0);

		/* Enable CSC */
		hdmi_phy_configure(hdmi, 0, 8, cscon);
	}

	hdmi->phy_enabled = true;
}

static enum hdmi_3d_structure mxc_3d_structure_infoframe(u32 layout)
{
	switch (layout) {
	case FB_VMODE_3D_SBS_HALF:
		return HDMI_3D_STRUCTURE_SIDE_BY_SIDE_HALF;
	case FB_VMODE_3D_SBS_FULL:
		return HDMI_3D_STRUCTURE_SIDE_BY_SIDE_FULL;
	case FB_VMODE_3D_TOP_BOTTOM:
		return HDMI_3D_STRUCTURE_TOP_AND_BOTTOM;
	case FB_VMODE_3D_FRAME_PACK:
		return HDMI_3D_STRUCTURE_FRAME_PACKING;
	default:
		return HDMI_3D_STRUCTURE_INVALID;
	}
}

static int mxc_hdmi_vendor_infoframe(struct mxc_hdmi *hdmi, struct hdmi_vendor_infoframe *frame)
{
	int err;
	u32 s3d_flags;
	u8 vic;
	uint8_t buffer[32]; //HDMI_FC_VSDPAYLOAD23 - HDMI_FC_VSDIEEEID0
			    //(with a hole starting at 0102B, ending 102F
	ssize_t len;
	int i;

	if (!frame)
		return -EINVAL;

	vic = hdmi->vic;
	s3d_flags = (*hdmi->fbi->mode).vmode & FB_VMODE_3D_MASK;

	if (!vic && !s3d_flags)
		return -EINVAL;

	err = hdmi_vendor_infoframe_init(frame);
	if (err < 0)
		return err;

	if (s3d_flags)
		frame->s3d_struct = mxc_3d_structure_infoframe(s3d_flags);
	else if (vic)
		frame->vic = vic;
	else
		return -EINVAL;

	/* see comment above for the reason for this offset */
	len = hdmi_vendor_infoframe_pack(frame, buffer+1, sizeof(buffer)-1);
	if (len < 0)
		return -EINVAL;

	buffer[0] = buffer[5];
	buffer[1] = buffer[3];
	buffer[2] = 0;
	buffer[3] = 0;
	buffer[4] = 0;
	buffer[5] = 0;

	//buffer[6] = buffer[6];
	//buffer[7] = buffer[7];

	for (i = 0; i < 32; i++)
		hdmi_writeb(buffer[i], HDMI_FC_VSDIEEEID0 + i);
	return 0;
}

static void hdmi_config_AVI(struct mxc_hdmi *hdmi)
{
	u8 val;
	u8 pix_fmt;
	u8 under_scan;
	u8 act_ratio, coded_ratio, colorimetry, ext_colorimetry;
	struct fb_videomode mode;
	const struct fb_videomode *edid_mode;
	bool aspect_16_9;
	struct hdmi_vendor_infoframe vendor_infoframe;

	dev_dbg(&hdmi->pdev->dev, "set up AVI frame\n");
	fb_var_to_videomode(&mode, &hdmi->fbi->var);
	/* Use mode from list extracted from EDID to get aspect ratio */
	if (!list_empty(&hdmi->fbi->modelist)) {
		edid_mode = fb_find_nearest_mode(&mode, &hdmi->fbi->modelist);
		if (edid_mode->vmode & FB_VMODE_ASPECT_16_9)
			aspect_16_9 = true;
		else
			aspect_16_9 = false;
	} else
		aspect_16_9 = false;

	/********************************************
	 * AVI Data Byte 1
	 ********************************************/
	if (hdmi->hdmi_data.enc_out_format == YCBCR444)
		pix_fmt = HDMI_FC_AVICONF0_PIX_FMT_YCBCR444;
	else if (hdmi->hdmi_data.enc_out_format == YCBCR422_8BITS)
		pix_fmt = HDMI_FC_AVICONF0_PIX_FMT_YCBCR422;
	else
		pix_fmt = HDMI_FC_AVICONF0_PIX_FMT_RGB;

	if (hdmi->edid_cfg.cea_underscan)
		under_scan = HDMI_FC_AVICONF0_SCAN_INFO_UNDERSCAN;
	else
		under_scan =  HDMI_FC_AVICONF0_SCAN_INFO_NODATA;

	/*
	 * Active format identification data is present in the AVI InfoFrame.
	 * Under scan info, no bar data
	 */
	val = pix_fmt | under_scan |
		HDMI_FC_AVICONF0_ACTIVE_FMT_INFO_PRESENT |
		HDMI_FC_AVICONF0_BAR_DATA_NO_DATA;

	hdmi_writeb(val, HDMI_FC_AVICONF0);

	/********************************************
	 * AVI Data Byte 2
	 ********************************************/

	/*  Set the Aspect Ratio */
	if (aspect_16_9) {
		act_ratio = HDMI_FC_AVICONF1_ACTIVE_ASPECT_RATIO_16_9;
		coded_ratio = HDMI_FC_AVICONF1_CODED_ASPECT_RATIO_16_9;
	} else {
		act_ratio = HDMI_FC_AVICONF1_ACTIVE_ASPECT_RATIO_4_3;
		coded_ratio = HDMI_FC_AVICONF1_CODED_ASPECT_RATIO_4_3;
	}

	/* Set up colorimetry */
	if (hdmi->hdmi_data.enc_out_format == XVYCC444) {
		colorimetry = HDMI_FC_AVICONF1_COLORIMETRY_EXTENDED_INFO;
		if (hdmi->hdmi_data.colorimetry == eITU601)
			ext_colorimetry =
				HDMI_FC_AVICONF2_EXT_COLORIMETRY_XVYCC601;
		else /* hdmi->hdmi_data.colorimetry == eITU709 */
			ext_colorimetry =
				HDMI_FC_AVICONF2_EXT_COLORIMETRY_XVYCC709;
	} else if (hdmi->hdmi_data.enc_out_format != RGB) {
		if (hdmi->hdmi_data.colorimetry == eITU601)
			colorimetry = HDMI_FC_AVICONF1_COLORIMETRY_SMPTE;
		else /* hdmi->hdmi_data.colorimetry == eITU709 */
			colorimetry = HDMI_FC_AVICONF1_COLORIMETRY_ITUR;
		ext_colorimetry = HDMI_FC_AVICONF2_EXT_COLORIMETRY_XVYCC601;
	} else { /* Carries no data */
		colorimetry = HDMI_FC_AVICONF1_COLORIMETRY_NO_DATA;
		ext_colorimetry = HDMI_FC_AVICONF2_EXT_COLORIMETRY_XVYCC601;
	}

	val = colorimetry | coded_ratio | act_ratio;
	hdmi_writeb(val, HDMI_FC_AVICONF1);

	/********************************************
	 * AVI Data Byte 3
	 ********************************************/

	val = HDMI_FC_AVICONF2_IT_CONTENT_NO_DATA | ext_colorimetry |
		getRGBQuantRange(hdmi) | HDMI_FC_AVICONF2_SCALING_NONE;
	hdmi_writeb(val, HDMI_FC_AVICONF2);

	/********************************************
	 * AVI Data Byte 4
	 ********************************************/
	hdmi_writeb(hdmi->vic, HDMI_FC_AVIVID);

	/********************************************
	 * AVI Data Byte 5
	 ********************************************/

	/* Set up input and output pixel repetition */
	val = (((hdmi->hdmi_data.video_mode.mPixelRepetitionInput + 1) <<
		HDMI_FC_PRCONF_INCOMING_PR_FACTOR_OFFSET) &
		HDMI_FC_PRCONF_INCOMING_PR_FACTOR_MASK) |
		((hdmi->hdmi_data.video_mode.mPixelRepetitionOutput <<
		HDMI_FC_PRCONF_OUTPUT_PR_FACTOR_OFFSET) &
		HDMI_FC_PRCONF_OUTPUT_PR_FACTOR_MASK);
	hdmi_writeb(val, HDMI_FC_PRCONF);

	/* IT Content and quantization range = don't care */
	val = HDMI_FC_AVICONF3_IT_CONTENT_TYPE_GRAPHICS |
		HDMI_FC_AVICONF3_QUANT_RANGE_LIMITED;
	hdmi_writeb(val, HDMI_FC_AVICONF3);

	/********************************************
	 * AVI Data Bytes 6-13
	 ********************************************/
	hdmi_writeb(0, HDMI_FC_AVIETB0);
	hdmi_writeb(0, HDMI_FC_AVIETB1);
	hdmi_writeb(0, HDMI_FC_AVISBB0);
	hdmi_writeb(0, HDMI_FC_AVISBB1);
	hdmi_writeb(0, HDMI_FC_AVIELB0);
	hdmi_writeb(0, HDMI_FC_AVIELB1);
	hdmi_writeb(0, HDMI_FC_AVISRB0);
	hdmi_writeb(0, HDMI_FC_AVISRB1);

	mxc_hdmi_vendor_infoframe(hdmi, &vendor_infoframe);
}

/*!
 * this submodule is responsible for the video/audio data composition.
 */
static void hdmi_av_composer(struct mxc_hdmi *hdmi)
{
	u8 inv_val;
	struct fb_info *fbi = hdmi->fbi;
	struct fb_videomode fb_mode;
	struct hdmi_vmode *vmode = &hdmi->hdmi_data.video_mode;
	int hblank, vblank;

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	fb_var_to_videomode(&fb_mode, &fbi->var);

	vmode->mHSyncPolarity = ((fb_mode.sync & FB_SYNC_HOR_HIGH_ACT) != 0);
	vmode->mVSyncPolarity = ((fb_mode.sync & FB_SYNC_VERT_HIGH_ACT) != 0);
	vmode->mInterlaced = ((fb_mode.vmode & FB_VMODE_INTERLACED) != 0);
	vmode->mPixelClock = (u32) (mxcPICOS2KHZ(fb_mode.pixclock, fb_mode.vmode) * 1000UL);

	dev_dbg(&hdmi->pdev->dev, "final pixclk = %d\n", vmode->mPixelClock);

	/* Set up HDMI_FC_INVIDCONF */
	inv_val = (vmode->mVSyncPolarity ?
		HDMI_FC_INVIDCONF_VSYNC_IN_POLARITY_ACTIVE_HIGH :
		HDMI_FC_INVIDCONF_VSYNC_IN_POLARITY_ACTIVE_LOW);

	inv_val |= (vmode->mHSyncPolarity ?
		HDMI_FC_INVIDCONF_HSYNC_IN_POLARITY_ACTIVE_HIGH :
		HDMI_FC_INVIDCONF_HSYNC_IN_POLARITY_ACTIVE_LOW);

	inv_val |= (vmode->mDataEnablePolarity ?
		HDMI_FC_INVIDCONF_DE_IN_POLARITY_ACTIVE_HIGH :
		HDMI_FC_INVIDCONF_DE_IN_POLARITY_ACTIVE_LOW);

	if (fb_mode.vmode & FB_VMODE_FRACTIONAL)
		inv_val |= HDMI_FC_INVIDCONF_R_V_BLANK_IN_OSC_ACTIVE_HIGH;
	else
		inv_val |= (vmode->mInterlaced ?
			HDMI_FC_INVIDCONF_R_V_BLANK_IN_OSC_ACTIVE_HIGH :
			HDMI_FC_INVIDCONF_R_V_BLANK_IN_OSC_ACTIVE_LOW);

	inv_val |= (vmode->mInterlaced ?
		HDMI_FC_INVIDCONF_IN_I_P_INTERLACED :
		HDMI_FC_INVIDCONF_IN_I_P_PROGRESSIVE);

	inv_val |= ((hdmi->hp_state == HDMI_HOTPLUG_CONNECTED_DVI) ?
		HDMI_FC_INVIDCONF_DVI_MODEZ_DVI_MODE :
		HDMI_FC_INVIDCONF_DVI_MODEZ_HDMI_MODE);

	hdmi_writeb(inv_val, HDMI_FC_INVIDCONF);

	/* Set up horizontal active pixel region width */
	hdmi_writeb(fb_mode.xres >> 8, HDMI_FC_INHACTV1);
	hdmi_writeb(fb_mode.xres, HDMI_FC_INHACTV0);

	/* Set up vertical blanking pixel region width */
	if (vmode->mInterlaced) {
		hdmi_writeb((fb_mode.yres/2) >> 8, HDMI_FC_INVACTV1);
		hdmi_writeb((fb_mode.yres/2), HDMI_FC_INVACTV0);
	} else {
		hdmi_writeb(fb_mode.yres >> 8, HDMI_FC_INVACTV1);
		hdmi_writeb(fb_mode.yres, HDMI_FC_INVACTV0);
	}

	/* Set up horizontal blanking pixel region width */
	hblank = fb_mode.left_margin + fb_mode.right_margin +
		fb_mode.hsync_len;
	hdmi_writeb(hblank >> 8, HDMI_FC_INHBLANK1);
	hdmi_writeb(hblank, HDMI_FC_INHBLANK0);

	/* Set up vertical blanking pixel region width */
	vblank = fb_mode.upper_margin + fb_mode.lower_margin +
		fb_mode.vsync_len;
	if (vmode->mInterlaced)
		hdmi_writeb(vblank/2, HDMI_FC_INVBLANK);
	else
		hdmi_writeb(vblank, HDMI_FC_INVBLANK);

	/* Set up HSYNC active edge delay width (in pixel clks) */
	hdmi_writeb(fb_mode.right_margin >> 8, HDMI_FC_HSYNCINDELAY1);
	hdmi_writeb(fb_mode.right_margin, HDMI_FC_HSYNCINDELAY0);

	/* Set up VSYNC active edge delay (in pixel clks) */
	if (vmode->mInterlaced)
		hdmi_writeb(fb_mode.lower_margin / 2, HDMI_FC_VSYNCINDELAY);
	else
		hdmi_writeb(fb_mode.lower_margin, HDMI_FC_VSYNCINDELAY);

	/* Set up HSYNC active pulse width (in pixel clks) */
	hdmi_writeb(fb_mode.hsync_len >> 8, HDMI_FC_HSYNCINWIDTH1);
	hdmi_writeb(fb_mode.hsync_len, HDMI_FC_HSYNCINWIDTH0);

	/* Set up VSYNC active edge delay (in pixel clks) */
	if (vmode->mInterlaced)
		hdmi_writeb(fb_mode.vsync_len / 2, HDMI_FC_VSYNCINWIDTH);
	else
		hdmi_writeb(fb_mode.vsync_len, HDMI_FC_VSYNCINWIDTH);

	dev_dbg(&hdmi->pdev->dev, "%s exit\n", __func__);
}

static int mxc_edid_read_internal(struct mxc_hdmi *hdmi, unsigned char *edid,
			struct mxc_edid_cfg *cfg, struct fb_info *fbi)
{
	int extblknum;
	int i, ret;
	unsigned char *ediddata = edid;
	unsigned char tmpedid[EDID_LENGTH];

	dev_info(&hdmi->pdev->dev, "%s\n", __func__);

	if (!edid || !cfg || !fbi)
		return -EINVAL;

	/* init HDMI I2CM for read edid*/
	hdmi_writeb(0x0, HDMI_I2CM_DIV);
	hdmi_writeb(0x00, HDMI_I2CM_SS_SCL_HCNT_1_ADDR);
	hdmi_writeb(0x79, HDMI_I2CM_SS_SCL_HCNT_0_ADDR);
	hdmi_writeb(0x00, HDMI_I2CM_SS_SCL_LCNT_1_ADDR);
	hdmi_writeb(0x91, HDMI_I2CM_SS_SCL_LCNT_0_ADDR);

	hdmi_writeb(0x00, HDMI_I2CM_FS_SCL_HCNT_1_ADDR);
	hdmi_writeb(0x0F, HDMI_I2CM_FS_SCL_HCNT_0_ADDR);
	hdmi_writeb(0x00, HDMI_I2CM_FS_SCL_LCNT_1_ADDR);
	hdmi_writeb(0x21, HDMI_I2CM_FS_SCL_LCNT_0_ADDR);

	hdmi_writeb(0x50, HDMI_I2CM_SLAVE);
	hdmi_writeb(0x30, HDMI_I2CM_SEGADDR);

	/* Umask edid interrupt */
	hdmi_writeb(HDMI_I2CM_INT_DONE_POL,
		    HDMI_I2CM_INT);

	hdmi_writeb(HDMI_I2CM_CTLINT_NAC_POL |
		    HDMI_I2CM_CTLINT_ARBITRATION_POL,
		    HDMI_I2CM_CTLINT);

	/* reset edid data zero */
	memset(edid, 0, EDID_LENGTH*4);
	memset(cfg, 0, sizeof(struct mxc_edid_cfg));

	/* Check first three byte of EDID head */
	if (!(hdmi_edid_i2c_read(hdmi, 0, 0) == 0x00) ||
		!(hdmi_edid_i2c_read(hdmi, 1, 0) == 0xFF) ||
		!(hdmi_edid_i2c_read(hdmi, 2, 0) == 0xFF)) {
		dev_info(&hdmi->pdev->dev, "EDID head check failed!");
		return -ENOENT;
	}

	for (i = 0; i < 128; i++) {
		*ediddata = hdmi_edid_i2c_read(hdmi, i, 0);
		ediddata++;
	}

	extblknum = edid[0x7E];

	if (extblknum < 0) {
		return extblknum;
	} else {
		ediddata = edid + EDID_LENGTH;
		for (i = 0; i < 128; i++) {
			*ediddata = hdmi_edid_i2c_read(hdmi, i, 1);
			ediddata++;
		}
	}

	/* edid first block parsing */
	memset(&fbi->monspecs, 0, sizeof(fbi->monspecs));
	fb_edid_to_monspecs(edid, &fbi->monspecs);

	ret = mxc_edid_parse_ext_blk(edid + EDID_LENGTH,
			cfg, &fbi->monspecs);
	if (ret < 0) {
                fb_edid_add_monspecs(edid + EDID_LENGTH, &fbi->monspecs);
                if (fbi->monspecs.modedb_len > 0)
                        hdmi->edid_cfg.hdmi_cap = false;
                else
			return -ENOENT;
	}

	/* need read segment block? */
	if (extblknum > 1) {
		int j;
		for (j = 1; j <= extblknum; j++) {
			for (i = 0; i < 128; i++)
				*(tmpedid + 1) = hdmi_edid_i2c_read(hdmi, i, j);

			/* edid ext block parsing */
			ret = mxc_edid_parse_ext_blk(tmpedid + EDID_LENGTH,
					cfg, &fbi->monspecs);
			if (ret < 0)
				return -ENOENT;
		}
	}

	return 0;
}

static int mxc_hdmi_read_edid(struct mxc_hdmi *hdmi)
{
	int ret;
	u8 edid_old[HDMI_EDID_LEN];
	u8 clkdis;

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	/* save old edid */
	memcpy(edid_old, hdmi->edid, HDMI_EDID_LEN);

	/* Read EDID via HDMI DDC when HDCP Enable */
	if (!hdcp_init) {
		ret = mxc_edid_read(hdmi_i2c->adapter, hdmi_i2c->addr,
				hdmi->edid, &hdmi->edid_cfg, hdmi->fbi);
		if (ret < 0 && hdmi->fbi->monspecs.modedb_len > 0) {
			hdmi->edid_cfg.hdmi_cap = false;
			ret = 0;
		}
	} else {

		/* Disable HDCP clk */
		if (hdmi->hdmi_data.hdcp_enable) {
			clkdis = hdmi_readb(HDMI_MC_CLKDIS);
			clkdis |= HDMI_MC_CLKDIS_HDCPCLK_DISABLE;
			hdmi_writeb(clkdis, HDMI_MC_CLKDIS);
		}

		ret = mxc_edid_read_internal(hdmi, hdmi->edid,
				&hdmi->edid_cfg, hdmi->fbi);

		/* Enable HDCP clk */
		if (hdmi->hdmi_data.hdcp_enable) {
			clkdis = hdmi_readb(HDMI_MC_CLKDIS);
			clkdis &= ~HDMI_MC_CLKDIS_HDCPCLK_DISABLE;
			hdmi_writeb(clkdis, HDMI_MC_CLKDIS);
		}

	}

	if (hdmi->edid_cfg.ext_edid) {
		release_firmware(hdmi->edid_cfg.ext_edid);
		hdmi->edid_cfg.ext_edid = NULL;
	}

	if (ret < 0)
		return HDMI_EDID_FAIL;

	if (!memcmp(edid_old, hdmi->edid, HDMI_EDID_LEN)) {
		dev_info(&hdmi->pdev->dev, "same edid\n");
		return HDMI_EDID_SAME;
	}

	if (hdmi->fbi->monspecs.modedb_len == 0) {
		dev_info(&hdmi->pdev->dev, "No modes read from edid\n");
		return HDMI_EDID_NO_MODES;
	}

	return HDMI_EDID_SUCCESS;
}

static void mxc_hdmi_phy_disable(struct mxc_hdmi *hdmi)
{
	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	if (!hdmi->phy_enabled)
		return;

	hdmi_disable_overflow_interrupts();

	/* Setting PHY to reset status */
	hdmi_writeb(HDMI_MC_PHYRSTZ_DEASSERT, HDMI_MC_PHYRSTZ);

	/* Power down PHY */
	mxc_hdmi_phy_enable_tmds(0);
	mxc_hdmi_phy_enable_power(0);
	mxc_hdmi_phy_gen2_txpwron(0);
	mxc_hdmi_phy_gen2_pddq(1);

	hdmi->phy_enabled = false;
	dev_dbg(&hdmi->pdev->dev, "%s - exit\n", __func__);
}

/* HDMI Initialization Step B.4 */
static void mxc_hdmi_enable_video_path(struct mxc_hdmi *hdmi)
{
	u8 clkdis;

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	/* control period minimum duration */
	hdmi_writeb(12, HDMI_FC_CTRLDUR);
	hdmi_writeb(32, HDMI_FC_EXCTRLDUR);
	hdmi_writeb(1, HDMI_FC_EXCTRLSPAC);

	/* Set to fill TMDS data channels */
	hdmi_writeb(0x0B, HDMI_FC_CH0PREAM);
	hdmi_writeb(0x16, HDMI_FC_CH1PREAM);
	hdmi_writeb(0x21, HDMI_FC_CH2PREAM);

	/* Save CEC clock */
	clkdis = hdmi_readb(HDMI_MC_CLKDIS) & HDMI_MC_CLKDIS_CECCLK_DISABLE;
	clkdis |= ~HDMI_MC_CLKDIS_CECCLK_DISABLE;

	/* Enable pixel clock and tmds data path */
	clkdis = 0x7F & clkdis;
	clkdis &= ~HDMI_MC_CLKDIS_PIXELCLK_DISABLE;
	hdmi_writeb(clkdis, HDMI_MC_CLKDIS);

	clkdis &= ~HDMI_MC_CLKDIS_TMDSCLK_DISABLE;
	hdmi_writeb(clkdis, HDMI_MC_CLKDIS);

	/* Enable csc path */
	if (isColorSpaceConversion(hdmi) &&
	    (hdmi->hp_state == HDMI_HOTPLUG_CONNECTED_HDMI)) {
		clkdis &= ~HDMI_MC_CLKDIS_CSCCLK_DISABLE;
		hdmi_writeb(clkdis, HDMI_MC_CLKDIS);
	}
}

static void hdmi_enable_audio_clk(struct mxc_hdmi *hdmi)
{
	u8 clkdis;

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	clkdis = hdmi_readb(HDMI_MC_CLKDIS);
	clkdis &= ~HDMI_MC_CLKDIS_AUDCLK_DISABLE;
	hdmi_writeb(clkdis, HDMI_MC_CLKDIS);
}

/* Workaround to clear the overflow condition */
static void mxc_hdmi_clear_overflow(struct mxc_hdmi *hdmi)
{
	int count;
	u8 val;

	/* TMDS software reset */
	hdmi_writeb((u8)~HDMI_MC_SWRSTZ_TMDSSWRST_REQ, HDMI_MC_SWRSTZ);

	val = hdmi_readb(HDMI_FC_INVIDCONF);

	if (cpu_is_imx6dl(hdmi)) {
		 hdmi_writeb(val, HDMI_FC_INVIDCONF);
		 return;
	}

	for (count = 0 ; count < 5 ; count++)
		hdmi_writeb(val, HDMI_FC_INVIDCONF);
}

static void hdmi_enable_overflow_interrupts(void)
{
	pr_debug("%s\n", __func__);
	hdmi_writeb(0, HDMI_FC_MASK2);
	hdmi_writeb(0, HDMI_IH_MUTE_FC_STAT2);
}

static void hdmi_disable_overflow_interrupts(void)
{
	pr_debug("%s\n", __func__);
	hdmi_writeb(HDMI_IH_MUTE_FC_STAT2_OVERFLOW_MASK,
		    HDMI_IH_MUTE_FC_STAT2);
	hdmi_writeb(0x7f, HDMI_FC_MASK2);
}

static void mxc_hdmi_notify_fb(struct mxc_hdmi *hdmi)
{
	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	/* Don't notify if we aren't registered yet */
	WARN_ON(!hdmi->fb_reg);

	/* disable the phy before ipu changes mode */
	mxc_hdmi_phy_disable(hdmi);

	/*
	 * Note that fb_set_var will block.  During this time,
	 * FB_EVENT_MODE_CHANGE callback will happen.
	 * So by the end of this function, mxc_hdmi_setup()
	 * will be done.
	 */
	hdmi->fbi->var.activate |= FB_ACTIVATE_FORCE;
	console_lock();
	hdmi->fbi->flags |= FBINFO_MISC_USEREVENT;
	fb_set_var(hdmi->fbi, &hdmi->fbi->var);
	hdmi->fbi->flags &= ~FBINFO_MISC_USEREVENT;
	console_unlock();

	dev_dbg(&hdmi->pdev->dev, "%s exit\n", __func__);
}

static void mxc_hdmi_log_modelist(struct mxc_hdmi *hdmi, struct fb_videomode *mode)
{
	char refresh[10];

	get_refresh_str(mode, refresh);
	dev_info(&hdmi->pdev->dev,
		"vic: %d, xres = %d, yres = %d, ratio = %s, freq = %s, vmode = %d, flag = %d, pclk = %d\n",
		mxc_edid_mode_to_vic(mode, 0),
		mode->xres,
		mode->yres,
		mode->vmode & FB_VMODE_ASPECT_1 ? "1" :
		    mode->vmode & FB_VMODE_ASPECT_4_3 ? "4/3" :
		    mode->vmode & FB_VMODE_ASPECT_5_4 ? "5/4" :
		    mode->vmode & FB_VMODE_ASPECT_16_10 ? "16/10" :
		    mode->vmode & FB_VMODE_ASPECT_16_9 ? "16/9" : "n/a",
		refresh,
		mode->vmode,
		mode->flag,
	mode->pixclock);
}

static void mxc_fb_add_videomode(struct mxc_hdmi *hdmi, const struct fb_videomode *src_mode, struct list_head *modelist, const u32 new_flag, const u32 mod_vmode)
{
	struct fb_videomode mode;

	memcpy(&mode, src_mode, sizeof(struct fb_videomode));
	mode.flag = new_flag; mode.vmode |= mod_vmode;
	fb_add_videomode(&mode, modelist);
	mxc_hdmi_log_modelist(hdmi, &mode);
}

struct stereo_mandatory_mode {
	int				vic;
	uint32_t			vmode;
};

static struct stereo_mandatory_mode stereo_mandatory_modes[] = {
	// 1280x720p @ 59.94 / 60Hz TOP-and-BOTTOM
	{  4, FB_VMODE_3D_TOP_BOTTOM },
	// 1920x1080p @ 23.98 / 24Hz TOP-and-BOTTOM
	{ 32, FB_VMODE_3D_TOP_BOTTOM },
	// 1280x720p @ 59.94 / 60Hz FRAME-PACK
	{  4, FB_VMODE_3D_FRAME_PACK },
	// 1920x1080p @ 23.98 / 24Hz FRAME-PACK
	{ 32, FB_VMODE_3D_FRAME_PACK },
	// 1920x1080i @ 59.94 / 60Hz SIDE-by-SIDE half
	{  5, FB_VMODE_3D_SBS_HALF   },
	// 1280x720p @ 50Hz TOP-and-BOTTOM
	{ 19, FB_VMODE_3D_TOP_BOTTOM },
	// 1280x720p @ 50Hz FRAME-PACK
	{ 19, FB_VMODE_3D_FRAME_PACK },
	// 1920x1080i @ 50Hz SIDE-by-SIDE half
	{ 20, FB_VMODE_3D_SBS_HALF   }
};

static int mxc_fb_check_existing(struct fb_videomode *match, int vic, struct list_head *head)
{
	struct list_head *pos;
	struct fb_modelist *modelist;
	struct fb_videomode *mode;

	list_for_each(pos, head) {
		modelist = list_entry(pos, struct fb_modelist, list);
		mode = &modelist->mode;
		if (match->xres == mode->xres
		&&  match->yres == mode->yres
		&&  match->refresh == mode->refresh
		&& (match->vmode & (FB_VMODE_MASK ^ FB_VMODE_ASPECT_MASK)) == (mode->vmode & (FB_VMODE_MASK ^ FB_VMODE_ASPECT_MASK)))
			return 1;
	}
	return 0;
}

static void mxc_hdmi_edid_rebuild_modelist(struct mxc_hdmi *hdmi)
{
	int i, j, nvic = 0, vic, k = 0;
	struct fb_videomode *mode;
	uint32_t fmasks[5] = { FB_MODE_IS_FIRST, ~(FB_MODE_IS_DETAILED | FB_MODE_IS_FIRST), FB_MODE_IS_DETAILED, ~0, 0 };

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	console_lock();

	fb_destroy_modelist(&hdmi->fbi->modelist);
	fb_add_videomode(&vga_mode, &hdmi->fbi->modelist);

	while (fmasks[k]) {
	    for (i = 0; i < hdmi->fbi->monspecs.modedb_len; i++) {
		/*
		 * We might check here if mode is supported by HDMI.
		 * We do not currently support interlaced modes.
		 * And add CEA modes in the modelist.
		 */
		mode = &hdmi->fbi->monspecs.modedb[i];

		if (hdmi->edid_cfg.hdmi_cap && fmasks[k] != ~0 && !(mode->flag & fmasks[k]))
				continue;

		if ((vic = mxc_edid_mode_to_vic(mode, 0)))
			nvic++;

		// allow detailed timing specification with vic=0 for HDMI
		// mode
		if (hdmi->edid_cfg.hdmi_cap &&
		   ((!(mode->flag & FB_MODE_IS_DETAILED || !mode->flag) && vic == 0)
				||
		   (mode->flag & FB_MODE_IS_VESA)))
				continue;

		if (!mode->xres || !mode->refresh)
			continue;

		if (!(mode->vmode & FB_VMODE_ASPECT_MASK)) {
			if (mode->yres == (mode->xres * 3)/4)
				mode->vmode |= FB_VMODE_ASPECT_4_3;
			else
				mode->vmode |= FB_VMODE_ASPECT_16_9;
		}

		mode->xres = ALIGN2(mode->xres, 8);
		mode->yres = ALIGN2(mode->yres, 8);

		if (mxc_fb_check_existing(mode, vic, &hdmi->fbi->modelist) || fb_add_videomode(mode, &hdmi->fbi->modelist))
			continue;

		mxc_hdmi_log_modelist(hdmi, mode);

		if (vic && hdmi->hdmi_data.enable_fract && (mode->refresh == 24 || mode->refresh == 30 || mode->refresh == 60))
			mxc_fb_add_videomode(hdmi, mode, &hdmi->fbi->modelist, mode->flag, FB_VMODE_FRACTIONAL);

		if (!hdmi->hdmi_data.enable_3d || !hdmi->edid_cfg.hdmi_3d_present)
			continue;

		/* according to HDMI 1.4 specs, add mandatory modes for 50 and 60Hz existing 2d modes */
		for (j = 0; j < sizeof(stereo_mandatory_modes) / sizeof(struct stereo_mandatory_mode); j++) {
			if (stereo_mandatory_modes[j].vic != vic)
				continue;
			mxc_fb_add_videomode(hdmi, &mxc_cea_mode[vic], &hdmi->fbi->modelist,
					FB_MODE_IS_3D, stereo_mandatory_modes[j].vmode);
		}

		if ((hdmi->edid_cfg.hdmi_3d_multi_present == 2 && hdmi->edid_cfg.hdmi_3d_mask_all & (1 << (nvic-1))) ||
		    (hdmi->edid_cfg.hdmi_3d_multi_present == 1 && nvic <= 16)) {
			if (hdmi->edid_cfg.hdmi_3d_struct_all & 0x1)
				mxc_fb_add_videomode(hdmi, mode, &hdmi->fbi->modelist, FB_MODE_IS_3D, FB_VMODE_3D_FRAME_PACK);
			if (hdmi->edid_cfg.hdmi_3d_struct_all & 0x6)
				mxc_fb_add_videomode(hdmi, mode, &hdmi->fbi->modelist, FB_MODE_IS_3D, FB_VMODE_3D_SBS_FULL);
			if (hdmi->edid_cfg.hdmi_3d_struct_all & 0x40)
				mxc_fb_add_videomode(hdmi, mode, &hdmi->fbi->modelist, FB_MODE_IS_3D, FB_VMODE_3D_TOP_BOTTOM);
			if (hdmi->edid_cfg.hdmi_3d_struct_all & 0x100)
				mxc_fb_add_videomode(hdmi, mode, &hdmi->fbi->modelist, FB_MODE_IS_3D, FB_VMODE_3D_SBS_HALF);
		}

		for (j = 0; j < hdmi->edid_cfg.hdmi_3d_len; j++) {
			if (hdmi->edid_cfg.hdmi_3d_format[j].vic_order_2d != nvic-1)
				continue;

			if (hdmi->edid_cfg.hdmi_3d_format[j].struct_3d == 0)
				mxc_fb_add_videomode(hdmi, mode, &hdmi->fbi->modelist, FB_MODE_IS_3D, FB_VMODE_3D_FRAME_PACK);
			if (hdmi->edid_cfg.hdmi_3d_format[j].struct_3d == 3)
				mxc_fb_add_videomode(hdmi, mode, &hdmi->fbi->modelist, FB_MODE_IS_3D, FB_VMODE_3D_SBS_FULL);
			if (hdmi->edid_cfg.hdmi_3d_format[j].struct_3d == 6)
				mxc_fb_add_videomode(hdmi, mode, &hdmi->fbi->modelist, FB_MODE_IS_3D, FB_VMODE_3D_TOP_BOTTOM);
			if (hdmi->edid_cfg.hdmi_3d_format[j].struct_3d == 8)
				mxc_fb_add_videomode(hdmi, mode, &hdmi->fbi->modelist, FB_MODE_IS_3D, FB_VMODE_3D_SBS_HALF);
		}
	    }
	    k++;
	}

	fb_new_modelist(hdmi->fbi);

	console_unlock();
}

static void  mxc_hdmi_default_edid_cfg(struct mxc_hdmi *hdmi)
{
	/* Default setting HDMI working in HDMI mode */
	hdmi->edid_cfg.hdmi_cap = true;
}

static void  mxc_hdmi_default_modelist(struct mxc_hdmi *hdmi)
{
	u32 i;
	struct fb_videomode mode;

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	/* If not EDID data read, set up default modelist  */
	dev_info(&hdmi->pdev->dev, "No modes read from edid\n");
	dev_info(&hdmi->pdev->dev, "create default modelist\n");

	console_lock();

	fb_destroy_modelist(&hdmi->fbi->modelist);

	fb_var_to_videomode(&mode, &hdmi->fbi->var);
	fb_add_videomode(&mode, &hdmi->fbi->modelist);

	/*Add all no interlaced CEA mode to default modelist */
	for (i = 0; i < ARRAY_SIZE(mxc_cea_mode); i++) {
		mode = mxc_cea_mode[i];
		if (mode.xres != 0) {
			mode.flag |= FB_MODE_IS_STANDARD;
			fb_add_videomode(&mode, &hdmi->fbi->modelist);
		}
	}

	fb_new_modelist(hdmi->fbi);

	console_unlock();
}

static void mxc_hdmi_set_mode(struct mxc_hdmi *hdmi, int edid_status)
{
	const struct fb_videomode *mode;
	struct fb_videomode m;
	struct fb_var_screeninfo var;
	unsigned long flags;
	u32 l;

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	/* Set the default mode only once. */
	if (!hdmi->dft_mode_set) {
		fb_videomode_to_var(&var, &hdmi->default_mode);
		hdmi->dft_mode_set = true;
		hdmi->requesting_vga_for_initialization = false;
	} else
		memcpy(&var, &hdmi->previous_non_vga_mode,
		       sizeof(struct fb_var_screeninfo));

	fb_var_to_videomode(&m, &var);

	mode = mxc_fb_find_nearest_mode(&m, &hdmi->fbi->modelist, false);
	if (!mode) {
		pr_err("%s: could not find mode in modelist\n", __func__);
		return;
	}

	console_lock();
	fb_blank(hdmi->fbi, FB_BLANK_UNBLANK);
	console_unlock();

	if (mxc_edid_fb_mode_is_equal(true, &m, mode, ~0) && edid_status == HDMI_EDID_SAME) {
		dev_dbg(&hdmi->pdev->dev,
				"%s: Video mode and EDID same as previous\n", __func__);
		/* update fbi mode in case modelist is updated */
		hdmi->fbi->mode = (struct fb_videomode *)mode;
		memcpy(&hdmi->fbi->var, &hdmi->previous_non_vga_mode,
			sizeof(struct fb_var_screeninfo));
		mxc_hdmi_setup(hdmi, 0);
	} else if (mxc_edid_fb_mode_is_equal(true, &m, mode, ~0) && edid_status != HDMI_EDID_SAME) {
		dev_dbg(&hdmi->pdev->dev,
				"%s: Video mode same as previous, EDID changed\n", __func__);
		/* update fbi mode in case modelist is updated */
		hdmi->fbi->mode = (struct fb_videomode *)mode;
		dump_fb_videomode(hdmi->fbi->mode);
		memcpy(&hdmi->fbi->var, &hdmi->previous_non_vga_mode,
		       sizeof(struct fb_var_screeninfo));
		mxc_hdmi_notify_fb(hdmi);
	} else if (edid_status != HDMI_EDID_SAME) {
		dev_dbg(&hdmi->pdev->dev, "%s: New video mode\n", __func__);
		fb_videomode_to_var(&hdmi->fbi->var, mode);
		dump_fb_videomode((struct fb_videomode *)mode);
		mxc_hdmi_notify_fb(hdmi);
	}

	spin_lock_irqsave(&hdmi->irq_lock, flags);
#ifdef CONFIG_MXC_HDMI_CEC
	memcpy(&l, &hdmi->edid_cfg.physical_address, 4 *sizeof(u8));
	mxc_hdmi_cec_handle(l);
#endif
	hdmi_set_cable_state(1);
	spin_unlock_irqrestore(&hdmi->irq_lock, flags);

}

static void mxc_hdmi_cable_connected_worker(struct work_struct *work)
{
	struct mxc_hdmi *hdmi = container_of(work, struct mxc_hdmi, hdmi_connected);
	int edid_status;

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	mutex_lock(&hdmi->m_lock);
	/* HDMI Initialization Step C */
	if (ignore_edid)
		edid_status = HDMI_EDID_FAIL;
	else
		edid_status = mxc_hdmi_read_edid(hdmi);

	/* Read EDID again if first EDID read failed */
	if (!ignore_edid && (edid_status == HDMI_EDID_NO_MODES ||
			edid_status == HDMI_EDID_FAIL)) {
		int retry_status;
		dev_info(&hdmi->pdev->dev, "Read EDID again\n");
		msleep(200);
		retry_status = mxc_hdmi_read_edid(hdmi);
		/* If we get NO_MODES on the 1st and SAME on the 2nd attempt we
		 * want NO_MODES as final result. */
		if (retry_status != HDMI_EDID_SAME)
			edid_status = retry_status;
	}

	/* HDMI Initialization Steps D, E, F */
	switch (edid_status) {
	case HDMI_EDID_SUCCESS:
		mxc_hdmi_edid_rebuild_modelist(hdmi);
		break;

	/* Nothing to do if EDID same */
	case HDMI_EDID_SAME:
		break;

	case HDMI_EDID_FAIL:
		mxc_hdmi_default_edid_cfg(hdmi);
		/* No break here  */
	case HDMI_EDID_NO_MODES:
	default:
		mxc_hdmi_default_modelist(hdmi);
		break;
	}

	dev_info(&hdmi->pdev->dev, "%s reports %s mode\n", __func__, hdmi->edid_cfg.hdmi_cap ? "HDMI" : "DVI");
	hdmi->hp_state   = hdmi->edid_cfg.hdmi_cap ? HDMI_HOTPLUG_CONNECTED_HDMI : HDMI_HOTPLUG_CONNECTED_DVI;
	hdmi->plug_event = hdmi->edid_cfg.hdmi_cap ? HDMI_IH_PHY_STAT0_HPD       : HDMI_DVI_IH_STAT;
	hdmi->plug_mask  = hdmi->edid_cfg.hdmi_cap ? HDMI_PHY_HPD                : HDMI_DVI_STAT;

	/* Save edid cfg for audio driver */
	hdmi_set_edid_cfg(edid_status, &hdmi->edid_cfg);

	/* Setting video mode */
	mxc_hdmi_set_mode(hdmi, edid_status);

	mutex_unlock(&hdmi->m_lock);
	dev_dbg(&hdmi->pdev->dev, "%s exit\n", __func__);
}

static void mxc_hdmi_edid_from_file(const struct firmware *fw, void *data)
{
	struct mxc_hdmi *hdmi = data;

	mutex_lock(&hdmi->m_lock);
	hdmi->edid_cfg.ext_edid = fw;
	mutex_unlock(&hdmi->m_lock);

	schedule_work(&hdmi->hdmi_connected);
}

static void mxc_hdmi_cable_connected(struct mxc_hdmi *hdmi)
{
	bool has_ext_edid = !!strcmp(hdmi->hdmi_data.edid_filename, "");

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	hdmi->hp_state = HDMI_HOTPLUG_CONNECTED_NO_EDID;

	if (has_ext_edid)
		request_firmware_nowait(THIS_MODULE, true, hdmi->hdmi_data.edid_filename,
					&hdmi->pdev->dev, GFP_KERNEL, hdmi, mxc_hdmi_edid_from_file);

	if (!has_ext_edid || !hdmi->dft_mode_set)
		schedule_work(&hdmi->hdmi_connected);

	dev_dbg(&hdmi->pdev->dev, "%s exit\n", __func__);
}

static int mxc_hdmi_power_on(struct mxc_dispdrv_handle *disp,
			     struct fb_info *fbi)
{
	struct mxc_hdmi *hdmi = mxc_dispdrv_getdata(disp);

	mxc_hdmi_phy_init(hdmi);
	hdmi_clk_regenerator_update_pixel_clock(hdmi->fbi->var.pixclock, hdmi->fbi->var.vmode);
	return 0;
}

static void mxc_hdmi_power_off(struct mxc_dispdrv_handle *disp,
			       struct fb_info *fbi)
{
	struct mxc_hdmi *hdmi = mxc_dispdrv_getdata(disp);

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	mxc_hdmi_phy_disable(hdmi);
}

static void mxc_hdmi_cable_disconnected(struct mxc_hdmi *hdmi)
{
	u8  clkdis;

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	/* Save CEC clock */
	clkdis = hdmi_readb(HDMI_MC_CLKDIS) & HDMI_MC_CLKDIS_CECCLK_DISABLE;
	clkdis |= ~HDMI_MC_CLKDIS_CECCLK_DISABLE;

	/* Disable All HDMI clock */
	hdmi_writeb(0x7f, HDMI_MC_CLKDIS);

	mxc_hdmi_phy_disable(hdmi);

	hdmi_disable_overflow_interrupts();
	hdmi_writeb(clkdis, HDMI_MC_CLKDIS);

	console_lock();
	fb_blank(hdmi->fbi, FB_BLANK_POWERDOWN);
	console_unlock();

	hdmi->hp_state = HDMI_HOTPLUG_DISCONNECTED;
}

static void hotplug_worker(struct work_struct *work)
{
	struct mxc_hdmi *hdmi =
		container_of(work, struct mxc_hdmi, hotplug_work);
	u32 hdmi_phy_stat0, hdmi_phy_pol0, hdmi_phy_mask0;
	unsigned long flags;
	char event_string[32];
	char *envp[] = { event_string, NULL };
//	u32 l;

	hdmi_phy_stat0 = hdmi_readb(HDMI_PHY_STAT0);
	hdmi_phy_pol0 = hdmi_readb(HDMI_PHY_POL0);

	dev_dbg(&hdmi->pdev->dev, "phy_int_stat=0x%x/0x%x, phy_int_pol=0x%x, plug_event=0x%x, plug_mask=0x%x\n",
			hdmi_phy_stat0, hdmi->latest_intr_stat, hdmi_phy_pol0, hdmi->plug_event, hdmi->plug_mask);

		/* Make HPD intr active low to capture unplug event or
		 * active high to capture plugin event */
		hdmi_writeb((hdmi->plug_mask & ~hdmi_phy_pol0), HDMI_PHY_POL0);

		/* cable connection changes */
		if (hdmi_phy_pol0 & hdmi->plug_mask) {
			/* Plugin event */
			dev_dbg(&hdmi->pdev->dev, "EVENT=plugin\n");

			mxc_hdmi_cable_connected(hdmi);

			sprintf(event_string, "EVENT=plugin");
			kobject_uevent_env(&hdmi->pdev->dev.kobj, KOBJ_CHANGE, envp);
#ifdef CONFIG_MXC_HDMI_CEC
			mxc_hdmi_cec_handle(0x80);
#endif
		} else {
			/* Plugout event */
			dev_dbg(&hdmi->pdev->dev, "EVENT=plugout\n");
			mxc_hdmi_abort_stream();
			hdmi_set_cable_state(0);
			mxc_hdmi_cable_disconnected(hdmi);

			sprintf(event_string, "EVENT=plugout");
			kobject_uevent_env(&hdmi->pdev->dev.kobj, KOBJ_CHANGE, envp);
#ifdef CONFIG_MXC_HDMI_CEC
			mxc_hdmi_cec_handle(0x100);
#endif
		}

	/* Lock here to ensure full powerdown sequence
	 * completed before next interrupt processed */
	spin_lock_irqsave(&hdmi->irq_lock, flags);

	/* Re-enable HPD interrupts */
	hdmi_phy_mask0 = hdmi_readb(HDMI_PHY_MASK0);
	hdmi_phy_mask0 &= ~hdmi->plug_mask;
	hdmi_writeb(hdmi_phy_mask0, HDMI_PHY_MASK0);

	/* Unmute interrupts */
	hdmi_writeb(~hdmi->plug_event, HDMI_IH_MUTE_PHY_STAT0);

	if (hdmi_readb(HDMI_IH_FC_STAT2) & HDMI_IH_FC_STAT2_OVERFLOW_MASK)
		mxc_hdmi_clear_overflow(hdmi);

	spin_unlock_irqrestore(&hdmi->irq_lock, flags);
	pr_debug("%s exit\n", __func__);
}

static void hotplug_work_launch(unsigned long data)
{
	struct mxc_hdmi *hdmi = (struct mxc_hdmi *)data;
	pr_debug("%s\n", __func__);
	schedule_work(&hdmi->hotplug_work);
}

static void hdcp_hdp_worker(struct work_struct *work)
{
	struct delayed_work *delay_work = to_delayed_work(work);
	struct mxc_hdmi *hdmi =
		container_of(delay_work, struct mxc_hdmi, hdcp_hdp_work);
	char event_string[32];
	char *envp[] = { event_string, NULL };

	/* HDCP interrupt */
	sprintf(event_string, "EVENT=hdcpint");
	kobject_uevent_env(&hdmi->pdev->dev.kobj, KOBJ_CHANGE, envp);

	/* Unmute interrupts in HDCP application*/
}

static irqreturn_t mxc_hdmi_hotplug(int irq, void *data)
{
	struct mxc_hdmi *hdmi = data;
	u8 val, intr_stat;
	unsigned long flags;

	spin_lock_irqsave(&hdmi->irq_lock, flags);

	/* Check and clean packet overflow interrupt.*/
	if (hdmi_readb(HDMI_IH_FC_STAT2) &
			HDMI_IH_FC_STAT2_OVERFLOW_MASK) {
		mxc_hdmi_clear_overflow(hdmi);

		dev_dbg(&hdmi->pdev->dev, "Overflow interrupt received\n");
		/* clear irq status */
		hdmi_writeb(HDMI_IH_FC_STAT2_OVERFLOW_MASK,
			    HDMI_IH_FC_STAT2);
	}

	/*
	 * We could not disable the irq.  Probably the audio driver
	 * has enabled it. Masking off the HDMI interrupts using
	 * HDMI registers.
	 */
	/* Capture status - used in hotplug_worker ISR */
	intr_stat = hdmi_readb(HDMI_IH_PHY_STAT0);
	if (intr_stat & hdmi->plug_event) {

		dev_dbg(&hdmi->pdev->dev, "Hotplug interrupt received\n");
		hdmi->latest_intr_stat = intr_stat;

		/* Mute interrupts until handled */

		val = hdmi_readb(HDMI_IH_MUTE_PHY_STAT0);
		val |= hdmi->plug_event;
		hdmi_writeb(val, HDMI_IH_MUTE_PHY_STAT0);

		val = hdmi_readb(HDMI_PHY_MASK0);
		val |= hdmi->plug_mask;
		hdmi_writeb(val, HDMI_PHY_MASK0);

		/* Clear Hotplug interrupts */
		hdmi_writeb(hdmi->plug_event, HDMI_IH_PHY_STAT0);

		if(hdmi_inited) {
			if (!hdmi->dft_mode_set)
				mod_timer(&hdmi->jitter_timer, jiffies + msecs_to_jiffies(100));
			else
				mod_timer(&hdmi->jitter_timer, jiffies + HZ);
		}
	}

	/* Check HDCP  interrupt state */
	if (hdmi->hdmi_data.hdcp_enable) {
		val = hdmi_readb(HDMI_A_APIINTSTAT);
		if (val != 0) {
			/* Mute interrupts until interrupt handled */
			val = 0x7F;
			hdmi_writeb(val, HDMI_A_APIINTMSK);
			schedule_delayed_work(&(hdmi->hdcp_hdp_work), msecs_to_jiffies(50));
		}
	}

	spin_unlock_irqrestore(&hdmi->irq_lock, flags);
	return IRQ_HANDLED;
}

static void mxc_hdmi_setup(struct mxc_hdmi *hdmi, unsigned long event)
{
	struct fb_videomode m;
	const struct fb_videomode *edid_mode;

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	fb_var_to_videomode(&m, &hdmi->fbi->var);

	dev_dbg(&hdmi->pdev->dev, "%s - video mode changed\n", __func__);

	hdmi->vic = 0;
	if (!hdmi->requesting_vga_for_initialization) {
		/* Save mode if this isn't the result of requesting
		 * vga default. */
		memcpy(&hdmi->previous_non_vga_mode, &hdmi->fbi->var,
		       sizeof(struct fb_var_screeninfo));
		if (!list_empty(&hdmi->fbi->modelist)) {
			edid_mode = mxc_fb_find_nearest_mode(&m, &hdmi->fbi->modelist, false);
			pr_debug("edid mode vx:%d vy:%d", hdmi->fbi->var.xres_virtual, hdmi->fbi->var.yres_virtual);
			dump_fb_videomode((struct fb_videomode *)edid_mode);
			/* update fbi mode */
			hdmi->fbi->mode = (struct fb_videomode *)edid_mode;
			hdmi->vic = mxc_edid_mode_to_vic(edid_mode, 0);
		}
	}

	hdmi_disable_overflow_interrupts();

	if ((hdmi->hp_state == HDMI_HOTPLUG_CONNECTED_HDMI) ||
	    (hdmi->hp_state == HDMI_HOTPLUG_CONNECTED_NO_EDID)) {
		dev_dbg(&hdmi->pdev->dev, "CEA mode used vic=%d\n", hdmi->vic);
		hdmi_set_dvi_mode(0);
	} else {
		dev_dbg(&hdmi->pdev->dev, "VESA mode used vic=%d\n", hdmi->vic);
		hdmi_set_dvi_mode(1);
	}

	if ((hdmi->vic == 6) || (hdmi->vic == 7) ||
		(hdmi->vic == 21) || (hdmi->vic == 22) ||
		(hdmi->vic == 2) || (hdmi->vic == 3) ||
		(hdmi->vic == 17) || (hdmi->vic == 18))
		hdmi->hdmi_data.colorimetry = eITU601;
	else
		hdmi->hdmi_data.colorimetry = eITU709;

	if ((hdmi->vic == 10) || (hdmi->vic == 11) ||
		(hdmi->vic == 12) || (hdmi->vic == 13) ||
		(hdmi->vic == 14) || (hdmi->vic == 15) ||
		(hdmi->vic == 25) || (hdmi->vic == 26) ||
		(hdmi->vic == 27) || (hdmi->vic == 28) ||
		(hdmi->vic == 29) || (hdmi->vic == 30) ||
		(hdmi->vic == 35) || (hdmi->vic == 36) ||
		(hdmi->vic == 37) || (hdmi->vic == 38))
		hdmi->hdmi_data.video_mode.mPixelRepetitionOutput = 1;
	else
		hdmi->hdmi_data.video_mode.mPixelRepetitionOutput = 0;

	hdmi->hdmi_data.video_mode.mPixelRepetitionInput = 0;

	/* TODO: Get input format from IPU (via FB driver iface) */
	hdmi->hdmi_data.enc_in_format = RGB;

	hdmi->hdmi_data.enc_out_format = RGB;

	/* YCbCr only enabled in HDMI mode */
	if ((hdmi->hp_state == HDMI_HOTPLUG_CONNECTED_HDMI) &&
		!hdmi->hdmi_data.rgb_out_enable) {
		if (hdmi->edid_cfg.cea_ycbcr444)
			hdmi->hdmi_data.enc_out_format = YCBCR444;
		else if (hdmi->edid_cfg.cea_ycbcr422)
			hdmi->hdmi_data.enc_out_format = YCBCR422_8BITS;
	}

	/* IPU not support depth color output */
	hdmi->hdmi_data.enc_color_depth = 8;
	hdmi->hdmi_data.pix_repet_factor = 0;
	hdmi->hdmi_data.video_mode.mDataEnablePolarity = true;

	/* HDMI Initialization Step B.1 */
	hdmi_av_composer(hdmi);

	/* HDMI Initializateion Step B.2 */
	mxc_hdmi_phy_init(hdmi);

	/* HDMI Initialization Step B.3 */
	mxc_hdmi_enable_video_path(hdmi);

	/* not for DVI mode */
	if (hdmi->hp_state == HDMI_HOTPLUG_CONNECTED_HDMI) {
		dev_dbg(&hdmi->pdev->dev, "%s CEA mode\n", __func__);

		/* HDMI Initialization Step E - Configure audio */
		hdmi_enable_audio_clk(hdmi);

		/* HDMI Initialization Step F - Configure AVI InfoFrame */
		hdmi_config_AVI(hdmi);
	}

	hdmi_video_packetize(hdmi);
	hdmi_video_csc(hdmi);
	hdmi_video_sample(hdmi);

	mxc_hdmi_clear_overflow(hdmi);

	if (hdmi->hp_state == HDMI_HOTPLUG_CONNECTED_HDMI)
		hdmi_enable_overflow_interrupts();

	dev_dbg(&hdmi->pdev->dev, "%s exit\n\n", __func__);

}

/* Wait until we are registered to enable interrupts */
static void mxc_hdmi_fb_registered(struct mxc_hdmi *hdmi)
{
	unsigned long flags;

	if (hdmi->fb_reg)
		return;

	mxcfb_blank(FB_BLANK_POWERDOWN, hdmi->fbi);

	spin_lock_irqsave(&hdmi->irq_lock, flags);

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	hdmi_writeb(HDMI_PHY_I2CM_INT_ADDR_DONE_POL,
		    HDMI_PHY_I2CM_INT_ADDR);

	hdmi_writeb(HDMI_PHY_I2CM_CTLINT_ADDR_NAC_POL |
		    HDMI_PHY_I2CM_CTLINT_ADDR_ARBITRATION_POL,
		    HDMI_PHY_I2CM_CTLINT_ADDR);

	/* enable cable hot plug irq */
	hdmi_writeb(~hdmi->plug_mask, HDMI_PHY_MASK0);

	/* Clear Hotplug interrupts */
	hdmi_writeb(hdmi->plug_event, HDMI_IH_PHY_STAT0);

	/* Unmute interrupts */
	hdmi_writeb(~hdmi->plug_event, HDMI_IH_MUTE_PHY_STAT0);

	hdmi->fb_reg = true;

	spin_unlock_irqrestore(&hdmi->irq_lock, flags);

}

static int mxc_hdmi_fb_event(struct notifier_block *nb,
					unsigned long val, void *v)
{
	struct fb_event *event = v;
	struct mxc_hdmi *hdmi = container_of(nb, struct mxc_hdmi, nb);

	if (strcmp(event->info->fix.id, hdmi->fbi->fix.id))
		return 0;

	switch (val) {
	case FB_EVENT_FB_REGISTERED:
		dev_dbg(&hdmi->pdev->dev, "event=FB_EVENT_FB_REGISTERED\n");
		mxc_hdmi_fb_registered(hdmi);
		hdmi_set_registered(1);
		break;

	case FB_EVENT_FB_UNREGISTERED:
		dev_dbg(&hdmi->pdev->dev, "event=FB_EVENT_FB_UNREGISTERED\n");
		hdmi->fb_reg = false;
		hdmi_set_registered(0);
		break;

	case FB_EVENT_MODE_CHANGE:
		dev_dbg(&hdmi->pdev->dev, "event=FB_EVENT_MODE_CHANGE\n");
		if (hdmi->fb_reg)
			mxc_hdmi_setup(hdmi, val);
		break;

	case FB_EVENT_BLANK:
		if (!hdmi->fb_reg || hdmi->hp_state == HDMI_HOTPLUG_DISCONNECTED) {
			dev_dbg(&hdmi->pdev->dev,
				"event=FB_EVENT_BLANK - NOOP\n");
			break;
		}

		if ((*((int *)event->data) == FB_BLANK_UNBLANK) &&
			(*((int *)event->data) != hdmi->blank)) {
			dev_dbg(&hdmi->pdev->dev,
				"event=FB_EVENT_BLANK - UNBLANK\n");

			hdmi->blank = *((int *)event->data);

			/* Re-enable HPD interrupts */
			val = hdmi_readb(HDMI_PHY_MASK0);
			val &= ~hdmi->plug_mask;
			hdmi_writeb(val, HDMI_PHY_MASK0);

			/* Unmute interrupts */
			hdmi_writeb(~hdmi->plug_event, HDMI_IH_MUTE_PHY_STAT0);

			hdmi_set_blank_state(1);
			if (check_hdmi_state())
				mxc_hdmi_setup(hdmi, val);

		} else if (*((int *)event->data) != hdmi->blank) {
			dev_dbg(&hdmi->pdev->dev,
				"event=FB_EVENT_BLANK - BLANK\n");
			mxc_hdmi_abort_stream();
			hdmi_set_blank_state(0);

			mxc_hdmi_phy_disable(hdmi);

			if(hdmi->hp_state == HDMI_HOTPLUG_CONNECTED_DVI) {
				u8 val;
				pr_info("In DVI Mode disable interrupts\n");
				val = hdmi_readb(HDMI_IH_MUTE_PHY_STAT0);
				val |= hdmi->plug_event;
				hdmi_writeb(val, HDMI_IH_MUTE_PHY_STAT0);

				val = hdmi_readb(HDMI_PHY_MASK0);
				val |= hdmi->plug_mask;
				hdmi_writeb(val, HDMI_PHY_MASK0);

				hdmi_set_dvi_mode(1);
			}

			hdmi->blank = *((int *)event->data);
		} else
			dev_dbg(&hdmi->pdev->dev,
				"FB BLANK state no changed!\n");

		break;

	case FB_EVENT_SUSPEND:
		dev_dbg(&hdmi->pdev->dev,
			"event=FB_EVENT_SUSPEND\n");

		if (hdmi->blank == FB_BLANK_UNBLANK) {
			mxc_hdmi_phy_disable(hdmi);
			clk_disable(hdmi->hdmi_iahb_clk);
			clk_disable(hdmi->hdmi_isfr_clk);
			clk_disable(hdmi->mipi_core_clk);
		}
		break;

	case FB_EVENT_RESUME:
		dev_dbg(&hdmi->pdev->dev,
			"event=FB_EVENT_RESUME\n");

		if (hdmi->blank == FB_BLANK_UNBLANK) {
			clk_enable(hdmi->mipi_core_clk);
			clk_enable(hdmi->hdmi_iahb_clk);
			clk_enable(hdmi->hdmi_isfr_clk);
			mxc_hdmi_phy_init(hdmi);
		}
		break;
	default:
		break;
	}

	return 0;
}

static void hdmi_init_route(struct mxc_hdmi *hdmi)
{
	uint32_t hdmi_mux_setting, reg;
	int ipu_id, disp_id;

	ipu_id = mxc_hdmi_ipu_id;
	disp_id = mxc_hdmi_disp_id;

	if ((ipu_id > 1) || (ipu_id < 0)) {
		pr_err("Invalid IPU select for HDMI: %d. Set to 0\n", ipu_id);
		ipu_id = 0;
	}

	if ((disp_id > 1) || (disp_id < 0)) {
		pr_err("Invalid DI select for HDMI: %d. Set to 0\n", disp_id);
		disp_id = 0;
	}

	reg = readl(hdmi->gpr_hdmi_base);

	/* Configure the connection between IPU1/2 and HDMI */
	hdmi_mux_setting = 2*ipu_id + disp_id;

	/* GPR3, bits 2-3 = HDMI_MUX_CTL */
	reg &= ~0xd;
	reg |= hdmi_mux_setting << 2;

	writel(reg, hdmi->gpr_hdmi_base);

	/* Set HDMI event as SDMA event2 for HDMI audio */
	reg = readl(hdmi->gpr_sdma_base);
	reg |= 0x1;
	writel(reg, hdmi->gpr_sdma_base);
}

static void hdmi_hdcp_get_property(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;

	/* Check hdcp enable by dts.*/
	hdcp_init = of_property_read_bool(np, "fsl,hdcp");
	if (hdcp_init)
		dev_dbg(&pdev->dev, "hdcp enable\n");
	else
		dev_dbg(&pdev->dev, "hdcp disable\n");
}

static void hdmi_get_of_property(struct mxc_hdmi *hdmi)
{
	struct platform_device *pdev = hdmi->pdev;
	struct device_node *np = pdev->dev.of_node;
	const struct of_device_id *of_id =
			of_match_device(imx_hdmi_dt_ids, &pdev->dev);
	int ret;
	u32 phy_reg_vlev = 0, phy_reg_cksymtx = 0, phy_reg_txterm = 0;

	if (of_id) {
		pdev->id_entry = of_id->data;
		hdmi->cpu_type = pdev->id_entry->driver_data;
	}

	/* HDMI PHY register vlev and cksymtx preperty is optional.
	 * It is for specific board to pass HCT electrical part.
	 * Default value will been setting in HDMI PHY config function
	 * if it is not define in device tree.
	 */
	ret = of_property_read_u32(np, "fsl,phy_reg_vlev", &phy_reg_vlev);
	if (ret)
		dev_dbg(&pdev->dev, "No board specific HDMI PHY vlev\n");

	ret = of_property_read_u32(np, "fsl,phy_reg_cksymtx", &phy_reg_cksymtx);
	if (ret)
		dev_dbg(&pdev->dev, "No board specific HDMI PHY cksymtx\n");

	ret = of_property_read_u32(np, "fsl,phy_reg_txterm", &phy_reg_txterm);
	if (ret)
		dev_dbg(&pdev->dev, "No board specific HDMI PHY txterm\n");

	/* Specific phy config */
	hdmi->phy_config.reg_cksymtx = phy_reg_cksymtx;
	hdmi->phy_config.reg_vlev = phy_reg_vlev;
	hdmi->phy_config.reg_txterm = phy_reg_txterm;

}

/* HDMI Initialization Step A */
static int mxc_hdmi_disp_init(struct mxc_dispdrv_handle *disp,
			      struct mxc_dispdrv_setting *setting)
{
	int ret = 0;
	u32 i;
	const struct fb_videomode *mode;
	struct fb_videomode m;
	struct mxc_hdmi *hdmi = mxc_dispdrv_getdata(disp);
	int irq = platform_get_irq(hdmi->pdev, 0);

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	/* Check hdmi disp init once */
	if (hdmi_inited) {
		dev_err(&hdmi->pdev->dev,
				"Error only one HDMI output support now!\n");
		return -1;
	}

	hdmi_get_of_property(hdmi);

	if (irq < 0)
		return -ENODEV;

	/* Setting HDMI default to blank state */
	hdmi->blank = FB_BLANK_POWERDOWN;

	ret = ipu_di_to_crtc(&hdmi->pdev->dev, mxc_hdmi_ipu_id,
			     mxc_hdmi_disp_id, &setting->crtc);
	if (ret < 0)
		return ret;

	setting->if_fmt = IPU_PIX_FMT_RGB24;

	hdmi->dft_mode_str = setting->dft_mode_str;
	hdmi->default_bpp = setting->default_bpp;
	dev_dbg(&hdmi->pdev->dev, "%s - default mode %s bpp=%d\n",
		__func__, hdmi->dft_mode_str, hdmi->default_bpp);

	hdmi->fbi = setting->fbi;

	hdmi_init_route(hdmi);

	hdmi->mipi_core_clk = clk_get(&hdmi->pdev->dev, "mipi_core");
	if (IS_ERR(hdmi->mipi_core_clk)) {
		ret = PTR_ERR(hdmi->mipi_core_clk);
		dev_err(&hdmi->pdev->dev,
			"Unable to get mipi core clk: %d\n", ret);
		goto egetclk;
	}

	ret = clk_prepare_enable(hdmi->mipi_core_clk);
	if (ret < 0) {
		dev_err(&hdmi->pdev->dev,
				"Cannot enable mipi core clock: %d\n", ret);
		goto erate;
	}

	hdmi->hdmi_isfr_clk = clk_get(&hdmi->pdev->dev, "hdmi_isfr");
	if (IS_ERR(hdmi->hdmi_isfr_clk)) {
		ret = PTR_ERR(hdmi->hdmi_isfr_clk);
		dev_err(&hdmi->pdev->dev,
			"Unable to get HDMI clk: %d\n", ret);
		goto egetclk1;
	}

	ret = clk_prepare_enable(hdmi->hdmi_isfr_clk);
	if (ret < 0) {
		dev_err(&hdmi->pdev->dev,
			"Cannot enable HDMI isfr clock: %d\n", ret);
		goto erate1;
	}

	hdmi->hdmi_iahb_clk = clk_get(&hdmi->pdev->dev, "hdmi_iahb");
	if (IS_ERR(hdmi->hdmi_iahb_clk)) {
		ret = PTR_ERR(hdmi->hdmi_iahb_clk);
		dev_err(&hdmi->pdev->dev,
			"Unable to get HDMI clk: %d\n", ret);
		goto egetclk2;
	}

	ret = clk_prepare_enable(hdmi->hdmi_iahb_clk);
	if (ret < 0) {
		dev_err(&hdmi->pdev->dev,
			"Cannot enable HDMI iahb clock: %d\n", ret);
		goto erate2;
	}

	dev_dbg(&hdmi->pdev->dev, "Enabled HDMI clocks\n");

	/* Init DDC pins for HDCP  */
	if (hdcp_init) {
		hdmi->pinctrl = devm_pinctrl_get_select_default(&hdmi->pdev->dev);
		if (IS_ERR(hdmi->pinctrl)) {
			dev_err(&hdmi->pdev->dev, "can't get/select DDC pinctrl\n");
			goto erate2;
		}
	}

	/* Product and revision IDs */
	dev_info(&hdmi->pdev->dev,
		"Detected HDMI controller 0x%x:0x%x:0x%x:0x%x\n",
		hdmi_readb(HDMI_DESIGN_ID),
		hdmi_readb(HDMI_REVISION_ID),
		hdmi_readb(HDMI_PRODUCT_ID0),
		hdmi_readb(HDMI_PRODUCT_ID1));

	/* To prevent overflows in HDMI_IH_FC_STAT2, set the clk regenerator
	 * N and cts values before enabling phy */
	hdmi_init_clk_regenerator();

	INIT_LIST_HEAD(&hdmi->fbi->modelist);

	spin_lock_init(&hdmi->irq_lock);

	/* Set the default mode and modelist when disp init. */
	fb_find_mode(&hdmi->fbi->var, hdmi->fbi,
		     hdmi->dft_mode_str, /*NULL, 0, NULL,*/mxc_cea_mode, ARRAY_SIZE(mxc_cea_mode), NULL,
		     hdmi->default_bpp);

	console_lock();

	fb_destroy_modelist(&hdmi->fbi->modelist);

	/*Add all no interlaced CEA mode to default modelist */
	for (i = 0; i < ARRAY_SIZE(mxc_cea_mode); i++) {
		mode = &mxc_cea_mode[i];
		if (!(mode->vmode & FB_VMODE_INTERLACED) && (mode->xres != 0))
			fb_add_videomode(mode, &hdmi->fbi->modelist);
	}

	console_unlock();

	/* Find a nearest mode in default modelist */
	fb_var_to_videomode(&m, &hdmi->fbi->var);
	hdmi->dft_mode_set = false;
	/* Save default video mode */
	memcpy(&hdmi->default_mode, &m, sizeof(struct fb_videomode));

	mode = fb_find_nearest_mode(&m, &hdmi->fbi->modelist);
	if (!mode)
		pr_err("%s: could not find mode in modelist\n", __func__);

	hdmi->default_mode.vmode |= mode->vmode;
	dump_fb_videomode((struct fb_videomode *)mode);
	fb_videomode_to_var(&hdmi->fbi->var, mode);

	/* update fbi mode */
	hdmi->fbi->mode = (struct fb_videomode *)mode;

	/* Default setting HDMI working in HDMI mode*/
	hdmi->hp_state = HDMI_HOTPLUG_DISCONNECTED;

	hdmi->plug_event = HDMI_DVI_IH_STAT;
	hdmi->plug_mask = HDMI_DVI_STAT;

	setup_timer(&hdmi->jitter_timer, hotplug_work_launch, (unsigned long)hdmi);
	INIT_WORK(&hdmi->hdmi_connected, mxc_hdmi_cable_connected_worker);
	INIT_WORK(&hdmi->hotplug_work, hotplug_worker);
	INIT_DELAYED_WORK(&hdmi->hdcp_hdp_work, hdcp_hdp_worker);

	mutex_init(&hdmi->m_lock);

	/* Configure registers related to HDMI interrupt
	 * generation before registering IRQ. */
	hdmi_writeb(hdmi->plug_mask, HDMI_PHY_POL0);

	/* Clear Hotplug interrupts */
	hdmi_writeb(hdmi->plug_event, HDMI_IH_PHY_STAT0);

	hdmi->nb.notifier_call = mxc_hdmi_fb_event;
	ret = fb_register_client(&hdmi->nb);
	if (ret < 0)
		goto efbclient;

	memset(&hdmi->hdmi_data, 0, sizeof(struct hdmi_data_info));

	hdmi->hdmi_data.edid_filename = kstrdup(ext_edid, GFP_KERNEL);
	pr_info("Edid specified in file: %s\n", hdmi->hdmi_data.edid_filename);

	/* Default HDMI working in RGB mode */
	hdmi->hdmi_data.rgb_out_enable = true;

	if (!strcasecmp(rgb_quant_range, "limited")) {
		hdmi->hdmi_data.rgb_quant_range = HDMI_FC_AVICONF2_RGB_QUANT_LIMITED_RANGE;
	} else if (!strcasecmp(rgb_quant_range, "full")) {
		hdmi->hdmi_data.rgb_quant_range = HDMI_FC_AVICONF2_RGB_QUANT_FULL_RANGE;
	} else if (!strcasecmp(rgb_quant_range, "default")) {
		hdmi->hdmi_data.rgb_quant_range = HDMI_FC_AVICONF2_RGB_QUANT_DEFAULT;
	} else {
		hdmi->hdmi_data.rgb_quant_range = HDMI_FC_AVICONF2_RGB_QUANT_MASK;
	}

	if (!strcasecmp(enable_3d, "disable")) {
		hdmi->hdmi_data.enable_3d = 0;
	} else if (!strcasecmp(enable_3d, "0")) {
		hdmi->hdmi_data.enable_3d = 0;
	} else {
		hdmi->hdmi_data.enable_3d = 1;
	}

	if (!strcasecmp(enable_fract, "disable")) {
		hdmi->hdmi_data.enable_fract = 0;
	} else if (!strcasecmp(enable_fract, "0")) {
		hdmi->hdmi_data.enable_fract = 0;
	} else {
		hdmi->hdmi_data.enable_fract = 1;
	}

	ret = devm_request_irq(&hdmi->pdev->dev, irq, mxc_hdmi_hotplug, IRQF_SHARED,
			dev_name(&hdmi->pdev->dev), hdmi);
	if (ret < 0) {
		dev_err(&hdmi->pdev->dev,
			"Unable to request irq: %d\n", ret);
		goto ereqirq;
	}

	ret = device_create_file(&hdmi->pdev->dev, &dev_attr_fb_name);
	if (ret < 0)
		dev_warn(&hdmi->pdev->dev,
			"cound not create sys node for fb name\n");
	ret = device_create_file(&hdmi->pdev->dev, &dev_attr_cable_state);
	if (ret < 0)
		dev_warn(&hdmi->pdev->dev,
			"cound not create sys node for cable state\n");
	ret = device_create_file(&hdmi->pdev->dev, &dev_attr_edid);
	if (ret < 0)
		dev_warn(&hdmi->pdev->dev,
			"cound not create sys node for edid\n");

	ret = device_create_file(&hdmi->pdev->dev, &dev_attr_rgb_out_enable);
	if (ret < 0)
		dev_warn(&hdmi->pdev->dev,
			"cound not create sys node for rgb out enable\n");

	ret = device_create_file(&hdmi->pdev->dev, &dev_attr_rgb_quant_range);
	if (ret < 0)
		dev_warn(&hdmi->pdev->dev,
			"cound not create sys node for rgb quant range\n");

	ret = device_create_file(&hdmi->pdev->dev, &dev_attr_enable_3d);
	if (ret < 0)
		dev_warn(&hdmi->pdev->dev,
			"cound not create sys node for enable_3d\n");

	ret = device_create_file(&hdmi->pdev->dev, &dev_attr_enable_fract);
	if (ret < 0)
		dev_warn(&hdmi->pdev->dev,
			"cound not create sys node for enable_fract\n");

	ret = device_create_file(&hdmi->pdev->dev, &dev_attr_hdcp_enable);
	if (ret < 0)
		dev_warn(&hdmi->pdev->dev,
			"cound not create sys node for hdcp enable\n");

	dev_dbg(&hdmi->pdev->dev, "%s exit\n", __func__);

	hdmi_inited = true;

	return ret;

efbclient:
	free_irq(irq, hdmi);
ereqirq:
	clk_disable_unprepare(hdmi->hdmi_iahb_clk);
erate2:
	clk_put(hdmi->hdmi_iahb_clk);
egetclk2:
	clk_disable_unprepare(hdmi->hdmi_isfr_clk);
erate1:
	clk_put(hdmi->hdmi_isfr_clk);
egetclk1:
	clk_disable_unprepare(hdmi->mipi_core_clk);
erate:
	clk_put(hdmi->mipi_core_clk);
egetclk:
	dev_dbg(&hdmi->pdev->dev, "%s error exit\n", __func__);

	return ret;
}

static void mxc_hdmi_disp_deinit(struct mxc_dispdrv_handle *disp)
{
	struct mxc_hdmi *hdmi = mxc_dispdrv_getdata(disp);

	dev_dbg(&hdmi->pdev->dev, "%s\n", __func__);

	fb_unregister_client(&hdmi->nb);

	clk_disable_unprepare(hdmi->hdmi_isfr_clk);
	clk_put(hdmi->hdmi_isfr_clk);
	clk_disable_unprepare(hdmi->hdmi_iahb_clk);
	clk_put(hdmi->hdmi_iahb_clk);
	clk_disable_unprepare(hdmi->mipi_core_clk);
	clk_put(hdmi->mipi_core_clk);

	kfree(hdmi->hdmi_data.edid_filename);
	platform_device_unregister(hdmi->pdev);

	hdmi_inited = false;
}

static struct mxc_dispdrv_driver mxc_hdmi_drv = {
	.name	= DISPDRV_HDMI,
	.init	= mxc_hdmi_disp_init,
	.deinit	= mxc_hdmi_disp_deinit,
	.enable = mxc_hdmi_power_on,
	.disable = mxc_hdmi_power_off,
};


static int mxc_hdmi_open(struct inode *inode, struct file *file)
{
	return 0;
}

static long mxc_hdmi_ioctl(struct file *file,
		unsigned int cmd, unsigned long arg)
{
	int __user *argp = (void __user *)arg;
	int ret = 0;

	switch (cmd) {
	case HDMI_IOC_GET_RESOURCE:
		ret = copy_to_user(argp, &g_hdmi->hdmi_data,
				sizeof(g_hdmi->hdmi_data)) ? -EFAULT : 0;
		break;
	case HDMI_IOC_GET_CPU_TYPE:
		*argp = g_hdmi->cpu_type;
		break;
	default:
		pr_debug("Unsupport cmd %d\n", cmd);
		break;
     }
     return ret;
}

static int mxc_hdmi_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations mxc_hdmi_fops = {
	.owner = THIS_MODULE,
	.open = mxc_hdmi_open,
	.release = mxc_hdmi_release,
	.unlocked_ioctl = mxc_hdmi_ioctl,
};


static int mxc_hdmi_probe(struct platform_device *pdev)
{
	struct mxc_hdmi *hdmi;
	struct device *temp_class;
	struct resource *res;
	int ret = 0;

	/* Check I2C driver is loaded and available
	 * check hdcp function is enable by dts */
	hdmi_hdcp_get_property(pdev);
	if (!hdmi_i2c && !hdcp_init)
		return -ENODEV;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENOENT;

	hdmi = devm_kzalloc(&pdev->dev,
				sizeof(struct mxc_hdmi),
				GFP_KERNEL);
	if (!hdmi) {
		dev_err(&pdev->dev, "Cannot allocate device data\n");
		ret = -ENOMEM;
		goto ealloc;
	}
	g_hdmi = hdmi;

	hdmi_major = register_chrdev(hdmi_major, "mxc_hdmi", &mxc_hdmi_fops);
	if (hdmi_major < 0) {
		printk(KERN_ERR "HDMI: unable to get a major for HDMI\n");
		ret = -EBUSY;
		goto ealloc;
	}

	hdmi_class = class_create(THIS_MODULE, "mxc_hdmi");
	if (IS_ERR(hdmi_class)) {
		ret = PTR_ERR(hdmi_class);
		goto err_out_chrdev;
	}

	temp_class = device_create(hdmi_class, NULL, MKDEV(hdmi_major, 0),
				   NULL, "mxc_hdmi");
	if (IS_ERR(temp_class)) {
		ret = PTR_ERR(temp_class);
		goto err_out_class;
	}

	hdmi->pdev = pdev;

	hdmi->core_pdev = platform_device_alloc("mxc_hdmi_core", -1);
	if (!hdmi->core_pdev) {
		pr_err("%s failed platform_device_alloc for hdmi core\n",
			__func__);
		ret = -ENOMEM;
		goto ecore;
	}

	hdmi->gpr_base = ioremap(res->start, resource_size(res));
	if (!hdmi->gpr_base) {
		dev_err(&pdev->dev, "ioremap failed\n");
		ret = -ENOMEM;
		goto eiomap;
	}

	hdmi->gpr_hdmi_base = hdmi->gpr_base + 3;
	hdmi->gpr_sdma_base = hdmi->gpr_base;

	hdmi_inited = false;

	hdmi->disp_mxc_hdmi = mxc_dispdrv_register(&mxc_hdmi_drv);
	if (IS_ERR(hdmi->disp_mxc_hdmi)) {
		dev_err(&pdev->dev, "Failed to register dispdrv - 0x%x\n",
			(int)hdmi->disp_mxc_hdmi);
		ret = (int)hdmi->disp_mxc_hdmi;
		goto edispdrv;
	}
	mxc_dispdrv_setdata(hdmi->disp_mxc_hdmi, hdmi);

	platform_set_drvdata(pdev, hdmi);

	return 0;
edispdrv:
	iounmap(hdmi->gpr_base);
eiomap:
	platform_device_put(hdmi->core_pdev);
ecore:
	kfree(hdmi);
err_out_class:
	device_destroy(hdmi_class, MKDEV(hdmi_major, 0));
	class_destroy(hdmi_class);
err_out_chrdev:
	unregister_chrdev(hdmi_major, "mxc_hdmi");
ealloc:
	return ret;
}

static int mxc_hdmi_remove(struct platform_device *pdev)
{
	struct mxc_hdmi *hdmi = platform_get_drvdata(pdev);
	int irq = platform_get_irq(pdev, 0);

	fb_unregister_client(&hdmi->nb);

	mxc_dispdrv_puthandle(hdmi->disp_mxc_hdmi);
	mxc_dispdrv_unregister(hdmi->disp_mxc_hdmi);
	iounmap(hdmi->gpr_base);
	/* No new work will be scheduled, wait for running ISR */
	free_irq(irq, hdmi);
	kfree(hdmi);
	g_hdmi = NULL;

	return 0;
}

static struct platform_driver mxc_hdmi_driver = {
	.probe = mxc_hdmi_probe,
	.remove = mxc_hdmi_remove,
	.driver = {
		.name = "mxc_hdmi",
		.of_match_table	= imx_hdmi_dt_ids,
		.owner = THIS_MODULE,
	},
};

static int __init mxc_hdmi_init(void)
{
	return platform_driver_register(&mxc_hdmi_driver);
}
module_init(mxc_hdmi_init);

static void __exit mxc_hdmi_exit(void)
{
	if (hdmi_major > 0) {
		device_destroy(hdmi_class, MKDEV(hdmi_major, 0));
		class_destroy(hdmi_class);
		unregister_chrdev(hdmi_major, "mxc_hdmi");
		hdmi_major = 0;
	}

	platform_driver_unregister(&mxc_hdmi_driver);
}
module_exit(mxc_hdmi_exit);

static int mxc_hdmi_i2c_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	if (!i2c_check_functionality(client->adapter,
				I2C_FUNC_SMBUS_BYTE | I2C_FUNC_I2C))
		return -ENODEV;

	hdmi_i2c = client;

	return 0;
}

static int mxc_hdmi_i2c_remove(struct i2c_client *client)
{
	hdmi_i2c = NULL;
	return 0;
}

static const struct of_device_id imx_hdmi_i2c_match[] = {
	{ .compatible = "fsl,imx6-hdmi-i2c", },
	{ /* sentinel */ }
};

static const struct i2c_device_id mxc_hdmi_i2c_id[] = {
	{ "mxc_hdmi_i2c", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c, mxc_hdmi_i2c_id);

static struct i2c_driver mxc_hdmi_i2c_driver = {
	.driver = {
		   .name = "mxc_hdmi_i2c",
			.of_match_table	= imx_hdmi_i2c_match,
		   },
	.probe = mxc_hdmi_i2c_probe,
	.remove = mxc_hdmi_i2c_remove,
	.id_table = mxc_hdmi_i2c_id,
};

static int __init mxc_hdmi_i2c_init(void)
{
	return i2c_add_driver(&mxc_hdmi_i2c_driver);
}

static void __exit mxc_hdmi_i2c_exit(void)
{
	i2c_del_driver(&mxc_hdmi_i2c_driver);
}

subsys_initcall(mxc_hdmi_i2c_init);
module_exit(mxc_hdmi_i2c_exit);

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
