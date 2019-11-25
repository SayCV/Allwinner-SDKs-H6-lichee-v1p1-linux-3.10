/*
 * linux-3.10/drivers/video/sunxi/disp2/disp/de/disp_manager.c
 *
 * Copyright (c) 2007-2017 Allwinnertech Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "disp_manager.h"
#if defined(__LINUX_PLAT__)
#include "../dev_disp.h"
#endif

#define DMABUF_CACHE_MAX_EACH 10
#define FORCE_SYNC_THRESHOLD 3
#define DMABUF_REF_SIZE 7

struct disp_manager_private_data {
	bool applied;
	bool enabled;
	bool color_range_modified;
	struct disp_manager_data *cfg;

	s32 (*shadow_protect)(u32 disp, bool protect);

	u32 reg_base;
	u32 irq_no;
	struct clk *clk;
	struct clk *clk_parent;
	struct clk *extra_clk;
	unsigned int layers_using;
	bool sync;
	bool force_sync;
	unsigned int nosync_cnt;
	unsigned int force_sync_cnt;
	bool err;
	unsigned int err_cnt;
	unsigned int dmabuf_unmap_skip_cnt;
	struct list_head dmabuf_list;
	unsigned int dmabuf_cnt;
	unsigned int dmabuf_cnt_max;
	unsigned int dmabuf_overflow_cnt;
	unsigned long long dmabuf_ref[DMABUF_REF_SIZE];
};

#if defined(__LINUX_PLAT__)
static spinlock_t mgr_data_lock;
static struct mutex mgr_mlock;
#else
static u32 mgr_data_lock;
//static u32 mgr_mlock;
#endif

static struct disp_manager *mgrs = NULL;
static struct disp_manager_private_data *mgr_private;
static struct disp_manager_data *mgr_cfgs;

static struct disp_layer_config_data* lyr_cfgs;

/*
 * layer unit
 */
struct disp_layer_private_data {
	struct disp_layer_config_data *cfg;
	s32 (*shadow_protect)(u32 sel, bool protect);
};

static struct disp_layer *lyrs = NULL;
static struct disp_layer_private_data *lyr_private;

struct disp_layer* disp_get_layer(u32 disp, u32 chn, u32 layer_id)
{
	u32 num_screens, max_num_layers = 0;
	struct disp_layer *lyr = lyrs;
	int i;

	num_screens = bsp_disp_feat_get_num_screens();
	if ((disp >= num_screens)) {
		DE_WRN("disp %d is out of range %d\n",
			disp, num_screens);
		return NULL;
	}

	for (i=0; i<num_screens; i++) {
		max_num_layers += bsp_disp_feat_get_num_layers(i);
	}

	for (i=0; i<max_num_layers; i++) {
		if ((lyr->disp == disp) && (lyr->chn == chn) && (lyr->id == layer_id)) {
			DE_INF("%d,%d,%d, name=%s\n", disp, chn, layer_id, lyr->name);
			return lyr;
		}
		lyr++;
	}

	DE_WRN("%s (%d,%d,%d) fail\n", __func__, disp, chn, layer_id);
	return NULL;

}

struct disp_layer* disp_get_layer_1(u32 disp, u32 layer_id)
{
	u32 num_screens, num_layers;
	u32 i,k;
	u32 layer_index = 0, start_index = 0;

	num_screens = bsp_disp_feat_get_num_screens();
	if ((disp >= num_screens)) {
		DE_WRN("disp %d is out of range %d\n",
			disp, num_screens);
		return NULL;
	}

	for (i=0; i<disp; i++)
		start_index += bsp_disp_feat_get_num_layers(i);

	layer_id += start_index;

	for (i=0; i<num_screens; i++) {
			num_layers = bsp_disp_feat_get_num_layers(i);
			for (k=0; k<num_layers; k++) {
				if (layer_index == layer_id) {
					DE_INF("disp%d layer%d: %d,%d,%d\n", disp, layer_id, lyrs[layer_index].disp, lyrs[layer_index].chn, lyrs[layer_index].id);
					return &lyrs[layer_index];
				}
				layer_index ++;
		}
	}

	DE_WRN("%s fail\n", __func__);
	return NULL;
}

static struct disp_layer_private_data *disp_lyr_get_priv(struct disp_layer *lyr)
{
	if (NULL == lyr) {
		DE_WRN("NULL hdl!\n");
		return NULL;
	}

	return (struct disp_layer_private_data *)lyr->data;
}

/** __disp_config_transfer2inner - transfer disp_layer_config to inner one
 */
s32 __disp_config_transfer2inner(
	struct disp_layer_config_inner *config_inner,
	struct disp_layer_config *config)
{
	config_inner->enable = config->enable;
	config_inner->channel = config->channel;
	config_inner->layer_id = config->layer_id;
	/* layer info */
	config_inner->info.mode = config->info.mode;
	config_inner->info.zorder = config->info.zorder;
	config_inner->info.alpha_mode = config->info.alpha_mode;
	config_inner->info.alpha_value = config->info.alpha_value;
	memcpy(&config_inner->info.screen_win,
	       &config->info.screen_win,
	       sizeof(struct disp_rect));
	config_inner->info.b_trd_out = config->info.b_trd_out;
	config_inner->info.out_trd_mode = config->info.out_trd_mode;
	config_inner->info.id = config->info.id;
	/* fb info */
	memcpy(config_inner->info.fb.addr,
	       config->info.fb.addr,
	       sizeof(long long) * 3);
	memcpy(config_inner->info.fb.size,
	       config->info.fb.size,
	       sizeof(struct disp_rectsz) * 3);
	memcpy(config_inner->info.fb.align,
	    config->info.fb.align, sizeof(int) * 3);
	config_inner->info.fb.format = config->info.fb.format;
	config_inner->info.fb.color_space = config->info.fb.color_space;
	memcpy(config_inner->info.fb.trd_right_addr,
	       config->info.fb.trd_right_addr,
	       sizeof(int) * 3);
	config_inner->info.fb.pre_multiply = config->info.fb.pre_multiply;
	memcpy(&config_inner->info.fb.crop,
	       &config->info.fb.crop,
	       sizeof(struct disp_rect64));
	config_inner->info.fb.flags = config->info.fb.flags;
	config_inner->info.fb.scan = config->info.fb.scan;
	config_inner->info.fb.eotf = DISP_EOTF_UNDEF;
	config_inner->info.fb.fbd_en = 0;
	config_inner->info.fb.metadata_buf = 0;
	config_inner->info.fb.fd = 0;
	config_inner->info.fb.metadata_size = 0;
	config_inner->info.fb.metadata_flag = 0;

	if (config_inner->info.mode == LAYER_MODE_COLOR)
		config_inner->info.color = config->info.color;

	return 0;
}

/** __disp_config2_transfer2inner -transfer disp_layer_config2 to inner one
 */
s32 __disp_config2_transfer2inner(struct disp_layer_config_inner *config_inner,
				      struct disp_layer_config2 *config2)
{
	config_inner->enable = config2->enable;
	config_inner->channel = config2->channel;
	config_inner->layer_id = config2->layer_id;
	/* layer info */
	config_inner->info.mode = config2->info.mode;
	config_inner->info.zorder = config2->info.zorder;
	config_inner->info.alpha_mode = config2->info.alpha_mode;
	config_inner->info.alpha_value = config2->info.alpha_value;
	memcpy(&config_inner->info.screen_win,
	       &config2->info.screen_win,
	       sizeof(struct disp_rect));
	config_inner->info.b_trd_out = config2->info.b_trd_out;
	config_inner->info.out_trd_mode = config2->info.out_trd_mode;
	/* fb info */
	config_inner->info.fb.fd = config2->info.fb.fd;
	memcpy(config_inner->info.fb.size,
	       config2->info.fb.size,
	       sizeof(struct disp_rectsz) * 3);
	memcpy(config_inner->info.fb.align,
	    config2->info.fb.align, sizeof(int) * 3);
	config_inner->info.fb.format = config2->info.fb.format;
	config_inner->info.fb.color_space = config2->info.fb.color_space;
	config_inner->info.fb.trd_right_fd = config2->info.fb.trd_right_fd;
	config_inner->info.fb.pre_multiply = config2->info.fb.pre_multiply;
	memcpy(&config_inner->info.fb.crop,
	       &config2->info.fb.crop,
	       sizeof(struct disp_rect64));
	config_inner->info.fb.flags = config2->info.fb.flags;
	config_inner->info.fb.scan = config2->info.fb.scan;
	config_inner->info.fb.depth = config2->info.fb.depth;
	/* hdr related */
	config_inner->info.fb.eotf = config2->info.fb.eotf;
	config_inner->info.fb.fbd_en = config2->info.fb.fbd_en;
	config_inner->info.fb.metadata_fd = config2->info.fb.metadata_fd;
	config_inner->info.fb.metadata_size = config2->info.fb.metadata_size;
	config_inner->info.fb.metadata_flag =
	    config2->info.fb.metadata_flag;

