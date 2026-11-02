// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Driver for MZ0380 based capture cards.
 *
 *  ALSA capture of the HDMI audio the card extracts.
 *
 *  The card pushes 16-bit stereo PCM into four host period buffers on
 *  window 3 and signals each finished one through EVENT bits 16+ with the
 *  period index in BAR0+0x4c (mz0380-audio-dma.c). This file copies every
 *  finished period into the ALSA ring from that event, so the stream only
 *  runs while the card's pipeline does - i.e. while video is streaming.
 */

#include "mz0380.h"
#include "mz0380-reg.h"

#if IS_ENABLED(CONFIG_SND)

#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

struct mz0380_pcm {
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct snd_pcm_substream *substream;
	struct mz0380_dev *dev;
	unsigned int period_bytes;
	unsigned int buffer_bytes;
	unsigned int hw_ptr_bytes;
	unsigned int period_fill;
	bool running;
};

#define MZ0380_AUDIO_RATE      48000
#define MZ0380_AUDIO_CHANNELS  2

/*
 * What SET_AIC asked for: frames x periods per card period (the capture app
 * reads -F x -B frames per transfer), 16-bit stereo. 256 x 4 x 4 = 4096.
 */
static unsigned int mz0380_card_period_bytes(void)
{
	return min_t(unsigned int, MZ0380_AUDIO_BUF_SIZE,
		     mz0380_aic_period_frames * mz0380_aic_periods *
		     MZ0380_AUDIO_CHANNELS * 2);
}

static const struct snd_pcm_hardware mz0380_pcm_hw = {
	.info             = SNDRV_PCM_INFO_INTERLEAVED |
			    SNDRV_PCM_INFO_BLOCK_TRANSFER |
			    SNDRV_PCM_INFO_MMAP |
			    SNDRV_PCM_INFO_MMAP_VALID,
	.formats          = SNDRV_PCM_FMTBIT_S16_LE,
	.rates            = SNDRV_PCM_RATE_48000,
	.rate_min         = MZ0380_AUDIO_RATE,
	.rate_max         = MZ0380_AUDIO_RATE,
	.channels_min     = MZ0380_AUDIO_CHANNELS,
	.channels_max     = MZ0380_AUDIO_CHANNELS,
	.buffer_bytes_max = 256 * 1024,
	.period_bytes_min = 1024,
	.period_bytes_max = 64 * 1024,
	.periods_min      = 2,
	.periods_max      = 64,
};

static int mz0380_pcm_open(struct snd_pcm_substream *ss)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;

	ss->runtime->hw = mz0380_pcm_hw;
	/* the card delivers whole periods; match them so wakeups line up */
	snd_pcm_hw_constraint_step(ss->runtime, 0,
				   SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
				   mz0380_card_period_bytes());
	pcm->substream = ss;
	return 0;
}

static int mz0380_pcm_close(struct snd_pcm_substream *ss)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;

	WRITE_ONCE(pcm->running, false);
	pcm->substream = NULL;
	return 0;
}

static int mz0380_pcm_hw_params(struct snd_pcm_substream *ss,
				struct snd_pcm_hw_params *p)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;

	pcm->period_bytes = params_period_bytes(p);
	pcm->buffer_bytes = params_buffer_bytes(p);
	return 0;
}

static int mz0380_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;

	WRITE_ONCE(pcm->hw_ptr_bytes, 0);
	pcm->period_fill = 0;
	return 0;
}

/* Never takes event_lock: mz0380_audio_period_elapsed() runs under it. */
static int mz0380_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		WRITE_ONCE(pcm->running, true);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		WRITE_ONCE(pcm->running, false);
		return 0;
	}
	return -EINVAL;
}

static snd_pcm_uframes_t mz0380_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;

	return bytes_to_frames(ss->runtime, READ_ONCE(pcm->hw_ptr_bytes));
}

static const struct snd_pcm_ops mz0380_pcm_ops = {
	.open      = mz0380_pcm_open,
	.close     = mz0380_pcm_close,
	.hw_params = mz0380_pcm_hw_params,
	.prepare   = mz0380_pcm_prepare,
	.trigger   = mz0380_pcm_trigger,
	.pointer   = mz0380_pcm_pointer,
};

