/* Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/mfd/pm8xxx/pm8921.h>
#include <linux/io.h>
#include <soc/qcom/subsystem_notif.h>
#include <soc/qcom/socinfo.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/jack.h>
#include <sound/q6afe-v2.h>
#include <sound/q6core.h>

#include "qdsp6v2/msm-pcm-routing-v2.h"
#include "../codecs/wcd9xxx-common.h"
#include "../codecs/wcd9306.h"

#define SAMPLING_RATE_48KHZ 48000
#define SAMPLING_RATE_96KHZ 96000
#define SAMPLING_RATE_192KHZ 192000

#define DRV_NAME "msm8226-asoc-tapan"

#define MSM_SLIM_0_RX_MAX_CHANNELS		2
#define MSM_SLIM_0_TX_MAX_CHANNELS		4

#define BTSCO_RATE_8KHZ 8000
#define BTSCO_RATE_16KHZ 16000

/* It takes about 13ms for Class-D PAs to ramp-up */
#define EXT_CLASS_D_EN_DELAY 13000
#define EXT_CLASS_D_DIS_DELAY 3000
#define EXT_CLASS_D_DELAY_DELTA 2000

#define WCD9XXX_MBHC_DEF_BUTTONS 8
#define WCD9XXX_MBHC_DEF_RLOADS 5
#define TAPAN_EXT_CLK_RATE 9600000

#define GPIO_SEC_MI2S_SCK    70
#define GPIO_SEC_MI2S_WS     69
#define GPIO_SEC_MI2S_DATA0  71
#define GPIO_SEC_MI2S_DATA1  68
#define GPIO_SEC_NUM 4

#define GPIO_TERT_MI2S_SCK    49
#define GPIO_TERT_MI2S_WS     50
#define GPIO_TERT_MI2S_DATA0  51
#define GPIO_TERT_MI2S_DATA1  52
#define GPIO_TERT_NUM 4

#define GPIO_QUAT_MI2S_SCK    46
#define GPIO_QUAT_MI2S_WS     47
#define GPIO_QUAT_MI2S_DATA0  48
#define GPIO_QUAT_NUM 3

#define GPIO_SDA4_AP_PA       10
#define GPIO_SCL4_AP_PA       11

struct request_gpio {
	unsigned gpio_no;
	char *gpio_name;
};

static struct request_gpio sec_mi2s_gpio[] = {
	{
		.gpio_no = GPIO_SEC_MI2S_SCK,
		.gpio_name = "SEC_MI2S_SCK",
	},
	{
		.gpio_no = GPIO_SEC_MI2S_WS,
		.gpio_name = "SEC_MI2S_WS",
	},
	{
		.gpio_no = GPIO_SEC_MI2S_DATA0,
		.gpio_name = "SEC_MI2S_DATA0",
	},
	{
		.gpio_no = GPIO_SEC_MI2S_DATA1,
		.gpio_name = "SEC_MI2S_DATA1",
	},
};

static struct request_gpio tert_mi2s_gpio[] = {
	{
		.gpio_no = GPIO_TERT_MI2S_SCK,
		.gpio_name = "TERT_MI2S_SCK",
	},
	{
		.gpio_no = GPIO_TERT_MI2S_WS,
		.gpio_name = "TERT_MI2S_WS",
	},
	{
		.gpio_no = GPIO_TERT_MI2S_DATA0,
		.gpio_name = "TERT_MI2S_DATA0",
	},
	{
		.gpio_no = GPIO_TERT_MI2S_DATA1,
		.gpio_name = "TERT_MI2S_DATA1",
	},
};
static struct request_gpio 	quat_mi2s_gpio[] = {
	{
		.gpio_no = GPIO_QUAT_MI2S_SCK,
		.gpio_name = "QUAT_MI2S_SCK",
	},
	{
		.gpio_no = GPIO_QUAT_MI2S_WS,
		.gpio_name = "QUAT_MI2S_WS",
	},
	{
		.gpio_no = GPIO_QUAT_MI2S_DATA0,
		.gpio_name = "QUAT_MI2S_DATA0",
	}
};
/* MI2S clock */
struct mi2s_clk {
	struct clk *core_clk;
	struct clk *osr_clk;
	struct clk *bit_clk;
	atomic_t mi2s_rsc_ref;
};
static struct mi2s_clk sec_mi2s_clk ;
static struct mi2s_clk tert_mi2s_clk ;
static struct mi2s_clk quat_mi2s_clk ;

#define NUM_OF_AUXPCM_GPIOS 4

#define LO_1_SPK_AMP   0x1
#define LO_2_SPK_AMP   0x2

#define ADSP_STATE_READY_TIMEOUT_MS 3000


static int msm8226_auxpcm_rate = 8000;
static atomic_t auxpcm_rsc_ref;
static const char *const auxpcm_rate_text[] = {"rate_8000", "rate_16000"};
static const struct soc_enum msm8226_auxpcm_enum[] = {
		SOC_ENUM_SINGLE_EXT(2, auxpcm_rate_text),
};

#define LPAIF_OFFSET 0xFE000000
#define LPAIF_PRI_MODE_MUXSEL (LPAIF_OFFSET + 0x2B000)
#define LPAIF_SEC_MODE_MUXSEL (LPAIF_OFFSET + 0x2C000)
#define LPAIF_TER_MODE_MUXSEL (LPAIF_OFFSET + 0x2D000)
#define LPAIF_QUAD_MODE_MUXSEL (LPAIF_OFFSET + 0x2E000)

#define I2S_PCM_SEL 1
#define I2S_PCM_SEL_OFFSET 1

void *def_tapan_mbhc_cal(void);
static int msm_snd_enable_codec_ext_clk(struct snd_soc_codec *codec, int enable,
					bool dapm);

static struct wcd9xxx_mbhc_config mbhc_cfg = {
	.read_fw_bin = false,
	.calibration = NULL,
	.micbias = MBHC_MICBIAS2,
	.anc_micbias = MBHC_MICBIAS2,
	.mclk_cb_fn = msm_snd_enable_codec_ext_clk,
	.mclk_rate = TAPAN_EXT_CLK_RATE,
	.gpio = 0,
	.gpio_irq = 0,
	.gpio_level_insert = 0,
	.detect_extn_cable = true,
	.micbias_enable_flags = 1 << MBHC_MICBIAS_ENABLE_THRESHOLD_HEADSET,
	.insert_detect = true,
	.swap_gnd_mic = NULL,
	.cs_enable_flags = (1 << MBHC_CS_ENABLE_POLLING |
			    1 << MBHC_CS_ENABLE_INSERTION |
			    1 << MBHC_CS_ENABLE_REMOVAL |
			    1 << MBHC_CS_ENABLE_DET_ANC),
	.do_recalibration = true,
	.use_vddio_meas = true,
	.enable_anc_mic_detect = false,
	.hw_jack_type = FOUR_POLE_JACK,
};

struct msm_auxpcm_gpio {
	unsigned gpio_no;
	const char *gpio_name;
};

struct msm_auxpcm_ctrl {
	struct msm_auxpcm_gpio *pin_data;
	u32 cnt;
};

struct msm8226_asoc_mach_data {
	int mclk_gpio;
	u32 mclk_freq;
	struct msm_auxpcm_ctrl *auxpcm_ctrl;
	int us_euro_gpio;
};

#define GPIO_NAME_INDEX 0
#define DT_PARSE_INDEX  1

static char *msm_auxpcm_gpio_name[][2] = {
	{"PRIM_AUXPCM_CLK",       "qcom,prim-auxpcm-gpio-clk"},
	{"PRIM_AUXPCM_SYNC",      "qcom,prim-auxpcm-gpio-sync"},
	{"PRIM_AUXPCM_DIN",       "qcom,prim-auxpcm-gpio-din"},
	{"PRIM_AUXPCM_DOUT",      "qcom,prim-auxpcm-gpio-dout"},
};

void *lpaif_pri_muxsel_virt_addr;

/* Shared channel numbers for Slimbus ports that connect APQ to MDM. */
enum {
	SLIM_1_RX_1 = 145, /* BT-SCO and USB TX */
	SLIM_1_TX_1 = 146, /* BT-SCO and USB RX */
	SLIM_2_RX_1 = 147, /* HDMI RX */
	SLIM_3_RX_1 = 148, /* In-call recording RX */
	SLIM_3_RX_2 = 149, /* In-call recording RX */
	SLIM_4_TX_1 = 150, /* In-call musid delivery TX */
};

static int msm8226_ext_spk_pamp;
static int msm_slim_0_rx_ch = 1;
static int msm_slim_0_tx_ch = 1;

static int msm_btsco_rate = BTSCO_RATE_8KHZ;
static int msm_btsco_ch = 1;

static struct mutex cdc_mclk_mutex;
static struct clk *codec_clk;
static int clk_users;
static int ext_spk_amp_gpio = -1;
static int vdd_spkr_gpio = -1;
static int msm_proxy_rx_ch = 2;

static int slim0_rx_sample_rate = SAMPLING_RATE_48KHZ;
static int slim0_rx_bit_format = SNDRV_PCM_FORMAT_S16_LE;

static inline int param_is_mask(int p)
{
	return ((p >= SNDRV_PCM_HW_PARAM_FIRST_MASK) &&
			(p <= SNDRV_PCM_HW_PARAM_LAST_MASK));
}

static inline struct snd_mask *param_to_mask(struct snd_pcm_hw_params *p, int n)
{
	return &(p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK]);
}

