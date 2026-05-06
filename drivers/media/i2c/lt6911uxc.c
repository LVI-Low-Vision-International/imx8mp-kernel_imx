// SPDX-License-Identifier: GPL-2.0-only
/*
 * lt6911uxc.c - Lontium LT6911UXC 4K HDMI-to-MIPI-CSI2 bridge driver
 *
 * Adapted for i.MX8M Plus / standard V4L2 subdev API from:
 *   lt6911uxc_zhaw.c by Lukas Neuner <neur@zhaw.ch> (ZHAW InES-HPMM)
 *   Copyright (c) 2020 Lukas Neuner, Alexey Gromov, Gianluca Pargaetzi
 *
 * This driver supports the Lontium LT6911UXC HDMI 2.0 to MIPI CSI-2
 * bridge chip. It detects HDMI signal presence, reads timing/format
 * information, and exposes a V4L2 subdevice source pad to the i.MX8
 * ISI / MIPI-CSI2 capture pipeline.
 *
 * Device tree compatible string: "lontium,lt6911uxc"
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/v4l2-dv-timings.h>
#include <linux/workqueue.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dv-timings.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mc.h>

#include "lt6911uxc_regs.h"

/* --------------------------------------------------------------------------
 * Module parameters
 */

static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Debug level (0-3)");

/* --------------------------------------------------------------------------
 * Custom V4L2 control IDs
 */

#define V4L2_CID_USER_LT6911UXC_BASE		(V4L2_CID_USER_BASE + 0x1090)
#define LT6911UXC_CID_AUDIO_SAMPLING_RATE	(V4L2_CID_USER_LT6911UXC_BASE + 1)
#define LT6911UXC_CID_AUDIO_PRESENT		(V4L2_CID_USER_LT6911UXC_BASE + 2)
#define LT6911UXC_CID_NUM_MIPI_LANES		(V4L2_CID_USER_LT6911UXC_BASE + 3)
#define LT6911UXC_CID_HDMI_SOURCE_PRESENT	(V4L2_CID_USER_LT6911UXC_BASE + 4)
#define LT6911UXC_NUM_CTRLS			4

/* --------------------------------------------------------------------------
 * Supported formats
 */

static const u32 lt6911uxc_mbus_formats[] = {
	MEDIA_BUS_FMT_UYVY8_2X8,
};

/* --------------------------------------------------------------------------
 * DV timings capability: supports up to 4K@30 / 1080p@60
 */

static const struct v4l2_dv_timings_cap lt6911uxc_timings_cap = {
	.type = V4L2_DV_BT_656_1120,
	.reserved = { 0 },
	V4L2_INIT_BT_TIMINGS(
		640,  3840,		/* min/max width */
		480,  2160,		/* min/max height */
		25000000, 600000000,	/* min/max pixel clock (Hz) */
		V4L2_DV_BT_STD_CEA861 | V4L2_DV_BT_STD_DMT |
		V4L2_DV_BT_STD_CVT,
		V4L2_DV_BT_CAP_PROGRESSIVE | V4L2_DV_BT_CAP_CUSTOM |
		V4L2_DV_BT_CAP_REDUCED_BLANKING)
};

static struct v4l2_dv_timings lt6911uxc_default_timings =
	V4L2_DV_BT_CEA_1920X1080P60;

/* --------------------------------------------------------------------------
 * Driver state
 */

struct lt6911uxc {
	struct v4l2_subdev		sd;
	struct media_pad		pad;

	struct i2c_client		*client;
	struct mutex			lock; /* protects timings, signal_present */

	/* GPIO */
	struct gpio_desc		*sleep_gpio; /* active-high sleep (LOW = awake) */
	struct gpio_desc		*reset_gpio; /* active-low reset */

	/* V4L2 controls */
	struct v4l2_ctrl_handler	ctrl_handler;
	struct v4l2_ctrl		*audio_sampling_rate_ctrl;
	struct v4l2_ctrl		*audio_present_ctrl;
	struct v4l2_ctrl		*num_mipi_lanes_ctrl;
	struct v4l2_ctrl		*hdmi_source_present_ctrl;

	/* Timings / format */
	struct v4l2_dv_timings		timings;	   /* configured timings */
	struct v4l2_dv_timings		detected_timings;  /* from HDMI PHY */
	u32				mbus_fmt_code;

	/* State flags */
	u8				bank;        /* active register bank */
	bool				enable_i2c;  /* external I2C control enabled */
	bool				streaming;
	bool				csi_tx_enabled;
	bool				signal_present;

	/* Polling work (fallback when IRQ GPIO not wired) */
	struct delayed_work		poll_work;
};

static inline struct lt6911uxc *to_lt6911uxc(struct v4l2_subdev *sd)
{
	return container_of(sd, struct lt6911uxc, sd);
}

/* --------------------------------------------------------------------------
 * I2C register access
 *
 * The LT6911UXC uses a banked register map.  Register addresses are 16-bit:
 *   bits[15:8] = bank selector  (written to reg 0xFF on the device)
 *   bits[ 7:0] = register offset within that bank
 */

static void lt6911uxc_reg_bank(struct lt6911uxc *state, u8 bank)
{
	struct i2c_client *client = state->client;
	u8 buf[2] = { 0xFF, bank };
	struct i2c_msg msg = {
		.addr  = client->addr,
		.flags = 0,
		.len   = 2,
		.buf   = buf,
	};

	if (state->bank == bank)
		return;

	if (i2c_transfer(client->adapter, &msg, 1) != 1)
		dev_err(&client->dev, "bank switch to 0x%02X failed\n", bank);
	else
		state->bank = bank;
}

static void lt6911uxc_i2c_wr8(struct lt6911uxc *state, u16 reg, u8 val)
{
	struct i2c_client *client = state->client;
	u8 buf[2] = { reg & 0xFF, val };
	struct i2c_msg msg = {
		.addr  = client->addr,
		.flags = 0,
		.len   = 2,
		.buf   = buf,
	};

	lt6911uxc_reg_bank(state, (reg >> 8) & 0xFF);

	if (i2c_transfer(client->adapter, &msg, 1) != 1)
		dev_err(&client->dev, "write reg 0x%04X failed\n", reg);
	else
		dev_dbg(&client->dev, "wr 0x%04X = 0x%02X\n", reg, val);
}

