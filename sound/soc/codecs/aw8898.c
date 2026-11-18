// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW8898 smart amplifier.
 *
 * The board tuning comes as a firmware file of register writes, each a
 * little-endian 16-bit register address followed by a little-endian 16-bit
 * value, applied before every power-up.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/unaligned.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define AW8898_REG_ID		0x00
#define AW8898_REG_SYSST	0x01
#define AW8898_REG_SYSCTRL	0x04
#define AW8898_REG_I2SCTRL	0x05
#define AW8898_REG_PWMCTRL	0x08
#define AW8898_REG_MAX		0x7f

#define AW8898_CHIP_ID		0x1702

#define AW8898_SYSST_PLLS	BIT(0)
#define AW8898_SYSCTRL_PWDN	BIT(0)
#define AW8898_I2SCTRL_FMS	GENMASK(7, 6)
#define AW8898_I2SCTRL_BCK	GENMASK(5, 4)
#define AW8898_I2SCTRL_SR	GENMASK(3, 0)
#define AW8898_PWMCTRL_HMUTE	BIT(0)

struct aw8898 {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	const char *fw_name;
	const struct firmware *fw;
};

static int aw8898_apply_tuning(struct aw8898 *aw)
{
	const u8 *data;
	size_t i;
	int ret;

	if (!aw->fw) {
		ret = request_firmware(&aw->fw, aw->fw_name, aw->dev);
		if (ret)
			return ret;
		if (aw->fw->size % 4) {
			dev_err(aw->dev, "%s: size %zu is not a multiple of 4\n",
				aw->fw_name, aw->fw->size);
			release_firmware(aw->fw);
			aw->fw = NULL;
			return -EINVAL;
		}
	}

	data = aw->fw->data;
	for (i = 0; i < aw->fw->size; i += 4) {
		ret = regmap_write(aw->regmap, get_unaligned_le16(data + i),
				   get_unaligned_le16(data + i + 2));
		if (ret)
			return ret;
	}

	return 0;
}

static int aw8898_power_down(struct aw8898 *aw)
{
	int ret;

	ret = regmap_set_bits(aw->regmap, AW8898_REG_PWMCTRL,
			      AW8898_PWMCTRL_HMUTE);
	if (ret)
		return ret;

	return regmap_set_bits(aw->regmap, AW8898_REG_SYSCTRL,
			       AW8898_SYSCTRL_PWDN);
}

static int aw8898_power_up(struct aw8898 *aw)
{
	unsigned int val;
	int ret;

	ret = regmap_clear_bits(aw->regmap, AW8898_REG_SYSCTRL,
				AW8898_SYSCTRL_PWDN);
	if (ret)
		return ret;

	/* The PLL locks to the bit clock, which must already be running */
	ret = regmap_read_poll_timeout(aw->regmap, AW8898_REG_SYSST, val,
				       val & AW8898_SYSST_PLLS, 2000, 20000);
	if (ret) {
		dev_err(aw->dev, "PLL did not lock: %d\n", ret);
		aw8898_power_down(aw);
		return ret;
	}

	return regmap_clear_bits(aw->regmap, AW8898_REG_PWMCTRL,
				 AW8898_PWMCTRL_HMUTE);
}

static int aw8898_startup(struct snd_pcm_substream *substream,
			  struct snd_soc_dai *dai)
{
	struct aw8898 *aw = snd_soc_component_get_drvdata(dai->component);
	int ret;

	ret = aw8898_apply_tuning(aw);
	if (ret)
		return ret;

	/* The tuning ends powered up; stay down until the stream runs */
	return aw8898_power_down(aw);
}

static int aw8898_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{
	struct aw8898 *aw = snd_soc_component_get_drvdata(dai->component);
	unsigned int sr, fms, bck;

	switch (params_rate(params)) {
	case 8000:
		sr = 0;
		break;
	case 16000:
		sr = 3;
		break;
	case 32000:
		sr = 6;
		break;
	case 44100:
		sr = 7;
		break;
	case 48000:
		sr = 8;
		break;
	case 96000:
		sr = 9;
		break;
	default:
		return -EINVAL;
	}

	switch (params_width(params)) {
	case 16:
		fms = 0;
		break;
	case 20:
		fms = 1;
		break;
	case 24:
		fms = 2;
		break;
	case 32:
		fms = 3;
		break;
	default:
		return -EINVAL;
	}

	/* Bit clock per frame: 32, 48 or 64 times the sample rate */
	switch (params_physical_width(params)) {
	case 16:
		bck = 0;
		break;
	case 24:
		bck = 1;
		break;
	case 32:
		bck = 2;
		break;
	default:
		return -EINVAL;
	}

	return regmap_update_bits(aw->regmap, AW8898_REG_I2SCTRL,
				  AW8898_I2SCTRL_FMS | AW8898_I2SCTRL_BCK |
				  AW8898_I2SCTRL_SR,
				  FIELD_PREP(AW8898_I2SCTRL_FMS, fms) |
				  FIELD_PREP(AW8898_I2SCTRL_BCK, bck) |
				  FIELD_PREP(AW8898_I2SCTRL_SR, sr));
}

