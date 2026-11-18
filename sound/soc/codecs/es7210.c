// SPDX-License-Identifier: GPL-2.0-only
/*
 * Everest Semiconductor ES7210 four-channel microphone ADC.
 *
 * The chips run in "N LRCK" TDM mode: several ES7210s share one I2S data
 * line, each sending its four channels as stereo pairs over successive
 * frames, and the first chip in the chain starts each sequence.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <sound/pcm.h>
#include <sound/soc.h>

#define ES7210_RESET		0x00
#define ES7210_SDP_CFG2		0x12
#define ES7210_ADC34_MUTE	0x14
#define ES7210_ADC12_MUTE	0x15
#define ES7210_MIC1_GAIN	0x43
#define ES7210_MIC2_GAIN	0x44
#define ES7210_MIC3_GAIN	0x45
#define ES7210_MIC4_GAIN	0x46
#define ES7210_CHIP_ID1		0x3d
#define ES7210_CHIP_ID0		0x3e
#define ES7210_MAX_REG		0x4e

#define ES7210_ADC_MUTE		GENMASK(1, 0)

struct es7210 {
	struct regmap *regmap;
	struct clk *mclk;
	bool tdm_first;
};

/*
 * 16-bit I2S frames in N LRCK TDM mode with all four microphones powered,
 * as the vendor driver sets the chips up. The ADCs stay muted until a
 * capture stream starts.
 */
static const struct reg_sequence es7210_init_head[] = {
	{ ES7210_RESET, 0xff },
	{ ES7210_RESET, 0x32 },
	{ 0x09, 0x30 },
	{ 0x0a, 0x30 },
	{ 0x23, 0x2a },
	{ 0x22, 0xca },
	{ 0x21, 0x2a },
	{ 0x20, 0xca },
	{ 0x08, 0x40 },
	{ 0x11, 0x60 },
};

static const struct reg_sequence es7210_init_tail[] = {
	{ 0x40, 0xc3 },
	{ 0x41, 0x70 },
	{ 0x42, 0x70 },
	{ ES7210_MIC1_GAIN, 0x1c },
	{ ES7210_MIC2_GAIN, 0x1c },
	{ ES7210_MIC3_GAIN, 0x1c },
	{ ES7210_MIC4_GAIN, 0x1c },
	{ 0x47, 0x08 },
	{ 0x48, 0x08 },
	{ 0x49, 0x08 },
	{ 0x4a, 0x08 },
	{ 0x07, 0x20 },
	{ 0x02, 0xc3 },
	{ 0x08, 0x60 },
	{ 0x06, 0x04 },
	{ 0x4b, 0x0f },
	{ 0x4c, 0x0f },
	{ ES7210_RESET, 0x71 },
	{ ES7210_RESET, 0x41 },
	{ ES7210_ADC34_MUTE, 0x03 },
	{ ES7210_ADC12_MUTE, 0x03 },
	{ 0x1a, 0x00 },
	{ 0x1e, 0xbf },
	{ 0x1d, 0xbf },
	{ 0x1c, 0xbf },
	{ 0x1b, 0xbf },
	{ 0x10, 0xc0 },
	{ 0x11, 0x00 },
};

static int es7210_init(struct es7210 *es)
{
	int ret;

	ret = regmap_multi_reg_write(es->regmap, es7210_init_head,
				     ARRAY_SIZE(es7210_init_head));
	if (ret)
		return ret;

	ret = regmap_write(es->regmap, ES7210_SDP_CFG2,
			   es->tdm_first ? 0x07 : 0x03);
	if (ret)
		return ret;

	return regmap_multi_reg_write(es->regmap, es7210_init_tail,
				      ARRAY_SIZE(es7210_init_tail));
}

static int es7210_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	struct es7210 *es = snd_soc_component_get_drvdata(dai->component);
	unsigned int val = mute ? ES7210_ADC_MUTE : 0;
	int ret;

	ret = regmap_update_bits(es->regmap, ES7210_ADC34_MUTE,
				 ES7210_ADC_MUTE, val);
	if (ret)
		return ret;

	return regmap_update_bits(es->regmap, ES7210_ADC12_MUTE,
				  ES7210_ADC_MUTE, val);
}

static int es7210_startup(struct snd_pcm_substream *substream,
			  struct snd_soc_dai *dai)
{
	struct es7210 *es = snd_soc_component_get_drvdata(dai->component);

	return clk_prepare_enable(es->mclk);
}