	config_inner->info.id = config2->info.id;
	/* atw related */
	config_inner->info.atw.used = config2->info.atw.used;
	config_inner->info.atw.mode = config2->info.atw.mode;
	config_inner->info.atw.b_row = config2->info.atw.b_row;
	config_inner->info.atw.b_col = config2->info.atw.b_col;
	config_inner->info.atw.cof_fd = config2->info.atw.cof_fd;
	if (config_inner->info.mode == LAYER_MODE_COLOR)
		config_inner->info.color = config2->info.color;

	return 0;
}

/** __disp_inner_transfer2config - transfer inner to disp_layer_config
 */
s32 __disp_inner_transfer2config(struct disp_layer_config *config,
				 struct disp_layer_config_inner *config_inner)
{
	config->enable = config_inner->enable;
	config->channel = config_inner->channel;
	config->layer_id = config_inner->layer_id;
	/* layer info */
	config->info.mode = config_inner->info.mode;
	config->info.zorder = config_inner->info.zorder;
	config->info.alpha_mode = config_inner->info.alpha_mode;
	config->info.alpha_value = config_inner->info.alpha_value;
	memcpy(&config->info.screen_win,
	       &config_inner->info.screen_win,
	       sizeof(struct disp_rect));
	config->info.b_trd_out = config_inner->info.b_trd_out;
	config->info.out_trd_mode = config_inner->info.out_trd_mode;
	config->info.id = config_inner->info.id;
	/* fb info */
	memcpy(config->info.fb.addr,
	       config_inner->info.fb.addr,
	       sizeof(long long) * 3);
	memcpy(config->info.fb.size,
	       config_inner->info.fb.size,
	       sizeof(struct disp_rectsz) * 3);
	memcpy(config->info.fb.align,
	    config_inner->info.fb.align, sizeof(int) * 3);
	config->info.fb.format = config_inner->info.fb.format;
	config->info.fb.color_space = config_inner->info.fb.color_space;
	memcpy(config->info.fb.trd_right_addr,
	       config_inner->info.fb.trd_right_addr,
	       sizeof(int) * 3);
	config->info.fb.pre_multiply = config_inner->info.fb.pre_multiply;
	memcpy(&config->info.fb.crop,
	       &config_inner->info.fb.crop,
	       sizeof(struct disp_rect64));
	config->info.fb.flags = config_inner->info.fb.flags;
	config->info.fb.scan = config_inner->info.fb.scan;

	if (config->info.mode == LAYER_MODE_COLOR)
		config->info.color = config_inner->info.color;

	return 0;
}

/** __disp_inner_transfer2config2 - transfer inner to disp_layer_config2
 */
s32 __disp_inner_transfer2config2(struct disp_layer_config2 *config2,
				  struct disp_layer_config_inner *config_inner)
{
	config2->enable = config_inner->enable;
	config2->channel = config_inner->channel;
	config2->layer_id = config_inner->layer_id;
	/* layer info */
	config2->info.mode = config_inner->info.mode;
	config2->info.zorder = config_inner->info.zorder;
	config2->info.alpha_mode = config_inner->info.alpha_mode;
	config2->info.alpha_value = config_inner->info.alpha_value;
	memcpy(&config2->info.screen_win,
	       &config_inner->info.screen_win,
	       sizeof(struct disp_rect));
	config2->info.b_trd_out = config_inner->info.b_trd_out;
	config2->info.out_trd_mode = config_inner->info.out_trd_mode;
	/* fb info */
	config2->info.fb.fd = config_inner->info.fb.fd;
	memcpy(config2->info.fb.size,
	       config_inner->info.fb.size,
	       sizeof(struct disp_rectsz) * 3);
	memcpy(config2->info.fb.align,
	    config_inner->info.fb.align, sizeof(int) * 3);
	config2->info.fb.format = config_inner->info.fb.format;
	config2->info.fb.color_space = config_inner->info.fb.color_space;
	config2->info.fb.trd_right_fd = config_inner->info.fb.trd_right_fd;
	config2->info.fb.pre_multiply = config_inner->info.fb.pre_multiply;
	memcpy(&config2->info.fb.crop,
	       &config_inner->info.fb.crop,
	       sizeof(struct disp_rect64));
	config2->info.fb.flags = config_inner->info.fb.flags;
	config2->info.fb.scan = config_inner->info.fb.scan;
	config2->info.fb.depth = config_inner->info.fb.depth;
	/* hdr related */
	config2->info.fb.eotf = config_inner->info.fb.eotf;
	config2->info.fb.fbd_en = config_inner->info.fb.fbd_en;
	config2->info.fb.metadata_fd = config_inner->info.fb.metadata_fd;
	config2->info.fb.metadata_size = config_inner->info.fb.metadata_size;
	config2->info.fb.metadata_flag =
	    config_inner->info.fb.metadata_flag;

	config2->info.id = config_inner->info.id;
	/* atw related */
	config2->info.atw.used = config_inner->info.atw.used;
	config2->info.atw.mode = config_inner->info.atw.mode;
	config2->info.atw.b_row = config_inner->info.atw.b_row;
	config2->info.atw.b_col = config_inner->info.atw.b_col;
	config2->info.atw.cof_fd = config_inner->info.atw.cof_fd;
	if (config2->info.mode == LAYER_MODE_COLOR)
		config2->info.color = config_inner->info.color;

	return 0;
}

