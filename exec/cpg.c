/*
 * vi: set autoindent tabstop=4 shiftwidth=4 :
 *
 * Copyright (c) 2006 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Patrick Caulfield (pcaulfie@redhat.com)
 *
 * This software licensed under BSD license, the text of which follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the MontaVista Software, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../include/saAis.h"
#include "../include/saClm.h"
#include "../include/ipc_gen.h"
#include "../include/ipc_cpg.h"
#include "../include/list.h"
#include "../include/queue.h"
#include "../lcr/lcr_comp.h"
#include "aispoll.h"
#include "totempg.h"
#include "totemip.h"
#include "main.h"
#include "mempool.h"
#include "handlers.h"
#include "jhash.h"
#include "swab.h"

#define LOG_SERVICE LOG_SERVICE_CPG
#include "print.h"

#define GROUP_HASH_SIZE 32

#define PI_FLAG_MEMBER 1

enum cpg_message_req_types {
	MESSAGE_REQ_EXEC_CPG_PROCJOIN = 0,
	MESSAGE_REQ_EXEC_CPG_PROCLEAVE = 1,
	MESSAGE_REQ_EXEC_CPG_JOINLIST = 2,
	MESSAGE_REQ_EXEC_CPG_MCAST = 3
};

struct removed_group
{
	struct group_info *gi;
	int left_list_entries;
	struct cpg_groupinfo left_list[32]; /* ?? arbitrary numbers R us */
	struct list_head list;
};

struct group_info {
	struct cpg_name groupName;
	struct list_head members;
	struct list_head list;    /* on hash list */
	struct removed_group *rg; /* when a node goes down */
};

struct process_info {
	struct totem_ip_address node;
	uint32_t pid;
	uint32_t flags;
	void *conn;
	void *trackerconn;
	struct group_info *group;
	struct list_head list; /* on the group_info members list */
};

struct join_list_entry {
	uint32_t pid;
	struct cpg_name groupName;
};

static struct list_head group_lists[GROUP_HASH_SIZE];

/*
 * Service Interfaces required by service_message_handler struct
 */
static void cpg_confchg_fn (
	enum totem_configuration_type configuration_type,
	struct totem_ip_address *member_list, int member_list_entries,
	struct totem_ip_address *left_list, int left_list_entries,
	struct totem_ip_address *joined_list, int joined_list_entries,
	struct memb_ring_id *ring_id);

static int cpg_exec_init_fn (struct openais_config *);

static int cpg_lib_init_fn (void *conn);

static int cpg_lib_exit_fn (void *conn);

static void message_handler_req_exec_cpg_procjoin (
	void *message,
	struct totem_ip_address *source_addr);

static void message_handler_req_exec_cpg_procleave (
	void *message,
	struct totem_ip_address *source_addr);

static void message_handler_req_exec_cpg_joinlist (
	void *message,
	struct totem_ip_address *source_addr);

static void message_handler_req_exec_cpg_mcast (
	void *message,
	struct totem_ip_address *source_addr);

static void exec_cpg_procjoin_endian_convert (void *msg);

static void exec_cpg_joinlist_endian_convert (void *msg);

static void exec_cpg_mcast_endian_convert (void *msg);

static void message_handler_req_lib_cpg_join (void *conn, void *message);

static void message_handler_req_lib_cpg_leave (void *conn, void *message);

static void message_handler_req_lib_cpg_mcast (void *conn, void *message);

static void message_handler_req_lib_cpg_membership (void *conn, void *message);

static void message_handler_req_lib_cpg_trackstart (void *conn, void *message);

static void message_handler_req_lib_cpg_trackstop (void *conn, void *message);

static int cpg_node_joinleave_send (struct group_info *gi, struct process_info *pi, int fn, int reason);

static void cpg_exec_send_joinlist(void);

/*
 * Library Handler Definition
 */
