/*
 *  FM Driver for Connectivity chip of Texas Instruments.
 *  This file provides interfaces to V4L2 subsystem.
 *
 *  This module registers with V4L2 subsystem as Radio
 *  data system interface (/dev/radio). During the registration,
 *  it will expose two set of function pointers.
 *
 *    1) File operation related API (open, close, read, write, poll...etc).
 *    2) Set of V4L2 IOCTL complaint API.
 *
 *  Copyright (C) 2011 Texas Instruments
 *  Author: Raja Mani <raja_mani@ti.com>
 *  Author: Manjunatha Halli <manjunatha_halli@ti.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include "fmdrv.h"
#include "fmdrv_v4l2.h"
#include "fmdrv_common.h"
#include "fmdrv_rx.h"
#include "fmdrv_tx.h"

static struct video_device *gradio_dev;
static u8 radio_disconnected;

/* -- V4L2 RADIO (/dev/radioX) device file operation interfaces --- */

/* Read RX RDS data */
static ssize_t fm_v4l2_fops_read(struct file *file, char __user * buf,
					size_t count, loff_t *ppos)
{
	u8 rds_mode;
	int ret;
	struct fmdev *fmdev;
	int no_of_chans;
	fmdev = video_drvdata(file);
	no_of_chans = fmdev->rx.no_of_chans;
	if (!radio_disconnected) {
		fmerr("FM device is already disconnected\n");
		return -EIO;
	}
	if (fmdev->rx.comp_scan_status == 1) {
		fmdev->rx.comp_scan_status = 0;
		memcpy(buf, &fmdev->rx.stat_found[0], 4*fmdev->rx.no_of_chans);

		if (fmdev->rx.rds.pause == 1) {
			fmdev->rx.rds.pause = 0;

			ret = fmc_set_rds_mode(fmdev, FM_RDS_ENABLE);
			if (ret < 0)
				fmerr("Failed to set RX RDS mode\n");
		}

		/* Set back the Original Frequency */
		fmc_set_freq(fmdev, fmdev->rx.freq);

		return 4*fmdev->rx.no_of_chans;
	}
	/* Turn on RDS mode , if it is disabled */
	ret = fm_rx_get_rds_mode(fmdev, &rds_mode);
	if (ret < 0) {
		fmerr("Unable to read current rds mode\n");
		return ret;
	}

	if (rds_mode == FM_RDS_DISABLE) {
		ret = fmc_set_rds_mode(fmdev, FM_RDS_ENABLE);
		if (ret < 0) {
			fmerr("Failed to enable rds mode\n");
			return ret;
		}
	}

	/* Copy RDS data from internal buffer to user buffer */
	return fmc_transfer_rds_from_internal_buff(fmdev, file, buf, count);
}

/* Write TX RDS data */
static ssize_t fm_v4l2_fops_write(struct file *file, const char __user * buf,
		size_t count, loff_t *ppos)
{
	struct tx_rds rds;
	int ret;
	struct fmdev *fmdev;

	ret = copy_from_user(&rds, buf, sizeof(rds));
	rds.text[sizeof(rds.text) - 1] = '\0';
	fmdbg("(%d)type: %d, text %s, af %d\n",
		   ret, rds.text_type, rds.text, rds.af_freq);
	if (ret)
		return -EFAULT;

	fmdev = video_drvdata(file);
	fm_tx_set_radio_text(fmdev, rds.text, rds.text_type);
	fm_tx_set_af(fmdev, rds.af_freq);

	return sizeof(rds);
}

static u32 fm_v4l2_fops_poll(struct file *file, struct poll_table_struct *pts)
{
	int ret;
	struct fmdev *fmdev;
	unsigned int mask = 0;

	fmdev = video_drvdata(file);
	if (fmdev->rx.comp_scan_status == 1) {
		if (fmdev->rx.comp_scan_done == 1) {
			mask |= POLLPRI | POLLIN;
			fmdev->rx.comp_scan_done = 0;
		} else {
			mask = 0;
		}

		return mask;
	}

	mask = 0;
	ret = fmc_is_rds_data_available(fmdev, file, pts);
	if (ret < 0)
		mask = 0;
	else
		mask |= (POLLIN | POLLRDNORM);

	return mask;
}
/**********************************************************************/
/* functions called from sysfs subsystem */