static void lt6911uxc_i2c_rd(struct lt6911uxc *state, u16 reg, u8 *buf, u32 n)
{
	struct i2c_client *client = state->client;
	u8 reg_addr = reg & 0xFF;
	struct i2c_msg msgs[] = {
		{
			.addr  = client->addr,
			.flags = 0,
			.len   = 1,
			.buf   = &reg_addr,
		},
		{
			.addr  = client->addr,
			.flags = I2C_M_RD,
			.len   = n,
			.buf   = buf,
		},
	};

	lt6911uxc_reg_bank(state, (reg >> 8) & 0xFF);

	if (i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs)) !=
	    ARRAY_SIZE(msgs))
		dev_err(&client->dev, "read reg 0x%04X failed\n", reg);
}

static u8 lt6911uxc_i2c_rd8(struct lt6911uxc *state, u16 reg)
{
	u8 val = 0;

	lt6911uxc_i2c_rd(state, reg, &val, 1);
	dev_dbg(&state->client->dev, "rd 0x%04X = 0x%02X\n", reg, val);
	return val;
}

static u16 lt6911uxc_i2c_rd16(struct lt6911uxc *state, u16 reg)
{
	u8 raw[2];
	u16 val;

	lt6911uxc_i2c_rd(state, reg, raw, 2);
	/* High byte at lower address */
	val = ((u16)raw[0] << 8) | raw[1];
	dev_dbg(&state->client->dev, "rd16 0x%04X = 0x%04X\n", reg, val);
	return val;
}

/* --------------------------------------------------------------------------
 * Chip control helpers
 */

static void lt6911uxc_ext_control(struct lt6911uxc *state, bool enable)
{
	if (state->enable_i2c == enable)
		return;

	state->enable_i2c = enable;
	if (enable) {
		lt6911uxc_i2c_wr8(state, LT6911UXC_ENABLE_I2C, 0x01);
		lt6911uxc_i2c_wr8(state, LT6911UXC_DISABLE_WD, 0x00);
	} else {
		lt6911uxc_i2c_wr8(state, LT6911UXC_ENABLE_I2C, 0x00);
	}
}

static void lt6911uxc_csi_enable(struct lt6911uxc *state, bool enable)
{
	lt6911uxc_i2c_wr8(state, LT6911UXC_MIPI_TX_CTRL,
			  enable ? LT6911UXC_MIPI_TX_ENABLE :
				   LT6911UXC_MIPI_TX_DISABLE);
}

/* --------------------------------------------------------------------------
 * Audio helpers
 */

static int lt6911uxc_get_audio_sampling_rate(struct lt6911uxc *state)
{
	static const int rates[] = {
		32000, 44100, 48000, 88200, 96000, 176400, 192000
	};
	static const int eps = 1500;
	int audio_fs, i;

	audio_fs = lt6911uxc_i2c_rd8(state, LT6911UXC_AUDIO_SR) * 1000;

	for (i = 0; i < ARRAY_SIZE(rates); i++) {
		if (abs(rates[i] - audio_fs) < eps)
			return rates[i];
	}

	dev_warn(&state->client->dev, "unhandled audio sampling rate %d Hz\n",
		 audio_fs);
	return 0;
}

/* --------------------------------------------------------------------------
 * DV timings detection
 *
 * Reads resolution, blanking intervals, sync polarity and pixel clock
 * from the chip's status registers.
 */

static int lt6911uxc_detect_timings(struct lt6911uxc *state,
				    struct v4l2_dv_timings *timings,
				    u8 lanes)
{
	struct v4l2_bt_timings *bt = &timings->bt;
	u8 fm2, fm1, fm0, pol;
	int half_pixel_clk;
	u32 htot, vtot;

	memset(timings, 0, sizeof(*timings));

	timings->type  = V4L2_DV_BT_656_1120;
	bt->interlaced = V4L2_DV_PROGRESSIVE;

	/* Active area - registers hold half-pixel values for ≤4 lanes */
	bt->width  = lt6911uxc_i2c_rd16(state, LT6911UXC_H_ACTIVE_0P5);
	bt->height = lt6911uxc_i2c_rd16(state, LT6911UXC_V_ACTIVE);
	if (lanes <= 4)
		bt->width *= 2;

	/* Blanking */
	bt->hfrontporch = lt6911uxc_i2c_rd16(state, LT6911UXC_H_FP_0P5);
	bt->hbackporch  = lt6911uxc_i2c_rd16(state, LT6911UXC_H_BP_0P5);
	bt->hsync       = lt6911uxc_i2c_rd16(state, LT6911UXC_H_SW_0P5);
	if (lanes <= 4) {
		bt->hfrontporch *= 2;
		bt->hbackporch  *= 2;
		bt->hsync       *= 2;
	}
	bt->vfrontporch = lt6911uxc_i2c_rd8(state, LT6911UXC_V_FP);
	bt->vbackporch  = lt6911uxc_i2c_rd8(state, LT6911UXC_V_BP);
	bt->vsync       = lt6911uxc_i2c_rd8(state, LT6911UXC_V_SW);

	/* Sync polarity */
	pol = lt6911uxc_i2c_rd8(state, LT6911UXC_SYNC_POL);
	if (pol & LT6911UXC_MASK_HSYNC_POL)
		bt->polarities |= V4L2_DV_HSYNC_POS_POL;
	if (pol & LT6911UXC_MASK_VSYNC_POL)
		bt->polarities |= V4L2_DV_VSYNC_POS_POL;

	/* Pixel clock - frequency meter measures half the pixel clock */
	lt6911uxc_i2c_wr8(state, LT6911UXC_AD_HALF_PCLK, 0x21);
	usleep_range(10000, 10100);

	fm2 = lt6911uxc_i2c_rd8(state, LT6911UXC_FM1_FREQ_IN2) &
	      LT6911UXC_MASK_FMI_FREQ2;
	fm1 = lt6911uxc_i2c_rd8(state, LT6911UXC_FM1_FREQ_IN1);
	fm0 = lt6911uxc_i2c_rd8(state, LT6911UXC_FM1_FREQ_IN0);

	half_pixel_clk = (fm2 << 16) | (fm1 << 8) | fm0;
	if (lanes > 4)
		half_pixel_clk /= 2;

	bt->pixelclock = (u64)half_pixel_clk * 2 * 1000; /* kHz → Hz */

	htot = V4L2_DV_BT_FRAME_WIDTH(bt);
	vtot = V4L2_DV_BT_FRAME_HEIGHT(bt);

	v4l2_dbg(1, debug, &state->sd,
		 "detected: %ux%u pclk=%llu htot=%u vtot=%u\n",
		 bt->width, bt->height, bt->pixelclock, htot, vtot);

	/* Sanity check */
	if (bt->width < 640 || bt->height < 480 ||
	    htot <= bt->width || vtot <= bt->height) {
		memset(timings, 0, sizeof(*timings));
		return -ENOLCK;
	}

	return 0;
}