static struct openais_lib_handler cpg_lib_handlers[] =
{
	{ /* 0 */
		.lib_handler_fn				= message_handler_req_lib_cpg_join,
		.response_size				= sizeof (struct res_lib_cpg_join),
		.response_id				= MESSAGE_RES_CPG_JOIN,
		.flow_control				= OPENAIS_FLOW_CONTROL_REQUIRED
	},
	{ /* 1 */
		.lib_handler_fn				= message_handler_req_lib_cpg_leave,
		.response_size				= sizeof (struct res_lib_cpg_leave),
		.response_id				= MESSAGE_RES_CPG_LEAVE,
		.flow_control				= OPENAIS_FLOW_CONTROL_REQUIRED
	},
	{ /* 2 */
		.lib_handler_fn				= message_handler_req_lib_cpg_mcast,
		.response_size				= sizeof (struct res_header),
		.response_id				= MESSAGE_RES_CPG_MCAST,
		.flow_control				= OPENAIS_FLOW_CONTROL_REQUIRED
	},
	{ /* 3 */
		.lib_handler_fn				= message_handler_req_lib_cpg_membership,
		.response_size				= sizeof (struct res_header),
		.response_id				= MESSAGE_RES_CPG_MEMBERSHIP,
		.flow_control				= OPENAIS_FLOW_CONTROL_NOT_REQUIRED
	},
	{ /* 4 */
		.lib_handler_fn				= message_handler_req_lib_cpg_trackstart,
		.response_size				= sizeof (struct res_lib_cpg_trackstart),
		.response_id				= MESSAGE_RES_CPG_TRACKSTART,
		.flow_control				= OPENAIS_FLOW_CONTROL_NOT_REQUIRED
	},
	{ /* 5 */
		.lib_handler_fn				= message_handler_req_lib_cpg_trackstop,
		.response_size				= sizeof (struct res_lib_cpg_trackstart),
		.response_id				= MESSAGE_RES_CPG_TRACKSTOP,
		.flow_control				= OPENAIS_FLOW_CONTROL_NOT_REQUIRED
	}
};

static struct openais_exec_handler cpg_exec_handlers[] =
{
	{ /* 0 */
		.exec_handler_fn	= message_handler_req_exec_cpg_procjoin,
		.exec_endian_convert_fn	= exec_cpg_procjoin_endian_convert
	},
	{ /* 1 */
		.exec_handler_fn	= message_handler_req_exec_cpg_procleave,
		.exec_endian_convert_fn	= exec_cpg_procjoin_endian_convert
	},
	{ /* 2 */
		.exec_handler_fn	= message_handler_req_exec_cpg_joinlist,
		.exec_endian_convert_fn	= exec_cpg_joinlist_endian_convert
	},
	{ /* 3 */
		.exec_handler_fn	= message_handler_req_exec_cpg_mcast,
		.exec_endian_convert_fn	= exec_cpg_mcast_endian_convert
	},
};

struct openais_service_handler cpg_service_handler = {
	.name				        = (unsigned char*)"openais cluster closed process group service v1.01",
	.id					= CPG_SERVICE,
	.private_data_size			= sizeof (struct process_info),
	.lib_init_fn				= cpg_lib_init_fn,
	.lib_exit_fn				= cpg_lib_exit_fn,
	.lib_handlers				= cpg_lib_handlers,
	.lib_handlers_count			= sizeof (cpg_lib_handlers) / sizeof (struct openais_lib_handler),
	.exec_init_fn				= cpg_exec_init_fn,
	.exec_dump_fn				= NULL,
	.exec_handlers				= cpg_exec_handlers,
	.exec_handlers_count		        = sizeof (cpg_exec_handlers) / sizeof (struct openais_exec_handler),
	.confchg_fn                             = cpg_confchg_fn,
};

/*
 * Dynamic loader definition
 */
struct openais_service_handler *cpg_get_service_handler_ver0 (void);

struct openais_service_handler_iface_ver0 cpg_service_handler_iface = {
	.openais_get_service_handler_ver0		= cpg_get_service_handler_ver0
};

struct lcr_iface openais_cpg_ver0[1] = {
	{
		.name				= "openais_cpg",
		.version			= 0,
		.versions_replace		= 0,
		.versions_replace_count         = 0,
		.dependencies			= 0,
		.dependency_count		= 0,
		.constructor			= NULL,
		.destructor			= NULL,
		.interfaces			= (void **)&cpg_service_handler_iface,
	}
};

struct lcr_comp cpg_comp_ver0 = {
	.iface_count			= 1,
	.ifaces			        = openais_cpg_ver0
};


struct openais_service_handler *cpg_get_service_handler_ver0 (void)
{
	return (&cpg_service_handler);
}

__attribute__ ((constructor)) static void cpg_comp_register (void) {
	lcr_component_register (&cpg_comp_ver0);
}

struct req_exec_cpg_procjoin {
	struct req_header header;
	struct cpg_name groupName;
	uint32_t pid;
	uint32_t reason;
};

struct req_exec_cpg_mcast {
	struct req_header header;
	struct cpg_name groupName;
	uint32_t msglen;
	uint32_t pid;
	char message[];
};

