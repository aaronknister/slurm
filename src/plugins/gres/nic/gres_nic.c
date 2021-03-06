/*****************************************************************************\
 *  gres_nic.c - Support NICs as a generic resources.
 *****************************************************************************
 *  Copyright (C) 2010 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
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

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "slurm/slurm.h"
#include "slurm/slurm_errno.h"

#include "src/common/slurm_xlator.h"
#include "src/common/bitstring.h"
#include "src/common/env.h"
#include "src/common/gres.h"
#include "src/common/list.h"
#include "src/common/xcgroup_read_config.c"
#include "src/common/xstring.h"

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - A string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - A string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  SLURM uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "auth" for SLURM authentication) and <method> is a
 * description of how this plugin satisfies that application.  SLURM will
 * only load authentication plugins if the plugin_type string has a prefix
 * of "auth/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char	plugin_name[]		= "Gres NIC plugin";
const char	plugin_type[]		= "gres/nic";
const uint32_t	plugin_version		= SLURM_VERSION_NUMBER;

static char	gres_name[]		= "nic";

static int *nic_devices = NULL;
static int nb_available_files = 0;

extern int init(void)
{
	debug("%s: %s loaded", __func__, plugin_name);

	return SLURM_SUCCESS;
}
extern int fini(void)
{
	debug("%s: unloading %s", __func__, plugin_name);
	xfree(nic_devices);
	nb_available_files = 0;

	return SLURM_SUCCESS;
}

/*
 * We could load gres state or validate it using various mechanisms here.
 * This only validates that the configuration was specified in gres.conf.
 * In the general case, no code would need to be changed.
 */
extern int node_config_load(List gres_conf_list)
{
	int i, rc = SLURM_SUCCESS;
	ListIterator iter;
	gres_slurmd_conf_t *gres_slurmd_conf;
	int nb_nic = 0;	/* Number of NICs in the list */
	int available_files_index = 0;

	xassert(gres_conf_list);
	iter = list_iterator_create(gres_conf_list);
	while ((gres_slurmd_conf = list_next(iter))) {
		if (xstrcmp(gres_slurmd_conf->name, gres_name))
			continue;
		if (gres_slurmd_conf->file)
			nb_nic++;
	}
	list_iterator_destroy(iter);
	xfree(nic_devices);	/* No-op if NULL */
	nb_available_files = -1;
	/* (Re-)Allocate memory if number of files changed */
	if (nb_nic > nb_available_files) {
		nic_devices = (int *) xmalloc(sizeof(int) * nb_nic);
		nb_available_files = nb_nic;
		for (i = 0; i < nb_available_files; i++)
			nic_devices[i] = -1;
	}

	iter = list_iterator_create(gres_conf_list);
	while ((gres_slurmd_conf = list_next(iter))) {
		if ((xstrcmp(gres_slurmd_conf->name, gres_name) == 0) &&
		    gres_slurmd_conf->file) {
			/* Populate nic_devices array with number
			 * at end of the file name */
			char *bracket, *fname, *tmp_name;
			hostlist_t hl;
			bracket = strrchr(gres_slurmd_conf->file, '[');
			if (bracket)
				tmp_name = xstrdup(bracket);
			else
				tmp_name = xstrdup(gres_slurmd_conf->file);
			hl = hostlist_create(tmp_name);
			xfree(tmp_name);
			if (!hl) {
				rc = EINVAL;
				break;
			}
			while ((fname = hostlist_shift(hl))) {
				if (available_files_index ==
				    nb_available_files) {
					nb_available_files++;
					xrealloc(nic_devices, sizeof(int) *
						 nb_available_files);
					nic_devices[available_files_index] = -1;
				}
				for (i = 0; fname[i]; i++) {
					if (!isdigit(fname[i]))
						continue;
					nic_devices[available_files_index] =
						atoi(fname + i);
					break;
				}
				available_files_index++;
				free(fname);
			}
			hostlist_destroy(hl);
		}
	}
	list_iterator_destroy(iter);

	if (rc != SLURM_SUCCESS)
		fatal("%s failed to load configuration", plugin_name);

	for (i = 0; i < nb_available_files; i++)
		info("nic %d is device number %d", i, nic_devices[i]);

	return rc;
}