static ssize_t show_fmtx_af(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fmdev *fmdev = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", fmdev->tx_data.af_frq);
}

static ssize_t store_fmtx_af(struct device *dev,
		struct device_attribute *attr, char *buf, size_t size)
{
	int ret;
	unsigned long af_freq;
	struct fmdev *fmdev = dev_get_drvdata(dev);

	if (kstrtoul(buf, 0, &af_freq))
		return -EINVAL;

	ret = fm_tx_set_af(fmdev, af_freq);
	if (ret < 0) {
		fmerr("Failed to set FM TX AF Frequency\n");
		return ret;
	}
	return size;
}
static ssize_t show_fmrx_comp_scan(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fmdev *fmdev = dev_get_drvdata(dev);

	/* Chip doesn't support complete scan for weather band */
	if (fmdev->rx.region.fm_band == FM_BAND_WEATHER)
		return -EINVAL;

	return sprintf(buf, "%d\n", fmdev->rx.no_of_chans);
}

static ssize_t store_fmrx_comp_scan(struct device *dev,
		struct device_attribute *attr, char *buf, size_t size)
{
	int ret;
	unsigned long comp_scan;
	struct fmdev *fmdev = dev_get_drvdata(dev);

	/* Chip doesn't support complete scan for weather band */
	if (fmdev->rx.region.fm_band == FM_BAND_WEATHER)
		return -EINVAL;

	if (kstrtoul(buf, 0, &comp_scan))
		return -EINVAL;

	ret = fm_rx_seek(fmdev, 1, 0, AUTO_SEARCH_SPACING, comp_scan);
	if (ret < 0)
		fmerr("RX complete scan failed - %d\n", ret);

	if (comp_scan == COMP_SCAN_READ)
		return (size_t) fmdev->rx.no_of_chans;
	else
		return size;
}
static ssize_t show_fmrx_deemphasis(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fmdev *fmdev = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", (fmdev->rx.deemphasis_mode ==
				FM_RX_EMPHASIS_FILTER_50_USEC) ? 50 : 75);
}

static ssize_t store_fmrx_deemphasis(struct device *dev,
		struct device_attribute *attr, char *buf, size_t size)
{
	int ret;
	unsigned long deemph_mode;
	struct fmdev *fmdev = dev_get_drvdata(dev);

	printk(KERN_ALERT"fmdrv: store_fmrx_deemphasis buff to write %s\n", buf);

	if (kstrtoul(buf, 0, &deemph_mode))
	{
		printk(KERN_ALERT"fmdrv strict_strtoul failed. deemph_mode is %ld\n", deemph_mode);
		return -EINVAL;
	}

	printk(KERN_ALERT"fmdrv:deemph_mode is %ld\n", deemph_mode);

	if (deemph_mode != 50 && deemph_mode != 75)
	{
		printk(KERN_ALERT"Invalid deemph_mode\n");
		return -EINVAL;
	}

	if (deemph_mode == 50)
		deemph_mode = FM_RX_EMPHASIS_FILTER_50_USEC;
	else
		deemph_mode = FM_RX_EMPHASIS_FILTER_75_USEC;

	ret = fm_rx_set_deemphasis_mode(fmdev, deemph_mode);
	if (ret < 0) {
		fmerr("Failed to set De-emphasis Mode\n");
		return ret;
	}

	return size;
}

static ssize_t show_fmrx_af(struct device *dev,
                struct device_attribute *attr, char *buf)
{
	struct fmdev *fmdev = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", fmdev->rx.af_mode);
}

