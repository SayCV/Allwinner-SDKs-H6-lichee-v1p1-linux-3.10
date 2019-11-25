/*
 *      Filename: google_vp9.c
 *      Version: 0.01alpha
 *      Description: Video engine driver API, Don't modify it in user space.
 *      License: GPLv2
 *      Author  : yangcaoyuan<yangcaoyuan@allwinnertech.com>
 *      Date    : 2017/04/04
 *
 *      Copyright (C) 2015 Allwinnertech Ltd.
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License version 2 as
 *      published by the Free Software Foundation.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */


#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/preempt.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/rmap.h>
#include <linux/wait.h>
#include <linux/semaphore.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/scatterlist.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/dma.h>
#include <linux/mm.h>
#include <asm/siginfo.h>
#include <asm/signal.h>
#include <linux/clk/sunxi.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#include "google_vp9.h"
#include "vp9_mem_list.h"
#include <linux/regulator/consumer.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>

#define DRV_VERSION "0.01alpha"

#ifndef VP9DEV_MAJOR
#define VP9DEV_MAJOR (0)
#endif
#ifndef VP9DEV_MINOR
#define VP9DEV_MINOR (0)
#endif

#define MACC_VP9_REGS_BASE      (0x01C00000)

#ifndef CONFIG_OF
#define SUNXI_IRQ_GOOGLE_VP9		(122)
#endif

/*#define VP9_DEBUG*/
#define google_vp9_printk(level, msg...) printk(level "google_vp9: " msg)

#define GOOGLE_VP9_CLK_HIGH_WATER  (700)
#define GOOGLE_VP9_CLK_LOW_WATER   (100)

#define PRINTK_IOMMU_ADDR 0

int vp9_dev_major = VP9DEV_MAJOR;
int vp9_dev_minor = VP9DEV_MINOR;
module_param(vp9_dev_major, int, S_IRUGO);
module_param(vp9_dev_minor, int, S_IRUGO);

struct clk *google_vp9_moduleclk;
struct clk *google_vp9_parent_pll_clk;

static unsigned long google_vp9_parent_clk_rate = 300000000;

struct iomap_para {
	char *regs_macc;
	char *regs_avs;
};

struct user_iommu_param {
	int				fd;
	unsigned int	iommu_addr;
};

struct cedarv_iommu_buffer {
	struct aw_mem_list_head i_list;
	int				fd;
	unsigned long	iommu_addr;
	struct dma_buf  *dma_buf;
	struct dma_buf_attachment *attachment;
	struct sg_table	*sgt;
};

static DECLARE_WAIT_QUEUE_HEAD(wait_vp9);
struct googlevp9_dev {
	struct cdev cdev;	             /* char device struct                 */
	struct device *dev;              /* ptr to class device struct         */
	struct device *platform_dev;	 /* ptr to class device struct		   */
	struct class  *class;            /* class for auto create device node  */

	struct semaphore sem;            /* mutual exclusion semaphore         */

	wait_queue_head_t wq;            /* wait queue for poll ops            */

	struct iomap_para iomap_addrs;   /* io remap addrs                     */

	struct timer_list vp9_engine_timer;
	struct timer_list vp9_engine_timer_rel;

	u32 irq;                         /* cedar video engine irq number      */
	u32 de_irq_flag;                    /* flag of video decoder engine irq generated */
	u32 de_irq_value;                   /* value of video decoder engine irq          */
	u32 irq_has_enable;
	u32 ref_count;

	unsigned int *sram_bass_vir;
	unsigned int *clk_bass_vir;

	struct aw_mem_list_head    list;        /* buffer list */
	struct mutex lock_mem;
};

struct google_vp9_info {
	unsigned int set_vol_flag;
};

struct googlevp9_dev *google_vp9_devp;
struct file *google_vp9_file;

#if defined(CONFIG_OF)
static struct of_device_id sunxi_google_vp9_match[] = {
	{ .compatible = "allwinner,sunxi-google-vp9",},
	{}
};
MODULE_DEVICE_TABLE(of, sunxi_google_vp9_match);
#endif

