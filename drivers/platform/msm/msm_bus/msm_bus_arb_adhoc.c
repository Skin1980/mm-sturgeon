/* Copyright (c) 2014, The Linux Foundation. All rights reserved.
 *
 * This program is Mree software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/clk.h>
#include <linux/msm-bus.h>
#include "msm_bus_core.h"
#include "msm_bus_adhoc.h"

#define NUM_CL_HANDLES	50
#define NUM_LNODES	3

struct bus_search_type {
	struct list_head link;
	struct list_head node_list;
};

struct handle_type {
	int num_entries;
	struct msm_bus_client **cl_list;
};

static struct handle_type handle_list;
struct list_head input_list;
struct list_head apply_list;

DEFINE_MUTEX(msm_bus_adhoc_lock);

/*
 * Duplicate instantiaion from msm_bus_arb.c. Todo there needs to be a
 * "util" file for these common func/macros.
 *
 * */
uint64_t msm_bus_div64(unsigned int w, uint64_t bw)
{
	uint64_t *b = &bw;

	if ((bw > 0) && (bw < w))
		return 1;

	switch (w) {
	case 0:
		WARN(1, "AXI: Divide by 0 attempted\n");
	case 1: return bw;
	case 2: return (bw >> 1);
	case 4: return (bw >> 2);
	case 8: return (bw >> 3);
	case 16: return (bw >> 4);
	case 32: return (bw >> 5);
	}

	do_div(*b, w);
	return *b;
}

int msm_bus_device_match_adhoc(struct device *dev, void *id)
{
	int ret = 0;
	struct msm_bus_node_device_type *bnode = dev->platform_data;

	if (bnode)
		ret = (bnode->node_info->id == *(unsigned int *)id);
	else
		ret = 0;

	return ret;
}

static int gen_lnode(struct device *dev,
			int next_hop, int prev_idx)
{
	struct link_node *lnode;
	struct msm_bus_node_device_type *cur_dev = NULL;
	int lnode_idx = -1;

	cur_dev = dev->platform_data;
	if (!cur_dev) {
		MSM_BUS_ERR("%s: Null device ptr", __func__);
		goto exit_gen_lnode;
	}

	if (!cur_dev->num_lnodes) {
		cur_dev->lnode_list = devm_kzalloc(dev,
				sizeof(struct link_node) * NUM_LNODES,
								GFP_KERNEL);
		lnode = cur_dev->lnode_list;
		cur_dev->num_lnodes = NUM_LNODES;
		lnode_idx = 0;
	} else {
		int i;
		for (i = 0; i < cur_dev->num_lnodes; i++) {
			if (!cur_dev->lnode_list[i].in_use)
				break;
		}

		if (i < cur_dev->num_lnodes) {
			lnode = &cur_dev->lnode_list[i];
			lnode_idx = i;
		} else {
			struct link_node *realloc_list;
			size_t cur_size = sizeof(struct link_node) *
					cur_dev->num_lnodes;

			cur_dev->num_lnodes += NUM_LNODES;
			realloc_list = msm_bus_realloc_devmem(
					dev,
					cur_dev->lnode_list,
					cur_size,
					sizeof(struct link_node) *
					cur_dev->num_lnodes, GFP_KERNEL);

			if (!realloc_list)
				goto exit_gen_lnode;

			cur_dev->lnode_list = realloc_list;
			lnode = &cur_dev->lnode_list[i];
			lnode_idx = i;
		}
	}

	lnode->in_use = 1;
	if (next_hop == cur_dev->node_info->id) {
		lnode->next = -1;
		lnode->next_dev = NULL;
	} else {
		lnode->next = prev_idx;
		lnode->next_dev = bus_find_device(&msm_bus_type, NULL,
					(void *) &next_hop,
					msm_bus_device_match_adhoc);
	}

	memset(lnode->lnode_ib, 0, sizeof(uint64_t) * NUM_CTX);
	memset(lnode->lnode_ab, 0, sizeof(uint64_t) * NUM_CTX);

exit_gen_lnode:
	return lnode_idx;
}