static int msm_snd_enable_codec_ext_clk(struct snd_soc_codec *codec, int enable,
					bool dapm)
{
	int ret = 0;
	pr_debug("%s: enable = %d clk_users = %d\n",
		__func__, enable, clk_users);

	mutex_lock(&cdc_mclk_mutex);
	if (enable) {
		if (!codec_clk) {
			dev_err(codec->dev, "%s: did not get Taiko MCLK\n",
					__func__);
			ret = -EINVAL;
			goto exit;
		}

		clk_users++;
		if (clk_users != 1)
			goto exit;
		if (codec_clk) {
			clk_prepare_enable(codec_clk);
			tapan_mclk_enable(codec, 1, dapm);
		} else {
			pr_err("%s: Error setting Tapan MCLK\n", __func__);
			clk_users--;
			ret = -EINVAL;
			goto exit;
		}
	} else {
		if (clk_users > 0) {
			clk_users--;
			if (clk_users == 0) {
				tapan_mclk_enable(codec, 0, dapm);
				clk_disable_unprepare(codec_clk);
			}
		} else {
			pr_err("%s: Error releasing Tapan MCLK\n", __func__);
			ret = -EINVAL;
			goto exit;
		}
	}
exit:
	mutex_unlock(&cdc_mclk_mutex);
	return ret;
}

static int msm8226_mclk_event(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	pr_debug("%s: event = %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		return msm_snd_enable_codec_ext_clk(w->codec, 1, true);
	case SND_SOC_DAPM_POST_PMD:
		return msm_snd_enable_codec_ext_clk(w->codec, 0, true);
	}

	return 0;
}

static void msm8226_ext_spk_power_amp_enable(u32 enable)
{
	if (enable) {
		gpio_direction_output(ext_spk_amp_gpio, enable);
		/* time takes enable the external power amplifier */
		usleep_range(EXT_CLASS_D_EN_DELAY,
			EXT_CLASS_D_EN_DELAY + EXT_CLASS_D_DELAY_DELTA);
	} else {
		gpio_direction_output(ext_spk_amp_gpio, enable);
		/* time takes disable the external power amplifier */
		usleep_range(EXT_CLASS_D_DIS_DELAY,
			EXT_CLASS_D_DIS_DELAY + EXT_CLASS_D_DELAY_DELTA);
	}

	pr_debug("%s: %s external speaker PAs.\n", __func__,
		enable ? "Enable" : "Disable");
}

static void msm8226_ext_spk_power_amp_on(u32 spk)
{
	if (gpio_is_valid(ext_spk_amp_gpio)) {
		if (spk & (LO_1_SPK_AMP | LO_2_SPK_AMP)) {
			pr_debug("%s:Enable left and right speakers case spk = 0x%x\n",
				__func__, spk);

			msm8226_ext_spk_pamp |= spk;

			if ((msm8226_ext_spk_pamp & LO_1_SPK_AMP) &&
				(msm8226_ext_spk_pamp & LO_2_SPK_AMP))
				if (ext_spk_amp_gpio >= 0) {
					pr_debug("%s  enable power", __func__);
					msm8226_ext_spk_power_amp_enable(1);
				}
		} else  {
			pr_err("%s: Invalid external speaker ampl. spk = 0x%x\n",
				__func__, spk);
		}
	}
}

static void msm8226_ext_spk_power_amp_off(u32 spk)
{
	if (gpio_is_valid(ext_spk_amp_gpio)) {
		if (spk & (LO_1_SPK_AMP | LO_2_SPK_AMP)) {
			pr_debug("%s Disable left and right speakers case spk = 0x%08x",
				__func__, spk);

			msm8226_ext_spk_pamp &= ~spk;

			if (!msm8226_ext_spk_pamp) {
				if (ext_spk_amp_gpio >= 0) {
					pr_debug("%s  disable power", __func__);
					msm8226_ext_spk_power_amp_enable(0);
				}
				msm8226_ext_spk_pamp = 0;
			}
		 } else  {
			pr_err("%s: ERROR : Invalid Ext Spk Ampl. spk = 0x%08x\n",
				__func__, spk);
		}
	}
}

static int msm8226_ext_spkramp_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *k, int event)
{
	pr_debug("%s()\n", __func__);

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		if (!strncmp(w->name, "Lineout_1 amp", 14))
			msm8226_ext_spk_power_amp_on(LO_1_SPK_AMP);
		else if (!strncmp(w->name, "Lineout_2 amp", 14))
			msm8226_ext_spk_power_amp_on(LO_2_SPK_AMP);
		else {
			pr_err("%s() Invalid Speaker Widget = %s\n",
				__func__, w->name);
			return -EINVAL;
		}
	} else {
		if (!strncmp(w->name, "Lineout_1 amp", 14))
			msm8226_ext_spk_power_amp_off(LO_1_SPK_AMP);
		else if (!strncmp(w->name, "Lineout_2 amp", 14))
			msm8226_ext_spk_power_amp_off(LO_2_SPK_AMP);
		else {
			pr_err("%s() Invalid Speaker Widget = %s\n",
				__func__, w->name);
			return -EINVAL;
		}
	}

	return 0;
}

static int msm8226_vdd_spkr_event(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	pr_debug("%s: event = %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		if (vdd_spkr_gpio >= 0) {
			gpio_direction_output(vdd_spkr_gpio, 1);
			pr_debug("%s: Enabled 5V external supply for speaker\n",
					__func__);
		}
		break;
	case SND_SOC_DAPM_POST_PMD:
		if (vdd_spkr_gpio >= 0) {
			gpio_direction_output(vdd_spkr_gpio, 0);
			pr_debug("%s: Disabled 5V external supply for speaker\n",
					__func__);
		}
		break;
	}
	return 0;
}

static const struct snd_soc_dapm_widget msm8226_dapm_widgets[] = {

	SND_SOC_DAPM_SUPPLY("MCLK",  SND_SOC_NOPM, 0, 0,
	msm8226_mclk_event, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MIC("Handset Mic", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("ANCRight Headset Mic", NULL),
	SND_SOC_DAPM_MIC("ANCLeft Headset Mic", NULL),

	SND_SOC_DAPM_MIC("Digital Mic1", NULL),
	SND_SOC_DAPM_MIC("Digital Mic2", NULL),
	SND_SOC_DAPM_MIC("Digital Mic3", NULL),
	SND_SOC_DAPM_MIC("Digital Mic4", NULL),
	SND_SOC_DAPM_MIC("Digital Mic5", NULL),
	SND_SOC_DAPM_MIC("Digital Mic6", NULL),

	SND_SOC_DAPM_MIC("Analog Mic3", NULL),
	SND_SOC_DAPM_MIC("Analog Mic4", NULL),

	SND_SOC_DAPM_SPK("Lineout_1 amp", msm8226_ext_spkramp_event),
	SND_SOC_DAPM_SPK("Lineout_2 amp", msm8226_ext_spkramp_event),

	SND_SOC_DAPM_SUPPLY("EXT_VDD_SPKR",  SND_SOC_NOPM, 0, 0,
	msm8226_vdd_spkr_event, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
};

static const char *const slim0_rx_ch_text[] = {"One", "Two"};
static const char *const slim0_tx_ch_text[] = {"One", "Two", "Three", "Four"};
static const char *const proxy_rx_ch_text[] = {"One", "Two", "Three", "Four",
	"Five", "Six", "Seven", "Eight"};
static char const *rx_bit_format_text[] = {"S16_LE", "S24_LE"};
static char const *slim0_rx_sample_rate_text[] = {"KHZ_48", "KHZ_96",
						  "KHZ_192"};

static const struct soc_enum msm_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, slim0_rx_ch_text),
	SOC_ENUM_SINGLE_EXT(4, slim0_tx_ch_text),
};

static const char *const btsco_rate_text[] = {"8000", "16000"};
static const struct soc_enum msm_btsco_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, btsco_rate_text),
};

static int slim0_rx_sample_rate_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int sample_rate_val = 0;

	switch (slim0_rx_sample_rate) {
	case SAMPLING_RATE_192KHZ:
		sample_rate_val = 2;
		break;

	case SAMPLING_RATE_96KHZ:
		sample_rate_val = 1;
		break;

	case SAMPLING_RATE_48KHZ:
	default:
		sample_rate_val = 0;
		break;
	}

	ucontrol->value.integer.value[0] = sample_rate_val;
	pr_debug("%s: slim0_rx_sample_rate = %d\n", __func__,
				slim0_rx_sample_rate);

	return 0;
}

static int slim0_rx_sample_rate_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: ucontrol value = %ld\n", __func__,
			ucontrol->value.integer.value[0]);

	switch (ucontrol->value.integer.value[0]) {
	case 2:
		slim0_rx_sample_rate = SAMPLING_RATE_192KHZ;
		break;
	case 1:
		slim0_rx_sample_rate = SAMPLING_RATE_96KHZ;
		break;
	case 0:
	default:
		slim0_rx_sample_rate = SAMPLING_RATE_48KHZ;
		break;
	}

	pr_debug("%s: slim0_rx_sample_rate = %d\n", __func__,
			slim0_rx_sample_rate);

	return 0;
}

static int msm_slim_0_rx_ch_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_slim_0_rx_ch  = %d\n", __func__,
		 msm_slim_0_rx_ch);
	ucontrol->value.integer.value[0] = msm_slim_0_rx_ch - 1;
	return 0;
}

static int msm_slim_0_rx_ch_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	msm_slim_0_rx_ch = ucontrol->value.integer.value[0] + 1;

	pr_debug("%s: msm_slim_0_rx_ch = %d\n", __func__,
		 msm_slim_0_rx_ch);
	return 1;
}

static int msm_slim_0_tx_ch_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_slim_0_tx_ch  = %d\n", __func__,
		 msm_slim_0_tx_ch);
	ucontrol->value.integer.value[0] = msm_slim_0_tx_ch - 1;
	return 0;
}