static ssize_t store_fmrx_af(struct device *dev,
		struct device_attribute *attr, char *buf, size_t size)
{
	int ret;
	unsigned long af_mode;
	struct fmdev *fmdev = dev_get_drvdata(dev);

	if (kstrtoul(buf, 0, &af_mode))
		return -EINVAL;

	if (af_mode < 0 || af_mode > 1)
		return -EINVAL;

	ret = fm_rx_set_af_switch(fmdev, af_mode);
	if (ret < 0) {
		fmerr("Failed to set AF Switch\n");
		return ret;
	}

	return size;
}

static ssize_t show_fmrx_band(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fmdev *fmdev = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", fmdev->rx.region.fm_band);
}

static ssize_t store_fmrx_band(struct device *dev,
		struct device_attribute *attr, char *buf, size_t size)
{
	int ret;
	unsigned long fm_band;
	struct fmdev *fmdev = dev_get_drvdata(dev);

	if (kstrtoul(buf, 0, &fm_band))
		return -EINVAL;
	if (fm_band < FM_BAND_EUROPE_US || fm_band > FM_BAND_WEATHER)
		return -EINVAL;

	ret = fm_rx_set_region(fmdev, fm_band);
	if (ret < 0) {
		fmerr("Failed to set FM Band\n");
		return ret;
	}

	return size;
}

static ssize_t show_fmrx_rssi_lvl(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fmdev *fmdev = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", fmdev->rx.rssi_threshold);
}
static ssize_t store_fmrx_rssi_lvl(struct device *dev,
		struct device_attribute *attr, char *buf, size_t size)
{
	int ret;
	unsigned long rssi_lvl;
	struct fmdev *fmdev = dev_get_drvdata(dev);

	if (kstrtoul(buf, 0, &rssi_lvl))
		return -EINVAL;

	ret = fm_rx_set_rssi_threshold(fmdev, rssi_lvl);
	if (ret < 0) {
		fmerr("Failed to set RSSI level\n");
		return ret;
	}

	return size;
}

/* structures specific for sysfs entries */
static struct kobj_attribute v4l2_fmtx_rds_af =
__ATTR(fmtx_rds_af, 0666, (void *)show_fmtx_af, (void *)store_fmtx_af);
/* To start FM RX complete scan*/
static struct kobj_attribute v4l2_fmrx_comp_scan =
__ATTR(fmrx_comp_scan, 0666, (void *)show_fmrx_comp_scan,
		(void *)store_fmrx_comp_scan);
/* To Set De-Emphasis filter mode */
static struct kobj_attribute v4l2_fmrx_deemph_mode =
__ATTR(fmrx_deemph_mode, 0666, (void *)show_fmrx_deemphasis,
		(void *)store_fmrx_deemphasis);

/* To Enable/Disable FM RX RDS AF feature */
static struct kobj_attribute v4l2_fmrx_rds_af =
__ATTR(fmrx_rds_af, 0666, (void *)show_fmrx_af, (void *)store_fmrx_af);

/* To switch between Japan/US bands */
static struct kobj_attribute v4l2_fmrx_band =
__ATTR(fmrx_band, 0666, (void *)show_fmrx_band, (void *)store_fmrx_band);

/* To set the desired FM reception RSSI level */
static struct kobj_attribute v4l2_fmrx_rssi_lvl =
__ATTR(fmrx_rssi_lvl, 0666, (void *) show_fmrx_rssi_lvl,
		(void *)store_fmrx_rssi_lvl);

static struct attribute *v4l2_fm_attrs[] = {
	&v4l2_fmtx_rds_af.attr,
	&v4l2_fmrx_comp_scan.attr,
	&v4l2_fmrx_deemph_mode.attr,
	&v4l2_fmrx_rds_af.attr,
	&v4l2_fmrx_band.attr,
	&v4l2_fmrx_rssi_lvl.attr,
	NULL,
};
static struct attribute_group v4l2_fm_attr_grp = {
	.attrs = v4l2_fm_attrs,
};
/*
 * Handle open request for "/dev/radioX" device.
 * Start with FM RX mode as default.
 */