static int remove_lnode(struct msm_bus_node_device_type *cur_dev,
				int lnode_idx)
{
	int ret = 0;

	if (!cur_dev) {
		MSM_BUS_ERR("%s: Null device ptr", __func__);
		ret = -ENODEV;
		goto exit_remove_lnode;
	}

	if (lnode_idx != -1) {
		if (!cur_dev->num_lnodes ||
				(lnode_idx > (cur_dev->num_lnodes - 1))) {
			MSM_BUS_ERR("%s: Invalid Idx %d, num_lnodes %d",
				__func__, lnode_idx, cur_dev->num_lnodes);
			ret = -ENODEV;
			goto exit_remove_lnode;
		}

		cur_dev->lnode_list[lnode_idx].next = -1;
		cur_dev->lnode_list[lnode_idx].next_dev = NULL;
		cur_dev->lnode_list[lnode_idx].in_use = 0;
	}

exit_remove_lnode:
	return ret;
}

static int prune_path(struct list_head *route_list, int dest, int src)
{
	struct bus_search_type *search_node;
	struct msm_bus_node_device_type *bus_node;
	int search_dev_id = dest;
	struct device *dest_dev = bus_find_device(&msm_bus_type, NULL,
					(void *) &dest,
					msm_bus_device_match_adhoc);
	int lnode_hop = -1;

	if (!dest_dev) {
		MSM_BUS_ERR("%s: Can't find dest dev %d", __func__, dest);
		goto exit_prune_path;
	}

	lnode_hop = gen_lnode(dest_dev, search_dev_id, lnode_hop);
	list_for_each_entry_reverse(search_node, route_list, link) {
		list_for_each_entry(bus_node, &search_node->node_list, link) {
			unsigned int i;
			for (i = 0; i < bus_node->node_info->num_connections;
									i++) {
				if (bus_node->node_info->connections[i] ==
								search_dev_id) {
						dest_dev = bus_find_device(
							&msm_bus_type,
							NULL,
							(void *)
							&bus_node->node_info->
								id,
						msm_bus_device_match_adhoc);
						lnode_hop = gen_lnode(dest_dev,
								search_dev_id,
								lnode_hop);
						search_dev_id =
							bus_node->node_info->id;
						break;
				}
			}
		}
	}

	list_for_each_entry_reverse(search_node, route_list, link) {
		if (search_node->link.next != route_list) {
			struct bus_search_type *del_node;
			struct list_head *del_link;

			del_link = search_node->link.next;
			del_node = list_entry(del_link,
					struct bus_search_type, link);
			list_del(del_link);
			kfree(del_node);
		}
	}
	search_node = list_entry(route_list->next,
				struct bus_search_type , link);
	kfree(search_node);


exit_prune_path:
	return lnode_hop;
}

static int getpath(int src, int dest)
{
	struct list_head traverse_list;
	struct list_head edge_list;
	struct list_head route_list;
	struct device *src_dev = bus_find_device(&msm_bus_type, NULL,
					(void *) &src,
					msm_bus_device_match_adhoc);
	struct msm_bus_node_device_type *src_node;
	struct bus_search_type *search_node;
	int found = 0;
	int depth_index = 0;
	int first_hop = -1;

	INIT_LIST_HEAD(&traverse_list);
	INIT_LIST_HEAD(&edge_list);
	INIT_LIST_HEAD(&route_list);

	if (!src_dev) {
		MSM_BUS_ERR("%s: Cannot locate src dev %d", __func__, src);
		goto exit_getpath;
	}

	src_node = src_dev->platform_data;
	if (!src_node) {
		MSM_BUS_ERR("%s:Fatal, Source dev %d not found", __func__, src);
		goto exit_getpath;
	}

	list_add_tail(&src_node->link, &traverse_list);

	while ((!found && !list_empty(&traverse_list))) {
		struct msm_bus_node_device_type *bus_node = NULL;
		/* Locate dest_id in the traverse list */
		list_for_each_entry(bus_node, &traverse_list, link) {
			if (bus_node->node_info->id == dest) {
				MSM_BUS_DBG("%s: Dest found", __func__);
				found = 1;
				break;
			}
		}

		if (!found) {
			/* Setup the new edge list */
			list_for_each_entry(bus_node, &traverse_list, link) {
				unsigned int i;
				for (i = 0;
				i < bus_node->node_info->num_connections; i++) {
					struct msm_bus_node_device_type
								*node_conn;

					node_conn =
					bus_node->node_info->dev_connections[i]
								->platform_data;
					list_add_tail(&node_conn->link,
								&edge_list);
				}
			}

			/* Keep tabs of the previous search list */
			search_node = kzalloc(sizeof(struct bus_search_type),
								GFP_KERNEL);
			INIT_LIST_HEAD(&search_node->node_list);
			list_splice_init(&traverse_list,
						&search_node->node_list);
			/* Add the previous search list to a route list */
			list_add_tail(&search_node->link, &route_list);
			/* Advancing the list depth */
			depth_index++;
			list_splice_init(&edge_list, &traverse_list);
		}
	}

	if (found)
		first_hop = prune_path(&route_list, dest, src);

exit_getpath:
	return first_hop;
}

