/*****************************************************************************\
 * src/slurmd/slurmd.c - main slurm node server daemon
 * $Id$
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <mgrondona@llnl.gov>.
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

#if HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <unistd.h>

#include <src/common/log.h>
#include <src/common/xmalloc.h>
#include <src/common/xstring.h>
#include <src/common/slurm_protocol_api.h>
#include <src/common/xsignal.h>
#include <src/common/credential_utils.h>
#include <src/common/signature_utils.h>
#include <src/common/parse_spec.h>
#include <src/common/hostlist.h>
#include <src/common/fd.h>

#include <src/slurmd/slurmd.h>
#include <src/slurmd/req.h>
#include <src/slurmd/shm.h>
#include <src/slurmd/get_mach_stat.h>

#define GETOPT_ARGS	"L:f:Dvhc"

#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN	64
#endif

typedef struct connection {
	slurm_fd fd;
	slurm_addr *cli_addr;
} conn_t;


/*
 * static shutdown and reconfigure flags:
 */
static sig_atomic_t shutdown = 0;
static sig_atomic_t reconfig = 0;

static void       _term_handler(int);
static void       _hup_handler(int);
static void       _process_cmdline(int ac, char **av);
static void       _create_msg_socket();
static void       _tid_free(pthread_t *);
static pthread_t *_tid_copy(pthread_t *);
static void       _msg_engine();
static int        _slurmd_init();
static int        _slurmd_fini();
static void       _create_conf();
static void       _init_conf();
static void       _read_config();
static void       _usage();
static void       _handle_connection(slurm_fd fd, slurm_addr *client);
static void      *_service_connection(void *);

static void _fill_registration_msg(slurm_node_registration_status_msg_t *);


int 
main (int argc, char *argv[])
{
	_create_conf();
	_init_conf();
	_process_cmdline(argc, argv);
	log_init(argv[0], conf->log_opts, LOG_DAEMON, conf->logfile);
	_read_config();
	_create_msg_socket();

	if (conf->daemonize) {
		daemon(0,0);
		chdir("/tmp");
	}

	conf->pid = getpid();
	
	if (_slurmd_init() < 0)
		exit(1);

        if (send_registration_msg() < 0) 
		error("Unable to register with slurm controller");

	xsignal(SIGTERM, &_term_handler);
	xsignal(SIGINT,  &_term_handler);
	xsignal(SIGHUP,  &_hup_handler );

	_msg_engine();

	_slurmd_fini();

	return 0;
}

static void
_msg_engine()
{
	slurm_fd sock;
	slurm_addr cli;

	while (1) {
		if (shutdown)
			break;
  again:
		if ((sock = slurm_accept_msg_conn(conf->lfd, &cli)) < 0) {
			if (errno == EINTR) {
				if (shutdown) {
					verbose("got shutdown request");
					break;
				}
				if (reconfig) {
					/* _reconfigure(); */
					verbose("got reconfigure request");
				}
				goto again;
			}
			error("accept: %m");
			continue;
		}
		if (sock > 0)
			_handle_connection(sock, &cli);
	}
	slurm_shutdown_msg_engine(conf->lfd);
	return;
}

static pthread_t *
_tid_copy(pthread_t *tid)
{
	pthread_t *id = xmalloc(sizeof(*id));
	*id = *tid;
	return id;
}

static void
_tid_free(pthread_t *tid)
{
	xfree(tid);
}

static void
_handle_connection(slurm_fd fd, slurm_addr *cli)
{
	int            rc;
	pthread_attr_t attr;
	pthread_t      id;
	conn_t         *arg = xmalloc(sizeof(*arg));

	arg->fd       = fd;
	arg->cli_addr = cli;

	if ((rc = pthread_attr_init(&attr)) != 0) {
		error("pthread_attr_init: %s", slurm_strerror(rc));
		return;
	}

	rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (rc != 0) {
		error("Unable to set detachstate on attr: %s", 
				slurm_strerror(rc));
		return;
	}

	fd_set_close_on_exec(fd);

	rc = pthread_create(&id, &attr, &_service_connection, (void *) arg);
	if (rc != 0) {
		error("msg_engine: pthread_create: %s", slurm_strerror(rc));
		_service_connection((void *) &arg);
		return;
	}

	list_append(conf->threads, (void *) _tid_copy(&id));

	return;
}

