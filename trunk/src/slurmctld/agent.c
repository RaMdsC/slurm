/*****************************************************************************\
 *  agent.c - parallel background communication functions. This is where  
 *	logic could be placed for broadcast communications.
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Moe Jette <jette@llnl.gov>, et. al.
 *  Derived from pdsh written by Jim Garlick <garlick1@llnl.gov>
 *  UCRL-CODE-2002-040.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 *****************************************************************************
 *  Theory of operation:
 *
 *  The functions below permit slurm to initiate parallel tasks as a 
 *  detached thread and let the functions below make sure the work happens. 
 *  For example, when a job step completes slurmctld needs to revoke credentials 
 *  for that job step on every node to which it was allocated. We don't want to 
 *  hang slurmctld's primary function (the job complete RPC) to perform this 
 *  work, so it just initiates an agent to perform the work. The agent is passed 
 *  all details required to perform the work, so it will be possible to execute
 *  the agent as an pthread, process, or even a daemon on some other computer.
 *
 *  The main agent thread creates a separate thread for each node to be
 *  communicated with up to AGENT_THREAD_COUNT. A special watchdog thread sends 
 *  SIGLARM to any threads that have been active (in DSH_ACTIVE state) for more 
 *  than COMMAND_TIMEOUT seconds. 
 *  The agent responds to slurmctld via an RPC as required.
 *  For example, informing slurmctld that some node is not responding.
 *
 *  All the state for each thread is maintailed in thd_t struct, which is 
 *  used by the watchdog thread as well as the communication threads.
\*****************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <src/common/log.h>
#include <src/common/slurm_protocol_defs.h>
#include <src/common/xmalloc.h>
#include <src/common/xstring.h>
#include <src/slurmctld/agent.h>
#include <src/slurmctld/locks.h>

#if COMMAND_TIMEOUT == 1
#define WDOG_POLL 		1	/* secs */
#else
#define WDOG_POLL 		2	/* secs */
#endif

typedef enum {DSH_NEW, DSH_ACTIVE, DSH_DONE, DSH_FAILED} state_t;

typedef struct thd {
        pthread_t	thread;			/* thread ID */
        pthread_attr_t	attr;			/* thread attributes */
        state_t		state;      		/* thread state */
        time_t 		time;   		/* time stamp for start or delta time */
	struct sockaddr_in slurm_addr;		/* network address */
	char		node_name[MAX_NAME_LEN];/* node's name */
} thd_t;

typedef struct agent_info {
	pthread_mutex_t	thread_mutex;		/* agent specific mutex */
	pthread_cond_t	thread_cond;		/* agent specific condition */
	uint32_t	thread_count;		/* number of threads records */
	uint32_t	threads_active;		/* count of currently active threads */
	thd_t 		*thread_struct;		/* thread structures */
	slurm_msg_type_t msg_type;		/* RPC to be issued */
	void		*msg_args;		/* RPC data to be used */
} agent_info_t;

typedef struct task_info {
	pthread_mutex_t	*thread_mutex_ptr;	/* pointer to agent specific mutex */
	pthread_cond_t	*thread_cond_ptr;	/* pointer to agent specific condition */
	uint32_t	*threads_active_ptr;	/* pointer to count of currently active threads */
	thd_t 		*thread_struct_ptr;	/* pointer to thread structures */
	slurm_msg_type_t msg_type;		/* RPC to be issued */
	void		*msg_args_ptr;		/* pointer to RPC data to be used */
} task_info_t;

static void alarm_handler(int dummy);
static void *thread_per_node_rpc (void *args);
static void *wdog (void *args);
static void xsignal(int signal, void (*handler)(int));

/*
 * agent - party responsible for transmitting an common RPC in parallel across a set 
 *	of nodes
 * input: pointer to agent_arg_t, which is xfree'd (including slurm_addr, node_names, 
 *	and msg_args) upon completion if AGENT_IS_THREAD is set
 */
