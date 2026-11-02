// SPDX-License-Identifier: GPL-2.0
/*
 * mz0380-audio-dma.c - the card's HDMI audio periods (window 3, op 0x03).
 *
 * Read off the card runtime in MZ0380.HD.HEX (Elgato installer 1.1.0.194,
 * firmware 01.11):
 *
 *  - audio_capture_mgr acts on SET_AIC (op 0x2a). For the HD product it
 *    needs cmd+4 (input) == 0 and cmd+5 (channels) >= 2, then runs
 *    "./capture_app_infinite -D -P 5 -d 0 -R freq -F frames -B periods".
 *    START (op 0x06) then sets hready, which the app waits on.
 *  - capture_app_infinite snd_pcm_readi()s the card's I2S and MMA-copies
 *    each period to card address 0x90000000 with options {0x50, 3, ch, idx},
 *    idx cycling 1..4. vpl_dmac turns those into pcie_set_outbound(3, ch,
 *    idx), which aims ep.ko's single outbound window at window-3 slot idx -
 *    the slots host opcode 0x03 fills. The window keeps only the high 32
 *    bits of a target, so each period needs its own 4 GiB-aligned buffer.
 *  - It then writes {ch, a, b, c, idx, type=1} to channel_done. ep.ko's
 *    store_channel_done() puts idx - 1 in the ch nibble of BAR0+0x4c and
 *    raises EVENT bit (16 + ch), or with aic_int_mode=1 bit (15 + idx).
 *
 * Confirmed on hardware: ~46.9 events/s of 4096-byte periods, i.e. 1024
 * frames of S16_LE stereo at 48 kHz, with the period index matching between
 * the EVENT bit and BAR0+0x4c. mz0380-audio.c hands each one to ALSA.
 */

#include <linux/iommu.h>

#include "mz0380.h"
#include "mz0380-reg.h"

#define MZ0380_AUDIO_EVENT_MASK		GENMASK(23, 16)

void mz0380_audio_ring_free(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pci->dev);
	unsigned int i;

	for (i = 0; i < MZ0380_AUDIO_PERIODS; i++) {
		struct mz0380_stream_buf *b = &dev->audio_win3[i];

		if (!b->pages)
			continue;
		if (b->dma && domain)
			iommu_unmap(domain, b->dma, MZ0380_AUDIO_BUF_SIZE);
		__free_pages(b->pages, get_order(MZ0380_AUDIO_BUF_SIZE));
		b->pages = NULL;
		b->va = NULL;
		b->dma = 0;
	}
}

/*
 * The stream, H.264 and raw-bank buffers use high dwords 1..16 of the IOVA
 * space; the periods take the next four.
 */
int mz0380_audio_ring_alloc_iova(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pci->dev);
	unsigned int i;
	int ret;

	if (!mz0380_dma_iova_remap || !domain)
		return -ENODEV;

	for (i = 0; i < MZ0380_AUDIO_PERIODS; i++) {
		struct mz0380_stream_buf *b = &dev->audio_win3[i];
		dma_addr_t iova = mz0380_dma_iova_base +
			((u64)(MZ0380_AUDIO_IOVA_SLOT + i) << 32);
		phys_addr_t phys;

		if (iommu_iova_to_phys(domain, iova)) {
			pr_err("%s: audio period IOVA 0x%llx is already mapped\n",
			       dev->name, (unsigned long long)iova);
			ret = -EBUSY;
			goto err;
		}
		b->pages = alloc_pages(GFP_KERNEL,
				       get_order(MZ0380_AUDIO_BUF_SIZE));
		if (!b->pages) {
			ret = -ENOMEM;
			goto err;
		}
		b->va = page_address(b->pages);
		phys = page_to_phys(b->pages);

		ret = iommu_map(domain, iova, phys, MZ0380_AUDIO_BUF_SIZE,
				IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
		if (ret) {
			pr_err("%s: audio period iommu_map(0x%llx -> %pa) failed (%d)\n",
			       dev->name, (unsigned long long)iova, &phys, ret);
			goto err;
		}
		b->dma = iova;
		memset(b->va, MZ0380_AUDIO_POISON_BYTE, MZ0380_AUDIO_BUF_SIZE);
	}
	wmb();
	pr_info("%s: audio periods: %u x %u KiB at IOVA 0x%llx..\n",
		dev->name, MZ0380_AUDIO_PERIODS, MZ0380_AUDIO_BUF_SIZE >> 10,
		(unsigned long long)dev->audio_win3[0].dma);
	return 0;

err:
	mz0380_audio_ring_free(dev);
	return ret;
}