static uint64_t arbitrate_bus_req(struct msm_bus_node_device_type *bus_dev,
								int ctx)
{
	int i;
	uint64_t max_ib = 0;
	uint64_t sum_ab = 0;
	uint64_t bw_max_hz;
	struct msm_bus_node_device_type *fab_dev = NULL;

	/* Find max ib */
	for (i = 0; i < bus_dev->num_lnodes; i++) {
		max_ib = max(max_ib, bus_dev->lnode_list[i].lnode_ib[ctx]);
		sum_ab += bus_dev->lnode_list[i].lnode_ab[ctx];
	}

	/*
	 *  Account for Util factor and vrail comp. The new aggregation
	 *  formula is:
	 *  Freq_hz = max((sum(ab) * util_fact)/num_chan, max(ib)/vrail_comp)
	 *				/ bus-width
	 *  util_fact and vrail comp are obtained from fabric's dts properties.
	 *  They default to 100 if absent.
	 */
	fab_dev = bus_dev->node_info->bus_device->platform_data;

	/* Don't do this for virtual fabrics */
	if (fab_dev && fab_dev->fabdev) {
		sum_ab *= fab_dev->fabdev->util_fact;
		sum_ab = msm_bus_div64(100, sum_ab);
		max_ib *= 100;
		max_ib = msm_bus_div64(fab_dev->fabdev->vrail_comp, max_ib);
	}

	/* Account for multiple channels if any */
	if (bus_dev->node_info->num_qports > 1)
		sum_ab = msm_bus_div64(bus_dev->node_info->num_qports,
					sum_ab);

	if (!bus_dev->node_info->buswidth) {
		MSM_BUS_WARN("No bus width found for %d. Using default\n",
					bus_dev->node_info->id);
		bus_dev->node_info->buswidth = 8;
	}

	bw_max_hz = max(max_ib, sum_ab);
	bw_max_hz = msm_bus_div64(bus_dev->node_info->buswidth,
					bw_max_hz);

	return bw_max_hz;
}

static void del_inp_list(struct list_head *list)
{
	struct rule_update_path_info *rule_node;
	struct rule_update_path_info *rule_node_tmp;

	list_for_each_entry_safe(rule_node, rule_node_tmp, list, link)
		list_del(&rule_node->link);
}

static void del_op_list(struct list_head *list)
{
	struct rule_apply_rcm_info *rule;
	struct rule_apply_rcm_info *rule_tmp;

	list_for_each_entry_safe(rule, rule_tmp, list, link)
		list_del(&rule->link);
}

