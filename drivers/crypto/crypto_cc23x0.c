/*
 * Copyright (c) 2024 BayLibre, SAS
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT ti_cc23x0_aes

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(crypto_cc23x0, CONFIG_CRYPTO_LOG_LEVEL);

#include <zephyr/crypto/crypto.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <string.h>

#include <driverlib/aes.h>
#include <driverlib/clkctl.h>

#include <inc/hw_memmap.h>

#define CRYPTO_CC23_CAP		(CAP_RAW_KEY | CAP_SEPARATE_IO_BUFS | \
				 CAP_SYNC_OPS | CAP_NO_IV_PREFIX)

/* CCM mode: see https://datatracker.ietf.org/doc/html/rfc3610 for reference */
#define CCM_CC23_MSG_LEN_SIZE_MIN	2
#define CCM_CC23_MSG_LEN_SIZE_MAX	8

#define CCM_CC23_NONCE_LEN_SIZE_MIN	(AES_BLOCK_SIZE - CCM_CC23_MSG_LEN_SIZE_MAX - 1)
#define CCM_CC23_NONCE_LEN_SIZE_MAX	(AES_BLOCK_SIZE - CCM_CC23_MSG_LEN_SIZE_MIN - 1)

#define CCM_CC23_AD_LEN_SIZE		2
#define CCM_CC23_AD_DATA_SIZE_MAX	(AES_BLOCK_SIZE - CCM_CC23_AD_LEN_SIZE)

#define CCM_CC23_TAG_SIZE_MIN		4
#define CCM_CC23_TAG_SIZE_MAX		16

#define CCM_CC23_BYTE_GET(idx, val)	FIELD_GET(0xff << ((idx) << 3), (val))

#define CCM_CC23_B0_GET(ad_len, tag_len, len_size) \
			(((ad_len) ? 1 << 6 : 0) + (((tag_len) - 2) << 2) + ((len_size) - 1))

/*
 * The Finite State Machine (FSM) processes the data in a column-fashioned way,
 * processing 2 columns/cycle, completing 10 rounds in 20 cycles. With three cycles
 * of pre-processing, the execution/encryption time is 23 cycles.
 */
#define CRYPTO_CC23_BLK_PROC_CYC	23
#define CRYPTO_CC23_BLK_PROC_TIMEOUT	(CRYPTO_CC23_BLK_PROC_CYC << 1)
#define CRYPTO_CC23_OP_TIMEOUT		K_CYC(CRYPTO_CC23_BLK_PROC_TIMEOUT)

#ifdef CONFIG_CRYPTO_CC23X0_DMA
#define CRYPTO_CC23_OP_TIMEOUT_DMA(len) \
	K_CYC(CRYPTO_CC23_BLK_PROC_TIMEOUT * ((len) / AES_BLOCK_SIZE))

#define CRYPTO_CC23_IS_INVALID_DATA_LEN_DMA(len)	((len) % AES_BLOCK_SIZE)

#define CRYPTO_CC23_REG_GET(offset)	(AES_BASE + (offset))

struct crypto_cc23x0_config {
	const struct device *dma_dev;
	uint8_t dma_channel_a;
	uint8_t dma_trigsrc_a;
	uint8_t dma_channel_b;
	uint8_t dma_trigsrc_b;
};
#endif

struct crypto_cc23x0_data {
	struct k_mutex device_mutex;
#ifdef CONFIG_CRYPTO_CC23X0_DMA
	struct k_sem cha_done;
	struct k_sem chb_done;
#else
	struct k_sem aes_done;
#endif
};

static void crypto_cc23x0_isr(const struct device *dev)
{
	struct crypto_cc23x0_data *data = dev->data;
	uint32_t status;

	status = AESGetMaskedInterruptStatus();

#ifdef CONFIG_CRYPTO_CC23X0_DMA
	if (status & AES_IMASK_CHADONE) {
		k_sem_give(&data->cha_done);
	} else if (status & AES_IMASK_CHBDONE) {
		k_sem_give(&data->chb_done);
	}
#else
	if (status & AES_IMASK_AESDONE) {
		k_sem_give(&data->aes_done);
	}
#endif

	AESClearInterrupt(status);
}

static void crypto_cc23x0_cleanup(const struct device *dev)
{
#ifdef CONFIG_CRYPTO_CC23X0_DMA
	const struct crypto_cc23x0_config *cfg = dev->config;

	dma_stop(cfg->dma_dev, cfg->dma_channel_b);
	dma_stop(cfg->dma_dev, cfg->dma_channel_a);
	AESDisableDMA();
#else
	ARG_UNUSED(dev);
#endif
	AESClearAUTOCFGTrigger();
	AESClearAUTOCFGBusHalt();
	AESClearTXTAndBUF();
}