static irqreturn_t GoogleVp9Interupt(int irq, void *dev)
{
	unsigned long vp9_int_status_reg;
	unsigned long vp9_int_ctrl_reg;
	unsigned int status;
	unsigned int interrupt_enable;
	struct iomap_para addrs = google_vp9_devp->iomap_addrs;
	/*1. check and get the interrupt enable bits */
	/*2. check and get the interrupt status bits */
	/*3. clear the interrupt enable bits */
	/*4. set the irq_value and irq_flag */
	/*5. wake up the user mode interrupt_func */
	vp9_int_status_reg = (unsigned long)(addrs.regs_macc + 0x04);
	status = readl((void *)vp9_int_status_reg);
	vp9_int_ctrl_reg = (unsigned long)(addrs.regs_macc + 0x04);
	interrupt_enable = (readl((void *)vp9_int_ctrl_reg) & (0x10));

	/* only check status[bit:18,16,13,12,11,8], enable[bit:4] */
	if ((status & 0x53900) &&
		(!interrupt_enable)) {
	    /*need check we must clear the interrupt enable bits or not?*/
	    unsigned int val;
		val = readl((void *)vp9_int_ctrl_reg);
		writel(val & 0xfffffeff, (void *)vp9_int_ctrl_reg);
		google_vp9_devp->de_irq_value = 1;
		google_vp9_devp->de_irq_flag = 1;
		wake_up_interruptible(&wait_vp9);
	}

	return IRQ_HANDLED;
}

static int clk_status;
static LIST_HEAD(run_task_list);
static LIST_HEAD(del_task_list);
static spinlock_t google_vp9_spin_lock;
#define CEDAR_RUN_LIST_NONULL	-1
#define CEDAR_NONBLOCK_TASK  0
#define CEDAR_BLOCK_TASK 1
#define CLK_REL_TIME 10000
#define TIMER_CIRCLE 50
#define TASK_INIT      0x00
#define TASK_TIMEOUT   0x55
#define TASK_RELEASE   0xaa
#define SIG_CEDAR		35

int enable_google_vp9_hw_clk(void)
{
	unsigned long flags;
	int res = -EFAULT;

	spin_lock_irqsave(&google_vp9_spin_lock, flags);

	if (clk_status == 1)
		goto out;

	clk_status = 1;

	sunxi_periph_reset_deassert(google_vp9_moduleclk);
	if (clk_enable(google_vp9_moduleclk)) {
		google_vp9_printk(KERN_WARNING,
		"enable google_vp9_moduleclk failed;\n");
		goto out;
	} else {
		res = 0;
	}

	AW_MEM_INIT_LIST_HEAD(&google_vp9_devp->list);
out:
	spin_unlock_irqrestore(&google_vp9_spin_lock, flags);
	return res;
}

int disable_google_vp9_hw_clk(void)
{
	unsigned long flags;
	int res = -EFAULT;
	struct aw_mem_list_head *pos, *q;

	spin_lock_irqsave(&google_vp9_spin_lock, flags);

	if (clk_status == 0) {
		res = 0;
		goto out;
	}
	clk_status = 0;

	if ((NULL == google_vp9_moduleclk) ||
		(IS_ERR(google_vp9_moduleclk))) {
		google_vp9_printk(KERN_WARNING,
		"google_vp9_moduleclk is invalid\n");
	} else {
		clk_disable(google_vp9_moduleclk);
		sunxi_periph_reset_assert(google_vp9_moduleclk);
		res = 0;
	}

	aw_mem_list_for_each_safe(pos, q, &google_vp9_devp->list)
	{
		struct cedarv_iommu_buffer *tmp;
		tmp = aw_mem_list_entry(pos, struct cedarv_iommu_buffer, i_list);
		aw_mem_list_del(pos);
		kfree(tmp);
	}
out:
	spin_unlock_irqrestore(&google_vp9_spin_lock, flags);
	return res;
}

static void vp9_engine_for_timer_rel(unsigned long arg)
{
	unsigned long flags;
	int ret = 0;
	spin_lock_irqsave(&google_vp9_spin_lock, flags);

	if (list_empty(&run_task_list)) {
		ret = disable_google_vp9_hw_clk();
		if (ret < 0)
			google_vp9_printk(KERN_WARNING, "clk disable error!\n");
	} else {
		google_vp9_printk(KERN_WARNING,
			"clk disable time out but task left\n");
		mod_timer(&google_vp9_devp->vp9_engine_timer,
			jiffies + msecs_to_jiffies(TIMER_CIRCLE));
	}

	spin_unlock_irqrestore(&google_vp9_spin_lock, flags);
}

