// SPDX-License-Identifier: GPL-2.0
/*
 * mz0380-hdmitx.c - ITE IT66121 HDMI transmitter (the card's HDMI OUT port).
 *
 * The board's passthrough output is not a passive splitter: the second device
 * on the card's I2C bus (8-bit 0x98, M83/M127c) is an IT66121 transmitter that
 * powers up with video and audio in software reset, its output driver in
 * reset and AV mute set. Nothing ever wrote it, so a monitor on HDMI OUT saw
 * no signal at all. A read-only dump on hardware (rev 1, subsys 1cfa:0006)
 * showed exactly that state: 0x04=0x1c, 0x61=0x10, 0xc1=0x81, 0xc0=0x00, with
 * the downstream monitor's HPD already visible in 0x0e.
 *
 * The register sequence follows the mainline IT66121 bridge driver
 * (drivers/gpu/drm/bridge/ite-it66121.c): attach-time power-up, then the
 * set_mode input/AFE configuration, then unmute. What mainline cannot tell us
 * is this board's input bus. The Windows driver writes an 18-byte BT.709
 * YCbCr->RGB matrix to 0x98 at 0x76 (RE_FINDINGS M83 notes, decoded constants
 * +1.5396 / -0.1831 / -0.4595 / +1.8159), so the transmitter is fed YCbCr and
 * converts to RGB. Settled on hardware by sweeping 0x70/0x71 live:
 *
 *  - 16-bit 4:2:2 with SEPARATE syncs. Embedded sync (0x70 bit 3) never
 *    reaches vid_stable, even though the receiver's capture path uses it.
 *  - Non-sequential IO mode (0x71 bit 2, Reg_TxIOMod): Y on D8-D15, C on
 *    D16-D23, i.e. the 16 bits are wired contiguously. Sequential mode gives
 *    a fuzzy, pink/white false-colour picture.
 *  - Input PCLK delay (0x70 bits 1:0, Reg_InClkDly) 1 or 2 is clean at
 *    1080p60; 0 and 3 are not.
 *
 * Everything here goes through the mailbox I2C proxy (op 0x1a/0x1b) and never
 * touches the encoder, so it costs no SET_VIC spawns and can be retried.
 */

#include <linux/delay.h>
#include <linux/hdmi.h>
#include <linux/math64.h>
#include <linux/mutex.h>

#include "mz0380.h"
#include "mz0380-reg.h"

#define IT66121_I2C_DEV			0x98

#define IT66121_VENDOR_ID		0x4954
#define IT66121_DEVICE_ID		0x0612