static int crypto_cc23x0_ecb_encrypt(struct cipher_ctx *ctx, struct cipher_pkt *pkt)
{
	const struct device *dev = ctx->device;
	struct crypto_cc23x0_data *data = dev->data;
	int out_bytes_processed = 0;
	int ret;
#ifdef CONFIG_CRYPTO_CC23X0_DMA
	uint32_t int_flags = AES_IMASK_CHBDONE;
	const struct crypto_cc23x0_config *cfg = dev->config;

	struct dma_block_config block_cfg_cha = {
		.source_address = (uint32_t)(pkt->in_buf),
		.dest_address = CRYPTO_CC23_REG_GET(AES_O_DMACHA),
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
		.block_size = pkt->in_len,
	};

	struct dma_config dma_cfg_cha = {
		.dma_slot = cfg->dma_trigsrc_a,
		.channel_direction = MEMORY_TO_PERIPHERAL,
		.block_count = 1,
		.head_block = &block_cfg_cha,
		.source_data_size = sizeof(uint32_t),
		.dest_data_size = sizeof(uint32_t),
		.source_burst_length = AES_BLOCK_SIZE,
		.dma_callback = NULL,
		.user_data = NULL,
	};

	struct dma_block_config block_cfg_chb = {
		.source_address = CRYPTO_CC23_REG_GET(AES_O_DMACHB),
		.dest_address = (uint32_t)(pkt->out_buf),
		.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
		.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.block_size = pkt->in_len,
	};

	struct dma_config dma_cfg_chb = {
		.dma_slot = cfg->dma_trigsrc_b,
		.channel_direction = PERIPHERAL_TO_MEMORY,
		.block_count = 1,
		.head_block = &block_cfg_chb,
		.source_data_size = sizeof(uint32_t),
		.dest_data_size = sizeof(uint32_t),
		.source_burst_length = AES_BLOCK_SIZE,
		.dma_callback = NULL,
		.user_data = NULL,
	};
#else
	uint32_t int_flags = AES_IMASK_AESDONE;
	int in_bytes_processed = 0;
#endif

#ifdef CONFIG_CRYPTO_CC23X0_DMA
	if (CRYPTO_CC23_IS_INVALID_DATA_LEN_DMA(pkt->in_len)) {
		LOG_ERR("In DMA mode, data length must be a multiple of %d", AES_BLOCK_SIZE);
		return -EINVAL;
	}
#endif

	if (pkt->out_buf_max < ROUND_UP(pkt->in_len, AES_BLOCK_SIZE)) {
		LOG_ERR("Output buffer too small");
		return -EINVAL;
	}

	k_mutex_lock(&data->device_mutex, K_FOREVER);

	/* Enable interrupts */
	AESSetIMASK(int_flags);

	/* Load key */
	AESWriteKEY(ctx->key.bit_stream);

	/* Configure source buffer and encryption triggers */
	AESSetAUTOCFG(AES_AUTOCFG_AESSRC_BUF |
		      AES_AUTOCFG_TRGAES_RDTXT3 |
		      AES_AUTOCFG_TRGAES_WRBUF3S);

#ifdef CONFIG_CRYPTO_CC23X0_DMA
	/* Setup the DMA for the AES engine */
	AESSetupDMA(AES_DMA_ADRCHA_BUF0 |
		    AES_DMA_TRGCHA_AESSTART |
		    AES_DMA_ADRCHB_TXT0 |
		    AES_DMA_TRGCHB_AESDONE |
		    (pkt->in_len == AES_BLOCK_SIZE ?
		     AES_DMA_DONEACT_GATE_TRGAES_ON_CHA :
		     AES_DMA_DONEACT_GATE_TRGAES_ON_CHA_DEL));

	ret = dma_config(cfg->dma_dev, cfg->dma_channel_a, &dma_cfg_cha);
	if (ret) {
		goto cleanup;
	}

	ret = dma_config(cfg->dma_dev, cfg->dma_channel_b, &dma_cfg_chb);
	if (ret) {
		goto cleanup;
	}

	dma_start(cfg->dma_dev, cfg->dma_channel_a);
	dma_start(cfg->dma_dev, cfg->dma_channel_b);

	/* Trigger AES operation */
	AESSetTrigger(AES_TRG_DMACHA);

	/* Wait for AES operation completion */
	ret = k_sem_take(&data->chb_done, CRYPTO_CC23_OP_TIMEOUT_DMA(pkt->in_len));
	if (ret) {
		goto cleanup;
	}

	LOG_DBG("AES operation completed");

	out_bytes_processed = pkt->in_len;
#else
	/* Write first block of input to trigger encryption */
	AESWriteBUF(pkt->in_buf);
	in_bytes_processed += AES_BLOCK_SIZE;

	do {
		if (in_bytes_processed < pkt->in_len) {
			/* Preload next input block */
			AESWriteBUF(&pkt->in_buf[in_bytes_processed]);
			in_bytes_processed += AES_BLOCK_SIZE;
		} else {
			/* Avoid triggering a spurious encryption upon reading the final output */
			AESClearAUTOCFGTrigger();
		}

		/* Wait for AES operation completion */
		ret = k_sem_take(&data->aes_done, CRYPTO_CC23_OP_TIMEOUT);
		if (ret) {
			goto cleanup;
		}

		LOG_DBG("AES operation completed");

		/*
		 * Read output and trigger encryption of next input that was
		 * preloaded at the start of this loop.
		 */
		AESReadTXT(&pkt->out_buf[out_bytes_processed]);
		out_bytes_processed += AES_BLOCK_SIZE;
	} while (out_bytes_processed < pkt->in_len);
#endif

cleanup:
	crypto_cc23x0_cleanup(dev);
	k_mutex_unlock(&data->device_mutex);
	pkt->out_len = out_bytes_processed;

	return ret;
}