static void es7210_shutdown(struct snd_pcm_substream *substream,
			    struct snd_soc_dai *dai)
{
	struct es7210 *es = snd_soc_component_get_drvdata(dai->component);

	clk_disable_unprepare(es->mclk);
}

static int es7210_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_CBC_CFC ||
	    (fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S ||
	    (fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF)
		return -EINVAL;

	return 0;
}

static const struct snd_soc_dai_ops es7210_dai_ops = {
	.startup = es7210_startup,
	.shutdown = es7210_shutdown,
	.set_fmt = es7210_set_fmt,
	.mute_stream = es7210_mute_stream,
};

static struct snd_soc_dai_driver es7210_dai = {
	.name = "es7210-tdm",
	.capture = {
		.stream_name = "Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_96000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &es7210_dai_ops,
};

static const struct snd_kcontrol_new es7210_controls[] = {
	SOC_SINGLE("MIC1 Volume", ES7210_MIC1_GAIN, 0, 14, 0),
	SOC_SINGLE("MIC2 Volume", ES7210_MIC2_GAIN, 0, 14, 0),
	SOC_SINGLE("MIC3 Volume", ES7210_MIC3_GAIN, 0, 14, 0),
	SOC_SINGLE("MIC4 Volume", ES7210_MIC4_GAIN, 0, 14, 0),
};

static const struct snd_soc_dapm_widget es7210_dapm_widgets[] = {
	SND_SOC_DAPM_INPUT("MIC1"),
	SND_SOC_DAPM_INPUT("MIC2"),
	SND_SOC_DAPM_INPUT("MIC3"),
	SND_SOC_DAPM_INPUT("MIC4"),
	SND_SOC_DAPM_AIF_OUT("AIF OUT", "Capture", 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route es7210_dapm_routes[] = {
	{ "AIF OUT", NULL, "MIC1" },
	{ "AIF OUT", NULL, "MIC2" },
	{ "AIF OUT", NULL, "MIC3" },
	{ "AIF OUT", NULL, "MIC4" },
};

static const struct snd_soc_component_driver es7210_component = {
	.controls = es7210_controls,
	.num_controls = ARRAY_SIZE(es7210_controls),
	.dapm_widgets = es7210_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(es7210_dapm_widgets),
	.dapm_routes = es7210_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(es7210_dapm_routes),
	.endianness = 1,
};

static const struct regmap_config es7210_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = ES7210_MAX_REG,
	.cache_type = REGCACHE_NONE,
};

static int es7210_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	unsigned int id1, id0;
	struct es7210 *es;
	int ret;

	es = devm_kzalloc(dev, sizeof(*es), GFP_KERNEL);
	if (!es)
		return -ENOMEM;

	i2c_set_clientdata(client, es);
	es->tdm_first = device_property_read_bool(dev, "everest,tdm-first");

	ret = devm_regulator_get_enable(dev, "vdda");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable vdda\n");

	es->mclk = devm_clk_get_optional(dev, "mclk");
	if (IS_ERR(es->mclk))
		return dev_err_probe(dev, PTR_ERR(es->mclk), "Failed to get mclk\n");

	es->regmap = devm_regmap_init_i2c(client, &es7210_regmap_config);
	if (IS_ERR(es->regmap))
		return dev_err_probe(dev, PTR_ERR(es->regmap),
				     "Failed to init regmap\n");

	ret = regmap_read(es->regmap, ES7210_CHIP_ID1, &id1);
	if (!ret)
		ret = regmap_read(es->regmap, ES7210_CHIP_ID0, &id0);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read chip ID\n");
	if (id1 != 0x72 || id0 != 0x10)
		return dev_err_probe(dev, -ENODEV, "Unknown chip ID %02x%02x\n",
				     id1, id0);

	ret = es7210_init(es);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize\n");

	return devm_snd_soc_register_component(dev, &es7210_component,
					       &es7210_dai, 1);
}

static const struct of_device_id es7210_of_match[] = {
	{ .compatible = "everest,es7210" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, es7210_of_match);

static const struct i2c_device_id es7210_i2c_id[] = {
	{ "es7210" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, es7210_i2c_id);

static struct i2c_driver es7210_i2c_driver = {
	.driver = {
		.name = "es7210",
		.of_match_table = es7210_of_match,
	},
	.probe = es7210_i2c_probe,
	.id_table = es7210_i2c_id,
};
module_i2c_driver(es7210_i2c_driver);

MODULE_DESCRIPTION("Everest ES7210 microphone ADC driver");
MODULE_LICENSE("GPL");