static s32 disp_lyr_set_manager(struct disp_layer *lyr, struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp) || (NULL == mgr)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	lyr->manager = mgr;
	list_add_tail(&lyr->list, &mgr->lyr_list);
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_lyr_unset_manager(struct disp_layer *lyr)
{
	unsigned long flags;
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	lyr->manager = NULL;
	list_del(&lyr->list);
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_lyr_check(struct disp_layer *lyr, struct disp_layer_config *config)
{
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	return DIS_SUCCESS;
}

static s32 disp_lyr_check2(struct disp_layer *lyr,
			   struct disp_layer_config2 *config)
{
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	return DIS_SUCCESS;
}

static s32 disp_lyr_save_and_dirty_check(struct disp_layer *lyr,
					 struct disp_layer_config *config)
{
	unsigned long flags;
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	if (lyrp->cfg) {
		struct disp_layer_config_inner *pre_config = &lyrp->cfg->config;
		if ((pre_config->enable != config->enable) ||
			(pre_config->info.fb.addr[0] != config->info.fb.addr[0]) ||
			(pre_config->info.fb.format != config->info.fb.format) ||
			(pre_config->info.fb.flags != config->info.fb.flags))
			lyrp->cfg->flag |= LAYER_ATTR_DIRTY;
		if ((pre_config->info.fb.size[0].width != config->info.fb.size[0].width) ||
			(pre_config->info.fb.size[0].height != config->info.fb.size[0].height) ||
			(pre_config->info.fb.crop.width != config->info.fb.crop.width) ||
			(pre_config->info.fb.crop.height != config->info.fb.crop.height) ||
			(pre_config->info.screen_win.width != config->info.screen_win.width) ||
			(pre_config->info.screen_win.height != config->info.screen_win.height))
			lyrp->cfg->flag |= LAYER_SIZE_DIRTY;
			lyrp->cfg->flag = LAYER_ALL_DIRTY;

		if ((pre_config->enable == config->enable) &&
			(config->enable == 0))
			lyrp->cfg->flag = 0;
		__disp_config_transfer2inner(&lyrp->cfg->config, config);
	} else {
		DE_INF("cfg is NULL\n");
	}
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_lyr_save_and_dirty_check2(struct disp_layer *lyr,
					 struct disp_layer_config2 *config)
{
	unsigned long flags;
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);
	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	if (lyrp->cfg) {
		struct disp_layer_config_inner *pre_cfg = &lyrp->cfg->config;
		struct disp_layer_info_inner *pre_info = &pre_cfg->info;
		struct disp_layer_info2 *info = &config->info;
		struct disp_fb_info_inner *pre_fb = &pre_info->fb;
		struct disp_fb_info2 *fb = &info->fb;

		if ((pre_cfg->enable != config->enable) ||
		    (pre_fb->fd != fb->fd) ||
		    (pre_fb->format != fb->format) ||
		    (pre_fb->flags != fb->flags))
			lyrp->cfg->flag |= LAYER_ATTR_DIRTY;
		if ((pre_fb->size[0].width != fb->size[0].width) ||
		    (pre_fb->size[0].height != info->fb.size[0].height) ||
		    (pre_fb->crop.width != info->fb.crop.width) ||
		    (pre_fb->crop.height != info->fb.crop.height) ||
		    (pre_info->screen_win.width != info->screen_win.width) ||
		    (pre_info->screen_win.height != info->screen_win.height))
			lyrp->cfg->flag |= LAYER_SIZE_DIRTY;

		lyrp->cfg->flag = LAYER_ALL_DIRTY;

		if ((pre_cfg->enable == config->enable) &&
			(config->enable == 0))
			lyrp->cfg->flag = 0;
		__disp_config2_transfer2inner(&lyrp->cfg->config,
						      config);
	} else {
		DE_INF("cfg is NULL\n");
	}
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_lyr_is_dirty(struct disp_layer *lyr)
{
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	if (lyrp->cfg) {
		return (lyrp->cfg->flag & LAYER_ALL_DIRTY);
	}

	return 0;
}

static s32 disp_lyr_dirty_clear(struct disp_layer *lyr)
{
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	lyrp->cfg->flag = 0;

	return 0;
}

static s32 disp_lyr_get_config2(struct disp_layer *lyr,
				struct disp_layer_config2 *config)
{
	unsigned long flags;
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	if (lyrp->cfg)
		__disp_inner_transfer2config2(config, &lyrp->cfg->config);
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_lyr_get_config(struct disp_layer *lyr,
			       struct disp_layer_config *config)
{
	unsigned long flags;
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	if (lyrp->cfg)
		__disp_inner_transfer2config(config, &lyrp->cfg->config);
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}
static s32 disp_lyr_apply(struct disp_layer *lyr)
{
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	return DIS_SUCCESS;
}

static s32 disp_lyr_force_apply(struct disp_layer *lyr)
{
	unsigned long flags;
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	lyrp->cfg->flag |= LAYER_ALL_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);
	disp_lyr_apply(lyr);

	return DIS_SUCCESS;
}

static s32 disp_lyr_dump(struct disp_layer *lyr, char *buf)
{
	unsigned long flags;
	struct disp_layer_config_data data;
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);
	u32 count = 0;

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	memcpy(&data, lyrp->cfg, sizeof(struct disp_layer_config_data));
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	count += sprintf(buf + count, " %5s ", (data.config.info.mode == LAYER_MODE_BUFFER)? "BUF":"COLOR");
	count += sprintf(buf + count, "%3s ", (data.config.enable == 1) ? "en" : "dis");
	count += sprintf(buf + count, "ch[%1d] ", data.config.channel);
	count += sprintf(buf + count, "lyr[%1d] ", data.config.layer_id);
	count += sprintf(buf + count, "z[%1d] ", data.config.info.zorder);
	count += sprintf(buf + count, "prem[%1s] ", (data.config.info.fb.pre_multiply)? "Y":"N");
	count += sprintf(buf + count, "fbd[%1s] ", (data.config.info.fb.fbd_en) ? "Y" : "N");
	count += sprintf(buf + count, "a[%5s %3d] ", (data.config.info.alpha_mode)? "globl":"pixel", data.config.info.alpha_value);
	count += sprintf(buf + count, "fmt[%3d] ", data.config.info.fb.format);
	count += sprintf(buf + count, "fb[%4d,%4d;%4d,%4d;%4d,%4d] ", data.config.info.fb.size[0].width, data.config.info.fb.size[0].height,
		data.config.info.fb.size[1].width, data.config.info.fb.size[1].height, data.config.info.fb.size[2].width, data.config.info.fb.size[2].height);
	count += sprintf(buf + count, "crop[%4d,%4d,%4d,%4d] ", (unsigned int)(data.config.info.fb.crop.x>>32), (unsigned int)(data.config.info.fb.crop.y>>32),
		(unsigned int)(data.config.info.fb.crop.width>>32), (unsigned int)(data.config.info.fb.crop.height>>32));
	count += sprintf(buf + count, "frame[%4d,%4d,%4d,%4d] ", data.config.info.screen_win.x, data.config.info.screen_win.y, data.config.info.screen_win.width, data.config.info.screen_win.height);
	count += sprintf(buf + count, "addr[%08llx,%08llx,%08llx] ", data.config.info.fb.addr[0], data.config.info.fb.addr[1], data.config.info.fb.addr[2]);
	count += sprintf(buf + count, "right[%08x,%08x,%08x] ", data.config.info.fb.trd_right_addr[0], data.config.info.fb.trd_right_addr[1], data.config.info.fb.trd_right_addr[2]);
	count += sprintf(buf + count, "flags[0x%02x] trd[%1d,%1d] ", data.config.info.fb.flags, data.config.info.b_trd_out, data.config.info.out_trd_mode);
	count += sprintf(buf + count, "depth[%2d]\n", data.config.info.fb.depth);

	return count;
}

static s32 disp_init_lyr(disp_bsp_init_para * para)
{
	u32 num_screens = 0, num_channels = 0, num_layers = 0;
	u32 max_num_layers = 0;
	u32 disp, chn, layer_id, layer_index = 0;

	DE_INF("disp_init_lyr\n");

	num_screens = bsp_disp_feat_get_num_screens();
	for (disp=0; disp<num_screens; disp++) {
		max_num_layers += bsp_disp_feat_get_num_layers(disp);
	}

	lyrs = (struct disp_layer *)kmalloc(sizeof(struct disp_layer) * max_num_layers, GFP_KERNEL | __GFP_ZERO);
	if (NULL == lyrs) {
		DE_WRN("malloc memory fail! size=0x%x\n", (unsigned int)sizeof(struct disp_layer) * max_num_layers);
		return DIS_FAIL;
	}

	lyr_private = (struct disp_layer_private_data *)kmalloc(sizeof(struct disp_layer_private_data)\
		* max_num_layers, GFP_KERNEL | __GFP_ZERO);
	if (NULL == lyr_private) {
		DE_WRN("malloc memory fail! size=0x%x\n", (unsigned int)sizeof(struct disp_layer_private_data) * max_num_layers);
		return DIS_FAIL;
	}

	lyr_cfgs = (struct disp_layer_config_data *)kmalloc(sizeof(struct disp_layer_config_data)\
		* max_num_layers, GFP_KERNEL | __GFP_ZERO);
	if (NULL == lyr_cfgs) {
		DE_WRN("malloc memory fail! size=0x%x\n", (unsigned int)sizeof(struct disp_layer_private_data) * max_num_layers);
		return DIS_FAIL;
	}

	for (disp=0; disp<num_screens; disp++) {

		num_channels = bsp_disp_feat_get_num_channels(disp);
		for (chn=0; chn<num_channels; chn++) {
			num_layers = bsp_disp_feat_get_num_layers_by_chn(disp, chn);
			for (layer_id=0; layer_id<num_layers; layer_id++,layer_index ++) {
				struct disp_layer *lyr = &lyrs[layer_index];
				struct disp_layer_config_data *lyr_cfg = &lyr_cfgs[layer_index];
				struct disp_layer_private_data *lyrp = &lyr_private[layer_index];

				lyrp->shadow_protect = para->shadow_protect;
				lyrp->cfg = lyr_cfg;

				lyr_cfg->ops.vmap = disp_vmap;
				lyr_cfg->ops.vunmap = disp_vunmap;
				sprintf(lyr->name, "mgr%d chn%d lyr%d", disp, chn, layer_id);
				lyr->disp = disp;
				lyr->chn = chn;
				lyr->id = layer_id;
				lyr->data = (void*)lyrp;

				lyr->set_manager = disp_lyr_set_manager;
				lyr->unset_manager = disp_lyr_unset_manager;
				lyr->apply = disp_lyr_apply;
				lyr->force_apply = disp_lyr_force_apply;
				lyr->check = disp_lyr_check;
				lyr->check2 = disp_lyr_check2;
				lyr->save_and_dirty_check =
				    disp_lyr_save_and_dirty_check;
				lyr->save_and_dirty_check2 =
				    disp_lyr_save_and_dirty_check2;
				lyr->get_config = disp_lyr_get_config;
				lyr->get_config2 = disp_lyr_get_config2;
				lyr->dump = disp_lyr_dump;
				lyr->is_dirty = disp_lyr_is_dirty;
				lyr->dirty_clear = disp_lyr_dirty_clear;
			}
		}
	}

	return 0;
}

struct disp_manager* disp_get_layer_manager(u32 disp)
{
	u32 num_screens;

	num_screens = bsp_disp_feat_get_num_screens();
	if (disp >= num_screens) {
		DE_WRN("disp %d out of range\n", disp);
		return NULL;
	}

	return &mgrs[disp];
}
EXPORT_SYMBOL(disp_get_layer_manager);

static struct disp_manager_private_data *disp_mgr_get_priv(struct disp_manager *mgr)
{
	if (NULL == mgr) {
		DE_WRN("NULL hdl!\n");
		return NULL;
	}

	return &mgr_private[mgr->disp];
}

static struct disp_layer_config_data * disp_mgr_get_layer_cfg_head(struct disp_manager *mgr)
{
	int layer_index = 0, disp;
	int num_screens = bsp_disp_feat_get_num_screens();

	for (disp=0; disp<num_screens && disp<mgr->disp; disp++) {
		layer_index += bsp_disp_feat_get_num_layers(disp);
	}
	return &lyr_cfgs[layer_index];
}

static s32 disp_mgr_shadow_protect(struct disp_manager *mgr, bool protect)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	if (mgrp->shadow_protect)
		return mgrp->shadow_protect(mgr->disp, protect);

	return -1;
}

