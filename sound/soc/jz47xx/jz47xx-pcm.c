#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include <asm/io.h>
#include "jz47xx-pcm.h"

static int jz_pcm_debug = 0;

module_param(jz_pcm_debug, int, 0644);
#define PCM_DEBUG_MSG(msg...)			\
	do {					\
		if (jz_pcm_debug)		\
			printk("PCM: " msg);	\
	} while(0)

struct jz47xx_dma_buf_aic {
	struct jz47xx_dma_buf_aic	*next;
	int			 size;		/* buffer size in bytes */
	dma_addr_t		 data;		/* start of DMA data */
	dma_addr_t		 ptr;		/* where the DMA got to [1] */
	void			*id;		/* client's id */
};

struct jz47xx_runtime_data {
	spinlock_t lock;
	int state;
	int aic_dma_flag; /* start dma transfer or not */
	unsigned int dma_loaded;//use dma buff
	unsigned int dma_limit;//dma buff nums
	unsigned int dma_period;//once dma buff max size
	dma_addr_t dma_start;
	dma_addr_t dma_pos;
	dma_addr_t dma_end;
	struct jz47xx_pcm_dma_params *params;

	dma_addr_t user_cur_addr;         /* user current write buffer start address */
	unsigned int user_cur_len;        /* user current write buffer length */

	/* buffer list and information */
	struct jz47xx_dma_buf_aic	*curr;		/* current dma buffer */
	struct jz47xx_dma_buf_aic	*next;		/* next buffer to load */
	struct jz47xx_dma_buf_aic	*end;		/* end of queue */

	int first_transfer;
	int tran_bit;
};

/* identify hardware playback capabilities */
static const struct snd_pcm_hardware jz47xx_pcm_hardware = {
	.info			= SNDRV_PCM_INFO_MMAP |
	                            SNDRV_PCM_INFO_PAUSE |
	                            SNDRV_PCM_INFO_RESUME |
	                            SNDRV_PCM_INFO_MMAP_VALID |
				    SNDRV_PCM_INFO_INTERLEAVED |
	                            SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats		= SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_U16_LE |
				    SNDRV_PCM_FMTBIT_U8 |
				    SNDRV_PCM_FMTBIT_S8,
	.rates                  = SNDRV_PCM_RATE_32000_192000,
	.rate_min               = 32000,
	.rate_max               = 192000,
	.channels_min		= 1,
	.channels_max		= 2,
	.buffer_bytes_max	= PAGE_SIZE * 128, //PAGE_SIZE * 256,
	.period_bytes_min	= PAGE_SIZE * 2,//8 /* 16K */ //dma_period
	.period_bytes_max	= PAGE_SIZE * 16, /* 64K */
	.periods_min		= 8,//dma_limit
	.periods_max		= 64,    //buffer_bytes_max/period_bytes_min
	.fifo_size		= 32,
};

/* jz47xx__dma_buf_enqueue
 *
 * queue an given buffer for dma transfer.
 *
 * allen --(use finish to) entern queue,(will use to)out queue
 *
 * data       the physical address of the buffer data
 * size       the size of the buffer in bytes
 *
*/
static int jz47xx_dma_buf_enqueue(struct jz47xx_runtime_data *prtd, dma_addr_t data, int size)
{
	struct jz47xx_dma_buf_aic *aic_buf;

	aic_buf = kzalloc(sizeof(struct jz47xx_dma_buf_aic), GFP_KERNEL);
	if (aic_buf == NULL) {
		printk("aic buffer allocate failed,no memory!\n");
		return -ENOMEM;
	}
	aic_buf->next = NULL;
	aic_buf->data = aic_buf->ptr = data;
	aic_buf->size = size;
	//memset((void *)data, 0x0, size);
	
	if( prtd->curr == NULL) {
		prtd->curr = aic_buf;
		prtd->end  = aic_buf;
		prtd->next = NULL;
	} else {
		if (prtd->end == NULL)
			printk("prtd->end is NULL\n");
		prtd->end->next = aic_buf;
		prtd->end = aic_buf;
	}

	/* if necessary, update the next buffer field */
	if (prtd->next == NULL)
		prtd->next = aic_buf;

	return 0;
}