static int crypto_cc23x0_ctr(struct cipher_ctx *ctx, struct cipher_pkt *pkt, uint8_t *iv)
{
	const struct device *dev = ctx->device;
	struct crypto_cc23x0_data *data = dev->data;
	uint32_t ctr_len = ctx->mode_params.ctr_info.ctr_len >> 3;
	uint8_t ctr[AES_BLOCK_SIZE] = { 0 };
	int bytes_processed = 0;
	int iv_len;
	int ret;
#ifdef CONFIG_CRYPTO_CC23X0_DMA
	uint32_t int_flags = AES_IMASK_CHBDONE;
	const struct crypto_cc23x0_config *cfg = dev->config;

	struct dma_block_config block_cfg_cha = {
		.source_address = (uint32_t)(pkt->in_buf),
		.dest_address = CRYPTO_CC23_REG_GET(AES_O_DMACHA),
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
		.block_size = pkt->in_len,
	};

	struct dma_config dma_cfg_cha = {
		.dma_slot = cfg->dma_trigsrc_a,
		.channel_direction = MEMORY_TO_PERIPHERAL,
		.block_count = 1,
		.head_block = &block_cfg_cha,
		.source_data_size = sizeof(uint32_t),
		.dest_data_size = sizeof(uint32_t),
		.source_burst_length = AES_BLOCK_SIZE,
		.dma_callback = NULL,
		.user_data = NULL,
	};

	struct dma_block_config block_cfg_chb = {
		.source_address = CRYPTO_CC23_REG_GET(AES_O_DMACHB),
		.dest_address = (uint32_t)(pkt->out_buf),
		.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
		.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.block_size = pkt->in_len,
	};

	struct dma_config dma_cfg_chb = {
		.dma_slot = cfg->dma_trigsrc_b,
		.channel_direction = PERIPHERAL_TO_MEMORY,
		.block_count = 1,
		.head_block = &block_cfg_chb,
		.source_data_size = sizeof(uint32_t),
		.dest_data_size = sizeof(uint32_t),
		.source_burst_length = AES_BLOCK_SIZE,
		.dma_callback = NULL,
		.user_data = NULL,
	};
#else
	uint32_t int_flags = AES_IMASK_AESDONE;
	uint8_t last_buf[AES_BLOCK_SIZE] = { 0 };
	int bytes_remaining = pkt->in_len;
	int block_size;
#endif

#ifdef CONFIG_CRYPTO_CC23X0_DMA
	if (CRYPTO_CC23_IS_INVALID_DATA_LEN_DMA(pkt->in_len)) {
		LOG_ERR("In DMA mode, data length must be a multiple of %d", AES_BLOCK_SIZE);
		return -EINVAL;
	}
#endif

	if (pkt->out_buf_max < ROUND_UP(pkt->in_len, AES_BLOCK_SIZE)) {
		LOG_ERR("Output buffer too small");
		return -EINVAL;
	}

	k_mutex_lock(&data->device_mutex, K_FOREVER);

	/* Enable interrupts */
	AESSetIMASK(int_flags);

	/* Load key */
	AESWriteKEY(ctx->key.bit_stream);

	/* Configure source buffer and encryption triggers */
	AESSetAUTOCFG(AES_AUTOCFG_AESSRC_BUF |
		      AES_AUTOCFG_TRGAES_RDTXT3 |
		      AES_AUTOCFG_TRGAES_WRBUF3S |
		      AES_AUTOCFG_CTRENDN_BIGENDIAN |
		      AES_AUTOCFG_CTRSIZE_CTR128);

#ifdef CONFIG_CRYPTO_CC23X0_DMA
	/* Setup the DMA for the AES engine */
	AESSetupDMA(AES_DMA_ADRCHA_TXTX0 |
		    AES_DMA_TRGCHA_AESDONE |
		    AES_DMA_ADRCHB_TXT0 |
		    AES_DMA_TRGCHB_WRTXT3);

	ret = dma_config(cfg->dma_dev, cfg->dma_channel_a, &dma_cfg_cha);
	if (ret) {
		goto cleanup;
	}

	ret = dma_config(cfg->dma_dev, cfg->dma_channel_b, &dma_cfg_chb);
	if (ret) {
		goto cleanup;
	}

	dma_start(cfg->dma_dev, cfg->dma_channel_a);
	dma_start(cfg->dma_dev, cfg->dma_channel_b);
#endif

	/* Write the counter value to the AES engine to trigger first encryption */
	iv_len = (ctx->ops.cipher_mode == CRYPTO_CIPHER_MODE_CCM) ?
		  AES_BLOCK_SIZE : (ctx->keylen - ctr_len);
	memcpy(ctr, iv, iv_len);
	AESWriteBUF(ctr);

#ifdef CONFIG_CRYPTO_CC23X0_DMA
	/* Wait for AES operation completion */
	ret = k_sem_take(&data->chb_done, CRYPTO_CC23_OP_TIMEOUT_DMA(pkt->in_len));
	if (ret) {
		goto cleanup;
	}

	LOG_DBG("AES operation completed");

	bytes_processed = pkt->in_len;
#else
	do {
		/* Wait for AES operation completion */
		ret = k_sem_take(&data->aes_done, CRYPTO_CC23_OP_TIMEOUT);
		if (ret) {
			goto cleanup;
		}

		LOG_DBG("AES operation completed");

		/* XOR input data with encrypted counter block to form ciphertext */
		if (bytes_remaining > AES_BLOCK_SIZE) {
			block_size = AES_BLOCK_SIZE;
			AESWriteTXTXOR(&pkt->in_buf[bytes_processed]);
		} else {
			block_size = bytes_remaining;
			memcpy(last_buf, &pkt->in_buf[bytes_processed], block_size);
			AESWriteTXTXOR(last_buf);

			/*
			 * Do not auto-trigger encrypt and increment of counter
			 * value for last block of data.
			 */
			AESClearAUTOCFGTrigger();
		}

		/*
		 * Read the output ciphertext and trigger the encryption
		 * of the next counter block
		 */
		AESReadTXT(&pkt->out_buf[bytes_processed]);

		bytes_processed += block_size;
		bytes_remaining -= block_size;
	} while (bytes_remaining > 0);
#endif

cleanup:
	crypto_cc23x0_cleanup(dev);
	k_mutex_unlock(&data->device_mutex);
	pkt->out_len = bytes_processed;

	return ret;
}

