/*****************************************************************************\
 *  burst_buffer_common.c - Common logic for managing burst_buffers
 *
 *  NOTE: These functions are designed so they can be used by multiple burst
 *  buffer plugins at the same time (e.g. you might provide users access to
 *  both burst_buffer/cray and burst_buffer/generic on the same system), so
 *  the state information is largely in the individual plugin and passed as
 *  a pointer argument to these functions.
 *****************************************************************************
 *  Copyright (C) 2014 SchedMD LLC.
 *  Written by Morris Jette <jette@schedmd.com>
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#if     HAVE_CONFIG_H
#  include "config.h"
#endif

#define _GNU_SOURCE	/* For POLLRDHUP */
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "slurm/slurm.h"

#include "src/common/list.h"
#include "src/common/pack.h"
#include "src/common/parse_config.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/timers.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/slurmctld.h"

#include "burst_buffer_common.h"
static struct bb_entry *_json_parse_array(json_object *, char *, int *);
static void _json_parse_object(json_object *, struct bb_entry *);

/* Translate colon delimitted list of users into a UID array,
 * Return value must be xfreed */
static uid_t *_parse_users(char *buf)
{
	char *delim, *tmp, *tok, *save_ptr = NULL;
	int inx = 0, array_size;
	uid_t *user_array = NULL;

	if (!buf)
		return user_array;
	tmp = xstrdup(buf);
	delim = strchr(tmp, ',');
	if (delim)
		delim[0] = '\0';
	array_size = 1;
	user_array = xmalloc(sizeof(uid_t) * array_size);
	tok = strtok_r(tmp, ":", &save_ptr);
	while (tok) {
		if ((uid_from_string(tok, user_array + inx) == -1) ||
		    (user_array[inx] == 0)) {
			error("%s: ignoring invalid user: %s", __func__, tok);
		} else {
			if (++inx >= array_size) {
				array_size *= 2;
				user_array = xrealloc(user_array,
						      sizeof(uid_t)*array_size);
			}
		}
		tok = strtok_r(NULL, ":", &save_ptr);
	}
	xfree(tmp);
	return user_array;
}

/* Translate an array of (zero terminated) UIDs into a string with colon
 * delimited UIDs
 * Return value must be xfreed */
static char *_print_users(uid_t *buf)
{
	char *user_elem, *user_str = NULL;
	int i;

	if (!buf)
		return user_str;
	for (i = 0; buf[i]; i++) {
		user_elem = uid_to_string(buf[i]);
		if (!user_elem)
			continue;
		if (user_str)
			xstrcat(user_str, ":");
		xstrcat(user_str, user_elem);
		xfree(user_elem);
	}
	return user_str;
}

/* Allocate burst buffer hash tables */
extern void bb_alloc_cache(bb_state_t *state_ptr)
{
	state_ptr->bb_hash  = xmalloc(sizeof(bb_alloc_t *) * BB_HASH_SIZE);
	state_ptr->bb_uhash = xmalloc(sizeof(bb_user_t *)  * BB_HASH_SIZE);
}

/* Clear all cached burst buffer records, freeing all memory. */
extern void bb_clear_cache(bb_state_t *state_ptr)
{
	bb_alloc_t *bb_current,   *bb_next;
	bb_user_t  *user_current, *user_next;
	int i;

	if (state_ptr->bb_hash) {
		for (i = 0; i < BB_HASH_SIZE; i++) {
			bb_current = state_ptr->bb_hash[i];
			while (bb_current) {
				bb_next = bb_current->next;
				xfree(bb_current);
				bb_current = bb_next;
			}
		}
		xfree(state_ptr->bb_hash);
	}

	if (state_ptr->bb_uhash) {
		for (i = 0; i < BB_HASH_SIZE; i++) {
			user_current = state_ptr->bb_uhash[i];
			while (user_current) {
				user_next = user_current->next;
				xfree(user_current);
				user_current = user_next;
			}
		}
		xfree(state_ptr->bb_uhash);
	}
}

/* Clear configuration parameters, free memory
 * config_ptr IN - Initial configuration to be cleared
 * fini IN - True if shutting down, do more complete clean-up */
