// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright 2019 NXP
 */

#include <linux/busfreq-imx.h>
#include <linux/clk.h>
#include <linux/iopoll.h>
#include <linux/media-bus-format.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/types.h>
#include <drm/drm_fourcc.h>
#include <video/imx-lcdifv3.h>
#include <video/videomode.h>

#include "lcdifv3-regs.h"

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/device.h>
#include <linux/miscdevice.h>

#include <linux/lviconfig_parameters.h>
#include <linux/notifier.h>

#define DRIVER_NAME "imx-lcdifv3"
#define DEVICE_NAME "lvicolor"

static int lcdifv3_major;
static struct class *lcdifv3_class;

static uint8_t lcdifv3_config_initialized = 0;

struct lcdifv3_csc_params {
	int brightness;
	int contrast;
	int saturation;
	int r_gain;
	int g_gain;
	int b_gain;
};

#define LCDIFV3_IOC_MAGIC 'L'
#define LCDIFV3_IOC_SET_CSC _IOW(LCDIFV3_IOC_MAGIC, 1, struct lcdifv3_csc_params)
#define LCDIFV3_IOC_GET_CSC _IOR(LCDIFV3_IOC_MAGIC, 2, struct lcdifv3_csc_params)

#define LCDIF2_BASE_ADDR 0x32e90000

static uint8_t lcdifv3_ioctl_is_created = 0;

int color_matrix[3][4] = {
	{ 256, 0, 0, 0 }, // R
	{ 0, 256, 0, 0 }, // G
	{ 0, 0, 256, 0 }, // B
};

static int imx_lcdifv3_ioctl_create(void);
void lcdifv3_build_color_matrix(int matrix[3][4], int brightness, int contrast, int saturation, int r_gain, int g_gain, int b_gain);
void lcdifv3_config_rgb_to_ycbcr(void __iomem *base, int matrix[3][4]);

struct lcdifv3_soc {
	struct device *dev;

	int irq;
	void __iomem *base;
	struct regmap *gpr;
	atomic_t rpm_suspended;

	struct miscdevice misc; /* add this line */

	struct clk *clk_pix;
	struct clk *clk_disp_axi;
	struct clk *clk_disp_apb;

	u32 thres_low_mul;
	u32 thres_low_div;
	u32 thres_high_mul;
	u32 thres_high_div;

	struct lcdifv3_csc_params current_csc;
	struct notifier_block lviconfig_nb;
};

struct lcdifv3_soc_pdata {
	bool hsync_invert;
	bool vsync_invert;
	bool de_invert;
	bool hdmimix;
};

struct lcdifv3_platform_reg {
	struct lcdifv3_client_platformdata pdata;
	char *name;
};

static struct lcdifv3_platform_reg client_reg[] = {
	{
		.pdata = {},
		.name = "imx-lcdifv3-crtc",
	},
};

static struct lcdifv3_soc_pdata imx8mp_lcdif1_pdata = {
	.hsync_invert = false,
	.vsync_invert = false,
	.de_invert = false,
	.hdmimix = false,
};

static struct lcdifv3_soc_pdata imx8mp_lcdif2_pdata = {
	.hsync_invert = false,
	.vsync_invert = false,
	.de_invert = true,
	.hdmimix = false,
};

static struct lcdifv3_soc_pdata imx8mp_lcdif3_pdata = {
	.hsync_invert = false,
	.vsync_invert = false,
	.de_invert = false,
	.hdmimix = true,
};
static const struct of_device_id imx_lcdifv3_dt_ids[] = { {
								  .compatible = "fsl,imx8mp-lcdif1",
								  .data = &imx8mp_lcdif1_pdata,
							  },
							  {
								  .compatible = "fsl,imx8mp-lcdif2",
								  .data = &imx8mp_lcdif2_pdata,
							  },
							  {
								  .compatible = "fsl,imx8mp-lcdif3",
								  .data = &imx8mp_lcdif3_pdata,
							  },
							  { /* sentinel */ } };
MODULE_DEVICE_TABLE(of, imx_lcdifv3_dt_ids);

static int lcdifv3_enable_clocks(struct lcdifv3_soc *lcdifv3)
{
	int ret;

	if (lcdifv3->clk_disp_axi) {
		ret = clk_prepare_enable(lcdifv3->clk_disp_axi);
		if (ret)
			return ret;
	}

	if (lcdifv3->clk_disp_apb) {
		ret = clk_prepare_enable(lcdifv3->clk_disp_apb);
		if (ret)
			goto disable_disp_axi;
	}

	ret = clk_prepare_enable(lcdifv3->clk_pix);
	if (ret)
		goto disable_disp_apb;

	return 0;

disable_disp_apb:
	if (lcdifv3->clk_disp_apb)
		clk_disable_unprepare(lcdifv3->clk_disp_apb);
disable_disp_axi:
	if (lcdifv3->clk_disp_axi)
		clk_disable_unprepare(lcdifv3->clk_disp_axi);

	return ret;
}

static void lcdifv3_disable_clocks(struct lcdifv3_soc *lcdifv3)
{
	clk_disable_unprepare(lcdifv3->clk_pix);

	if (lcdifv3->clk_disp_axi)
		clk_disable_unprepare(lcdifv3->clk_disp_axi);

	if (lcdifv3->clk_disp_apb)
		clk_disable_unprepare(lcdifv3->clk_disp_apb);
}

static void lcdifv3_enable_plane_panic(struct lcdifv3_soc *lcdifv3)
{
	u32 panic_thres, thres_low, thres_high;

	/* apb clock has been enabled */

	/* As suggestion, the thres_low should be 1/3 FIFO,
	 * and thres_high should be 2/3 FIFO (The FIFO size
	 * is 8KB = 512 * 128bit).
	 * threshold = n * 128bit (n: 0 ~ 511)
	 */
	thres_low = DIV_ROUND_UP(511 * lcdifv3->thres_low_mul, lcdifv3->thres_low_div);
	thres_high = DIV_ROUND_UP(511 * lcdifv3->thres_high_mul, lcdifv3->thres_high_div);

	panic_thres = PANIC0_THRES_PANIC_THRES_LOW(thres_low) | PANIC0_THRES_PANIC_THRES_HIGH(thres_high);

	writel(panic_thres, lcdifv3->base + LCDIFV3_PANIC0_THRES);

	/* Enable Panic:
	 *
	 * As designed, the panic won't trigger an irq,
	 * so it is unnecessary to handle this as an irq
	 * and NoC + QoS modules will handle panic
	 * automatically.
	 */
	writel(INT_ENABLE_D1_PLANE_PANIC_EN, lcdifv3->base + LCDIFV3_INT_ENABLE_D1);
}