static int fm_v4l2_fops_open(struct file *file)
{
	int ret;
	struct fmdev *fmdev = NULL;

	/* Don't allow multiple open */
	if (radio_disconnected) {
		fmerr("FM device is already opened\n");
		return -EBUSY;
	}

	fmdev = video_drvdata(file);

	ret = fmc_prepare(fmdev);
	if (ret < 0) {
		fmerr("Unable to prepare FM CORE\n");
		return ret;
	}

	fmdbg("Load FM RX firmware..\n");

	ret = fmc_set_mode(fmdev, FM_MODE_RX);
	if (ret < 0) {
		fmerr("Unable to load FM RX firmware\n");
		return ret;
	}
	radio_disconnected = 1;
	/* Register sysfs entries */
	ret = sysfs_create_group(&fmdev->radio_dev->dev.kobj, &v4l2_fm_attr_grp);
	if (ret) {
		pr_err("failed to create sysfs entries");
		return ret;
	}
	return ret;
}

static int fm_v4l2_fops_release(struct file *file)
{
	int ret;
	struct fmdev *fmdev;

	fmdev = video_drvdata(file);
	if (fmdev->rx.comp_scan_status == 1) {
		if (fmdev->rx.comp_scan_done == 0) {
			ret = fm_rx_seek(fmdev, 1, 0, FM_CHANNEL_SPACING_200KHZ,
					COMP_SCAN_STOP);
			if (ret < 0)
				fmerr("RX complete scan failed - %d\n", ret);
		}
	}
	if (!radio_disconnected) {
		fmdbg("FM device is already closed\n");
		return 0;
	}

	ret = fmc_set_mode(fmdev, FM_MODE_OFF);
	if (ret < 0) {
		fmerr("Unable to turn off the chip\n");
		return ret;
	}
	sysfs_remove_group(&fmdev->radio_dev->dev.kobj, &v4l2_fm_attr_grp);
	ret = fmc_release(fmdev);
	if (ret < 0) {
		fmerr("FM CORE release failed\n");
		return ret;
	}
	radio_disconnected = 0;

	return ret;
}

/* V4L2 RADIO (/dev/radioX) device IOCTL interfaces */
static int fm_v4l2_vidioc_querycap(struct file *file, void *priv,
		struct v4l2_capability *capability)
{
	strlcpy(capability->driver, FM_DRV_NAME, sizeof(capability->driver));
	strlcpy(capability->card, FM_DRV_CARD_SHORT_NAME,
			sizeof(capability->card));
	sprintf(capability->bus_info, "UART");
//	capability->version = FM_DRV_RADIO_VERSION;
	capability->capabilities = V4L2_CAP_HW_FREQ_SEEK | V4L2_CAP_TUNER |
		V4L2_CAP_RADIO | V4L2_CAP_MODULATOR |
		V4L2_CAP_AUDIO | V4L2_CAP_READWRITE |
		V4L2_CAP_RDS_CAPTURE;

	return 0;
}

static int fm_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct fmdev *fmdev = container_of(ctrl->handler,
			struct fmdev, ctrl_handler);

	switch (ctrl->id) {
	case  V4L2_CID_TUNE_ANTENNA_CAPACITOR:
		ctrl->val = fm_tx_get_tune_cap_val(fmdev);
		break;
	default:
		fmwarn("%s: Unknown IOCTL: %d\n", __func__, ctrl->id);
		break;
	}

	return 0;
}