static int msm_slim_0_tx_ch_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	msm_slim_0_tx_ch = ucontrol->value.integer.value[0] + 1;

	pr_debug("%s: msm_slim_0_tx_ch = %d\n", __func__, msm_slim_0_tx_ch);
	return 1;
}

static int msm_btsco_rate_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_btsco_rate  = %d", __func__, msm_btsco_rate);
	ucontrol->value.integer.value[0] = msm_btsco_rate;
	return 0;
}

static int msm_btsco_rate_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 8000:
		msm_btsco_rate = BTSCO_RATE_8KHZ;
		break;
	case 16000:
		msm_btsco_rate = BTSCO_RATE_16KHZ;
		break;
	default:
		msm_btsco_rate = BTSCO_RATE_8KHZ;
		break;
	}

	pr_debug("%s: msm_btsco_rate = %d\n", __func__, msm_btsco_rate);
	return 0;
}

static int msm_btsco_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	rate->min = rate->max = msm_btsco_rate;
	channels->min = channels->max = msm_btsco_ch;

	return 0;
}

static int msm_be_fm_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_debug("%s()\n", __func__);
	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;

	return 0;
}

static int msm8226_auxpcm_rate_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm8226_auxpcm_rate;
	return 0;
}

static int msm8226_auxpcm_rate_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		msm8226_auxpcm_rate = 8000;
		break;
	case 1:
		msm8226_auxpcm_rate = 16000;
		break;
	default:
		msm8226_auxpcm_rate = 8000;
		break;
	}
	return 0;
}

static int msm_proxy_rx_ch_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_proxy_rx_ch = %d\n", __func__,
						msm_proxy_rx_ch);
	ucontrol->value.integer.value[0] = msm_proxy_rx_ch - 1;
	return 0;
}

static int msm_proxy_rx_ch_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	msm_proxy_rx_ch = ucontrol->value.integer.value[0] + 1;
	pr_debug("%s: msm_proxy_rx_ch = %d\n", __func__,
						msm_proxy_rx_ch);
	return 1;
}

static int slim0_rx_bit_format_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{

	switch (slim0_rx_bit_format) {
	case SNDRV_PCM_FORMAT_S24_LE:
		ucontrol->value.integer.value[0] = 1;
		break;

	case SNDRV_PCM_FORMAT_S16_LE:
	default:
		ucontrol->value.integer.value[0] = 0;
		break;
	}

	pr_debug("%s: slim0_rx_bit_format = %d, ucontrol value = %ld\n",
			__func__, slim0_rx_bit_format,
			ucontrol->value.integer.value[0]);

	return 0;
}

static int slim0_rx_bit_format_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 1:
		slim0_rx_bit_format = SNDRV_PCM_FORMAT_S24_LE;
		break;
	case 0:
	default:
		slim0_rx_bit_format = SNDRV_PCM_FORMAT_S16_LE;
		break;
	}
	return 0;
}


static int msm_auxpcm_be_params_fixup(struct snd_soc_pcm_runtime *rtd,
					struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate =
	    hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels =
	    hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);

	rate->min = rate->max = msm8226_auxpcm_rate;
	channels->min = channels->max = 1;

	return 0;
}

static int msm_proxy_rx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_debug("%s: msm_proxy_rx_ch =%d\n", __func__, msm_proxy_rx_ch);

	if (channels->max < 2)
		channels->min = channels->max = 2;
	channels->min = channels->max = msm_proxy_rx_ch;
	rate->min = rate->max = 48000;
	return 0;
}

static int msm_proxy_tx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	rate->min = rate->max = 48000;
	return 0;
}

static int msm_aux_pcm_get_gpios(struct msm_auxpcm_ctrl *auxpcm_ctrl)
{
	struct msm_auxpcm_gpio *pin_data = NULL;
	int ret = 0;
	int i;
	int j;

	pin_data = auxpcm_ctrl->pin_data;
	if (!pin_data) {
		pr_err("%s: Invalid control data for AUXPCM\n", __func__);
		ret = -EINVAL;
		goto err;
	}
	for (i = 0; i < auxpcm_ctrl->cnt; i++, pin_data++) {
		ret = gpio_request(pin_data->gpio_no,
				pin_data->gpio_name);
		pr_debug("%s: gpio = %d, gpio name = %s\n"
			"ret = %d\n", __func__,
			pin_data->gpio_no,
			pin_data->gpio_name,
			ret);
		if (ret) {
			pr_err("%s: Failed to request gpio %d\n",
				__func__, pin_data->gpio_no);
			/* Release all GPIOs on failure */
			if (i > 0) {
				for (j = i; j >= 0; j--)
					gpio_free(pin_data->gpio_no);
			}
			goto err;
		}
	}
err:
	return ret;
}

static int msm_aux_pcm_free_gpios(struct msm_auxpcm_ctrl *auxpcm_ctrl)
{
	struct msm_auxpcm_gpio *pin_data = NULL;
	int i;
	int ret = 0;

	if (auxpcm_ctrl == NULL || auxpcm_ctrl->pin_data == NULL) {
		pr_err("%s: Invalid control data for AUXPCM\n", __func__);
		ret = -EINVAL;
		goto err;
	}

	pin_data = auxpcm_ctrl->pin_data;
	for (i = 0; i < auxpcm_ctrl->cnt; i++, pin_data++) {
		gpio_free(pin_data->gpio_no);
		pr_debug("%s: gpio = %d, gpio_name = %s\n",
			__func__, pin_data->gpio_no,
			pin_data->gpio_name);
	}
err:
	return ret;
}

static int msm_auxpcm_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct msm8226_asoc_mach_data *pdata = snd_soc_card_get_drvdata(card);
	struct msm_auxpcm_ctrl *auxpcm_ctrl = NULL;
	int ret = 0;

	pr_debug("%s(): substream = %s, auxpcm_rsc_ref counter = %d\n",
		__func__, substream->name, atomic_read(&auxpcm_rsc_ref));

	auxpcm_ctrl = pdata->auxpcm_ctrl;

	if (auxpcm_ctrl == NULL || auxpcm_ctrl->pin_data == NULL ||
		lpaif_pri_muxsel_virt_addr == NULL) {
		pr_err("%s: Invalid control data for AUXPCM\n", __func__);
		ret = -EINVAL;
		goto err;
	}
	if (atomic_inc_return(&auxpcm_rsc_ref) == 1) {
		iowrite32(I2S_PCM_SEL << I2S_PCM_SEL_OFFSET,
				lpaif_pri_muxsel_virt_addr);
		ret = msm_aux_pcm_get_gpios(auxpcm_ctrl);
	}
	if (ret < 0) {
		pr_err("%s: Aux PCM GPIO request failed\n", __func__);
		return -EINVAL;
	}
err:
	return ret;
}

static void msm_auxpcm_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct msm8226_asoc_mach_data *pdata = snd_soc_card_get_drvdata(card);
	struct msm_auxpcm_ctrl *auxpcm_ctrl = NULL;

	pr_debug("%s(): substream = %s, auxpcm_rsc_ref counter = %d\n",
		__func__, substream->name, atomic_read(&auxpcm_rsc_ref));

	auxpcm_ctrl = pdata->auxpcm_ctrl;

	if (atomic_dec_return(&auxpcm_rsc_ref) == 0)
		msm_aux_pcm_free_gpios(auxpcm_ctrl);
}

static struct snd_soc_ops msm_auxpcm_be_ops = {
	.startup = msm_auxpcm_startup,
	.shutdown = msm_auxpcm_shutdown,
};


static int msm_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	pr_debug("%s()\n", __func__);
	rate->min = rate->max = 48000;

	return 0;
}

static int msm_be_sec_mi2s_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
				SNDRV_PCM_HW_PARAM_RATE);

	rate->min = rate->max = 8000;

	return 0;
}

static int msm_be_tert_mi2s_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
				SNDRV_PCM_HW_PARAM_RATE);

	rate->min = rate->max = 48000;

	return 0;
}

static int msm_be_quat_mi2s_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
				SNDRV_PCM_HW_PARAM_RATE);

	rate->min = rate->max = 48000;

	return 0;
}

static const struct soc_enum msm_snd_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, slim0_rx_ch_text),
	SOC_ENUM_SINGLE_EXT(4, slim0_tx_ch_text),
	SOC_ENUM_SINGLE_EXT(8, proxy_rx_ch_text),
	SOC_ENUM_SINGLE_EXT(2, rx_bit_format_text),
	SOC_ENUM_SINGLE_EXT(3, slim0_rx_sample_rate_text),
};

static const struct snd_kcontrol_new msm_snd_controls[] = {
	SOC_ENUM_EXT("SLIM_0_RX Channels", msm_snd_enum[0],
		     msm_slim_0_rx_ch_get, msm_slim_0_rx_ch_put),
	SOC_ENUM_EXT("SLIM_0_TX Channels", msm_snd_enum[1],
		     msm_slim_0_tx_ch_get, msm_slim_0_tx_ch_put),
	SOC_ENUM_EXT("AUX PCM SampleRate", msm8226_auxpcm_enum[0],
			msm8226_auxpcm_rate_get, msm8226_auxpcm_rate_put),
	SOC_ENUM_EXT("Internal BTSCO SampleRate", msm_btsco_enum[0],
		     msm_btsco_rate_get, msm_btsco_rate_put),
	SOC_ENUM_EXT("PROXY_RX Channels", msm_snd_enum[2],
			msm_proxy_rx_ch_get, msm_proxy_rx_ch_put),
	SOC_ENUM_EXT("SLIM_0_RX Format", msm_snd_enum[3],
			slim0_rx_bit_format_get, slim0_rx_bit_format_put),
	SOC_ENUM_EXT("SLIM_0_RX SampleRate", msm_snd_enum[4],
			slim0_rx_sample_rate_get, slim0_rx_sample_rate_put),
};