extern void bb_clear_config(bb_config_t *config_ptr, bool fini)
{
	int i;

	xassert(config_ptr);
	xfree(config_ptr->allow_users);
	xfree(config_ptr->allow_users_str);
	config_ptr->debug_flag = false;
	xfree(config_ptr->deny_users);
	xfree(config_ptr->deny_users_str);
	xfree(config_ptr->get_sys_state);
	config_ptr->granularity = 1;
	if (fini) {
		for (i = 0; i < config_ptr->gres_cnt; i++)
			xfree(config_ptr->gres_ptr[i].name);
		xfree(config_ptr->gres_ptr);
		config_ptr->gres_cnt = 0;
	} else {
		for (i = 0; i < config_ptr->gres_cnt; i++)
			config_ptr->gres_ptr[i].avail_cnt = 0;
	}
	config_ptr->job_size_limit = NO_VAL;
	config_ptr->stage_in_timeout = 0;
	config_ptr->stage_out_timeout = 0;
	config_ptr->prio_boost_alloc = 0;
	config_ptr->prio_boost_use = 0;
	xfree(config_ptr->start_stage_in);
	xfree(config_ptr->start_stage_out);
	xfree(config_ptr->stop_stage_in);
	xfree(config_ptr->stop_stage_out);
	config_ptr->user_size_limit = NO_VAL;
}

/* Find a per-job burst buffer record for a specific job.
 * If not found, return NULL. */
extern bb_alloc_t *bb_find_job_rec(struct job_record *job_ptr,
				   bb_alloc_t **bb_hash)
{
	bb_alloc_t *bb_ptr = NULL;

	xassert(job_ptr);
	bb_ptr = bb_hash[job_ptr->user_id % BB_HASH_SIZE];
	while (bb_ptr) {
		if (bb_ptr->job_id == job_ptr->job_id) {
			if (bb_ptr->user_id == job_ptr->user_id)
				return bb_ptr;
			error("%s: Slurm state inconsistent with burst "
			      "buffer. JobID %u has UserID mismatch (%u != %u)",
			      __func__, job_ptr->user_id, bb_ptr->user_id,
			      job_ptr->user_id);
			/* This has been observed when slurmctld crashed and
			 * the job state recovered was missing some jobs
			 * which already had burst buffers configured. */
		}
		bb_ptr = bb_ptr->next;
	}
	return bb_ptr;
}

/* Add a burst buffer allocation to a user's load */
extern void bb_add_user_load(bb_alloc_t *bb_ptr, bb_state_t *state_ptr)
{
	bb_user_t *user_ptr;

	state_ptr->used_space += bb_ptr->size;

	user_ptr = bb_find_user_rec(bb_ptr->user_id, state_ptr->bb_uhash);
	user_ptr->size += bb_ptr->size;
}

/* Find a per-user burst buffer record for a specific user ID */
extern bb_user_t *bb_find_user_rec(uint32_t user_id, bb_user_t **bb_uhash)
{
	int inx = user_id % BB_HASH_SIZE;
	bb_user_t *user_ptr;

	xassert(bb_uhash);
	user_ptr = bb_uhash[inx];
	while (user_ptr) {
		if (user_ptr->user_id == user_id)
			return user_ptr;
		user_ptr = user_ptr->next;
	}
	user_ptr = xmalloc(sizeof(bb_user_t));
	user_ptr->next = bb_uhash[inx];
	/* user_ptr->size = 0;	initialized by xmalloc */
	user_ptr->user_id = user_id;
	bb_uhash[inx] = user_ptr;
	return user_ptr;
}

/* Remove a burst buffer allocation from a user's load */
extern void bb_remove_user_load(bb_alloc_t *bb_ptr, bb_state_t *state_ptr)
{
	bb_user_t *user_ptr;

	if (state_ptr->used_space >= bb_ptr->size) {
		state_ptr->used_space -= bb_ptr->size;
	} else {
		error("%s: used space underflow releasing buffer for job %u",
		      __func__, bb_ptr->job_id);
		state_ptr->used_space = 0;
	}

	user_ptr = bb_find_user_rec(bb_ptr->user_id, state_ptr->bb_uhash);
	if (user_ptr->size >= bb_ptr->size) {
		user_ptr->size -= bb_ptr->size;
	} else {
		error("%s: user %u table underflow",
		      __func__, user_ptr->user_id);
		user_ptr->size = 0;
	}
}

static uint32_t _atoi(char *tok)
{
	char *end_ptr = NULL;
	int32_t size_i;
	uint32_t size_u = 0;

	size_i = strtol(tok, &end_ptr, 10);
	if (size_i > 0) {
		size_u = (uint32_t) size_i;
		if ((end_ptr[0] == 'k') || (end_ptr[0] == 'K')) {
			size_u = size_u * 1024;
		} else if ((end_ptr[0] == 'm') || (end_ptr[0] == 'M')) {
			size_u = size_u * 1024 * 1024;
		} else if ((end_ptr[0] == 'g') || (end_ptr[0] == 'G')) {
			size_u = size_u * 1024 * 1024 * 1024;
		}
	}
	return size_u;
}