static int
_find_tid(pthread_t *tid, pthread_t *key)
{
	return (*tid == *key);
}

static void *
_service_connection(void *arg)
{
	int rc;
	pthread_t tid = pthread_self();
	conn_t *con = (conn_t *) arg;
	slurm_msg_t *msg = xmalloc(sizeof(*msg));

	if ((rc = slurm_receive_msg(con->fd, msg)) < 0) {
		error("slurm_recieve_msg: %m");
		slurm_free_msg(msg);
	} else {
		msg->conn_fd = con->fd;
		slurmd_req(msg, con->cli_addr);
	}
	slurm_close_accepted_conn(con->fd);	
	xfree(con);
	list_delete_all(conf->threads, (ListFindF) _find_tid, &tid);
	return NULL;
}

int
send_registration_msg()
{
	slurm_msg_t req;
	slurm_msg_t resp;
	slurm_node_registration_status_msg_t msg;

	_fill_registration_msg(&msg);

	req.msg_type = MESSAGE_NODE_REGISTRATION_STATUS;
	req.data     = &msg;

	if (slurm_send_recv_controller_msg(&req, &resp) < 0) {
		error("Unable to register: %m");
		return SLURM_FAILURE;
	}

	/* XXX look at response msg
	 */

	return SLURM_SUCCESS;
}

static void
_fill_registration_msg(slurm_node_registration_status_msg_t *msg)
{
	List         steps;
	ListIterator i;
	job_step_t  *s;
	int          n;

	msg->node_name = conf->hostname;

	get_procs(&msg->cpus);
	get_memory(&msg->real_memory_size);
	get_tmp_disk(&msg->temporary_disk_space);

	steps          = shm_get_steps();
	msg->job_count = list_count(steps);
	msg->job_id    = xmalloc(msg->job_count * sizeof(*msg->job_id));
	msg->step_id   = xmalloc(msg->job_count * sizeof(*msg->step_id));

	i = list_iterator_create(steps);
	n = 0;
	while ((s = list_next(i))) {
		debug("found currently running job %d.%d", 
		      s->jobid, s->stepid);
		msg->job_id[n]  = s->jobid;
		msg->step_id[n] = s->stepid;
		n++;
	}
	list_iterator_destroy(i);
	list_destroy(steps);

	msg->timestamp = time(NULL);

	return;
}

static inline int
_free_and_set(char **confvar, char *newval)
{
	if (newval) {
		if (*confvar)
			xfree(*confvar);
		*confvar = newval;
		return 1;
	} else
		return 0;
}

static void
_read_config()
{
	int   rc, i, j;
	int   line = 0;
	char  in[BUFSIZ];
	char *epilog, *prolog, *tmpfs, *savedir, *pubkey;
	FILE *fp;

	epilog = prolog = tmpfs = savedir = pubkey = NULL;

	if ((fp = fopen(conf->conffile, "r")) == NULL) {
		error("Unable to open config file `%s': %m", conf->conffile); 
		exit(1);
	}
	
	line = 0;
	while ((fgets(in, BUFSIZ, fp))) {
		line++;
		if (strlen(in) == BUFSIZ - 1) {
			info("Warning: %s:line %d may be too long",
					conf->conffile, line);
		}

		/* XXX this needs work, but it is how the rest of
		 * slurm reads config file so we keep it the same
		 * for now
		 */
		for (i = 0; i < BUFSIZ; i++) {
			if (in[i] == (char) NULL)
				break;
			if (in[i] != '#')
				continue;
			if ((i > 0) && (in[i - 1] == '\\')) {    
				for (j = i; j < BUFSIZ; j++) {
					in[j - 1] = in[j];
				}
				continue;
			}
			in[i] = '\0';
			break;
		}

		rc = slurm_parser(in,
		     "Epilog=",                         's', &epilog,
		     "Prolog=",                         's', &prolog,
		     "TmpFS=",                          's', &tmpfs,
		     "JobCredentialPublicCertificate=", 's', &pubkey,
		     "StateSaveLocation=",              's', &savedir,
		     "HeartbeatInterval=",              'd', &conf->hbeat,
		     "SlurmdPort=",                     'd', &conf->port,
		     "END");
	}

	debug3("Epilog = `%s'",       epilog );
	_free_and_set(&conf->epilog,  epilog );
	debug3("Prolog = `%s'",       prolog );
	_free_and_set(&conf->prolog,  prolog );
	debug3("TmpFS = `%s'",        tmpfs  );
	_free_and_set(&conf->tmpfs,   tmpfs  );
	debug3("Public Cert = `%s'",  pubkey );
	_free_and_set(&conf->pubkey,  pubkey );
	debug3("Save dir = `%s'",     savedir);
	_free_and_set(&conf->savedir, savedir);

	if (fclose(fp) < 0) {
		error("Closing slurm log file `%s': %m", conf->conffile);
	}
}