void *def_tapan_mbhc_cal(void)
{
	void *tapan_cal;
	struct wcd9xxx_mbhc_btn_detect_cfg *btn_cfg;
	u16 *btn_low, *btn_high;
	u8 *n_ready, *n_cic, *gain;

	tapan_cal = kzalloc(WCD9XXX_MBHC_CAL_SIZE(WCD9XXX_MBHC_DEF_BUTTONS,
						WCD9XXX_MBHC_DEF_RLOADS),
			    GFP_KERNEL);
	if (!tapan_cal) {
		pr_err("%s: out of memory\n", __func__);
		return NULL;
	}

#define S(X, Y) ((WCD9XXX_MBHC_CAL_GENERAL_PTR(tapan_cal)->X) = (Y))
	S(t_ldoh, 100);
	S(t_bg_fast_settle, 100);
	S(t_shutdown_plug_rem, 255);
	S(mbhc_nsa, 2);
	S(mbhc_navg, 128);
#undef S
#define S(X, Y) ((WCD9XXX_MBHC_CAL_PLUG_DET_PTR(tapan_cal)->X) = (Y))
	S(mic_current, TAPAN_PID_MIC_5_UA);
	S(hph_current, TAPAN_PID_MIC_5_UA);
	S(t_mic_pid, 100);
	S(t_ins_complete, 250);
	S(t_ins_retry, 200);
#undef S
#define S(X, Y) ((WCD9XXX_MBHC_CAL_PLUG_TYPE_PTR(tapan_cal)->X) = (Y))
	S(v_no_mic, 30);
	S(v_hs_max, 2450);
#undef S
#define S(X, Y) ((WCD9XXX_MBHC_CAL_BTN_DET_PTR(tapan_cal)->X) = (Y))
	S(c[0], 62);
	S(c[1], 124);
	S(nc, 1);
	S(n_meas, 5);
	S(mbhc_nsc, 10);
	S(n_btn_meas, 1);
	S(n_btn_con, 2);
	S(num_btn, WCD9XXX_MBHC_DEF_BUTTONS);
	S(v_btn_press_delta_sta, 100);
	S(v_btn_press_delta_cic, 50);
#undef S
	btn_cfg = WCD9XXX_MBHC_CAL_BTN_DET_PTR(tapan_cal);
	btn_low = wcd9xxx_mbhc_cal_btn_det_mp(btn_cfg, MBHC_BTN_DET_V_BTN_LOW);
	btn_high = wcd9xxx_mbhc_cal_btn_det_mp(btn_cfg,
					       MBHC_BTN_DET_V_BTN_HIGH);
	btn_low[0] = -50;
	btn_high[0] = 20;
	btn_low[1] = 21;
	btn_high[1] = 61;
	btn_low[2] = 62;
	btn_high[2] = 104;
	btn_low[3] = 105;
	btn_high[3] = 148;
	btn_low[4] = 149;
	btn_high[4] = 189;
	btn_low[5] = 190;
	btn_high[5] = 228;
	btn_low[6] = 229;
	btn_high[6] = 269;
	btn_low[7] = 270;
	btn_high[7] = 500;
	n_ready = wcd9xxx_mbhc_cal_btn_det_mp(btn_cfg, MBHC_BTN_DET_N_READY);
	n_ready[0] = 80;
	n_ready[1] = 12;
	n_cic = wcd9xxx_mbhc_cal_btn_det_mp(btn_cfg, MBHC_BTN_DET_N_CIC);
	n_cic[0] = 60;
	n_cic[1] = 47;
	gain = wcd9xxx_mbhc_cal_btn_det_mp(btn_cfg, MBHC_BTN_DET_GAIN);
	gain[0] = 11;
	gain[1] = 14;

	return tapan_cal;
}
static int msm8226_mi2s_free_gpios(struct request_gpio *mi2s_gpio,int size)
{
	int	i;
	for (i = 0; i < size; i++)
                gpio_free(mi2s_gpio[i].gpio_no);
	return 0;
}
static struct afe_clk_cfg lpass_sec_mi2s_enable = {
        AFE_API_VERSION_I2S_CONFIG,
        Q6AFE_LPASS_IBIT_CLK_256_KHZ,/* bit_clk */
        Q6AFE_LPASS_OSR_CLK_12_P288_MHZ, /* osr_clk */
        Q6AFE_LPASS_CLK_SRC_INTERNAL,
        Q6AFE_LPASS_CLK_ROOT_DEFAULT,
        Q6AFE_LPASS_MODE_BOTH_VALID,
        0,
};
static struct afe_clk_cfg lpass_mi2s_enable = {
        AFE_API_VERSION_I2S_CONFIG,
        Q6AFE_LPASS_IBIT_CLK_1_P536_MHZ,/* bit_clk */
        Q6AFE_LPASS_OSR_CLK_12_P288_MHZ, /* osr_clk */
        Q6AFE_LPASS_CLK_SRC_INTERNAL,
        Q6AFE_LPASS_CLK_ROOT_DEFAULT,
        Q6AFE_LPASS_MODE_BOTH_VALID,
        0,
};
static struct afe_clk_cfg lpass_mi2s_disable = {
        AFE_API_VERSION_I2S_CONFIG,
        0,
        0,
        Q6AFE_LPASS_CLK_SRC_INTERNAL,
        Q6AFE_LPASS_CLK_ROOT_DEFAULT,
        Q6AFE_LPASS_MODE_BOTH_VALID,
        0,
};

static int msm8226_tfa98xx_i2c_gpio_request(void)
{
	int rtn = 0;
	pr_info("%s: request i2c gpios\n", __func__);

	rtn = gpio_request(GPIO_SDA4_AP_PA,"SPK_I2C_SDA");
	if (rtn)
	{
		pr_err("%s: Failed to request GPIO_SDA4_AP_PA\n", __func__);
		return rtn;
	}

	rtn = gpio_request(GPIO_SCL4_AP_PA,"SPK_I2C_SCL");
	if (rtn)
	{
		pr_err("%s: Failed to request GPIO_SCL4_AP_PA\n", __func__);
		gpio_free(GPIO_SDA4_AP_PA);
		return rtn;
	}
	return rtn;
}

static int msm8226_tfa98xx_i2c_gpio_free(void)
{
	pr_info("%s: free i2c gpios\n", __func__);
	gpio_free(GPIO_SDA4_AP_PA);
	gpio_free(GPIO_SCL4_AP_PA);
	return 0;
}
static void msm8226_mi2s_shutdown(struct snd_pcm_substream *substream,int id,struct mi2s_clk *mi2s_clk,struct request_gpio *mi2s_gpio,int size)
{
	int ret =0;

	if (atomic_dec_return(&(mi2s_clk->mi2s_rsc_ref)) == 0) {
		pr_info("%s: free mi2s resources\n", __func__);
	
       		ret = afe_set_lpass_clock(id, &lpass_mi2s_disable);	
       		if (ret < 0) {	
      			pr_err("%s: afe_set_lpass_clock failed\n", __func__);	
       	
      		}	
		msm8226_mi2s_free_gpios(mi2s_gpio,size);
		if(AFE_PORT_ID_TERTIARY_MI2S_RX == id)
		{
			msm8226_tfa98xx_i2c_gpio_free();
		}
	}
}

static int msm8226_configure_mi2s_gpio(struct request_gpio *mi2s_gpio,int size)
{
	int	rtn;
	int	i;
	for (i = 0; i < size; i++) {

		rtn = gpio_request(mi2s_gpio[i].gpio_no,
				mi2s_gpio[i].gpio_name);

		pr_info("%s: gpio = %d, gpio name = %s, rtn = %d\n", __func__,
		mi2s_gpio[i].gpio_no, mi2s_gpio[i].gpio_name, rtn);		
		if (rtn) {
			pr_err("%s: Failed to request gpio %d\n",
				   __func__,
				   mi2s_gpio[i].gpio_no);
			while( i >= 0) {
				gpio_free(mi2s_gpio[i].gpio_no);
				i--;
			}
			break;
		}
	}

	return rtn;
}
int msm_q6_enable_mi2s_clocks(bool enable)
{
	union afe_port_config port_config;
	int rc = 0;

	pr_info("msm_q6_enable_mi2s_clocks enter\n");
	if(enable)
	{
		port_config.i2s.channel_mode = AFE_PORT_I2S_SD0;
		port_config.i2s.mono_stereo = MSM_AFE_CH_STEREO;
		port_config.i2s.data_format= 0;
		port_config.i2s.bit_width = 16;
		port_config.i2s.reserved = 0;
		port_config.i2s.i2s_cfg_minor_version = AFE_API_VERSION_I2S_CONFIG;
		port_config.i2s.sample_rate = 48000;
		port_config.i2s.ws_src = 1;

		rc = afe_port_start(AFE_PORT_ID_TERTIARY_MI2S_RX, &port_config, 48000);
		pr_debug("afe_port_start ret: %d\n",rc);
		if(IS_ERR_VALUE(rc))
		{
			pr_err("%s:fail to open AFE port\n",__func__);
			return -EINVAL;
		}

		pr_debug("<%s> <%d>: Config AFE_PORT_ID_TERTIARY_MI2S_RX success.\n", __func__, __LINE__);
		pr_debug("<%s> <%d>: port_config.i2s.sample_rate =%d.\n", __func__, __LINE__,port_config.i2s.sample_rate);
	}
	else
	{
		pr_info("%s:afe_port_stop_nowait\n",__func__);
		rc = afe_port_stop_nowait(AFE_PORT_ID_TERTIARY_MI2S_RX);
		if (IS_ERR_VALUE(rc))
		{
			pr_err(KERN_ERR"fail to stop AFE port\n");
			return -EINVAL;
		}

		pr_debug("<%s> <%d>: Stop AFE_PORT_ID_TERTIARY_MI2S_RX success.\n", __func__, __LINE__);
	}
	return rc;
}