#define IT66121_SW_RST_REG		0x04
#define IT66121_SW_RST_REF		BIT(5)
#define IT66121_SW_RST_AREF		BIT(4)
#define IT66121_SW_RST_VID		BIT(3)
#define IT66121_SW_RST_AUD		BIT(2)
#define IT66121_INT_REG			0x05
#define IT66121_INT_TX_CLK_OFF		BIT(0)
#define IT66121_SYS_STATUS_REG		0x0e
#define IT66121_SYS_STATUS_HPDETECT	BIT(6)
#define IT66121_SYS_STATUS_RXSENSE	BIT(5)
#define IT66121_SYS_STATUS_VID_STABLE	BIT(4)
#define IT66121_CLK_BANK_REG		0x0f
#define IT66121_CLK_BANK_PWROFF_RCLK	BIT(6)
#define IT66121_CLK_BANK_PWROFF_TXCLK	BIT(4)
#define IT66121_CLK_BANK_SEL		BIT(0)
#define IT66121_AFE_DRV_REG		0x61
#define IT66121_AFE_DRV_RST		BIT(4)
#define IT66121_AFE_DRV_PWD		BIT(5)
#define IT66121_AFE_XP_REG		0x62
#define IT66121_AFE_XP_GAINBIT		BIT(7)
#define IT66121_AFE_XP_PWDPLL		BIT(6)
#define IT66121_AFE_XP_ENO		BIT(4)
#define IT66121_AFE_XP_RESETB		BIT(3)
#define IT66121_AFE_XP_PWDI		BIT(2)
#define IT66121_AFE_IP_REG		0x64
#define IT66121_AFE_IP_GAINBIT		BIT(7)
#define IT66121_AFE_IP_PWDPLL		BIT(6)
#define IT66121_AFE_IP_ER0		BIT(3)
#define IT66121_AFE_IP_RESETB		BIT(2)
#define IT66121_AFE_IP_EC1		BIT(0)
#define IT66121_CLK_CTRL0_REG		0x58
#define IT66121_CLK_CTRL0_AUTO_OVER_SAMPLING	BIT(4)
#define IT66121_CLK_CTRL0_EXT_MCLK_SHIFT	2
#define IT66121_CLK_CTRL0_AUTO_IPCLK	BIT(0)
#define IT66121_AFE_XP_EC1_REG		0x68
#define IT66121_AFE_XP_EC1_LOWCLK	BIT(4)
#define IT66121_AFE_CLK_HIGH_KHZ	80000
#define IT66121_INPUT_MODE_REG		0x70
#define IT66121_INPUT_MODE_YUV422	BIT(6)
#define IT66121_INPUT_MODE_SYNCEMB	BIT(3)
#define IT66121_TXFIFO_REG		0x71
#define IT66121_TXFIFO_IOMOD_NONSEQ	BIT(2)
#define IT66121_TXFIFO_RST		BIT(1)
#define IT66121_INPUT_CSC_REG		0x72
#define IT66121_INPUT_CSC_ENUDFILTER	BIT(6)
#define IT66121_INPUT_CSC_YUV_TO_RGB	0x03
#define IT66121_CSC_YOFF_REG		0x73
#define IT66121_CSC_MTX_REG		0x76
#define IT66121_HDMI_MODE_REG		0xc0
#define IT66121_HDMI_MODE_HDMI		BIT(0)
#define IT66121_AV_MUTE_REG		0xc1
#define IT66121_AV_MUTE_ON		BIT(0)
#define IT66121_PKT_CTS_CTRL_REG	0xc5
#define IT66121_PKT_CTS_CTRL_SW		BIT(1)
#define IT66121_PKT_GEN_CTRL_REG	0xc6
#define IT66121_PKT_GEN_CTRL_ON		BIT(0)
#define IT66121_PKT_GEN_CTRL_RPT	BIT(1)
#define IT66121_AVI_INFO_PKT_REG	0xcd
#define IT66121_AVI_INFO_PKT_ON		BIT(0)
#define IT66121_AVI_INFO_PKT_RPT	BIT(1)
#define IT66121_AUD_INFO_PKT_REG	0xce
#define IT66121_AUD_CTRL0_REG		0xe0
#define IT66121_AUD_CTRL0_24BIT		(3 << 6)
#define IT66121_AUD_CTRL0_SPDIF		BIT(4)
#define IT66121_AUD_CTRL0_EN_I2S0	BIT(0)
#define IT66121_AUD_CTRL1_REG		0xe1
#define IT66121_AUD_FIFOMAP_REG		0xe2
#define IT66121_AUD_CTRL3_REG		0xe3
#define IT66121_AUD_SRCVALID_FLAT_REG	0xe4
#define IT66121_AUD_HDAUDIO_REG		0xe5
#define IT66121_KEY_REG			0xf8	/* unlocks 0xc5: c3, a5 ... ff */
/* bank 1 */
#define IT66121_AUD_PKT_N0_REG		0x133
#define IT66121_AVIINFO_DB1_REG		0x158
#define IT66121_AVIINFO_CSUM_REG	0x15d
#define IT66121_AVIINFO_DB6_REG		0x15e
#define IT66121_AUD_INFO_DB1_REG	0x168
#define IT66121_AUD_INFO_CSUM_REG	0x16d
#define IT66121_AUD_CHST_MODE_REG	0x191
#define IT66121_AUD_CHST_CAT_REG	0x192
#define IT66121_AUD_CHST_SRCNUM_REG	0x193
#define IT66121_AUD_CHST_CHTNUM_REG	0x194
#define IT66121_AUD_CHST_CA_FS_REG	0x198
#define IT66121_AUD_CHST_OFS_WL_REG	0x199
#define IT66121_AUD_PKT_CTS_CNT0_REG	0x1a0