/*
 * The card finished period @idx (0..3). Called from the event path under
 * event_lock, in IRQ or poller context.
 */
void mz0380_audio_period_elapsed(struct mz0380_dev *dev, unsigned int idx)
{
	struct mz0380_pcm *pcm = dev->snd_pcm;
	struct snd_pcm_substream *ss;
	unsigned int n, first, ptr;
	const u8 *src;
	u8 *dst;

	if (!pcm || !READ_ONCE(pcm->running) || idx >= MZ0380_AUDIO_PERIODS)
		return;
	ss = pcm->substream;
	src = dev->audio_win3[idx].va;
	if (!ss || !src || !pcm->buffer_bytes)
		return;

	/* pair the endpoint's completion with its DMA, as the video path does */
	dma_rmb();
	n = min(mz0380_card_period_bytes(), pcm->buffer_bytes);
	dst = ss->runtime->dma_area;
	ptr = pcm->hw_ptr_bytes;
	first = min(n, pcm->buffer_bytes - ptr);
	memcpy(dst + ptr, src, first);
	memcpy(dst, src + first, n - first);
	WRITE_ONCE(pcm->hw_ptr_bytes, (ptr + n) % pcm->buffer_bytes);

	pcm->period_fill += n;
	if (pcm->period_fill >= pcm->period_bytes) {
		pcm->period_fill %= pcm->period_bytes;
		snd_pcm_period_elapsed(ss);
	}
}
EXPORT_SYMBOL_GPL(mz0380_audio_period_elapsed);

int mz0380_audio_register(struct mz0380_dev *dev)
{
	struct snd_card *card;
	struct snd_pcm *pcm_dev;
	struct mz0380_pcm *pcm;
	int ret;

	if (dev->audio_registered)
		return 0;

	ret = snd_card_new(&dev->pci->dev, SNDRV_DEFAULT_IDX1, "HD60Pro",
			   THIS_MODULE, sizeof(*pcm), &card);
	if (ret)
		return ret;

	pcm = card->private_data;
	pcm->card = card;
	pcm->dev = dev;
	dev->snd_card = card;
	dev->snd_pcm  = pcm;

	strscpy(card->driver, "mz0380", sizeof(card->driver));
	strscpy(card->shortname, "HD60 Pro HDMI", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "%s HDMI audio at %s", mz0380_boards[dev->board].name,
		 pci_name(dev->pci));

	ret = snd_pcm_new(card, "HD60 Pro HDMI", 0, 0, 1, &pcm_dev);
	if (ret)
		goto fail_card;
	pcm->pcm = pcm_dev;
	pcm_dev->private_data = dev;
	pcm_dev->info_flags = 0;
	strscpy(pcm_dev->name, "HD60 Pro HDMI", sizeof(pcm_dev->name));
	snd_pcm_set_ops(pcm_dev, SNDRV_PCM_STREAM_CAPTURE, &mz0380_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm_dev,
				       SNDRV_DMA_TYPE_VMALLOC, NULL,
				       0, 256 * 1024);

	ret = snd_card_register(card);
	if (ret)
		goto fail_card;

	dev->audio_registered = true;
	pr_info("%s: ALSA card '%s' registered\n",
		dev->name, card->shortname);
	return 0;

fail_card:
	snd_card_free(card);
	dev->snd_card = NULL;
	dev->snd_pcm  = NULL;
	return ret;
}
EXPORT_SYMBOL_GPL(mz0380_audio_register);

void mz0380_audio_unregister(struct mz0380_dev *dev)
{
	if (!dev->audio_registered)
		return;

	if (dev->snd_card) {
		snd_card_free(dev->snd_card);
		dev->snd_card = NULL;
	}
	dev->snd_pcm = NULL;
	dev->audio_registered = false;
}
EXPORT_SYMBOL_GPL(mz0380_audio_unregister);

#endif /* CONFIG_SND */