static unsigned int get_dma_curr_count(int chan) {
	unsigned int count, ds;
	int dma_ds[] = {4, 1, 2, 16, 32};

	ds = (REG_DMAC_DCMD(chan) & DMAC_DCMD_DS_MASK) >> DMAC_DCMD_DS_BIT;
	count = REG_DMAC_DTCR(chan);
	count = count * dma_ds[ds];

	return count;
}

void jz_pcm_start_normal_dma(struct jz47xx_runtime_data *prtd,
			     int chan, unsigned long phyaddr, int count, int mode)
{
	unsigned long flags;
	unsigned long start_time;
	u32 dma_cmd = 0;
	u32 src_addr = 0;
	u32 dst_addr = 0;
	u32 req_src = 0;
	int ds;

	if ((DMA_MODE_WRITE == mode) && ((count % 32) == 0))
		ds = 32;      /* 32 byte */
	else if ((count % 16) == 0)
		ds = 16;      /* 16 byte */
	else
		ds = 4;	      /* default to 4 byte */

	start_time = jiffies;

	while (REG_DMAC_DMACR(chan / HALF_DMA_NUM) & (DMAC_DMACR_HLT | DMAC_DMACR_AR)) {
		if (jiffies - start_time > 10) { /* 100ms */
			printk("DMAC unavailable! REG_DMAC_DMACR(%d) = 0x%08x\n", chan / HALF_DMA_NUM, REG_DMAC_DMACR(chan / HALF_DMA_NUM));
			jz_stop_dma(chan);
			break;
		}
	}

	start_time = jiffies;
	while (REG_DMAC_DCCSR(chan) & (DMAC_DCCSR_HLT | DMAC_DCCSR_TT | DMAC_DCCSR_AR)) {
		if (jiffies - start_time > 10) { /* 100ms */
			printk("DMA channel %d unavailable! REG_DMAC_DCCSR(%d) = 0x%08x\n", chan, chan, REG_DMAC_DCCSR(chan));
			jz_stop_dma(chan);
			break;
		}
	}

	flags = claim_dma_lock();
	disable_dma(chan);

	switch (prtd->tran_bit) {
	case 8:
		dma_cmd = DMAC_DCMD_SWDH_8 | DMAC_DCMD_DWDH_8;
		break;
	case 16:
		dma_cmd = DMAC_DCMD_SWDH_16 | DMAC_DCMD_DWDH_16;
		break;
	case 17 ... 32:
	default:
		dma_cmd = DMAC_DCMD_SWDH_32 | DMAC_DCMD_DWDH_32;
		break;
	}

	dma_cmd |= DMAC_DCMD_RDIL_IGN | DMAC_DCMD_TIE;


	switch (ds) {
	case 32:
		dma_cmd |= DMAC_DCMD_DS_32BYTE;
		break;

	case 16:
		dma_cmd |= DMAC_DCMD_DS_16BYTE;
		break;

	case 4:
		dma_cmd |= DMAC_DCMD_DS_32BIT;
		break;

	default:
		;
	}

	if (DMA_MODE_WRITE == mode) {
		dma_cmd |= DMAC_DCMD_SAI;
		src_addr = (unsigned int)phyaddr;      /* DMA source address */
		dst_addr = CPHYSADDR(AIC_DR);
		req_src = DMAC_DRSR_RS_AICOUT;
	} else {
		dma_cmd |= DMAC_DCMD_DAI;
		src_addr = CPHYSADDR(AIC_DR);
		dst_addr = (unsigned int)phyaddr;
		req_src = DMAC_DRSR_RS_AICIN;
	}

	REG_DMAC_DCCSR(chan) |= DMAC_DCCSR_NDES; /* No-descriptor transfer */
	REG_DMAC_DSAR(chan) = src_addr;
	REG_DMAC_DTAR(chan) = dst_addr;
	REG_DMAC_DTCR(chan) = (count + ds - 1) / ds;
	REG_DMAC_DCMD(chan) = dma_cmd;
	REG_DMAC_DRSR(chan) = req_src;

	REG_DMAC_DMACR(chan / HALF_DMA_NUM) |= DMAC_DMACR_DMAE;
	REG_DMAC_DCCSR(chan) |= DMAC_DCCSR_EN;

	release_dma_lock(flags);
}