static int fm_v4l2_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct fmdev *fmdev = container_of(ctrl->handler,
			struct fmdev, ctrl_handler);
	int ret;
	switch (ctrl->id) {
	case V4L2_CID_AUDIO_VOLUME:	/* set volume */
		return fm_rx_set_volume(fmdev, (u16)ctrl->val);

	case V4L2_CID_AUDIO_MUTE:	/* set mute */
		return fmc_set_mute_mode(fmdev, (u8)ctrl->val);

	case V4L2_CID_TUNE_POWER_LEVEL:
		/* set TX power level - ext control */
		return fm_tx_set_pwr_lvl(fmdev, (u8)ctrl->val);

	case V4L2_CID_TUNE_PREEMPHASIS:
		return fm_tx_set_preemph_filter(fmdev, (u8) ctrl->val);
	case V4L2_CID_RDS_TX_PI:
		ret = set_rds_picode(fmdev, ctrl->val);
		if (ret < 0) {
			fmerr("Failed to set RDS Radio PS Name\n");
			return ret;
		}
		return 0;

	case V4L2_CID_RDS_TX_PTY:
		ret = set_rds_pty(fmdev, ctrl->val);
		if (ret < 0) {
			fmerr("Failed to set RDS Radio PS Name\n");
			return ret;
		}
		return 0;

	case V4L2_CID_RDS_TX_PS_NAME:
		ret = fm_tx_set_radio_text(fmdev, ctrl->string, 1);
		if (ret < 0) {
			fmerr("Failed to set RDS Radio PS Name\n");
			return ret;
		}
		return 0;

	case V4L2_CID_RDS_TX_RADIO_TEXT:
		ret = fm_tx_set_radio_text(fmdev, ctrl->string, 2);
		if (ret < 0) {
			fmerr("Failed to set RDS Radio Text\n");
			return ret;
		}
		return 0;
	default:
		return -EINVAL;
	}
}

static int fm_v4l2_vidioc_g_audio(struct file *file, void *priv,
		struct v4l2_audio *audio)
{
	memset(audio, 0, sizeof(*audio));
	strcpy(audio->name, "Radio");
	audio->capability = V4L2_AUDCAP_STEREO;

	return 0;
}

static int fm_v4l2_vidioc_s_audio(struct file *file, void *priv,
		struct v4l2_audio *audio)
{
	if (audio->index != 0)
		return -EINVAL;

	return 0;
}

/* Get tuner attributes. If current mode is NOT RX, return error */
static int fm_v4l2_vidioc_g_tuner(struct file *file, void *priv,
		struct v4l2_tuner *tuner)
{
	struct fmdev *fmdev = video_drvdata(file);
	u32 bottom_freq;
	u32 top_freq;
	u16 stereo_mono_mode;
	u16 rssilvl;
	int ret;

	if (tuner->index != 0)
		return -EINVAL;

	if (fmdev->curr_fmmode != FM_MODE_RX)
		return -EPERM;

	ret = fm_rx_get_band_freq_range(fmdev, &bottom_freq, &top_freq);
	if (ret != 0)
		return ret;

	ret = fm_rx_get_stereo_mono(fmdev, &stereo_mono_mode);
	if (ret != 0)
		return ret;

	ret = fm_rx_get_rssi_level(fmdev, &rssilvl);
	if (ret != 0)
		return ret;

	strcpy(tuner->name, "FM");
	tuner->type = V4L2_TUNER_RADIO;
	/* Store rangelow and rangehigh freq in unit of 62.5 Hz */
	tuner->rangelow = bottom_freq * 16;
	tuner->rangehigh = top_freq * 16;
	tuner->rxsubchans = V4L2_TUNER_SUB_MONO | V4L2_TUNER_SUB_STEREO |
	((fmdev->rx.rds.flag == FM_RDS_ENABLE) ? V4L2_TUNER_SUB_RDS : 0);
	tuner->capability = V4L2_TUNER_CAP_STEREO | V4L2_TUNER_CAP_RDS |
			    V4L2_TUNER_CAP_LOW;
	tuner->audmode = (stereo_mono_mode ?
			  V4L2_TUNER_MODE_MONO : V4L2_TUNER_MODE_STEREO);