static int notify_lib_joinlist(struct group_info *gi, void *conn,
			       int joined_list_entries, struct cpg_groupinfo *joined_list,
			       int left_list_entries, struct cpg_groupinfo *left_list,
			       int id)
{
	int count = 0;
	char *buf;
	struct res_lib_cpg_confchg_callback *res;
	struct list_head *iter;
	struct list_head *tmp;
	struct cpg_groupinfo *retgi;
	int size;

	/* First, we need to know how many nodes are in the list. While we're
	   traversing this list, look for the 'us' entry so we knw which
	   connection to send back down */
	for (iter = gi->members.next; iter != &gi->members; iter = iter->next) {
		struct process_info *pi = list_entry(iter, struct process_info, list);
		if (pi->pid)
			count++;
	}

	log_printf(LOG_LEVEL_DEBUG, "Sending new joinlist (%d elements) to clients\n", count);

	size = sizeof(struct res_lib_cpg_confchg_callback) +
		sizeof(struct cpg_groupinfo) * (count + left_list_entries + joined_list_entries);
	buf = alloca(size);
	if (!buf)
		return SA_AIS_ERR_NO_SPACE;

	res = (struct res_lib_cpg_confchg_callback *)buf;
	res->joined_list_entries = joined_list_entries;
	res->left_list_entries = left_list_entries;
 	retgi = res->member_list;

	res->header.size = size;
	res->header.id = id;
	memcpy(&res->groupName, &gi->groupName, sizeof(struct cpg_name));

	/* Build up the message */
	count = 0;
	for (iter = gi->members.next; iter != &gi->members; iter = iter->next) {
		struct process_info *pi = list_entry(iter, struct process_info, list);
		if (pi->pid) {
			/* Processes leaving will be removed AFTER this is done (so that they get their
			   own leave notifications), so exclude them from the members list here */
			int i;
			for (i=0; i<left_list_entries; i++) {
				if (left_list[i].pid == pi->pid && left_list[i].nodeId == pi->node.nodeid)
					goto next_member;
			}

			retgi->nodeId = pi->node.nodeid;
			retgi->pid = pi->pid;
			retgi++;
			count++;
		next_member: ;
		}
	}
	res->member_list_entries = count;

	if (left_list_entries) {
		memcpy(retgi, left_list, left_list_entries * sizeof(struct cpg_groupinfo));
		retgi += left_list_entries;
	}

	if (joined_list_entries) {
		memcpy(retgi, joined_list, joined_list_entries * sizeof(struct cpg_groupinfo));
		retgi += joined_list_entries;
	}

	if (conn) {
		openais_conn_send_response(conn, buf, size);
	}
	else {
		/* Send it to all listeners */
		for (iter = gi->members.next, tmp=iter->next; iter != &gi->members; iter = tmp, tmp=iter->next) {
			struct process_info *pi = list_entry(iter, struct process_info, list);
			if (pi->trackerconn && (pi->flags & PI_FLAG_MEMBER)) {
				if (openais_conn_send_response(pi->trackerconn, buf, size) == -1) {
					// Error ??
				}
			}
		}
	}

	return SA_AIS_OK;
}

static void remove_group(struct group_info *gi)
{
	list_del(&gi->list);
	free(gi);
}


static int cpg_exec_init_fn (struct openais_config *openais_config)
{
	int i;

	for (i=0; i<GROUP_HASH_SIZE; i++) {
		list_init(&group_lists[i]);
	}

	return (0);
}

static int cpg_lib_exit_fn (void *conn)
{
	struct process_info *pi = (struct process_info *)openais_conn_private_data_get (conn);
	struct group_info *gi = pi->group;
	struct cpg_groupinfo notify_info;

	log_printf(LOG_LEVEL_DEBUG, "exit_fn for conn=%p\n", conn);

	if (gi) {
		notify_info.pid = pi->pid;
		notify_info.nodeId = this_ip->nodeid;
		notify_info.reason = CONFCHG_CPG_REASON_PROCDOWN;
		cpg_node_joinleave_send(gi, pi, MESSAGE_REQ_EXEC_CPG_PROCLEAVE, CONFCHG_CPG_REASON_PROCDOWN);
		list_del(&pi->list);
	}
	return (0);
}

static struct group_info *get_group(struct cpg_name *name)
{
	struct list_head *iter;
	struct group_info *gi = NULL;
	uint32_t hash = jhash(name->value, name->length, 0) % GROUP_HASH_SIZE;

	for (iter = group_lists[hash].next; iter != &group_lists[hash]; iter = iter->next) {
		gi = list_entry(iter, struct group_info, list);
		if (memcmp(gi->groupName.value, name->value, name->length) == 0)
			break;
	}