static void 
_create_conf()
{
	conf = xmalloc(sizeof(*conf));
}

static void
_init_conf()
{
	char  host[MAXHOSTNAMELEN];
	log_options_t lopts = LOG_OPTS_STDERR_ONLY;

	if (getnodename(host, MAXHOSTNAMELEN) < 0) {
		error("Unable to get my hostname: %m");
		exit(1);
	}
	conf->hostname = xstrdup(host);
	if (read_slurm_port_config() < 0)
		error("Unable to get slurmd listen port");
	conf->conffile  = xstrdup(SLURM_CONFIG_FILE);
	conf->port      = slurm_get_slurmd_port();
	conf->savedir	= NULL;
	conf->pubkey    = NULL;
	conf->prolog    = NULL;
	conf->epilog    = NULL;
	conf->daemonize =  0;
	conf->lfd       = -1;
	conf->log_opts  = lopts;
	return;
}

static void
_process_cmdline(int ac, char **av)
{
	int c;

	conf->prog = xbasename(av[0]);

	while ((c = getopt(ac, av, GETOPT_ARGS)) > 0) {
		switch (c) {
		case 'D': 
			conf->daemonize = 0;
			break;
		case 'v':
			conf->log_opts.stderr_level++;
			break;
		case 'h':
			_usage();
			exit(0);
			break;
		case 'f':
			conf->conffile = xstrdup(optarg);
			break;
		case 'L':
			conf->logfile = xstrdup(optarg);
			break;
		case 'c':
			shm_cleanup();
			break;
		default:
			_usage(c);
			exit(1);
			break;
		}
	}
}


static void
_create_msg_socket()
{
	slurm_fd ld = slurm_init_msg_engine_port(conf->port);

	if (ld < 0) {
		error("Unable to bind listen port (%d): %m", conf->port);
		exit(1);
	}

	fd_set_close_on_exec(ld);

	conf->lfd = ld;

	return;
}


static int
_slurmd_init()
{
	struct rlimit rlim;

	if (getrlimit(RLIMIT_NOFILE,&rlim) == 0) {
		rlim.rlim_cur = rlim.rlim_max;
		setrlimit(RLIMIT_NOFILE,&rlim);
	}

	slurm_ssl_init();
	slurm_init_verifier(&conf->vctx, conf->pubkey);
	initialize_credential_state_list(&conf->cred_state_list);
	conf->threads = list_create((ListDelF) _tid_free);
	if (shm_init() < 0)
		return SLURM_FAILURE;
	return SLURM_SUCCESS;
}

static int
_slurmd_fini()
{
	list_destroy(conf->threads);
	destroy_credential_state_list(conf->cred_state_list);
	slurm_destroy_ssl_key_ctx(&conf->vctx);
	slurm_ssl_destroy();
	shm_fini();
	return SLURM_SUCCESS;
}

static void
_term_handler(int signum)
{
	if (signum == SIGTERM || signum == SIGINT) 
		shutdown = 1;
}

static void 
_hup_handler(int signum)
{
	if (signum == SIGHUP)
		reconfig = 1;
}


static void 
_usage()
{
	fprintf(stderr, "Usage: %s [OPTIONS]\n", conf->prog);
	fprintf(stderr, "  -f file "
			"\tUse `file' as slurmd config file.\n");
	fprintf(stderr, "  -L logfile "
			"\tLog messages to the file `logfile'\n");
	fprintf(stderr, "  -v      "
			"\tVerbose mode. Multiple -v's increase verbosity.\n");
	fprintf(stderr, "  -D      "
			"\tRun daemon in forground.\n");
	fprintf(stderr, "  -c      "
			"\tForce cleanup of slurmd shared memory.\n");
	fprintf(stderr, "  -h      "
			"\tPrint this help message.\n");
}
