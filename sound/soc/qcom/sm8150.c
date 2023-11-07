// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022, The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include "qdsp6/q6afe.h"
#include "common.h"

#define DRIVER_NAME		"sm8150"

struct sm8150_snd_data {
	bool stream_prepared[AFE_PORT_MAX];
	struct snd_soc_card *card;
};

static unsigned int tdm_slot_offset[8] = {0, 4, 8, 12, 16, 20, 24, 28};

static const struct {
	unsigned int rx[1];
} cs35l41_tdm_channel_map[] = {
	{.rx = {6}}, /* BR */
	{.rx = {7}}, /* TR */
	{.rx = {6}}, /* BL */
	{.rx = {7}}, /* TL */
};

static int sm8150_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;
	snd_mask_none(fmt);
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S24_LE);

	return 0;
}

static int sm8150_tdm_snd_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = asoc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	int ret = 0, j;
	int channels, slot_width;
	unsigned int slot_mask;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S24_LE:
		slot_width = 32;
		break;
	default:
		dev_err(rtd->dev, "%s: invalid param format 0x%x\n",
				__func__, params_format(params));
		return -EINVAL;
	}

	channels = params_channels(params);
	slot_mask = 0x44;// 0x0000FFFF >> (16-channels);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0, slot_mask,
				8, slot_width);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set tdm slot, err:%d\n",
					__func__, ret);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
				channels, tdm_slot_offset);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set channel map, err:%d\n",
					__func__, ret);
			goto end;
		}
	} else {
		ret = snd_soc_dai_set_tdm_slot(cpu_dai, slot_mask, 0,
				8, slot_width);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set tdm slot, err:%d\n",
					__func__, ret);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai, channels,
				tdm_slot_offset, 0, NULL);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set channel map, err:%d\n",
					__func__, ret);
			goto end;
		}
	}

	for_each_rtd_codec_dais(rtd, j, codec_dai) {
		/* call dai driver's set_sysclk() callback */
		ret = snd_soc_dai_set_sysclk(codec_dai, 0,
					     12288000, SND_SOC_CLOCK_IN);
		if (ret < 0) {
			dev_err(codec_dai->dev, "fail to set sysclk, ret %d\n",
				ret);
			return ret;
		}

		/* call component driver's set_sysclk() callback */
		ret = snd_soc_component_set_sysclk(codec_dai->component,
						   0, 0,
						   12288000, SND_SOC_CLOCK_IN);
		if (ret < 0) {
			dev_err(codec_dai->dev, "fail to set component sysclk, ret %d\n",
				ret);
			return ret;
		}

		/* setup channel map */
		ret = snd_soc_dai_set_channel_map(codec_dai, 0, NULL,
						  ARRAY_SIZE(cs35l41_tdm_channel_map[j].rx),
						  (unsigned int *)cs35l41_tdm_channel_map[j].rx);
		if (ret < 0) {
			dev_err(codec_dai->dev, "fail to set channel map, ret %d\n",
				ret);
			return ret;
		}
	}

end:
	return ret;
}

static int sm8150_snd_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = asoc_rtd_to_cpu(rtd, 0);
	int ret = 0;

	switch (cpu_dai->id) {
	case QUATERNARY_TDM_RX_0:
	case QUATERNARY_TDM_TX_0:
		ret = sm8150_tdm_snd_hw_params(substream, params);
		break;
	default:
		pr_err("%s: invalid dai id 0x%x\n", __func__, cpu_dai->id);
		break;
	}
	return ret;
}

static int sm8150_snd_startup(struct snd_pcm_substream *substream)
{
	unsigned int fmt = SND_SOC_DAIFMT_CBS_CFS;
	unsigned int codec_dai_fmt = SND_SOC_DAIFMT_CBS_CFS;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = asoc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai = asoc_rtd_to_codec(rtd, 0);

	switch (cpu_dai->id) {
	case QUATERNARY_TDM_RX_0:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_DSP_A;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_QUAD_TDM_IBIT,
			12288000, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	default:
		break;
	}
	return 0;
}

static const struct snd_soc_ops sm8150_be_ops = {
	.hw_params = sm8150_snd_hw_params,
	.startup = sm8150_snd_startup,
};

static void sm8150_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->be_hw_params_fixup = sm8150_be_hw_params_fixup;
			link->ops = &sm8150_be_ops;
		}
	}
}

static int sm8150_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sm8150_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	/* Allocate the private data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	card->dev = dev;
	dev_set_drvdata(dev, card);
	snd_soc_card_set_drvdata(card, data);
	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	card->driver_name = DRIVER_NAME;
	sm8150_add_be_ops(card);
	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id snd_sm8150_dt_match[] = {
	{.compatible = "qcom,sm8150-sndcard"},
	{}
};

MODULE_DEVICE_TABLE(of, snd_sm8150_dt_match);

static struct platform_driver snd_sm8150_driver = {
	.probe  = sm8150_platform_probe,
	.driver = {
		.name = "snd-sm8150",
		.of_match_table = snd_sm8150_dt_match,
	},
};
module_platform_driver(snd_sm8150_driver);
MODULE_AUTHOR("map220v <map220v300@gmail.com");
MODULE_DESCRIPTION("SM8150 ASoC Machine Driver");
MODULE_LICENSE("GPL v2");
