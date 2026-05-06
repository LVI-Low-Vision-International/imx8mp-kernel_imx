// SPDX-License-Identifier: GPL-2.0
// TI TPS55287 buck-boost converter (I2C) – DT-configured bring-up
//
// Programs VOUT, current limit, slew-rate, FPWM/hiccup, CDC, feedback mode.
// Based on datasheet SLVSGW6 (Aug 2024).

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/units.h>
#include <linux/atomic.h>

atomic_t tps55287_ready = ATOMIC_INIT(0);
EXPORT_SYMBOL(tps55287_ready);

#define DRV_NAME "tps55287"

/* Registers (datasheet "Table 6-3. Device Registers") */
#define TPS55287_REG_REF_LSB 0x00 /* write LSB first */
#define TPS55287_REG_REF_MSB 0x01
#define TPS55287_REG_IOUT_LIMIT 0x02
#define TPS55287_REG_VOUT_SR 0x03
#define TPS55287_REG_VOUT_FS 0x04
#define TPS55287_REG_CDC 0x05
#define TPS55287_REG_MODE 0x06
#define TPS55287_REG_STATUS 0x07

/* MODE register (Table 6-10) */
#define TPS55287_MODE_OE BIT(7)
#define TPS55287_MODE_FSWDBL BIT(6)
#define TPS55287_MODE_HICCUP BIT(5)
#define TPS55287_MODE_DISCHG BIT(4)
#define TPS55287_MODE_FORCE_DISCHG BIT(3)
#define TPS55287_MODE_FPWM BIT(1)

/* VOUT_SR (Table 6-6) */
#define TPS55287_SR_MASK GENMASK(1, 0) /* 00=1.25, 01=2.5 (def), 10=5, 11=10 mV/us */
#define TPS55287_OCP_DELAY_MASK GENMASK(5, 4) /* 00=128us (def), 01=~3ms, 10=~6ms, 11=~12ms */

/* VOUT_FS (Table 6-7) */
#define TPS55287_VOUT_FS_FB_INT 0 /* FB/INT pin is fault output, internal feedback used */
#define TPS55287_VOUT_FS_FB_EXT BIT(7) /* use external divider into FB/INT; INT feedback disabled */
#define TPS55287_VOUT_FS_INTFB_MASK GENMASK(1, 0) /* 00:0.2256, 01:0.1128, 10:0.0752, 11:0.0564 (def) */

/* CDC (Table 6-9) */
#define TPS55287_CDC_SC_MASK BIT(7)
#define TPS55287_CDC_OCP_MASK BIT(6)
#define TPS55287_CDC_OVP_MASK BIT(5)
#define TPS55287_CDC_OPTION_EXT BIT(3) /* 0 internal (default), 1 external via CDC pin */
#define TPS55287_CDC_COMP_MASK GENMASK(2, 0) /* 0..7 -> 0..0.7V rise at 50mV sense */

/* IOUT_LIMIT (Table 6-5) */
#define TPS55287_ILIM_EN BIT(7)
#define TPS55287_ILIM_SET_MASK GENMASK(6, 0) /* LSB = 0.5 mV between ISP-ISN */

/* REF register details (Tables 6-4 & 6-8)
 * REF_LSB (0x00): 8 LSBs of VREF
 * REF_MSB (0x01): VREF[10:8] at bits [2:0], rest reserved
 * One LSB (in REF_LSB) = 0.5645 mV internal reference step.
 */