static int msm_bus_apply_rules(struct list_head *list, bool after_clk_commit)
{
	struct rule_apply_rcm_info *rule;
	struct device *dev = NULL;
	struct msm_bus_node_device_type *dev_info = NULL;
	int ret = 0;
	bool throttle_en = false;

	list_for_each_entry(rule, list, link) {
		if (rule && (rule->after_clk_commit != after_clk_commit))
			continue;

		dev = bus_find_device(&msm_bus_type, NULL,
				(void *) &rule->id,
				msm_bus_device_match_adhoc);

		if (!dev) {
			MSM_BUS_ERR("Can't find dev node for %d", rule->id);
			continue;
		}
		dev_info = dev->platform_data;

		throttle_en = ((rule->throttle == THROTTLE_ON) ? true : false);
		ret = msm_bus_enable_limiter(dev_info, throttle_en,
							rule->lim_bw);
		if (ret)
			MSM_BUS_ERR("Failed to set limiter for %d", rule->id);
	}

	return ret;
}

static uint64_t get_node_ib(struct msm_bus_node_device_type *bus_dev)
{
	int i;
	int ctx;
	uint64_t max_ib = 0;

	for (ctx = 0; ctx < NUM_CTX; ctx++) {
		for (i = 0; i < bus_dev->num_lnodes; i++)
			max_ib = max(max_ib,
				bus_dev->lnode_list[i].lnode_ib[ctx]);
	}
	return max_ib;
}

static int update_path(int src, int dest, uint64_t req_ib, uint64_t req_bw,
			uint64_t cur_ib, uint64_t cur_bw, int src_idx, int ctx)
{
	struct device *src_dev = NULL;
	struct device *next_dev = NULL;
	struct link_node *lnode = NULL;
	struct msm_bus_node_device_type *dev_info = NULL;
	int curr_idx;
	int ret = 0;
	int *dirty_nodes = NULL;
	int num_dirty = 0;
	struct rule_update_path_info *rule_node;
	bool rules_registered = msm_rule_are_rules_registered();

	src_dev = bus_find_device(&msm_bus_type, NULL,
				(void *) &src,
				msm_bus_device_match_adhoc);

	if (!src_dev) {
		MSM_BUS_ERR("%s: Can't find source device %d", __func__, src);
		ret = -ENODEV;
		goto exit_update_path;
	}

	next_dev = src_dev;

	if (src_idx < 0) {
		MSM_BUS_ERR("%s: Invalid lnode idx %d", __func__, src_idx);
		ret = -ENXIO;
		goto exit_update_path;
	}
	curr_idx = src_idx;

	INIT_LIST_HEAD(&input_list);
	INIT_LIST_HEAD(&apply_list);

	while (next_dev) {
		dev_info = next_dev->platform_data;

		if (curr_idx >= dev_info->num_lnodes) {
			MSM_BUS_ERR("%s: Invalid lnode Idx %d num lnodes %d",
			 __func__, curr_idx, dev_info->num_lnodes);
			ret = -ENXIO;
			goto exit_update_path;
		}

		lnode = &dev_info->lnode_list[curr_idx];
		lnode->lnode_ib[ctx] = req_ib;
		lnode->lnode_ab[ctx] = req_bw;

		dev_info->cur_clk_hz[ctx] = arbitrate_bus_req(dev_info, ctx);

		/* Start updating the clocks at the first hop.
		 * Its ok to figure out the aggregated
		 * request at this node.
		 */
		if (src_dev != next_dev) {
			ret = msm_bus_update_clks(dev_info, ctx, &dirty_nodes,
								&num_dirty);
			if (ret) {
				MSM_BUS_ERR("%s: Failed to update clks dev %d",
					__func__, dev_info->node_info->id);
				goto exit_update_path;
			}
		}

		ret = msm_bus_update_bw(dev_info, ctx, req_bw, &dirty_nodes,
								&num_dirty);
		if (ret) {
			MSM_BUS_ERR("%s: Failed to update bw dev %d",
				__func__, dev_info->node_info->id);
			goto exit_update_path;
		}

		if (rules_registered) {
			rule_node = &dev_info->node_info->rule;
			rule_node->id = dev_info->node_info->id;
			rule_node->ib =  get_node_ib(dev_info);
			rule_node->ab = dev_info->node_ab.ab[ACTIVE_CTX];
			list_add_tail(&rule_node->link, &input_list);
		}

		next_dev = lnode->next_dev;
		curr_idx = lnode->next;
	}

	if (rules_registered) {
		msm_rules_update_path(&input_list, &apply_list);
		msm_bus_apply_rules(&apply_list, false);
	}

	msm_bus_commit_data(dirty_nodes, ctx, num_dirty);

	if (rules_registered) {
		msm_bus_apply_rules(&apply_list, true);
		del_inp_list(&input_list);
		del_op_list(&apply_list);
	}
exit_update_path:
	return ret;
}

