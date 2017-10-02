/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2000-2017
 *					All rights reserved
 *
 *  This file is part of GPAC / BIFS decoder filter
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <gpac/filters.h>
#include <gpac/bifs.h>
#include <gpac/constants.h>
#include <gpac/compositor.h>
#include <gpac/internal/terminal_dev.h>


typedef struct
{
	GF_BifsDecoder *bifs_dec;
	GF_ObjectManager *odm;
	GF_Scene *scene;

} GF_BIFSDecCtx;

#ifndef GPAC_DISABLE_BIFS

static GF_Err bifs_dec_configure_bifs_dec(GF_BIFSDecCtx *ctx, GF_FilterPid *pid)
{
	GF_Err e;
	u32 es_id=0;
	u32 oti=0;
	const GF_PropertyValue *prop;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_ID);
	if (prop) es_id = prop->value.uint;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_OTI);
	if (prop) oti = prop->value.uint;


	//we must have a dsi
	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_DECODER_CONFIG);
	if (!prop || !prop->value.data || !prop->data_len) {
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	if (!ctx->bifs_dec) {
		/*if a node asked for this media object, use the scene graph of the node (AnimationStream in PROTO)*/
		if (ctx->odm->mo && ctx->odm->mo->node_ptr) {
			GF_SceneGraph *sg = gf_node_get_graph((GF_Node*)ctx->odm->mo->node_ptr);
			ctx->bifs_dec = gf_bifs_decoder_new(sg, GF_TRUE);
			ctx->odm->mo->node_ptr = NULL;
		} else {
			ctx->bifs_dec = gf_bifs_decoder_new(ctx->scene->graph, GF_FALSE);
		}
	}


	e = gf_bifs_decoder_configure_stream(ctx->bifs_dec, es_id, prop->value.data, prop->data_len, oti);
	if (e) return e;

	return GF_OK;
}

Bool gf_scene_check_namespace(GF_Scene *scene, GF_FilterPid *pid)
{
	if (!scene->root_od) return GF_TRUE;
	assert(scene->root_od->scene_ns);
	assert(scene->root_od->scene_ns->source_filter);

	return gf_filter_pid_is_filter_in_parents(pid, scene->root_od->scene_ns->source_filter);
}

GF_Err bifs_dec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_FilterPid *out_pid;
	GF_BIFSDecCtx *ctx = gf_filter_get_udta(filter);
	const GF_PropertyValue *prop;

	//we must have streamtype SCENE
	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_STREAM_TYPE);
	if (!prop || (prop->value.uint != GF_STREAM_SCENE)) {
		return GF_NOT_SUPPORTED;
	}
	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_OTI);
	if (!prop || ( (prop->value.uint != GPAC_OTI_SCENE_BIFS) && (prop->value.uint != GPAC_OTI_SCENE_BIFS_V2)) ) {
		return GF_NOT_SUPPORTED;
	}
	//we must have a dsi
	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_DECODER_CONFIG);
	if (!prop || !prop->value.data || !prop->data_len) {
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	if (is_remove) {
		out_pid = gf_filter_pid_get_udta(pid);
		gf_filter_pid_remove(out_pid);
		return GF_OK;
	}
	//this is a reconfigure
	if (gf_filter_pid_get_udta(pid)) {
		return bifs_dec_configure_bifs_dec(ctx, pid);
	}

	//check our namespace
	if (ctx->scene && ! gf_filter_pid_is_filter_in_parents(pid, ctx->scene->root_od->scene_ns->source_filter)) {
		return GF_REQUIRES_NEW_INSTANCE;
	}


	//declare a new output PID of type STREAM, OTI RAW
	out_pid = gf_filter_pid_new(filter);

	gf_filter_pid_reset_properties(out_pid);
	gf_filter_pid_copy_properties(out_pid, pid);
	gf_filter_pid_set_property(out_pid, GF_PROP_PID_OTI, &PROP_UINT(GPAC_OTI_RAW_MEDIA_STREAM) );
	gf_filter_pid_set_udta(pid, out_pid);
	return GF_OK;
}


