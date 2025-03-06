/** @file
 *@brief Driver for Xilinx AXI DMA.
 */
/*
 * Copyright (c) 2024 CISPA Helmholtz Center for Information Security gGmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/cache.h>

#include "dma_xilinx_axi_dma.h"

#define XILINX_AXI_DMA_SG_DESCRIPTOR_ADDRESS_MASK 0x3f

LOG_MODULE_REGISTER(dma_xilinx_axi_dma, CONFIG_DMA_LOG_LEVEL);

/* masks for control field in SG descriptor */
#define XILINX_AXI_DMA_SG_DESCRIPTOR_CTRL_RESERVED_MASK 0xF0000000
/* descriptor is for start of transfer */
#define XILINX_AXI_DMA_SG_DESCRIPTOR_CTRL_SOF_MASK      0x08000000
/* descriptor is for end of transfer */
#define XILINX_AXI_DMA_SG_DESCRIPTOR_CTRL_EOF_MASK      0x04000000
/* length of the associated buffer in main memory */
#define XILINX_AXI_DMA_SG_DESCRIPTOR_CTRL_LENGTH_MASK   0x03FFFFFF
#define XILINX_AXI_DMA_SG_DESCRIPTOR_STATUS_LENGTH_MASK 0x03FFFFFF

/* masks for status field in SG descriptor */
/* transfer completed */
#define XILINX_AXI_DMA_SG_DESCRIPTOR_STATUS_COMPLETE_MASK    0x80000000
/* decode error, i.e., DECERR on AXI bus from memory */
#define XILINX_AXI_DMA_SG_DESCRIPTOR_STATUS_DEC_ERR_MASK     0x40000000
/* slave error, i.e., SLVERR on AXI bus from memory */
#define XILINX_AXI_DMA_SG_DESCRIPTOR_STATUS_SLV_ERR_MASK     0x20000000
/* internal DMA error, e.g., 0-length transfer */
#define XILINX_AXI_DMA_SG_DESCRIPTOR_STATUS_INT_ERR_MASK     0x10000000
/* reserved */
#define XILINX_AXI_DMA_SG_DESCRIPTOR_STATUS_INT_RES_MASK     0x0C000000
/* number of transferred bytes */
#define XILINX_AXI_DMA_SG_DESCRIPTOR_STATUS_TRANSFERRED_MASK 0x03FFFFFF

#define XILINX_AXI_DMA_SG_DESCRIPTOR_APP0_CHECKSUM_OFFLOAD_FULL 0x00000002
#define XILINX_AXI_DMA_SG_DESCRIPTOR_APP0_CHECKSUM_OFFLOAD_NONE 0x00000000
#define XILINX_AXI_DMA_SG_DESCRIPTOR_APP2_FCS_ERR_MASK          0x00000100
#define XILINX_AXI_DMA_SG_DESCRIPTOR_APP2_IP_ERR_MASK           0x00000028
#define XILINX_AXI_DMA_SG_DESCRIPTOR_APP2_UDP_ERR_MASK          0x00000030
#define XILINX_AXI_DMA_SG_DESCRIPTOR_APP2_TCP_ERR_MASK          0x00000038

/* masks for DMA registers */

#define XILINX_AXI_DMA_REGS_DMACR_IRQTHRESH_SHIFT_BITS 16
#define XILINX_AXI_DMA_REGS_DMACR_IRQDELAY_SHIFT_BITS  24
/* masks for DMACR register */
/* interrupt timeout - trigger interrupt after X cycles when no transfer. Unit is 125 * */
/* clock_period. */
#define XILINX_AXI_DMA_REGS_DMACR_IRQDELAY             0xFF000000
/* irqthreshold - this can be used to generate interrupts after X completed packets */
/* instead of after every packet */
#define XILINX_AXI_DMA_REGS_DMACR_IRQTHRESH            0x00FF0000
#define XILINX_AXI_DMA_REGS_DMACR_RESERVED1            0x00008000
/* interrupt on error enable */
#define XILINX_AXI_DMA_REGS_DMACR_ERR_IRQEN            0x00004000
/* interrupt on delay timer interrupt enable */
#define XILINX_AXI_DMA_REGS_DMACR_DLY_IRQEN            0x00002000
/* interrupt on complete enable */
#define XILINX_AXI_DMA_REGS_DMACR_IOC_IRQEN            0x00001000
#define XILINX_AXI_DMA_REGS_DMACR_ALL_IRQEN                                                        \
	(XILINX_AXI_DMA_REGS_DMACR_ERR_IRQEN | XILINX_AXI_DMA_REGS_DMACR_DLY_IRQEN |               \
	 XILINX_AXI_DMA_REGS_DMACR_IOC_IRQEN)
#define XILINX_AXI_DMA_REGS_DMACR_RESERVED2 0x00000FE0
/* DMA ignores completed bit in SG descriptor and overwrites descriptors */
#define XILINX_AXI_DMA_REGS_DMACR_CYC_BD_EN 0x00000010
/* use AXI fixed burst instead of incrementing burst for TX transfers, e.g., useful for reading a */
/* FIFO */
#define XILINX_AXI_DMA_REGS_DMACR_KEYHOLE   0x00000008
/* soft reset */
#define XILINX_AXI_DMA_REGS_DMACR_RESET     0x00000004
#define XILINX_AXI_DMA_REGS_DMACR_RESERVED3 0x00000002
/* run-stop */
#define XILINX_AXI_DMA_REGS_DMACR_RS        0x00000001