void audio_start_dma(struct jz47xx_runtime_data *prtd, int mode)
{
	struct jz47xx_dma_buf_aic *aic_buf;
	int channel;

	spin_lock(&prtd->lock);

	if (prtd->first_transfer == 1) 
		prtd->first_transfer = 0;
	else 
	{
		aic_buf = prtd->curr;
		
		if (aic_buf != NULL) 
		{
			prtd->curr = aic_buf->next;
			prtd->next = prtd->curr->next;
			aic_buf->next  = NULL;
			kfree(aic_buf);
            prtd->dma_loaded--;
			aic_buf = NULL;
		}
	}

	aic_buf = prtd->curr;
	channel = prtd->params->channel;
	if (aic_buf) {
		dma_cache_wback_inv(CKSEG1ADDR((unsigned long)aic_buf->data),(unsigned long)aic_buf->size);

		jz_pcm_start_normal_dma(prtd, channel, aic_buf->data,aic_buf->size, mode);
		
		prtd->aic_dma_flag |= AIC_START_DMA;
	} else {
		printk("next buffer is NULL for capture\n");
		prtd->aic_dma_flag &= ~AIC_START_DMA;
	}

	spin_unlock(&prtd->lock);
}

/*
 * place a dma buffer onto the queue for the dma system to handle.
*/
static void jz47xx_pcm_enqueue(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct jz47xx_runtime_data *prtd = runtime->private_data;
	dma_addr_t pos = prtd->dma_pos;
	int ret;
	
	while (prtd->dma_loaded < prtd->dma_limit)
	{
		unsigned long len = prtd->dma_period;//16k

		if ((pos + len) > prtd->dma_end) {
			len  = prtd->dma_end - pos;
		}
		ret = jz47xx_dma_buf_enqueue(prtd, pos, len);
		if (ret == 0) {
			prtd->dma_loaded++;
			//pos += prtd->dma_period;    //jintao debug it//allenjt
			pos += len;
			if (pos >= prtd->dma_end)
				pos = prtd->dma_start;
		} 
		else
			break;
	}

	prtd->dma_pos = pos;
}

static void schedule_next_period(struct snd_pcm_substream *substream) {
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct jz47xx_runtime_data *prtd = runtime->private_data;
	unsigned long flags;

    if (substream && snd_pcm_running(substream))
		snd_pcm_period_elapsed(substream);

	local_irq_save(flags);
	if (prtd->state & ST_RUNNING) {
		if (prtd->dma_loaded) {
			if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
				audio_start_dma(prtd, DMA_MODE_WRITE);
			else
				audio_start_dma(prtd, DMA_MODE_READ);
		}
	}
	local_irq_restore(flags);

    spin_lock(&prtd->lock);
    jz47xx_pcm_enqueue(substream);
	spin_unlock(&prtd->lock);
}

/*
 * call the function:jz47xx_pcm_dma_irq() after DMA has transfered the current buffer
 */