/* 2ch 48 kHz LPCM: what the receiver's RxAudioInit (I2S, 48 kHz) delivers */
#define IT66121_AUD_FS_48K		0x2
#define IT66121_AUD_SWL_24BIT		0xb
#define IT66121_AUD_N_48K		6144

/*
 * BT.709 YCbCr -> RGB, limited range in and out, 14-bit two's-complement Q11,
 * little-endian. Rows are the G, R, B outputs; columns Y, Cr, Cb - the order
 * in which the decoded Windows constants land (G row carries both negative
 * chroma terms, R only Cr, B only Cb). Limited-range RGB is what the AVI
 * infoframe below declares.
 */
static const u8 it66121_csc_709_limited[18] = {
	0x00, 0x08, 0x57, 0x3c, 0x89, 0x3e,	/* G = Y - 0.4577 Cr - 0.1831 Cb */
	0x00, 0x08, 0x51, 0x0c, 0x00, 0x00,	/* R = Y + 1.5396 Cr */
	0x00, 0x08, 0x00, 0x00, 0x83, 0x0e,	/* B = Y + 1.8142 Cb */
};

static DEFINE_MUTEX(it66121_lock);

static int tx_bank(struct mz0380_dev *dev, unsigned int bank)
{
	u32 v = 0;
	int ret = mz0380_periph_read(dev, IT66121_I2C_DEV,
				     IT66121_CLK_BANK_REG, &v);

	if (ret)
		return ret;
	v = (v & ~IT66121_CLK_BANK_SEL) | (bank ? IT66121_CLK_BANK_SEL : 0);
	return mz0380_periph_write(dev, IT66121_I2C_DEV, IT66121_CLK_BANK_REG,
				   v & 0xff);
}

/* Registers 0x100-0x1ff are bank 1; the caller restores bank 0. */
static int tx_wr(struct mz0380_dev *dev, u16 reg, u8 val)
{
	return mz0380_periph_write(dev, IT66121_I2C_DEV, reg & 0xff, val);
}

static int tx_rd(struct mz0380_dev *dev, u16 reg, u8 *val)
{
	u32 v = 0;
	int ret = mz0380_periph_read(dev, IT66121_I2C_DEV, reg & 0xff, &v);

	if (ret)
		return ret;
	*val = v & 0xff;
	return 0;
}

static int tx_update(struct mz0380_dev *dev, u16 reg, u8 mask, u8 val)
{
	u8 v;
	int ret = tx_rd(dev, reg, &v);

	if (ret)
		return ret;
	return tx_wr(dev, reg, (v & ~mask) | (val & mask));
}

#define TX_TRY(_op) do { \
		ret = (_op); \
		if (ret) \
			goto out; \
	} while (0)

/* mainline it66121_bridge_attach(): clocks, PLLs and the output driver up */
static int tx_power_up(struct mz0380_dev *dev)
{
	int ret;

	TX_TRY(tx_update(dev, IT66121_CLK_BANK_REG,
			 IT66121_CLK_BANK_PWROFF_RCLK, 0));
	TX_TRY(tx_update(dev, IT66121_INT_REG, IT66121_INT_TX_CLK_OFF, 0));
	TX_TRY(tx_update(dev, IT66121_AFE_DRV_REG, IT66121_AFE_DRV_PWD, 0));
	TX_TRY(tx_update(dev, IT66121_AFE_XP_REG,
			 IT66121_AFE_XP_PWDI | IT66121_AFE_XP_PWDPLL, 0));
	TX_TRY(tx_update(dev, IT66121_AFE_IP_REG, IT66121_AFE_IP_PWDPLL, 0));
	TX_TRY(tx_update(dev, IT66121_AFE_DRV_REG, IT66121_AFE_DRV_RST, 0));
	TX_TRY(tx_update(dev, IT66121_AFE_XP_REG, IT66121_AFE_XP_RESETB,
			 IT66121_AFE_XP_RESETB));
	TX_TRY(tx_update(dev, IT66121_AFE_IP_REG, IT66121_AFE_IP_RESETB,
			 IT66121_AFE_IP_RESETB));
	TX_TRY(tx_update(dev, IT66121_SW_RST_REG, IT66121_SW_RST_REF,
			 IT66121_SW_RST_REF));
	/* per the programming manual, as mainline does */
	msleep(50);
out:
	return ret;
}