/* masks for DMASR register */
/* interrupt delay time status */
#define XILINX_AXI_DMA_REGS_DMASR_IRQDELAYSTS  0xFF000000
/* interrupt threshold status */
#define XILINX_AXI_DMA_REGS_DMASR_IRQTHRESHSTS 0x00FF0000
#define XILINX_AXI_DMA_REGS_DMASR_RESERVED1    0x00008000
/* current interrupt was generated on error */
#define XILINX_AXI_DMA_REGS_DMASR_ERR_IRQ      0x00004000
/* current interrupt was generated by timoeout */
#define XILINX_AXI_DMA_REGS_DMASR_DLY_IRQ      0x00002000
/* current interrupt was generated by completion of a transfer */
#define XILINX_AXI_DMA_REGS_DMASR_IOC_IRQ      0x00001000
#define XILINX_AXI_DMA_REGS_DMASR_RESERVED2    0x00000800
/* scatter gather decode error */
#define XILINX_AXI_DMA_REGS_DMASR_SGDECERR     0x00000400
/* scatter gather slave error */
#define XILINX_AXI_DMA_REGS_DMASR_SGSLVERR     0x00000200
/* scatter gather internal error, i.e., fetched a descriptor with complete bit already set */
#define XILINX_AXI_DMA_REGS_DMASR_SGINTERR     0x00000100
#define XILINX_AXI_DMA_REGS_DMASR_RESERVED3    0x00000080
/* DMA decode error */
#define XILINX_AXI_DMA_REGS_DMASR_DMADECERR    0x00000040
/* DMA slave error */
#define XILINX_AXI_DMA_REGS_DMASR_SLVERR       0x00000020
/* DMA internal error */
#define XILINX_AXI_DMA_REGS_DMASR_INTERR       0x00000010
/* scatter/gather support enabled at build time */
#define XILINX_AXI_DMA_REGS_DMASR_SGINCL       0x00000008
#define XILINX_AXI_DMA_REGS_DMASR_RESERVED4    0x00000004
/* DMA channel is idle, i.e., DMA operations completed; writing tail restarts operation */
#define XILINX_AXI_DMA_REGS_DMASR_IDLE         0x00000002
/* RS (run-stop) in DMACR is 0 and operations completed; writing tail does nothing */
#define XILINX_AXI_DMA_REGS_DMASR_HALTED       0x00000001

#define XILINX_AXI_DMA_REGS_SG_CTRL_CACHE_MASK 0x0000000F
#define XILINX_AXI_DMA_REGS_SG_CTRL_RES1_MASK  0x000000F0
#define XILINX_AXI_DMA_REGS_SG_CTRL_USER_MASK  0x00000F00
#define XILINX_AXI_DMA_REGS_SG_CTRL_RES2_MASK  0xFFFFF000

static inline void dma_xilinx_axi_dma_flush_dcache(void *addr, size_t len)
{
#ifdef CONFIG_DMA_XILINX_AXI_DMA_DISABLE_CACHE_WHEN_ACCESSING_SG_DESCRIPTORS
	sys_cache_data_flush_range(addr, len);
#endif
}
static inline void dma_xilinx_axi_dma_invd_dcache(void *addr, size_t len)
{
#ifdef CONFIG_DMA_XILINX_AXI_DMA_DISABLE_CACHE_WHEN_ACCESSING_SG_DESCRIPTORS
	sys_cache_data_invd_range(addr, len);
#endif
}

/* in-memory descriptor, read by the DMA, that instructs it how many bits to transfer from which */
/* buffer */
struct __attribute__((__packed__)) dma_xilinx_axi_dma_sg_descriptor {
	/* next descriptor[31:6], bits 5-0 reserved */
	uint32_t nxtdesc;
	/* next descriptor[63:32] */
	uint32_t nxtdesc_msb;
	/* address of buffer to transfer[31:0] */
	uint32_t buffer_address;
	/* address of buffer to transfer[63:32] */
	uint32_t buffer_address_msb;
	uint32_t reserved1;
	uint32_t reserved2;

	/* bitfield, masks for access defined above */
	uint32_t control;
	/* bitfield, masks for access defined above */
	uint32_t status;

	/* application-specific fields used, e.g., to enable checksum offloading */
	/* for the Ethernet Subsystem */
	uint32_t app0;
	uint32_t app1;
	uint32_t app2;
	uint32_t app3;
	uint32_t app4;
} __aligned(64);

enum AxiDmaDirectionRegister {
	/* DMA control register */
	/* bitfield, masks defined above */
	XILINX_AXI_DMA_REG_DMACR = 0x00,
	/* DMA status register */
	/* bitfield, masks defined above */
	XILINX_AXI_DMA_REG_DMASR = 0x04,
	/* current descriptor address[31:0] */
	XILINX_AXI_DMA_REG_CURDESC = 0x08,
	/* current descriptor address[63:32] */
	XILINX_AXI_DMA_REG_CURDESC_MSB = 0x0C,
	/* current descriptor address[31:0] */
	XILINX_AXI_DMA_REG_TAILDESC = 0x10,
	/* current descriptor address[63:32] */
	XILINX_AXI_DMA_REG_TAILDESC_MSB = 0x14,
};

#define XILINX_AXI_DMA_MM2S_REG_OFFSET 0x00
#define XILINX_AXI_DMA_S2MM_REG_OFFSET 0x30

struct dma_xilinx_axi_dma_data;

/* global configuration per DMA device */
struct dma_xilinx_axi_dma_config {
	mm_reg_t reg;
	/* this should always be 2 - one for TX, one for RX */
	uint32_t channels;
	void (*irq_configure)(struct dma_xilinx_axi_dma_data *data);
};

typedef void (*dma_xilinx_axi_dma_isr_t)(const struct device *dev);

/* per-channel state */
struct dma_xilinx_axi_dma_channel {
	volatile struct dma_xilinx_axi_dma_sg_descriptor *descriptors;

	size_t num_descriptors;

	/* Last descriptor populated with pending transfer */
	size_t populated_desc_index;

	/* Next descriptor to check for completion by HW */
	size_t completion_desc_index;

	mm_reg_t channel_regs;

	uint32_t irq;

	enum dma_channel_direction direction;

	/* call this when the transfer is complete */
	dma_callback_t completion_callback;
	void *completion_callback_user_data;

	uint32_t last_rx_size;

	uint32_t sg_desc_app0;
	bool check_csum_in_isr;
};

/* global state for device and array of per-channel states */
struct dma_xilinx_axi_dma_data {
	struct dma_context ctx;
	struct dma_xilinx_axi_dma_channel *channels;

	__aligned(64) struct dma_xilinx_axi_dma_sg_descriptor
		descriptors_tx[CONFIG_DMA_XILINX_AXI_DMA_SG_DESCRIPTOR_NUM_TX];
	__aligned(64) struct dma_xilinx_axi_dma_sg_descriptor
		descriptors_rx[CONFIG_DMA_XILINX_AXI_DMA_SG_DESCRIPTOR_NUM_RX];
};