/* Load and process configuration parameters */
extern void bb_load_config(bb_state_t *state_ptr, char *type)
{
	s_p_hashtbl_t *bb_hashtbl = NULL;
	char *bb_conf, *colon, *save_ptr, *tmp = NULL, *tok, *value;
	uint32_t gres_cnt;
	int fd, i;
	static s_p_options_t bb_options[] = {
		{"AllowUsers", S_P_STRING},
		{"DenyUsers", S_P_STRING},
		{"GetSysState", S_P_STRING},
		{"Granularity", S_P_STRING},
		{"Gres", S_P_STRING},
		{"JobSizeLimit", S_P_STRING},
		{"PrioBoostAlloc", S_P_UINT32},
		{"PrioBoostUse", S_P_UINT32},
		{"PrivateData", S_P_STRING},
		{"StageInTimeout", S_P_UINT32},
		{"StageOutTimeout", S_P_UINT32},
		{"StartStageIn", S_P_STRING},
		{"StartStageOut", S_P_STRING},
		{"StopStageIn", S_P_STRING},
		{"StopStageOut", S_P_STRING},
		{"UserSizeLimit", S_P_STRING},
		{NULL}
	};

	bb_clear_config(&state_ptr->bb_config, false);
	if (slurm_get_debug_flags() & DEBUG_FLAG_BURST_BUF)
		state_ptr->bb_config.debug_flag = true;

	/* First look for "burst_buffer.conf" then with "type" field,
	 * for example "burst_buffer_cray.conf" */
	bb_conf = get_extra_conf_path("burst_buffer.conf");
	fd = open(bb_conf, 0);
	if (fd >= 0) {
		close(fd);
	} else {
		char *new_path = NULL;
		xfree(bb_conf);
		xstrfmtcat(new_path, "burst_buffer_%s.conf", type);
		bb_conf = get_extra_conf_path(new_path);
		fd = open(bb_conf, 0);
		if (fd < 0) {
			fatal("%s: Unable to find configuration file %s or "
			      "burst_buffer.conf", __func__, new_path);
		}
		xfree(new_path);
	}

	bb_hashtbl = s_p_hashtbl_create(bb_options);
	if (s_p_parse_file(bb_hashtbl, NULL, bb_conf, false) == SLURM_ERROR) {
		fatal("%s: something wrong with opening/reading %s: %m",
		      __func__, bb_conf);
	}
	if (s_p_get_string(&state_ptr->bb_config.allow_users_str, "AllowUsers",
			   bb_hashtbl)) {
		state_ptr->bb_config.allow_users = _parse_users(
					state_ptr->bb_config.allow_users_str);
	}
	if (s_p_get_string(&state_ptr->bb_config.deny_users_str, "DenyUsers",
			   bb_hashtbl)) {
		state_ptr->bb_config.deny_users = _parse_users(
					state_ptr->bb_config.deny_users_str);
	}
	s_p_get_string(&state_ptr->bb_config.get_sys_state, "GetSysState",
		       bb_hashtbl);
	if (s_p_get_string(&tmp, "Granularity", bb_hashtbl)) {
		state_ptr->bb_config.granularity = bb_get_size_num(tmp, 1);
		xfree(tmp);
		if (state_ptr->bb_config.granularity == 0) {
			error("%s: Granularity=0 is invalid", __func__);
			state_ptr->bb_config.granularity = 1;
		}
	}
	if (s_p_get_string(&tmp, "Gres", bb_hashtbl)) {
		tok = strtok_r(tmp, ",", &save_ptr);
		while (tok) {
			colon = strchr(tok, ':');
			if (colon) {
				colon[0] = '\0';
				gres_cnt = _atoi(colon+1);
			} else
				gres_cnt = 1;
			state_ptr->bb_config.gres_ptr = xrealloc(
				state_ptr->bb_config.gres_ptr,
				sizeof(burst_buffer_gres_t) *
				(state_ptr->bb_config.gres_cnt + 1));
			state_ptr->bb_config.
				gres_ptr[state_ptr->bb_config.gres_cnt].name =
				xstrdup(tok);
			state_ptr->bb_config.
				gres_ptr[state_ptr->bb_config.gres_cnt].
				avail_cnt = gres_cnt;
			state_ptr->bb_config.gres_cnt++;
			tok = strtok_r(NULL, ",", &save_ptr);
		}
		xfree(tmp);
	}
	if (s_p_get_string(&tmp, "JobSizeLimit", bb_hashtbl)) {
		state_ptr->bb_config.job_size_limit = bb_get_size_num(tmp, 1);
		xfree(tmp);
	}
	if (s_p_get_uint32(&state_ptr->bb_config.prio_boost_alloc,
			   "PrioBoostAlloc", bb_hashtbl) &&
	    (state_ptr->bb_config.prio_boost_alloc > NICE_OFFSET)) {
		error("%s: PrioBoostAlloc can not exceed %u",
		      __func__, NICE_OFFSET);
		state_ptr->bb_config.prio_boost_alloc = NICE_OFFSET;
	}
	if (s_p_get_uint32(&state_ptr->bb_config.prio_boost_use, "PrioBoostUse",
			   bb_hashtbl) &&
	    (state_ptr->bb_config.prio_boost_use > NICE_OFFSET)) {
		error("%s: PrioBoostUse can not exceed %u",
		      __func__, NICE_OFFSET);
		state_ptr->bb_config.prio_boost_use = NICE_OFFSET;
	}
	if (s_p_get_string(&tmp, "PrivateData", bb_hashtbl)) {
		if (!strcasecmp(tmp, "true") ||
		    !strcasecmp(tmp, "yes")  ||
		    !strcasecmp(tmp, "1"))
			state_ptr->bb_config.private_data = 1;
		xfree(tmp);
	}
	s_p_get_uint32(&state_ptr->bb_config.stage_in_timeout, "StageInTimeout",
		       bb_hashtbl);
	s_p_get_uint32(&state_ptr->bb_config.stage_out_timeout,
		       "StageOutTimeout", bb_hashtbl);
	s_p_get_string(&state_ptr->bb_config.start_stage_in, "StartStageIn",
		       bb_hashtbl);
	s_p_get_string(&state_ptr->bb_config.start_stage_out, "StartStageOut",
			    bb_hashtbl);
	s_p_get_string(&state_ptr->bb_config.stop_stage_in, "StopStageIn",
		       bb_hashtbl);
	s_p_get_string(&state_ptr->bb_config.stop_stage_out, "StopStageOut",
		       bb_hashtbl);
	if (s_p_get_string(&tmp, "UserSizeLimit", bb_hashtbl)) {
		state_ptr->bb_config.user_size_limit = bb_get_size_num(tmp, 1);
		xfree(tmp);
	}

	s_p_hashtbl_destroy(bb_hashtbl);
	xfree(bb_conf);

	if (state_ptr->bb_config.debug_flag) {
		value = _print_users(state_ptr->bb_config.allow_users);
		info("%s: AllowUsers:%s",  __func__, value);
		xfree(value);

		value = _print_users(state_ptr->bb_config.deny_users);
		info("%s: DenyUsers:%s",  __func__, value);
		xfree(value);

		info("%s: GetSysState:%s",  __func__,
		     state_ptr->bb_config.get_sys_state);
		info("%s: Granularity:%u",  __func__,
		     state_ptr->bb_config.granularity);
		for (i = 0; i < state_ptr->bb_config.gres_cnt; i++) {
			info("%s: Gres[%d]:%s:%u", __func__, i,
			     state_ptr->bb_config.gres_ptr[i].name,
			     state_ptr->bb_config.gres_ptr[i].avail_cnt);
		}
		info("%s: JobSizeLimit:%u",  __func__,
		     state_ptr->bb_config.job_size_limit);
		info("%s: PrioBoostAlloc:%u", __func__,
		     state_ptr->bb_config.prio_boost_alloc);
		info("%s: PrioBoostUse:%u", __func__,
		     state_ptr->bb_config.prio_boost_use);
		info("%s: StageInTimeout:%u", __func__,
		     state_ptr->bb_config.stage_in_timeout);
		info("%s: StageOutTimeout:%u", __func__,
		     state_ptr->bb_config.stage_out_timeout);
		info("%s: StartStageIn:%s",  __func__,
		     state_ptr->bb_config.start_stage_in);
		info("%s: StartStageOut:%s",  __func__,
		     state_ptr->bb_config.start_stage_out);
		info("%s: StopStageIn:%s",  __func__,
		     state_ptr->bb_config.stop_stage_in);
		info("%s: StopStageOut:%s",  __func__,
		     state_ptr->bb_config.stop_stage_out);
		info("%s: UserSizeLimit:%u",  __func__,
		     state_ptr->bb_config.user_size_limit);
	}
}