static irqreturn_t jz47xx_pcm_dma_irq(int dma_ch, void *dev_id)
{
	struct snd_pcm_substream *substream = dev_id;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct jz47xx_runtime_data *prtd = runtime->private_data;
	int channel = prtd->params->channel;


	disable_dma(channel);
	prtd->aic_dma_flag &= ~AIC_START_DMA;
	/* must clear TT bit in DCCSR to avoid interrupt again */
	if (__dmac_channel_transmit_end_detected(channel)) {
		__dmac_channel_clear_transmit_end(channel);
	}
	if (__dmac_channel_transmit_halt_detected(channel)) {
		__dmac_channel_clear_transmit_halt(channel);
	}

	if (__dmac_channel_address_error_detected(channel)) {
		__dmac_channel_clear_address_error(channel);
	}

	schedule_next_period(substream);

	return IRQ_HANDLED;
}

static int replay_dma_chan = -1;
static int replay_dma_inited = 0;
static int record_dma_chan = -1;
static int record_dma_inited = 0;

/* some parameter about DMA operation */
static int jz47xx_pcm_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct jz47xx_runtime_data *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct jz47xx_pcm_dma_params *dma = rtd->dai->cpu_dai->dma_data;
	size_t totbytes = params_buffer_bytes(params);
	int ret = 0;

	PCM_DEBUG_MSG("enter %s\n", __func__);

	if (!dma)
	 	return 0;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_U8:
	case SNDRV_PCM_FORMAT_S8:
		prtd->tran_bit = 8;
		break;
	case SNDRV_PCM_FORMAT_U16_LE:
	case SNDRV_PCM_FORMAT_S16_LE:
		prtd->tran_bit = 16;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
	default:
		prtd->tran_bit = 32;
		break;
	}

	/* prepare DMA */
	prtd->params = dma;
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (!replay_dma_inited) {
			ret = jz_request_dma(DMA_ID_AIC_TX, prtd->params->client->name,
					     jz47xx_pcm_dma_irq, IRQF_DISABLED, substream);
			if (ret < 0) {
				printk("alsa playback: request dma channel failed!\n");
				return ret;
			}

			replay_dma_chan = ret;
			replay_dma_inited = 1;
		}
		prtd->params->channel = replay_dma_chan;
	} else {
		if (!record_dma_inited) {
			ret = jz_request_dma(DMA_ID_AIC_RX, prtd->params->client->name,
					     jz47xx_pcm_dma_irq, IRQF_DISABLED, substream);
			if (ret < 0) {
				printk("alsa record: request dma channel failed!\n");
				return ret;
			}

			record_dma_chan = ret;
			record_dma_inited = 1;
		}
		prtd->params->channel = record_dma_chan;
	}

	snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);
	runtime->dma_bytes = totbytes;

	spin_lock_irq(&prtd->lock);
	prtd->dma_loaded = 0;
	prtd->aic_dma_flag = 0;
	prtd->dma_limit = runtime->hw.periods_min;
	prtd->dma_period = params_period_bytes(params);//16k
	prtd->dma_start = runtime->dma_addr;
	prtd->dma_pos = prtd->dma_start;
	prtd->dma_end = prtd->dma_start + totbytes;
	prtd->curr = NULL;
	prtd->next = NULL;
	prtd->end = NULL;
	prtd->first_transfer = 1;


	__dmac_disable_descriptor(prtd->params->channel);
	__dmac_channel_disable_irq(prtd->params->channel);
	spin_unlock_irq(&prtd->lock);
	

#if 0
	printk("===>totbytes =  0x%08x\n", totbytes);
	printk("===>dma_limit = %d\n", prtd->dma_limit);
	printk("===>dma_period = %d\n", prtd->dma_period);
	printk("===>dma_start = 0x%08x\n", prtd->dma_start);
	printk("===>dma_end =   0x%08x\n", prtd->dma_end);
	printk("===>params_channels = %d\n", params_channels(params));
	printk("===>params_period_size = %d\n", params_period_size(params));
	printk("===>snd_pcm_format_physical_width(params_format(p)) = %d\n", snd_pcm_format_physical_width(params_format(params)));
#endif

	return ret;
}