static void vp9_engine_for_events(unsigned long arg)
{
	struct vp9_engine_task *task_entry, *task_entry_tmp;
	struct siginfo info;
	unsigned long flags;

	spin_lock_irqsave(&google_vp9_spin_lock, flags);

	list_for_each_entry_safe(task_entry,
							task_entry_tmp,
							&run_task_list, list) {
		mod_timer(&google_vp9_devp->vp9_engine_timer_rel,
			jiffies + msecs_to_jiffies(CLK_REL_TIME));
		if (task_entry->status == TASK_RELEASE ||
				time_after(jiffies, task_entry->t.timeout)) {
			if (task_entry->status == TASK_INIT)
				task_entry->status = TASK_TIMEOUT;
			list_move(&task_entry->list, &del_task_list);
		}
	}

	list_for_each_entry_safe(task_entry,
							task_entry_tmp,
							&del_task_list, list) {
		info.si_signo = SIG_CEDAR;
		info.si_code = task_entry->t.ID;
		if (task_entry->status == TASK_TIMEOUT) {
			info.si_errno = TASK_TIMEOUT;
			send_sig_info(SIG_CEDAR, &info, task_entry->task_handle);
		} else if (task_entry->status == TASK_RELEASE) {
			info.si_errno = TASK_RELEASE;
			send_sig_info(SIG_CEDAR, &info, task_entry->task_handle);
		}
		list_del(&task_entry->list);
		kfree(task_entry);
	}

	if (!list_empty(&run_task_list)) {
		task_entry = list_entry(run_task_list.next,
					struct vp9_engine_task, list);
		if (task_entry->running == 0) {
			task_entry->running = 1;
			info.si_signo = SIG_CEDAR;
			info.si_code = task_entry->t.ID;
			info.si_errno = TASK_INIT;
			send_sig_info(SIG_CEDAR, &info, task_entry->task_handle);
		}

		mod_timer(&google_vp9_devp->vp9_engine_timer,
			jiffies + msecs_to_jiffies(TIMER_CIRCLE));
	}

	spin_unlock_irqrestore(&google_vp9_spin_lock, flags);
}