static void lt6911uxc_get_current_timings(struct lt6911uxc *state,
					  struct v4l2_dv_timings *timings)
{
	*timings = state->timings;
	if (!timings->bt.width || !timings->bt.height)
		*timings = lt6911uxc_default_timings;
}

static void lt6911uxc_fill_framefmt(const struct v4l2_dv_timings *timings,
				    u32 code,
				    struct v4l2_mbus_framefmt *fmt)
{
	const struct v4l2_bt_timings *bt = &timings->bt;

	fmt->code = MEDIA_BUS_FMT_UYVY8_2X8;
	fmt->width = bt->width ?: 1920;
	fmt->height = bt->height ?: 1080;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_DEFAULT;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static void lt6911uxc_get_frame_interval(const struct v4l2_dv_timings *timings,
					 struct v4l2_fract *interval)
{
	const struct v4l2_bt_timings *bt = &timings->bt;
	u32 htot = V4L2_DV_BT_FRAME_WIDTH(bt);
	u32 vtot = V4L2_DV_BT_FRAME_HEIGHT(bt);
	u64 fps;

	interval->numerator = 1;
	interval->denominator = 60;

	if (!bt->pixelclock || !htot || !vtot)
		return;

	fps = DIV_ROUND_CLOSEST_ULL(bt->pixelclock, (u64)htot * vtot);
	if (bt->interlaced)
		fps *= 2;

	if (fps)
		interval->denominator = (u32)fps;
}

static void lt6911uxc_log_query_state(struct lt6911uxc *state,
				      const char *op,
				      u32 id,
				      u32 value)
{
	struct v4l2_dv_timings timings;
	bool signal_present;
	bool streaming;
	bool csi_tx_enabled;

	mutex_lock(&state->lock);
	signal_present = state->signal_present;
	streaming = state->streaming;
	csi_tx_enabled = state->csi_tx_enabled;
	timings = state->detected_timings;
	mutex_unlock(&state->lock);

	dev_info_ratelimited(&state->client->dev,
		"%s: id=0x%08X value=0x%08X signal_present=%d streaming=%d csi_tx=%d detected=%ux%u pclk=%llu\n",
		op, id, value, signal_present, streaming, csi_tx_enabled,
		timings.bt.width, timings.bt.height,
		(unsigned long long)timings.bt.pixelclock);
}

/* --------------------------------------------------------------------------
 * HDMI / Audio interrupt handlers
 */

static void lt6911uxc_hdmi_int_handler(struct lt6911uxc *state, bool *handled)
{
	static const struct v4l2_event ev_src_change = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};
	struct device *dev = &state->client->dev;
	struct v4l2_dv_timings timings = {};
	u8 int_event, lanes;
	u8 fm2, fm1, fm0;
	int byte_clock;

	int_event = lt6911uxc_i2c_rd8(state, LT6911UXC_INT_HDMI);
	dev_dbg(dev, "HDMI int event = 0x%02X\n", int_event);

	switch (int_event) {
	case LT6911UXC_INT_HDMI_DISCONNECT:
	{
		bool had_signal;
		bool disable_tx;

		dev_info(dev, "HDMI signal disconnected\n");
		v4l2_ctrl_s_ctrl(state->num_mipi_lanes_ctrl, 0);
		v4l2_ctrl_s_ctrl(state->hdmi_source_present_ctrl, 0);

		mutex_lock(&state->lock);
		had_signal = state->signal_present;
		disable_tx = state->csi_tx_enabled;
		state->signal_present = false;
		state->csi_tx_enabled = false;
		memset(&state->timings, 0, sizeof(state->timings));
		memset(&state->detected_timings, 0,
		       sizeof(state->detected_timings));
		mutex_unlock(&state->lock);

		if (disable_tx)
			lt6911uxc_csi_enable(state, false);
		if (had_signal)
			v4l2_subdev_notify_event(&state->sd, &ev_src_change);
		if (handled)
			*handled = true;
		break;
	}

	case LT6911UXC_INT_HDMI_STABLE:
	{
		bool had_signal;
		bool timings_changed;
		bool update_tx;
		bool enable_tx;
		int err;

		dev_info(dev, "HDMI signal stable\n");

		lanes = lt6911uxc_i2c_rd8(state, LT6911UXC_MIPI_LANES);
		dev_dbg(dev, "MIPI lanes: %d\n", lanes);
		v4l2_ctrl_s_ctrl(state->num_mipi_lanes_ctrl, lanes);

		err = lt6911uxc_detect_timings(state, &timings, lanes);
		if (err) {
			dev_warn(dev, "failed to detect HDMI timings: %d\n", err);
			if (handled)
				*handled = true;
			break;
		}

		/* Read byte clock for diagnostics */
		lt6911uxc_i2c_wr8(state, LT6911UXC_AD_HALF_PCLK, 0x1B);
		usleep_range(10000, 10100);
		fm2 = lt6911uxc_i2c_rd8(state, LT6911UXC_FM1_FREQ_IN2) &
		      LT6911UXC_MASK_FMI_FREQ2;
		fm1 = lt6911uxc_i2c_rd8(state, LT6911UXC_FM1_FREQ_IN1);
		fm0 = lt6911uxc_i2c_rd8(state, LT6911UXC_FM1_FREQ_IN0);
		byte_clock = (fm2 << 16) | (fm1 << 8) | fm0;
		dev_dbg(dev, "byte clock %d kHz, MIPI clock %d kHz\n",
			byte_clock, byte_clock * 4);

		mutex_lock(&state->lock);
		had_signal = state->signal_present;
		timings_changed = !state->detected_timings.bt.width ||
			!v4l2_match_dv_timings(&timings, &state->detected_timings,
					       250000, false);
		state->signal_present = true;
		state->detected_timings = timings;
		state->timings = timings;
		enable_tx = state->streaming;
		update_tx = state->csi_tx_enabled != enable_tx;
		state->csi_tx_enabled = enable_tx;
		mutex_unlock(&state->lock);
		v4l2_ctrl_s_ctrl(state->hdmi_source_present_ctrl, 1);

		if (update_tx)
			lt6911uxc_csi_enable(state, enable_tx);
		if (!had_signal || timings_changed)
			v4l2_subdev_notify_event(&state->sd, &ev_src_change);

		if (handled)
			*handled = true;
		break;
	}

	default:
		dev_warn(dev, "unknown HDMI int event 0x%02X\n", int_event);
		break;
	}
}