GF_Err bifs_dec_process(GF_Filter *filter)
{
	GF_Err e;
	Double ts_offset;
	u64 now, cts;
	u32 obj_time;
	u32 i, count;
	const char *data;
	u32 size, ESID=0;
	const GF_PropertyValue *prop;
	GF_FilterPacket *pck;
	GF_BIFSDecCtx *ctx = gf_filter_get_udta(filter);

	GF_Scene *scene = ctx->scene;

	if (!scene || !ctx->bifs_dec) return GF_OK;

	count = gf_filter_get_ipid_count(filter);
	for (i=0; i<count; i++) {
		GF_FilterPid *pid = gf_filter_get_ipid(filter, i);
		GF_FilterPid *opid = gf_filter_pid_get_udta(pid);

		GF_ObjectManager *odm = gf_filter_pid_get_udta(opid);
		if (!odm) continue;
		//object clock shall be valid
		assert(odm->ck);

		pck = gf_filter_pid_get_packet(pid);
		if (!pck) {
			Bool is_eos = gf_filter_pid_is_eos(pid);
			if (is_eos)
				gf_filter_pid_set_eos(opid);
			continue;
		}
		data = gf_filter_pck_get_data(pck, &size);

		prop = gf_filter_pid_get_property(pid, GF_PROP_PID_ID);
		if (prop) ESID = prop->value.uint;

		cts = gf_filter_pck_get_cts( pck );
		ts_offset = (Double) cts;
		ts_offset /= gf_filter_pck_get_timescale(pck);

		gf_odm_check_buffering(odm, pid);


		//we still process any frame before our clock time even when buffering
		obj_time = gf_clock_time(odm->ck);
		if (ts_offset * 1000 > obj_time) {
			u32 wait_ms = (u32) (ts_offset * 1000 - obj_time);
			if (!scene->compositor->ms_until_next_frame || (wait_ms<scene->compositor->ms_until_next_frame))
				scene->compositor->ms_until_next_frame = wait_ms;

			continue;
		}

		now = gf_sys_clock_high_res();
		e = gf_bifs_decode_au(ctx->bifs_dec, ESID, data, size, ts_offset);
		now = gf_sys_clock_high_res() - now;

		GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[BIFS] ODM%d #CH%d at %d decoded AU TS %u in "LLU" us\n", odm->ID, ESID, obj_time, cts, now));

		gf_filter_pid_drop_packet(pid);

		if (e) return e;
		if (odm == ctx->odm)
			gf_scene_attach_to_compositor(scene);
	}
	return GF_OK;
}

static void bifs_dec_finalize(GF_Filter *filter)
{
	GF_BIFSDecCtx *ctx = gf_filter_get_udta(filter);
	if (ctx->bifs_dec) gf_bifs_decoder_del(ctx->bifs_dec);
}


static Bool bifs_dec_process_event(GF_Filter *filter, GF_FilterEvent *com)
{
	u32 count, i;
	GF_BIFSDecCtx *ctx = gf_filter_get_udta(filter);
	GF_FilterPid *ipid;
	//check for scene attach
	switch (com->base.type) {
	case GF_FEVT_ATTACH_SCENE:
		break;
	default:
		return GF_FALSE;
	}
	if (!com->attach_scene.on_pid) return GF_TRUE;

	ipid = gf_filter_pid_get_udta(com->attach_scene.on_pid);

	count = gf_filter_get_ipid_count(filter);
	for (i=0; i<count; i++) {
		GF_FilterPid *ipid = gf_filter_get_ipid(filter, i);
		GF_FilterPid *opid = gf_filter_pid_get_udta(ipid);
		//we found our pid, set it up
		if (opid == com->attach_scene.on_pid) {
			if (!ctx->odm) {
				ctx->odm = com->attach_scene.object_manager;
				ctx->scene = ctx->odm->subscene ? ctx->odm->subscene : ctx->odm->parentscene;
			}
			bifs_dec_configure_bifs_dec(ctx, ipid);
			gf_filter_pid_set_udta(opid, com->attach_scene.object_manager);
			return GF_TRUE;
		}
	}

	return GF_TRUE;
}

static const GF_FilterCapability BIFSDecInputs[] =
{
	{.code=GF_PROP_PID_STREAM_TYPE, PROP_UINT(GF_STREAM_SCENE)},
	{.code=GF_PROP_PID_OTI, PROP_UINT(GPAC_OTI_SCENE_BIFS) },
	{.code=GF_PROP_PID_UNFRAMED, PROP_BOOL(GF_TRUE), .exclude=GF_TRUE},

	{.code=GF_PROP_PID_STREAM_TYPE, PROP_UINT(GF_STREAM_SCENE), .start=GF_TRUE},
	{.code=GF_PROP_PID_OTI, PROP_UINT(GPAC_OTI_SCENE_BIFS_V2) },
	{.code=GF_PROP_PID_UNFRAMED, PROP_BOOL(GF_TRUE), .exclude=GF_TRUE},

	{}
};

static const GF_FilterCapability BIFSDecOutputs[] =
{
	{.code= GF_PROP_PID_STREAM_TYPE, PROP_UINT(GF_STREAM_SCENE)},
	{.code= GF_PROP_PID_OTI, PROP_UINT( GPAC_OTI_RAW_MEDIA_STREAM )},
	{}
};

GF_FilterRegister BIFSDecRegister = {
	.name = "bifs_dec",
	.description = "MPEG-4 BIFS Decoder",
	.private_size = sizeof(GF_BIFSDecCtx),
	.requires_main_thread = GF_TRUE,
	.args = NULL,
	.input_caps = BIFSDecInputs,
	.output_caps = BIFSDecOutputs,
	.finalize = bifs_dec_finalize,
	.process = bifs_dec_process,
	.configure_pid = bifs_dec_configure_pid,
	.update_arg = NULL,
	.process_event = bifs_dec_process_event,
};

#endif /*GPAC_DISABLE_BIFS*/

const GF_FilterRegister *bifs_dec_register(GF_FilterSession *session)
{
#ifdef GPAC_DISABLE_BIFS
	return NULL;
#else
	return &BIFSDecRegister;
#endif /*GPAC_DISABLE_BIFS*/
}