/* Pack individual burst buffer records into a  buffer */
extern int bb_pack_bufs(uid_t uid, bb_alloc_t **bb_hash, Buf buffer,
			uint16_t protocol_version)
{
	int i, j, rec_count = 0;
	struct bb_alloc *bb_next;

	if (!bb_hash)
		return rec_count;

	for (i = 0; i < BB_HASH_SIZE; i++) {
		bb_next = bb_hash[i];
		while (bb_next) {
			if ((uid == 0) || (uid == bb_next->user_id)) {
				pack32(bb_next->array_job_id,  buffer);
				pack32(bb_next->array_task_id, buffer);
				pack32(bb_next->gres_cnt, buffer);
				for (j = 0; j < bb_next->gres_cnt; j++) {
					packstr(bb_next->gres_ptr[j].name,
						buffer);
					pack32(bb_next->gres_ptr[j].avail_cnt,
					       buffer);
					pack32(bb_next->gres_ptr[j].used_cnt,
					       buffer);
				}
				pack32(bb_next->job_id,        buffer);
				packstr(bb_next->name,         buffer);
				pack32(bb_next->size,          buffer);
				pack16(bb_next->state,         buffer);
				pack_time(bb_next->state_time, buffer);
				pack32(bb_next->user_id,       buffer);
				rec_count++;
			}
			bb_next = bb_next->next;
		}
	}

	return rec_count;
}