static int remove_path(int src, int dst, uint64_t cur_ib, uint64_t cur_ab,
				int src_idx, int active_only)
{
	struct device *src_dev = NULL;
	struct device *next_dev = NULL;
	struct link_node *lnode = NULL;
	struct msm_bus_node_device_type *dev_info = NULL;
	int ret = 0;
	int cur_idx = src_idx;
	int next_idx;

	/* Update the current path to zero out all request from
	 * this cient on all paths
	 */

	ret = update_path(src, dst, 0, 0, cur_ib, cur_ab, src_idx,
							active_only);
	if (ret) {
		MSM_BUS_ERR("%s: Error zeroing out path ctx %d",
					__func__, ACTIVE_CTX);
		goto exit_remove_path;
	}

	src_dev = bus_find_device(&msm_bus_type, NULL,
				(void *) &src,
				msm_bus_device_match_adhoc);
	if (!src_dev) {
		MSM_BUS_ERR("%s: Can't find source device %d", __func__, src);
		ret = -ENODEV;
		goto exit_remove_path;
	}

	next_dev = src_dev;

	while (next_dev) {
		dev_info = next_dev->platform_data;
		lnode = &dev_info->lnode_list[cur_idx];
		next_idx = lnode->next;
		next_dev = lnode->next_dev;
		remove_lnode(dev_info, cur_idx);
		cur_idx = next_idx;
	}

exit_remove_path:
	return ret;
}

static void getpath_debug(int src, int curr, int active_only)
{
	struct device *dev_node;
	struct device *dev_it;
	unsigned int hop = 1;
	int idx;
	struct msm_bus_node_device_type *devinfo;
	int i;

	dev_node = bus_find_device(&msm_bus_type, NULL,
				(void *) &src,
				msm_bus_device_match_adhoc);

	if (!dev_node) {
		MSM_BUS_ERR("SRC NOT FOUND %d", src);
		return;
	}

	idx = curr;
	devinfo = dev_node->platform_data;
	dev_it = dev_node;

	MSM_BUS_ERR("Route list Src %d", src);
	while (dev_it) {
		struct msm_bus_node_device_type *busdev =
			devinfo->node_info->bus_device->platform_data;

		MSM_BUS_ERR("Hop[%d] at Device %d ctx %d", hop,
					devinfo->node_info->id, active_only);

		for (i = 0; i < NUM_CTX; i++) {
			MSM_BUS_ERR("dev info sel ib %llu",
						devinfo->cur_clk_hz[i]);
			MSM_BUS_ERR("dev info sel ab %llu",
						devinfo->node_ab.ab[i]);
		}

		dev_it = devinfo->lnode_list[idx].next_dev;
		idx = devinfo->lnode_list[idx].next;
		if (dev_it)
			devinfo = dev_it->platform_data;

		MSM_BUS_ERR("Bus Device %d", busdev->node_info->id);
		MSM_BUS_ERR("Bus Clock %llu", busdev->clk[active_only].rate);

		if (idx < 0)
			break;
		hop++;
	}
}