static int msm_tert_mi2s_clk = 0;

int msm_external_pa_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_tert_mi2s_clk = %d\n", __func__, msm_tert_mi2s_clk);
	ucontrol->value.integer.value[0] = msm_tert_mi2s_clk;
	return 0;
}

int msm_external_pa_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	int ret = -EINVAL;
	msm_tert_mi2s_clk = ucontrol->value.integer.value[0];

	pr_debug("%s:msm_tert_mi2s_clk : %d\n",__func__, msm_tert_mi2s_clk);
	if(msm_tert_mi2s_clk)
	{
		if(atomic_inc_return(&(tert_mi2s_clk.mi2s_rsc_ref)) == 1)
		{
			msm8226_tfa98xx_i2c_gpio_request();
			msm8226_configure_mi2s_gpio(tert_mi2s_gpio,GPIO_TERT_NUM);
			ret = afe_set_lpass_clock(AFE_PORT_ID_TERTIARY_MI2S_RX, &lpass_mi2s_enable);
			if (ret < 0)
			{
				pr_err("%s: enable afe_set_lpass_clock failed\n", __func__);
				return ret;
			}

			ret = msm_q6_enable_mi2s_clocks(1);
			if (ret < 0)
			{
				pr_err("%s: enable_mi2s_clocks failed\n", __func__);
				return ret;
			}
		}
	}
	else
	{
		if(atomic_dec_return(&(tert_mi2s_clk.mi2s_rsc_ref)) == 0)
		{
			ret = msm_q6_enable_mi2s_clocks(0);
			if (ret < 0)
			{
				pr_err("%s: disable_mi2s_clocks failed\n", __func__);
				return ret;
			}

			ret = afe_set_lpass_clock(AFE_PORT_ID_TERTIARY_MI2S_RX, &lpass_mi2s_disable);
			if (ret < 0)
			{
				pr_err("%s: disable afe_set_lpass_clock failed\n", __func__);
				return ret;
			}
			msm8226_mi2s_free_gpios(tert_mi2s_gpio,GPIO_TERT_NUM);
			msm8226_tfa98xx_i2c_gpio_free();
			/*Close tertiary_mi2s_tx, otherwise the current will be up to 15mA when system sleep.
			Tertiary mi2s tx is for echo reference, it's started up by hfp-sco of mixer_paths in audio HAL.*/
			afe_close(AFE_PORT_ID_TERTIARY_MI2S_TX);
		}
	}
	return ret;
}
static int msm8226_mi2s_startup(struct snd_pcm_substream *substream,int id,struct mi2s_clk *mi2s_clk,struct request_gpio *mi2s_gpio,int size)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;

	pr_info("%s: dai name %s %p\n", __func__, cpu_dai->name, cpu_dai->dev);

	if (atomic_inc_return(&(mi2s_clk->mi2s_rsc_ref)) == 1) {
		pr_info("%s: acquire mi2s resources\n", __func__);
		if(AFE_PORT_ID_TERTIARY_MI2S_RX == id)
		{
			msm8226_tfa98xx_i2c_gpio_request();
		}
		msm8226_configure_mi2s_gpio(mi2s_gpio,size);	
		if (id == AFE_PORT_ID_TERTIARY_MI2S_RX || id == AFE_PORT_ID_TERTIARY_MI2S_TX || id == AFE_PORT_ID_QUATERNARY_MI2S_TX)
		{
			pr_info("%s: setting clock for port = 0x%x\n", __func__,id);
			ret = afe_set_lpass_clock(id, &lpass_mi2s_enable);
			if (ret < 0) {
				pr_err("%s: afe_set_lpass_tert_quat_clock failed\n", __func__);
				return ret;
			}
		}else{
			ret = afe_set_lpass_clock(id, &lpass_sec_mi2s_enable);
			if (ret < 0) {
				pr_err("%s: afe_set_lpass_sec_clock failed\n", __func__);
				return ret;
			}
		}
		ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_CBS_CFS);
		if (ret < 0)
			dev_err(cpu_dai->dev, "set format for CPU dai"
				" failed\n");
		ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_CBS_CFS);
		if (ret < 0)
			dev_err(codec_dai->dev, "set format for codec dai"
				 " failed\n");

		ret  = 0;
	}
        if(AFE_PORT_ID_QUATERNARY_MI2S_TX == id)
	{
	    usleep(200000); /*sleep 200ms for mic self calibration*/
	}
	return ret;
}
static int msm8226_sec_mi2s_startup(struct snd_pcm_substream *substream)
{
	return msm8226_mi2s_startup(substream,AFE_PORT_ID_SECONDARY_MI2S_RX,&sec_mi2s_clk,sec_mi2s_gpio,GPIO_SEC_NUM);
}
static void msm8226_sec_mi2s_shutdown(struct snd_pcm_substream *substream)
{
	msm8226_mi2s_shutdown(substream,AFE_PORT_ID_SECONDARY_MI2S_RX,&sec_mi2s_clk,sec_mi2s_gpio,GPIO_SEC_NUM);
}
static struct snd_soc_ops msm8226_sec_mi2s_be_ops = {
	.startup = msm8226_sec_mi2s_startup,
	.shutdown = msm8226_sec_mi2s_shutdown
};


static int msm8226_tert_mi2s_startup(struct snd_pcm_substream *substream)
{
	return msm8226_mi2s_startup(substream,AFE_PORT_ID_TERTIARY_MI2S_RX,&tert_mi2s_clk,tert_mi2s_gpio,GPIO_TERT_NUM);
}
static void msm8226_tert_mi2s_shutdown(struct snd_pcm_substream *substream)
{
	msm8226_mi2s_shutdown(substream,AFE_PORT_ID_TERTIARY_MI2S_RX,&tert_mi2s_clk,tert_mi2s_gpio,GPIO_TERT_NUM);
}
static struct snd_soc_ops msm8226_tert_mi2s_be_ops = {
	.startup = msm8226_tert_mi2s_startup,
	.shutdown = msm8226_tert_mi2s_shutdown
};

static void msm8226_quat_mi2s_shutdown(struct snd_pcm_substream *substream)
{
	msm8226_mi2s_shutdown(substream,AFE_PORT_ID_QUATERNARY_MI2S_TX,&quat_mi2s_clk,quat_mi2s_gpio,GPIO_QUAT_NUM);
}