/* Pack state and configuration parameters into a buffer */
extern void bb_pack_state(bb_state_t *state_ptr, Buf buffer,
			  uint16_t protocol_version)
{
	bb_config_t *config_ptr = &state_ptr->bb_config;
	int i;

	packstr(config_ptr->allow_users_str, buffer);
	packstr(config_ptr->deny_users_str,  buffer);
	packstr(config_ptr->get_sys_state,   buffer);
	pack32(config_ptr->granularity,      buffer);
	pack32(config_ptr->gres_cnt,         buffer);
	for (i = 0; i < config_ptr->gres_cnt; i++) {
		packstr(config_ptr->gres_ptr[i].name, buffer);
		pack32(config_ptr->gres_ptr[i].avail_cnt, buffer);
		pack32(config_ptr->gres_ptr[i].used_cnt, buffer);
	}
	pack16(config_ptr->private_data,     buffer);
	packstr(config_ptr->start_stage_in,  buffer);
	packstr(config_ptr->start_stage_out, buffer);
	packstr(config_ptr->stop_stage_in,   buffer);
	packstr(config_ptr->stop_stage_out,  buffer);
	pack32(config_ptr->job_size_limit,   buffer);
	pack32(config_ptr->prio_boost_alloc, buffer);
	pack32(config_ptr->prio_boost_use,   buffer);
	pack32(config_ptr->stage_in_timeout, buffer);
	pack32(config_ptr->stage_out_timeout,buffer);
	pack32(state_ptr->total_space,       buffer);
	pack32(state_ptr->used_space,        buffer);
	pack32(config_ptr->user_size_limit,  buffer);
}

/* Translate a burst buffer size specification in string form to numeric form,
 * recognizing various sufficies (MB, GB, TB, PB, and Nodes). */
extern uint32_t bb_get_size_num(char *tok, uint32_t granularity)
{
	char *end_ptr = NULL;
	int32_t bb_size_i;
	uint32_t bb_size_u = 0;

	bb_size_i = strtol(tok, &end_ptr, 10);
	if (bb_size_i > 0) {
		bb_size_u = (uint32_t) bb_size_i;
		if ((end_ptr[0] == 'm') || (end_ptr[0] == 'M')) {
			bb_size_u = (bb_size_u + 1023) / 1024;
		} else if ((end_ptr[0] == 'g') || (end_ptr[0] == 'G')) {
			;
		} else if ((end_ptr[0] == 't') || (end_ptr[0] == 'T')) {
			bb_size_u *= 1024;
		} else if ((end_ptr[0] == 'p') || (end_ptr[0] == 'P')) {
			bb_size_u *= (1024 * 1024);
		}
	}

	if (granularity > 1) {
		bb_size_u = ((bb_size_u + granularity - 1) / granularity) *
			    granularity;
	}

	return bb_size_u;
}

extern void bb_job_queue_del(void *x)
{
	xfree(x);
}

/* Sort job queue by expected start time */
extern int bb_job_queue_sort(void *x, void *y)
{
	job_queue_rec_t *job_rec1 = *(job_queue_rec_t **) x;
	job_queue_rec_t *job_rec2 = *(job_queue_rec_t **) y;
	struct job_record *job_ptr1 = job_rec1->job_ptr;
	struct job_record *job_ptr2 = job_rec2->job_ptr;

	if (job_ptr1->start_time > job_ptr2->start_time)
		return 1;
	if (job_ptr1->start_time < job_ptr2->start_time)
		return -1;
	return 0;
}