/* mainline it66121_configure_afe(), keyed on the source pixel clock */
static int tx_configure_afe(struct mz0380_dev *dev, u32 pixclk_khz)
{
	bool high = pixclk_khz > IT66121_AFE_CLK_HIGH_KHZ;
	int ret;

	TX_TRY(tx_wr(dev, IT66121_AFE_DRV_REG, IT66121_AFE_DRV_RST));
	TX_TRY(tx_update(dev, IT66121_AFE_XP_REG,
			 IT66121_AFE_XP_GAINBIT | IT66121_AFE_XP_ENO,
			 high ? IT66121_AFE_XP_GAINBIT : IT66121_AFE_XP_ENO));
	TX_TRY(tx_update(dev, IT66121_AFE_IP_REG,
			 IT66121_AFE_IP_GAINBIT | IT66121_AFE_IP_ER0,
			 high ? IT66121_AFE_IP_GAINBIT : IT66121_AFE_IP_ER0));
	TX_TRY(tx_update(dev, IT66121_AFE_IP_REG, IT66121_AFE_IP_EC1,
			 high ? 0 : IT66121_AFE_IP_EC1));
	TX_TRY(tx_update(dev, IT66121_AFE_XP_EC1_REG, IT66121_AFE_XP_EC1_LOWCLK,
			 high ? 0 : IT66121_AFE_XP_EC1_LOWCLK));
	TX_TRY(tx_update(dev, IT66121_SW_RST_REG,
			 IT66121_SW_RST_REF | IT66121_SW_RST_VID, 0));
	TX_TRY(tx_wr(dev, IT66121_AFE_DRV_REG, 0));
out:
	return ret;
}

static int tx_configure_input(struct mz0380_dev *dev)
{
	u8 csc = IT66121_INPUT_CSC_YUV_TO_RGB;
	unsigned int i;
	int ret;

	TX_TRY(tx_wr(dev, IT66121_INPUT_MODE_REG, mz0380_hdmitx_input & 0xff));
	/* contiguous 16-bit bus; pulse the TX FIFO reset across the change */
	TX_TRY(tx_update(dev, IT66121_TXFIFO_REG,
			 IT66121_TXFIFO_IOMOD_NONSEQ | IT66121_TXFIFO_RST,
			 IT66121_TXFIFO_IOMOD_NONSEQ | IT66121_TXFIFO_RST));
	TX_TRY(tx_update(dev, IT66121_TXFIFO_REG, IT66121_TXFIFO_RST, 0));
	if (mz0380_hdmitx_input & IT66121_INPUT_MODE_YUV422)
		csc |= IT66121_INPUT_CSC_ENUDFILTER;
	TX_TRY(tx_wr(dev, IT66121_INPUT_CSC_REG, csc));
	TX_TRY(tx_wr(dev, IT66121_CSC_YOFF_REG, 0x00));
	for (i = 0; i < ARRAY_SIZE(it66121_csc_709_limited); i++)
		TX_TRY(tx_wr(dev, IT66121_CSC_MTX_REG + i,
			     it66121_csc_709_limited[i]));
out:
	return ret;
}

/*
 * AVI infoframe: RGB, BT.709, 16:9, explicitly limited quantization so the
 * sink agrees with the matrix above even when the VIC is unknown (VIC 0
 * would otherwise default to full range).
 */