static inline int dma_xilinx_axi_dma_lock_irq(const struct device *dev, const uint32_t channel_num)
{
	const struct dma_xilinx_axi_dma_data *data = dev->data;
	int ret;

	if (IS_ENABLED(CONFIG_DMA_XILINX_AXI_DMA_LOCK_ALL_IRQS)) {
		ret = irq_lock();
	} else if (IS_ENABLED(CONFIG_DMA_XILINX_AXI_DMA_LOCK_DMA_IRQS)) {
		/* TX is 0, RX is 1 */
		ret = irq_is_enabled(data->channels[0].irq) ? 1 : 0;
		ret |= (irq_is_enabled(data->channels[1].irq) ? 1 : 0) << 1;

		LOG_DBG("DMA IRQ state: %x TX IRQN: %" PRIu32 " RX IRQN: %" PRIu32, ret,
			data->channels[0].irq, data->channels[1].irq);

		irq_disable(data->channels[0].irq);
		irq_disable(data->channels[1].irq);
	} else {
		/* CONFIG_DMA_XILINX_AXI_DMA_LOCK_CHANNEL_IRQ */
		ret = irq_is_enabled(data->channels[channel_num].irq);

		LOG_DBG("DMA IRQ state: %x ", ret);

		irq_disable(data->channels[channel_num].irq);
	}

	return ret;
}

static inline void dma_xilinx_axi_dma_unlock_irq(const struct device *dev,
						 const uint32_t channel_num, int key)
{
	const struct dma_xilinx_axi_dma_data *data = dev->data;

	if (IS_ENABLED(CONFIG_DMA_XILINX_AXI_DMA_LOCK_ALL_IRQS)) {
		irq_unlock(key);
	} else if (IS_ENABLED(CONFIG_DMA_XILINX_AXI_DMA_LOCK_DMA_IRQS)) {
		if (key & 0x1) {
			/* TX was enabled */
			irq_enable(data->channels[0].irq);
		}
		if (key & 0x2) {
			/* RX was enabled */
			irq_enable(data->channels[1].irq);
		}
	} else {
		/* CONFIG_DMA_XILINX_AXI_DMA_LOCK_CHANNEL_IRQ */
		if (key) {
			/* was enabled */
			irq_enable(data->channels[channel_num].irq);
		}
	}
}

static void dma_xilinx_axi_dma_write_reg(const struct dma_xilinx_axi_dma_channel *channel_data,
					 enum AxiDmaDirectionRegister reg, uint32_t val)
{
	sys_write32(val, channel_data->channel_regs + reg);
}

static uint32_t dma_xilinx_axi_dma_read_reg(const struct dma_xilinx_axi_dma_channel *channel_data,
					    enum AxiDmaDirectionRegister reg)
{
	return sys_read32(channel_data->channel_regs + reg);
}

uint32_t dma_xilinx_axi_dma_last_received_frame_length(const struct device *dev)
{
	const struct dma_xilinx_axi_dma_data *data = dev->data;

	return data->channels[XILINX_AXI_DMA_RX_CHANNEL_NUM].last_rx_size;
}

static int
dma_xilinx_axi_dma_clean_up_sg_descriptors(const struct device *dev,
					   struct dma_xilinx_axi_dma_channel *channel_data,
					   const char *chan_name)
{
	volatile struct dma_xilinx_axi_dma_sg_descriptor *current_descriptor =
		&channel_data->descriptors[channel_data->completion_desc_index];
	unsigned int processed_packets = 0;
	uint32_t current_status;

	dma_xilinx_axi_dma_invd_dcache((void *)current_descriptor, sizeof(*current_descriptor));
	current_status = current_descriptor->status;

	while (current_status & ~XILINX_AXI_DMA_SG_DESCRIPTOR_STATUS_TRANSFERRED_MASK) {
		/* descriptor completed or errored out - need to call callback */
		int retval = DMA_STATUS_COMPLETE;

		/* this is meaningless / ignored for TX channel */
		channel_data->last_rx_size =
			current_status & XILINX_AXI_DMA_SG_DESCRIPTOR_STATUS_LENGTH_MASK;

		if (current_status & XILINX_AXI_DMA_SG_DESCRIPTOR_STATUS_DEC_ERR_MASK) {
			LOG_ERR("Descriptor has SG decode error, status=%" PRIx32, current_status);
			retval = -EFAULT;
		}

		if (current_status & XILINX_AXI_DMA_SG_DESCRIPTOR_STATUS_SLV_ERR_MASK) {
			LOG_ERR("Descriptor has SG slave error, status=%" PRIx32, current_status);
			retval = -EFAULT;
		}

		if (current_status & XILINX_AXI_DMA_SG_DESCRIPTOR_STATUS_INT_ERR_MASK) {
			LOG_ERR("Descriptor has SG internal error, status=%" PRIx32,
				current_status);
			retval = -EFAULT;
		}

		if (channel_data->check_csum_in_isr) {
			uint32_t checksum_status = current_descriptor->app2;

			if (checksum_status & XILINX_AXI_DMA_SG_DESCRIPTOR_APP2_FCS_ERR_MASK) {
				LOG_ERR("Checksum offloading has FCS error status %" PRIx32 "!",
					checksum_status);
				retval = -EFAULT;
			}

			if ((checksum_status & XILINX_AXI_DMA_SG_DESCRIPTOR_APP2_IP_ERR_MASK) ==
			    XILINX_AXI_DMA_SG_DESCRIPTOR_APP2_IP_ERR_MASK) {
				LOG_ERR("Checksum offloading has IP error status %" PRIx32 "!",
					checksum_status);
				retval = -EFAULT;
			}

			if ((checksum_status & XILINX_AXI_DMA_SG_DESCRIPTOR_APP2_UDP_ERR_MASK) ==
			    XILINX_AXI_DMA_SG_DESCRIPTOR_APP2_UDP_ERR_MASK) {
				LOG_ERR("Checksum offloading has UDP error status %" PRIx32 "!",
					checksum_status);
				retval = -EFAULT;
			}

			if ((checksum_status & XILINX_AXI_DMA_SG_DESCRIPTOR_APP2_TCP_ERR_MASK) ==
			    XILINX_AXI_DMA_SG_DESCRIPTOR_APP2_TCP_ERR_MASK) {
				LOG_ERR("Checksum offloading has TCP error status %" PRIx32 "!",
					checksum_status);
				retval = -EFAULT;
			}
			/* FIXME in some corner cases, the hardware cannot check the checksum */
			/* in this case, we cannot let the Zephyr network stack know, */
			/* as we do not have per-skb flags for checksum status */
		}

		if (channel_data->completion_callback) {
			LOG_DBG("Completed packet descriptor %zu with %u bytes!",
				channel_data->completion_desc_index, channel_data->last_rx_size);
			if (channel_data->direction == PERIPHERAL_TO_MEMORY) {
				dma_xilinx_axi_dma_invd_dcache(
					(void *)current_descriptor->buffer_address,
					channel_data->last_rx_size);
			}
			channel_data->completion_callback(
				dev, channel_data->completion_callback_user_data,
				channel_data->direction == MEMORY_TO_PERIPHERAL
					? XILINX_AXI_DMA_TX_CHANNEL_NUM
					: XILINX_AXI_DMA_RX_CHANNEL_NUM,
				retval);
		}

		/* clears the flags such that the DMA does not transfer it twice or errors */
		current_descriptor->control = current_descriptor->status = 0;
		barrier_dmem_fence_full();
		dma_xilinx_axi_dma_flush_dcache((void *)current_descriptor,
						sizeof(*current_descriptor));

		channel_data->completion_desc_index++;
		if (channel_data->completion_desc_index >= channel_data->num_descriptors) {
			channel_data->completion_desc_index = 0;
		}
		current_descriptor =
			&channel_data->descriptors[channel_data->completion_desc_index];
		dma_xilinx_axi_dma_invd_dcache((void *)current_descriptor,
					       sizeof(*current_descriptor));
		current_status = current_descriptor->status;
		processed_packets++;
	}

	return processed_packets;
}