static void unregister_client_adhoc(uint32_t cl)
{
	int i;
	struct msm_bus_scale_pdata *pdata;
	int lnode, src, curr, dest;
	uint64_t  cur_clk, cur_bw;
	struct msm_bus_client *client;

	mutex_lock(&msm_bus_adhoc_lock);
	if (!cl) {
		MSM_BUS_ERR("%s: Null cl handle passed unregister\n",
				__func__);
		goto exit_unregister_client;
	}
	client = handle_list.cl_list[cl];
	curr = client->curr;
	pdata = client->pdata;
	if (!pdata) {
		MSM_BUS_ERR("%s: Null pdata passed to unregister\n",
				__func__);
		goto exit_unregister_client;
	}

	MSM_BUS_DBG("%s: Unregistering client %p", __func__, client);

	for (i = 0; i < pdata->usecase->num_paths; i++) {
		src = client->pdata->usecase[curr].vectors[i].src;
		dest = client->pdata->usecase[curr].vectors[i].dst;

		lnode = client->src_pnode[i];
		cur_clk = client->pdata->usecase[curr].vectors[i].ib;
		cur_bw = client->pdata->usecase[curr].vectors[i].ab;
		if (curr < 0) {
			cur_clk = 0;
			cur_bw = 0;
		}
		remove_path(src, dest, cur_clk, cur_bw, lnode,
						pdata->active_only);
	}
	msm_bus_dbg_client_data(client->pdata, MSM_BUS_DBG_UNREGISTER, cl);
	kfree(client->src_pnode);
	kfree(client);
	handle_list.cl_list[cl] = NULL;
exit_unregister_client:
	mutex_unlock(&msm_bus_adhoc_lock);
	return;
}

static int alloc_handle_lst(int size)
{
	int ret = 0;
	struct msm_bus_client **t_cl_list;

	if (!handle_list.num_entries) {
		t_cl_list = kzalloc(sizeof(struct msm_bus_client *)
			* NUM_CL_HANDLES, GFP_KERNEL);
		if (ZERO_OR_NULL_PTR(t_cl_list)) {
			ret = -ENOMEM;
			MSM_BUS_ERR("%s: Failed to allocate handles list",
								__func__);
			goto exit_alloc_handle_lst;
		}
		handle_list.cl_list = t_cl_list;
		handle_list.num_entries += NUM_CL_HANDLES;
	} else {
		t_cl_list = krealloc(handle_list.cl_list,
				sizeof(struct msm_bus_client *) *
				handle_list.num_entries + NUM_CL_HANDLES,
				GFP_KERNEL);
		if (ZERO_OR_NULL_PTR(t_cl_list)) {
			ret = -ENOMEM;
			MSM_BUS_ERR("%s: Failed to allocate handles list",
								__func__);
			goto exit_alloc_handle_lst;
		}

		memset(&handle_list.cl_list[handle_list.num_entries], 0,
			NUM_CL_HANDLES * sizeof(struct msm_bus_client *));
		handle_list.num_entries += NUM_CL_HANDLES;
		handle_list.cl_list = t_cl_list;
	}
exit_alloc_handle_lst:
	return ret;
}

static uint32_t gen_handle(struct msm_bus_client *client)
{
	uint32_t handle = 0;
	int i;
	int ret = 0;

	for (i = 0; i < handle_list.num_entries; i++) {
		if (i && !handle_list.cl_list[i]) {
			handle = i;
			break;
		}
	}

	if (!handle) {
		ret = alloc_handle_lst(NUM_CL_HANDLES);

		if (ret) {
			MSM_BUS_ERR("%s: Failed to allocate handle list",
							__func__);
			goto exit_gen_handle;
		}
		handle = i + 1;
	}
	handle_list.cl_list[handle] = client;
exit_gen_handle:
	return handle;
}

static uint32_t register_client_adhoc(struct msm_bus_scale_pdata *pdata)
{
	int src, dest;
	int i;
	struct msm_bus_client *client = NULL;
	int *lnode;
	uint32_t handle = 0;

	mutex_lock(&msm_bus_adhoc_lock);
	client = kzalloc(sizeof(struct msm_bus_client), GFP_KERNEL);
	if (!client) {
		MSM_BUS_ERR("%s: Error allocating client data", __func__);
		goto exit_register_client;
	}
	client->pdata = pdata;

	lnode = kzalloc(pdata->usecase->num_paths * sizeof(int), GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(lnode)) {
		MSM_BUS_ERR("%s: Error allocating pathnode ptr!", __func__);
		goto exit_register_client;
	}
	client->src_pnode = lnode;

	for (i = 0; i < pdata->usecase->num_paths; i++) {
		src = pdata->usecase->vectors[i].src;
		dest = pdata->usecase->vectors[i].dst;

		if ((src < 0) || (dest < 0)) {
			MSM_BUS_ERR("%s:Invalid src/dst.src %d dest %d",
				__func__, src, dest);
			goto exit_register_client;
		}

		lnode[i] = getpath(src, dest);
		if (lnode[i] < 0) {
			MSM_BUS_ERR("%s:Failed to find path.src %d dest %d",
				__func__, src, dest);
			goto exit_register_client;
		}
	}

	handle = gen_handle(client);
	msm_bus_dbg_client_data(client->pdata, MSM_BUS_DBG_REGISTER,
					handle);
	MSM_BUS_DBG("%s:Client handle %d %s", __func__, handle,
						client->pdata->name);
exit_register_client:
	mutex_unlock(&msm_bus_adhoc_lock);
	return handle;
}