void *
agent (void *args)
{
	int i, rc;
	pthread_attr_t attr_wdog;
	pthread_t thread_wdog;
	agent_arg_t *agent_arg_ptr = args;
	agent_info_t *agent_info_ptr = NULL;
	thd_t *thread_ptr;
	task_info_t *task_specific_ptr;

	/* basic argument value tests */
	if (agent_arg_ptr == NULL)
		fatal ("agent NULL argument");
	if (agent_arg_ptr->addr_count == 0)
		goto cleanup;	/* no messages to be sent */
	if (agent_arg_ptr->slurm_addr == NULL)
		fatal ("agent passed NULL address list");
	if (agent_arg_ptr->node_names == NULL)
		fatal ("agent passed NULL node name list");
	if ((agent_arg_ptr->msg_type != REQUEST_REVOKE_JOB_CREDENTIAL) &&
	    (agent_arg_ptr->msg_type != REQUEST_NODE_REGISTRATION_STATUS) &&
	    (agent_arg_ptr->msg_type != REQUEST_PING))
		fatal ("agent passed invalid message type %d", agent_arg_ptr->msg_type);

	/* initialize the data structures */
	agent_info_ptr = xmalloc (sizeof (agent_info_t));
	if (pthread_mutex_init (&agent_info_ptr->thread_mutex, NULL))
		fatal (" pthread_mutex_init error %m");
	if (pthread_cond_init (&agent_info_ptr->thread_cond, NULL))
		fatal ("pthread_cond_init error %m");
	agent_info_ptr->thread_count = agent_arg_ptr->addr_count;
	agent_info_ptr->threads_active = 0;
	thread_ptr = xmalloc (agent_arg_ptr->addr_count * sizeof (thd_t));
	agent_info_ptr->thread_struct = thread_ptr;
	agent_info_ptr->msg_type = agent_arg_ptr->msg_type;
	agent_info_ptr->msg_args = agent_arg_ptr->msg_args;
	for (i = 0; i < agent_info_ptr->thread_count; i++) {
		thread_ptr[i].state = DSH_NEW;
		thread_ptr[i].slurm_addr = agent_arg_ptr->slurm_addr[i];
		strncpy (thread_ptr[i].node_name,
		         &agent_arg_ptr->node_names[i*MAX_NAME_LEN], MAX_NAME_LEN);
	}

	/* start the watchdog thread */
	if (pthread_attr_init (&attr_wdog))
		fatal ("pthread_attr_init error %m");
	if (pthread_attr_setdetachstate (&attr_wdog, PTHREAD_CREATE_JOINABLE))
		error ("pthread_attr_setdetachstate error %m");
#ifdef PTHREAD_SCOPE_SYSTEM
	if (pthread_attr_setscope (&attr_wdog, PTHREAD_SCOPE_SYSTEM))
		error ("pthread_attr_setscope error %m");
#endif
	if (pthread_create (&thread_wdog, &attr_wdog, wdog, (void *)agent_info_ptr)) {
		error ("pthread_create error %m");
		sleep (1); /* sleep and try once more */
		if (pthread_create (&thread_wdog, &attr_wdog, wdog, args))
			fatal ("pthread_create error %m");
	}

	/* start all the other threads (up to AGENT_THREAD_COUNT active at once) */
	for (i = 0; i < agent_info_ptr->thread_count; i++) {
		
		/* wait until "room" for another thread */	
		pthread_mutex_lock (&agent_info_ptr->thread_mutex);
#if 		AGENT_THREAD_COUNT < 1
		fatal ("AGENT_THREAD_COUNT value is invalid");
#endif
     		while (agent_info_ptr->threads_active >= AGENT_THREAD_COUNT) {
			pthread_cond_wait (&agent_info_ptr->thread_cond, 
			                   &agent_info_ptr->thread_mutex);
		}
 
		/* create thread, note this is freed from  thread_per_node_rpc() */
		task_specific_ptr 			= xmalloc (sizeof (task_info_t));
		task_specific_ptr->thread_mutex_ptr	= &agent_info_ptr->thread_mutex;
		task_specific_ptr->thread_cond_ptr	= &agent_info_ptr->thread_cond;
		task_specific_ptr->threads_active_ptr	= &agent_info_ptr->threads_active;
		task_specific_ptr->thread_struct_ptr	= &thread_ptr[i];
		task_specific_ptr->msg_type		= agent_info_ptr->msg_type;
		task_specific_ptr->msg_args_ptr		= &agent_info_ptr->msg_args;

		if (pthread_attr_init (&thread_ptr[i].attr))
			fatal ("pthread_attr_init error %m");
		if (pthread_attr_setdetachstate (&thread_ptr[i].attr, PTHREAD_CREATE_DETACHED))
			error ("pthread_attr_setdetachstate error %m");
#ifdef PTHREAD_SCOPE_SYSTEM
		if (pthread_attr_setscope (&thread_ptr[i].attr, PTHREAD_SCOPE_SYSTEM))
			error ("pthread_attr_setscope error %m");
#endif
		while ( (rc = pthread_create (&thread_ptr[i].thread, 
		                              &thread_ptr[i].attr, 
		                              thread_per_node_rpc, 
		                              (void *) task_specific_ptr)) ) {
			error ("pthread_create error %m");
			if (agent_info_ptr->threads_active)
				pthread_cond_wait (&agent_info_ptr->thread_cond, 
				                   &agent_info_ptr->thread_mutex);
			else {
				pthread_mutex_unlock (&agent_info_ptr->thread_mutex);
				sleep (1);
				pthread_mutex_lock (&agent_info_ptr->thread_mutex);
			}
		}

		agent_info_ptr->threads_active++;
		pthread_mutex_unlock (&agent_info_ptr->thread_mutex);
        }

	/* wait for termination of remaining threads */
	pthread_join (thread_wdog, NULL);

cleanup:
#if AGENT_IS_THREAD
	if (agent_arg_ptr) {
		if (agent_arg_ptr->slurm_addr)
			xfree (agent_arg_ptr->slurm_addr);
		if (agent_arg_ptr->node_names)
			xfree (agent_arg_ptr->node_names);
		if (agent_arg_ptr->msg_args)
			xfree (agent_arg_ptr->msg_args);
		xfree (agent_arg_ptr);
	}
#endif
	if (agent_info_ptr) {
		if (agent_info_ptr->thread_struct)
			xfree (agent_info_ptr->thread_struct);
		xfree (agent_info_ptr);
	}
	return NULL;
}