/* Sort preempt_bb_recs in order of DECREASING use_time */
extern int bb_preempt_queue_sort(void *x, void *y)
{
	struct preempt_bb_recs *bb_ptr1 = *(struct preempt_bb_recs **) x;
	struct preempt_bb_recs *bb_ptr2 = *(struct preempt_bb_recs **) y;

	if (bb_ptr1->use_time > bb_ptr2->use_time)
		return -1;
	if (bb_ptr1->use_time < bb_ptr2->use_time)
		return 1;
	return 0;
};

/* For each burst buffer record, set the use_time to the time at which its
 * use is expected to begin (i.e. each job's expected start time) */
extern void bb_set_use_time(bb_state_t *state_ptr)
{
	struct job_record *job_ptr;
	bb_alloc_t *bb_ptr = NULL;
	time_t now = time(NULL);
	int i;

	state_ptr->next_end_time = now + 60 * 60; /* Start estimate now+1hour */
	for (i = 0; i < BB_HASH_SIZE; i++) {
		bb_ptr = state_ptr->bb_hash[i];
		while (bb_ptr) {
			if (bb_ptr->job_id &&
			    ((bb_ptr->state == BB_STATE_STAGING_IN) ||
			     (bb_ptr->state == BB_STATE_STAGED_IN))) {
				job_ptr = find_job_record(bb_ptr->job_id);
				if (!job_ptr) {
					error("%s: job %u with allocated burst "
					      "buffers not found",
					      __func__, bb_ptr->job_id);
					bb_ptr->use_time = now + 24 * 60 * 60;
				} else if (job_ptr->start_time) {
					bb_ptr->end_time = job_ptr->end_time;
					bb_ptr->use_time = job_ptr->start_time;
				} else {
					/* Unknown start time */
					bb_ptr->use_time = now + 60 * 60;
				}
			} else if (bb_ptr->job_id) {
				job_ptr = find_job_record(bb_ptr->job_id);
				if (job_ptr)
					bb_ptr->end_time = job_ptr->end_time;
			} else {
				bb_ptr->use_time = now;
			}
			if (bb_ptr->end_time && bb_ptr->size) {
				if (bb_ptr->end_time <= now)
					state_ptr->next_end_time = now;
				else if (state_ptr->next_end_time >
					 bb_ptr->end_time) {
					state_ptr->next_end_time =
						bb_ptr->end_time;
				}
			}
			bb_ptr = bb_ptr->next;
		}
	}
}

/* Sleep function, also handles termination signal */
extern void bb_sleep(bb_state_t *state_ptr, int add_secs)
{
	struct timespec ts = {0, 0};
	struct timeval  tv = {0, 0};

	if (gettimeofday(&tv, NULL)) {		/* Some error */
		sleep(1);
		return;
	}

	ts.tv_sec  = tv.tv_sec + add_secs;
	ts.tv_nsec = tv.tv_usec * 1000;
	pthread_mutex_lock(&state_ptr->term_mutex);
	if (!state_ptr->term_flag) {
		pthread_cond_timedwait(&state_ptr->term_cond,
				       &state_ptr->term_mutex, &ts);
	}
	pthread_mutex_unlock(&state_ptr->term_mutex);
}


/* Allocate a named burst buffer record for a specific user.
 * Return a pointer to that record. */
extern bb_alloc_t *bb_alloc_name_rec(bb_state_t *state_ptr, char *name,
				     uint32_t user_id)
{
	bb_alloc_t *bb_ptr = NULL;
	int i;

	xassert(state_ptr->bb_hash);
	bb_ptr = xmalloc(sizeof(bb_alloc_t));
	i = user_id % BB_HASH_SIZE;
	bb_ptr->next = state_ptr->bb_hash[i];
	state_ptr->bb_hash[i] = bb_ptr;
	bb_ptr->name = xstrdup(name);
	bb_ptr->state = BB_STATE_ALLOCATED;
	bb_ptr->state_time = time(NULL);
	bb_ptr->seen_time = time(NULL);
	bb_ptr->user_id = user_id;

	return bb_ptr;
}

/* Allocate a per-job burst buffer record for a specific job.
 * Return a pointer to that record. */
extern bb_alloc_t *bb_alloc_job_rec(bb_state_t *state_ptr,
				    struct job_record *job_ptr,
				    uint32_t bb_size)
{
	bb_alloc_t *bb_ptr = NULL;
	int i;

	xassert(state_ptr->bb_hash);
	xassert(job_ptr);
	bb_ptr = xmalloc(sizeof(bb_alloc_t));
	bb_ptr->array_job_id = job_ptr->array_job_id;
	bb_ptr->array_task_id = job_ptr->array_task_id;
	bb_ptr->job_id = job_ptr->job_id;
	i = job_ptr->user_id % BB_HASH_SIZE;
	bb_ptr->next = state_ptr->bb_hash[i];
	state_ptr->bb_hash[i] = bb_ptr;
	bb_ptr->size = bb_size;
	bb_ptr->state = BB_STATE_ALLOCATED;
	bb_ptr->state_time = time(NULL);
	bb_ptr->seen_time = time(NULL);
	bb_ptr->user_id = job_ptr->user_id;

	return bb_ptr;
}