static void lt6911uxc_audio_int_handler(struct lt6911uxc *state, bool *handled)
{
	struct device *dev = &state->client->dev;
	u8 int_event;
	int audio_fs = 0;

	int_event = lt6911uxc_i2c_rd8(state, LT6911UXC_INT_AUDIO);
	dev_dbg(dev, "Audio int event = 0x%02X\n", int_event);

	switch (int_event) {
	case LT6911UXC_INT_AUDIO_DISCONNECT:
		dev_info(dev, "Audio signal disconnected\n");
		audio_fs = 0;
		break;
	case LT6911UXC_INT_AUDIO_SR_HIGH:
	case LT6911UXC_INT_AUDIO_SR_LOW:
		if (state->signal_present)
			audio_fs = lt6911uxc_get_audio_sampling_rate(state);
		break;
	default:
		dev_warn(dev, "unknown audio int event 0x%02X\n", int_event);
		return;
	}

	v4l2_ctrl_s_ctrl(state->audio_present_ctrl, audio_fs != 0);
	v4l2_ctrl_s_ctrl(state->audio_sampling_rate_ctrl, audio_fs);

	if (handled)
		*handled = true;
}

static irqreturn_t lt6911uxc_irq_handler(int irq, void *dev_id)
{
	struct lt6911uxc *state = dev_id;
	bool handled = false;

	/*
	 * I2C reads happen inside the handlers — do NOT hold state->lock
	 * across them to avoid deadlocking get_fmt ioctls.  The handlers
	 * take the lock themselves only when updating in-memory state.
	 */
	lt6911uxc_ext_control(state, true);
	lt6911uxc_hdmi_int_handler(state, &handled);
	lt6911uxc_audio_int_handler(state, &handled);
	lt6911uxc_ext_control(state, false);

	return handled ? IRQ_HANDLED : IRQ_NONE;
}

/* --------------------------------------------------------------------------
 * V4L2 core ops
 */

static int lt6911uxc_s_power(struct v4l2_subdev *sd, int on)
{
	/* The LT6911UXC manages its own power state via the sleep/reset GPIOs
	 * during probe.  Return success so the ISI pipeline can start. */
	return 0;
}

static int lt6911uxc_log_status(struct v4l2_subdev *sd)
{
	struct lt6911uxc *state = to_lt6911uxc(sd);

	v4l2_info(sd, "--- LT6911UXC status ---\n");
	v4l2_info(sd, "Signal present: %s\n",
		  state->signal_present ? "yes" : "no");

	if (state->detected_timings.bt.width)
		v4l2_print_dv_timings(sd->name, "detected: ",
				      &state->detected_timings, true);
	else
		v4l2_info(sd, "no detected timings\n");

	v4l2_print_dv_timings(sd->name, "configured: ",
			      &state->timings, true);
	return 0;
}

static int lt6911uxc_subscribe_event(struct v4l2_subdev *sd,
				     struct v4l2_fh *fh,
				     struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subdev_subscribe(sd, fh, sub);
	case V4L2_EVENT_CTRL:
		return v4l2_ctrl_subdev_subscribe_event(sd, fh, sub);
	default:
		return -EINVAL;
	}
}

/* --------------------------------------------------------------------------
 * V4L2 video ops
 */

static int lt6911uxc_g_input_status(struct v4l2_subdev *sd, u32 *status)
{
	struct lt6911uxc *state = to_lt6911uxc(sd);

	*status = state->signal_present ? 0 : V4L2_IN_ST_NO_SIGNAL;
	// lt6911uxc_log_query_state(state, "VIDIOC_G_INPUT_STATUS", 0, *status);
	return 0;
}

static int lt6911uxc_s_dv_timings(struct v4l2_subdev *sd,
				  struct v4l2_dv_timings *timings)
{
	struct lt6911uxc *state = to_lt6911uxc(sd);

	if (!v4l2_valid_dv_timings(timings, &lt6911uxc_timings_cap,
				   NULL, NULL)) {
		v4l2_err(sd, "s_dv_timings: timings out of range\n");
		return -EINVAL;
	}

	v4l2_find_dv_timings_cap(timings, &lt6911uxc_timings_cap, 0,
				 NULL, NULL);

	if (v4l2_match_dv_timings(timings, &state->timings, 0, false))
		return 0; /* no change */

	memset(timings->bt.reserved, 0, sizeof(timings->bt.reserved));

	mutex_lock(&state->lock);
	state->timings = *timings;
	mutex_unlock(&state->lock);

	return 0;
}

static int lt6911uxc_g_dv_timings(struct v4l2_subdev *sd,
				  struct v4l2_dv_timings *timings)
{
	struct lt6911uxc *state = to_lt6911uxc(sd);

	mutex_lock(&state->lock);
	*timings = state->timings;
	mutex_unlock(&state->lock);
	return 0;
}

static int lt6911uxc_query_dv_timings(struct v4l2_subdev *sd,
				      struct v4l2_dv_timings *timings)
{
	struct lt6911uxc *state = to_lt6911uxc(sd);

	mutex_lock(&state->lock);

	if (!state->signal_present) {
		mutex_unlock(&state->lock);
		return -ENOLINK;
	}

	if (!v4l2_valid_dv_timings(&state->detected_timings,
				   &lt6911uxc_timings_cap, NULL, NULL)) {
		mutex_unlock(&state->lock);
		return -ERANGE;
	}

	*timings = state->detected_timings;
	mutex_unlock(&state->lock);
	return 0;
}

static int lt6911uxc_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct lt6911uxc *state = to_lt6911uxc(sd);
	bool enable_tx;
	bool update_tx;

	enable = !!enable;

	mutex_lock(&state->lock);
	state->streaming = enable;
	enable_tx = state->signal_present && state->streaming;
	update_tx = state->csi_tx_enabled != enable_tx;
	state->csi_tx_enabled = enable_tx;
	mutex_unlock(&state->lock);

	if (update_tx) {
		lt6911uxc_ext_control(state, true);
		lt6911uxc_csi_enable(state, enable_tx);
		lt6911uxc_ext_control(state, false);
	}

	v4l2_dbg(1, debug, sd, "s_stream: %d\n", enable);
	return 0;
}

static int lt6911uxc_g_frame_interval(struct v4l2_subdev *sd,
					      struct v4l2_subdev_frame_interval *fi)
{
	struct lt6911uxc *state = to_lt6911uxc(sd);
	struct v4l2_dv_timings timings;

	if (fi->pad != 0)
		return -EINVAL;

	mutex_lock(&state->lock);
	lt6911uxc_get_current_timings(state, &timings);
	mutex_unlock(&state->lock);

	lt6911uxc_get_frame_interval(&timings, &fi->interval);
	return 0;
}

static int lt6911uxc_s_frame_interval(struct v4l2_subdev *sd,
					      struct v4l2_subdev_frame_interval *fi)
{
	if (fi->pad != 0)
		return -EINVAL;