/* 
 * wdog - Watchdog thread. Send SIGALRM to threads which have been active for too long.
 *	Sleep for WDOG_POLL seconds between polls.
 */
static void *
wdog (void *args)
{
	int i, fail_cnt, work_done, delay, max_delay = 0;
	agent_info_t *agent_ptr = (agent_info_t *) args;
	thd_t *thread_ptr = agent_ptr->thread_struct;
#if AGENT_IS_THREAD
	/* Locks: Write job and write node */
	slurmctld_lock_t node_write_lock = { NO_LOCK, WRITE_LOCK, WRITE_LOCK, NO_LOCK };
#else
	int done_cnt;
	char *slurm_names;
#endif

	while (1) {
		work_done = 1;	/* assume all threads complete for now */
		fail_cnt = 0;	/* assume all threads complete sucessfully for now */
		sleep (WDOG_POLL);

		pthread_mutex_lock (&agent_ptr->thread_mutex);
		for (i = 0; i < agent_ptr->thread_count; i++) {
			switch (thread_ptr[i].state) {
				case DSH_ACTIVE:
					work_done = 0;
					delay = difftime (time (NULL), thread_ptr[i].time);
					if ( delay >= COMMAND_TIMEOUT)
						pthread_kill(thread_ptr[i].thread, SIGALRM);
					break;
				case DSH_NEW:
					work_done = 0;
					break;
				case DSH_DONE:
					if ( max_delay < (int) thread_ptr[i].time )
						max_delay = (int) thread_ptr[i].time;
					break;
				case DSH_FAILED:
					fail_cnt++;
					break;
			}
		}
		if (work_done)
			break;
		pthread_mutex_unlock (&agent_ptr->thread_mutex);
	}

	/* Notify slurmctld of non-responding nodes */
	if (fail_cnt) {
#if AGENT_IS_THREAD
		/* Update node table data for non-responding nodes */
		lock_slurmctld (node_write_lock);
		for (i = 0; i < agent_ptr->thread_count; i++) {
			if (thread_ptr[i].state == DSH_FAILED)
				node_not_resp (thread_ptr[i].node_name);
		}
		unlock_slurmctld (node_write_lock);
#else
		/* Build a list of all non-responding nodes and send it to slurmctld */
		slurm_names = xmalloc (fail_cnt * MAX_NAME_LEN);
		fail_cnt = 0;
		for (i = 0; i < agent_ptr->thread_count; i++) {
			if (thread_ptr[i].state == DSH_FAILED) {
				strncpy (&slurm_names[MAX_NAME_LEN * fail_cnt],
				         thread_ptr[i].node_name, MAX_NAME_LEN);
				error ("agent/wdog: node %s failed to respond", 
				       thread_ptr[i].node_name);
				fail_cnt++;
			}
		}

		/* send RPC */
		fatal ("Code development needed here if agent is not thread");

		xfree (slurm_names);
#endif
	}
#if AGENT_IS_THREAD
	/* Update last_response on responding nodes */
	lock_slurmctld (node_write_lock);
	for (i = 0; i < agent_ptr->thread_count; i++) {
		if (thread_ptr[i].state == DSH_DONE)
			node_did_resp (thread_ptr[i].node_name);
	}
	unlock_slurmctld (node_write_lock);
#else
	/* Build a list of all responding nodes and send it to slurmctld to update time stamps */
	done_cnt = agent_ptr->thread_count - fail_cnt;
	slurm_names = xmalloc (done_cnt * MAX_NAME_LEN);
	done_cnt = 0;
	for (i = 0; i < agent_ptr->thread_count; i++) {
		if (thread_ptr[i].state == DSH_DONE) {
			strncpy (&slurm_names[MAX_NAME_LEN * done_cnt],
			         thread_ptr[i].node_name, MAX_NAME_LEN);
			done_cnt++;
		}
	}

	/* send RPC */
	fatal ("Code development needed here if agent is not thread");

	xfree (slurm_addr);
#endif
	if (max_delay)
		debug ("agent maximum delay %d seconds", max_delay);

	pthread_mutex_unlock (&agent_ptr->thread_mutex);
	return (void *) NULL;
}