static void dma_xilinx_axi_dma_tx_isr(const struct device *dev)
{
	struct dma_xilinx_axi_dma_data *data = dev->data;
	struct dma_xilinx_axi_dma_channel *channel_data =
		&data->channels[XILINX_AXI_DMA_TX_CHANNEL_NUM];
	const int irq_enabled = irq_is_enabled(channel_data->irq);
	uint32_t dmasr;

	irq_disable(channel_data->irq);
	dmasr = dma_xilinx_axi_dma_read_reg(channel_data, XILINX_AXI_DMA_REG_DMASR);

	if (dmasr & XILINX_AXI_DMA_REGS_DMASR_ERR_IRQ) {
		LOG_ERR("DMA reports TX error, DMASR = 0x%" PRIx32, dmasr);
		dma_xilinx_axi_dma_write_reg(channel_data, XILINX_AXI_DMA_REG_DMASR,
					     XILINX_AXI_DMA_REGS_DMASR_ERR_IRQ);
	}

	if (dmasr & (XILINX_AXI_DMA_REGS_DMASR_IOC_IRQ | XILINX_AXI_DMA_REGS_DMASR_DLY_IRQ)) {
		int processed_packets;

		/* Clear the IRQ now so that new completions trigger another interrupt */
		dma_xilinx_axi_dma_write_reg(channel_data, XILINX_AXI_DMA_REG_DMASR,
					     dmasr & (XILINX_AXI_DMA_REGS_DMASR_IOC_IRQ |
						      XILINX_AXI_DMA_REGS_DMASR_DLY_IRQ));

		processed_packets =
			dma_xilinx_axi_dma_clean_up_sg_descriptors(dev, channel_data, "TX");

		LOG_DBG("Completed %u TX packets in this ISR!\n", processed_packets);
	}
	if (irq_enabled) {
		irq_enable(channel_data->irq);
	}
}

static void dma_xilinx_axi_dma_rx_isr(const struct device *dev)
{
	struct dma_xilinx_axi_dma_data *data = dev->data;
	struct dma_xilinx_axi_dma_channel *channel_data =
		&data->channels[XILINX_AXI_DMA_RX_CHANNEL_NUM];
	const int irq_enabled = irq_is_enabled(channel_data->irq);
	uint32_t dmasr;

	irq_disable(channel_data->irq);
	dmasr = dma_xilinx_axi_dma_read_reg(channel_data, XILINX_AXI_DMA_REG_DMASR);

	if (dmasr & XILINX_AXI_DMA_REGS_DMASR_ERR_IRQ) {
		LOG_ERR("DMA reports RX error, DMASR = 0x%" PRIx32, dmasr);
		dma_xilinx_axi_dma_write_reg(channel_data, XILINX_AXI_DMA_REG_DMASR,
					     XILINX_AXI_DMA_REGS_DMASR_ERR_IRQ);
	}

	if (dmasr & (XILINX_AXI_DMA_REGS_DMASR_IOC_IRQ | XILINX_AXI_DMA_REGS_DMASR_DLY_IRQ)) {
		int processed_packets;

		/* Clear the IRQ now so that new completions trigger another interrupt */
		dma_xilinx_axi_dma_write_reg(channel_data, XILINX_AXI_DMA_REG_DMASR,
					     dmasr & (XILINX_AXI_DMA_REGS_DMASR_IOC_IRQ |
						      XILINX_AXI_DMA_REGS_DMASR_DLY_IRQ));

		processed_packets =
			dma_xilinx_axi_dma_clean_up_sg_descriptors(dev, channel_data, "RX");

		LOG_DBG("Cleaned up %u RX packets in this ISR!", processed_packets);
	}
	if (irq_enabled) {
		irq_enable(channel_data->irq);
	}
}

#ifdef CONFIG_DMA_64BIT
typedef uint64_t dma_addr_t;
#else
typedef uint32_t dma_addr_t;
#endif