	if (!gi) {
		gi = malloc(sizeof(struct group_info));
		if (!gi) {
			log_printf(LOG_LEVEL_WARNING, "Unable to allocate group_info struct");
			return NULL;
		}
		memcpy(&gi->groupName, name, sizeof(struct cpg_name));
		gi->rg = NULL;
		list_init(&gi->members);
		list_add(&gi->list, &group_lists[hash]);
	}
	return gi;
}

static int cpg_node_joinleave_send (struct group_info *gi, struct process_info *pi, int fn, int reason)
{
	struct req_exec_cpg_procjoin req_exec_cpg_procjoin;
	struct iovec req_exec_cpg_iovec;
	int result;

	memcpy(&req_exec_cpg_procjoin.groupName, &gi->groupName, sizeof(struct cpg_name));
	req_exec_cpg_procjoin.pid = pi->pid;
	req_exec_cpg_procjoin.reason = reason;

	req_exec_cpg_procjoin.header.size = sizeof(req_exec_cpg_procjoin);
	req_exec_cpg_procjoin.header.id = SERVICE_ID_MAKE(CPG_SERVICE, fn);

	req_exec_cpg_iovec.iov_base = &req_exec_cpg_procjoin;
	req_exec_cpg_iovec.iov_len = sizeof(req_exec_cpg_procjoin);

	result = totempg_groups_mcast_joined (openais_group_handle, &req_exec_cpg_iovec, 1, TOTEMPG_AGREED);

	return (result);
}

static void remove_node_from_groups(struct totem_ip_address *node, struct list_head *remlist)
{
	int i;
	struct list_head *iter, *iter2, *tmp;
	struct process_info *pi;
	struct group_info *gi;

	for (i=0; i < GROUP_HASH_SIZE; i++) {
		for (iter = group_lists[i].next, tmp=iter->next; iter != &group_lists[i]; iter = tmp, tmp=iter->next) {
			gi = list_entry(iter, struct group_info, list);
			for (iter2 = gi->members.next; iter2 != &gi->members; iter2 = iter2->next) {
				pi = list_entry(iter2, struct process_info, list);

				if (totemip_equal(&pi->node, node)) {

					/* Add it to the list of nodes to send notifications for */
					if (!gi->rg) {
						gi->rg = malloc(sizeof(struct removed_group));
						if (gi->rg) {
							list_add(&gi->rg->list, remlist);
							gi->rg->gi = gi;
							gi->rg->left_list_entries = 0;
						}
						gi->rg->left_list[gi->rg->left_list_entries].pid = pi->pid;
						gi->rg->left_list[gi->rg->left_list_entries].nodeId = pi->node.nodeid;
						gi->rg->left_list[gi->rg->left_list_entries].reason = CONFCHG_CPG_REASON_NODEDOWN;
						gi->rg->left_list_entries++;

						/* Remove node info for dead node */
						list_del(&pi->list);
						free(pi);
					}
				}
			}
		}
	}
}


static void cpg_confchg_fn (
	enum totem_configuration_type configuration_type,
	struct totem_ip_address *member_list, int member_list_entries,
	struct totem_ip_address *left_list, int left_list_entries,
	struct totem_ip_address *joined_list, int joined_list_entries,
	struct memb_ring_id *ring_id)
{

	int i;
	struct list_head removed_list;

	log_printf(LOG_LEVEL_DEBUG, "confchg. joined_list: %d, left_list: %d\n", joined_list_entries, left_list_entries);

	list_init(&removed_list);

	/* Tell any newly joined nodes our list of joined groups */
	if (configuration_type == TOTEM_CONFIGURATION_REGULAR) {
		cpg_exec_send_joinlist();
	}

	/* Remove nodes from joined groups and add removed groups to the list */
	for (i = 0; i < left_list_entries; i++) {
		remove_node_from_groups(&left_list[i], &removed_list);
	}

	if (!list_empty(&removed_list)) {
		struct list_head *iter;

		for (iter = removed_list.next; iter != &removed_list; iter = iter->next) {
			struct removed_group *rg = list_entry(iter, struct removed_group, list);

			notify_lib_joinlist(rg->gi, NULL,
					    0, NULL,
					    rg->left_list_entries, rg->left_list,
					    MESSAGE_RES_CPG_CONFCHG_CALLBACK);
			rg->gi->rg = NULL;
			free(rg);
		}
	}
}

/* Can byteswap join & leave messages */
static void exec_cpg_procjoin_endian_convert (void *msg)
{
	struct req_exec_cpg_procjoin *req_exec_cpg_procjoin = (struct req_exec_cpg_procjoin *)msg;

	req_exec_cpg_procjoin->pid = swab32(req_exec_cpg_procjoin->pid);
	req_exec_cpg_procjoin->groupName.length = swab32(req_exec_cpg_procjoin->groupName.length);
}