/* Device-tree property names */
#define DT_VOUT_UV "ti,vout-microvolt" /* desired output when using internal feedback */
#define DT_FB_INTERNAL "ti,internal-feedback" /* bool: true=internal (default), false=external */
#define DT_INTFB_RATIO "ti,intfb-ratio" /* 0,1,2,3 -> 0.2256,0.1128,0.0752,0.0564 (default=3) */
#define DT_ILIM_MA "ti,ilim-milliamp" /* desired output current limit in mA */
#define DT_RSENSE_UOHM "ti,rsense-micro-ohms" /* shunt in micro-ohms; needed if ilim specified */
#define DT_FPWM "ti,force-pwm" /* bool: FPWM at light load */
#define DT_HICCUP "ti,hiccup-enable" /* bool */
#define DT_DISCHG_ON_SHDN "ti,dischg-on-shutdown" /* bool: DISCHG bit */
#define DT_FORCE_DISCHG "ti,force-dischg" /* bool: Force_DISCHG, keep short (<10ms) in sw use */
#define DT_SLEW_MVUS "ti,slew-rate-mv-per-us" /* 1.25, 2.5 (default), 5, 10 */
#define DT_OCP_DELAY_MS "ti,ocp-delay-ms" /* 0.128 (default), 3, 6, 12 */
#define DT_CDC_OPTION_EXT "ti,cdc-external" /* bool: external CDC via resistor on CDC pin */
#define DT_CDC_STEP_100MV "ti,cdc-rise-100mv-steps" /* 0..7 -> 0..0.7V rise at 50mV sense */
#define DT_FSWDBL "ti,double-fsw-in-buckboost" /* bool */

/* Driver data */
struct tps55287 {
	struct i2c_client *client;
	struct regmap *regmap;

	/* cached config (optional for debugging) */
	uint32_t vout_uv;
	bool fb_internal;
	uint32_t intfb_ratio_sel; /* 0..3 */
};

static const struct regmap_config tps55287_regmap_cfg = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = TPS55287_REG_STATUS,
	.cache_type = REGCACHE_RBTREE,
};

/* Helpers -------------------------------------------------------------- */

static int tps55287_write_ref(struct tps55287 *tps, uint16_t vref_code)
{
	/* Write LSB first, then MSB (datasheet 6.6.1) */
	int ret = regmap_write(tps->regmap, TPS55287_REG_REF_LSB, vref_code & 0xFF);
	if (ret)
		return ret;
	return regmap_write(tps->regmap, TPS55287_REG_REF_MSB, (vref_code >> 8) & 0x07);
}

/* Compute REF code for desired VOUT when using internal feedback.
 * Vout = Vref / K, where K depends on INTFB bits:
 *   sel: 0->0.2256, 1->0.1128, 2->0.0752, 3->0.0564 (default)
 * Vref_LSB = 0.5645 mV.
 */
static int tps55287_calc_ref_code(uint32_t vout_uv, uint32_t intfb_sel, uint16_t *code_out)
{
	static const /* K * 1e6 */ uint32_t K_ppm[4] = { 225600, 112800, 75200, 56400 };
	uint64_t vref_uuv; /* micro-microvolt */
	uint32_t K = K_ppm[intfb_sel];

	/* Vref = Vout * K  (both in microvolts), but K is in ppm (1e6) of Vref/Vout */
	/* vref (uV) = vout_uv * K / 1e6 */
	vref_uuv = (uint64_t)vout_uv * K; /* uV * ppm */
	do_div(vref_uuv, 1000000); /* -> vref in uV */

	/* Convert vref_uV to code: each LSB = 0.5645 mV = 564.5 uV */
	/* code = round(vref_uV / 564.5) */
	{
		uint64_t num = vref_uuv * 10; /* multiply to keep 0.1 uV granularity */
		uint32_t denom = 5645; /* 564.5 uV -> 5645 * 0.1 uV units */
		uint32_t code;

		do_div(num, denom);
		code = (uint32_t)num;
		if (code > 0x7FF) /* 11 bits (max ~1.129V) */
			code = 0x7FF;
		*code_out = (uint16_t)code;
	}
	return 0;
}