static int dma_xilinx_axi_dma_start(const struct device *dev, uint32_t channel)
{
	const struct dma_xilinx_axi_dma_config *cfg = dev->config;
	struct dma_xilinx_axi_dma_data *data = dev->data;
	struct dma_xilinx_axi_dma_channel *channel_data;
	volatile struct dma_xilinx_axi_dma_sg_descriptor *current_descriptor;

	/* running ISR in parallel could cause issues with the metadata */
	const int irq_key = dma_xilinx_axi_dma_lock_irq(dev, channel);

	if (channel >= cfg->channels) {
		LOG_ERR("Invalid channel %" PRIu32 " - must be < %" PRIu32 "!", channel,
			cfg->channels);
		dma_xilinx_axi_dma_unlock_irq(dev, channel, irq_key);
		return -EINVAL;
	}

	channel_data = &data->channels[channel];
	current_descriptor = &channel_data->descriptors[channel_data->populated_desc_index];

	LOG_DBG("Starting DMA on %s channel with descriptor %zu at %p",
		channel == XILINX_AXI_DMA_TX_CHANNEL_NUM ? "TX" : "RX",
		channel_data->populated_desc_index, (void *)current_descriptor);

	if (dma_xilinx_axi_dma_read_reg(channel_data, XILINX_AXI_DMA_REG_DMASR) &
	    XILINX_AXI_DMA_REGS_DMASR_HALTED) {
		uint32_t new_control = 0;

		LOG_DBG("AXI DMA is halted - restart operation!");

		new_control |= XILINX_AXI_DMA_REGS_DMACR_RS;
		/* no reset */
		new_control &= ~XILINX_AXI_DMA_REGS_DMACR_RESET;
		/* TODO make this a DT parameter */
		/* for Eth DMA, this should never be used */
		new_control &= ~XILINX_AXI_DMA_REGS_DMACR_KEYHOLE;
		/* no cyclic mode - we use completed bit to control which */
		/* transfers where completed */
		new_control &= ~XILINX_AXI_DMA_REGS_DMACR_CYC_BD_EN;
		/* we want interrupts on complete */
		new_control |= XILINX_AXI_DMA_REGS_DMACR_IOC_IRQEN;
		/* we do want timeout IRQs */
		/* they are used to catch cases where we missed interrupts */
		new_control |= XILINX_AXI_DMA_REGS_DMACR_DLY_IRQEN;
		/* we want IRQs on error */
		new_control |= XILINX_AXI_DMA_REGS_DMACR_ERR_IRQEN;
		/* interrupt after every completed transfer */
		new_control |= CONFIG_DMA_XILINX_AXI_DMA_INTERRUPT_THRESHOLD
			       << XILINX_AXI_DMA_REGS_DMACR_IRQTHRESH_SHIFT_BITS;
		/* timeout after config * 125 * clock period */
		new_control |= CONFIG_DMA_XILINX_AXI_DMA_INTERRUPT_TIMEOUT
			       << XILINX_AXI_DMA_REGS_DMACR_IRQDELAY_SHIFT_BITS;

		LOG_DBG("New DMACR value: %" PRIx32, new_control);

		dma_xilinx_axi_dma_write_reg(channel_data, XILINX_AXI_DMA_REG_DMACR, new_control);
		/* need to make sure start was committed before writing tail */
		barrier_dmem_fence_full();
	}

#ifdef CONFIG_DMA_64BIT
	dma_xilinx_axi_dma_write_reg(channel_data, XILINX_AXI_DMA_REG_TAILDESC,
				     (uint32_t)(((uintptr_t)current_descriptor) & 0xffffffff));
	dma_xilinx_axi_dma_write_reg(channel_data, XILINX_AXI_DMA_REG_TAILDESC_MSB,
				     (uint32_t)(((uintptr_t)current_descriptor) >> 32));
#else
	dma_xilinx_axi_dma_write_reg(channel_data, XILINX_AXI_DMA_REG_TAILDESC,
				     (uint32_t)(uintptr_t)current_descriptor);
#endif

	dma_xilinx_axi_dma_unlock_irq(dev, channel, irq_key);

	/* commit stores before returning to caller */
	barrier_dmem_fence_full();

	return 0;
}

static int dma_xilinx_axi_dma_stop(const struct device *dev, uint32_t channel)
{
	const struct dma_xilinx_axi_dma_config *cfg = dev->config;
	struct dma_xilinx_axi_dma_data *data = dev->data;
	struct dma_xilinx_axi_dma_channel *channel_data = &data->channels[channel];

	uint32_t new_control;

	if (channel >= cfg->channels) {
		LOG_ERR("Invalid channel %" PRIu32 " - must be < %" PRIu32 "!", channel,
			cfg->channels);
		return -EINVAL;
	}

	new_control = dma_xilinx_axi_dma_read_reg(channel_data, XILINX_AXI_DMA_REG_DMACR);
	/* RS = 0 --> DMA will complete ongoing transactions and then go into hold */
	new_control = new_control & ~XILINX_AXI_DMA_REGS_DMACR_RS;

	dma_xilinx_axi_dma_write_reg(channel_data, XILINX_AXI_DMA_REG_DMACR, new_control);

	/* commit before returning to caller */
	barrier_dmem_fence_full();

	return 0;
}

static int dma_xilinx_axi_dma_get_status(const struct device *dev, uint32_t channel,
					 struct dma_status *stat)
{
	const struct dma_xilinx_axi_dma_config *cfg = dev->config;
	struct dma_xilinx_axi_dma_data *data = dev->data;
	struct dma_xilinx_axi_dma_channel *channel_data = &data->channels[channel];

	if (channel >= cfg->channels) {
		LOG_ERR("Invalid channel %" PRIu32 " - must be < %" PRIu32 "!", channel,
			cfg->channels);
		return -EINVAL;
	}

	memset(stat, 0, sizeof(*stat));

	stat->busy = !(dma_xilinx_axi_dma_read_reg(channel_data, XILINX_AXI_DMA_REG_DMASR) &
		       XILINX_AXI_DMA_REGS_DMASR_IDLE) &&
		     !(dma_xilinx_axi_dma_read_reg(channel_data, XILINX_AXI_DMA_REG_DMASR) &
		       XILINX_AXI_DMA_REGS_DMASR_HALTED);
	stat->dir = channel_data->direction;

	/* FIXME fill hardware-specific fields */

	return 0;
}
/**
 * Transfers a single buffer through the DMA
 * If is_first or is_last are NOT set, the buffer is considered part of a SG transfer consisting of
 * multiple blocks. Otherwise, the block is one transfer.
 */