static int tx_write_avi(struct mz0380_dev *dev, u8 vic)
{
	struct hdmi_avi_infoframe frame;
	u8 buf[HDMI_INFOFRAME_SIZE(AVI)];
	unsigned int i;
	ssize_t len;
	int ret;

	hdmi_avi_infoframe_init(&frame);
	frame.colorspace = HDMI_COLORSPACE_RGB;
	frame.colorimetry = HDMI_COLORIMETRY_ITU_709;
	frame.picture_aspect = HDMI_PICTURE_ASPECT_16_9;
	frame.active_aspect = HDMI_ACTIVE_ASPECT_PICTURE;
	frame.quantization_range = HDMI_QUANTIZATION_RANGE_LIMITED;
	frame.video_code = vic;
	len = hdmi_avi_infoframe_pack(&frame, buf, sizeof(buf));
	if (len < 0)
		return len;

	TX_TRY(tx_bank(dev, 1));
	for (i = 0; i < 5; i++)
		TX_TRY(tx_wr(dev, IT66121_AVIINFO_DB1_REG + i,
			     buf[HDMI_INFOFRAME_HEADER_SIZE + i]));
	TX_TRY(tx_wr(dev, IT66121_AVIINFO_CSUM_REG, buf[3]));
	for (i = 5; i < HDMI_AVI_INFOFRAME_SIZE; i++)
		TX_TRY(tx_wr(dev, IT66121_AVIINFO_DB6_REG + i - 5,
			     buf[HDMI_INFOFRAME_HEADER_SIZE + i]));
	TX_TRY(tx_bank(dev, 0));
	ret = tx_wr(dev, IT66121_AVI_INFO_PKT_REG,
		    IT66121_AVI_INFO_PKT_ON | IT66121_AVI_INFO_PKT_RPT);
out:
	if (ret)
		tx_bank(dev, 0);
	return ret;
}

/*
 * mainline it66121_hdmi_audio_prepare() + audio_output_enable(), fixed at
 * 2ch 48 kHz 24-bit I2S on I2S0. CTS is measured by the hardware: mainline's
 * software-CTS path writes value 1 under mask BIT(1), which always lands as 0
 * (hardware CTS), so that is what every mainline user actually runs.
 */
static int tx_configure_audio(struct mz0380_dev *dev)
{
	const u8 fs = IT66121_AUD_FS_48K;
	struct hdmi_audio_infoframe frame;
	u8 buf[HDMI_INFOFRAME_SIZE(AUDIO)];
	ssize_t len;
	unsigned int i;
	int ret;

	hdmi_audio_infoframe_init(&frame);
	frame.channels = 2;
	len = hdmi_audio_infoframe_pack(&frame, buf, sizeof(buf));
	if (len < 0)
		return len;

	TX_TRY(tx_wr(dev, IT66121_CLK_CTRL0_REG,
		     IT66121_CLK_CTRL0_AUTO_OVER_SAMPLING |
		     (mz0380_hdmitx_mclk & 3) << IT66121_CLK_CTRL0_EXT_MCLK_SHIFT |
		     IT66121_CLK_CTRL0_AUTO_IPCLK));

	/* I2S, 24-bit, channels held off until the end */
	TX_TRY(tx_wr(dev, IT66121_AUD_CTRL0_REG, IT66121_AUD_CTRL0_24BIT));
	TX_TRY(tx_wr(dev, IT66121_AUD_CTRL1_REG, 0x01));
	TX_TRY(tx_wr(dev, IT66121_AUD_FIFOMAP_REG, 0xe4));
	TX_TRY(tx_wr(dev, IT66121_AUD_CTRL3_REG, 0x00));
	TX_TRY(tx_wr(dev, IT66121_AUD_SRCVALID_FLAT_REG, 0x00));
	TX_TRY(tx_wr(dev, IT66121_AUD_HDAUDIO_REG, 0x00));

	TX_TRY(tx_wr(dev, IT66121_KEY_REG, 0xc3));
	TX_TRY(tx_wr(dev, IT66121_KEY_REG, 0xa5));
	TX_TRY(tx_update(dev, IT66121_PKT_CTS_CTRL_REG,
			 IT66121_PKT_CTS_CTRL_SW, 0));
	TX_TRY(tx_wr(dev, IT66121_KEY_REG, 0xff));

	TX_TRY(tx_bank(dev, 1));
	TX_TRY(tx_wr(dev, IT66121_AUD_PKT_N0_REG, IT66121_AUD_N_48K & 0xff));
	TX_TRY(tx_wr(dev, IT66121_AUD_PKT_N0_REG + 1,
		     (IT66121_AUD_N_48K >> 8) & 0xff));
	TX_TRY(tx_wr(dev, IT66121_AUD_PKT_N0_REG + 2,
		     (IT66121_AUD_N_48K >> 16) & 0x0f));

	/* IEC 60958 channel status: PCM, 1 source, 2 channels, 48 kHz, 24-bit */
	TX_TRY(tx_wr(dev, IT66121_AUD_CHST_MODE_REG, 0x00));
	TX_TRY(tx_wr(dev, IT66121_AUD_CHST_CAT_REG, 0x00));
	TX_TRY(tx_wr(dev, IT66121_AUD_CHST_SRCNUM_REG, 0x01));
	TX_TRY(tx_wr(dev, IT66121_AUD_CHST_CHTNUM_REG, 0x02));
	TX_TRY(tx_wr(dev, IT66121_AUD_CHST_CA_FS_REG, fs));
	TX_TRY(tx_wr(dev, IT66121_AUD_CHST_OFS_WL_REG,
		     ((~fs << 4) & 0xf0) | IT66121_AUD_SWL_24BIT));

	for (i = 0; i < 5; i++)
		TX_TRY(tx_wr(dev, IT66121_AUD_INFO_DB1_REG + i,
			     buf[HDMI_INFOFRAME_HEADER_SIZE + i]));
	TX_TRY(tx_wr(dev, IT66121_AUD_INFO_CSUM_REG, buf[3]));
	TX_TRY(tx_bank(dev, 0));
	TX_TRY(tx_wr(dev, IT66121_AUD_INFO_PKT_REG,
		     IT66121_AVI_INFO_PKT_ON | IT66121_AVI_INFO_PKT_RPT));

	TX_TRY(tx_update(dev, IT66121_SW_RST_REG,
			 IT66121_SW_RST_AUD | IT66121_SW_RST_AREF, 0));
	ret = tx_wr(dev, IT66121_AUD_CTRL0_REG,
		    IT66121_AUD_CTRL0_24BIT | IT66121_AUD_CTRL0_EN_I2S0);
out:
	if (ret)
		tx_bank(dev, 0);
	return ret;
}