static int tps55287_apply_dt(struct tps55287 *tps)
{
	struct device *dev = &tps->client->dev;
	uint32_t tmp, reg, vout_uv = 0;
	bool fb_internal = true;
	uint32_t intfb_sel = 3; /* default 0.0564 -> 10 mV step */
	int ret;

	/* Feedback selection */
	of_property_read_u32(dev->of_node, DT_INTFB_RATIO, &intfb_sel);
	if (intfb_sel > 3)
		intfb_sel = 3;
	fb_internal = of_property_read_bool(dev->of_node, DT_FB_INTERNAL) || true;

	/* VOUT setting only applies for internal feedback mode */
	if (fb_internal) {
		if (of_property_read_u32(dev->of_node, DT_VOUT_UV, &vout_uv) || vout_uv < 800000 || vout_uv > 22000000) {
			dev_warn(dev, "ti,vout-microvolt missing/invalid (0.8V..22V). Using defaults.\n");
			vout_uv = 5000000; /* 5V default example */
		}
		/* Program VOUT_FS: FB=0 (internal), INTFB bits = intfb_sel */
		reg = (TPS55287_VOUT_FS_FB_INT) | (intfb_sel & TPS55287_VOUT_FS_INTFB_MASK);
		ret = regmap_write(tps->regmap, TPS55287_REG_VOUT_FS, reg);
		if (ret)
			return ret;

		/* Compute and write VREF code */
		{
			uint16_t ref_code;
			tps55287_calc_ref_code(vout_uv, intfb_sel, &ref_code);
			ret = tps55287_write_ref(tps, ref_code);
			if (ret)
				return ret;
			tps->vout_uv = vout_uv;
			tps->intfb_ratio_sel = intfb_sel;
		}
	} else {
		/* External divider to FB/INT; we don't set VOUT here */
		reg = TPS55287_VOUT_FS_FB_EXT; /* INTFB ignored */
		ret = regmap_write(tps->regmap, TPS55287_REG_VOUT_FS, reg);
		if (ret)
			return ret;
		tps->fb_internal = false;
	}

	/* Current limit (requires shunt value) */
	if (!of_property_read_u32(dev->of_node, DT_ILIM_MA, &tmp)) {
		uint32_t ilim_mA = tmp;
		uint32_t rsense_uohm;
		uint64_t vsense_uV; /* declare only */
		uint32_t vsense_mV2;
		uint32_t code;

		if (of_property_read_u32(dev->of_node, DT_RSENSE_UOHM, &rsense_uohm)) {
			dev_err(dev, "ti,rsense-micro-ohms required with ti,ilim-milliamp\n");
			return -EINVAL;
		}

		/* V_sense (mV) = I(mA) * R(mΩ) / 1000. R(mΩ) = rsense_uohm / 1000. */
		/* So V(mV) = I(mA) * rsense_uohm / 1e6 */
		/* LSB = 0.5 mV -> code = V/0.5 */
		vsense_uV = (uint64_t)ilim_mA * rsense_uohm; /* microvolt */
		vsense_mV2 = (uint32_t)div_u64(vsense_uV, 1000); /* convert to mV with extra 1000 scaling kept */
		/* code = round( (V_mV) / 0.5 ) = (2 * V_mV) */
		code = DIV_ROUND_CLOSEST(vsense_mV2, 500); /* because we kept extra scaling */
		if (code > 0x7F)
			code = 0x7F;

		reg = TPS55287_ILIM_EN | (code & TPS55287_ILIM_SET_MASK);
		ret = regmap_write(tps->regmap, TPS55287_REG_IOUT_LIMIT, reg);
		if (ret)
			return ret;
	}

	/* Slew rate + OCP delay */
	{
		uint8_t sr_bits = 0x01; /* 2.5 mV/us default */
		uint8_t ocp_bits = 0x00; /* 128us default */
		uint32_t sr_mv_us;
		if (!of_property_read_u32(dev->of_node, DT_SLEW_MVUS, &sr_mv_us)) {
			if (sr_mv_us <= 1250)
				sr_bits = 0x00;
			else if (sr_mv_us <= 2500)
				sr_bits = 0x01;
			else if (sr_mv_us <= 5000)
				sr_bits = 0x02;
			else
				sr_bits = 0x03;
		}
		{
			uint32_t d_ms;
			if (!of_property_read_u32(dev->of_node, DT_OCP_DELAY_MS, &d_ms)) {
				if (d_ms < 2)
					ocp_bits = 0x00; /* 0.128ms ~0 */
				else if (d_ms < 5)
					ocp_bits = 0x01; /* ~3ms */
				else if (d_ms < 9)
					ocp_bits = 0x02; /* ~6ms */
				else
					ocp_bits = 0x03; /* ~12ms */
			}
		}
		reg = (ocp_bits << 4) | (sr_bits << 0);
		ret = regmap_write(tps->regmap, TPS55287_REG_VOUT_SR, reg);
		if (ret)
			return ret;
	}

	/* CDC: enable status masks by default; set comp + option from DT */
	{
		uint8_t cdc = TPS55287_CDC_SC_MASK | TPS55287_CDC_OCP_MASK | TPS55287_CDC_OVP_MASK;
		uint32_t steps;
		if (of_property_read_bool(dev->of_node, DT_CDC_OPTION_EXT))
			cdc |= TPS55287_CDC_OPTION_EXT;
		if (!of_property_read_u32(dev->of_node, DT_CDC_STEP_100MV, &steps)) {
			if (steps > 7)
				steps = 7;
			cdc |= (steps & 0x7);
		}
		ret = regmap_write(tps->regmap, TPS55287_REG_CDC, cdc);
		if (ret)
			return ret;
	}

	/* MODE: FPWM, HICCUP, DISCHG, FORCE_DISCHG, FSWDBL; then OE=1 */
	{
		uint8_t mode = 0;
		if (of_property_read_bool(dev->of_node, DT_FPWM))
			mode |= TPS55287_MODE_FPWM;
		if (of_property_read_bool(dev->of_node, DT_HICCUP))
			mode |= TPS55287_MODE_HICCUP;
		if (of_property_read_bool(dev->of_node, DT_DISCHG_ON_SHDN))
			mode |= TPS55287_MODE_DISCHG;
		if (of_property_read_bool(dev->of_node, DT_FORCE_DISCHG))
			mode |= TPS55287_MODE_FORCE_DISCHG;
		if (of_property_read_bool(dev->of_node, DT_FSWDBL))
			mode |= TPS55287_MODE_FSWDBL;

		/* Finally enable output */
		mode |= TPS55287_MODE_OE;
		ret = regmap_write(tps->regmap, TPS55287_REG_MODE, mode);
		if (ret)
			return ret;
	}

	return 0;
}