static inline int dma_xilinx_axi_dma_transfer_block(const struct device *dev, uint32_t channel,
						    dma_addr_t buffer_addr, size_t block_size,
						    bool is_first, bool is_last)
{
	struct dma_xilinx_axi_dma_data *data = dev->data;
	struct dma_xilinx_axi_dma_channel *channel_data = &data->channels[channel];
	volatile struct dma_xilinx_axi_dma_sg_descriptor *current_descriptor;

	/* running ISR in parallel could cause issues with the metadata */
	const int irq_key = dma_xilinx_axi_dma_lock_irq(dev, channel);
	size_t next_desc_index = channel_data->populated_desc_index + 1;

	if (next_desc_index >= channel_data->num_descriptors) {
		next_desc_index = 0;
	}

	current_descriptor = &channel_data->descriptors[next_desc_index];

	dma_xilinx_axi_dma_invd_dcache((void *)current_descriptor, sizeof(*current_descriptor));
	if (current_descriptor->control || current_descriptor->status) {
		/* Do not overwrite this descriptor as it has not been completed yet. */
		LOG_WRN("Descriptor %" PRIu32 " is not yet completed, not starting new transfer!",
			next_desc_index);
		dma_xilinx_axi_dma_unlock_irq(dev, channel, irq_key);
		return -EBUSY;
	}

	if (channel == XILINX_AXI_DMA_TX_CHANNEL_NUM) {
		/* Ensure DMA can see contents of TX buffer */
		dma_xilinx_axi_dma_flush_dcache((void *)buffer_addr, block_size);
	} else {
#ifdef CONFIG_DMA_XILINX_AXI_DMA_DISABLE_CACHE_WHEN_ACCESSING_SG_DESCRIPTORS
		if (((uintptr_t)buffer_addr & (sys_cache_data_line_size_get() - 1)) ||
		    (block_size & (sys_cache_data_line_size_get() - 1))) {
			LOG_ERR("RX buffer address and block size must be cache line size aligned");
			dma_xilinx_axi_dma_unlock_irq(dev, channel, irq_key);
			return -EINVAL;
		}
#endif
		/* Invalidate before starting the read, to ensure the CPU does not
		 * try to write back data to the buffer and clobber the DMA transfer.
		 */
		dma_xilinx_axi_dma_invd_dcache((void *)buffer_addr, block_size);
	}

#ifdef CONFIG_DMA_64BIT
	current_descriptor->buffer_address = (uint32_t)buffer_addr & 0xffffffff;
	current_descriptor->buffer_address_msb = (uint32_t)(buffer_addr >> 32);
#else
	current_descriptor->buffer_address = buffer_addr;
#endif
	current_descriptor->app0 = channel_data->sg_desc_app0;

	if (block_size > UINT32_MAX) {
		LOG_ERR("Too large block: %zu bytes!", block_size);

		dma_xilinx_axi_dma_unlock_irq(dev, channel, irq_key);

		return -EINVAL;
	}
	/* clears the start of frame / end of frame flags as well */
	current_descriptor->control = (uint32_t)block_size;

	if (is_first) {
		current_descriptor->control =
			current_descriptor->control | XILINX_AXI_DMA_SG_DESCRIPTOR_CTRL_SOF_MASK;
	}
	if (is_last) {
		current_descriptor->control =
			current_descriptor->control | XILINX_AXI_DMA_SG_DESCRIPTOR_CTRL_EOF_MASK;
	}

	/* SG descriptor must be completed BEFORE hardware is made aware of it */
	barrier_dmem_fence_full();
	dma_xilinx_axi_dma_flush_dcache((void *)current_descriptor, sizeof(*current_descriptor));

	channel_data->populated_desc_index = next_desc_index;

	dma_xilinx_axi_dma_unlock_irq(dev, channel, irq_key);

	return 0;
}

#ifdef CONFIG_DMA_64BIT
static inline int dma_xilinx_axi_dma_config_reload(const struct device *dev, uint32_t channel,
						   uint64_t src, uint64_t dst, size_t size)
#else
static inline int dma_xilinx_axi_dma_config_reload(const struct device *dev, uint32_t channel,
						   uint32_t src, uint32_t dst, size_t size)
#endif
{
	const struct dma_xilinx_axi_dma_config *cfg = dev->config;

	if (channel >= cfg->channels) {
		LOG_ERR("Invalid channel %" PRIu32 " - must be < %" PRIu32 "!", channel,
			cfg->channels);
		return -EINVAL;
	}
	/* one-block-at-a-time transfer */
	return dma_xilinx_axi_dma_transfer_block(
		dev, channel, channel == XILINX_AXI_DMA_TX_CHANNEL_NUM ? src : dst, size, true,
		true);
}

static int dma_xilinx_axi_dma_configure(const struct device *dev, uint32_t channel,
					struct dma_config *dma_cfg)
{
	const struct dma_xilinx_axi_dma_config *cfg = dev->config;
	struct dma_xilinx_axi_dma_data *data = dev->data;
	struct dma_block_config *current_block = dma_cfg->head_block;
	int ret = 0;
	int block_count = 0;

	if (channel >= cfg->channels) {
		LOG_ERR("Invalid channel %" PRIu32 " - must be < %" PRIu32 "!", channel,
			cfg->channels);
		return -EINVAL;
	}

	if (dma_cfg->head_block->source_addr_adj == DMA_ADDR_ADJ_DECREMENT) {
		LOG_ERR("Xilinx AXI DMA only supports incrementing addresses!");
		return -ENOTSUP;
	}

	if (dma_cfg->head_block->dest_addr_adj == DMA_ADDR_ADJ_DECREMENT) {
		LOG_ERR("Xilinx AXI DMA only supports incrementing addresses!");
		return -ENOTSUP;
	}

	if (dma_cfg->head_block->source_addr_adj != DMA_ADDR_ADJ_INCREMENT &&
	    dma_cfg->head_block->source_addr_adj != DMA_ADDR_ADJ_NO_CHANGE) {
		LOG_ERR("invalid source_addr_adj %" PRIu16, dma_cfg->head_block->source_addr_adj);
		return -ENOTSUP;
	}
	if (dma_cfg->head_block->dest_addr_adj != DMA_ADDR_ADJ_INCREMENT &&
	    dma_cfg->head_block->dest_addr_adj != DMA_ADDR_ADJ_NO_CHANGE) {
		LOG_ERR("invalid dest_addr_adj %" PRIu16, dma_cfg->head_block->dest_addr_adj);
		return -ENOTSUP;
	}