static int aw8898_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_CBC_CFC ||
	    (fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S ||
	    (fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF)
		return -EINVAL;

	return 0;
}

static int aw8898_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	struct aw8898 *aw = snd_soc_component_get_drvdata(dai->component);

	return mute ? aw8898_power_down(aw) : aw8898_power_up(aw);
}

static const struct snd_soc_dai_ops aw8898_dai_ops = {
	.startup = aw8898_startup,
	.hw_params = aw8898_hw_params,
	.set_fmt = aw8898_set_fmt,
	.mute_stream = aw8898_mute_stream,
	.no_capture_mute = 1,
};

static struct snd_soc_dai_driver aw8898_dai = {
	.name = "aw8898-aif",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
			 SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
			 SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &aw8898_dai_ops,
};

static const struct snd_soc_dapm_widget aw8898_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("AIF IN", "Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUTPUT("SPK"),
};

static const struct snd_soc_dapm_route aw8898_dapm_routes[] = {
	{ "SPK", NULL, "AIF IN" },
};

static const struct snd_soc_component_driver aw8898_component = {
	.dapm_widgets = aw8898_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(aw8898_dapm_widgets),
	.dapm_routes = aw8898_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(aw8898_dapm_routes),
	.endianness = 1,
};

static bool aw8898_volatile_reg(struct device *dev, unsigned int reg)
{
	return reg == AW8898_REG_SYSST;
}

static const struct regmap_config aw8898_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = AW8898_REG_MAX,
	.volatile_reg = aw8898_volatile_reg,
	.cache_type = REGCACHE_NONE,
};

static void aw8898_release_firmware(void *data)
{
	struct aw8898 *aw = data;

	release_firmware(aw->fw);
}

static int aw8898_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct aw8898 *aw;
	unsigned int id;
	int ret;

	aw = devm_kzalloc(dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;

	aw->dev = dev;
	i2c_set_clientdata(client, aw);

	ret = device_property_read_string(dev, "firmware-name", &aw->fw_name);
	if (ret)
		return dev_err_probe(dev, ret, "Missing firmware-name\n");

	aw->regmap = devm_regmap_init_i2c(client, &aw8898_regmap_config);
	if (IS_ERR(aw->regmap))
		return dev_err_probe(dev, PTR_ERR(aw->regmap),
				     "Failed to init regmap\n");

	aw->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(aw->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(aw->reset_gpio),
				     "Failed to get reset GPIO\n");
	if (aw->reset_gpio) {
		usleep_range(1000, 2000);
		gpiod_set_value_cansleep(aw->reset_gpio, 0);
		usleep_range(1000, 2000);
	}

	ret = regmap_read(aw->regmap, AW8898_REG_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read chip ID\n");
	if (id != AW8898_CHIP_ID)
		return dev_err_probe(dev, -ENODEV, "Unknown chip ID 0x%04x\n", id);

	ret = devm_add_action_or_reset(dev, aw8898_release_firmware, aw);
	if (ret)
		return ret;

	ret = aw8898_power_down(aw);
	if (ret)
		return ret;

	return devm_snd_soc_register_component(dev, &aw8898_component,
					       &aw8898_dai, 1);
}

static const struct of_device_id aw8898_of_match[] = {
	{ .compatible = "awinic,aw8898" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, aw8898_of_match);

static const struct i2c_device_id aw8898_i2c_id[] = {
	{ "aw8898" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, aw8898_i2c_id);

static struct i2c_driver aw8898_i2c_driver = {
	.driver = {
		.name = "aw8898",
		.of_match_table = aw8898_of_match,
	},
	.probe = aw8898_i2c_probe,
	.id_table = aw8898_i2c_id,
};
module_i2c_driver(aw8898_i2c_driver);

MODULE_DESCRIPTION("Awinic AW8898 smart amplifier driver");
MODULE_LICENSE("GPL");