/* Allocate a burst buffer record for a job and increase the job priority
 * if so configured. */
extern bb_alloc_t *bb_alloc_job(bb_state_t *state_ptr,
				struct job_record *job_ptr, uint32_t bb_size)
{
	bb_alloc_t *bb_ptr;
	uint16_t new_nice;

	if (state_ptr->bb_config.prio_boost_use && job_ptr && job_ptr->details){
		new_nice = (NICE_OFFSET - state_ptr->bb_config.prio_boost_use);
		if (new_nice < job_ptr->details->nice) {
			int64_t new_prio = job_ptr->priority;
			new_prio += job_ptr->details->nice;
			new_prio -= new_nice;
			job_ptr->priority = new_prio;
			job_ptr->details->nice = new_nice;
			info("%s: Uses burst buffer, reset priority to %u "
			     "for job_id %u", __func__,
			     job_ptr->priority, job_ptr->job_id);
		}
	}

	bb_ptr = bb_alloc_job_rec(state_ptr, job_ptr, bb_size);
	bb_add_user_load(bb_ptr, state_ptr);

	return bb_ptr;
}

/* get_bb_entry()
 *
 * This little parser handles the json stream
 * coming from the cray comamnd describing the
 * pools of burst buffers. The json stream is like
 * this { "pools": [ {}, .... {} ] } key pools
 * and an array of objects describing each pool.
 * The objects have only string and int types (for now).
 */
bb_entry_t *
get_bb_entry(int *num_ent, bb_state_t *state_ptr)
{
	bb_entry_t *ents;
	char *string;
	char **script_argv;
	int i;
	json_object *j;
	json_object_iter iter;

	script_argv = xmalloc(sizeof(char *) * 3);
	xstrfmtcat(script_argv[0], "%s", "jsonpools");
	xstrfmtcat(script_argv[1], "%s", "pools");

	string = run_script("jsonpools",
			    state_ptr->bb_config.get_sys_state,
			    script_argv,
			    3600);
	if (string == NULL) {
		error("%s: %s did not return any pool",
		      __func__,state_ptr->bb_config.get_sys_state);
		for (i = 0; script_argv[i]; i++)
			xfree(script_argv[i]);
		xfree(script_argv);
		return NULL;
	}
	for (i = 0; script_argv[i]; i++)
		xfree(script_argv[i]);
	xfree(script_argv);

	j = json_tokener_parse(string);
	if (j == NULL) {
		error("%s: json parser failed on %s", __func__, string);
		xfree(string);
		return NULL;
	}
	xfree(string);

	json_object_object_foreachC(j, iter) {
		ents = _json_parse_array(j, iter.key, num_ent);
	}
	json_object_put(j);

	return ents;
}

/* free_bb_ents()
 */
void
free_bb_ents(struct bb_entry *ents, int num)
{
	int i;

	for (i = 0; i < num; i++) {
		xfree(ents[i].id);
		xfree(ents[i].units);
	}

	xfree(ents);
}

/* Execute a script, wait for termination and return its stdout.
 * script_type IN - Type of program being run (e.g. "StartStageIn")
 * script_path IN - Fully qualified pathname of the program to execute
 * script_args IN - Arguments to the script
 * max_wait IN - maximum time to wait in seconds, -1 for no limit (asynchronous)
 * Return stdout of spawned program, value must be xfreed. */