	if (channel == XILINX_AXI_DMA_TX_CHANNEL_NUM &&
	    dma_cfg->channel_direction != MEMORY_TO_PERIPHERAL) {
		LOG_ERR("TX channel must be used with MEMORY_TO_PERIPHERAL!");
		return -ENOTSUP;
	}

	if (channel == XILINX_AXI_DMA_RX_CHANNEL_NUM &&
	    dma_cfg->channel_direction != PERIPHERAL_TO_MEMORY) {
		LOG_ERR("RX channel must be used with PERIPHERAL_TO_MEMORY!");
		return -ENOTSUP;
	}

	LOG_DBG("Configuring %zu DMA descriptors for %s", data->channels[channel].num_descriptors,
		channel == XILINX_AXI_DMA_TX_CHANNEL_NUM ? "TX" : "RX");

	/* only configures fields whos default is not 0, as descriptors are in zero-initialized */
	/* segment */
	data->channels[channel].populated_desc_index = data->channels[channel].num_descriptors - 1;
	data->channels[channel].completion_desc_index = 0;
	for (int i = 0; i < data->channels[channel].num_descriptors; i++) {
		uintptr_t nextdesc;
		uint32_t low_bytes;
#ifdef CONFIG_DMA_64BIT
		uint32_t high_bytes;
#endif
		if (i + 1 < data->channels[channel].num_descriptors) {
			nextdesc = (uintptr_t)&data->channels[channel].descriptors[i + 1];
		} else {
			nextdesc = (uintptr_t)&data->channels[channel].descriptors[0];
		}
		/* SG descriptors have 64-byte alignment requirements */
		/* we check this here, for each descriptor */
		__ASSERT(
			nextdesc & XILINX_AXI_DMA_SG_DESCRIPTOR_ADDRESS_MASK == 0,
			"SG descriptor address %p (offset %u) was not aligned to 64-byte boundary!",
			(void *)nextdesc, i);

		low_bytes = (uint32_t)(((uint64_t)nextdesc) & 0xffffffff);
		data->channels[channel].descriptors[i].nxtdesc = low_bytes;

#ifdef CONFIG_DMA_64BIT
		high_bytes = (uint32_t)(((uint64_t)nextdesc >> 32) & 0xffffffff);
		data->channels[channel].descriptors[i].nxtdesc_msb = high_bytes;
#endif
		dma_xilinx_axi_dma_flush_dcache((void *)&data->channels[channel].descriptors[i],
						sizeof(data->channels[channel].descriptors[i]));
	}

#ifdef CONFIG_DMA_64BIT
	dma_xilinx_axi_dma_write_reg(
		&data->channels[channel], XILINX_AXI_DMA_REG_CURDESC,
		(uint32_t)(((uintptr_t)&data->channels[channel].descriptors[0]) & 0xffffffff));
	dma_xilinx_axi_dma_write_reg(
		&data->channels[channel], XILINX_AXI_DMA_REG_CURDESC_MSB,
		(uint32_t)(((uintptr_t)&data->channels[channel].descriptors[0]) >> 32));
#else
	dma_xilinx_axi_dma_write_reg(&data->channels[channel], XILINX_AXI_DMA_REG_CURDESC,
				     (uint32_t)(uintptr_t)&data->channels[channel].descriptors[0]);
#endif
	data->channels[channel].check_csum_in_isr = false;

	/* the DMA passes the app fields through to the AXIStream-connected device */
	/* whether the connected device understands these flags needs to be determined by the */
	/* caller! */
	switch (dma_cfg->linked_channel) {
	case XILINX_AXI_DMA_LINKED_CHANNEL_FULL_CSUM_OFFLOAD:
		if (channel == XILINX_AXI_DMA_TX_CHANNEL_NUM) {
			/* for the TX channel, we need to indicate that we would like to use */
			/* checksum offloading */
			data->channels[channel].sg_desc_app0 =
				XILINX_AXI_DMA_SG_DESCRIPTOR_APP0_CHECKSUM_OFFLOAD_FULL;
		} else {
			/* for the RX channel, the Ethernet core will indicate to us that it has */
			/* computed a checksum and whether it is valid we need to check this in */
			/* the ISR and report it upstream */
			data->channels[channel].check_csum_in_isr = true;
		}
		break;
	case XILINX_AXI_DMA_LINKED_CHANNEL_NO_CSUM_OFFLOAD:
		data->channels[channel].sg_desc_app0 =
			XILINX_AXI_DMA_SG_DESCRIPTOR_APP0_CHECKSUM_OFFLOAD_NONE;
		break;
	default:
		LOG_ERR("Linked channel invalid! Valid values: %u for full ethernt checksum "
			"offloading %u for no checksum offloading!",
			XILINX_AXI_DMA_LINKED_CHANNEL_FULL_CSUM_OFFLOAD,
			XILINX_AXI_DMA_LINKED_CHANNEL_NO_CSUM_OFFLOAD);
		return -EINVAL;
	}

	data->channels[channel].completion_callback = dma_cfg->dma_callback;
	data->channels[channel].completion_callback_user_data = dma_cfg->user_data;

	LOG_INF("Completed configuration of AXI DMA - Starting transfer!");

	do {
		ret = ret ||
		      dma_xilinx_axi_dma_transfer_block(dev, channel,
							channel == XILINX_AXI_DMA_TX_CHANNEL_NUM
								? current_block->source_address
								: current_block->dest_address,
							current_block->block_size, block_count == 0,
							current_block->next_block == NULL);
		block_count++;
	} while ((current_block = current_block->next_block) && ret == 0);

	return ret;
}

static bool dma_xilinx_axi_dma_chan_filter(const struct device *dev, int channel,
					   void *filter_param)
{
	const char *filter_str = (const char *)filter_param;

	if (strcmp(filter_str, "tx") == 0) {
		return channel == XILINX_AXI_DMA_TX_CHANNEL_NUM;
	}
	if (strcmp(filter_str, "rx") == 0) {
		return channel == XILINX_AXI_DMA_RX_CHANNEL_NUM;
	}

	return false;
}