/*
 * Test if OMPI_MCA_btl_openib_if_include should be set to global device ID or a
 * device ID that always starts at zero (based upon what the application can see).
 * RET true if TaskPlugin=task/cgroup AND ConstrainDevices=yes (in cgroup.conf).
 */
static bool _use_local_device_index(void)
{
	slurm_cgroup_conf_t slurm_cgroup_conf;
	char *task_plugin = slurm_get_task_plugin();
	bool use_cgroup = false, use_local_index = false;

	if (!task_plugin)
		return use_local_index;

	if (strstr(task_plugin, "cgroup"))
		use_cgroup = true;
	xfree(task_plugin);
	if (!use_cgroup)
		return use_local_index;

	/* Read and parse cgroup.conf */
	bzero(&slurm_cgroup_conf, sizeof(slurm_cgroup_conf_t));
	if (read_slurm_cgroup_conf(&slurm_cgroup_conf) != SLURM_SUCCESS)
		return use_local_index;
	if (slurm_cgroup_conf.constrain_devices)
		use_local_index = true;
	free_slurm_cgroup_conf(&slurm_cgroup_conf);

	return use_local_index;
}

/*
 * Set environment variables as appropriate for a job (i.e. all tasks) based
 * upon the job's GRES state.
 */
extern void job_set_env(char ***job_env_ptr, void *gres_ptr)
{
	int i, len, local_inx = 0;
	char *dev_list = NULL;
	gres_job_state_t *gres_job_ptr = (gres_job_state_t *) gres_ptr;
	bool use_local_dev_index = _use_local_device_index();

	if ((gres_job_ptr != NULL) &&
	    (gres_job_ptr->node_cnt == 1) &&
	    (gres_job_ptr->gres_bit_alloc != NULL) &&
	    (gres_job_ptr->gres_bit_alloc[0] != NULL)) {
		len = bit_size(gres_job_ptr->gres_bit_alloc[0]);
		for (i = 0; i < len; i++) {
			if (!bit_test(gres_job_ptr->gres_bit_alloc[0], i))
				continue;
			if (!dev_list)
				dev_list = xmalloc(128);
			else
				xstrcat(dev_list, ",");
			if (use_local_dev_index) {
				xstrfmtcat(dev_list, "mlx4_%d", local_inx++);
			} else if (nic_devices && (i < nb_available_files) &&
				   (nic_devices[i] >= 0)) {
				xstrfmtcat(dev_list, "mlx4_%d", nic_devices[i]);
			} else {
				xstrfmtcat(dev_list, "mlx4_%d", i);
			}
		}
	} else if (gres_job_ptr && (gres_job_ptr->gres_cnt_alloc > 0)) {
		/* The gres.conf file must identify specific device files
		 * in order to set the OMPI_MCA_btl_openib_if_include env var */
		debug("gres/nic unable to set OMPI_MCA_btl_openib_if_include, no device files configured");
	} else {
		xstrcat(dev_list, "NoDevFiles");
	}

	if (dev_list) {
		/* we assume mellanox cards and OpenMPI programm */
		env_array_overwrite(job_env_ptr,
				    "OMPI_MCA_btl_openib_if_include",
				    dev_list);
		xfree(dev_list);
	}
}

/*
 * Set environment variables as appropriate for a job (i.e. all tasks) based
 * upon the job step's GRES state.
 */
extern void step_set_env(char ***job_env_ptr, void *gres_ptr)
{
	int i, len, local_inx = 0;
	char *dev_list = NULL;
	gres_step_state_t *gres_step_ptr = (gres_step_state_t *) gres_ptr;
	bool use_local_dev_index = _use_local_device_index();

	if ((gres_step_ptr != NULL) &&
	    (gres_step_ptr->node_cnt == 1) &&
	    (gres_step_ptr->gres_bit_alloc != NULL) &&
	    (gres_step_ptr->gres_bit_alloc[0] != NULL)) {
		len = bit_size(gres_step_ptr->gres_bit_alloc[0]);
		for (i = 0; i < len; i++) {
			if (!bit_test(gres_step_ptr->gres_bit_alloc[0], i))
				continue;
			if (!dev_list)
				dev_list = xmalloc(128);
			else
				xstrcat(dev_list, ",");
			if (use_local_dev_index) {
				xstrfmtcat(dev_list, "mlx4_%d", local_inx++);
			} else if (nic_devices && (i < nb_available_files) &&
				   (nic_devices[i] >= 0)) {
				xstrfmtcat(dev_list, "mlx4_%d", nic_devices[i]);
			} else {
				xstrfmtcat(dev_list, "mlx4_%d", i);
			}
		}
	} else if (gres_step_ptr && (gres_step_ptr->gres_cnt_alloc > 0)) {
		/* The gres.conf file must identify specific device files
		 * in order to set the OMPI_MCA_btl_openib_if_include env var */
		error("gres/nic unable to set OMPI_MCA_btl_openib_if_include, "
		      "no device files configured");
	} else {
		xstrcat(dev_list, "NoDevFiles");
	}

	if (dev_list) {
		/* we assume mellanox cards and OpenMPI programm */
		env_array_overwrite(job_env_ptr,
				    "OMPI_MCA_btl_openib_if_include",
				    dev_list);
		xfree(dev_list);
	}
}