int lcdifv3_vblank_irq_get(struct lcdifv3_soc *lcdifv3)
{
	return lcdifv3->irq;
}
EXPORT_SYMBOL(lcdifv3_vblank_irq_get);

/* TODO: use VS_BLANK or VSYNC? */
void lcdifv3_vblank_irq_enable(struct lcdifv3_soc *lcdifv3)
{
	uint32_t int_enable_d0;

	int_enable_d0 = readl(lcdifv3->base + LCDIFV3_INT_ENABLE_D0);
	int_enable_d0 |= INT_STATUS_D0_VS_BLANK;

	/* W1C */
	writel(INT_STATUS_D0_VS_BLANK, lcdifv3->base + LCDIFV3_INT_STATUS_D0);
	/* enable */
	writel(int_enable_d0, lcdifv3->base + LCDIFV3_INT_ENABLE_D0);
}
EXPORT_SYMBOL(lcdifv3_vblank_irq_enable);

void lcdifv3_vblank_irq_disable(struct lcdifv3_soc *lcdifv3)
{
	uint32_t int_enable_d0;

	int_enable_d0 = readl(lcdifv3->base + LCDIFV3_INT_ENABLE_D0);
	int_enable_d0 &= ~INT_STATUS_D0_VS_BLANK;

	/* disable */
	writel(int_enable_d0, lcdifv3->base + LCDIFV3_INT_ENABLE_D0);
	/* W1C */
	writel(INT_STATUS_D0_VS_BLANK, lcdifv3->base + LCDIFV3_INT_STATUS_D0);
}
EXPORT_SYMBOL(lcdifv3_vblank_irq_disable);

void lcdifv3_vblank_irq_clear(struct lcdifv3_soc *lcdifv3)
{
	/* W1C */
	writel(INT_STATUS_D0_VS_BLANK, lcdifv3->base + LCDIFV3_INT_STATUS_D0);
}
EXPORT_SYMBOL(lcdifv3_vblank_irq_clear);

static uint32_t lcdifv3_get_bpp_from_fmt(uint32_t format)
{
	/* TODO: only support RGB for now */

	switch (format) {
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_BGR565:
	case DRM_FORMAT_ARGB1555:
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_ABGR1555:
	case DRM_FORMAT_XBGR1555:
		return 16;
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX8888:
		return 32;
	default:
		/* unsupported format */
		return 0;
	}
}

/*
 * Get the bus format supported by LCDIF
 * according to drm fourcc format
 */