static int msm8226_quat_mi2s_startup(struct snd_pcm_substream *substream)
{
	return msm8226_mi2s_startup(substream,AFE_PORT_ID_QUATERNARY_MI2S_TX,&quat_mi2s_clk,quat_mi2s_gpio,GPIO_QUAT_NUM);
}
static struct snd_soc_ops msm8226_quat_mi2s_be_ops = {
	.startup = msm8226_quat_mi2s_startup,
	.shutdown = msm8226_quat_mi2s_shutdown
};
/* Digital audio interface glue - connects codec <---> CPU */
static struct snd_soc_dai_link msm8226_common_dai[] = {
	/* FrontEnd DAI Links */
	{
		.name = "MSM8226 Media1",
		.stream_name = "MultiMedia1",
		.cpu_dai_name	= "MultiMedia1",
		.platform_name  = "msm-pcm-dsp.0",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.ignore_suspend = 1,
		/* This dainlink has playback support */
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA1
	},
	{
		.name = "MSM8226 Media2",
		.stream_name = "MultiMedia2",
		.cpu_dai_name   = "MultiMedia2",
		.platform_name  = "msm-pcm-dsp.0",
		.dynamic = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* This dainlink has playback support */
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA2,
	},
	{
		.name = "Circuit-Switch Voice",
		.stream_name = "CS-Voice",
		.cpu_dai_name   = "CS-VOICE",
		.platform_name  = "msm-pcm-voice",
		.dynamic = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_CS_VOICE,
	},
	{
		.name = "MSM VoIP",
		.stream_name = "VoIP",
		.cpu_dai_name	= "VoIP",
		.platform_name  = "msm-voip-dsp",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_VOIP,
	},
	{
		.name = "MSM8226 LPA",
		.stream_name = "LPA",
		.cpu_dai_name	= "MultiMedia3",
		.platform_name  = "msm-pcm-lpa",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA3,
	},
	/* Hostless PCM purpose */
	{
		.name = "SLIMBUS_0 Hostless",
		.stream_name = "SLIMBUS_0 Hostless",
		.cpu_dai_name = "SLIMBUS0_HOSTLESS",
		.platform_name = "msm-pcm-hostless",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, /* dai link has playback support */
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "INT_FM Hostless",
		.stream_name = "INT_FM Hostless",
		.cpu_dai_name	= "INT_FM_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "MSM AFE-PCM RX",
		.stream_name = "AFE-PROXY RX",
		.cpu_dai_name = "msm-dai-q6-dev.241",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.platform_name  = "msm-pcm-afe",
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
	},
	{
		.name = "MSM AFE-PCM TX",
		.stream_name = "AFE-PROXY TX",
		.cpu_dai_name = "msm-dai-q6-dev.240",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.platform_name  = "msm-pcm-afe",
		.ignore_suspend = 1,
	},
	{
		.name = "MSM8226 Compr",
		.stream_name = "COMPR",
		.cpu_dai_name	= "MultiMedia4",
		.platform_name  = "msm-compress-dsp",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		 /* this dainlink has playback support */
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA4,
	},
	{
		.name = "AUXPCM Hostless",
		.stream_name = "AUXPCM Hostless",
		.cpu_dai_name   = "AUXPCM_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "SLIMBUS_1 Hostless",
		.stream_name = "SLIMBUS_1 Hostless",
		.cpu_dai_name = "SLIMBUS1_HOSTLESS",
		.platform_name = "msm-pcm-hostless",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, /* dai link has playback support */
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "SLIMBUS_3 Hostless",
		.stream_name = "SLIMBUS_3 Hostless",
		.cpu_dai_name = "SLIMBUS3_HOSTLESS",
		.platform_name = "msm-pcm-hostless",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, /* dai link has playback support */
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "SLIMBUS_4 Hostless",
		.stream_name = "SLIMBUS_4 Hostless",
		.cpu_dai_name = "SLIMBUS4_HOSTLESS",
		.platform_name = "msm-pcm-hostless",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, /* dai link has playback support */
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "Voice2",
		.stream_name = "Voice2",
		.cpu_dai_name   = "Voice2",
		.platform_name  = "msm-pcm-voice",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "MSM8226 LowLatency",
		.stream_name = "MultiMedia5",
		.cpu_dai_name   = "MultiMedia5",
		.platform_name  = "msm-pcm-dsp.1",
		.dynamic = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA5,
	},
	{
		.name = "MSM8226 Media9",
		.stream_name = "MultiMedia9",
		.cpu_dai_name   = "MultiMedia9",
		.platform_name  = "msm-pcm-dsp.0",
		.dynamic = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* This dailink has playback support */
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA9,
	},
	{
		.name = "VoLTE",
		.stream_name = "VoLTE",
		.cpu_dai_name   = "VoLTE",
		.platform_name  = "msm-pcm-voice",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.be_id = MSM_FRONTEND_DAI_VOLTE,
	},
	{
		.name = "QCHAT",
		.stream_name = "QCHAT",
		.cpu_dai_name   = "QCHAT",
		.platform_name  = "msm-pcm-voice",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.be_id = MSM_FRONTEND_DAI_QCHAT,
	},
	/* LSM FE */
	{/* hw:x,19 */
		.name = "Listen 1 Audio Service",
		.stream_name = "Listen 1 Audio Service",
		.cpu_dai_name = "LSM1",
		.platform_name = "msm-lsm-client",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST },
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.be_id = MSM_FRONTEND_DAI_LSM1,
	},
	{/* hw:x,20 */
		.name = "MSM8226 Compr8",
		.stream_name = "COMPR8",
		.cpu_dai_name   = "MultiMedia8",
		.platform_name  = "msm-compr-dsp",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		/* this dainlink has playback support */
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA8,
	},
	{/* hw:x,21 */
		.name = "Listen 2 Audio Service",
		.stream_name = "Listen 2 Audio Service",
		.cpu_dai_name = "LSM2",
		.platform_name = "msm-lsm-client",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST },
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.be_id = MSM_FRONTEND_DAI_LSM2,
	},
	{/* hw:x,22 */
		.name = "Listen 3 Audio Service",
		.stream_name = "Listen 3 Audio Service",
		.cpu_dai_name = "LSM3",
		.platform_name = "msm-lsm-client",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST },
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.be_id = MSM_FRONTEND_DAI_LSM3,
	},
	{/* hw:x,23 */
		.name = "Listen 4 Audio Service",
		.stream_name = "Listen 4 Audio Service",
		.cpu_dai_name = "LSM4",
		.platform_name = "msm-lsm-client",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST },
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.be_id = MSM_FRONTEND_DAI_LSM4,
	},
	{/* hw:x,24 */
		.name = "Listen 5 Audio Service",
		.stream_name = "Listen 5 Audio Service",
		.cpu_dai_name = "LSM5",
		.platform_name = "msm-lsm-client",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST },
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.be_id = MSM_FRONTEND_DAI_LSM5,
	},
	{/* hw:x,25 */
		.name = "Listen 6 Audio Service",
		.stream_name = "Listen 6 Audio Service",
		.cpu_dai_name = "LSM6",
		.platform_name = "msm-lsm-client",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST },
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.be_id = MSM_FRONTEND_DAI_LSM6,
	},
	{/* hw:x,26 */
		.name = "Listen 7 Audio Service",
		.stream_name = "Listen 7 Audio Service",
		.cpu_dai_name = "LSM7",
		.platform_name = "msm-lsm-client",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST },
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.be_id = MSM_FRONTEND_DAI_LSM7,
	},
	{/* hw:x,27 */
		.name = "Listen 8 Audio Service",
		.stream_name = "Listen 8 Audio Service",
		.cpu_dai_name = "LSM8",
		.platform_name = "msm-lsm-client",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST },
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.be_id = MSM_FRONTEND_DAI_LSM8,
	},
	{/* hw:x,28 */
		.name = "INT_HFP_BT Hostless",
		.stream_name = "INT_HFP_BT Hostless",
		.cpu_dai_name   = "INT_HFP_BT_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* this dai link has playback support */
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{/* hw:x,29 */
		.name = "MSM8226 HFP TX",
		.stream_name = "MultiMedia6",
		.cpu_dai_name = "MultiMedia6",
		.platform_name  = "msm-pcm-loopback",
		.dynamic = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		/* this dai link has playback support */
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA6,
	},
	{/* hw:x,30 */
		.name = "VoWLAN",
		.stream_name = "VoWLAN",
		.cpu_dai_name   = "VoWLAN",
		.platform_name  = "msm-pcm-voice",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.be_id = MSM_FRONTEND_DAI_VOWLAN,
	},
	{
		.name = "SEC_MI2S Hostless",
		.stream_name = "SEC_MI2S Hostless",
		.cpu_dai_name = "SEC_MI2S_HOSTLESS",
		.platform_name = "msm-pcm-hostless",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, /* dai link has playback support */
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "TERT_MI2S Hostless",
		.stream_name = "TERT_MI2S Hostless",
		.cpu_dai_name = "TERT_MI2S_HOSTLESS",
		.platform_name = "msm-pcm-hostless",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, /* dai link has playback support */
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "QUAT_MI2S Hostless",
		.stream_name = "QUAT_MI2S Hostless",
		.cpu_dai_name = "QUAT_MI2S_HOSTLESS",
		.platform_name = "msm-pcm-hostless",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, /* dai link has playback support */
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{/* hw:x,31 */
		.name = "MSM8226 Loopback2",
		.stream_name = "MultiMedia10",
		.cpu_dai_name = "MultiMedia10",
		.platform_name  = "msm-pcm-loopback",
		.dynamic = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		/* this dai link has playback support */
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA10,
	},
	/* Backend BT/FM DAI Links */
	{
		.name = LPASS_BE_INT_BT_SCO_RX,
		.stream_name = "Internal BT-SCO Playback",
		.cpu_dai_name = "msm-dai-q6-dev.12288",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name	= "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_BT_SCO_RX,
		.be_hw_params_fixup = msm_btsco_be_hw_params_fixup,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_INT_BT_SCO_TX,
		.stream_name = "Internal BT-SCO Capture",
		.cpu_dai_name = "msm-dai-q6-dev.12289",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name	= "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_BT_SCO_TX,
		.be_hw_params_fixup = msm_btsco_be_hw_params_fixup,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_INT_FM_RX,
		.stream_name = "Internal FM Playback",
		.cpu_dai_name = "msm-dai-q6-dev.12292",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_FM_RX,
		.be_hw_params_fixup = msm_be_fm_hw_params_fixup,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_INT_FM_TX,
		.stream_name = "Internal FM Capture",
		.cpu_dai_name = "msm-dai-q6-dev.12293",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_FM_TX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ignore_suspend = 1,
	},
	/* Backend AFE DAI Links */
	{
		.name = LPASS_BE_AFE_PCM_RX,
		.stream_name = "AFE Playback",
		.cpu_dai_name = "msm-dai-q6-dev.224",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AFE_PCM_RX,
		.be_hw_params_fixup = msm_proxy_rx_be_hw_params_fixup,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_AFE_PCM_TX,
		.stream_name = "AFE Capture",
		.cpu_dai_name = "msm-dai-q6-dev.225",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AFE_PCM_TX,
		.be_hw_params_fixup = msm_proxy_tx_be_hw_params_fixup,
		.ignore_suspend = 1,
	},
	/* HDMI Hostless */
	{
		.name = "HDMI_RX_HOSTLESS",
		.stream_name = "HDMI_RX_HOSTLESS",
		.cpu_dai_name = "HDMI_HOSTLESS",
		.platform_name = "msm-pcm-hostless",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	/* AUX PCM Backend DAI Links */
	{
		.name = LPASS_BE_AUXPCM_RX,
		.stream_name = "AUX PCM Playback",
		.cpu_dai_name = "msm-dai-q6-auxpcm.1",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AUXPCM_RX,
		.be_hw_params_fixup = msm_auxpcm_be_params_fixup,
		.ops = &msm_auxpcm_be_ops,
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1
		/* this dainlink has playback support */
	},
	{
		.name = LPASS_BE_AUXPCM_TX,
		.stream_name = "AUX PCM Capture",
		.cpu_dai_name = "msm-dai-q6-auxpcm.1",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AUXPCM_TX,
		.be_hw_params_fixup = msm_auxpcm_be_params_fixup,
		.ops = &msm_auxpcm_be_ops,
		.ignore_suspend = 1
	},
	/* Incall Record Uplink BACK END DAI Link */
	{
		.name = LPASS_BE_INCALL_RECORD_TX,
		.stream_name = "Voice Uplink Capture",
		.cpu_dai_name = "msm-dai-q6-dev.32772",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INCALL_RECORD_TX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ignore_suspend = 1,
	},
	/* Incall Record Downlink BACK END DAI Link */
	{
		.name = LPASS_BE_INCALL_RECORD_RX,
		.stream_name = "Voice Downlink Capture",
		.cpu_dai_name = "msm-dai-q6-dev.32771",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INCALL_RECORD_RX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ignore_suspend = 1,
	},
	/* Incall Music BACK END DAI Link */
	{
		.name = LPASS_BE_VOICE_PLAYBACK_TX,
		.stream_name = "Voice Farend Playback",
		.cpu_dai_name = "msm-dai-q6-dev.32773",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_VOICE_PLAYBACK_TX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ignore_suspend = 1,
	},
	/* Incall Music 2 BACK END DAI Link */
	{
		.name = LPASS_BE_VOICE2_PLAYBACK_TX,
		.stream_name = "Voice2 Farend Playback",
		.cpu_dai_name = "msm-dai-q6-dev.32770",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_VOICE2_PLAYBACK_TX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_SEC_MI2S_RX,
		.stream_name = "Secondary MI2S Playback",
		.cpu_dai_name = "msm-dai-q6-mi2s.1",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_SECONDARY_MI2S_RX,
		.be_hw_params_fixup = msm_be_sec_mi2s_hw_params_fixup,
		.ops = &msm8226_sec_mi2s_be_ops,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_SEC_MI2S_TX,
		.stream_name = "Secondary MI2S Capture",
		.cpu_dai_name = "msm-dai-q6-mi2s.1",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_SECONDARY_MI2S_TX,
		.be_hw_params_fixup = msm_be_sec_mi2s_hw_params_fixup,
		.ops = &msm8226_sec_mi2s_be_ops,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_TERT_MI2S_RX,
		.stream_name = "Tertiary MI2S Playback",
		.cpu_dai_name = "msm-dai-q6-mi2s.2",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_TERTIARY_MI2S_RX,
		.be_hw_params_fixup = msm_be_tert_mi2s_hw_params_fixup,
		.ops = &msm8226_tert_mi2s_be_ops,
		.ignore_suspend = 1,
	},

	{
		.name = LPASS_BE_TERT_MI2S_TX,
		.stream_name = "Tertiary MI2S Capture",
		.cpu_dai_name = "msm-dai-q6-mi2s.2",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_TERTIARY_MI2S_TX,
		.be_hw_params_fixup = msm_be_tert_mi2s_hw_params_fixup,
		.ops = &msm8226_tert_mi2s_be_ops,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_QUAT_MI2S_RX,
		.stream_name = "Quaternary MI2S Playback",
		.cpu_dai_name = "msm-dai-q6-mi2s.3",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_QUATERNARY_MI2S_RX,
		.be_hw_params_fixup = msm_be_quat_mi2s_hw_params_fixup,
		.ops = &msm8226_quat_mi2s_be_ops,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_QUAT_MI2S_TX,
		.stream_name = "Quaternary MI2S Capture",
		.cpu_dai_name = "msm-dai-q6-mi2s.3",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_QUATERNARY_MI2S_TX,
		.be_hw_params_fixup = msm_be_quat_mi2s_hw_params_fixup,
		.ops = &msm8226_quat_mi2s_be_ops,
		.ignore_suspend = 1,
	},

};

static struct snd_soc_dai_link msm8226_9306_dai[] = {

};

static struct snd_soc_dai_link msm8226_9302_dai[] = {
	
};

static struct snd_soc_dai_link msm8226_9306_dai_links[
				ARRAY_SIZE(msm8226_common_dai) +
				ARRAY_SIZE(msm8226_9306_dai)];

static struct snd_soc_dai_link msm8226_9302_dai_links[
				ARRAY_SIZE(msm8226_common_dai) +
				ARRAY_SIZE(msm8226_9302_dai)];

struct snd_soc_card snd_soc_card_msm8226 = {
	.name		= "msm8226-tapan-snd-card",
	.dai_link	= msm8226_9306_dai_links,
	.num_links	= ARRAY_SIZE(msm8226_9306_dai_links),
};

struct snd_soc_card snd_soc_card_9302_msm8226 = {
	.name		= "msm8226-tapan9302-snd-card",
	.dai_link	= msm8226_9302_dai_links,
	.num_links	= ARRAY_SIZE(msm8226_9302_dai_links),
};

static int msm8226_dtparse_auxpcm(struct platform_device *pdev,
				struct msm_auxpcm_ctrl **auxpcm_ctrl,
				char *msm_auxpcm_gpio_name[][2])
{
	int ret = 0;
	int i = 0;
	struct msm_auxpcm_gpio *pin_data = NULL;
	struct msm_auxpcm_ctrl *ctrl;
	unsigned int gpio_no[NUM_OF_AUXPCM_GPIOS];
	enum of_gpio_flags flags = OF_GPIO_ACTIVE_LOW;
	int auxpcm_cnt = 0;

	pin_data = devm_kzalloc(&pdev->dev, (ARRAY_SIZE(gpio_no) *
				sizeof(struct msm_auxpcm_gpio)),
				GFP_KERNEL);
	if (!pin_data) {
		dev_err(&pdev->dev, "No memory for gpio\n");
		ret = -ENOMEM;
		goto err;
	}

	for (i = 0; i < ARRAY_SIZE(gpio_no); i++) {
		gpio_no[i] = of_get_named_gpio_flags(pdev->dev.of_node,
				msm_auxpcm_gpio_name[i][DT_PARSE_INDEX],
				0, &flags);

		if (gpio_no[i] > 0) {
			pin_data[i].gpio_name =
			     msm_auxpcm_gpio_name[auxpcm_cnt][GPIO_NAME_INDEX];
			pin_data[i].gpio_no = gpio_no[i];
			dev_dbg(&pdev->dev, "%s:GPIO gpio[%s] =\n"
				"0x%x\n", __func__,
				pin_data[i].gpio_name,
				pin_data[i].gpio_no);
			auxpcm_cnt++;
		} else {
			dev_err(&pdev->dev, "%s:Invalid AUXPCM GPIO[%s]= %x\n",
				 __func__,
				msm_auxpcm_gpio_name[i][GPIO_NAME_INDEX],
				gpio_no[i]);
			ret = -ENODEV;
			goto err;
		}
	}

	ctrl = devm_kzalloc(&pdev->dev,
				sizeof(struct msm_auxpcm_ctrl), GFP_KERNEL);
	if (!ctrl) {
		dev_err(&pdev->dev, "No memory for gpio\n");
		ret = -ENOMEM;
		goto err;
	}

	ctrl->pin_data = pin_data;
	ctrl->cnt = auxpcm_cnt;
	*auxpcm_ctrl = ctrl;
	return ret;

err:
	if (pin_data)
		devm_kfree(&pdev->dev, pin_data);
	return ret;
}

static int msm8226_prepare_codec_mclk(struct snd_soc_card *card)
{
	struct msm8226_asoc_mach_data *pdata = snd_soc_card_get_drvdata(card);
	int ret;
	if (pdata->mclk_gpio) {
		ret = gpio_request(pdata->mclk_gpio, "TAPAN_CODEC_PMIC_MCLK");
		if (ret) {
			dev_err(card->dev,
				"%s: Failed to request tapan mclk gpio %d\n",
				__func__, pdata->mclk_gpio);
			return ret;
		}
	}
	return 0;
}

static bool msm8226_swap_gnd_mic(struct snd_soc_codec *codec)
{
	struct snd_soc_card *card = codec->card;
	struct msm8226_asoc_mach_data *pdata = snd_soc_card_get_drvdata(card);
	int value = gpio_get_value_cansleep(pdata->us_euro_gpio);

	pr_debug("%s: swap select switch %d to %d\n", __func__, value, !value);
	gpio_direction_output(pdata->us_euro_gpio, !value);

	return true;
}

static int msm8226_setup_hs_jack(struct platform_device *pdev,
		struct msm8226_asoc_mach_data *pdata)
{
	int rc;

	pdata->us_euro_gpio = of_get_named_gpio(pdev->dev.of_node,
				"qcom,cdc-us-euro-gpios", 0);
	if (pdata->us_euro_gpio < 0) {
		dev_dbg(&pdev->dev,
			"property %s in node %s not found %d\n",
			"qcom,cdc-us-euro-gpios", pdev->dev.of_node->full_name,
			pdata->us_euro_gpio);
	} else {
		rc = gpio_request(pdata->us_euro_gpio,
						  "TAPAN_CODEC_US_EURO_GPIO");
		if (rc) {
			dev_err(&pdev->dev,
				"%s: Failed to request tapan us-euro gpio %d\n",
				__func__, pdata->us_euro_gpio);
		} else {
			mbhc_cfg.swap_gnd_mic = msm8226_swap_gnd_mic;
		}
	}
	return 0;
}

static struct snd_soc_card *populate_snd_card_dailinks(struct device *dev)
{

	struct snd_soc_card *card;

	if (of_property_read_bool(dev->of_node,
					"qcom,tapan-codec-9302")) {
		card = &snd_soc_card_9302_msm8226;

		memcpy(msm8226_9302_dai_links, msm8226_common_dai,
				sizeof(msm8226_common_dai));
		memcpy(msm8226_9302_dai_links + ARRAY_SIZE(msm8226_common_dai),
			msm8226_9302_dai, sizeof(msm8226_9302_dai));

	} else {

		card = &snd_soc_card_msm8226;

		memcpy(msm8226_9306_dai_links, msm8226_common_dai,
				sizeof(msm8226_common_dai));
		memcpy(msm8226_9306_dai_links + ARRAY_SIZE(msm8226_common_dai),
			msm8226_9306_dai, sizeof(msm8226_9306_dai));
	}

	return card;
}

static int msm8226_asoc_machine_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct msm8226_asoc_mach_data *pdata;
	int ret;
	const char *auxpcm_pri_gpio_set = NULL;
	const char *mbhc_audio_jack_type = NULL;
  
	if (!pdev->dev.of_node) {
		dev_err(&pdev->dev, "No platform supplied from device tree\n");
		return -EINVAL;
	}

	pdata = devm_kzalloc(&pdev->dev,
			sizeof(struct msm8226_asoc_mach_data), GFP_KERNEL);
	if (!pdata) {
		dev_err(&pdev->dev, "Can't allocate msm8226_asoc_mach_data\n");
		return -ENOMEM;
	}
	
	card = populate_snd_card_dailinks(&pdev->dev);

	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, pdata);

	ret = snd_soc_of_parse_card_name(card, "qcom,model");
	if (ret)
		goto err;

	ret = snd_soc_of_parse_audio_routing(card,
			"qcom,audio-routing");
	if (ret)
		goto err;

	ret = of_property_read_u32(pdev->dev.of_node,
			"qcom,tapan-mclk-clk-freq", &pdata->mclk_freq);
	if (ret) {
		dev_err(&pdev->dev, "Looking up %s property in node %s failed",
			"qcom,tapan-mclk-clk-freq",
			pdev->dev.of_node->full_name);
		goto err;
	}

	if (pdata->mclk_freq != 9600000) {
		dev_err(&pdev->dev, "unsupported tapan mclk freq %u\n",
			pdata->mclk_freq);
		ret = -EINVAL;
		goto err;
	}

	pdata->mclk_gpio = of_get_named_gpio(pdev->dev.of_node,
				"qcom,cdc-mclk-gpios", 0);
	if (pdata->mclk_gpio < 0) {
		dev_err(&pdev->dev,
			"Looking up %s property in node %s failed %d\n",
			"qcom, cdc-mclk-gpios", pdev->dev.of_node->full_name,
			pdata->mclk_gpio);
		ret = -ENODEV;
		goto err;
	}
	ret = msm8226_prepare_codec_mclk(card);
	if (ret)
		goto err1;

	mutex_init(&cdc_mclk_mutex);

	mbhc_cfg.gpio_level_insert = of_property_read_bool(pdev->dev.of_node,
					"qcom,headset-jack-type-NC");

	ret = of_property_read_string(pdev->dev.of_node,
		"qcom,mbhc-audio-jack-type", &mbhc_audio_jack_type);
	if (ret) {
		dev_dbg(&pdev->dev, "Looking up %s property in node %s failed",
			"qcom,mbhc-audio-jack-type",
			pdev->dev.of_node->full_name);
		mbhc_cfg.hw_jack_type = FOUR_POLE_JACK;
		mbhc_cfg.enable_anc_mic_detect = false;
		dev_dbg(&pdev->dev, "Jack type properties set to default");
	} else {
		if (!strcmp(mbhc_audio_jack_type, "4-pole-jack")) {
			mbhc_cfg.hw_jack_type = FOUR_POLE_JACK;
			mbhc_cfg.enable_anc_mic_detect = false;
			dev_dbg(&pdev->dev, "This hardware has 4 pole jack");
		} else if (!strcmp(mbhc_audio_jack_type, "5-pole-jack")) {
			mbhc_cfg.hw_jack_type = FIVE_POLE_JACK;
			mbhc_cfg.enable_anc_mic_detect = true;
			dev_dbg(&pdev->dev, "This hardware has 5 pole jack");
		} else if (!strcmp(mbhc_audio_jack_type, "6-pole-jack")) {
			mbhc_cfg.hw_jack_type = SIX_POLE_JACK;
			mbhc_cfg.enable_anc_mic_detect = true;
			dev_dbg(&pdev->dev, "This hardware has 6 pole jack");
		} else {
			mbhc_cfg.hw_jack_type = FOUR_POLE_JACK;
			mbhc_cfg.enable_anc_mic_detect = false;
			dev_dbg(&pdev->dev, "Unknown value, hence setting to default");
		}
	}

	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n",
			ret);
		goto err;
	}
	
	/* Parse AUXPCM info from DT */
	ret = msm8226_dtparse_auxpcm(pdev, &pdata->auxpcm_ctrl,
					msm_auxpcm_gpio_name);
	if (ret) {
		dev_err(&pdev->dev,
		"%s: Auxpcm pin data parse failed\n", __func__);
		goto err;
	}

	vdd_spkr_gpio = of_get_named_gpio(pdev->dev.of_node,
				"qcom,cdc-vdd-spkr-gpios", 0);
	if (vdd_spkr_gpio < 0) {
		dev_dbg(&pdev->dev,
			"Looking up %s property in node %s failed %d\n",
			"qcom, cdc-vdd-spkr-gpios",
			pdev->dev.of_node->full_name, vdd_spkr_gpio);
	} else {
		ret = gpio_request(vdd_spkr_gpio, "TAPAN_CODEC_VDD_SPKR");
		if (ret) {
			/* GPIO to enable EXT VDD exists, but failed request */
			dev_err(card->dev,
					"%s: Failed to request tapan vdd spkr gpio %d\n",
					__func__, vdd_spkr_gpio);
			goto err;
		}
	}

	ext_spk_amp_gpio = of_get_named_gpio(pdev->dev.of_node,
			"qcom,cdc-lineout-spkr-gpios", 0);
	if (ext_spk_amp_gpio < 0) {
		dev_err(&pdev->dev,
			"Looking up %s property in node %s failed %d\n",
			"qcom, cdc-lineout-spkr-gpios",
			pdev->dev.of_node->full_name, ext_spk_amp_gpio);
	} else {
		ret = gpio_request(ext_spk_amp_gpio,
				"TAPAN_CODEC_LINEOUT_SPKR");
		if (ret) {
			/* GPIO to enable EXT AMP exists, but failed request */
			dev_err(card->dev,
				"%s: Failed to request tapan amp spkr gpio %d\n",
				__func__, ext_spk_amp_gpio);
			goto err_vdd_spkr;
		}
	}
	atomic_set(&sec_mi2s_clk.mi2s_rsc_ref, 0);
	atomic_set(&tert_mi2s_clk.mi2s_rsc_ref, 0);
	atomic_set(&quat_mi2s_clk.mi2s_rsc_ref, 0);
    msm8226_setup_hs_jack(pdev, pdata);
	ret = of_property_read_string(pdev->dev.of_node,
			"qcom,prim-auxpcm-gpio-set", &auxpcm_pri_gpio_set);
	if (ret) {
		dev_err(&pdev->dev, "Looking up %s property in node %s failed",
			"qcom,prim-auxpcm-gpio-set",
			pdev->dev.of_node->full_name);
		goto err_lineout_spkr;
	}
	if (!strcmp(auxpcm_pri_gpio_set, "prim-gpio-prim")) {
		lpaif_pri_muxsel_virt_addr = ioremap(LPAIF_PRI_MODE_MUXSEL, 4);
	} else if (!strcmp(auxpcm_pri_gpio_set, "prim-gpio-tert")) {
		lpaif_pri_muxsel_virt_addr = ioremap(LPAIF_TER_MODE_MUXSEL, 4);
	} else {
		dev_err(&pdev->dev, "Invalid value %s for AUXPCM GPIO set\n",
			auxpcm_pri_gpio_set);
		ret = -EINVAL;
		goto err_lineout_spkr;
	}
	if (lpaif_pri_muxsel_virt_addr == NULL) {
		pr_err("%s Pri muxsel virt addr is null\n", __func__);
		ret = -EINVAL;
		goto err_lineout_spkr;
	}

	return 0;