	/*
	 * Actual rssi value lies in between -128 to +127.
	 * Convert this range from 0 to 255 by adding +128
	 */
	rssilvl += 128;

	/*
	 * Return signal strength value should be within 0 to 65535.
	 * Find out correct signal radio by multiplying (65535/255) = 257
	 */
	tuner->signal = rssilvl * 257;
	tuner->afc = 0;

	return ret;
}

/*
 * Set tuner attributes. If current mode is NOT RX, set to RX.
 * Currently, we set only audio mode (mono/stereo) and RDS state (on/off).
 * Should we set other tuner attributes, too?
 */
static int fm_v4l2_vidioc_s_tuner(struct file *file, void *priv,
		struct v4l2_tuner *tuner)
{
	struct fmdev *fmdev = video_drvdata(file);
	u16 aud_mode;
	u8 rds_mode;
	int ret;

	if (tuner->index != 0)
		return -EINVAL;

	aud_mode = (tuner->audmode == V4L2_TUNER_MODE_STEREO) ?
			FM_STEREO_MODE : FM_MONO_MODE;
	rds_mode = (tuner->rxsubchans & V4L2_TUNER_SUB_RDS) ?
			FM_RDS_ENABLE : FM_RDS_DISABLE;

	if (fmdev->curr_fmmode != FM_MODE_RX) {
		ret = fmc_set_mode(fmdev, FM_MODE_RX);
		if (ret < 0) {
			fmerr("Failed to set RX mode\n");
			return ret;
		}
	}

	ret = fmc_set_stereo_mono(fmdev, aud_mode);
	if (ret < 0) {
		fmerr("Failed to set RX stereo/mono mode\n");
		return ret;
	}

	ret = fmc_set_rds_mode(fmdev, rds_mode);
	if (ret < 0)
		fmerr("Failed to set RX RDS mode\n");

	return ret;
}

/* Get tuner or modulator radio frequency */
static int fm_v4l2_vidioc_g_freq(struct file *file, void *priv,
		struct v4l2_frequency *freq)
{
	struct fmdev *fmdev = video_drvdata(file);
	int ret;

	ret = fmc_get_freq(fmdev, &freq->frequency);
	if (ret < 0) {
		fmerr("Failed to get frequency\n");
		return ret;
	}

	/* Frequency unit of 62.5 Hz*/
	freq->frequency = (u32) freq->frequency * 16;

	return 0;
}

/* Set tuner or modulator radio frequency */
static int fm_v4l2_vidioc_s_freq(struct file *file, void *priv,
		struct v4l2_frequency *freq)
{
	struct fmdev *fmdev = video_drvdata(file);

	/*
	 * As V4L2_TUNER_CAP_LOW is set 1 user sends the frequency
	 * in units of 62.5 Hz.
	 */
	freq->frequency = (u32)(freq->frequency / 16);

	return fmc_set_freq(fmdev, freq->frequency);
}

/* Set hardware frequency seek. If current mode is NOT RX, set it RX. */
static int fm_v4l2_vidioc_s_hw_freq_seek(struct file *file, void *priv,
		struct v4l2_hw_freq_seek *seek)
{
	struct fmdev *fmdev = video_drvdata(file);
	int ret;

	if (fmdev->curr_fmmode != FM_MODE_RX) {
		ret = fmc_set_mode(fmdev, FM_MODE_RX);
		if (ret != 0) {
			fmerr("Failed to set RX mode\n");
			return ret;
		}
	}

	ret = fm_rx_seek(fmdev, seek->seek_upward, seek->wrap_around,
			seek->spacing, SEEK_START);
	if (ret < 0)
		fmerr("RX seek failed - %d\n", ret);

	return ret;
}
/* Get modulator attributes. If mode is not TX, return no attributes. */
static int fm_v4l2_vidioc_g_modulator(struct file *file, void *priv,
		struct v4l2_modulator *mod)
{
	struct fmdev *fmdev = video_drvdata(file);;