int lcdifv3_get_bus_fmt_from_pix_fmt(struct lcdifv3_soc *lcdifv3, uint32_t format)
{
	uint32_t bpp;

	bpp = lcdifv3_get_bpp_from_fmt(format);
	if (!bpp)
		return -EINVAL;

	switch (bpp) {
	case 16:
		return MEDIA_BUS_FMT_RGB565_1X16;
	case 18:
		return MEDIA_BUS_FMT_RGB666_1X18;
	case 24:
	case 32:
		return MEDIA_BUS_FMT_RGB888_1X24;
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL(lcdifv3_get_bus_fmt_from_pix_fmt);

int lcdifv3_set_pix_fmt(struct lcdifv3_soc *lcdifv3, u32 format)
{
	struct drm_format_name_buf format_name;
	uint32_t ctrldescl0_5 = 0;

	ctrldescl0_5 = readl(lcdifv3->base + LCDIFV3_CTRLDESCL0_5);

	ctrldescl0_5 &= ~(CTRLDESCL0_5_BPP(0xf) | CTRLDESCL0_5_YUV_FORMAT(0x3));

	switch (format) {
	case DRM_FORMAT_RGB565:
		ctrldescl0_5 |= CTRLDESCL0_5_BPP(BPP16_RGB565);
		break;
	case DRM_FORMAT_ARGB1555:
	case DRM_FORMAT_XRGB1555:
		ctrldescl0_5 |= CTRLDESCL0_5_BPP(BPP16_ARGB1555);
		break;
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
		ctrldescl0_5 |= CTRLDESCL0_5_BPP(BPP32_ARGB8888);
		break;
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
		ctrldescl0_5 |= CTRLDESCL0_5_BPP(BPP32_ABGR8888);
		break;
	default:
		dev_err(lcdifv3->dev, "unsupported pixel format: %s\n", drm_get_format_name(format, &format_name));
		return -EINVAL;
	}

	writel(ctrldescl0_5, lcdifv3->base + LCDIFV3_CTRLDESCL0_5);

	return 0;
}
EXPORT_SYMBOL(lcdifv3_set_pix_fmt);

void lcdifv3_set_bus_fmt(struct lcdifv3_soc *lcdifv3, u32 bus_format)
{
	uint32_t disp_para = 0;

	disp_para = readl(lcdifv3->base + LCDIFV3_DISP_PARA);

	/* clear line pattern bits */
	disp_para &= ~DISP_PARA_LINE_PATTERN(0xf);

	switch (bus_format) {
	case MEDIA_BUS_FMT_RGB565_1X16:
		disp_para |= DISP_PARA_LINE_PATTERN(LP_RGB565);
		break;
	case MEDIA_BUS_FMT_RGB888_1X24:
		disp_para |= DISP_PARA_LINE_PATTERN(LP_RGB888_OR_YUV444);
		break;
	default:
		dev_err(lcdifv3->dev, "unknown bus format: %#x\n", bus_format);
		return;
	}

	/* config display mode: default is normal mode */
	disp_para &= ~DISP_PARA_DISP_MODE(3);
	disp_para |= DISP_PARA_DISP_MODE(0);

	writel(disp_para, lcdifv3->base + LCDIFV3_DISP_PARA);
}
EXPORT_SYMBOL(lcdifv3_set_bus_fmt);

void lcdifv3_set_fb_addr(struct lcdifv3_soc *lcdifv3, int id, u32 addr)
{
	switch (id) {
	case 0:
		/* primary plane */
		writel(addr, lcdifv3->base + LCDIFV3_CTRLDESCL_LOW0_4);
		break;
	default:
		/* TODO: add overlay support */
		return;
	}
}
EXPORT_SYMBOL(lcdifv3_set_fb_addr);

void lcdifv3_set_fb_hcrop(struct lcdifv3_soc *lcdifv3, u32 src_w, u32 pitch, bool crop)
{
	uint32_t ctrldescl0_3 = 0;

	/* config P_SIZE and T_SIZE:
	 * 1. P_SIZE and T_SIZE should never
	 *    be less than AXI bus width.
	 * 2. P_SIZE should never be less than T_SIZE.
	 */
	ctrldescl0_3 |= CTRLDESCL0_3_P_SIZE(2);
	ctrldescl0_3 |= CTRLDESCL0_3_T_SIZE(2);

	/* config pitch */
	ctrldescl0_3 |= CTRLDESCL0_3_PITCH(pitch);

	/* enable frame clear to clear FIFO data on
	 * every vsync blank period to make sure no
	 * dirty data exits to affect next frame
	 * display, otherwise some flicker issue may
	 * be observed in some cases.
	 */
	ctrldescl0_3 |= CTRLDESCL0_3_STATE_CLEAR_VSYNC;

	writel(ctrldescl0_3, lcdifv3->base + LCDIFV3_CTRLDESCL0_3);
}
EXPORT_SYMBOL(lcdifv3_set_fb_hcrop);

void lcdifv3_set_mode(struct lcdifv3_soc *lcdifv3, struct videomode *vmode)
{
	const struct of_device_id *of_id = of_match_device(imx_lcdifv3_dt_ids, lcdifv3->dev);
	const struct lcdifv3_soc_pdata *soc_pdata;
	u32 disp_size, hsyn_para, vsyn_para, vsyn_hsyn_width, ctrldescl0_1;

	if (unlikely(!of_id))
		return;
	soc_pdata = of_id->data;

	/* set pixel clock rate */
	clk_disable_unprepare(lcdifv3->clk_pix);
	clk_set_rate(lcdifv3->clk_pix, vmode->pixelclock);
	clk_prepare_enable(lcdifv3->clk_pix);

	/* config display timings */
	disp_size = DISP_SIZE_DELTA_Y(vmode->vactive) | DISP_SIZE_DELTA_X(vmode->hactive);
	writel(disp_size, lcdifv3->base + LCDIFV3_DISP_SIZE);

	WARN_ON(!vmode->hback_porch || !vmode->hfront_porch);
	hsyn_para = HSYN_PARA_BP_H(vmode->hback_porch) | HSYN_PARA_FP_H(vmode->hfront_porch);
	writel(hsyn_para, lcdifv3->base + LCDIFV3_HSYN_PARA);

	WARN_ON(!vmode->vback_porch || !vmode->vfront_porch);
	vsyn_para = VSYN_PARA_BP_V(vmode->vback_porch) | VSYN_PARA_FP_V(vmode->vfront_porch);
	writel(vsyn_para, lcdifv3->base + LCDIFV3_VSYN_PARA);

	WARN_ON(!vmode->vsync_len || !vmode->hsync_len);
	vsyn_hsyn_width = VSYN_HSYN_WIDTH_PW_V(vmode->vsync_len) | VSYN_HSYN_WIDTH_PW_H(vmode->hsync_len);
	writel(vsyn_hsyn_width, lcdifv3->base + LCDIFV3_VSYN_HSYN_WIDTH);

	/* config layer size */
	/* TODO: 32bits alignment for width */
	ctrldescl0_1 = CTRLDESCL0_1_HEIGHT(vmode->vactive) | CTRLDESCL0_1_WIDTH(vmode->hactive);
	writel(ctrldescl0_1, lcdifv3->base + LCDIFV3_CTRLDESCL0_1);

	/* Polarities */
	if (soc_pdata) {
		if ((soc_pdata->hsync_invert && vmode->flags & DISPLAY_FLAGS_HSYNC_HIGH) || (!soc_pdata->hsync_invert && vmode->flags & DISPLAY_FLAGS_HSYNC_LOW))
			writel(CTRL_INV_HS, lcdifv3->base + LCDIFV3_CTRL_SET);
		else
			writel(CTRL_INV_HS, lcdifv3->base + LCDIFV3_CTRL_CLR);

		if ((soc_pdata->vsync_invert && vmode->flags & DISPLAY_FLAGS_VSYNC_HIGH) || (!soc_pdata->vsync_invert && vmode->flags & DISPLAY_FLAGS_VSYNC_LOW))
			writel(CTRL_INV_VS, lcdifv3->base + LCDIFV3_CTRL_SET);
		else
			writel(CTRL_INV_VS, lcdifv3->base + LCDIFV3_CTRL_CLR);

		if ((soc_pdata->de_invert && vmode->flags & DISPLAY_FLAGS_DE_HIGH) || (!soc_pdata->de_invert && vmode->flags & DISPLAY_FLAGS_DE_LOW))
			writel(CTRL_INV_DE, lcdifv3->base + LCDIFV3_CTRL_SET);
		else
			writel(CTRL_INV_DE, lcdifv3->base + LCDIFV3_CTRL_CLR);
	} else {
		if (vmode->flags & DISPLAY_FLAGS_HSYNC_LOW)
			writel(CTRL_INV_HS, lcdifv3->base + LCDIFV3_CTRL_SET);
		else
			writel(CTRL_INV_HS, lcdifv3->base + LCDIFV3_CTRL_CLR);
		if (vmode->flags & DISPLAY_FLAGS_VSYNC_LOW)
			writel(CTRL_INV_VS, lcdifv3->base + LCDIFV3_CTRL_SET);
		else
			writel(CTRL_INV_VS, lcdifv3->base + LCDIFV3_CTRL_CLR);
		if (vmode->flags & DISPLAY_FLAGS_DE_LOW)
			writel(CTRL_INV_DE, lcdifv3->base + LCDIFV3_CTRL_SET);
		else
			writel(CTRL_INV_DE, lcdifv3->base + LCDIFV3_CTRL_CLR);
	}

	if (vmode->flags & DISPLAY_FLAGS_PIXDATA_NEGEDGE)
		writel(CTRL_INV_PXCK, lcdifv3->base + LCDIFV3_CTRL_CLR);
	else
		writel(CTRL_INV_PXCK, lcdifv3->base + LCDIFV3_CTRL_SET);
}
EXPORT_SYMBOL(lcdifv3_set_mode);

void lcdifv3_en_shadow_load(struct lcdifv3_soc *lcdifv3)
{
	u32 ctrldescl0_5;

	ctrldescl0_5 = readl(lcdifv3->base + LCDIFV3_CTRLDESCL0_5);
	ctrldescl0_5 |= CTRLDESCL0_5_SHADOW_LOAD_EN;

	writel(ctrldescl0_5, lcdifv3->base + LCDIFV3_CTRLDESCL0_5);
}
EXPORT_SYMBOL(lcdifv3_en_shadow_load);

void lcdifv3_enable_controller(struct lcdifv3_soc *lcdifv3)
{
	u32 disp_para, ctrldescl0_5;

	disp_para = readl(lcdifv3->base + LCDIFV3_DISP_PARA);
	ctrldescl0_5 = readl(lcdifv3->base + LCDIFV3_CTRLDESCL0_5);

	/* disp on */
	disp_para |= DISP_PARA_DISP_ON;
	writel(disp_para, lcdifv3->base + LCDIFV3_DISP_PARA);

	/* enable layer dma */
	ctrldescl0_5 |= CTRLDESCL0_5_EN;
	writel(ctrldescl0_5, lcdifv3->base + LCDIFV3_CTRLDESCL0_5);
}
EXPORT_SYMBOL(lcdifv3_enable_controller);

void lcdifv3_disable_controller(struct lcdifv3_soc *lcdifv3)
{
	u32 disp_para, ctrldescl0_5;

	disp_para = readl(lcdifv3->base + LCDIFV3_DISP_PARA);
	ctrldescl0_5 = readl(lcdifv3->base + LCDIFV3_CTRLDESCL0_5);

	/* disable dma */
	ctrldescl0_5 &= ~CTRLDESCL0_5_EN;
	writel(ctrldescl0_5, lcdifv3->base + LCDIFV3_CTRLDESCL0_5);

	/* dma config only takes effect at the end of
	 * one frame, so add delay to wait dma disable
	 * done before turn off disp.
	 */
	usleep_range(20000, 25000);

	/* disp off */
	disp_para &= ~DISP_PARA_DISP_ON;
	writel(disp_para, lcdifv3->base + LCDIFV3_DISP_PARA);
}
EXPORT_SYMBOL(lcdifv3_disable_controller);

long lcdifv3_pix_clk_round_rate(struct lcdifv3_soc *lcdifv3, unsigned long rate)
{
	if (unlikely(!rate))
		return -EINVAL;

	return clk_round_rate(lcdifv3->clk_pix, rate);
}
EXPORT_SYMBOL(lcdifv3_pix_clk_round_rate);

static int hdmimix_lcdif3_setup(struct lcdifv3_soc *lcdifv3)
{
	struct device *dev = lcdifv3->dev;
	int ret;

	struct clk_bulk_data clocks[] = {
		{ .id = "mix_apb" },   { .id = "mix_axi" },   { .id = "xtl_24m" },   { .id = "mix_pix" },   { .id = "lcdif_apb" },
		{ .id = "lcdif_axi" }, { .id = "lcdif_pdi" }, { .id = "lcdif_pix" }, { .id = "lcdif_spu" }, { .id = "noc_hdmi" },
	};

	/* power up hdmimix lcdif and nor */
	ret = device_reset(dev);
	if (ret)
		dev_warn(dev, "No hdmimix sub reset found\n");
	if (ret == -EPROBE_DEFER)
		return ret;

	/* enable lpcg of hdmimix lcdif and nor */
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(clocks), clocks);
	if (ret < 0)
		return ret;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(clocks), clocks);
	if (ret < 0)
		return ret;

	return 0;
}