static int crypto_cc23x0_cmac(struct cipher_ctx *ctx, struct cipher_pkt *pkt,
			      uint8_t *b0, uint8_t *b1)
{
	const struct device *dev = ctx->device;
	struct crypto_cc23x0_data *data = dev->data;
	uint32_t iv[AES_BLOCK_SIZE_WORDS] = { 0 };
	int bytes_processed = 0;
	int ret;
#ifdef CONFIG_CRYPTO_CC23X0_DMA
	uint32_t int_flags = AES_IMASK_CHADONE;
	const struct crypto_cc23x0_config *cfg = dev->config;

	struct dma_block_config block_cfg_cha = {
		.source_address = (uint32_t)b0,
		.dest_address = CRYPTO_CC23_REG_GET(AES_O_DMACHA),
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
		.block_size = AES_BLOCK_SIZE,
	};

	struct dma_config dma_cfg_cha = {
		.dma_slot = cfg->dma_trigsrc_a,
		.channel_direction = MEMORY_TO_PERIPHERAL,
		.block_count = 1,
		.head_block = &block_cfg_cha,
		.source_data_size = sizeof(uint32_t),
		.dest_data_size = sizeof(uint32_t),
		.source_burst_length = AES_BLOCK_SIZE,
		.dma_callback = NULL,
		.user_data = NULL,
	};
#else
	uint32_t int_flags = AES_IMASK_AESDONE;
	uint8_t last_buf[AES_BLOCK_SIZE] = { 0 };
	int bytes_remaining = pkt->in_len;
	int block_size;
#endif

#ifdef CONFIG_CRYPTO_CC23X0_DMA
	if (CRYPTO_CC23_IS_INVALID_DATA_LEN_DMA(pkt->in_len)) {
		LOG_ERR("In DMA mode, data length must be a multiple of %d", AES_BLOCK_SIZE);
		return -EINVAL;
	}
#endif

	if (pkt->out_buf_max < AES_BLOCK_SIZE) {
		LOG_ERR("Output buffer too small");
		return -EINVAL;
	}

	k_mutex_lock(&data->device_mutex, K_FOREVER);

	/* Enable interrupts */
	AESSetIMASK(int_flags);

	/* Load key */
	AESWriteKEY(ctx->key.bit_stream);

	/* Configure source buffer and encryption triggers */
	AESSetAUTOCFG(AES_AUTOCFG_AESSRC_TXTXBUF |
		      AES_AUTOCFG_TRGAES_WRBUF3 |
		      AES_AUTOCFG_BUSHALT_EN);

	/* Write zero'd IV */
	AESWriteIV32(iv);

	if (b0) {
#ifdef CONFIG_CRYPTO_CC23X0_DMA
		/* Setup the DMA for the AES engine */
		AESSetupDMA(AES_DMA_ADRCHA_BUF0 |
			    AES_DMA_TRGCHA_AESSTART);

		ret = dma_config(cfg->dma_dev, cfg->dma_channel_a, &dma_cfg_cha);
		if (ret) {
			goto out;
		}

		dma_start(cfg->dma_dev, cfg->dma_channel_a);

		/* Trigger AES operation */
		AESSetTrigger(AES_TRG_DMACHA);

		/* Wait for AES operation completion */
		ret = k_sem_take(&data->cha_done, CRYPTO_CC23_OP_TIMEOUT_DMA(AES_BLOCK_SIZE));
		if (ret) {
			goto out;
		}
#else
		/* Load input block */
		AESWriteBUF(b0);

		/* Wait for AES operation completion */
		ret = k_sem_take(&data->aes_done, CRYPTO_CC23_OP_TIMEOUT);
		if (ret) {
			goto out;
		}
#endif
		LOG_DBG("AES operation completed (block 0)");
	}

	if (b1) {
#ifdef CONFIG_CRYPTO_CC23X0_DMA
		block_cfg_cha.source_address = (uint32_t)b1;

		ret = dma_config(cfg->dma_dev, cfg->dma_channel_a, &dma_cfg_cha);
		if (ret) {
			goto out;
		}

		dma_start(cfg->dma_dev, cfg->dma_channel_a);

		/* Trigger AES operation */
		AESSetTrigger(AES_TRG_DMACHA);

		/* Wait for AES operation completion */
		ret = k_sem_take(&data->cha_done, CRYPTO_CC23_OP_TIMEOUT_DMA(AES_BLOCK_SIZE));
		if (ret) {
			goto out;
		}
#else
		/* Load input block */
		AESWriteBUF(b1);

		/* Wait for AES operation completion */
		ret = k_sem_take(&data->aes_done, CRYPTO_CC23_OP_TIMEOUT);
		if (ret) {
			goto out;
		}
#endif
		LOG_DBG("AES operation completed (block 1)");
	}

#ifdef CONFIG_CRYPTO_CC23X0_DMA
	block_cfg_cha.source_address = (uint32_t)(pkt->in_buf);
	block_cfg_cha.block_size = pkt->in_len;

	ret = dma_config(cfg->dma_dev, cfg->dma_channel_a, &dma_cfg_cha);
	if (ret) {
		goto out;
	}

	dma_start(cfg->dma_dev, cfg->dma_channel_a);

	/* Trigger AES operation */
	AESSetTrigger(AES_TRG_DMACHA);

	/* Wait for AES operation completion */
	ret = k_sem_take(&data->cha_done, CRYPTO_CC23_OP_TIMEOUT_DMA(pkt->in_len));
	if (ret) {
		goto out;
	}

	LOG_DBG("AES operation completed (data)");

	bytes_processed = pkt->in_len;
#else
	do {
		/* Load input block */
		if (bytes_remaining >= AES_BLOCK_SIZE) {
			block_size = AES_BLOCK_SIZE;
			AESWriteBUF(&pkt->in_buf[bytes_processed]);
		} else {
			block_size = bytes_remaining;
			memcpy(last_buf, &pkt->in_buf[bytes_processed], block_size);
			AESWriteBUF(last_buf);
		}

		/* Wait for AES operation completion */
		ret = k_sem_take(&data->aes_done, CRYPTO_CC23_OP_TIMEOUT);
		if (ret) {
			goto out;
		}

		LOG_DBG("AES operation completed (data)");

		bytes_processed += block_size;
		bytes_remaining -= block_size;
	} while (bytes_remaining > 0);
#endif

	/* Read tag */
	AESReadTag(pkt->out_buf);

out:
	crypto_cc23x0_cleanup(dev);
	k_mutex_unlock(&data->device_mutex);
	pkt->out_len = bytes_processed;

	return ret;
}