static int jz47xx_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct jz47xx_runtime_data *prtd = substream->runtime->private_data;

	snd_pcm_set_runtime_buffer(substream, NULL);
	if (prtd->params) {
#if 0
		jz_free_dma(prtd->params->channel);
#endif
		prtd->params = NULL;
	}

	return 0;
}

/* set some dma para for playback/capture */
static int jz47xx_dma_ctrl(int channel)
{
	disable_dma(channel);

	/* must clear TT bit in DCCSR to avoid interrupt again */
	if (__dmac_channel_transmit_end_detected(channel)) {
		__dmac_channel_clear_transmit_end(channel);
	}
	if (__dmac_channel_transmit_halt_detected(channel)) {
		__dmac_channel_clear_transmit_halt(channel);
	}

	if (__dmac_channel_address_error_detected(channel)) {
		__dmac_channel_clear_address_error(channel);
	}

	return 0;

}

static int jz47xx_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct jz47xx_runtime_data *prtd = substream->runtime->private_data;
	int ret = 0;

	/* return if this is a bufferless transfer e.g */
	if (!prtd->params)
	 	return 0;

	/* flush the DMA channel and DMA channel bit check */
	jz47xx_dma_ctrl(prtd->params->channel);

	/* enqueue dma buffers */
	jz47xx_pcm_enqueue(substream);

	return ret;

}

static void jz47xx_pcm_clean(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct jz47xx_runtime_data *prtd = runtime->private_data;
    struct jz47xx_dma_buf_aic *aic_buf = NULL;

    aic_buf = prtd->curr;
    while (aic_buf != NULL) 
	{
        prtd->curr = aic_buf->next;
        kfree(aic_buf);
        aic_buf = prtd->curr;
    }

	prtd->aic_dma_flag = 0;
    prtd->curr = NULL;
    prtd->next = NULL;
    prtd->end = NULL;
    prtd->dma_loaded = 0;
    prtd->dma_start = runtime->dma_addr;
    prtd->dma_pos = prtd->dma_start;
    prtd->first_transfer = 1;
}

static int jz47xx_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct jz47xx_runtime_data *prtd = runtime->private_data;

	int ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		//printk(" START\n");
		prtd->state |= ST_RUNNING;
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			audio_start_dma(prtd, DMA_MODE_WRITE);
		} else {
			audio_start_dma(prtd, DMA_MODE_READ);
		}

		break;

	case SNDRV_PCM_TRIGGER_STOP:
		//printk(" STOP\n");
		prtd->state &= ~ST_RUNNING;
		__i2s_disable_transmit_dma();
		jz47xx_dma_ctrl(prtd->params->channel);
        jz47xx_pcm_clean(substream);//allenjt add
		break;

	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		printk(" SUSPEND \n");
		prtd->state &= ~ST_RUNNING;
		break;

	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		printk(" RESUME \n");
		prtd->state |= ST_RUNNING;
		break;

	default:
		ret = -EINVAL;
	}

	return ret;
}

static snd_pcm_uframes_t
jz47xx_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct jz47xx_runtime_data *prtd = runtime->private_data;
	struct jz47xx_dma_buf_aic *aic_buf = prtd->curr;

	int channel = prtd->params->channel;
#if 1

	long count,res;
	dma_addr_t ptr;
	snd_pcm_uframes_t x;

#if 0
	printk("===>aic_buf->size = %d\n", aic_buf->size);
	printk("===>aic_buf->data = 0x%08x\n", aic_buf->data);
	printk("===>prtd->dma_start = 0x%08x\n", prtd->dma_start);
	printk("===>prtd->dma_end = 0x%08x\n", prtd->dma_end);
	printk("===>prtd->dma_cnt = 0x%08x\n", prtd->dma_end - prtd->dma_start);
	printk("===>snd_pcm_lib_buffer_bytes(substream) = %d\n", snd_pcm_lib_buffer_bytes(substream));