void mz0380_hdmitx_status(struct mz0380_dev *dev)
{
	static const u16 regs[] = { 0x04, 0x0e, 0x0f, 0x61, 0x62, 0x64,
				    0x70, 0x72, 0xc0, 0xc1, 0xcd,
				    0x58, 0xe0, 0xce };
	u8 v[ARRAY_SIZE(regs)] = { 0 };
	u8 cts[3] = { 0 };
	unsigned int i;

	mutex_lock(&it66121_lock);
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		tx_rd(dev, regs[i], &v[i]);
	if (!tx_bank(dev, 1)) {
		for (i = 0; i < ARRAY_SIZE(cts); i++)
			tx_rd(dev, IT66121_AUD_PKT_CTS_CNT0_REG + i, &cts[i]);
		tx_bank(dev, 0);
	}
	mutex_unlock(&it66121_lock);

	pr_info("%s: IT66121 status: hpd=%u rxsense=%u vid_stable=%u | 04=%02x 0e=%02x 0f=%02x 61=%02x 62=%02x 64=%02x 70=%02x 72=%02x c0=%02x c1=%02x cd=%02x\n",
		dev->name, !!(v[1] & IT66121_SYS_STATUS_HPDETECT),
		!!(v[1] & IT66121_SYS_STATUS_RXSENSE),
		!!(v[1] & IT66121_SYS_STATUS_VID_STABLE),
		v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9],
		v[10]);
	/* a measured CTS of 0 means no audio clock is reaching the TX */
	pr_info("%s: IT66121 audio: 58=%02x e0=%02x ce=%02x measured CTS=%u\n",
		dev->name, v[11], v[12], v[13],
		cts[2] << 12 | cts[1] << 4 | cts[0] >> 4);
}
EXPORT_SYMBOL_GPL(mz0380_hdmitx_status);