static int platform_remove_device_fn(struct device *dev, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);

	platform_device_unregister(pdev);

	return 0;
}

static void platform_device_unregister_children(struct platform_device *pdev)
{
	device_for_each_child(&pdev->dev, NULL, platform_remove_device_fn);
}

static DEFINE_MUTEX(lcdifv3_client_id_mutex);
static int lcdifv3_client_id;

static int lcdifv3_add_client_devices(struct lcdifv3_soc *lcdifv3)
{
	int ret = 0, i, id;
	struct device *dev = lcdifv3->dev;
	struct platform_device *pdev = NULL;
	struct device_node *of_node;

	for (i = 0; i < ARRAY_SIZE(client_reg); i++) {
		of_node = of_graph_get_port_by_id(dev->of_node, i);
		if (!of_node) {
			dev_info(dev, "no port@%d node in %s\n", i, dev->of_node->full_name);
			continue;
		}
		of_node_put(of_node);

		mutex_lock(&lcdifv3_client_id_mutex);
		id = lcdifv3_client_id++;
		mutex_unlock(&lcdifv3_client_id_mutex);

		pdev = platform_device_alloc(client_reg[i].name, id);
		if (!pdev) {
			dev_err(dev, "Can't allocate port pdev\n");
			ret = -ENOMEM;
			goto err_register;
		}

		pdev->dev.parent = dev;
		client_reg[i].pdata.of_node = of_node;

		/* make child device 'dma_mask' to point to its
		 * coherent dma mask, otherwise later probe will
		 * print warning message: 'DMA mask not set'.
		 */
		pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;

		ret = platform_device_add_data(pdev, &client_reg[i].pdata, sizeof(client_reg[i].pdata));
		if (!ret)
			ret = platform_device_add(pdev);
		if (ret) {
			platform_device_put(pdev);
			goto err_register;
		}

		pdev->dev.of_node = of_node;
	}

	if (!pdev)
		return -ENODEV;

	return 0;

err_register:
	platform_device_unregister_children(to_platform_device(dev));
	return ret;
}

static int imx_lcdifv3_check_thres_value(u32 mul, u32 div)
{
	if (!div)
		return -EINVAL;

	if (mul > div)
		return -EINVAL;

	return 0;
}

static void imx_lcdifv3_of_parse_thres(struct lcdifv3_soc *lcdifv3)
{
	int ret;
	u32 thres_low[2], thres_high[2];
	struct device_node *np = lcdifv3->dev->of_node;

	/* default 'thres-low' value:  FIFO * 1/3;
	 * default 'thres-high' value: FIFO * 2/3.
	 */
	lcdifv3->thres_low_mul = 1;
	lcdifv3->thres_low_div = 3;
	lcdifv3->thres_high_mul = 2;
	lcdifv3->thres_high_div = 3;

	ret = of_property_read_u32_array(np, "thres-low", thres_low, 2);
	if (!ret) {
		/* check the value effectiveness */
		ret = imx_lcdifv3_check_thres_value(thres_low[0], thres_low[1]);
		if (!ret) {
			lcdifv3->thres_low_mul = thres_low[0];
			lcdifv3->thres_low_div = thres_low[1];
		}
	}

	ret = of_property_read_u32_array(np, "thres-high", thres_high, 2);
	if (!ret) {
		/* check the value effectiveness */
		ret = imx_lcdifv3_check_thres_value(thres_high[0], thres_high[1]);
		if (!ret) {
			lcdifv3->thres_high_mul = thres_high[0];
			lcdifv3->thres_high_div = thres_high[1];
		}
	}
}