#if 0
extern void sync_event_proc(u32 disp);
#if defined(__LINUX_PLAT__)
s32 disp_mgr_event_proc(int irq, void *parg)
#else
static s32 disp_mgr_event_proc(void *parg)
#endif
{
	u32 disp = (u32)parg;

	if (disp_al_manager_query_irq(disp)) {
		/* FIXME, curline & start_delay */
			sync_event_proc(disp);
	}

	return DISP_IRQ_RETURN;
}
#endif

static s32 disp_mgr_clk_init(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	mgrp->clk_parent = clk_get_parent(mgrp->clk);
	mgrp->cfg->config.de_freq = clk_get_rate(mgrp->clk);

	return 0;
}

static s32 disp_mgr_clk_exit(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	if (mgrp->clk_parent)
		clk_put(mgrp->clk_parent);

	return 0;
}

static s32 disp_mgr_clk_enable(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	int ret = 0;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	DE_INF("mgr %d clk enable\n", mgr->disp);
	ret = clk_prepare_enable(mgrp->clk);
	if (0 != ret)
		DE_WRN("fail enable mgr's clock!\n");

	if (mgrp->extra_clk) {
		ret = clk_prepare_enable(mgrp->extra_clk);
		if (0 != ret)
			DE_WRN("fail enable mgr's extra_clk!\n");
	}

	return ret;
}

static s32 disp_mgr_clk_disable(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	if (mgrp->extra_clk)
		clk_disable(mgrp->extra_clk);

	clk_disable(mgrp->clk);

	return 0;
}

/* Return: unit(hz) */
static s32 disp_mgr_get_clk_rate(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return 0;
	}

	return mgrp->cfg->config.de_freq;
}

static s32 disp_mgr_init(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	disp_mgr_clk_init(mgr);
	return 0;
}

static s32 disp_mgr_exit(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	//FIXME, disable manager
	disp_mgr_clk_exit(mgr);

	return 0;
}

static s32 disp_mgr_set_back_color(struct disp_manager *mgr, struct disp_color *back_color)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	memcpy(&mgrp->cfg->config.back_color, back_color, sizeof(struct disp_color));
	mgrp->cfg->flag |= MANAGER_BACK_COLOR_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	mgr->apply(mgr);

	return DIS_SUCCESS;
}

static s32 disp_mgr_get_back_color(struct disp_manager *mgr, struct disp_color *back_color)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

  spin_lock_irqsave(&mgr_data_lock, flags);
	memcpy(back_color, &mgrp->cfg->config.back_color, sizeof(struct disp_color));
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_mgr_set_color_key(struct disp_manager *mgr, struct disp_colorkey *ck)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	memcpy(&mgrp->cfg->config.ck, ck, sizeof(struct disp_colorkey));
	mgrp->cfg->flag |= MANAGER_CK_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	mgr->apply(mgr);

	return DIS_SUCCESS;
}

static s32 disp_mgr_get_color_key(struct disp_manager *mgr, struct disp_colorkey *ck)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	memcpy(ck, &mgrp->cfg->config.ck, sizeof(struct disp_colorkey));
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_mgr_set_output_color_range(struct disp_manager *mgr, u32 color_range)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->cfg->config.color_range = color_range;
	mgrp->cfg->flag |= MANAGER_COLOR_RANGE_DIRTY;
	mgrp->color_range_modified = true;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	mgr->apply(mgr);

	return DIS_SUCCESS;
}

static s32 disp_mgr_get_output_color_range(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return 0;
	}

	return mgrp->cfg->config.color_range;
}