	/* The HDMI source dictates the frame interval. */
	return lt6911uxc_g_frame_interval(sd, fi);
}

/* --------------------------------------------------------------------------
 * V4L2 pad ops
 */

static int lt6911uxc_get_fmt(struct v4l2_subdev *sd,
			     struct v4l2_subdev_pad_config *cfg,
			     struct v4l2_subdev_format *format)
{
	struct lt6911uxc *state = to_lt6911uxc(sd);
	struct v4l2_dv_timings timings;
	u32 code;

	if (format->pad != 0)
		return -EINVAL;
	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		if (!cfg)
			return -EINVAL;

		format->format = *v4l2_subdev_get_try_format(sd, cfg,
							   format->pad);
		return 0;
	}

	mutex_lock(&state->lock);
	code = state->mbus_fmt_code;
	lt6911uxc_get_current_timings(state, &timings);
	mutex_unlock(&state->lock);

	lt6911uxc_fill_framefmt(&timings, code, &format->format);

	return 0;
}

static int lt6911uxc_set_fmt(struct v4l2_subdev *sd,
			     struct v4l2_subdev_pad_config *cfg,
			     struct v4l2_subdev_format *format)
{
	struct lt6911uxc *state = to_lt6911uxc(sd);
	struct v4l2_dv_timings timings;
	u32 code = format->format.code;
	unsigned int i;

	if (format->pad != 0)
		return -EINVAL;

	/*
	 * When the ISI pipeline calls set_fmt(ACTIVE) just before STREAMON,
	 * re-read the chip's timing registers so that any stale/wrong value
	 * cached by an earlier partial-lock poll (e.g. height=1139 instead
	 * of 2160) is corrected before the ISI upscale guard runs.
	 * Only update the cache if the fresh read succeeds; keep the old
	 * value if the chip is still transitioning.
	 */
	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE && state->signal_present) {
		struct v4l2_dv_timings fresh = {};
		u8 lanes;
		int ret;

		lt6911uxc_ext_control(state, true);
		lanes = lt6911uxc_i2c_rd8(state, LT6911UXC_MIPI_LANES);
		ret = lt6911uxc_detect_timings(state, &fresh, lanes);
		lt6911uxc_ext_control(state, false);

		if (ret == 0) {
			mutex_lock(&state->lock);
			state->detected_timings = fresh;
			state->timings = fresh;
			mutex_unlock(&state->lock);
			dev_dbg(&state->client->dev,
				"set_fmt: refreshed timings %ux%u pclk=%llu\n",
				fresh.bt.width, fresh.bt.height,
				fresh.bt.pixelclock);
		}
	}

	/* Validate requested mbus code */
	for (i = 0; i < ARRAY_SIZE(lt6911uxc_mbus_formats); i++) {
		if (lt6911uxc_mbus_formats[i] == code)
			break;
	}
	if (i == ARRAY_SIZE(lt6911uxc_mbus_formats))
		code = lt6911uxc_mbus_formats[0];

	mutex_lock(&state->lock);
	lt6911uxc_get_current_timings(state, &timings);
	if (format->which != V4L2_SUBDEV_FORMAT_TRY)
		state->mbus_fmt_code = code;
	mutex_unlock(&state->lock);

	lt6911uxc_fill_framefmt(&timings, code, &format->format);

	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		if (!cfg)
			return -EINVAL;
		*v4l2_subdev_get_try_format(sd, cfg, format->pad) =
			format->format;
		return 0;
	}

	return 0;
}

static int lt6911uxc_enum_mbus_code(struct v4l2_subdev *sd,
				    struct v4l2_subdev_pad_config *cfg,
				    struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(lt6911uxc_mbus_formats))
		return -EINVAL;

	code->code = lt6911uxc_mbus_formats[code->index];
	return 0;
}

static int lt6911uxc_enum_frame_size(struct v4l2_subdev *sd,
				     struct v4l2_subdev_pad_config *cfg,
				     struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->pad != 0 || fse->index != 0)
		return -EINVAL;
	if (fse->code != lt6911uxc_mbus_formats[0])
		return -EINVAL;

	fse->min_width = lt6911uxc_timings_cap.bt.min_width;
	fse->max_width = lt6911uxc_timings_cap.bt.max_width;
	fse->min_height = lt6911uxc_timings_cap.bt.min_height;
	fse->max_height = lt6911uxc_timings_cap.bt.max_height;

	return 0;
}

static int lt6911uxc_enum_frame_interval(
		struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_frame_interval_enum *fie)
{
	struct lt6911uxc *state = to_lt6911uxc(sd);
	struct v4l2_dv_timings timings;

	if (fie->pad != 0 || fie->index != 0)
		return -EINVAL;
	if (fie->code != lt6911uxc_mbus_formats[0])
		return -EINVAL;
	if (fie->width < lt6911uxc_timings_cap.bt.min_width ||
	    fie->width > lt6911uxc_timings_cap.bt.max_width ||
	    fie->height < lt6911uxc_timings_cap.bt.min_height ||
	    fie->height > lt6911uxc_timings_cap.bt.max_height)
		return -EINVAL;

	mutex_lock(&state->lock);
	lt6911uxc_get_current_timings(state, &timings);
	mutex_unlock(&state->lock);

	lt6911uxc_get_frame_interval(&timings, &fie->interval);
	return 0;
}

static int lt6911uxc_dv_timings_cap(struct v4l2_subdev *sd,
				    struct v4l2_dv_timings_cap *cap)
{
	if (cap->pad != 0)
		return -EINVAL;

	*cap = lt6911uxc_timings_cap;
	return 0;
}

static int lt6911uxc_enum_dv_timings(struct v4l2_subdev *sd,
				     struct v4l2_enum_dv_timings *timings)
{
	if (timings->pad != 0)
		return -EINVAL;

	return v4l2_enum_dv_timings_cap(timings, &lt6911uxc_timings_cap,
					NULL, NULL);
}

/* --------------------------------------------------------------------------
 * V4L2 subdev internal ops
 */

static int lt6911uxc_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct lt6911uxc *state = to_lt6911uxc(sd);
	struct v4l2_dv_timings timings;
	struct v4l2_mbus_framefmt *fmt =
		v4l2_subdev_get_try_format(sd, fh->pad, 0);

	mutex_lock(&state->lock);
	lt6911uxc_get_current_timings(state, &timings);
	lt6911uxc_fill_framefmt(&timings, state->mbus_fmt_code, fmt);
	mutex_unlock(&state->lock);

	return 0;
}

/* --------------------------------------------------------------------------
 * Ops tables
 */