static void exec_cpg_joinlist_endian_convert (void *msg)
{
	struct res_header *res = (struct res_header *)msg;
	struct join_list_entry *jle = (struct join_list_entry *)(msg + sizeof(struct res_header));

	while ((void*)jle < msg + res->size) {
		jle->pid = swab32(jle->pid);
		jle->groupName.length = swab32(jle->groupName.length);
		jle++;
	}
}

static void exec_cpg_mcast_endian_convert (void *msg)
{
	struct req_exec_cpg_mcast *req_exec_cpg_mcast = (struct req_exec_cpg_mcast *)msg;

	req_exec_cpg_mcast->pid = swab32(req_exec_cpg_mcast->pid);
	req_exec_cpg_mcast->msglen = swab32(req_exec_cpg_mcast->msglen);
	req_exec_cpg_mcast->groupName.length = swab32(req_exec_cpg_mcast->groupName.length);

}

static void do_proc_join(struct cpg_name *name, uint32_t pid, struct totem_ip_address *node, int reason)
{
	struct group_info *gi;
	struct process_info *pi;
	struct list_head *iter;
	struct cpg_groupinfo notify_info;

	gi = get_group(name); /* this will always succeed ! */
	assert(gi);

	/* See if it already exists in this group */
	for (iter = gi->members.next; iter != &gi->members; iter = iter->next) {
		pi = list_entry(iter, struct process_info, list);
		if (pi->pid == pid && pi->node.nodeid == node->nodeid) {

			/* It could be a local join message */
			if (totemip_equal(node, this_ip) && (!pi->flags & PI_FLAG_MEMBER))
				goto local_join;
			else
				return;
		}
	}

	pi = malloc(sizeof(struct process_info));
	if (!pi) {
		log_printf(LOG_LEVEL_WARNING, "Unable to allocate process_info struct");
		return;
	}
	totemip_copy(&pi->node, node);
	pi->pid = pid;
	pi->group = gi;
	pi->conn = NULL;
	pi->trackerconn = NULL;
	list_add_tail(&pi->list, &gi->members);

local_join:

	pi->flags = PI_FLAG_MEMBER;
	notify_info.pid = pi->pid;
	notify_info.nodeId = node->nodeid;
	notify_info.reason = reason;

	notify_lib_joinlist(gi, NULL,
			    1, &notify_info,
			    0, NULL,
			    MESSAGE_RES_CPG_CONFCHG_CALLBACK);
}

static void message_handler_req_exec_cpg_procjoin (
	void *message,
	struct totem_ip_address *source_addr)
{
	struct req_exec_cpg_procjoin *req_exec_cpg_procjoin = (struct req_exec_cpg_procjoin *)message;

	log_printf(LOG_LEVEL_DEBUG, "got procjoin message from cluster\n");

	do_proc_join(&req_exec_cpg_procjoin->groupName, req_exec_cpg_procjoin->pid, source_addr, CONFCHG_CPG_REASON_JOIN);
}

static void message_handler_req_exec_cpg_procleave (
	void *message,
	struct totem_ip_address *source_addr)
{
	struct req_exec_cpg_procjoin *req_exec_cpg_procjoin = (struct req_exec_cpg_procjoin *)message;
	struct group_info *gi;
	struct process_info *pi;
	struct list_head *iter;
	struct cpg_groupinfo notify_info;

	log_printf(LOG_LEVEL_DEBUG, "got procleave message from cluster\n");

	gi = get_group(&req_exec_cpg_procjoin->groupName); /* this will always succeed ! */
	assert(gi);

	notify_info.pid = req_exec_cpg_procjoin->pid;
	notify_info.nodeId = source_addr->nodeid;
	notify_info.reason = req_exec_cpg_procjoin->reason;

	notify_lib_joinlist(gi, NULL,
			    0, NULL,
			    1, &notify_info,
			    MESSAGE_RES_CPG_CONFCHG_CALLBACK);

        /* Find the node/PID to remove */
	for (iter = gi->members.next; iter != &gi->members; iter = iter->next) {
		pi = list_entry(iter, struct process_info, list);
		if (pi->pid == req_exec_cpg_procjoin->pid && totemip_equal(&pi->node, source_addr)) {

			list_del(&pi->list);
			if (!pi->conn)
				free(pi);

			if (list_empty(&gi->members)) {
				remove_group(gi);
			}
			break;
		}
	}
}