static s32 disp_mgr_update_color_space(struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	unsigned int cs = 0, color_range = 0;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	if (mgr->device) {
		if (mgr->device->get_input_csc)
			cs = mgr->device->get_input_csc(mgr->device);
		if (mgr->device && mgr->device->get_input_color_range)
			color_range = mgr->device->get_input_color_range(mgr->device);
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->cfg->config.cs = cs;
	if (!mgrp->color_range_modified)
		mgrp->cfg->config.color_range = color_range;
	mgrp->cfg->flag |= MANAGER_COLOR_SPACE_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	mgr->apply(mgr);

	return DIS_SUCCESS;
}

static s32 disp_mgr_smooth_switch(struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	struct disp_device_config dev_config;
	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	if (mgr->device) {

		if (mgr->device->get_static_config)
			mgr->device->get_static_config(mgr->device,
								&dev_config);
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->cfg->config.color_space = dev_config.cs;
	mgrp->cfg->config.eotf = dev_config.eotf;
	mgrp->cfg->flag |= MANAGER_COLOR_SPACE_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	mgr->apply(mgr);

	return DIS_SUCCESS;
}



static int _force_layer_en;
static struct disp_layer_config backup_layer[2][16];
static int backup_layer_num;

static void layer_mask_init(unsigned int *mask, unsigned int total)
{
	*mask = (0x00000001 << total) - 1;
}
static void layer_mask_clear(unsigned int *mask, unsigned int channel, unsigned int id)
{
	unsigned int bit = (0x00000001 << id) << (channel * 4);
	(*mask) = (*mask) & (~bit);
}
static int layer_mask_test(unsigned int *mask, unsigned int channel, unsigned int id)
{
	unsigned int bit = (0x00000001 << id) << (channel * 4);
	return (*mask) & bit;
}

static s32 disp_mgr_force_set_layer_config(struct disp_manager *mgr, struct disp_layer_config *config, unsigned int layer_num)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	unsigned int num_layers = 0, layer_index = 0;
	struct disp_layer *lyr = NULL;
	unsigned int mask = 0;
	struct disp_layer_config dummy;
	struct disp_layer *pre_lyr = NULL;
	int channel, id;
	int layers_cnt = 0;
	int channels_cnt = 0;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	mutex_lock(&mgr_mlock);
	_force_layer_en = 1;

	num_layers = bsp_disp_feat_get_num_layers(mgr->disp);
	if ((NULL == config) || (layer_num == 0) || (layer_num > num_layers)) {
		DE_WRN("NULL hdl!\n");
		mutex_unlock(&mgr_mlock);
		return -1;
	}

	layer_mask_init(&mask, num_layers);
	for (layer_index = 0; layer_index < layer_num; layer_index++) {

		lyr = disp_get_layer(mgr->disp, config->channel, config->layer_id);
		if (NULL == lyr)
			continue;
		if (!lyr->check(lyr, config)) {
			lyr->save_and_dirty_check(lyr, config);
		}
		layer_mask_clear(&mask, config->channel, config->layer_id);
		config++;
	}

	channels_cnt = bsp_disp_feat_get_num_channels(mgr->disp);
	memset(&dummy, 0, sizeof(dummy));
	for (channel = 0; channel < channels_cnt; channel++) {
		layers_cnt = bsp_disp_feat_get_num_layers_by_chn(mgr->disp, channel);
		for (id = 0; id < layers_cnt; id++) {
			if (layer_mask_test(&mask, channel, id) == 0)
				continue;

			layer_mask_clear(&mask, channel, id);
			pre_lyr = disp_get_layer(mgr->disp, channel, id);
			if (NULL == pre_lyr)
				continue;

			dummy.channel = channel;
			dummy.layer_id = id;
			if (!pre_lyr->check(pre_lyr, &dummy)) {
				pre_lyr->save_and_dirty_check(pre_lyr, &dummy);
			}
		}
	}

	if (mgr->apply)
		mgr->apply(mgr);

	list_for_each_entry(lyr, &mgr->lyr_list, list) {
		lyr->dirty_clear(lyr);
	}

	mutex_unlock(&mgr_mlock);
	return DIS_SUCCESS;
}

static s32 disp_mgr_force_set_layer_config_exit(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	unsigned int num_layers = 16, layer_index = 0;
	unsigned int layer_num;
	struct disp_layer *lyr = NULL;
	struct disp_layer_config *backup;
	struct disp_layer_config dummy;
	int channel = 0;
	int id = 0;
	int layers_cnt = 0;
	int channels_cnt;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	num_layers = bsp_disp_feat_get_num_layers(mgr->disp);

	mutex_lock(&mgr_mlock);
	layer_num = backup_layer_num;
	backup = &(backup_layer[mgr->disp][0]);

	if (layer_num <= 0 || layer_num > num_layers) {
		DE_WRN("invalid input params!\n");
		mutex_unlock(&mgr_mlock);
		return -1;
	}

	/* disable all layer first */
	channels_cnt = bsp_disp_feat_get_num_channels(mgr->disp);

	memset(&dummy, 0, sizeof(dummy));
	for (channel = 0; channel < channels_cnt; channel++) {
		layers_cnt = bsp_disp_feat_get_num_layers_by_chn(mgr->disp, channel);
		for (id = 0; id < layers_cnt; id++) {

			lyr = disp_get_layer(mgr->disp, channel, id);
			if (NULL == lyr)
				continue;

			dummy.channel = channel;
			dummy.layer_id = id;
			if (!lyr->check(lyr, &dummy)) {
				lyr->save_and_dirty_check(lyr, &dummy);
			}
		}
	}

	for (layer_index = 0; layer_index < layer_num; layer_index++) {

		lyr = disp_get_layer(mgr->disp, backup->channel, backup->layer_id);
		if (NULL == lyr)
			continue;
		if (!lyr->check(lyr, backup)) {
			lyr->save_and_dirty_check(lyr, backup);
		}
		backup++;
	}

	if (mgr->apply)
		mgr->apply(mgr);

	list_for_each_entry(lyr, &mgr->lyr_list, list) {
		lyr->dirty_clear(lyr);
	}
	_force_layer_en = 0;
	mutex_unlock(&mgr_mlock);

	return DIS_SUCCESS;
}

static s32 disp_mgr_set_layer_config(struct disp_manager *mgr,
	struct disp_layer_config *config, unsigned int layer_num)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	unsigned int num_layers = 0, layer_index = 0;
	struct disp_layer *lyr = NULL;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	DE_INF("mgr%d, config %d layers\n", mgr->disp, layer_num);

	num_layers = bsp_disp_feat_get_num_layers(mgr->disp);
	if ((NULL == config) || (layer_num == 0) || (layer_num > num_layers)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	DE_INF("layer:ch%d, layer%d, format=%d, size=<%d,%d>, crop=<%lld,%lld,%lld,%lld>,frame=<%d,%d>, en=%d addr[0x%llx,0x%llx,0x%llx> alpha=<%d,%d>\n",
		config->channel, config->layer_id,  config->info.fb.format,
		config->info.fb.size[0].width, config->info.fb.size[0].height,
		config->info.fb.crop.x>>32, config->info.fb.crop.y>>32, config->info.fb.crop.width>>32, config->info.fb.crop.height>>32,
		config->info.screen_win.width, config->info.screen_win.height,
		config->enable, config->info.fb.addr[0], config->info.fb.addr[1], config->info.fb.addr[2], config->info.alpha_mode, config->info.alpha_value);

	mutex_lock(&mgr_mlock);
	{
		struct disp_layer_config *src = config;
		struct disp_layer_config *backup = &(backup_layer[mgr->disp][0]);
		backup_layer_num = layer_num;
		memset(backup, 0, sizeof(struct disp_layer_config)*16);
		for (layer_index = 0; layer_index < layer_num; layer_index++) {
			memcpy(backup, src, sizeof(struct disp_layer_config));
			backup++;
			src++;
		}

		if (_force_layer_en) {
			mutex_unlock(&mgr_mlock);
			return -1;
		}
	}

	for (layer_index=0; layer_index < layer_num; layer_index++) {
		struct disp_layer *lyr = NULL;

		lyr = disp_get_layer(mgr->disp, config->channel, config->layer_id);
		if (lyr != NULL)
			lyr->save_and_dirty_check(lyr, config);
		else
			DE_WRN("get layer(%d,%d,%d) fail\n", mgr->disp,
			       config->channel, config->layer_id);

		config++;
	}

	if (mgr->apply)
		mgr->apply(mgr);

	list_for_each_entry(lyr, &mgr->lyr_list, list) {
		lyr->dirty_clear(lyr);
	}
	mutex_unlock(&mgr_mlock);

	return DIS_SUCCESS;
}

static s32 disp_mgr_get_layer_config(struct disp_manager *mgr, struct disp_layer_config *config, unsigned int layer_num)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	struct disp_layer *lyr;
	unsigned int num_layers = 0, layer_index = 0;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	num_layers = bsp_disp_feat_get_num_layers(mgr->disp);
	if ((NULL == config) || (layer_num == 0) || (layer_num > num_layers)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	for (layer_index=0; layer_index < layer_num; layer_index++) {
		lyr = disp_get_layer(mgr->disp, config->channel, config->layer_id);
		if (lyr != NULL)
			lyr->get_config(lyr, config);
		else
			DE_WRN("get layer(%d,%d,%d) fail\n", mgr->disp,
			       config->channel, config->layer_id);

		config++;
	}

	return DIS_SUCCESS;
}

static void __disp_update_dmabuf_ref(struct disp_manager_private_data *mgrp)
{
	struct dmabuf_item *item;
	int i;

	for (i = 0; i < DMABUF_REF_SIZE; i++)
		mgrp->dmabuf_ref[i] = 0;

	list_for_each_entry(item, &mgrp->dmabuf_list, list) {
		if (mgrp->dmabuf_ref[0] < item->id) {
			for (i = DMABUF_REF_SIZE - 1; i > 0; i--)
				mgrp->dmabuf_ref[i] = mgrp->dmabuf_ref[i - 1];

			mgrp->dmabuf_ref[0] = item->id;
		}
	}
}

static void disp_mgr_dmabuf_list_add(struct dmabuf_item *item,
			struct disp_manager_private_data *mgrp,
			unsigned long long ref)
{
	item->id = ref;
	list_add_tail(&item->list, &mgrp->dmabuf_list);
	mgrp->dmabuf_cnt++;
}

static s32
disp_mgr_set_layer_config2(struct disp_manager *mgr,
			  struct disp_layer_config2 *config,
			  unsigned int layer_num)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	unsigned int num_layers = 0, i = 0;
	struct disp_layer *lyr = NULL;
	struct disp_layer_config_data *lyr_cfg;
	struct dmabuf_item *item, *tmp;
	struct disp_layer_config2 *config1 = config;
	unsigned long long ref = 0;
	unsigned int layers_using = 0, layers_max_using;
	struct fb_address_transfer fb;
	bool pre_force_sync = mgrp->force_sync;
	struct disp_device_dynamic_config dconf;
	struct disp_device *dispdev = NULL;
	unsigned int map_err_cnt = 0;

	if ((mgr == NULL) || (mgrp == NULL)) {
		DE_WRN("NULL hdl!\n");
		goto err;
	}
	DE_INF("mgr%d, config %d layers\n", mgr->disp, layer_num);

	num_layers = bsp_disp_feat_get_num_layers(mgr->disp);
	if ((config == NULL) || (layer_num == 0) || (layer_num > num_layers)) {
		DE_WRN("NULL hdl!\n");
		goto err;
	}

	memset(&dconf, 0, sizeof(struct disp_device_dynamic_config));
	mutex_lock(&mgr_mlock);
	for (i = 0; i < layer_num; i++) {
		struct disp_layer *lyr = NULL;

		lyr = disp_get_layer(mgr->disp, config1->channel,
				     config1->layer_id);

		if (lyr) {
			lyr->save_and_dirty_check2(lyr, config1);
			if (lyr->is_dirty(lyr) &&
			   (config1->info.fb.metadata_flag & 0x3)) {
				dconf.metadata_fd =
						config1->info.fb.metadata_fd;
				dconf.metadata_size =
						config1->info.fb.metadata_size;
				dconf.metadata_flag =
						config1->info.fb.metadata_flag;
			}
		}

		if (config1->enable == 1)
			layers_using++;
		config1++;
	}

	ref = gdisp.screen[mgr->disp].health_info.irq_cnt;

	lyr_cfg = disp_mgr_get_layer_cfg_head(mgr);
	for (i = 0; i < layer_num; i++, lyr_cfg++) {
		if (lyr_cfg->config.enable == 0)
			continue;

		if (lyr_cfg->config.info.mode == LAYER_MODE_COLOR)
			continue;

		item = disp_dma_map(lyr_cfg->config.info.fb.fd);
		if (item == NULL) {
			DE_WRN("disp dma map fail!\n");
			lyr_cfg->config.enable = 0;
			map_err_cnt++;
			continue;
		}
		fb.format = lyr_cfg->config.info.fb.format;
		memcpy(fb.size, lyr_cfg->config.info.fb.size,
		       sizeof(struct disp_rectsz) * 3);
		memcpy(fb.align, lyr_cfg->config.info.fb.align,
		       sizeof(int) * 3);
		fb.depth = lyr_cfg->config.info.fb.depth;
		fb.dma_addr = item->dma_addr;
		disp_set_fb_info(&fb, true);
		memcpy(lyr_cfg->config.info.fb.addr,
		       fb.addr,
		       sizeof(long long) * 3);
		lyr_cfg->config.info.fb.trd_right_addr[0] =
				(unsigned int)fb.trd_right_addr[0];
		lyr_cfg->config.info.fb.trd_right_addr[1] =
				(unsigned int)fb.trd_right_addr[1];
		lyr_cfg->config.info.fb.trd_right_addr[2] =
				(unsigned int)fb.trd_right_addr[2];
		disp_mgr_dmabuf_list_add(item, mgrp, ref);

		/* get dma_buf for right image buffer */
		if (lyr_cfg->config.info.fb.flags == DISP_BF_STEREO_FP) {
			item = disp_dma_map(lyr_cfg->config.info.atw.cof_fd);
			if (item == NULL) {
				DE_WRN("disp dma map for right buffer fail!\n");
				lyr_cfg->config.info.fb.flags = DISP_BF_NORMAL;
				continue;
			}
			fb.dma_addr = item->dma_addr;
			disp_set_fb_info(&fb, false);
			lyr_cfg->config.info.fb.trd_right_addr[0] =
					(unsigned int)fb.trd_right_addr[0];
			lyr_cfg->config.info.fb.trd_right_addr[1] =
					(unsigned int)fb.trd_right_addr[1];
			lyr_cfg->config.info.fb.trd_right_addr[2] =
					(unsigned int)fb.trd_right_addr[2];
			disp_mgr_dmabuf_list_add(item, mgrp, ref);
		}

		/* process 2d plus depth stereo mode */
		if (lyr_cfg->config.info.fb.flags == DISP_BF_STEREO_2D_DEPTH)  {
			lyr_cfg->config.info.fb.flags = DISP_BF_STEREO_FP;
			/* process depth, only support rgb format */
			if ((lyr_cfg->config.info.fb.depth != 0) &&
			    (lyr_cfg->config.info.fb.format
			      < DISP_FORMAT_YUV444_I_AYUV)) {
				int depth = lyr_cfg->config.info.fb.depth;
				unsigned long long abs_depth =
					(depth > 0) ? depth : (-depth);

				memcpy(fb.addr,
				       lyr_cfg->config.info.fb.addr,
				       sizeof(long long) * 3);
				fb.trd_right_addr[0] =
				    lyr_cfg->config.info.fb.trd_right_addr[0];
				fb.trd_right_addr[1] =
				    lyr_cfg->config.info.fb.trd_right_addr[1];
				fb.trd_right_addr[2] =
				    lyr_cfg->config.info.fb.trd_right_addr[2];
				if (disp_set_fb_base_on_depth(&fb) == 0) {
					memcpy(lyr_cfg->config.info.fb.addr,
					       fb.addr,
					       sizeof(long long) * 3);
					lyr_cfg->config.info.fb.trd_right_addr[0] =
					    (unsigned int)fb.trd_right_addr[0];
					lyr_cfg->config.info.fb.trd_right_addr[1] =
					    (unsigned int)fb.trd_right_addr[1];
					lyr_cfg->config.info.fb.trd_right_addr[2] =
					    (unsigned int)fb.trd_right_addr[2];

					lyr_cfg->config.info.fb.crop.width -=
					    (abs_depth << 32);
				}
			}

		}

		/* get dma_buf for atw coef buffer */
		if (!lyr_cfg->config.info.atw.used)
			continue;

		item = disp_dma_map(lyr_cfg->config.info.atw.cof_fd);
		if (item == NULL) {
			DE_WRN("disp dma map for atw coef fail!\n");
			lyr_cfg->config.info.atw.used = 0;
			continue;
		}
		lyr_cfg->config.info.atw.cof_addr = item->dma_addr;
		disp_mgr_dmabuf_list_add(item, mgrp, ref);


	}
	if (map_err_cnt == 0)
		__disp_update_dmabuf_ref(mgrp);

	mgrp->dmabuf_cnt_max = (mgrp->dmabuf_cnt > mgrp->dmabuf_cnt_max) ?
				mgrp->dmabuf_cnt : mgrp->dmabuf_cnt_max;

	if (mgr->apply)
		mgr->apply(mgr);

	list_for_each_entry(lyr, &mgr->lyr_list, list) {
		lyr->dirty_clear(lyr);
	}

	/* we will force sync the manager when continue nosync appear */
	mgrp->force_sync = false;
	if (!mgrp->sync) {
		mgrp->nosync_cnt++;
		if (mgrp->nosync_cnt >= FORCE_SYNC_THRESHOLD) {
			mgrp->nosync_cnt = 0;
			mgrp->force_sync_cnt++;
			mgrp->force_sync = true;
			mgr->sync(mgr, true);
		}
	} else {
		mgrp->nosync_cnt = 0;
	}

	layers_max_using = (layers_using > mgrp->layers_using) ?
			layers_using : mgrp->layers_using;
	mgrp->layers_using = layers_using;
	if (mgrp->dmabuf_cnt > (DMABUF_CACHE_MAX_EACH * layers_max_using * 2))
		goto exit_unmap;
	/*
	 * We skip unmap dma_buf when the manager is not sync
	 * or there is a error.
	 * But when the cache dma_buf number is overflow,
	 * we will force unmap it.
	 */
	if ((mgrp->dmabuf_cnt < (DMABUF_CACHE_MAX_EACH * layers_max_using)) &&
	    (!mgrp->sync || mgrp->err)) {
		mgrp->dmabuf_unmap_skip_cnt++;
		goto exit;
	}

	/* we will defer unmap until next 2 commit when force sync happens */
	if (pre_force_sync || mgrp->force_sync) {
		mgrp->dmabuf_unmap_skip_cnt++;
		goto exit;
	}

	if (mgrp->dmabuf_cnt >= (DMABUF_CACHE_MAX_EACH * layers_max_using))
		mgrp->dmabuf_overflow_cnt++;

exit_unmap:
	list_for_each_entry_safe(item, tmp, &mgrp->dmabuf_list, list) {
		if (item->id < mgrp->dmabuf_ref[DMABUF_REF_SIZE - 1]) {
			list_del(&item->list);
			disp_dma_unmap(item);
			mgrp->dmabuf_cnt--;
		}
	}

exit:
	mutex_unlock(&mgr_mlock);

	dispdev = mgr->device;
	if ((dconf.metadata_flag & 0x3) &&
		dispdev && dispdev->set_dynamic_config)
		dispdev->set_dynamic_config(dispdev, &dconf);

	return DIS_SUCCESS;
err:
	return -1;
}