static const struct v4l2_subdev_internal_ops lt6911uxc_internal_ops = {
	.open = lt6911uxc_open,
};

static const struct v4l2_subdev_core_ops lt6911uxc_core_ops = {
	.log_status        = lt6911uxc_log_status,
	.s_power           = lt6911uxc_s_power,
	.subscribe_event   = lt6911uxc_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops lt6911uxc_video_ops = {
	.g_input_status   = lt6911uxc_g_input_status,
	.s_dv_timings     = lt6911uxc_s_dv_timings,
	.g_dv_timings     = lt6911uxc_g_dv_timings,
	.query_dv_timings = lt6911uxc_query_dv_timings,
	.s_stream         = lt6911uxc_s_stream,
	.g_frame_interval = lt6911uxc_g_frame_interval,
	.s_frame_interval = lt6911uxc_s_frame_interval,
};

static const struct v4l2_subdev_pad_ops lt6911uxc_pad_ops = {
	.get_fmt          = lt6911uxc_get_fmt,
	.set_fmt          = lt6911uxc_set_fmt,
	.enum_mbus_code   = lt6911uxc_enum_mbus_code,
	.enum_frame_size  = lt6911uxc_enum_frame_size,
	.enum_frame_interval = lt6911uxc_enum_frame_interval,
	.dv_timings_cap   = lt6911uxc_dv_timings_cap,
	.enum_dv_timings  = lt6911uxc_enum_dv_timings,
};

static const struct v4l2_subdev_ops lt6911uxc_ops = {
	.core  = &lt6911uxc_core_ops,
	.video = &lt6911uxc_video_ops,
	.pad   = &lt6911uxc_pad_ops,
};

static int lt6911uxc_link_setup(struct media_entity *entity,
				const struct media_pad *local,
				const struct media_pad *remote, u32 flags)
{
	/* mxc_md_create_links calls this via media_entity_call().
	 * Without a link_setup handler media_entity_call returns -ENOIOCTLCMD
	 * (-515) which causes the entire mxc-md probe to fail. */
	return 0;
}

static const struct media_entity_operations lt6911uxc_media_ops = {
	.link_setup    = lt6911uxc_link_setup,
	.link_validate = v4l2_subdev_link_validate,
};

/* --------------------------------------------------------------------------
 * Custom V4L2 control configs
 */

static int lt6911uxc_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct lt6911uxc *state =
		container_of(ctrl->handler, struct lt6911uxc, ctrl_handler);

	switch (ctrl->id) {
	case LT6911UXC_CID_HDMI_SOURCE_PRESENT:
		ctrl->val = state->signal_present ? 1 : 0;
		// lt6911uxc_log_query_state(state, "VIDIOC_G_CTRL", ctrl->id, ctrl->val);
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops lt6911uxc_ctrl_ops = {
	.g_volatile_ctrl = lt6911uxc_g_volatile_ctrl,
};

static const struct v4l2_ctrl_config lt6911uxc_ctrl_audio_sampling_rate = {
	.id    = LT6911UXC_CID_AUDIO_SAMPLING_RATE,
	.name  = "Audio Sampling Rate",
	.type  = V4L2_CTRL_TYPE_INTEGER,
	.min   = 0,
	.max   = 192000,
	.step  = 1,
	.def   = 0,
	.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,
};

static const struct v4l2_ctrl_config lt6911uxc_ctrl_audio_present = {
	.id    = LT6911UXC_CID_AUDIO_PRESENT,
	.name  = "Audio Present",
	.type  = V4L2_CTRL_TYPE_BOOLEAN,
	.min   = 0,
	.max   = 1,
	.step  = 1,
	.def   = 0,
	.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,
};

static const struct v4l2_ctrl_config lt6911uxc_ctrl_num_mipi_lanes = {
	.id    = LT6911UXC_CID_NUM_MIPI_LANES,
	.name  = "Number of MIPI Lanes Used",
	.type  = V4L2_CTRL_TYPE_INTEGER,
	.min   = 0,
	.max   = 8,
	.step  = 4,
	.def   = 0,
	.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,
};

static const struct v4l2_ctrl_config lt6911uxc_ctrl_hdmi_source_present = {
	.ops   = &lt6911uxc_ctrl_ops,
	.id    = LT6911UXC_CID_HDMI_SOURCE_PRESENT,
	.name  = "HDMI Source Present",
	.type  = V4L2_CTRL_TYPE_BOOLEAN,
	.min   = 0,
	.max   = 1,
	.step  = 1,
	.def   = 0,
	.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,
};

/* --------------------------------------------------------------------------
 * Polling work — detects HDMI connect/disconnect and timing changes
 * without relying on IRQ delivery
 */

static void lt6911uxc_poll_work(struct work_struct *work)
{
	static const struct v4l2_event ev_src_change = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};
	struct lt6911uxc *state = container_of(to_delayed_work(work),
					       struct lt6911uxc, poll_work);
	struct device *dev = &state->client->dev;
	u16 h_active;
	u8 int_status;
	bool hdmi_locked;
	bool signal_present_snap;
	bool do_notify = false;

	/*
	 * Perform all I2C transfers OUTSIDE the mutex.  The mutex only
	 * protects in-memory state (signal_present, timings).  Holding it
	 * across slow I2C calls would block get_fmt/set_fmt ioctls and cause
	 * media-ctl / v4l2-ctl to deadlock.
	 */
	lt6911uxc_ext_control(state, true);
	h_active   = lt6911uxc_i2c_rd16(state, LT6911UXC_H_ACTIVE_0P5);
	int_status = lt6911uxc_i2c_rd8(state, LT6911UXC_INT_HDMI);
	lt6911uxc_ext_control(state, false);

	/*
	 * Treat HDMI as locked only when:
	 *   a) the chip explicitly reports STABLE (0x55), OR
	 *   b) h_active is non-zero AND the chip is NOT reporting a
	 *      DISCONNECT event (0x88 = actively transitioning/unstable).
	 * Without the second guard the polling path would accept transitional
	 * register garbage from a chip that is still negotiating the link,
	 * resulting in wrong timings (e.g. 3840x1139 instead of 3840x2160)
	 * being locked into state->timings.
	 */
	hdmi_locked = (int_status == LT6911UXC_INT_HDMI_STABLE) ||
		      ((h_active > 0) &&
		       (int_status != LT6911UXC_INT_HDMI_DISCONNECT));

	/* Snapshot signal_present under lock for the comparison below */
	mutex_lock(&state->lock);
	signal_present_snap = state->signal_present;
	mutex_unlock(&state->lock);

	if (hdmi_locked && !signal_present_snap) {
		struct v4l2_dv_timings timings = {};
		bool enable_tx;
		bool update_tx;
		u8 lanes;
		int err;

		dev_info(dev, "polling: HDMI detected (h_active_0p5=%u int=0x%02X)\n",
			 h_active, int_status);

		/* All I2C for timing detection outside the mutex */
		lt6911uxc_ext_control(state, true);
		lanes = lt6911uxc_i2c_rd8(state, LT6911UXC_MIPI_LANES);

		err = lt6911uxc_detect_timings(state, &timings, lanes);
		if (err == 0) {
			dev_info(dev, "detected: %ux%u pclk=%llu lanes=%u\n",
				 timings.bt.width, timings.bt.height,
				 timings.bt.pixelclock, lanes);
		} else {
			dev_warn(dev, "detect_timings failed: %d (lanes=%u)\n",
				 err, lanes);
		}

		/* Commit detected timings under lock.
		 * Set state->timings directly here — do NOT call
		 * lt6911uxc_s_dv_timings() since that also acquires
		 * state->lock and would self-deadlock. */
		mutex_lock(&state->lock);
		if (err == 0) {
			state->signal_present = true;
			state->detected_timings = timings;
			state->timings = timings;
			enable_tx = state->streaming;
			update_tx = state->csi_tx_enabled != enable_tx;
			state->csi_tx_enabled = enable_tx;
		} else {
			enable_tx = false;
			update_tx = false;
		}
		mutex_unlock(&state->lock);

		if (err == 0)
			v4l2_ctrl_s_ctrl(state->num_mipi_lanes_ctrl, lanes);
		if (err == 0)
			v4l2_ctrl_s_ctrl(state->hdmi_source_present_ctrl, 1);
		if (update_tx) {
			lt6911uxc_csi_enable(state, enable_tx);
			dev_info(dev, "MIPI TX %s\n",
				 enable_tx ? "enabled" : "disabled");
		}
		lt6911uxc_ext_control(state, false);
		do_notify = (err == 0);

	} else if (hdmi_locked && signal_present_snap) {
		struct v4l2_dv_timings timings = {};
		bool timings_changed;
		bool enable_tx;
		bool update_tx;
		u8 lanes;
		int err;

		lt6911uxc_ext_control(state, true);
		lanes = lt6911uxc_i2c_rd8(state, LT6911UXC_MIPI_LANES);
		err = lt6911uxc_detect_timings(state, &timings, lanes);

		mutex_lock(&state->lock);
		timings_changed = !err &&
			(!state->detected_timings.bt.width ||
			 !v4l2_match_dv_timings(&timings,
					       &state->detected_timings,
					       250000, false));
		if (timings_changed) {
			state->detected_timings = timings;
			state->timings = timings;
			enable_tx = state->streaming;
			update_tx = state->csi_tx_enabled != enable_tx;
			state->csi_tx_enabled = enable_tx;
		} else {
			enable_tx = state->csi_tx_enabled;
			update_tx = false;
		}
		mutex_unlock(&state->lock);

		if (!err)
			v4l2_ctrl_s_ctrl(state->num_mipi_lanes_ctrl, lanes);
		if (!err)
			v4l2_ctrl_s_ctrl(state->hdmi_source_present_ctrl, 1);
		if (update_tx)
			lt6911uxc_csi_enable(state, enable_tx);
		lt6911uxc_ext_control(state, false);

		if (err) {
			dev_dbg(dev, "polling: detect_timings failed while locked: %d\n",
				err);
		} else if (timings_changed) {
			dev_info(dev,
				 "polling: HDMI timings changed to %ux%u pclk=%llu lanes=%u\n",
				 timings.bt.width, timings.bt.height,
				 timings.bt.pixelclock, lanes);
			do_notify = true;
		}

	} else if (!hdmi_locked && signal_present_snap) {
		bool disable_tx;

		dev_info(dev, "polling: HDMI disconnected\n");

		mutex_lock(&state->lock);
		disable_tx = state->csi_tx_enabled;
		state->signal_present = false;
		state->csi_tx_enabled = false;
		memset(&state->timings, 0, sizeof(state->timings));
		memset(&state->detected_timings, 0,
		       sizeof(state->detected_timings));
		mutex_unlock(&state->lock);

		/* I2C to disable MIPI TX outside the mutex */
		lt6911uxc_ext_control(state, true);
		if (disable_tx)
			lt6911uxc_csi_enable(state, false);
		lt6911uxc_ext_control(state, false);

		v4l2_ctrl_s_ctrl(state->num_mipi_lanes_ctrl, 0);
		v4l2_ctrl_s_ctrl(state->hdmi_source_present_ctrl, 0);
		do_notify = true;
	}

	if (do_notify)
		v4l2_subdev_notify_event(&state->sd, &ev_src_change);

	/* Reschedule unconditionally — polls every 2 s until driver removed */
	schedule_delayed_work(&state->poll_work, msecs_to_jiffies(2000));
}

/* --------------------------------------------------------------------------
 * Probe / Remove
 */

static int lt6911uxc_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct lt6911uxc *state;
	struct v4l2_subdev *sd;
	int err;

	dev_info(dev, "probing LT6911UXC at 0x%02X\n", client->addr);

	state = devm_kzalloc(dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	state->client = client;
	mutex_init(&state->lock);

	/* ---- Sleep GPIO (active-high, HIGH=sleep) ---- */
	/* Deassert SLEEP *before* reset so the chip can power up. */
	state->sleep_gpio = devm_gpiod_get_optional(dev, "sleep",
						    GPIOD_OUT_LOW);
	if (IS_ERR(state->sleep_gpio)) {
		err = PTR_ERR(state->sleep_gpio);
		if (err == -EPROBE_DEFER)
			return err;
		dev_warn(dev, "failed to get sleep GPIO: %d\n", err);
		state->sleep_gpio = NULL;
	}
	if (state->sleep_gpio) {
		/* Logical 0 = chip awake (GPIOD_OUT_LOW handles polarity) */
		gpiod_set_value_cansleep(state->sleep_gpio, 0);
		usleep_range(5000, 6000); /* t_wake: 5 ms */
		dev_info(dev, "LT6911UXC sleep deasserted\n");
	}

	/* ---- Reset GPIO (optional, active-low) ---- */
	state->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_LOW);
	if (IS_ERR(state->reset_gpio)) {
		err = PTR_ERR(state->reset_gpio);
		if (err == -EPROBE_DEFER)
			return err;
		dev_warn(dev, "failed to get reset GPIO: %d\n", err);
		state->reset_gpio = NULL;
	}

	/* Hardware reset */
	if (state->reset_gpio) {
		gpiod_set_value_cansleep(state->reset_gpio, 1);
		usleep_range(1000, 2000);
		gpiod_set_value_cansleep(state->reset_gpio, 0);
		msleep(100); /* LT6911UXC MCU needs ~100ms to boot after reset */
		dev_info(dev, "LT6911UXC reset complete\n");
	}

	/* Quick I2C sanity check — read INT_HDMI with ext_control */
	{
		u8 int_val;
		state->bank = 0xFF; /* force first bank switch */
		lt6911uxc_ext_control(state, true);
		int_val = lt6911uxc_i2c_rd8(state, LT6911UXC_INT_HDMI);
		lt6911uxc_ext_control(state, false);
		dev_info(dev, "I2C check: INT_HDMI=0x%02X (%s)\n", int_val,
			 int_val == 0x88 ? "HDMI disconnected" :
			 int_val == 0x55 ? "HDMI stable" : "unknown");
	}

	/* ---- V4L2 subdev init ---- */
	sd = &state->sd;
	v4l2_i2c_subdev_init(sd, client, &lt6911uxc_ops);

	sd->flags         |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
	sd->internal_ops   = &lt6911uxc_internal_ops;
	sd->entity.ops     = &lt6911uxc_media_ops;
	sd->entity.function = MEDIA_ENT_F_VID_IF_BRIDGE; /* HDMI-to-CSI bridge */

	/* ---- Media pad (source - outputs MIPI CSI data) ---- */
	state->pad.flags = MEDIA_PAD_FL_SOURCE;
	err = media_entity_pads_init(&sd->entity, 1, &state->pad);
	if (err) {
		dev_err(dev, "media_entity_pads_init failed: %d\n", err);
		goto err_mutex;
	}

	/* ---- Controls ---- */
	v4l2_ctrl_handler_init(&state->ctrl_handler, LT6911UXC_NUM_CTRLS);

	state->audio_sampling_rate_ctrl =
		v4l2_ctrl_new_custom(&state->ctrl_handler,
				     &lt6911uxc_ctrl_audio_sampling_rate, NULL);
	state->audio_present_ctrl =
		v4l2_ctrl_new_custom(&state->ctrl_handler,
				     &lt6911uxc_ctrl_audio_present, NULL);
	state->num_mipi_lanes_ctrl =
		v4l2_ctrl_new_custom(&state->ctrl_handler,
				     &lt6911uxc_ctrl_num_mipi_lanes, NULL);
	state->hdmi_source_present_ctrl =
		v4l2_ctrl_new_custom(&state->ctrl_handler,
				     &lt6911uxc_ctrl_hdmi_source_present, NULL);

	if (state->ctrl_handler.error) {
		err = state->ctrl_handler.error;
		dev_err(dev, "control handler error: %d\n", err);
		goto err_entity;
	}

	sd->ctrl_handler = &state->ctrl_handler;

	err = v4l2_ctrl_handler_setup(&state->ctrl_handler);
	if (err) {
		dev_err(dev, "v4l2_ctrl_handler_setup failed: %d\n", err);
		goto err_ctrl_handler;
	}

	/* ---- Initial format / timings ---- */
	state->mbus_fmt_code = MEDIA_BUS_FMT_UYVY8_2X8;
	lt6911uxc_s_dv_timings(sd, &lt6911uxc_default_timings);
	lt6911uxc_ext_control(state, true);
	lt6911uxc_csi_enable(state, false);
	lt6911uxc_ext_control(state, false);

	/* ---- IRQ ---- */
	if (client->irq) {
		err = devm_request_threaded_irq(dev, client->irq,
						NULL,
						lt6911uxc_irq_handler,
						IRQF_ONESHOT,
						sd->name, state);
		if (err) {
			dev_err(dev, "failed to request IRQ %d: %d\n",
				client->irq, err);
			goto err_ctrl_handler;
		}
		dev_info(dev, "registered IRQ %d\n", client->irq);
	} else {
		dev_info(dev, "no IRQ defined - using polling mode\n");
	}

	/* ---- Schedule polling work ----
	 * The chip needs ~2-3 s after reset to lock onto HDMI.  A delayed
	 * work polls the live H_ACTIVE register every 2 s so that boot-time
	 * HDMI connections are detected without relying on the interrupt GPIO.
	 * The IRQ handler (if wired correctly) will fire faster on transitions.
	 */
	INIT_DELAYED_WORK(&state->poll_work, lt6911uxc_poll_work);
	schedule_delayed_work(&state->poll_work, msecs_to_jiffies(3000));

	/* ---- Register subdev ---- */
	err = v4l2_async_register_subdev_sensor_common(sd);
	if (err) {
		dev_err(dev, "v4l2_async_register_subdev failed: %d\n", err);
		goto err_ctrl_handler;
	}

	dev_info(dev, "LT6911UXC probe OK (irq=%d)\n", client->irq);
	return 0;

err_ctrl_handler:
	v4l2_ctrl_handler_free(&state->ctrl_handler);
err_entity:
	media_entity_cleanup(&sd->entity);
err_mutex:
	mutex_destroy(&state->lock);
	return err;
}