int mz0380_hdmitx_write_reg(struct mz0380_dev *dev, u16 reg, u8 val)
{
	int ret;

	if (reg > 0x1ff)
		return -EINVAL;

	mutex_lock(&it66121_lock);
	if (reg > 0xff) {
		ret = tx_bank(dev, 1);
		if (!ret)
			ret = tx_wr(dev, reg, val);
		tx_bank(dev, 0);
	} else {
		ret = tx_wr(dev, reg, val);
	}
	mutex_unlock(&it66121_lock);

	pr_info("%s: IT66121 write 0x%03x = 0x%02x (%d)\n",
		dev->name, reg, val, ret);
	return ret;
}
EXPORT_SYMBOL_GPL(mz0380_hdmitx_write_reg);

int mz0380_hdmitx_enable(struct mz0380_dev *dev)
{
	const struct v4l2_bt_timings *bt = &dev->detected_timings.bt;
	u32 pixclk_khz = 148500;	/* 1080p60, the only mode supported today */
	u8 id[4] = { 0 }, vic = 0;
	unsigned int i;
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -EAGAIN;

	if (bt->width && bt->pixelclock)
		pixclk_khz = div_u64(bt->pixelclock, 1000);
	if (bt->flags & V4L2_DV_FL_HAS_CEA861_VIC)
		vic = bt->cea861_vic;

	mutex_lock(&it66121_lock);

	for (i = 0; i < ARRAY_SIZE(id); i++)
		TX_TRY(tx_rd(dev, i, &id[i]));
	if ((id[0] | id[1] << 8) != IT66121_VENDOR_ID ||
	    ((id[2] | id[3] << 8) & 0x0fff) != IT66121_DEVICE_ID) {
		pr_warn("%s: no IT66121 at I2C 0x%02x (id %02x %02x %02x %02x) - HDMI OUT left alone\n",
			dev->name, IT66121_I2C_DEV, id[0], id[1], id[2], id[3]);
		ret = -ENODEV;
		goto out;
	}

	TX_TRY(tx_bank(dev, 0));
	TX_TRY(tx_power_up(dev));

	/* mainline it66121_set_mode() */
	TX_TRY(tx_wr(dev, IT66121_HDMI_MODE_REG,
		     mz0380_hdmitx_hdmi ? IT66121_HDMI_MODE_HDMI : 0));
	TX_TRY(tx_update(dev, IT66121_CLK_BANK_REG,
			 IT66121_CLK_BANK_PWROFF_TXCLK,
			 IT66121_CLK_BANK_PWROFF_TXCLK));
	TX_TRY(tx_configure_input(dev));
	TX_TRY(tx_configure_afe(dev, pixclk_khz));
	TX_TRY(tx_update(dev, IT66121_CLK_BANK_REG,
			 IT66121_CLK_BANK_PWROFF_TXCLK, 0));

	if (mz0380_hdmitx_hdmi)
		TX_TRY(tx_write_avi(dev, vic));
	/* audio needs HDMI mode; DVI carries none */
	if (mz0380_hdmitx_hdmi && mz0380_hdmitx_audio)
		TX_TRY(tx_configure_audio(dev));

	/* mainline it66121_set_mute(false) */
	TX_TRY(tx_update(dev, IT66121_AV_MUTE_REG, IT66121_AV_MUTE_ON, 0));
	TX_TRY(tx_wr(dev, IT66121_PKT_GEN_CTRL_REG,
		     IT66121_PKT_GEN_CTRL_ON | IT66121_PKT_GEN_CTRL_RPT));
out:
	mutex_unlock(&it66121_lock);

	if (ret == -ENODEV)
		return ret;
	if (ret) {
		pr_warn("%s: IT66121 bring-up failed (%d) - HDMI OUT may stay dark\n",
			dev->name, ret);
		return ret;
	}

	pr_info("%s: IT66121 HDMI OUT enabled (%s, input 0x%02x, %u kHz, VIC %u, audio %s)\n",
		dev->name, mz0380_hdmitx_hdmi ? "HDMI" : "DVI",
		mz0380_hdmitx_input & 0xff, pixclk_khz, vic,
		mz0380_hdmitx_hdmi && mz0380_hdmitx_audio ? "I2S 2ch 48k" : "off");
	msleep(100);
	mz0380_hdmitx_status(dev);
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_hdmitx_enable);