/* Got a proclist from another node */
static void message_handler_req_exec_cpg_joinlist (
	void *message,
	struct totem_ip_address *source_addr)
{
	struct res_header *res = (struct res_header *)message;
	struct join_list_entry *jle = (struct join_list_entry *)(message + sizeof(struct res_header));

	log_printf(LOG_LEVEL_NOTICE, "got joinlist message from node %s\n", totemip_print(source_addr));

	/* Ignore our own messages */
	if (totemip_equal(source_addr, this_ip))
		return;

	while ((void*)jle < message + res->size) {
		do_proc_join(&jle->groupName, jle->pid, source_addr, CONFCHG_CPG_REASON_NODEUP);
		jle++;
	}
}

static void message_handler_req_exec_cpg_mcast (
	void *message,
	struct totem_ip_address *source_addr)
{
	struct req_exec_cpg_mcast *req_exec_cpg_mcast = (struct req_exec_cpg_mcast *)message;
	struct res_lib_cpg_deliver_callback *res_lib_cpg_mcast;
	int msglen = req_exec_cpg_mcast->msglen;
	char buf[sizeof(*res_lib_cpg_mcast) + msglen];
	struct group_info *gi;
	struct list_head *iter;

	gi = get_group(&req_exec_cpg_mcast->groupName); /* this will always succeed ! */
	assert(gi);

	res_lib_cpg_mcast = (struct res_lib_cpg_deliver_callback *)buf;
	res_lib_cpg_mcast->header.id = MESSAGE_RES_CPG_DELIVER_CALLBACK;
	res_lib_cpg_mcast->header.size = sizeof(*res_lib_cpg_mcast) + msglen;
	res_lib_cpg_mcast->msglen = msglen;
	res_lib_cpg_mcast->pid = req_exec_cpg_mcast->pid;
	res_lib_cpg_mcast->nodeid = source_addr->nodeid;
	memcpy(&res_lib_cpg_mcast->groupName, &gi->groupName, sizeof(struct cpg_name));
	memcpy(&res_lib_cpg_mcast->message, req_exec_cpg_mcast->message, msglen);

	/* Send to all interested members */
	for (iter = gi->members.next; iter != &gi->members; iter = iter->next) {
		struct process_info *pi = list_entry(iter, struct process_info, list);
		if (pi->trackerconn) {
			openais_conn_send_response(pi->trackerconn, buf, res_lib_cpg_mcast->header.size);
		}
	}
}


static void cpg_exec_send_joinlist()
{
	int count = 0;
	char *buf;
	int i;
	struct list_head *iter;
	struct list_head *iter2;
	struct group_info *gi;
	struct res_header *res;
	struct join_list_entry *jle;
	struct iovec req_exec_cpg_iovec;

	log_printf(LOG_LEVEL_DEBUG, "sending joinlist to cluster\n");

	/* Count the number of groups we are a member of */
	for (i=0; i<GROUP_HASH_SIZE; i++) {
		for (iter = group_lists[i].next; iter != &group_lists[i]; iter = iter->next) {
			gi = list_entry(iter, struct group_info, list);
			for (iter2 = gi->members.next; iter2 != &gi->members; iter2 = iter2->next) {
				struct process_info *pi = list_entry(iter2, struct process_info, list);
				if (pi->pid && pi->node.nodeid == this_ip->nodeid) {
					count++;
				}
			}
		}
	}

	/* Nothing to send */
	if (!count)
		return;

	buf = alloca(sizeof(struct res_header) + sizeof(struct join_list_entry) * count);
	if (!buf) {
		log_printf(LOG_LEVEL_WARNING, "Unable to allocate joinlist buffer");
		return;
	}

	jle = (struct join_list_entry *)(buf + sizeof(struct res_header));
	res = (struct res_header *)buf;

	for (i=0; i<GROUP_HASH_SIZE; i++) {
		for (iter = group_lists[i].next; iter != &group_lists[i]; iter = iter->next) {

			gi = list_entry(iter, struct group_info, list);
			for (iter2 = gi->members.next; iter2 != &gi->members; iter2 = iter2->next) {

				struct process_info *pi = list_entry(iter2, struct process_info, list);
				if (pi->pid && pi->node.nodeid == this_ip->nodeid) {
					memcpy(&jle->groupName, &gi->groupName, sizeof(struct cpg_name));
					jle->pid = pi->pid;
					jle++;
				}
			}
		}
	}

	res->id = SERVICE_ID_MAKE(CPG_SERVICE, MESSAGE_REQ_EXEC_CPG_JOINLIST);
	res->size = sizeof(struct res_header)+sizeof(struct join_list_entry) * count;

	req_exec_cpg_iovec.iov_base = buf;
	req_exec_cpg_iovec.iov_len = res->size;

	totempg_groups_mcast_joined (openais_group_handle, &req_exec_cpg_iovec, 1, TOTEMPG_AGREED);
}