static int lt6911uxc_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct lt6911uxc *state = to_lt6911uxc(sd);

	dev_info(&client->dev, "removing LT6911UXC\n");

	cancel_delayed_work_sync(&state->poll_work);
	v4l2_async_unregister_subdev(sd);
	v4l2_ctrl_handler_free(&state->ctrl_handler);
	media_entity_cleanup(&sd->entity);
	mutex_destroy(&state->lock);

	return 0;
}

/* --------------------------------------------------------------------------
 * Driver registration
 */

static const struct of_device_id lt6911uxc_of_match[] = {
	{ .compatible = "lontium,lt6911uxc" },
	{ }
};
MODULE_DEVICE_TABLE(of, lt6911uxc_of_match);

static const struct i2c_device_id lt6911uxc_id[] = {
	{ "lt6911uxc", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, lt6911uxc_id);

static struct i2c_driver lt6911uxc_driver = {
	.driver = {
		.name          = "lt6911uxc",
		.of_match_table = of_match_ptr(lt6911uxc_of_match),
	},
	.probe    = lt6911uxc_probe,
	.remove   = lt6911uxc_remove,
	.id_table = lt6911uxc_id,
};

module_i2c_driver(lt6911uxc_driver);

MODULE_DESCRIPTION("Lontium LT6911UXC HDMI-to-MIPI-CSI2 bridge driver");
MODULE_AUTHOR("Adapted for i.MX8 from ZHAW InES-HPMM lt6911uxc_zhaw.c ");
MODULE_LICENSE("GPL v2");