static long lcdifv3_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	// pr_info("[%s] lcdifv3_ioctl: called with cmd=0x%x\n", __func__, cmd);

	struct lcdifv3_csc_params csc_params;
	struct lcdifv3_soc *lcdifv3 = file->private_data;
	int ret = 0;

	// pr_info("[%s] lcdifv3_ioctl: lcdifv3=%px, lcdifv3->base=%px\n",
	// 	__func__, lcdifv3, lcdifv3 ? lcdifv3->base : NULL);

	switch (cmd) {
	case LCDIFV3_IOC_SET_CSC:
		// pr_info("[%s] lcdifv3_ioctl: LCDIFV3_IOC_SET_CSC received\n",
		// 	__func__);

		if (copy_from_user(&csc_params, (struct lcdifv3_csc_params *)arg, sizeof(struct lcdifv3_csc_params))) {
			pr_err("[%s] lcdifv3_ioctl: copy_from_user failed\n", __func__);
			ret = -EFAULT;
			break;
		}

		lcdifv3->current_csc = csc_params;

		*(confMonitorMode[0].Brightness.data) = csc_params.brightness;
		*(confMonitorMode[0].Contrast.data) = csc_params.contrast;
		*(confMonitorMode[0].Saturation.data) = csc_params.saturation;
		*(confMonitorMode[0].ColorGainR.data) = csc_params.r_gain;
		*(confMonitorMode[0].ColorGainG.data) = csc_params.g_gain;
		*(confMonitorMode[0].ColorGainB.data) = csc_params.b_gain;

		lcdifv3_build_color_matrix(color_matrix, csc_params.brightness, csc_params.contrast, csc_params.saturation, csc_params.r_gain, csc_params.g_gain, csc_params.b_gain);
		// pr_info("[%s] lcdifv3_ioctl: Color matrix calculated.\n",
		// 	__func__);

		lcdifv3_config_rgb_to_ycbcr(lcdifv3->base, color_matrix);
		// pr_info("[%s] lcdifv3_ioctl: CSC registers configured.\n",
		// 	__func__);

		break;

	case LCDIFV3_IOC_GET_CSC:
		// pr_info("[%s] lcdifv3_ioctl: LCDIFV3_IOC_GET_CSC received\n",
		// 	__func__);

		if (copy_to_user((struct lcdifv3_csc_params *)arg, &lcdifv3->current_csc, sizeof(struct lcdifv3_csc_params))) {
			printk(KERN_ERR "lcdifv3_ioctl: copy_to_user failed\n");

			ret = -EFAULT;
		}

		break;

	default:
		pr_err("[%s] lcdifv3_ioctl: Unknown ioctl command: 0x%x\n", __func__, cmd);
		ret = -ENOTTY;
		break;
	}
	return ret;
}

static int lcdifv3_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct lcdifv3_soc *lcdifv3;

	/* miscdevice is embedded in lcdifv3_soc */
	lcdifv3 = container_of(misc, struct lcdifv3_soc, misc);
	if (!lcdifv3) {
		pr_err("[%s] lcdifv3_open: no context\n", __func__);
		return -ENODEV;
	}

	/* now stash the real context pointer for ioctl() */
	file->private_data = lcdifv3;

	pr_info("[%s] lcdifv3_open: got base=%px\n", __func__, lcdifv3->base);
	return 0;
}

static int lcdifv3_release(struct inode *inode, struct file *file)
{
	return 0;
}

// static int lcdifv3_remove(struct inode *inode, struct file *file)
// {
// 	pr_info("[%s] lcdifv3: ioctl remove call\n", __func__);

// 		return 0;
// }

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = lcdifv3_open,
	.release = lcdifv3_release,
	.unlocked_ioctl = lcdifv3_ioctl,
};

/**
 * lcdifv3_lviconfig_notifier - react to a confMonitorMode[0] parameter change.
 * Rebuilds and re-applies the CSC matrix whenever brightness, contrast,
 * saturation or an RGB gain parameter is updated through lviconfig.
 */
static int lcdifv3_lviconfig_notifier(struct notifier_block *nb,
				      unsigned long action, void *data)
{
	struct lcdifv3_soc *lcdifv3 =
		container_of(nb, struct lcdifv3_soc, lviconfig_nb);
	ConfigParam *param = (ConfigParam *)data;
	int local_matrix[3][4];
	int brightness, contrast, saturation, r_gain, g_gain, b_gain;

	/* Only care about the six display-pipeline parameters */
	if (param != &confMonitorMode[0].Brightness &&
	    param != &confMonitorMode[0].Contrast   &&
	    param != &confMonitorMode[0].Saturation  &&
	    param != &confMonitorMode[0].ColorGainR  &&
	    param != &confMonitorMode[0].ColorGainG  &&
	    param != &confMonitorMode[0].ColorGainB)
		return NOTIFY_DONE;

	if (!lcdifv3->base)
		return NOTIFY_DONE;

	brightness = *(confMonitorMode[0].Brightness.data);
	contrast   = *(confMonitorMode[0].Contrast.data);
	saturation = *(confMonitorMode[0].Saturation.data);
	r_gain     = *(confMonitorMode[0].ColorGainR.data);
	g_gain     = *(confMonitorMode[0].ColorGainG.data);
	b_gain     = *(confMonitorMode[0].ColorGainB.data);

	if (brightness == 0 && contrast == 0 && saturation == 0) {
		brightness = 128;
		contrast   = 128;
		saturation = 128;
	}
	if (r_gain == 0 && g_gain == 0 && b_gain == 0) {
		r_gain = 128;
		g_gain = 128;
		b_gain = 128;
	}

	lcdifv3->current_csc.brightness = min(max(brightness, 10), 255);
	lcdifv3->current_csc.contrast   = min(max(contrast,   10), 255);
	lcdifv3->current_csc.saturation = min(max(saturation, 10), 255);
	lcdifv3->current_csc.r_gain     = min(max(r_gain,      0), 255);
	lcdifv3->current_csc.g_gain     = min(max(g_gain,      0), 255);
	lcdifv3->current_csc.b_gain     = min(max(b_gain,      0), 255);

	lcdifv3_build_color_matrix(local_matrix,
				   lcdifv3->current_csc.brightness,
				   lcdifv3->current_csc.contrast,
				   lcdifv3->current_csc.saturation,
				   lcdifv3->current_csc.r_gain,
				   lcdifv3->current_csc.g_gain,
				   lcdifv3->current_csc.b_gain);
	lcdifv3_config_rgb_to_ycbcr(lcdifv3->base, local_matrix);

	pr_info("[lcdifv3] notifier: CSC updated: B=%d C=%d S=%d R=%d G=%d B=%d\n",
		lcdifv3->current_csc.brightness, lcdifv3->current_csc.contrast,
		lcdifv3->current_csc.saturation, lcdifv3->current_csc.r_gain,
		lcdifv3->current_csc.g_gain,     lcdifv3->current_csc.b_gain);

	return NOTIFY_OK;
}