static long compat_googlevp9dev_ioctl(struct file *filp,
		unsigned int cmd,
		unsigned long arg)
{
	long  ret = 0;
	int ve_timeout = 0;
	unsigned long flags;
	struct google_vp9_info *info;

	info = filp->private_data;

	switch (cmd) {
	case VP9_IOCTL_ENGINE_REQ:
		google_vp9_devp->ref_count++;
		if (1 == google_vp9_devp->ref_count)
			enable_google_vp9_hw_clk();
		break;
	case VP9_IOCTL_ENGINE_REL:
		google_vp9_devp->ref_count--;
		if (0 == google_vp9_devp->ref_count) {
			ret = disable_google_vp9_hw_clk();
			if (ret < 0) {
				google_vp9_printk(KERN_WARNING,
				"IOCTL_ENGINE_REL clk disable error!\n");
				return -EFAULT;
			}
		}
		return ret;
	case VP9_IOCTL_WAIT_INTERRUPT:
		ve_timeout = (int)arg;
		google_vp9_devp->de_irq_value = 0;

		spin_lock_irqsave(&google_vp9_spin_lock, flags);
		if (google_vp9_devp->de_irq_flag)
			google_vp9_devp->de_irq_value = 1;
		spin_unlock_irqrestore(&google_vp9_spin_lock, flags);

		wait_event_interruptible_timeout(wait_vp9,
			google_vp9_devp->de_irq_flag, ve_timeout*HZ);
		google_vp9_devp->de_irq_flag = 0;

		return google_vp9_devp->de_irq_value;

	case VP9_IOCTL_RESET:
		sunxi_periph_reset_assert(google_vp9_moduleclk);
		sunxi_periph_reset_deassert(google_vp9_moduleclk);
		break;

	case VP9_IOCTL_SET_FREQ:
		{
			int arg_rate = (int)arg;

			if (arg_rate >= GOOGLE_VP9_CLK_LOW_WATER &&
				arg_rate <= GOOGLE_VP9_CLK_HIGH_WATER &&
				clk_get_rate(google_vp9_moduleclk)/1000000 != arg_rate) {
				if (!clk_set_rate(google_vp9_parent_pll_clk, arg_rate*1000000)) {
					google_vp9_parent_clk_rate = clk_get_rate(google_vp9_parent_pll_clk);

					if (clk_set_rate(google_vp9_moduleclk,
									google_vp9_parent_clk_rate))
						google_vp9_printk(KERN_WARNING,
							"set vp9 clock failed\n");
				} else {
					google_vp9_printk(KERN_WARNING,
						"set pll_vp9_parent clock failed\n");
				}
			}
			ret = clk_get_rate(google_vp9_moduleclk);
			break;
		}

	case VP9_IOCTL_GET_ENV_INFO:
		{
			struct vp9_env_infomation_compat env_info;

			env_info.phymem_start = 0;
			env_info.phymem_total_size = 0;
			env_info.address_macc = 0;
			if (copy_to_user((char *)arg, &env_info,
				sizeof(struct vp9_env_infomation_compat)))
				return -EFAULT;
		}
		break;
	case IOCTL_GET_IOMMU_ADDR:
	{
		int ret, i;
		struct sg_table *sgt, *sgt_bak;
		struct scatterlist *sgl, *sgl_bak;
	    struct user_iommu_param sUserIommuParam;
		struct cedarv_iommu_buffer *pVeIommuBuf = NULL;

		pVeIommuBuf = kmalloc(sizeof(struct cedarv_iommu_buffer), GFP_KERNEL);
		if (pVeIommuBuf == NULL) {
			google_vp9_printk(KERN_ERR,
				"IOCTL_GET_IOMMU_ADDR malloc cedarv_iommu_buffererror\n");
			return -EFAULT;
		}
		if (copy_from_user(&sUserIommuParam, (void __user *)arg,
			sizeof(struct user_iommu_param))) {
			google_vp9_printk(KERN_ERR,
				"IOCTL_GET_IOMMU_ADDR copy_from_user erro\n");
			return -EFAULT;
		}

		pVeIommuBuf->fd = sUserIommuParam.fd;

		pVeIommuBuf->dma_buf = dma_buf_get(pVeIommuBuf->fd);
		if (pVeIommuBuf->dma_buf < 0) {
			google_vp9_printk(KERN_ERR,
						"vp9 get dma_buf error");
			return -EFAULT;
		}

		pVeIommuBuf->attachment =
							   dma_buf_attach(pVeIommuBuf->dma_buf,
														google_vp9_devp->platform_dev);
		if (pVeIommuBuf->attachment < 0) {
			google_vp9_printk(KERN_ERR,
				"vp9 get dma_buf_attachment error");
			goto RELEASE_DMA_BUF;
		}

		sgt = dma_buf_map_attachment(pVeIommuBuf->attachment,
											DMA_BIDIRECTIONAL);

		sgt_bak = kmalloc(sizeof(struct sg_table), GFP_KERNEL | __GFP_ZERO);
		if (sgt_bak == NULL)
			google_vp9_printk(KERN_ERR,
				"vp9 get kmalloc sg_table error");

		ret = sg_alloc_table(sgt_bak, sgt->nents, GFP_KERNEL);
		if (ret != 0)
			google_vp9_printk(KERN_ERR,
				"vp9 sg_alloc_table error");

		sgl_bak = sgt_bak->sgl;
		for_each_sg(sgt->sgl, sgl, sgt->nents, i)  {
			sg_set_page(sgl_bak, sg_page(sgl), sgl->length, sgl->offset);
			sgl_bak = sg_next(sgl_bak);
		}

		pVeIommuBuf->sgt = sgt_bak;
		if (pVeIommuBuf->sgt < 0) {
			google_vp9_printk(KERN_ERR,
				"vp9 get sg_table error\n");
			goto RELEASE_DMA_BUF;
		}

		ret = dma_map_sg(google_vp9_devp->platform_dev, pVeIommuBuf->sgt->sgl,
									pVeIommuBuf->sgt->nents,
									DMA_BIDIRECTIONAL);
		if (ret != 1) {
			google_vp9_printk(KERN_ERR,
				"vp9 dma_map_sg error\n");
			goto RELEASE_DMA_BUF;
		}

		pVeIommuBuf->iommu_addr = sg_dma_address(pVeIommuBuf->sgt->sgl);
		sUserIommuParam.iommu_addr = (unsigned int)(pVeIommuBuf->iommu_addr & 0xffffffff);


		if (copy_to_user((void __user *)arg, &sUserIommuParam, sizeof(struct user_iommu_param))) {
			google_vp9_printk(KERN_ERR,
				"vp9 get iommu copy_to_user error\n");
			goto RELEASE_DMA_BUF;
		}

		#if PRINTK_IOMMU_ADDR
		google_vp9_printk(KERN_DEBUG,
			   "fd:%d, iommu_addr:%lx, dma_buf:%p, dma_buf_attach:%p, sg_table:%p, nents:%d\n",
		pVeIommuBuf->fd,
		pVeIommuBuf->iommu_addr,
		pVeIommuBuf->dma_buf,
		pVeIommuBuf->attachment,
		pVeIommuBuf->sgt,
		pVeIommuBuf->sgt->nents);
		#endif

		mutex_lock(&google_vp9_devp->lock_mem);
		aw_mem_list_add_tail(&pVeIommuBuf->i_list, &google_vp9_devp->list);
		mutex_unlock(&google_vp9_devp->lock_mem);
		break;

		RELEASE_DMA_BUF:
			if (pVeIommuBuf->dma_buf > 0) {
				if (pVeIommuBuf->attachment > 0) {
					if (pVeIommuBuf->sgt > 0) {
						dma_unmap_sg(google_vp9_devp->platform_dev,
									pVeIommuBuf->sgt->sgl,
									pVeIommuBuf->sgt->nents,
									DMA_BIDIRECTIONAL);
						dma_buf_unmap_attachment(pVeIommuBuf->attachment, pVeIommuBuf->sgt,
												DMA_BIDIRECTIONAL);
						sg_free_table(pVeIommuBuf->sgt);
						kfree(pVeIommuBuf->sgt);
					}

					dma_buf_detach(pVeIommuBuf->dma_buf,
								pVeIommuBuf->attachment);
				}

				dma_buf_put(pVeIommuBuf->dma_buf);
				return -1;
			}
			kfree(pVeIommuBuf);
		break;
	}
	case IOCTL_FREE_IOMMU_ADDR:
	{
	    struct user_iommu_param sUserIommuParam;
		struct cedarv_iommu_buffer *pVeIommuBuf;
		if (copy_from_user(&sUserIommuParam, (void __user *)arg,
			sizeof(struct user_iommu_param))) {
			google_vp9_printk(KERN_ERR,
				"IOCTL_FREE_IOMMU_ADDR copy_from_user error");
			return -EFAULT;
		}
		aw_mem_list_for_each_entry(pVeIommuBuf, &google_vp9_devp->list, i_list)
		{
			if (pVeIommuBuf->fd == sUserIommuParam.fd) {
					#if PRINTK_IOMMU_ADDR
					google_vp9_printk(KERN_DEBUG, "free: fd:%d, iommu_addr:%lx, dma_buf:%p, dma_buf_attach:%p, sg_table:%p nets:%d\n",
					pVeIommuBuf->fd,
					pVeIommuBuf->iommu_addr,
					pVeIommuBuf->dma_buf,
					pVeIommuBuf->attachment,
					pVeIommuBuf->sgt,
					pVeIommuBuf->sgt->nents);
					#endif

					if (pVeIommuBuf->dma_buf > 0) {
						if (pVeIommuBuf->attachment > 0) {
							if (pVeIommuBuf->sgt > 0) {
								dma_unmap_sg(google_vp9_devp->platform_dev,
											pVeIommuBuf->sgt->sgl,
											pVeIommuBuf->sgt->nents,
											DMA_BIDIRECTIONAL);
								dma_buf_unmap_attachment(pVeIommuBuf->attachment,
													pVeIommuBuf->sgt,
														DMA_BIDIRECTIONAL);
								sg_free_table(pVeIommuBuf->sgt);
								kfree(pVeIommuBuf->sgt);
							}

							dma_buf_detach(pVeIommuBuf->dma_buf, pVeIommuBuf->attachment);
						}

						dma_buf_put(pVeIommuBuf->dma_buf);
					}

					mutex_lock(&google_vp9_devp->lock_mem);
					aw_mem_list_del(&pVeIommuBuf->i_list);
					kfree(pVeIommuBuf);
					mutex_unlock(&google_vp9_devp->lock_mem);
					break;
			}
		}
		break;
	}
	default:
		return -1;
	}
	return ret;
}