static s32
disp_mgr_get_layer_config2(struct disp_manager *mgr,
			  struct disp_layer_config2 *config,
			  unsigned int layer_num)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	struct disp_layer *lyr;
	unsigned int num_layers = 0, layer_index = 0;

	if ((mgr == NULL) || (mgrp == NULL)) {
		DE_WRN("NULL hdl!\n");
		goto err;
	}

	num_layers = bsp_disp_feat_get_num_layers(mgr->disp);
	if ((config == NULL) || (layer_num == 0) || (layer_num > num_layers)) {
		DE_WRN("NULL hdl!\n");
		goto err;
	}
	for (layer_index = 0; layer_index < layer_num; layer_index++) {
		lyr = disp_get_layer(mgr->disp, config->channel,
				     config->layer_id);
		if (lyr != NULL)
			lyr->get_config2(lyr, config);
		else
			DE_WRN("get layer(%d,%d,%d) fail\n", mgr->disp,
			       config->channel, config->layer_id);
		config++;
	}

	return DIS_SUCCESS;

err:
	return -1;
}

static s32 disp_mgr_sync(struct disp_manager *mgr, bool sync)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = NULL;
	struct disp_enhance *enhance = NULL;
	struct disp_smbl *smbl = NULL;
	struct disp_capture *cptr = NULL;
	struct disp_device *dispdev;

	if (NULL == mgr) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	mgrp = disp_mgr_get_priv(mgr);
	if (NULL == mgrp) {
		DE_WRN("get mgr %d's priv fail!!\n", mgr->disp);
		return -1;
	}

	mgrp->sync = sync;
	if (!mgrp->sync)
		return 0;

	enhance = mgr->enhance;
	smbl = mgr->smbl;
	cptr = mgr->cptr;
	dispdev = mgr->device;

	spin_lock_irqsave(&mgr_data_lock, flags);
	if (!mgrp->enabled) {
		spin_unlock_irqrestore(&mgr_data_lock, flags);
		return -1;
	}
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	//mgr->update_regs(mgr);
	disp_al_manager_sync(mgr->disp);
	mgr->update_regs(mgr);
	if (dispdev && dispdev->is_in_safe_period) {
		if (!dispdev->is_in_safe_period(dispdev)) {
			mgrp->err = true;
			mgrp->err_cnt++;
		} else {
			mgrp->err = false;
		}
	}
	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->applied = false;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	/* enhance */
	if (enhance && enhance->sync)
		enhance->sync(enhance);

	/* smbl */
	if (smbl && smbl->sync)
		smbl->sync(smbl);

	/* capture */
	if (cptr && cptr->sync)
		cptr->sync(cptr);

	return DIS_SUCCESS;
}