static int imx_lcdifv3_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct lcdifv3_soc *lcdifv3;
	struct resource *res;
	const struct of_device_id *of_id;
	const struct lcdifv3_soc_pdata *soc_pdata;

	pr_info("[%s] probe begin\n", __func__);

	if (ConfigParam_IsInitialized() == 0) {
		pr_warn("[%s] ConfigParam not yet initialized, deferring probe\n", __func__);
		return -EPROBE_DEFER;
	} else {
		pr_info("[%s] ConfigParam is initialized, proceeding with probe\n", __func__);
	}

	of_id = of_match_device(imx_lcdifv3_dt_ids, dev);
	if (!of_id) {
		pr_warn("[%s] OF data missing\n", __func__);
		return -EINVAL;
	}

	soc_pdata = of_id->data;

	lcdifv3 = devm_kzalloc(dev, sizeof(*lcdifv3), GFP_KERNEL);
	if (!lcdifv3) {
		pr_warn("[%s] Can't allocate 'lcdifv3_soc' structure\n", __func__);
		return -ENOMEM;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	lcdifv3->irq = platform_get_irq(pdev, 0);
	if (lcdifv3->irq < 0) {
		pr_warn("[%s] No irq get, ret=%d\n", __func__, lcdifv3->irq);
		return lcdifv3->irq;
	}

	lcdifv3->clk_pix = devm_clk_get(dev, "pix");
	if (IS_ERR(lcdifv3->clk_pix)) {
		ret = PTR_ERR(lcdifv3->clk_pix);
		pr_warn("[%s] No pix clock get: %d\n", __func__, ret);
		return ret;
	}

	lcdifv3->clk_disp_axi = devm_clk_get(dev, "disp-axi");
	if (IS_ERR(lcdifv3->clk_disp_axi))
		lcdifv3->clk_disp_axi = NULL;

	lcdifv3->clk_disp_apb = devm_clk_get(dev, "disp-apb");
	if (IS_ERR(lcdifv3->clk_disp_apb))
		lcdifv3->clk_disp_apb = NULL;

	lcdifv3->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(lcdifv3->base))
		return PTR_ERR(lcdifv3->base);

	lcdifv3->dev = dev;

	/* reset controller to avoid any conflict
	 * with uboot splash screen settings.
	 */
	if (of_device_is_compatible(np, "fsl,imx8mp-lcdif1")) {
		/* TODO: Maybe the clock enable should
		 *	 be done in reset driver.
		 */
		clk_prepare_enable(lcdifv3->clk_disp_axi);
		clk_prepare_enable(lcdifv3->clk_disp_apb);

		writel(CTRL_SW_RESET, lcdifv3->base + LCDIFV3_CTRL_CLR);

		ret = device_reset(dev);
		if (ret)
			pr_warn("[%s] lcdif1 reset failed: %d\n", __func__, ret);

		clk_disable_unprepare(lcdifv3->clk_disp_axi);
		clk_disable_unprepare(lcdifv3->clk_disp_apb);
	}

	imx_lcdifv3_of_parse_thres(lcdifv3);

	platform_set_drvdata(pdev, lcdifv3);

	if (!of_device_is_compatible(np, "fsl,imx8mp-lcdif2"))
		goto skip_ioctl;

	lcdifv3->misc.minor = MISC_DYNAMIC_MINOR;
	lcdifv3->misc.name = DEVICE_NAME; // "/dev/lvicolor"
	lcdifv3->misc.fops = &fops;
	lcdifv3->misc.parent = &pdev->dev;

	ret = misc_register(&lcdifv3->misc);
	// if (ret)
	// dev_warn(...);

skip_ioctl:

	if (soc_pdata->hdmimix) {
		ret = hdmimix_lcdif3_setup(lcdifv3);
		if (ret < 0) {
			pr_warn("[%s] hdmimix lcdif3 setup failed\n", __func__);
			return ret;
		}
	}

	atomic_set(&lcdifv3->rpm_suspended, 0);
	pm_runtime_enable(dev);
	atomic_inc(&lcdifv3->rpm_suspended);

	int brightness = *(confMonitorMode[0].Brightness.data);
	int contrast = *(confMonitorMode[0].Contrast.data);
	int saturation = *(confMonitorMode[0].Saturation.data);
	int r_gain = *(confMonitorMode[0].ColorGainR.data);
	int g_gain = *(confMonitorMode[0].ColorGainG.data);
	int b_gain = *(confMonitorMode[0].ColorGainB.data);

	if (brightness == 0 && contrast == 0 && saturation == 0) {
		brightness = 128;
		contrast = 128;
		saturation = 128;
	}

	if (r_gain == 0 && g_gain == 0 && b_gain == 0) {
		r_gain = 128;
		g_gain = 128;
		b_gain = 128;
	}

	lcdifv3->current_csc.brightness = min(max(brightness, 10), 255);
	lcdifv3->current_csc.contrast = min(max(contrast, 10), 255);
	lcdifv3->current_csc.saturation = min(max(saturation, 10), 255);
	lcdifv3->current_csc.r_gain = min(max(r_gain, 0), 255);
	lcdifv3->current_csc.g_gain = min(max(g_gain, 0), 255);
	lcdifv3->current_csc.b_gain = min(max(b_gain, 0), 255);

	pr_info("[%s] confMonitorMode values:\n", __func__);
	pr_info("[%s] confMonitorMode[0]: R_Gain=%d, G_Gain=%d, B_Gain=%d, Brightness=%d, Contrast=%d, Saturation=%d\n", __func__, *(confMonitorMode[0].ColorGainR.data),
		*(confMonitorMode[0].ColorGainG.data), *(confMonitorMode[0].ColorGainB.data), *(confMonitorMode[0].Brightness.data), *(confMonitorMode[0].Contrast.data),
		*(confMonitorMode[0].Saturation.data));

	pr_info("[%s] Initial CSC values: brightness=%d, contrast=%d, saturation=%d, r_gain=%d, g_gain=%d, b_gain=%d\n", __func__, lcdifv3->current_csc.brightness, lcdifv3->current_csc.contrast,
		lcdifv3->current_csc.saturation, lcdifv3->current_csc.r_gain, lcdifv3->current_csc.g_gain, lcdifv3->current_csc.b_gain);

	pr_info("[%s] probe end\n", __func__);

	lcdifv3->lviconfig_nb.notifier_call = lcdifv3_lviconfig_notifier;
	lviconfig_register_notifier(&lcdifv3->lviconfig_nb);

	return lcdifv3_add_client_devices(lcdifv3);
}