static int crypto_cc23x0_ccm_check_param(struct cipher_ctx *ctx, struct cipher_aead_pkt *aead_op)
{
	uint16_t ad_len = aead_op->ad_len;
	uint16_t tag_len = ctx->mode_params.ccm_info.tag_len;
	uint16_t nonce_len = ctx->mode_params.ccm_info.nonce_len;

	if (aead_op->pkt->out_buf_max < ROUND_UP(aead_op->pkt->in_len, AES_BLOCK_SIZE)) {
		LOG_ERR("Output buffer too small");
		return -EINVAL;
	}

	if (tag_len < CCM_CC23_TAG_SIZE_MIN || tag_len > CCM_CC23_TAG_SIZE_MAX || tag_len & 1) {
		LOG_ERR("CCM parameter invalid (tag_len must be an even value from %d to %d)",
			CCM_CC23_TAG_SIZE_MIN, CCM_CC23_TAG_SIZE_MAX);
		return -EINVAL;
	}

	if (nonce_len < CCM_CC23_NONCE_LEN_SIZE_MIN || nonce_len > CCM_CC23_NONCE_LEN_SIZE_MAX) {
		LOG_ERR("CCM parameter invalid (nonce_len must be a value from %d to %d)",
			CCM_CC23_NONCE_LEN_SIZE_MIN, CCM_CC23_NONCE_LEN_SIZE_MAX);
		return -EINVAL;
	}

	if (ad_len > CCM_CC23_AD_DATA_SIZE_MAX) {
		LOG_ERR("CCM parameter invalid (ad_len max = %d)", CCM_CC23_AD_DATA_SIZE_MAX);
		return -EINVAL;
	}

	return 0;
}