#endif

	spin_lock(&prtd->lock);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		count = get_dma_curr_count(channel);
		count = aic_buf->size - count;
		ptr = aic_buf->data + count;
		res = ptr - prtd->dma_start;
	} else {
		count = get_dma_curr_count(channel);
		count = aic_buf->size - count;
		ptr = aic_buf->data + count;
		res = ptr - prtd->dma_start;
	}

	spin_unlock(&prtd->lock);
	x = bytes_to_frames(runtime, res);
	if (x >= runtime->buffer_size)         //allenjt change it from '==' to '>='
		x = 0;

	return x;
#else

	long remain_cnt, transfered_cnt;

	spin_lock(&prtd->lock);

	remain_cnt = get_dma_residue(channel);

	spin_unlock(&prtd->lock);

	transfered_cnt = (prtd->dma_end - prtd->dma_start) - remain_cnt;

	/* May never happer, just for sanity check */
	if (transfered_cnt >= snd_pcm_lib_buffer_bytes(substream)) {
		if (transfered_cnt == snd_pcm_lib_buffer_bytes(substream))
			transfered_cnt = 0;
	}

	return bytes_to_frames(substream->runtime, transfered_cnt);

#endif
}

static int jz47xx_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct jz47xx_runtime_data *prtd;

	PCM_DEBUG_MSG("enter jz47xx_pcm_open, substream is %s\n",
		  (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		  "playback" : "capture");

	snd_soc_set_runtime_hwparams(substream, &jz47xx_pcm_hardware);
	prtd = kzalloc(sizeof(struct jz47xx_runtime_data), GFP_KERNEL);
	if (prtd == NULL)
		return -ENOMEM;

	spin_lock_init(&prtd->lock);

	runtime->private_data = prtd;

	return 0;
}

static int jz47xx_pcm_close(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct jz47xx_runtime_data *prtd = runtime->private_data;
	struct jz47xx_dma_buf_aic *aic_buf = NULL;
	
	PCM_DEBUG_MSG("enter jz47xx_pcm_close, substream is %s\n",
		      (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		      "playback" : "capture");

	if (prtd)
		aic_buf = prtd->curr;

	while (aic_buf != NULL) {
		prtd->curr = aic_buf->next;
		kfree(aic_buf);
		aic_buf = prtd->curr;
	}

	if (prtd) {
		prtd->curr = NULL;
		prtd->next = NULL;
		prtd->end = NULL;
		kfree(prtd);
	}

	return 0;
}

static int jz47xx_pcm_mmap(struct snd_pcm_substream *substream,
			   struct vm_area_struct *vma)//include/linux/mm.h
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long start;
	unsigned long off;
	u32 len;
	int ret = -ENXIO;

	off = vma->vm_pgoff << PAGE_SHIFT;
	start = runtime->dma_addr;

	len = PAGE_ALIGN((start & ~PAGE_MASK) + runtime->dma_bytes);
	start &= PAGE_MASK;

	if ((vma->vm_end - vma->vm_start + off) > len) {
		return -EINVAL;
	}

	off += start;
	vma->vm_pgoff = off >> PAGE_SHIFT;
	vma->vm_flags |= VM_IO;

#if defined(CONFIG_MIPS32)
	pgprot_val(vma->vm_page_prot) &= ~_CACHE_MASK;
	pgprot_val(vma->vm_page_prot) |= _CACHE_UNCACHED;
	/* pgprot_val(vma->vm_page_prot) |= _CACHE_CACHABLE_NONCOHERENT; */
#endif
	ret = io_remap_pfn_range(vma, vma->vm_start, off >> PAGE_SHIFT,
			       vma->vm_end - vma->vm_start,
			       vma->vm_page_prot);

	return ret;
}