/* op 0x03 -> window-3 slots 1..4 of the video channel, one period each */
int mz0380_audio_program_ring(struct mz0380_dev *dev)
{
	u32 params[2 + 2 * MZ0380_AUDIO_PERIODS] = { 0 };
	unsigned int i;
	int ret;

	params[0] = MZ0380_STREAM_VIDEO_CHANNEL;
	params[1] = MZ0380_AUDIO_BUF_SIZE;
	for (i = 0; i < MZ0380_AUDIO_PERIODS; i++) {
		struct mz0380_stream_buf *b = &dev->audio_win3[i];

		if (!b->va)
			return -ENODEV;
		memset(b->va, MZ0380_AUDIO_POISON_BYTE, MZ0380_AUDIO_BUF_SIZE);
		params[2 + 2 * i] = upper_32_bits(b->dma);
		params[2 + 2 * i + 1] = lower_32_bits(b->dma);
	}
	wmb();

	ret = mz0380_send_command(dev, MZ0380_CMD_SET_BUF_3, params,
				  ARRAY_SIZE(params), NULL, 2000);
	pr_info("%s: audio periods registered on window3 (op 0x03, first 0x%016llx) ret=%d\n",
		dev->name, (unsigned long long)dev->audio_win3[0].dma, ret);
	return ret;
}

/* Called under event_lock with the EVENT word about to be acked. */
void mz0380_audio_note_event(struct mz0380_dev *dev, u32 event)
{
	if (!(event & MZ0380_AUDIO_EVENT_MASK))
		return;
	atomic_inc(&dev->irq_audio_count);
	dev->audio_last_event = event;
	dev->audio_last_token = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD3);
	/* the video channel's nibble holds the finished period, idx - 1 */
	mz0380_audio_period_elapsed(dev,
		(dev->audio_last_token >> (4 * MZ0380_STREAM_VIDEO_CHANNEL)) & 0xf);
}

void mz0380_audio_ring_dump(struct mz0380_dev *dev)
{
	unsigned int i, j;

	pr_info("%s: audio: events=%d last EVENT=%08x last 0x4c=%08x | live EVENT=%08x 0x4c=%08x\n",
		dev->name, atomic_read(&dev->irq_audio_count),
		dev->audio_last_event, dev->audio_last_token,
		mz_mmio_read(dev, MZ0380_MB_EVENT),
		mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD3));

	rmb();
	for (i = 0; i < MZ0380_AUDIO_PERIODS; i++) {
		const u8 *p = dev->audio_win3[i].va;
		unsigned int written = 0, last = 0;

		if (!p) {
			pr_info("%s: audio: period %u not allocated\n",
				dev->name, i);
			continue;
		}
		for (j = 0; j < MZ0380_AUDIO_BUF_SIZE; j++) {
			if (p[j] != MZ0380_AUDIO_POISON_BYTE) {
				written++;
				last = j + 1;
			}
		}
		pr_info("%s: audio: period %u: %u bytes written, extent %u\n",
			dev->name, i, written, last);
		print_hex_dump(KERN_INFO, "mz0380 audio: ", DUMP_PREFIX_OFFSET,
			       16, 2, p, 64, false);
	}
}