/* Probe/remove --------------------------------------------------------- */

static int tps55287_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tps55287 *tps;
	int ret;

	if (!dev->of_node)
		return dev_err_probe(dev, -EINVAL, "No device-tree node\n");

	tps = devm_kzalloc(dev, sizeof(*tps), GFP_KERNEL);
	if (!tps)
		return -ENOMEM;

	tps->client = client;
	tps->regmap = devm_regmap_init_i2c(client, &tps55287_regmap_cfg);
	if (IS_ERR(tps->regmap))
		return dev_err_probe(dev, PTR_ERR(tps->regmap), "regmap init failed\n");

	i2c_set_clientdata(client, tps);

	ret = tps55287_apply_dt(tps);
	if (ret)
		return dev_err_probe(dev, ret, "failed to apply DT settings\n");

	dev_info(dev, "TPS55287 configured and enabled\n");

	atomic_set(&tps55287_ready, 1);
	return 0;
}

static int tps55287_remove(struct i2c_client *client)
{
	/* Optionally: clear OE to turn off output */
	struct tps55287 *tps = i2c_get_clientdata(client);
	regmap_update_bits(tps->regmap, TPS55287_REG_MODE, TPS55287_MODE_OE, 0);

	return 0;
}

static const struct of_device_id tps55287_of_match[] = { {
								 .compatible = "ti,tps55287",
							 },
							 {} };
MODULE_DEVICE_TABLE(of, tps55287_of_match);

static struct i2c_driver tps55287_driver = {
	.driver = {
		.name = DRV_NAME,
		.of_match_table = tps55287_of_match,
	},
	.probe_new = tps55287_probe,
	.remove = tps55287_remove,
};
module_i2c_driver(tps55287_driver);

MODULE_AUTHOR("Max Borglowe");
MODULE_DESCRIPTION("TI TPS55287 buck-boost (I2C) DT-configured driver");
MODULE_LICENSE("GPL");