static int crypto_cc23x0_ccm_encrypt(struct cipher_ctx *ctx,
				     struct cipher_aead_pkt *aead_op, uint8_t *nonce)
{
	struct cipher_pkt tag_pkt = { 0 };
	struct cipher_pkt data_pkt = { 0 };
	uint8_t tag[AES_BLOCK_SIZE] = { 0 };
	uint8_t b0[AES_BLOCK_SIZE] = { 0 };
	uint8_t b1[AES_BLOCK_SIZE] = { 0 };
	uint8_t ctri[AES_BLOCK_SIZE] = { 0 };
	uint32_t msg_len = aead_op->pkt->in_len;
	uint16_t ad_len = aead_op->ad_len;
	uint16_t tag_len = ctx->mode_params.ccm_info.tag_len;
	uint16_t nonce_len = ctx->mode_params.ccm_info.nonce_len;
	uint8_t len_size = AES_BLOCK_SIZE - nonce_len - 1;
	int ret;
	int i;

	ret = crypto_cc23x0_ccm_check_param(ctx, aead_op);
	if (ret) {
		return ret;
	}

	/*
	 * Build the first block B0 required for CMAC computation.
	 * ============================================
	 * Block B0 formatting per RFC3610:
	 *    Octet Number   Contents
	 *    ------------   ---------
	 *    0		     Flags
	 *    1 ... 15-L     Nonce N
	 *    16-L ... 15    l(m), MSB first
	 *
	 * Flags in octet 0 of B0:
	 *    Bit Number   Contents
	 *    ----------   ----------------------
	 *    7		   Reserved (always zero)
	 *    6		   Adata = 1 if l(a) > 0, 0 otherwise
	 *    5 ... 3	   M' = (M - 2) / 2 where M = Number of octets in authentication field
	 *    2 ... 0	   L' = L - 1 where L = Number of octets in length field
	 * ============================================
	 */
	b0[0] = CCM_CC23_B0_GET(aead_op->ad_len, tag_len, len_size);

	for (i = 0 ; i < sizeof(msg_len) ; i++) {
		b0[AES_BLOCK_SIZE - 1 - i] = CCM_CC23_BYTE_GET(i, msg_len);
	}

	memcpy(&b0[1], nonce, nonce_len);

	/*
	 * Build the second block B1 for additional data (header).
	 * ============================================
	 * Block B1 formatting per RFC3610, for 0 < l(a) < (2^16 - 2^8):
	 *    Octet Number   Contents
	 *    ------------   ---------
	 *    0 ... 1	     l(a), MSB first
	 *    2 ... N	     Header data
	 *    N+1 ... 15     Zero padding
	 * ============================================
	 */
	for (i = 0 ; i < sizeof(ad_len) ; i++) {
		b1[CCM_CC23_AD_LEN_SIZE - 1 - i] = CCM_CC23_BYTE_GET(i, ad_len);
	}

	memcpy(&b1[CCM_CC23_AD_LEN_SIZE], aead_op->ad, ad_len);

	/* Calculate the authentication tag by passing B0, B1, and data to CMAC function. */
	LOG_DBG("Compute CMAC");

	data_pkt.in_buf = aead_op->pkt->in_buf;
	data_pkt.in_len = aead_op->pkt->in_len;
	data_pkt.out_buf = tag;
	data_pkt.out_buf_max = AES_BLOCK_SIZE;

	ret = crypto_cc23x0_cmac(ctx, &data_pkt, b0, b1);
	if (ret) {
		return ret;
	}

	/*
	 * Prepare the initial counter block CTR1 for the CTR mode.
	 * ============================================
	 * Block CTRi formatting per RFC3610:
	 *    Octet Number   Contents
	 *    ------------   ---------
	 *    0              Flags
	 *    1 ... 15-L     Nonce N
	 *    16-L ... 15    Counter i, MSB first
	 *
	 * Flags in octet 0 of CTR0:
	 *    Bit Number   Contents
	 *    ----------   ----------------------
	 *    7            Reserved (always zero)
	 *    6            Reserved (always zero)
	 *    5 ... 3      Zero
	 *    2 ... 0      L' = L - 1 where L = Number of octets in length field
	 * ============================================
	 */
	ctri[0] = len_size - 1;
	memcpy(&ctri[1], nonce, nonce_len);
	ctri[AES_BLOCK_SIZE - 1] = 1;

	/* Encrypt the data using the counter block CTR1. */
	LOG_DBG("Encrypt data");

	ret = crypto_cc23x0_ctr(ctx, aead_op->pkt, ctri);
	if (ret) {
		return ret;
	}

	/* Encrypt the authentication tag using the counter block CTR0. */
	LOG_DBG("Encrypt tag");

	ctri[AES_BLOCK_SIZE - 1] = 0;