static s32 disp_mgr_tasklet(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = NULL;
	struct disp_enhance *enhance = NULL;
	struct disp_smbl *smbl = NULL;
	struct disp_capture *cptr = NULL;

	if (NULL == mgr) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	mgrp = disp_mgr_get_priv(mgr);
	if (NULL == mgrp) {
		DE_WRN("get mgr %d's priv fail!!\n", mgr->disp);
		return -1;
	}
	enhance = mgr->enhance;
	smbl = mgr->smbl;
	cptr = mgr->cptr;

	if (!mgrp->enabled)
		return -1;

	/* enhance */
	if (enhance && enhance->tasklet)
		enhance->tasklet(enhance);

	/* smbl */
	if (smbl && smbl->tasklet)
		smbl->tasklet(smbl);

	/* capture */
	if (cptr && cptr->tasklet)
		cptr->tasklet(cptr);

	return DIS_SUCCESS;
}

static s32 disp_mgr_update_regs(struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	//__inf("disp_mgr_update_regs, mgr%d\n", mgr->disp);

#if 0
	spin_lock_irqsave(&mgr_data_lock, flags);
	if (!mgrp->enabled) {
		spin_unlock_irqrestore(&mgr_data_lock, flags);
	   return -1;
	}
	spin_unlock_irqrestore(&mgr_data_lock, flags);
#endif

	//disp_mgr_shadow_protect(mgr, true);
	//FIXME update_regs, other module may need to sync while manager don't
	//if (true == mgrp->applied)
		disp_al_manager_update_regs(mgr->disp);
	//disp_mgr_shadow_protect(mgr, false);

	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->applied = false;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_mgr_apply(struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	struct disp_manager_data data;
	bool mgr_dirty = false;
	bool lyr_drity = false;
	struct disp_layer *lyr = NULL;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	DE_INF("mgr %d apply\n", mgr->disp);

	spin_lock_irqsave(&mgr_data_lock, flags);
	if (((mgrp->enabled) && (mgrp->cfg->flag & MANAGER_ALL_DIRTY))
	    || (mgrp->cfg->flag & MANAGER_ENABLE_DIRTY))
	{
		mgr_dirty = true;
		memcpy(&data, mgrp->cfg, sizeof(struct disp_manager_data));
		mgrp->cfg->flag &= ~MANAGER_ALL_DIRTY;
	}

	list_for_each_entry(lyr, &mgr->lyr_list, list)
	{
		if (lyr->is_dirty && lyr->is_dirty(lyr)) {
			lyr_drity = true;
			break;
		}
	}

	mgrp->applied = true;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	disp_mgr_shadow_protect(mgr, true);
	if (mgr_dirty)
		disp_al_manager_apply(mgr->disp, &data);

	if (lyr_drity &&
	    (mgrp->enabled || (data.flag & MANAGER_ENABLE_DIRTY))) {
		u32 num_layers = bsp_disp_feat_get_num_layers(mgr->disp);
		struct disp_layer_config_data *lyr_cfg = disp_mgr_get_layer_cfg_head(mgr);

		disp_al_layer_apply(mgr->disp, lyr_cfg, num_layers);
	}
	disp_mgr_shadow_protect(mgr, false);

	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->applied = true;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_mgr_force_apply(struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	struct disp_layer* lyr = NULL;
	struct disp_enhance *enhance = NULL;
	struct disp_smbl *smbl = NULL;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	enhance = mgr->enhance;
	smbl = mgr->smbl;

	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->cfg->flag |= MANAGER_ALL_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);
	list_for_each_entry(lyr, &mgr->lyr_list, list) {
		lyr->force_apply(lyr);
	}

	disp_mgr_apply(mgr);
	disp_mgr_sync(mgr, true);

	/* enhance */
	if (enhance && enhance->force_apply)
		enhance->force_apply(enhance);

	/* smbl */
	if (smbl && smbl->force_apply)
		smbl->force_apply(smbl);

	return 0;
}

static s32 disp_mgr_enable(struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	unsigned int width = 0, height = 0;
	unsigned int cs = 0, color_range = DISP_COLOR_RANGE_0_255;
	int ret;
	struct disp_device_config dev_config;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	DE_INF("mgr %d enable\n", mgr->disp);

	//disp_sys_register_irq(mgrp->irq_no,0,disp_mgr_event_proc,(void*)mgr->disp,0,0);
	//disp_sys_enable_irq(mgrp->irq_no);

	dev_config.bits = DISP_DATA_8BITS;
	dev_config.eotf = DISP_EOTF_GAMMA22;
	dev_config.cs = DISP_BT601_F;
	dev_config.dvi_hdmi = DISP_HDMI;
	dev_config.range = DISP_COLOR_RANGE_16_235;
	dev_config.scan = DISP_SCANINFO_NO_DATA;
	dev_config.aspect_ratio = 8;

	ret = disp_mgr_clk_enable(mgr);
	if (0 != ret)
		return ret;

	disp_al_manager_init(mgr->disp);

	if (mgr->device) {
		if (mgr->device->get_resolution)
			mgr->device->get_resolution(mgr->device, &width, &height);
		if (mgr->device->get_static_config)
			mgr->device->get_static_config(mgr->device,
						       &dev_config);
		if (mgr->device && mgr->device->get_input_color_range)
			color_range = mgr->device->get_input_color_range(mgr->device);
		mgrp->cfg->config.disp_device = mgr->device->disp;
		mgrp->cfg->config.hwdev_index = mgr->device->hwdev_index;
		if (mgr->device && mgr->device->is_interlace)
			mgrp->cfg->config.interlace = mgr->device->is_interlace(mgr->device);
		else
			mgrp->cfg->config.interlace = 0;
	}

	DE_INF("output res: %d x %d, cs=%d, range=%d, interlace=%d\n",
		width, height, cs, color_range,mgrp->cfg->config.interlace);

	mutex_lock(&mgr_mlock);
	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->enabled = 1;
	mgrp->cfg->config.enable = 1;
	mgrp->cfg->flag |= MANAGER_ENABLE_DIRTY;

	mgrp->cfg->config.size.width = width;
	mgrp->cfg->config.size.height = height;
	mgrp->cfg->config.cs = dev_config.format;
	mgrp->cfg->config.color_space = dev_config.cs;
	mgrp->cfg->config.eotf = dev_config.eotf;
	mgrp->cfg->config.data_bits = dev_config.bits;
	if (!mgrp->color_range_modified)
		mgrp->cfg->config.color_range = color_range;
	mgrp->cfg->flag |= MANAGER_SIZE_DIRTY;
	mgrp->cfg->flag |= MANAGER_COLOR_SPACE_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	disp_mgr_force_apply(mgr);

	if (mgr->enhance && mgr->enhance->enable)
		mgr->enhance->enable(mgr->enhance);
	mutex_unlock(&mgr_mlock);

	return 0;
}

static s32 disp_mgr_sw_enable(struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	unsigned int width = 0, height = 0;
	unsigned int cs = 0, color_range = DISP_COLOR_RANGE_0_255;
	struct disp_device_config dev_config;
	struct disp_device *dispdev;
	unsigned long curtime;
	unsigned int curline0, curline1;
	struct disp_enhance *enhance = NULL;
	struct disp_smbl *smbl = NULL;
	struct disp_layer *lyr = NULL;
	unsigned long cnt = 0;

	memset(&dev_config, 0, sizeof(struct disp_device_config));

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	DE_INF("mgr %d enable\n", mgr->disp);

	dev_config.bits = DISP_DATA_8BITS;
	dev_config.eotf = DISP_EOTF_GAMMA22;
	dev_config.cs = DISP_BT601_F;
#if !defined(CONFIG_COMMON_CLK_ENABLE_SYNCBOOT)
	if (0 != disp_mgr_clk_enable(mgr))
		return -1;
#endif

	if (mgr->device) {
		if (mgr->device->get_resolution)
			mgr->device->get_resolution(mgr->device, &width, &height);
		if (mgr->device->get_static_config)
			mgr->device->get_static_config(mgr->device,
						       &dev_config);
		if (mgr->device->get_input_csc)
			cs = mgr->device->get_input_csc(mgr->device);
		if (mgr->device && mgr->device->get_input_color_range)
			color_range = mgr->device->get_input_color_range(mgr->device);
		mgrp->cfg->config.disp_device = mgr->device->disp;
		mgrp->cfg->config.hwdev_index = mgr->device->hwdev_index;
		if (mgr->device && mgr->device->is_interlace)
			mgrp->cfg->config.interlace = mgr->device->is_interlace(mgr->device);
		else
			mgrp->cfg->config.interlace = 0;
	}
	DE_INF("output res: %d x %d, cs=%d, range=%d, interlace=%d\n",
		width, height, cs, color_range,mgrp->cfg->config.interlace);

	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->enabled = 1;
	mgrp->cfg->config.enable = 1;

	mgrp->cfg->config.size.width = width;
	mgrp->cfg->config.size.height = height;
	mgrp->cfg->config.cs = dev_config.format;
	mgrp->cfg->config.color_space = dev_config.cs;
	mgrp->cfg->config.eotf = dev_config.eotf;
	mgrp->cfg->config.data_bits = dev_config.bits;
	if (!mgrp->color_range_modified)
		mgrp->cfg->config.color_range = color_range;
	mgrp->cfg->flag |= MANAGER_ALL_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);
	list_for_each_entry(lyr, &mgr->lyr_list, list) {
		lyr->force_apply(lyr);
	}

	disp_mgr_apply(mgr);
	disp_mgr_update_regs(mgr);

	/* wait for vertical blank period */
	dispdev = mgr->device;
	if (dispdev && dispdev->usec_before_vblank) {
		curtime = jiffies;
		while (dispdev->usec_before_vblank(dispdev) != 0) {
			if (time_after(jiffies,
					curtime +  msecs_to_jiffies(50)))
				break;
			cnt++;
			if (cnt >= 1000 * 1000)
				break;
		}
	}
	curline0 = disp_al_device_get_cur_line(dispdev->hwdev_index);
	disp_mgr_sync(mgr, true);
#if defined(CONFIG_SUNXI_IOMMU)
	sunxi_enable_device_iommu(DE_MASTOR_ID, true);
#endif
	curline1 = disp_al_device_get_cur_line(dispdev->hwdev_index);
	if (dispdev && dispdev->is_in_safe_period) {
		if (!dispdev->is_in_safe_period(dispdev)) {
			DE_WRN("sync at non-safe period,start=%d,end=%d line\n",
				curline0, curline1);
		}
	}

	enhance = mgr->enhance;
	smbl = mgr->smbl;

	/* enhance */
	if (enhance && enhance->force_apply)
		enhance->force_apply(enhance);

	/* smbl */
	if (smbl && smbl->force_apply)
		smbl->force_apply(smbl);

	if (mgr->enhance && mgr->enhance->enable)
		mgr->enhance->enable(mgr->enhance);

	return 0;
}