struct snd_pcm_ops jz47xx_pcm_ops = {
	.open		= jz47xx_pcm_open,
	.close		= jz47xx_pcm_close,
	.ioctl		= snd_pcm_lib_ioctl,
	.hw_params	= jz47xx_pcm_hw_params,
	.hw_free	= jz47xx_pcm_hw_free,
	.prepare	= jz47xx_pcm_prepare,
	.trigger	= jz47xx_pcm_trigger,
	.pointer	= jz47xx_pcm_pointer,
	.mmap		= jz47xx_pcm_mmap,
};

static int jz47xx_pcm_preallocate_dma_buffer(struct snd_pcm *pcm, int stream)
{
	struct snd_pcm_substream *substream = pcm->streams[stream].substream;
	struct snd_dma_buffer *buf = &substream->dma_buffer;
	size_t size = jz47xx_pcm_hardware.buffer_bytes_max;

	buf->dev.type = SNDRV_DMA_TYPE_DEV;
	buf->dev.dev = pcm->card->dev;
	buf->private_data = NULL;

#if 1//allen add(no use recoad),less memory use
	if(stream == SNDRV_PCM_STREAM_CAPTURE)
		size = 256;
#endif

	/*buf->area = dma_alloc_coherent(pcm->card->dev, size,
	  &buf->addr, GFP_KERNEL);*/
	buf->area = dma_alloc_noncoherent(pcm->card->dev, size,
					  &buf->addr, GFP_KERNEL);
	if (!buf->area)
		return -ENOMEM;
	buf->bytes = size;
	return 0;
}

static void jz47xx_pcm_free_dma_buffers(struct snd_pcm *pcm)
{
	struct snd_pcm_substream *substream;
	struct snd_dma_buffer *buf;
	int stream;

	for (stream = 0; stream < 2; stream++) {
		substream = pcm->streams[stream].substream;
		if (!substream)
			continue;

		buf = &substream->dma_buffer;
		if (!buf->area)
			continue;

		dma_free_noncoherent(pcm->card->dev, buf->bytes,
		  buf->area, buf->addr);
		buf->area = NULL;
	}
}

static u64 jz47xx_pcm_dmamask = DMA_BIT_MASK(32);

int jz47xx_pcm_new(struct snd_card *card, struct snd_soc_dai *dai,
	struct snd_pcm *pcm)
{
	int ret = 0;

	if (!card->dev->dma_mask)
		card->dev->dma_mask = &jz47xx_pcm_dmamask;
	if (!card->dev->coherent_dma_mask)
		card->dev->coherent_dma_mask = DMA_BIT_MASK(32);

	if (dai->playback.channels_min) {
		ret = jz47xx_pcm_preallocate_dma_buffer(pcm,
			SNDRV_PCM_STREAM_PLAYBACK);
		if (ret)
			goto out;
	}

	if (dai->capture.channels_min) {
		printk("\n alenn: record malloc dma buffer ...... \n");
		ret = jz47xx_pcm_preallocate_dma_buffer(pcm,
			SNDRV_PCM_STREAM_CAPTURE);
		if (ret)
			goto out;
	}

 out:

	return ret;
}

struct snd_soc_platform jz47xx_soc_platform = {
	.name		= "jz47xx-audio",
	.pcm_ops 	= &jz47xx_pcm_ops,
	.pcm_new	= jz47xx_pcm_new,
	.pcm_free	= jz47xx_pcm_free_dma_buffers,
};

EXPORT_SYMBOL_GPL(jz47xx_soc_platform);

static int __init jz47xx_soc_platform_init(void)
{
	return snd_soc_register_platform(&jz47xx_soc_platform);
}
module_init(jz47xx_soc_platform_init);

static void __exit jz47xx_soc_platform_exit(void)
{
	snd_soc_unregister_platform(&jz47xx_soc_platform);
}
module_exit(jz47xx_soc_platform_exit);

MODULE_AUTHOR("Lutts Wolf <slcao@ingenic.cn>");
MODULE_DESCRIPTION("Ingenic Jz47xx PCM DMA module");
MODULE_LICENSE("GPL");
