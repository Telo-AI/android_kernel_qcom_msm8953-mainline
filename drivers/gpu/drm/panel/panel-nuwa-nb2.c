// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generated with linux-mdss-dsi-panel-driver-generator from the vendor
 * device tree of the Nuwa AIR-H201 (nb2).
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#define NB2_INIT_FIRST_REG	0x80
#define NB2_INIT_REG_COUNT	7

struct nb2_panel_desc {
	u8 init[NB2_INIT_REG_COUNT];
};

struct nb2_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
	const struct nb2_panel_desc *desc;
};

static inline struct nb2_panel *to_nb2_panel(struct drm_panel *panel)
{
	return container_of_const(panel, struct nb2_panel, panel);
}

static void nb2_panel_reset(struct nb2_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(30);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(55);
}

static int nb2_panel_on(struct nb2_panel *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	int i;

	for (i = 0; i < NB2_INIT_REG_COUNT; i++) {
		u8 cmd[2] = { NB2_INIT_FIRST_REG + i, ctx->desc->init[i] };

		mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, cmd, sizeof(cmd));
	}

	return dsi_ctx.accum_err;
}

static int nb2_panel_off(struct nb2_panel *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static int nb2_panel_prepare(struct drm_panel *panel)
{
	struct nb2_panel *ctx = to_nb2_panel(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	nb2_panel_reset(ctx);

	ret = nb2_panel_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		return ret;
	}

	return 0;
}

static int nb2_panel_unprepare(struct drm_panel *panel)
{
	struct nb2_panel *ctx = to_nb2_panel(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = nb2_panel_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	return 0;
}

static const struct drm_display_mode nb2_panel_mode = {
	.clock = (1024 + 160 + 70 + 160) * (600 + 12 + 10 + 23) * 60 / 1000,
	.hdisplay = 1024,
	.hsync_start = 1024 + 160,
	.hsync_end = 1024 + 160 + 70,
	.htotal = 1024 + 160 + 70 + 160,
	.vdisplay = 600,
	.vsync_start = 600 + 12,
	.vsync_end = 600 + 12 + 10,
	.vtotal = 600 + 12 + 10 + 23,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int nb2_panel_get_modes(struct drm_panel *panel,
			       struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &nb2_panel_mode);
}

static const struct drm_panel_funcs nb2_panel_funcs = {
	.prepare = nb2_panel_prepare,
	.unprepare = nb2_panel_unprepare,
	.get_modes = nb2_panel_get_modes,
};

static int nb2_panel_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct nb2_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct nb2_panel, panel,
				   &nb2_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->desc = of_device_get_match_data(dev);

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void nb2_panel_remove(struct mipi_dsi_device *dsi)
{
	struct nb2_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

/* Nuwa's two panel variants differ only in registers 0x80-0x86. */
static const struct nb2_panel_desc nb2_720p_desc = {
	.init = { 0x58, 0x47, 0xd4, 0x88, 0xa9, 0xc3, 0x82 },
};

static const struct nb2_panel_desc nb2_720p_innolux_desc = {
	.init = { 0x48, 0xb8, 0x88, 0x88, 0x58, 0xd2, 0x88 },
};

static const struct of_device_id nb2_panel_of_match[] = {
	{ .compatible = "nuwa,nb2-720p", .data = &nb2_720p_desc },
	{ .compatible = "nuwa,nb2-720p-innolux", .data = &nb2_720p_innolux_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, nb2_panel_of_match);

static struct mipi_dsi_driver nb2_panel_driver = {
	.probe = nb2_panel_probe,
	.remove = nb2_panel_remove,
	.driver = {
		.name = "panel-nuwa-nb2",
		.of_match_table = nb2_panel_of_match,
	},
};
module_mipi_dsi_driver(nb2_panel_driver);

MODULE_DESCRIPTION("DRM driver for the Nuwa AIR-H201 (nb2) DSI panels");
MODULE_LICENSE("GPL");