static int update_request_adhoc(uint32_t cl, unsigned int index)
{
	int i, ret = 0;
	struct msm_bus_scale_pdata *pdata;
	int lnode, src, curr, dest;
	uint64_t req_clk, req_bw, curr_clk, curr_bw;
	struct msm_bus_client *client;
	const char *test_cl = "Null";
	bool log_transaction = false;

	mutex_lock(&msm_bus_adhoc_lock);

	if (!cl) {
		MSM_BUS_ERR("%s: Invalid client handle %d", __func__, cl);
		ret = -ENXIO;
		goto exit_update_request;
	}

	client = handle_list.cl_list[cl];
	pdata = client->pdata;
	if (!pdata) {
		MSM_BUS_ERR("%s: Client data Null.[client didn't register]",
				__func__);
		ret = -ENXIO;
		goto exit_update_request;
	}

	if (index >= pdata->num_usecases) {
		MSM_BUS_ERR("Client %u passed invalid index: %d\n",
			cl, index);
		ret = -ENXIO;
		goto exit_update_request;
	}

	if (client->curr == index) {
		MSM_BUS_DBG("%s: Not updating client request idx %d unchanged",
				__func__, index);
		goto exit_update_request;
	}

	curr = client->curr;
	client->curr = index;

	if (!strcmp(test_cl, pdata->name))
		log_transaction = true;

	MSM_BUS_DBG("%s: cl: %u index: %d curr: %d num_paths: %d\n", __func__,
		cl, index, client->curr, client->pdata->usecase->num_paths);

	for (i = 0; i < pdata->usecase->num_paths; i++) {
		src = client->pdata->usecase[index].vectors[i].src;
		dest = client->pdata->usecase[index].vectors[i].dst;

		lnode = client->src_pnode[i];
		req_clk = client->pdata->usecase[index].vectors[i].ib;
		req_bw = client->pdata->usecase[index].vectors[i].ab;
		if (curr < 0) {
			curr_clk = 0;
			curr_bw = 0;
		} else {
			curr_clk = client->pdata->usecase[curr].vectors[i].ib;
			curr_bw = client->pdata->usecase[curr].vectors[i].ab;
			MSM_BUS_DBG("%s:ab: %llu ib: %llu\n", __func__,
					curr_bw, curr_clk);
		}

		ret = update_path(src, dest, req_clk, req_bw,
				curr_clk, curr_bw, lnode, pdata->active_only);

		if (ret) {
			MSM_BUS_ERR("%s: Update path failed! %d ctx %d\n",
					__func__, ret, ACTIVE_CTX);
			goto exit_update_request;
		}

		if (log_transaction)
			getpath_debug(src, lnode, pdata->active_only);
	}
	msm_bus_dbg_client_data(client->pdata, index , cl);
exit_update_request:
	mutex_unlock(&msm_bus_adhoc_lock);
	return ret;
}

/**
 *  msm_bus_arb_setops_adhoc() : Setup the bus arbitration ops
 *  @ arb_ops: pointer to the arb ops.
 */
void msm_bus_arb_setops_adhoc(struct msm_bus_arb_ops *arb_ops)
{
	arb_ops->register_client = register_client_adhoc;
	arb_ops->update_request = update_request_adhoc;
	arb_ops->unregister_client = unregister_client_adhoc;
}