	if (mod->index != 0)
		return -EINVAL;

	if (fmdev->curr_fmmode != FM_MODE_TX)
		return -EPERM;

	mod->txsubchans = ((fmdev->tx_data.aud_mode == FM_STEREO_MODE) ?
				V4L2_TUNER_SUB_STEREO : V4L2_TUNER_SUB_MONO) |
				((fmdev->tx_data.rds.flag == FM_RDS_ENABLE) ?
				V4L2_TUNER_SUB_RDS : 0);

	mod->capability = V4L2_TUNER_CAP_STEREO | V4L2_TUNER_CAP_RDS |
				V4L2_TUNER_CAP_LOW;

	return 0;
}

/* Set modulator attributes. If mode is not TX, set to TX. */
static int fm_v4l2_vidioc_s_modulator(struct file *file, void *priv,
		struct v4l2_modulator *mod)
{
	struct fmdev *fmdev = video_drvdata(file);
	u8 rds_mode;
	u16 aud_mode;
	int ret;

	if (mod->index != 0)
		return -EINVAL;

	if (fmdev->curr_fmmode != FM_MODE_TX) {
		ret = fmc_set_mode(fmdev, FM_MODE_TX);
		if (ret != 0) {
			fmerr("Failed to set TX mode\n");
			return ret;
		}
	}

	aud_mode = (mod->txsubchans & V4L2_TUNER_SUB_STEREO) ?
			FM_STEREO_MODE : FM_MONO_MODE;
	rds_mode = (mod->txsubchans & V4L2_TUNER_SUB_RDS) ?
			FM_RDS_ENABLE : FM_RDS_DISABLE;
	ret = fm_tx_set_stereo_mono(fmdev, aud_mode);
	if (ret < 0) {
		fmerr("Failed to set mono/stereo mode for TX\n");
		return ret;
	}
	ret = fm_tx_set_rds_mode(fmdev, rds_mode);
	if (ret < 0)
		fmerr("Failed to set rds mode for TX\n");

	return ret;
}

static const struct v4l2_file_operations fm_drv_fops = {
	.owner = THIS_MODULE,
	.read = fm_v4l2_fops_read,
	.write = fm_v4l2_fops_write,
	.poll = fm_v4l2_fops_poll,
	.unlocked_ioctl = video_ioctl2,
	.open = fm_v4l2_fops_open,
	.release = fm_v4l2_fops_release,
};

static const struct v4l2_ctrl_ops fm_ctrl_ops = {
	.s_ctrl = fm_v4l2_s_ctrl,
	.g_volatile_ctrl = fm_g_volatile_ctrl,
};
static const struct v4l2_ioctl_ops fm_drv_ioctl_ops = {
	.vidioc_querycap = fm_v4l2_vidioc_querycap,
	.vidioc_g_audio = fm_v4l2_vidioc_g_audio,
	.vidioc_s_audio = fm_v4l2_vidioc_s_audio,
	.vidioc_g_tuner = fm_v4l2_vidioc_g_tuner,
	.vidioc_s_tuner = fm_v4l2_vidioc_s_tuner,
	.vidioc_g_frequency = fm_v4l2_vidioc_g_freq,
	.vidioc_s_frequency = fm_v4l2_vidioc_s_freq,
	.vidioc_s_hw_freq_seek = fm_v4l2_vidioc_s_hw_freq_seek,
	.vidioc_g_modulator = fm_v4l2_vidioc_g_modulator,
	.vidioc_s_modulator = fm_v4l2_vidioc_s_modulator
};

/* V4L2 RADIO device parent structure */
static struct video_device fm_viddev_template = {
	.fops = &fm_drv_fops,
	.ioctl_ops = &fm_drv_ioctl_ops,
	.name = FM_DRV_NAME,
	.release = video_device_release,
};