	tag_pkt.in_buf = tag;
	tag_pkt.in_len = tag_len;
	tag_pkt.out_buf = aead_op->tag;
	tag_pkt.out_buf_max = AES_BLOCK_SIZE;

	return crypto_cc23x0_ctr(ctx, &tag_pkt, ctri);
}

static int crypto_cc23x0_ccm_decrypt(struct cipher_ctx *ctx,
				     struct cipher_aead_pkt *aead_op, uint8_t *nonce)
{
	struct cipher_pkt tag_pkt = { 0 };
	struct cipher_pkt data_pkt = { 0 };
	uint8_t enc_tag[AES_BLOCK_SIZE] = { 0 };
	uint8_t calc_tag[AES_BLOCK_SIZE] = { 0 };
	uint8_t b0[AES_BLOCK_SIZE] = { 0 };
	uint8_t b1[AES_BLOCK_SIZE] = { 0 };
	uint8_t ctri[AES_BLOCK_SIZE] = { 0 };
	uint32_t msg_len = aead_op->pkt->in_len;
	uint16_t ad_len = aead_op->ad_len;
	uint16_t tag_len = ctx->mode_params.ccm_info.tag_len;
	uint16_t nonce_len = ctx->mode_params.ccm_info.nonce_len;
	uint8_t len_size = AES_BLOCK_SIZE - nonce_len - 1;
	int ret;
	int i;

	ret = crypto_cc23x0_ccm_check_param(ctx, aead_op);
	if (ret) {
		return ret;
	}

	/* Prepare the initial counter block CTR1 for the CTR mode. */
	ctri[0] = len_size - 1;
	memcpy(&ctri[1], nonce, nonce_len);
	ctri[AES_BLOCK_SIZE - 1] = 1;

	/* Decrypt the data using the counter block CTR1. */
	LOG_DBG("Decrypt data");

	ret = crypto_cc23x0_ctr(ctx, aead_op->pkt, ctri);
	if (ret) {
		goto clear_out_buf;
	}

	/* Build the first block B0 required for CMAC computation. */
	b0[0] = CCM_CC23_B0_GET(aead_op->ad_len, tag_len, len_size);

	for (i = 0 ; i < sizeof(msg_len) ; i++) {
		b0[AES_BLOCK_SIZE - 1 - i] = CCM_CC23_BYTE_GET(i, msg_len);
	}

	memcpy(&b0[1], nonce, nonce_len);

	/* Build the second block B1 for additional data (header). */
	for (i = 0 ; i < sizeof(ad_len) ; i++) {
		b1[CCM_CC23_AD_LEN_SIZE - 1 - i] = CCM_CC23_BYTE_GET(i, ad_len);
	}

	memcpy(&b1[CCM_CC23_AD_LEN_SIZE], aead_op->ad, ad_len);

	/*
	 * Calculate the authentication tag by passing B0, B1, and decrypted data
	 * to CMAC function.
	 */
	LOG_DBG("Compute CMAC");

	data_pkt.in_buf = aead_op->pkt->out_buf;
	data_pkt.in_len = aead_op->pkt->out_len;
	data_pkt.out_buf = calc_tag;
	data_pkt.out_buf_max = AES_BLOCK_SIZE;

	ret = crypto_cc23x0_cmac(ctx, &data_pkt, b0, b1);
	if (ret) {
		goto clear_out_buf;
	}

	/* Encrypt the recalculated authentication tag using the counter block CTR0. */
	LOG_DBG("Encrypt tag");

	ctri[AES_BLOCK_SIZE - 1] = 0;

	tag_pkt.in_buf = calc_tag;
	tag_pkt.in_len = tag_len;
	tag_pkt.out_buf = enc_tag;
	tag_pkt.out_buf_max = AES_BLOCK_SIZE;

	ret = crypto_cc23x0_ctr(ctx, &tag_pkt, ctri);
	if (ret) {
		goto clear_out_buf;
	}

	/*
	 * Compare the recalculated encrypted authentication tag with the one supplied by the app.
	 * If they match, the decrypted data is returned. Otherwise, the authentication has failed
	 * and the output buffer is zero'd.
	 */
	LOG_DBG("Check tag");

	if (!memcmp(enc_tag, aead_op->tag, tag_len)) {
		return 0;
	}

	LOG_ERR("Invalid tag");
	ret = -EINVAL;

clear_out_buf:
	memset(aead_op->pkt->out_buf, 0, msg_len);

	return ret;
}

static int crypto_cc23x0_session_setup(const struct device *dev,
				       struct cipher_ctx *ctx,
				       enum cipher_algo algo,
				       enum cipher_mode mode,
				       enum cipher_op op_type)
{
	if (ctx->flags & ~(CRYPTO_CC23_CAP)) {
		LOG_ERR("Unsupported feature");
		return -EINVAL;
	}

	if (algo != CRYPTO_CIPHER_ALGO_AES) {
		LOG_ERR("Unsupported algo");
		return -EINVAL;
	}