err_lineout_spkr:
	if (ext_spk_amp_gpio >= 0) {
		gpio_free(ext_spk_amp_gpio);
		ext_spk_amp_gpio = -1;
	}

err_vdd_spkr:
	if (vdd_spkr_gpio >= 0) {
		gpio_free(vdd_spkr_gpio);
		vdd_spkr_gpio = -1;
	}

err:
	if (pdata->mclk_gpio > 0) {
		dev_dbg(&pdev->dev, "%s free gpio %d\n",
			__func__, pdata->mclk_gpio);
		gpio_free(pdata->mclk_gpio);
		pdata->mclk_gpio = 0;
	}
err1:
	devm_kfree(&pdev->dev, pdata);
	return ret;
}

static int msm8226_asoc_machine_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct msm8226_asoc_mach_data *pdata = snd_soc_card_get_drvdata(card);

	gpio_free(pdata->mclk_gpio);
	if (vdd_spkr_gpio >= 0)
		gpio_free(vdd_spkr_gpio);
	if (ext_spk_amp_gpio >= 0)
		gpio_free(ext_spk_amp_gpio);
	if (pdata->us_euro_gpio > 0)
		gpio_free(pdata->us_euro_gpio);

	vdd_spkr_gpio = -1;
	ext_spk_amp_gpio = -1;
	snd_soc_unregister_card(card);

	return 0;
}

static const struct of_device_id msm8226_asoc_machine_of_match[]  = {
	{ .compatible = "qcom,msm8226-audio-tapan", },
	{},
};

static struct platform_driver msm8226_asoc_machine_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = msm8226_asoc_machine_of_match,
	},
	.probe = msm8226_asoc_machine_probe,
	.remove = msm8226_asoc_machine_remove,
};
module_platform_driver(msm8226_asoc_machine_driver);

MODULE_DESCRIPTION("ALSA SoC msm");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRV_NAME);
MODULE_DEVICE_TABLE(of, msm8226_asoc_machine_of_match);
