/*****************************************************************************\
 *  agent.h - data structures and function definitions for parallel 
 *	background communications
 *
 *  $Id$
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Moe Jette <jette@llnl.gov>, et. al.
 *  Derived from dsh written by Jim Garlick <garlick1@llnl.gov>
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
\*****************************************************************************/

#ifndef _AGENT_H
#define _AGENT_H

#include "src/slurmctld/agent.h"
#include "src/slurmctld/slurmctld.h"

#define AGENT_IS_THREAD  	 1	/* set if agent itself a thread of 
					 * slurmctld, 0 for function call */
#define AGENT_THREAD_COUNT	10	/* maximum active threads per agent */
#define AGENT_SPREAD_COUNT	10	/* maximum number of 
					   forwards per node */
#define COMMAND_TIMEOUT 	10	/* command requeue or error, seconds */
#define MAX_AGENT_CNT		(MAX_SERVER_THREADS / (AGENT_THREAD_COUNT + 2))
					/* maximum simultaneous agents, note 
					 *   total thread count is product of
					 *   MAX_AGENT_CNT and
					 *   (AGENT_THREAD_COUNT + 2) */ 

typedef struct agent_arg {
	uint32_t	node_count;	/* number of nodes to communicate 
					 * with */
	uint16_t	retry;		/* if set, keep trying */
	slurm_addr	*slurm_addr;	/* array of network addresses */
	char		*node_names;	/* array with MAX_NAME_LEN bytes
					 * per node */
	slurm_msg_type_t msg_type;	/* RPC to be issued */
	void		*msg_args;	/* RPC data to be transmitted */
} agent_arg_t;

/*
 * agent - party responsible for transmitting an common RPC in parallel 
 *	across a set of nodes. agent_queue_request() if immediate 
 *	execution is not essential.
 * IN pointer to agent_arg_t, which is xfree'd (including slurm_addr, 
 *	node_names and msg_args) upon completion if AGENT_IS_THREAD is set
 * RET always NULL (function format just for use as pthread)
 */
extern void *agent (void *args);

/*
 * agent_queue_request - put a request on the queue for later execution or
 *	execute now if not too busy
 * IN agent_arg_ptr - the request to enqueue
 */
extern void agent_queue_request(agent_arg_t *agent_arg_ptr);

/*
 * agent_retry - Agent for retrying pending RPCs. One pending request is 
 *	issued if it has been pending for at least min_wait seconds
 * IN min_wait - Minimum wait time between re-issue of a pending RPC
 * RET count of queued requests remaining (zero if none are old enough 
 * to re-issue)
 */
extern int agent_retry (int min_wait);

/* agent_purge - purge all pending RPC requests */
extern void agent_purge (void);

/*
 * mail_job_info - Send e-mail notice of job state change
 * IN job_ptr - job identification
 * IN state_type - job transition type, see MAIL_JOB in slurm.h
 */ 
extern void mail_job_info (struct job_record *job_ptr, uint16_t mail_type);

#endif /* !_AGENT_H */