/* DMA API callbacks */
static DEVICE_API(dma, dma_xilinx_axi_dma_driver_api) = {
	.config = dma_xilinx_axi_dma_configure,
	.reload = dma_xilinx_axi_dma_config_reload,
	.start = dma_xilinx_axi_dma_start,
	.stop = dma_xilinx_axi_dma_stop,
	.suspend = NULL,
	.resume = NULL,
	.get_status = dma_xilinx_axi_dma_get_status,
	.chan_filter = dma_xilinx_axi_dma_chan_filter,
};

static int dma_xilinx_axi_dma_init(const struct device *dev)
{
	const struct dma_xilinx_axi_dma_config *cfg = dev->config;
	struct dma_xilinx_axi_dma_data *data = dev->data;
	bool reset = false;

	if (cfg->channels != XILINX_AXI_DMA_NUM_CHANNELS) {
		LOG_ERR("Invalid number of configured channels (%" PRIu32
			") - Xilinx AXI DMA must have %" PRIu32 " channels!",
			cfg->channels, XILINX_AXI_DMA_NUM_CHANNELS);
		return -EINVAL;
	}

	data->channels[XILINX_AXI_DMA_TX_CHANNEL_NUM].descriptors = data->descriptors_tx;
	data->channels[XILINX_AXI_DMA_TX_CHANNEL_NUM].num_descriptors =
		ARRAY_SIZE(data->descriptors_tx);
	data->channels[XILINX_AXI_DMA_TX_CHANNEL_NUM].channel_regs =
		cfg->reg + XILINX_AXI_DMA_MM2S_REG_OFFSET;
	data->channels[XILINX_AXI_DMA_TX_CHANNEL_NUM].direction = MEMORY_TO_PERIPHERAL;

	data->channels[XILINX_AXI_DMA_RX_CHANNEL_NUM].descriptors = data->descriptors_rx;
	data->channels[XILINX_AXI_DMA_RX_CHANNEL_NUM].num_descriptors =
		ARRAY_SIZE(data->descriptors_rx);
	data->channels[XILINX_AXI_DMA_RX_CHANNEL_NUM].channel_regs =
		cfg->reg + XILINX_AXI_DMA_S2MM_REG_OFFSET;
	data->channels[XILINX_AXI_DMA_TX_CHANNEL_NUM].direction = PERIPHERAL_TO_MEMORY;

	LOG_INF("Soft-resetting the DMA core!");
	/* this resets BOTH RX and TX channels, although it is triggered in per-channel
	 * DMACR
	 */
	dma_xilinx_axi_dma_write_reg(&data->channels[XILINX_AXI_DMA_RX_CHANNEL_NUM],
				     XILINX_AXI_DMA_REG_DMACR, XILINX_AXI_DMA_REGS_DMACR_RESET);
	for (int i = 0; i < 1000; i++) {
		if (dma_xilinx_axi_dma_read_reg(&data->channels[XILINX_AXI_DMA_RX_CHANNEL_NUM],
						XILINX_AXI_DMA_REG_DMACR) &
		    XILINX_AXI_DMA_REGS_DMACR_RESET) {
			k_msleep(1);
		} else {
			reset = true;
			break;
		}
	}
	if (!reset) {
		LOG_ERR("DMA reset timed out!");
		return -EIO;
	}

	cfg->irq_configure(data);
	return 0;
}

#define XILINX_AXI_DMA_INIT(inst)                                                                  \
	static void dma_xilinx_axi_dma##inst##_irq_configure(struct dma_xilinx_axi_dma_data *data) \
	{                                                                                          \
		data->channels[XILINX_AXI_DMA_TX_CHANNEL_NUM].irq = DT_INST_IRQN_BY_IDX(inst, 0);  \
		IRQ_CONNECT(DT_INST_IRQN_BY_IDX(inst, 0), DT_INST_IRQ_BY_IDX(inst, 0, priority),   \
			    dma_xilinx_axi_dma_tx_isr, DEVICE_DT_INST_GET(inst), 0);               \
		irq_enable(DT_INST_IRQN_BY_IDX(inst, 0));                                          \
		data->channels[XILINX_AXI_DMA_RX_CHANNEL_NUM].irq = DT_INST_IRQN_BY_IDX(inst, 1);  \
		IRQ_CONNECT(DT_INST_IRQN_BY_IDX(inst, 1), DT_INST_IRQ_BY_IDX(inst, 1, priority),   \
			    dma_xilinx_axi_dma_rx_isr, DEVICE_DT_INST_GET(inst), 0);               \
		irq_enable(DT_INST_IRQN_BY_IDX(inst, 1));                                          \
	}                                                                                          \
	static const struct dma_xilinx_axi_dma_config dma_xilinx_axi_dma##inst##_config = {        \
		.reg = DT_INST_REG_ADDR(inst),                                                     \
		.channels = DT_INST_PROP(inst, dma_channels),                                      \
		.irq_configure = dma_xilinx_axi_dma##inst##_irq_configure,                         \
	};                                                                                         \
	static struct dma_xilinx_axi_dma_channel                                                   \
		dma_xilinx_axi_dma##inst##_channels[DT_INST_PROP(inst, dma_channels)];             \
	static struct dma_xilinx_axi_dma_data dma_xilinx_axi_dma##inst##_data = {                  \
		.ctx = {.magic = DMA_MAGIC, .atomic = NULL},                                       \
		.channels = dma_xilinx_axi_dma##inst##_channels,                                   \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, &dma_xilinx_axi_dma_init, NULL,                                \
			      &dma_xilinx_axi_dma##inst##_data,                                    \
			      &dma_xilinx_axi_dma##inst##_config, POST_KERNEL,                     \
			      CONFIG_DMA_INIT_PRIORITY, &dma_xilinx_axi_dma_driver_api);

/* two different compatibles match the very same Xilinx AXI DMA, */
/* depending on if it is used in the AXI Ethernet subsystem or not */
#define DT_DRV_COMPAT xlnx_eth_dma
DT_INST_FOREACH_STATUS_OKAY(XILINX_AXI_DMA_INIT)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT xlnx_axi_dma_1_00_a
DT_INST_FOREACH_STATUS_OKAY(XILINX_AXI_DMA_INIT)