/* thread_per_node_rpc - thread to revoke a credential on a collection of nodes
 *	This xfrees the argument passed to it */
static void *
thread_per_node_rpc (void *args)
{
	int msg_size ;
	int rc ;
	slurm_fd sockfd ;
	slurm_msg_t request_msg ;
	slurm_msg_t response_msg ;
	return_code_msg_t * slurm_rc_msg ;
	task_info_t *task_ptr = (task_info_t *) args;
	thd_t *thread_ptr = task_ptr->thread_struct_ptr;
	state_t thread_state  = DSH_FAILED;
	sigset_t set;

	/* set up SIGALRM handler */
	if (sigemptyset (&set))
		error ("sigemptyset error: %m");
	if (sigaddset (&set, SIGALRM))
		error ("sigaddset error on SIGALRM: %m");
	if (sigprocmask (SIG_UNBLOCK, &set, NULL) != 0)
		fatal ("sigprocmask error: %m");
	xsignal(SIGALRM, alarm_handler);

	if (args == NULL)
		fatal ("thread_per_node_rpc has NULL argument");
	pthread_mutex_lock (task_ptr->thread_mutex_ptr);
	thread_ptr->state = DSH_ACTIVE;
	thread_ptr->time = time (NULL);
	pthread_mutex_unlock (task_ptr->thread_mutex_ptr);

	/* init message connection for message communication */
	if ( ( sockfd = slurm_open_msg_conn (& thread_ptr->slurm_addr) ) == SLURM_SOCKET_ERROR ) {
		error ("thread_per_node_rpc/slurm_open_msg_conn error %m");
		goto cleanup;
	}

	/* send request message */
	request_msg . msg_type = task_ptr->msg_type ;
	request_msg . data = task_ptr->msg_args_ptr ; 
	if ( ( rc = slurm_send_node_msg ( sockfd , & request_msg ) ) == SLURM_SOCKET_ERROR ) {
		error ("thread_per_node_rpc/slurm_send_node_msg error %m");
		goto cleanup;
	}

	/* receive message */
	if ( ( msg_size = slurm_receive_msg ( sockfd , & response_msg ) ) == SLURM_SOCKET_ERROR ) {
		error ("thread_per_node_rpc/slurm_receive_msg error %m");
		goto cleanup;
	}

	/* shutdown message connection */
	if ( ( rc = slurm_shutdown_msg_conn ( sockfd ) ) == SLURM_SOCKET_ERROR ) {
		error ("thread_per_node_rpc/slurm_shutdown_msg_conn error %m");
		goto cleanup;
	}
	if ( msg_size ) {
		error ("thread_per_node_rpc/msg_size error %d", msg_size);
		goto cleanup;
	}

	switch ( response_msg . msg_type )
	{
		case RESPONSE_SLURM_RC:
			slurm_rc_msg = ( return_code_msg_t * ) response_msg . data ;
			rc = slurm_rc_msg->return_code;
			slurm_free_return_code_msg ( slurm_rc_msg );	
			if (rc)
				error ("thread_per_node_rpc/rc error %d", rc);
			else {
				debug3 ("agent sucessfully processed RPC to node %s", 
				        thread_ptr->node_name);
				thread_state = DSH_DONE;
			}

			break ;
		default:
			error ("thread_per_node_rpc bad msg_type %d",response_msg.msg_type);
			break ;
	}

cleanup:
	pthread_mutex_lock (task_ptr->thread_mutex_ptr);
	thread_ptr->state = thread_state;
	thread_ptr->time = (time_t) difftime (time (NULL), thread_ptr->time);

	/* Signal completion so another thread can replace us */
	(*task_ptr->threads_active_ptr)--;
	pthread_cond_signal(task_ptr->thread_cond_ptr);
	pthread_mutex_unlock (task_ptr->thread_mutex_ptr);

	xfree (args);
	return (void *) NULL;
}

/*
 * Emulate signal() but with BSD semantics (i.e. don't restore signal to
 * SIGDFL prior to executing handler).
 */
static void xsignal(int signal, void (*handler)(int))
{
	struct sigaction sa, old_sa;

	sa.sa_handler = handler;
        sigemptyset(&sa.sa_mask);
        sigaddset(&sa.sa_mask, signal);
	sa.sa_flags = 0;
	sigaction(signal, &sa, &old_sa);
}

/*
 * SIGALRM handler.  This is just a stub because we are really interested
 * in interrupting connect() in k4cmd/rcmd or select() in rsh() below and
 * causing them to return EINTR.
 */
static void alarm_handler(int dummy)
{
}