int fm_v4l2_init_video_device(struct fmdev *fmdev, int radio_nr)
{
	struct v4l2_ctrl *ctrl;
	int ret;

	/* Init mutex for core locking */
	mutex_init(&fmdev->mutex);

	/* Allocate new video device */
	gradio_dev = video_device_alloc();
	if (NULL == gradio_dev) {
		fmerr("Can't allocate video device\n");
		return -ENOMEM;
	}

	/* Setup FM driver's V4L2 properties */
	memcpy(gradio_dev, &fm_viddev_template, sizeof(fm_viddev_template));

	video_set_drvdata(gradio_dev, fmdev);

	gradio_dev->lock = &fmdev->mutex;

	/* Register with V4L2 subsystem as RADIO device */
	if (video_register_device(gradio_dev, VFL_TYPE_RADIO, radio_nr)) {
		video_device_release(gradio_dev);
		fmerr("Could not register video device\n");
		return -ENOMEM;
	}

	fmdev->radio_dev = gradio_dev;

	/* Register to v4l2 ctrl handler framework */
	fmdev->radio_dev->ctrl_handler = &fmdev->ctrl_handler;

	ret = v4l2_ctrl_handler_init(&fmdev->ctrl_handler, 5);
	if (ret < 0) {
		fmerr("(fmdev): Can't init ctrl handler\n");
		v4l2_ctrl_handler_free(&fmdev->ctrl_handler);
		return -EBUSY;
	}

	/*
	 * Following controls are handled by V4L2 control framework.
	 * Added in ascending ID order.
	 */
	v4l2_ctrl_new_std(&fmdev->ctrl_handler, &fm_ctrl_ops,
			V4L2_CID_AUDIO_VOLUME, FM_RX_VOLUME_MIN,
			FM_RX_VOLUME_MAX, 1, FM_RX_VOLUME_MAX);
	v4l2_ctrl_new_std(&fmdev->ctrl_handler, &fm_ctrl_ops,
			V4L2_CID_AUDIO_MUTE, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&fmdev->ctrl_handler, &fm_ctrl_ops,
			V4L2_CID_RDS_TX_PI, 0x0, 0xf, 1, 0x0);

	v4l2_ctrl_new_std(&fmdev->ctrl_handler, &fm_ctrl_ops,
			V4L2_CID_RDS_TX_PTY, 0, 32, 1, 0);

	v4l2_ctrl_new_std(&fmdev->ctrl_handler, &fm_ctrl_ops,
			V4L2_CID_RDS_TX_PS_NAME, 0, 0xffff, 1, 0);

	v4l2_ctrl_new_std(&fmdev->ctrl_handler, &fm_ctrl_ops,
			V4L2_CID_RDS_TX_RADIO_TEXT, 0, 0xffff, 1, 0);
	v4l2_ctrl_new_std_menu(&fmdev->ctrl_handler, &fm_ctrl_ops,
			V4L2_CID_TUNE_PREEMPHASIS, V4L2_PREEMPHASIS_75_uS,
			0, V4L2_PREEMPHASIS_75_uS);

	v4l2_ctrl_new_std(&fmdev->ctrl_handler, &fm_ctrl_ops,
			V4L2_CID_TUNE_POWER_LEVEL, FM_PWR_LVL_LOW,
			FM_PWR_LVL_HIGH, 1, FM_PWR_LVL_HIGH);

	ctrl = v4l2_ctrl_new_std(&fmdev->ctrl_handler, &fm_ctrl_ops,
			V4L2_CID_TUNE_ANTENNA_CAPACITOR, 0,
			255, 1, 255);

	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;

	return 0;
}

void *fm_v4l2_deinit_video_device(void)
{
	struct fmdev *fmdev;


	fmdev = video_get_drvdata(gradio_dev);

	/* Unregister to v4l2 ctrl handler framework*/
	v4l2_ctrl_handler_free(&fmdev->ctrl_handler);

	/* Unregister RADIO device from V4L2 subsystem */
	video_unregister_device(gradio_dev);

	return fmdev;
}