static int cpg_lib_init_fn (void *conn)
{
	struct process_info *pi = (struct process_info *)openais_conn_private_data_get (conn);
	pi->conn = conn;

	log_printf(LOG_LEVEL_DEBUG, "lib_init_fn: conn=%p, pi=%p\n", conn, pi);
	return (0);
}

/* Join message from the library */
static void message_handler_req_lib_cpg_join (void *conn, void *message)
{
	struct req_lib_cpg_join *req_lib_cpg_join = (struct req_lib_cpg_join *)message;
	struct process_info *pi = (struct process_info *)openais_conn_private_data_get (conn);
	struct res_lib_cpg_join res_lib_cpg_join;
	struct group_info *gi;
	SaAisErrorT error = SA_AIS_OK;

	log_printf(LOG_LEVEL_DEBUG, "got join request on %p, pi=%p, pi->pid=%d\n", conn, pi, pi->pid);

	/* Already joined on this conn */
	if (pi->pid) {
		error = SA_AIS_ERR_INVALID_PARAM;
		goto join_err;
	}

	gi = get_group(&req_lib_cpg_join->groupName);
	if (!gi) {
		error = SA_AIS_ERR_NO_SPACE;
		goto join_err;
	}

	/* Add a node entry for us */
	totemip_copy(&pi->node, this_ip);
	pi->pid = req_lib_cpg_join->pid;
	pi->group = gi;
	list_add(&pi->list, &gi->members);

	/* Tell the rest of the cluster */
	cpg_node_joinleave_send(gi, pi, MESSAGE_REQ_EXEC_CPG_PROCJOIN, CONFCHG_CPG_REASON_JOIN);

join_err:
	res_lib_cpg_join.header.size = sizeof(res_lib_cpg_join);
	res_lib_cpg_join.header.id = MESSAGE_RES_CPG_JOIN;
	res_lib_cpg_join.header.error = error;
	openais_conn_send_response(conn, &res_lib_cpg_join, sizeof(res_lib_cpg_join));
}

/* Leave message from the library */
static void message_handler_req_lib_cpg_leave (void *conn, void *message)
{
	struct process_info *pi = (struct process_info *)openais_conn_private_data_get (conn);
	struct res_lib_cpg_leave res_lib_cpg_leave;
	struct group_info *gi;
	SaAisErrorT error = SA_AIS_OK;

	log_printf(LOG_LEVEL_DEBUG, "got leave request on %p\n", conn);

	if (!pi || !pi->pid || !pi->group) {
		error = SA_AIS_ERR_INVALID_PARAM;
		goto leave_ret;
	}
	gi = pi->group;

	/* Tell other nodes we are leaving.
	   When we get this message back we will leave too */
	cpg_node_joinleave_send(gi, pi, MESSAGE_REQ_EXEC_CPG_PROCLEAVE, CONFCHG_CPG_REASON_LEAVE);
	pi->group = NULL;

leave_ret:
	/* send return */
	res_lib_cpg_leave.header.size = sizeof(res_lib_cpg_leave);
	res_lib_cpg_leave.header.id = MESSAGE_RES_CPG_LEAVE;
	res_lib_cpg_leave.header.error = error;
	openais_conn_send_response(conn, &res_lib_cpg_leave, sizeof(res_lib_cpg_leave));
}