	if (mode != CRYPTO_CIPHER_MODE_ECB &&
	    mode != CRYPTO_CIPHER_MODE_CTR &&
	    mode != CRYPTO_CIPHER_MODE_CCM) {
		LOG_ERR("Unsupported mode");
		return -EINVAL;
	}

	if (ctx->keylen != 16U) {
		LOG_ERR("%u key size is not supported", ctx->keylen);
		return -EINVAL;
	}

	if (!ctx->key.bit_stream) {
		LOG_ERR("No key provided");
		return -EINVAL;
	}

	if (op_type == CRYPTO_CIPHER_OP_ENCRYPT) {
		switch (mode) {
		case CRYPTO_CIPHER_MODE_ECB:
			ctx->ops.block_crypt_hndlr = crypto_cc23x0_ecb_encrypt;
			break;
		case CRYPTO_CIPHER_MODE_CTR:
			ctx->ops.ctr_crypt_hndlr = crypto_cc23x0_ctr;
			break;
		case CRYPTO_CIPHER_MODE_CCM:
			ctx->ops.ccm_crypt_hndlr = crypto_cc23x0_ccm_encrypt;
			break;
		default:
			return -EINVAL;
		}
	} else {
		switch (mode) {
		case CRYPTO_CIPHER_MODE_ECB:
			LOG_ERR("ECB decryption not supported by the hardware");
			return -EINVAL;
		case CRYPTO_CIPHER_MODE_CTR:
			ctx->ops.ctr_crypt_hndlr = crypto_cc23x0_ctr;
			break;
		case CRYPTO_CIPHER_MODE_CCM:
			ctx->ops.ccm_crypt_hndlr = crypto_cc23x0_ccm_decrypt;
			break;
		default:
			return -EINVAL;
		}
	}

	ctx->ops.cipher_mode = mode;
	ctx->device = dev;

	return 0;
}

static int crypto_cc23x0_session_free(const struct device *dev,
				      struct cipher_ctx *ctx)
{
	ARG_UNUSED(dev);

	ctx->ops.ccm_crypt_hndlr = NULL;
	ctx->device = NULL;

	return 0;
}

static int crypto_cc23x0_query_caps(const struct device *dev)
{
	ARG_UNUSED(dev);

	return CRYPTO_CC23_CAP;
}

static int crypto_cc23x0_init(const struct device *dev)
{
#ifdef CONFIG_CRYPTO_CC23X0_DMA
	const struct crypto_cc23x0_config *cfg = dev->config;
#endif
	struct crypto_cc23x0_data *data = dev->data;

	IRQ_CONNECT(DT_INST_IRQN(0),
		    DT_INST_IRQ(0, priority),
		    crypto_cc23x0_isr,
		    DEVICE_DT_INST_GET(0),
		    0);
	irq_enable(DT_INST_IRQN(0));

	CLKCTLEnable(CLKCTL_BASE, CLKCTL_LAES);

	k_mutex_init(&data->device_mutex);

#ifdef CONFIG_CRYPTO_CC23X0_DMA
	k_sem_init(&data->cha_done, 0, 1);
	k_sem_init(&data->chb_done, 0, 1);

	if (!device_is_ready(cfg->dma_dev)) {
		return -ENODEV;
	}
#else
	k_sem_init(&data->aes_done, 0, 1);
#endif

	return 0;
}

static DEVICE_API(crypto, crypto_enc_funcs) = {
	.cipher_begin_session = crypto_cc23x0_session_setup,
	.cipher_free_session = crypto_cc23x0_session_free,
	.query_hw_caps = crypto_cc23x0_query_caps,
};

static struct crypto_cc23x0_data crypto_cc23x0_dev_data;

#ifdef CONFIG_CRYPTO_CC23X0_DMA
static const struct crypto_cc23x0_config crypto_cc23x0_dev_config = {
	.dma_dev = DEVICE_DT_GET(TI_CC23X0_DT_INST_DMA_CTLR(0, cha)),
	.dma_channel_a = TI_CC23X0_DT_INST_DMA_CHANNEL(0, cha),
	.dma_trigsrc_a = TI_CC23X0_DT_INST_DMA_TRIGSRC(0, cha),
	.dma_channel_b = TI_CC23X0_DT_INST_DMA_CHANNEL(0, chb),
	.dma_trigsrc_b = TI_CC23X0_DT_INST_DMA_TRIGSRC(0, chb),
};

DEVICE_DT_INST_DEFINE(0,
		      crypto_cc23x0_init,
		      NULL,
		      &crypto_cc23x0_dev_data,
		      &crypto_cc23x0_dev_config,
		      POST_KERNEL,
		      CONFIG_CRYPTO_INIT_PRIORITY,
		      &crypto_enc_funcs);
#else
DEVICE_DT_INST_DEFINE(0,
		      crypto_cc23x0_init,
		      NULL,
		      &crypto_cc23x0_dev_data,
		      NULL,
		      POST_KERNEL,
		      CONFIG_CRYPTO_INIT_PRIORITY,
		      &crypto_enc_funcs);
#endif