char *run_script(char *script_type, char *script_path,
		 char **script_argv, int max_wait)
{
	int i, status, new_wait, resp_size = 0, resp_offset = 0;
	pid_t cpid;
	char *resp = NULL;
	int pfd[2] = { -1, -1 };

	if ((script_path == NULL) || (script_path[0] == '\0')) {
		error("%s: no script specified", __func__);
		return resp;
	}
	if (script_path[0] != '/') {
		error("%s: %s is not fully qualified pathname (%s)",
		      __func__, script_type, script_path);
		return resp;
	}
	if (access(script_path, R_OK | X_OK) < 0) {
		error("%s: %s can not be executed (%s) %m",
		      __func__, script_type, script_path);
		return resp;
	}
	if (max_wait != -1) {
		if (pipe(pfd) != 0) {
			error("%s: pipe(): %m", __func__);
			return resp;
		}
	}
	if ((cpid = fork()) == 0) {
		int cc;

		cc = sysconf(_SC_OPEN_MAX);
		if (max_wait != -1) {
			dup2(pfd[1], STDOUT_FILENO);
			for (i = 0; i < cc; i++) {
				if (i != STDOUT_FILENO)
					close(i);
			}
		} else {
			for (i = 0; i < cc; i++)
				close(i);
			if ((cpid = fork()) < 0)
				exit(127);
			else if (cpid > 0)
				exit(0);
		}
#ifdef SETPGRP_TWO_ARGS
		setpgrp(0, 0);
#else
		setpgrp();
#endif
		execv(script_path, script_argv);
		error("%s: execv(%s): %m", __func__, script_path);
		exit(127);
	} else if (cpid < 0) {
		if (max_wait != -1) {
			close(pfd[0]);
			close(pfd[1]);
		}
		error("%s: fork(): %m", __func__);
	} else if (max_wait != -1) {
		struct pollfd fds;
		time_t start_time = time(NULL);
		resp_size = 1024;
		resp = xmalloc(resp_size);
		close(pfd[1]);
		while (1) {
			fds.fd = pfd[0];
			fds.events = POLLIN | POLLHUP | POLLRDHUP;
			fds.revents = 0;
			if (max_wait == -1) {
				new_wait = -1;
			} else {
				new_wait = time(NULL) - start_time + max_wait;
				if (new_wait <= 0)
					break;
			}
			status = poll(&fds, 1, new_wait);
			if (status < 1) {
				error("%s: %s timeout",
				      __func__, script_type);
				break;
			}
			if ((fds.revents & POLLIN) == 0)
				break;
			i = read(pfd[0], resp + resp_offset,
				 resp_size - resp_offset);
			if (i == 0) {
				break;
			} else if (i < 0) {
				if (errno == EAGAIN)
					continue;
				error("%s: read(%s): %m", __func__,
				      script_path);
				break;
			} else {
				resp_offset += i;
				if (resp_offset + 1024 >= resp_size) {
					resp_size *= 2;
					resp = xrealloc(resp, resp_size);
				}
			}
		}
		killpg(cpid, SIGKILL);
		waitpid(cpid, &status, 0);
		close(pfd[0]);
	} else {
		waitpid(cpid, &status, 0);
	}
	return resp;
}

/* json_parse_array()
 */
static struct bb_entry *
_json_parse_array(json_object *jobj, char *key, int *num)
{
	enum json_type type;
	json_object *jarray;
	int i;
	json_object *jvalue;
	struct bb_entry *ents;

	jarray = jobj;
	json_object_object_get_ex(jobj, key, &jarray);

	*num = json_object_array_length(jarray);
	ents = xmalloc(*num * sizeof(struct bb_entry));

	for (i = 0; i < *num; i++){
		jvalue = json_object_array_get_idx(jarray, i);
		type = json_object_get_type(jvalue);
		_json_parse_object(jvalue, &ents[i]);
		/* Convert to GB
		 */
		if (strcmp(ents[i].units, "bytes") == 0) {
			ents[i].gb_granularity
				= ents[i].granularity/(1024*1024*1024);
			ents[i].gb_quantity
				= ents[i].quantity * ents[i].gb_granularity;
			ents[i].gb_free
				= ents[i].free * ents[i].gb_granularity;
		} else {
			/* So the caller can use all the entries
			 * in a loop.
			 */
			ents[i].gb_granularity = ents[i].granularity;
			ents[i].gb_quantity = ents[i].quantity;
			ents[i].gb_free = ents[i].free;
		}
	}

	return ents;
}

/* json_parse_object()
 */
static void
_json_parse_object(json_object *jobj, struct bb_entry *ent)
{
	enum json_type type;
	struct json_object_iter iter;
	int64_t x;
	const char *p;

	json_object_object_foreachC(jobj, iter) {

		type = json_object_get_type(iter.val);
		switch (type) {
			case json_type_boolean:
			case json_type_double:
			case json_type_null:
			case json_type_object:
			case json_type_array:
				break;
			case json_type_int:
				x = json_object_get_int64(iter.val);
				if (strcmp(iter.key, "granularity") == 0) {
					ent->granularity = x;
				} else if (strcmp(iter.key, "quantity") == 0) {
					ent->quantity = x;
				}  else if (strcmp(iter.key, "free") == 0) {
					ent->free = x;
				}
				break;
			case json_type_string:
				p = json_object_get_string(iter.val);
				if (strcmp(iter.key, "id") == 0) {
					ent->id = xstrdup(p);
				} else if (strcmp(iter.key, "units") == 0) {
					ent->units = xstrdup(p);
				}
				break;
		}
	}
}