/*
 * Reset environment variables as appropriate for a job (i.e. this one tasks)
 * based upon the job step's GRES state and assigned CPUs.
 */
extern void step_reset_env(char ***job_env_ptr, void *gres_ptr,
			   bitstr_t *usable_gres)
{
	int i, len, first_match = -1;
	char *dev_list = NULL;
	gres_step_state_t *gres_step_ptr = (gres_step_state_t *) gres_ptr;

	if ((gres_step_ptr != NULL) &&
	    (gres_step_ptr->node_cnt == 1) &&
	    (gres_step_ptr->gres_bit_alloc != NULL) &&
	    (gres_step_ptr->gres_bit_alloc[0] != NULL) &&
	    (usable_gres != NULL)) {
		len = MIN(bit_size(gres_step_ptr->gres_bit_alloc[0]),
			  bit_size(usable_gres));
		for (i = 0; i < len; i++) {
			if (!bit_test(gres_step_ptr->gres_bit_alloc[0], i))
				continue;
			if (first_match == -1)
				first_match = i;
			if (!bit_test(usable_gres, i))
				continue;
			if (!dev_list)
				dev_list = xmalloc(128);
			else
				xstrcat(dev_list, ",");
			if (nic_devices && (i < nb_available_files) &&
			    (nic_devices[i] >= 0)) {
				xstrfmtcat(dev_list, "mlx4_%d", nic_devices[i]);
			} else {
				xstrfmtcat(dev_list, "mlx4_%d", i);
			}
		}
		if (!dev_list && (first_match != -1)) {
			i = first_match;
			dev_list = xmalloc(128);
			if (nic_devices && (i < nb_available_files) &&
			    (nic_devices[i] >= 0)) {
				xstrfmtcat(dev_list, "mlx4_%d", nic_devices[i]);
			} else {
				xstrfmtcat(dev_list, "mlx4_%d", i);
			}
		}
	}

	if (dev_list) {
		/* we assume mellanox cards and OpenMPI programm */
		env_array_overwrite(job_env_ptr,
				    "OMPI_MCA_btl_openib_if_include",
				    dev_list);
		xfree(dev_list);
	}
}

/* Send GRES information to slurmstepd on the specified file descriptor*/
extern void send_stepd(int fd)
{
	int i;

	safe_write(fd, &nb_available_files, sizeof(int));
	for (i = 0; i < nb_available_files; i++)
		safe_write(fd, &nic_devices[i], sizeof(int));
	return;

rwfail:	error("gres_plugin_send_stepd failed");
}

/* Receive GRES information from slurmd on the specified file descriptor */
extern void recv_stepd(int fd)
{
	int i;

	safe_read(fd, &nb_available_files, sizeof(int));
	if (nb_available_files > 0) {
		xfree(nic_devices);	/* No-op if NULL */
		nic_devices = xmalloc(sizeof(int) * nb_available_files);
	}
	for (i = 0; i < nb_available_files; i++)
		safe_read(fd, &nic_devices[i], sizeof(int));
	return;

rwfail:	error("gres_plugin_recv_stepd failed");
}

extern int job_info(gres_job_state_t *job_gres_data, uint32_t node_inx,
		     enum gres_job_data_type data_type, void *data)
{
	return EINVAL;
}

extern int step_info(gres_step_state_t *step_gres_data, uint32_t node_inx,
		     enum gres_step_data_type data_type, void *data)
{
	return EINVAL;
}