static int googlevp9dev_open(struct inode *inode, struct file *filp)
{
	struct google_vp9_info *info;

	info = kmalloc(sizeof(struct google_vp9_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->set_vol_flag = 0;
	google_vp9_file = filp;

	filp->private_data = info;
	if (down_interruptible(&google_vp9_devp->sem))
		return -ERESTARTSYS;

	if (0 == google_vp9_devp->ref_count)
		google_vp9_devp->de_irq_flag = 0;

	up(&google_vp9_devp->sem);
	nonseekable_open(inode, filp);
	return 0;
}

static int googlevp9dev_release(struct inode *inode, struct file *filp)
{
	struct google_vp9_info *info;

	info = filp->private_data;

	if (down_interruptible(&google_vp9_devp->sem))
		return -ERESTARTSYS;

	/* release other resource here */
	if (0 == google_vp9_devp->ref_count)
		google_vp9_devp->de_irq_flag = 1;

	up(&google_vp9_devp->sem);

	kfree(info);
	google_vp9_file = NULL;
	return 0;
}

static void googlevp9dev_vma_open(struct vm_area_struct *vma)
{
}

static void googlevp9dev_vma_close(struct vm_area_struct *vma)
{
}

static struct vm_operations_struct cedardev_remap_vm_ops = {
	.open  = googlevp9dev_vma_open,
	.close = googlevp9dev_vma_close,
};

static int googlevp9dev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long temp_pfn;

	if (vma->vm_end - vma->vm_start == 0) {
		google_vp9_printk(KERN_WARNING,
			"vma->vm_end is equal vma->vm_start : %lx\n",
				vma->vm_start);
		return 0;
	}
	if (vma->vm_pgoff > (~0UL >> PAGE_SHIFT)) {
		google_vp9_printk(KERN_WARNING,
		"the vma->vm_pgoff is %lx,it is large than the largest page number\n",
				vma->vm_pgoff);
		return -EINVAL;
	}


	temp_pfn = MACC_VP9_REGS_BASE >> 12;


	/* Set reserved and I/O flag for the area. */
	vma->vm_flags |= /*VM_RESERVED | */VM_IO;
	/* Select uncached access. */
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (io_remap_pfn_range(vma, vma->vm_start, temp_pfn,
		vma->vm_end - vma->vm_start, vma->vm_page_prot)) {
		return -EAGAIN;
	}


	vma->vm_ops = &cedardev_remap_vm_ops;
	googlevp9dev_vma_open(vma);

	return 0;
}

#ifdef CONFIG_PM
static int snd_sw_google_vp9_suspend(struct platform_device *pdev,
										pm_message_t state)
{
	int ret = 0;

	google_vp9_printk(KERN_WARNING, "[google_vp9] standby suspend\n");
	ret = disable_google_vp9_hw_clk();

	if (ret < 0) {
		google_vp9_printk(KERN_WARNING,
			"google_vp9 clk disable somewhere error!\n");
		return -EFAULT;
	}

	return 0;
}

static int snd_sw_google_vp9_resume(struct platform_device *pdev)
{
	int ret = 0;

	google_vp9_printk(KERN_WARNING, "[google_vp9] standby resume\n");

	if (google_vp9_devp->ref_count == 0)
		return 0;

	ret = enable_google_vp9_hw_clk();
	if (ret < 0) {
		google_vp9_printk(KERN_WARNING,
			"google_vp9 clk enable somewhere error!\n");
		return -EFAULT;
	}
	return 0;
}
#endif

static const struct file_operations googlevp9dev_fops = {
	.owner   = THIS_MODULE,
	.mmap    = googlevp9dev_mmap,
	.open    = googlevp9dev_open,
	.release = googlevp9dev_release,
	.llseek  = no_llseek,
	.unlocked_ioctl   = compat_googlevp9dev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_googlevp9dev_ioctl,
#endif
};

static int googleVp9dev_init(struct platform_device *pdev)
{
	int ret = 0;
	int devno;

#if defined(CONFIG_OF)
	struct device_node *node;
#endif
	dev_t dev;

	dev = 0;

	google_vp9_printk(KERN_WARNING, "[google vp9]: install start!!!\n");


#if defined(CONFIG_OF)
	node = pdev->dev.of_node;
#endif

	/*register or alloc the device number.*/
	if (vp9_dev_major) {
		dev = MKDEV(vp9_dev_major, vp9_dev_minor);
		ret = register_chrdev_region(dev, 1, "googlevp9_dev");
	} else {
		ret = alloc_chrdev_region(&dev, vp9_dev_minor, 1, "googlevp9_dev");
		vp9_dev_major = MAJOR(dev);
		vp9_dev_minor = MINOR(dev);
	}

	if (ret < 0) {
		google_vp9_printk(KERN_WARNING,
			"googlevp9_dev: can't get major %d\n",
			vp9_dev_major);
		return ret;
	}
	spin_lock_init(&google_vp9_spin_lock);
	google_vp9_devp = kmalloc(sizeof(struct googlevp9_dev), GFP_KERNEL);
	if (google_vp9_devp == NULL) {
		google_vp9_printk(KERN_WARNING,
			"malloc mem for google vp9 device err\n");
		return -ENOMEM;
	}
	memset(google_vp9_devp, 0, sizeof(struct googlevp9_dev));

#if defined(CONFIG_OF)
	google_vp9_devp->irq = irq_of_parse_and_map(node, 0);
	google_vp9_printk(KERN_INFO, "google vp9: the get irq is %d\n",
		google_vp9_devp->irq);
	if (google_vp9_devp->irq <= 0)
		google_vp9_printk(KERN_WARNING, "Can't parse IRQ");
#else
	google_vp9_devp->irq = SUNXI_IRQ_GOOGLE_VP9;
#endif

	sema_init(&google_vp9_devp->sem, 1);
	init_waitqueue_head(&google_vp9_devp->wq);

	memset(&google_vp9_devp->iomap_addrs, 0, sizeof(struct iomap_para));

	ret = request_irq(google_vp9_devp->irq,
				GoogleVp9Interupt, 0, "googlevp9_dev", NULL);
	if (ret < 0) {
		google_vp9_printk(KERN_WARNING, "request irq err\n");
		return -EINVAL;
	}

	/* map for macc io space */
#if defined(CONFIG_OF)
	google_vp9_devp->iomap_addrs.regs_macc = of_iomap(node, 0);
	if (!google_vp9_devp->iomap_addrs.regs_macc)
		google_vp9_printk(KERN_WARNING, "vp9 Can't map registers");

	google_vp9_devp->sram_bass_vir = (unsigned int *)of_iomap(node, 1);
	if (!google_vp9_devp->sram_bass_vir)
		google_vp9_printk(KERN_WARNING,
			"vp9 Can't map sram_bass_vir registers");

	google_vp9_devp->clk_bass_vir = (unsigned int *)of_iomap(node, 2);
	if (!google_vp9_devp->clk_bass_vir)
		google_vp9_printk(KERN_WARNING,
			"vp9 Can't map clk_bass_vir registers");
#endif

#if defined(CONFIG_OF)
	google_vp9_parent_pll_clk = of_clk_get(node, 0);
	if ((!google_vp9_parent_pll_clk) || IS_ERR(google_vp9_parent_pll_clk)) {
		google_vp9_printk(KERN_WARNING,
			"try to get google_vp9_parent_pll_clk fail\n");
		return -EINVAL;
	}

	google_vp9_moduleclk = of_clk_get(node, 1);
	if (!google_vp9_moduleclk || IS_ERR(google_vp9_moduleclk))
		google_vp9_printk(KERN_WARNING, "get google_vp9_moduleclk failed;\n");
#endif


	google_vp9_devp->platform_dev = &pdev->dev;

	sunxi_periph_reset_assert(google_vp9_moduleclk);
	clk_prepare(google_vp9_moduleclk);

	/* Create char device */
	devno = MKDEV(vp9_dev_major, vp9_dev_minor);
	cdev_init(&google_vp9_devp->cdev, &googlevp9dev_fops);
	google_vp9_devp->cdev.owner = THIS_MODULE;
	ret = cdev_add(&google_vp9_devp->cdev, devno, 1);
	if (ret)
		google_vp9_printk(KERN_WARNING, "Err:%d add cedardev", ret);

	google_vp9_devp->class = class_create(THIS_MODULE, "googlevp9_dev");
	google_vp9_devp->dev =
	device_create(google_vp9_devp->class,
									NULL, devno, NULL, "googlevp9_dev");

	setup_timer(&google_vp9_devp->vp9_engine_timer,
		vp9_engine_for_events, (unsigned long)google_vp9_devp);
	setup_timer(&google_vp9_devp->vp9_engine_timer_rel,
		vp9_engine_for_timer_rel, (unsigned long)google_vp9_devp);

	mutex_init(&google_vp9_devp->lock_mem);

	google_vp9_printk(KERN_WARNING, "[cedar]: install end!!!\n");
	return 0;
}


static void googleVp9dev_exit(void)
{
	dev_t dev;
	dev = MKDEV(vp9_dev_major, vp9_dev_minor);

	free_irq(google_vp9_devp->irq, NULL);
	iounmap(google_vp9_devp->iomap_addrs.regs_macc);
	/* Destroy char device */
	if (google_vp9_devp) {
		cdev_del(&google_vp9_devp->cdev);
		device_destroy(google_vp9_devp->class, dev);
		class_destroy(google_vp9_devp->class);
	}

	if (NULL == google_vp9_moduleclk ||
		IS_ERR(google_vp9_moduleclk)) {
		google_vp9_printk(KERN_WARNING,
		"google_vp9_moduleclk handle is invalid,just return!\n");
	} else {
		clk_disable_unprepare(google_vp9_moduleclk);
		clk_put(google_vp9_moduleclk);
		google_vp9_moduleclk = NULL;
	}

	if (NULL == google_vp9_parent_pll_clk ||
		IS_ERR(google_vp9_parent_pll_clk)) {
		google_vp9_printk(KERN_WARNING,
		"google_vp9_parent_pll_clk ,handle is invalid,just return!\n");
	} else {
		clk_put(google_vp9_parent_pll_clk);
	}

	unregister_chrdev_region(dev, 1);
	kfree(google_vp9_devp);
}

static int  sunxi_google_vp9_remove(struct platform_device *pdev)
{
	googleVp9dev_exit();
	return 0;
}

static int  sunxi_google_vp9_probe(struct platform_device *pdev)
{
	googleVp9dev_init(pdev);
	return 0;
}

/*share the irq no. with timer2*/
/*
static struct resource sunxi_cedar_resource[] = {
	[0] = {
		.start = SUNXI_IRQ_GOOGLE_VP9,
		.end   = SUNXI_IRQ_GOOGLE_VP9,
		.flags = IORESOURCE_IRQ,
	},
};

struct platform_device sunxi_device_cedar = {
	.name		= "sunxi-cedar",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(sunxi_cedar_resource),
	.resource	= sunxi_cedar_resource,
};
*/

static struct platform_driver sunxi_google_vp9_driver = {
	.probe		= sunxi_google_vp9_probe,
	.remove		= sunxi_google_vp9_remove,
#ifdef CONFIG_PM
	.suspend	= snd_sw_google_vp9_suspend,
	.resume		= snd_sw_google_vp9_resume,
#endif
	.driver		= {
		.name	= "sunxi-google-vp9",
		.owner	= THIS_MODULE,

#if defined(CONFIG_OF)
		.of_match_table = sunxi_google_vp9_match,
#endif
	},
};

static int __init sunxi_google_vp9_init(void)
{
	google_vp9_printk(KERN_WARNING, "sunxi google vp9 version 0.1\n");
	return platform_driver_register(&sunxi_google_vp9_driver);
}

static void __exit sunxi_google_vp9_exit(void)
{
	platform_driver_unregister(&sunxi_google_vp9_driver);
}

module_init(sunxi_google_vp9_init);
module_exit(sunxi_google_vp9_exit);


MODULE_AUTHOR("Soft-Reuuimlla");
MODULE_DESCRIPTION("User mode GOOGLE VP9 device interface");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
MODULE_ALIAS("platform:cedarx-sunxi");