static int imx_lcdifv3_remove(struct platform_device *pdev)
{
	pr_info("[%s] remove call\n", __func__);

	if (lcdifv3_ioctl_is_created) {
		// Cleanup: Destroy device and class, unregister char device
		device_destroy(lcdifv3_class, MKDEV(lcdifv3_major, 0));
		class_destroy(lcdifv3_class);
		unregister_chrdev(lcdifv3_major, DEVICE_NAME);

		lcdifv3_ioctl_is_created = 0;
	}

	struct lcdifv3_soc *lcdifv3 = platform_get_drvdata(pdev);

	lviconfig_unregister_notifier(&lcdifv3->lviconfig_nb);

	/* undo misc so no leaks */
	if (lcdifv3->misc.name)
		misc_deregister(&lcdifv3->misc);

	pm_runtime_disable(&pdev->dev);

	return 0;
}

#ifdef CONFIG_PM

// BT.601 luma weights as fixed-point values (8-bit fraction)
#define RW 76 // 0.299 * 256 = ~76
#define GW 150 // 0.587 * 256 = ~150
#define BW 29 // 0.114 * 256 = ~29

// Fixed-point multiplication: (a * b) >> 8
#define FP_MUL(a, b) (((a) * (b)) >> 8)

// Direct conversion for input values to fixed point (no floats)
#define CONTRAST_TO_FP(x) ((x) * 2) // Scale 0-128-255 to 0-256-510 fixed point
#define SATURATION_TO_FP(x) ((x) * 2) // Scale 0-128-255 to 0-256-510 fixed point
#define GAIN_TO_FP(x) ((x) * 2) // Scale 0-128-255 to 0-256-510 fixed point

/**
 * lcdifv3_build_color_matrix - Build a color correction matrix with RGB gains
 * @matrix: 3x4 transformation matrix to fill
 * @brightness: Brightness adjustment [0-255], 128 is neutral
 * @contrast: Contrast adjustment [0-255], 128 is neutral (1.0x)
 * @saturation: Saturation adjustment [0-255], 128 is neutral (1.0x)
 * @r_gain: Red gain adjustment [0-255], 128 is neutral (1.0x)
 * @g_gain: Green gain adjustment [0-255], 128 is neutral (1.0x)
 * @b_gain: Blue gain adjustment [0-255], 128 is neutral (1.0x)
 *
 * This function builds a color correction matrix for adjusting brightness,
 * contrast, saturation and individual RGB channel gains.
 */
void lcdifv3_build_color_matrix(int matrix[3][4], int brightness, int contrast, int saturation, int r_gain, int g_gain, int b_gain)
{
	// pr_info("[%s] Setting color matrix\n", __func__);

	// Convert parameters to fixed-point representation (all integer math)
	int b_fp = brightness - 128; // -128 to 127
	int c_fp = CONTRAST_TO_FP(contrast); // 0 to 510 (0.0 to 2.0 in fixed point)
	int s_fp = SATURATION_TO_FP(saturation); // 0 to 510 (0.0 to 2.0 in fixed point)
	int r_gain_fp = GAIN_TO_FP(r_gain); // 0 to 510 (0.0 to 2.0 in fixed point)
	int g_gain_fp = GAIN_TO_FP(g_gain); // 0 to 510 (0.0 to 2.0 in fixed point)
	int b_gain_fp = GAIN_TO_FP(b_gain); // 0 to 510 (0.0 to 2.0 in fixed point)

	// Compute inverse saturation (1.0 - saturation) in fixed point
	int inv_s_fp = 256 - s_fp; // 256 = 1.0 in fixed point

	// Luma weight scale
	int rw = FP_MUL(RW, inv_s_fp);
	int gw = FP_MUL(GW, inv_s_fp);
	int bw = FP_MUL(BW, inv_s_fp);

	// Compute half contrast offset: 0.5 * (contrast - 1.0)
	int half_contrast_offset = ((c_fp - 256) >> 1);

	// Compute brightness offset term
	int b_offset = b_fp - half_contrast_offset;

	// RGB transform rows with RGB gains applied
	// R row
	matrix[0][0] = FP_MUL(FP_MUL(c_fp, (rw + s_fp)), r_gain_fp); // R←R with gain
	matrix[0][1] = FP_MUL(c_fp, gw); // R←G
	matrix[0][2] = FP_MUL(c_fp, bw); // R←B
	matrix[0][3] = b_offset; // R offset

	// G row
	matrix[1][0] = FP_MUL(c_fp, rw); // G←R
	matrix[1][1] = FP_MUL(FP_MUL(c_fp, (gw + s_fp)), g_gain_fp); // G←G with gain
	matrix[1][2] = FP_MUL(c_fp, bw); // G←B
	matrix[1][3] = b_offset; // G offset

	// B row
	matrix[2][0] = FP_MUL(c_fp, rw); // B←R
	matrix[2][1] = FP_MUL(c_fp, gw); // B←G
	matrix[2][2] = FP_MUL(FP_MUL(c_fp, (bw + s_fp)), b_gain_fp); // B←B with gain
	matrix[2][3] = b_offset; // B offset

	// pr_info("[%s] Color matrix set!\n", __func__);
}

// Function to apply the color matrix to the CSC registers
void lcdifv3_config_rgb_to_ycbcr(void __iomem *base, int matrix[3][4])
{
	uint32_t v;

	/* 1) Turn OFF bypass so CSC will run */
	writel(0x4, base + LCDIFV3_CSC0_CTRL);
	mb();

	/* 2) Y row → COEF0/1:  A1 (R), A2 (G), A3 (B) */
	v = CSC0_COEF0_A2(matrix[0][1]) /* A2 = G→Y */
	    | CSC0_COEF0_A1(matrix[0][0]); /* A1 = R→Y */
	writel(v, base + LCDIFV3_CSC0_COEF0);

	v = CSC0_COEF1_B1(matrix[1][0]) /* B1 = R→U */
	    | CSC0_COEF1_A3(matrix[0][2]); /* A3 = B→Y */
	writel(v, base + LCDIFV3_CSC0_COEF1);

	/* 3) U row → COEF2:  B2 (G), B3 (B) */
	v = CSC0_COEF2_B2(matrix[1][1]) /* B2 = G→U */
	    | CSC0_COEF2_B3(matrix[1][2]); /* B3 = B→U */
	writel(v, base + LCDIFV3_CSC0_COEF2);

	/* 4) V row → COEF3/4:  C1 (R), C2 (G), C3 (B) */
	v = CSC0_COEF3_C2(matrix[2][1]) /* C2 = G→V */
	    | CSC0_COEF3_C1(matrix[2][0]); /* C1 = R→V */
	writel(v, base + LCDIFV3_CSC0_COEF3);

	v = CSC0_COEF4_C3(matrix[2][2]) /* C3 = B→V */
	    | CSC0_COEF4_D1(matrix[0][3]); /* D1 = Y offset */
	writel(v, base + LCDIFV3_CSC0_COEF4);

	/* 5) Offsets D2 (U), D3 (V) → COEF5 */
	v = CSC0_COEF5_D2(matrix[1][3]) /* D2 = U offset */
	    | CSC0_COEF5_D3(matrix[2][3]); /* D3 = V offset */
	writel(v, base + LCDIFV3_CSC0_COEF5);

	mb();

	// pr_info("[%s] configuring coeff registers with RGB-YCbCr values.\n",
	// 	__func__);
}