static s32 disp_mgr_disable(struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	DE_INF("mgr %d disable\n", mgr->disp);

	mutex_lock(&mgr_mlock);
	if (mgr->enhance && mgr->enhance->disable)
		mgr->enhance->disable(mgr->enhance);

	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->enabled = 0;
	mgrp->cfg->flag |= MANAGER_ENABLE_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);
	disp_mgr_force_apply(mgr);
	disp_delay_ms(5);

	disp_al_manager_exit(mgr->disp);
	disp_mgr_clk_disable(mgr);
	mutex_unlock(&mgr_mlock);

	//disp_sys_disable_irq(mgrp->irq_no);
	//disp_sys_unregister_irq(mgrp->irq_no, disp_mgr_event_proc,(void*)mgr->disp);

	return 0;
}

static s32 disp_mgr_is_enabled(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	return mgrp->enabled;

}

static s32 disp_mgr_dump(struct disp_manager *mgr, char *buf)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	unsigned int count = 0;
	char const *fmt_name[] = {
		"rgb",
		"yuv444",
		"yuv422",
		"yuv420"
	};
	char const *bits_name[] = {
		"8bits",
		"10bits",
		"12bits",
		"16bits"
	};

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	count += sprintf(buf + count,
	    "mgr%d: %dx%d fmt[%s] cs[0x%x] range[%s] eotf[0x%x] bits[%s] %s err[%d] force_sync[%d]\n",
	    mgr->disp,
	    mgrp->cfg->config.size.width, mgrp->cfg->config.size.height,
	    (mgrp->cfg->config.cs < 4) ?
		fmt_name[mgrp->cfg->config.cs] : "undef",
	    mgrp->cfg->config.color_space,
	    (mgrp->cfg->config.color_range == DISP_COLOR_RANGE_0_255) ?
		"full" : "limit",
	    mgrp->cfg->config.eotf,
	    (mgrp->cfg->config.data_bits < 4) ?
		bits_name[mgrp->cfg->config.data_bits] : "undef",
	    (mgrp->cfg->config.blank) ? "blank" : "unblank", mgrp->err_cnt,
	    mgrp->force_sync_cnt);
	count += sprintf(buf + count,
	    "dmabuf: cache[%d] cache max[%d] umap skip[%d] overflow[%d]\n",
	    mgrp->dmabuf_cnt, mgrp->dmabuf_cnt_max, mgrp->dmabuf_unmap_skip_cnt,
	    mgrp->dmabuf_overflow_cnt);

	return count;
}

static s32 disp_mgr_blank(struct disp_manager *mgr, bool blank)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	struct disp_layer* lyr = NULL;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	mutex_lock(&mgr_mlock);
	list_for_each_entry(lyr, &mgr->lyr_list, list) {
		lyr->force_apply(lyr);
	}
	mgrp->cfg->config.blank = blank;
	mgrp->cfg->flag |= MANAGER_BLANK_DIRTY;
	mgr->apply(mgr);
	mutex_unlock(&mgr_mlock);


	return 0;
}

s32 disp_init_mgr(disp_bsp_init_para * para)
{
	u32 num_screens;
	u32 disp;
	struct disp_manager *mgr;
	struct disp_manager_private_data *mgrp;

	DE_INF("%s\n", __func__);

#if defined(__LINUX_PLAT__)
	spin_lock_init(&mgr_data_lock);
	mutex_init(&mgr_mlock);
#endif
	num_screens = bsp_disp_feat_get_num_screens();
	mgrs = (struct disp_manager *)kmalloc(sizeof(struct disp_manager) * num_screens, GFP_KERNEL | __GFP_ZERO);
	if (NULL == mgrs) {
		DE_WRN("malloc memory fail!\n");
		return DIS_FAIL;
	}
	mgr_private = (struct disp_manager_private_data *)kmalloc(sizeof(struct disp_manager_private_data)\
	* num_screens, GFP_KERNEL | __GFP_ZERO);
	if (NULL == mgr_private) {
		DE_WRN("malloc memory fail! size=0x%x x %d\n", (unsigned int)sizeof(struct disp_manager_private_data), num_screens);
		return DIS_FAIL;
	}
	mgr_cfgs = (struct disp_manager_data *)kmalloc(sizeof(struct disp_manager_data)\
		* num_screens, GFP_KERNEL | __GFP_ZERO);
	if (NULL == mgr_private) {
		DE_WRN("malloc memory fail! size=0x%x x %d\n",
			(unsigned int)sizeof(struct disp_manager_private_data), num_screens);
		return DIS_FAIL;
	}

	for (disp=0; disp<num_screens; disp++) {
		mgr = &mgrs[disp];
		mgrp = &mgr_private[disp];

		DE_INF("mgr %d, 0x%p\n", disp, mgr);

		sprintf(mgr->name, "mgr%d", disp);
		mgr->disp = disp;
		mgrp->cfg = &mgr_cfgs[disp];
		mgrp->irq_no = para->irq_no[DISP_MOD_DE];
		mgrp->shadow_protect = para->shadow_protect;
		mgrp->clk = para->mclk[DISP_MOD_DE];
#if defined(HAVE_DEVICE_COMMON_MODULE)
		mgrp->extra_clk = para->mclk[DISP_MOD_DEVICE];
#endif

		mgr->enable = disp_mgr_enable;
		mgr->sw_enable = disp_mgr_sw_enable;
		mgr->disable = disp_mgr_disable;
		mgr->is_enabled = disp_mgr_is_enabled;
		mgr->set_color_key = disp_mgr_set_color_key;
		mgr->get_color_key = disp_mgr_get_color_key;
		mgr->set_back_color = disp_mgr_set_back_color;
		mgr->get_back_color = disp_mgr_get_back_color;
		mgr->set_layer_config = disp_mgr_set_layer_config;
		mgr->force_set_layer_config = disp_mgr_force_set_layer_config;
		mgr->force_set_layer_config_exit = disp_mgr_force_set_layer_config_exit;
		mgr->get_layer_config = disp_mgr_get_layer_config;
		mgr->set_layer_config2 = disp_mgr_set_layer_config2;
		mgr->get_layer_config2 = disp_mgr_get_layer_config2;
		mgr->set_output_color_range = disp_mgr_set_output_color_range;
		mgr->get_output_color_range = disp_mgr_get_output_color_range;
		mgr->update_color_space = disp_mgr_update_color_space;
		mgr->smooth_switch = disp_mgr_smooth_switch;
		mgr->dump = disp_mgr_dump;
		mgr->blank = disp_mgr_blank;
		mgr->get_clk_rate = disp_mgr_get_clk_rate;

		mgr->init = disp_mgr_init;
		mgr->exit = disp_mgr_exit;

		mgr->apply = disp_mgr_apply;
		mgr->update_regs = disp_mgr_update_regs;
		mgr->force_apply = disp_mgr_force_apply;
		mgr->sync = disp_mgr_sync;
		mgr->tasklet = disp_mgr_tasklet;

		INIT_LIST_HEAD(&mgr->lyr_list);
		INIT_LIST_HEAD(&mgrp->dmabuf_list);

		mgr->init(mgr);
	}

	disp_init_lyr(para);
	return 0;
}