/* Mcast message from the library */
static void message_handler_req_lib_cpg_mcast (void *conn, void *message)
{
	struct req_lib_cpg_mcast *req_lib_cpg_mcast = (struct req_lib_cpg_mcast *)message;
	struct process_info *pi = (struct process_info *)openais_conn_private_data_get (conn);
	struct group_info *gi = pi->group;
	struct iovec req_exec_cpg_iovec[2];
	struct req_exec_cpg_mcast req_exec_cpg_mcast;
	struct res_header res;
	int msglen = req_lib_cpg_mcast->msglen;
	int result;

	log_printf(LOG_LEVEL_DEBUG, "got mcast request on %p\n", conn);

	/* Can't send if we're not joined */
	if (!gi) {
		res.size = sizeof(res);
		res.id = MESSAGE_RES_CPG_MCAST;
		res.error = SA_AIS_ERR_ACCESS; /* TODO Better error code ?? */
		openais_conn_send_response(conn, &res, sizeof(res));
		return;
	}

	req_exec_cpg_mcast.header.size = sizeof(req_exec_cpg_mcast) + msglen;
	req_exec_cpg_mcast.header.id = SERVICE_ID_MAKE(CPG_SERVICE, MESSAGE_REQ_EXEC_CPG_MCAST);
	req_exec_cpg_mcast.pid = pi->pid;
	req_exec_cpg_mcast.msglen = msglen;
	memcpy(&req_exec_cpg_mcast.groupName, &gi->groupName, sizeof(struct cpg_name));

	req_exec_cpg_iovec[0].iov_base = &req_exec_cpg_mcast;
	req_exec_cpg_iovec[0].iov_len = sizeof(req_exec_cpg_mcast);
	req_exec_cpg_iovec[1].iov_base = &req_lib_cpg_mcast->message;
	req_exec_cpg_iovec[1].iov_len = msglen;

	// TODO: guarantee type...
	result = totempg_groups_mcast_joined (openais_group_handle, req_exec_cpg_iovec, 2, TOTEMPG_AGREED);

	res.size = sizeof(res);
	res.id = MESSAGE_RES_CPG_MCAST;
	res.error = SA_AIS_OK;
	openais_conn_send_response(conn, &res, sizeof(res));
}

static void message_handler_req_lib_cpg_membership (void *conn, void *message)
{
	struct process_info *pi = (struct process_info *)openais_conn_private_data_get (conn);

	log_printf(LOG_LEVEL_DEBUG, "got membership request on %p\n", conn);
	if (!pi->group) {
		struct res_header res;
		res.size = sizeof(res);
		res.id = MESSAGE_RES_CPG_MEMBERSHIP;
		res.error = SA_AIS_ERR_ACCESS; /* TODO Better error code */
		openais_conn_send_response(conn, &res, sizeof(res));
		return;
	}

	notify_lib_joinlist(pi->group, conn, 0, NULL, 0, NULL, MESSAGE_RES_CPG_MEMBERSHIP);
}


static void message_handler_req_lib_cpg_trackstart (void *conn, void *message)
{
	struct req_lib_cpg_trackstart *req_lib_cpg_trackstart = (struct req_lib_cpg_trackstart *)message;
	struct res_lib_cpg_trackstart res_lib_cpg_trackstart;
	struct group_info *gi;
	struct process_info *otherpi;
	void *otherconn;
	SaAisErrorT error = SA_AIS_OK;

	log_printf(LOG_LEVEL_DEBUG, "got trackstart request on %p\n", conn);

	gi = get_group(&req_lib_cpg_trackstart->groupName);
	if (!gi) {
		error = SA_AIS_ERR_NO_SPACE;
		goto tstart_ret;
	}

	/* Find the partner connection and add us to it's process_info struct */
	otherconn = openais_conn_partner_get (conn);
	otherpi = (struct process_info *)openais_conn_private_data_get (conn);
	otherpi->trackerconn = conn;

tstart_ret:
	res_lib_cpg_trackstart.header.size = sizeof(res_lib_cpg_trackstart);
	res_lib_cpg_trackstart.header.id = MESSAGE_RES_CPG_TRACKSTART;
	res_lib_cpg_trackstart.header.error = SA_AIS_OK;
	openais_conn_send_response(conn, &res_lib_cpg_trackstart, sizeof(res_lib_cpg_trackstart));
}

static void message_handler_req_lib_cpg_trackstop (void *conn, void *message)
{
	struct req_lib_cpg_trackstop *req_lib_cpg_trackstop = (struct req_lib_cpg_trackstop *)message;
	struct res_lib_cpg_trackstop res_lib_cpg_trackstop;
	struct process_info *otherpi;
	void *otherconn;
	struct group_info *gi;
	SaAisErrorT error = SA_AIS_OK;

	log_printf(LOG_LEVEL_DEBUG, "got trackstop request on %p\n", conn);

	gi = get_group(&req_lib_cpg_trackstop->groupName);
	if (!gi) {
		error = SA_AIS_ERR_NO_SPACE;
		goto tstop_ret;
	}

	/* Find the partner connection and add us to it's process_info struct */
	otherconn = openais_conn_partner_get (conn);
	otherpi = (struct process_info *)openais_conn_private_data_get (conn);
	otherpi->trackerconn = NULL;

tstop_ret:
	res_lib_cpg_trackstop.header.size = sizeof(res_lib_cpg_trackstop);
	res_lib_cpg_trackstop.header.id = MESSAGE_RES_CPG_TRACKSTOP;
	res_lib_cpg_trackstop.header.error = SA_AIS_OK;
	openais_conn_send_response(conn, &res_lib_cpg_trackstop.header, sizeof(res_lib_cpg_trackstop));
}