static int imx_lcdifv3_runtime_suspend(struct device *dev)
{
	struct lcdifv3_soc *lcdifv3 = dev_get_drvdata(dev);

	if (atomic_inc_return(&lcdifv3->rpm_suspended) > 1)
		return 0;

	lcdifv3_disable_clocks(lcdifv3);

	release_bus_freq(BUS_FREQ_HIGH);

	return 0;
}

static int imx_lcdifv3_ioctl_create(void)
{
	int ret;

	if (lcdifv3_ioctl_is_created == 0) {
		//register character device
		lcdifv3_major = register_chrdev(0, DEVICE_NAME, &fops);
		if (lcdifv3_major < 0) {
			pr_err("[%s] Failed to register character device\n", __func__);
			ret = lcdifv3_major;
		}

		// Create the device class
		lcdifv3_class = class_create(THIS_MODULE, DEVICE_NAME);
		if (IS_ERR(lcdifv3_class)) {
			unregister_chrdev(lcdifv3_major, DEVICE_NAME);
			ret = PTR_ERR(lcdifv3_class);
		}

		// Create the device node
		if (device_create(lcdifv3_class, NULL, MKDEV(lcdifv3_major, 0), NULL, DEVICE_NAME) == NULL) {
			class_destroy(lcdifv3_class);
			unregister_chrdev(lcdifv3_major, DEVICE_NAME);
			ret = -1;
		}

		lcdifv3_ioctl_is_created = 1;
	}

	return ret;
}

static int imx_lcdifv3_runtime_resume(struct device *dev)
{
	int ret = 0;
	struct lcdifv3_soc *lcdifv3 = dev_get_drvdata(dev);

	if (unlikely(!atomic_read(&lcdifv3->rpm_suspended))) {
		dev_warn(lcdifv3->dev, "Unbalanced %s!\n", __func__);
		return 0;
	}

	if (!atomic_dec_and_test(&lcdifv3->rpm_suspended))
		return 0;

	request_bus_freq(BUS_FREQ_HIGH);

	ret = lcdifv3_enable_clocks(lcdifv3);
	if (ret) {
		release_bus_freq(BUS_FREQ_HIGH);
		return ret;
	}

	/* clear sw_reset */
	writel(CTRL_SW_RESET, lcdifv3->base + LCDIFV3_CTRL_CLR);

	imx_lcdifv3_ioctl_create();

	// Print pointers properly
	pr_info("[%s] lcdifv3=%px, lcdifv3->base=%px\n", __func__, lcdifv3, lcdifv3 ? lcdifv3->base : NULL);

	if (!lcdifv3_config_initialized) {
		/* Initialize color matrix */
		lcdifv3_config_initialized = 1;
		pr_info("[%s] Configuration parameters initialized!\n", __func__);
	}

	struct lcdifv3_csc_params csc_params;

	int brightness = *(confMonitorMode[0].Brightness.data);
	int contrast = *(confMonitorMode[0].Contrast.data);
	int saturation = *(confMonitorMode[0].Saturation.data);
	int r_gain = *(confMonitorMode[0].ColorGainR.data);
	int g_gain = *(confMonitorMode[0].ColorGainG.data);
	int b_gain = *(confMonitorMode[0].ColorGainB.data);

	if (brightness == 0 && contrast == 0 && saturation == 0) {
		brightness = 128;
		contrast = 128;
		saturation = 128;
	}

	if (r_gain == 0 && g_gain == 0 && b_gain == 0) {
		r_gain = 128;
		g_gain = 128;
		b_gain = 128;
	}

	csc_params.brightness = min(max(brightness, 10), 255);
	csc_params.contrast = min(max(contrast, 10), 255);
	csc_params.saturation = min(max(saturation, 10), 255);
	csc_params.r_gain = min(max(r_gain, 0), 255);
	csc_params.g_gain = min(max(g_gain, 0), 255);
	csc_params.b_gain = min(max(b_gain, 0), 255);

	pr_info("[%s] CSC params: brightness=%d, contrast=%d, saturation=%d, r_gain=%d, g_gain=%d, b_gain=%d\n", __func__, csc_params.brightness, csc_params.contrast, csc_params.saturation,
		csc_params.r_gain, csc_params.g_gain, csc_params.b_gain);

	lcdifv3_build_color_matrix(color_matrix, csc_params.brightness, csc_params.contrast, csc_params.saturation, csc_params.r_gain, csc_params.g_gain, csc_params.b_gain);
	pr_info("[%s] lcdifv3_ioctl: Color matrix calculated.\n", __func__);
	lcdifv3_config_rgb_to_ycbcr(lcdifv3->base, color_matrix);
	dev_info(lcdifv3->dev, "CSC0_CTRL after resume = 0x%08x\n", readl(lcdifv3->base + LCDIFV3_CSC0_CTRL));

	/* enable plane FIFO panic */
	lcdifv3_enable_plane_panic(lcdifv3);

	return ret;
}
#endif

#ifdef CONFIG_PM_SLEEP
static int imx_lcdifv3_suspend(struct device *dev)
{
	return imx_lcdifv3_runtime_suspend(dev);
}

static int imx_lcdifv3_resume(struct device *dev)
{
	return imx_lcdifv3_runtime_resume(dev);
}
#endif

static const struct dev_pm_ops imx_lcdifv3_pm_ops = { SET_LATE_SYSTEM_SLEEP_PM_OPS(imx_lcdifv3_suspend, imx_lcdifv3_resume)
							      SET_RUNTIME_PM_OPS(imx_lcdifv3_runtime_suspend, imx_lcdifv3_runtime_resume, NULL) };

struct platform_driver imx_lcdifv3_driver = {
	.probe = imx_lcdifv3_probe,
	.remove = imx_lcdifv3_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = imx_lcdifv3_dt_ids,
		.pm = &imx_lcdifv3_pm_ops,
},
};

module_platform_driver(imx_lcdifv3_driver);

MODULE_DESCRIPTION("NXP i.MX LCDIFV3 Display Controller driver");
MODULE_AUTHOR("Fancy Fang <chen.fang@nxp.com>");
MODULE_LICENSE("GPL");