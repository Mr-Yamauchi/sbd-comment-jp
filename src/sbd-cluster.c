/* 
 * Copyright (C) 2013 Lars Marowsky-Bree <lmb@suse.com>
 * 
 * Based on crm_mon.c, which was:
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <config.h>
#include <crm_config.h>

#include <crm/cluster.h>
#include <crm/common/mainloop.h>

#include "sbd.h"

//undef SUPPORT_PLUGIN
//define SUPPORT_PLUGIN 1

static bool remote_node = false;
static pid_t remoted_pid = 0;
static int reconnect_msec = 1000;
static GMainLoop *mainloop = NULL;
static guint notify_timer = 0;
static crm_cluster_t cluster;
static gboolean sbd_remote_check(gpointer user_data);
static long unsigned int find_pacemaker_remote(void);
static void sbd_membership_destroy(gpointer user_data);

#if SUPPORT_PLUGIN
static void
sbd_plugin_membership_dispatch(cpg_handle_t handle,
                           const struct cpg_name *groupName,
                           uint32_t nodeid, uint32_t pid, void *msg, size_t msg_len)
{
    if(msg_len > 0) {
        set_servant_health(pcmk_health_online, LOG_INFO,
                           "Connected to %s", name_for_cluster_type(get_cluster_type()));
    } else {
        set_servant_health(pcmk_health_unclean, LOG_WARNING,
                           "Broken %s message", name_for_cluster_type(get_cluster_type()));
    }
    notify_parent();
    return;
}
#endif

#if SUPPORT_COROSYNC
/* CPG構成通知コールバック　*/
void
sbd_cpg_membership_dispatch(cpg_handle_t handle,
                    const struct cpg_name *groupName,
                    const struct cpg_address *member_list, size_t member_list_entries,
                    const struct cpg_address *left_list, size_t left_list_entries,
                    const struct cpg_address *joined_list, size_t joined_list_entries)
{
	/* メンバー接続状態を通知にセット */
    if(member_list_entries > 0) {
        set_servant_health(pcmk_health_online, LOG_INFO,
                           "Connected to %s", name_for_cluster_type(get_cluster_type()));
    } else {
        set_servant_health(pcmk_health_unclean, LOG_WARNING,
                           "Empty %s membership", name_for_cluster_type(get_cluster_type()));
    }
    /* inqusitorへの状態の通知 */
    notify_parent();
}
#endif
/* タイマー通知コールバック */
static gboolean
notify_timer_cb(gpointer data)
{
    cl_log(LOG_DEBUG, "Refreshing %sstate", remote_node?"remote ":"");

    if(remote_node) {
		/* pacemaker_remoteノードの処理の場合 */
		/* SBDのpacemaker_remote用のチェック処理 */
        sbd_remote_check(NULL);
        return TRUE;
    }
	/* clusterタイプによって通知を切り替える */
    switch (get_cluster_type()) {
        case pcmk_cluster_classic_ais:
            send_cluster_text(crm_class_quorum, NULL, TRUE, NULL, crm_msg_ais);
            break;

        case pcmk_cluster_corosync:	/* COROSYNC2.x系の場合 */
        case pcmk_cluster_cman:
            /* TODO - Make a CPG call and only call notify_parent() when we get a reply */
            /* inqusitorへの状態の通知 */
            notify_parent();
            break;

        default:
            break;
    }
    return TRUE;
}

/* corosync CPGサービスへの接続 */
static void
sbd_membership_connect(void)
{
    bool connected = false;

    cl_log(LOG_NOTICE, "Attempting cluster connection");

    cluster.destroy = sbd_membership_destroy;

#if SUPPORT_PLUGIN
    cluster.cpg.cpg_deliver_fn = sbd_plugin_membership_dispatch;
#endif

#if SUPPORT_COROSYNC
    cluster.cpg.cpg_confchg_fn = sbd_cpg_membership_dispatch;
#endif

    while(connected == false) {
		/* clusterタイプを取得する */
        enum cluster_type_e stack = get_cluster_type();
        if(get_cluster_type() == pcmk_cluster_unknown) {
            crm_debug("Attempting pacemaker remote connection");
            /* Nothing is up, go looking for the pacemaker remote process */
            /* clusterタイプが不明の場合は、pacemaker_remoteプロセスを検索する */
            if(find_pacemaker_remote() > 0) {
				/* pacemaker_remoteプロセスが存在すれば接続状態とする */
                connected = true;
            }

        } else {
            cl_log(LOG_INFO, "Attempting connection to %s", name_for_cluster_type(stack));
			/* corosync:CPGサービスへの接続 */
            if(crm_cluster_connect(&cluster)) {
				/* 接続状態とする */
                connected = true;
            }
        }

        if(connected == false) {
            cl_log(LOG_INFO, "Failed, retrying in %ds", reconnect_msec / 1000);
            sleep(reconnect_msec / 1000);
        }
    }
	/* CPG接続待ち状態をセット */
    set_servant_health(pcmk_health_transient, LOG_NOTICE, "Connected, waiting for initial membership");
    /* inqusitorへの状態の通知 */
    notify_parent();

    notify_timer_cb(NULL);
}
/* corosync:CPGとの切断コールバック */
static void
sbd_membership_destroy(gpointer user_data)
{
    cl_log(LOG_WARNING, "Lost connection to %s", name_for_cluster_type(get_cluster_type()));
	/* 通知状態をUNCLEANにセットする。 */
    set_servant_health(pcmk_health_unclean, LOG_ERR, "Cluster connection terminated");
    /* inqusitorへの状態の通知 */
    notify_parent();
    /* Attempt to reconnect, the watchdog will take the node down if the problem isn't transient */
    /* corosync CPGサービスへの再接続 */
    sbd_membership_connect();
}

/*
 * \internal
 * \brief Get process ID and name associated with a /proc directory entry
 *
 * \param[in]  entry    Directory entry (must be result of readdir() on /proc)
 * \param[out] name     If not NULL, a char[64] to hold the process name
 * \param[out] pid      If not NULL, will be set to process ID of entry
 *
 * \return 0 on success, -1 if entry is not for a process or info not found
 *
 * \note This should be called only on Linux systems, as not all systems that
 *       support /proc store process names and IDs in the same way.
 *       Copied from the Pacemaker implementation.
 */
int
sbd_procfs_process_info(struct dirent *entry, char *name, int *pid)
{
    int fd, local_pid;
    FILE *file;
    struct stat statbuf;
    char key[16] = { 0 }, procpath[128] = { 0 };

    /* We're only interested in entries whose name is a PID,
     * so skip anything non-numeric or that is too long.
     *
     * 114 = 128 - strlen("/proc/") - strlen("/status") - 1
     */
    local_pid = atoi(entry->d_name);
    if ((local_pid <= 0) || (strlen(entry->d_name) > 114)) {
        return -1;
    }
    if (pid) {
        *pid = local_pid;
    }

    /* Get this entry's file information */
    strcpy(procpath, "/proc/");
    strcat(procpath, entry->d_name);
    fd = open(procpath, O_RDONLY);
    if (fd < 0 ) {
        return -1;
    }
    if (fstat(fd, &statbuf) < 0) {
        close(fd);
        return -1;
    }
    close(fd);

    /* We're only interested in subdirectories */
    if (!S_ISDIR(statbuf.st_mode)) {
        return -1;
    }

    /* Read the first entry ("Name:") from the process's status file.
     * We could handle the valgrind case if we parsed the cmdline file
     * instead, but that's more of a pain than it's worth.
     */
    if (name != NULL) {
        strcat(procpath, "/status");
        file = fopen(procpath, "r");
        if (!file) {
            return -1;
        }
        if ((fscanf(file, "%15s%63s", key, name) != 2)
            || safe_str_neq(key, "Name:")) {
            fclose(file);
            return -1;
        }
        fclose(file);
    }

    return 0;
}

/* SBDのpacemaker_remote用のチェック処理 */
static gboolean
sbd_remote_check(gpointer user_data)
{
    static int have_proc_pid = 0;

    int running = 0;

    cl_log(LOG_DEBUG, "Checking pacemaker remote connection: %d/%d", have_proc_pid, remoted_pid);
    
    if(have_proc_pid == 0) {
        char proc_path[PATH_MAX], exe_path[PATH_MAX];

        /* check to make sure pid hasn't been reused by another process */
        snprintf(proc_path, sizeof(proc_path), "/proc/%lu/exe", (long unsigned int)getpid());

        have_proc_pid = 1;
        if(readlink(proc_path, exe_path, PATH_MAX - 1) < 0) {
            have_proc_pid = -1;
        }
    }
    
    if (remoted_pid <= 0) {
        set_servant_health(pcmk_health_transient, LOG_WARNING, "No Pacemaker Remote connection");
        goto notify;

    } else if (kill(remoted_pid, 0) < 0 && errno == ESRCH) {
        /* Not running */

    } else if(have_proc_pid == -1) {
        running = 1;
        cl_log(LOG_DEBUG, "Poccess %ld is active", (long)remoted_pid);

    } else {
        int rc = 0;
        char proc_path[PATH_MAX], exe_path[PATH_MAX], expected_path[PATH_MAX];

        /* check to make sure pid hasn't been reused by another process */
        snprintf(proc_path, sizeof(proc_path), "/proc/%lu/exe", (long unsigned int)remoted_pid);

        rc = readlink(proc_path, exe_path, PATH_MAX - 1);
        if (rc < 0) {
            crm_perror(LOG_ERR, "Could not read from %s", proc_path);
            goto done;
        }
        exe_path[rc] = 0;

        rc = snprintf(expected_path, sizeof(proc_path), "%s/pacemaker_remoted", SBINDIR);
        expected_path[rc] = 0;

        if (strcmp(exe_path, expected_path) == 0) {
			/* pacemaker_remoteプロセスの存在を確認した場合　 */
            cl_log(LOG_DEBUG, "Poccess %s (%ld) is active",
                   exe_path, (long)remoted_pid);
            /* 稼働状態としてセット */
            running = 1;
        }
    }

  done:
    /* 稼働、非稼働で通知状態をセットする */
    if(running) {
        set_servant_health(pcmk_health_online, LOG_INFO,
                           "Connected to Pacemaker Remote %lu", (long unsigned int)remoted_pid);
    } else {
        set_servant_health(pcmk_health_unclean, LOG_WARNING,
                           "Connection to Pacemaker Remote %lu lost", (long unsigned int)remoted_pid);
    }

  notify:    
  	/* inqusitorにpacemaker_remoteプロセスの状態をセットする */
    notify_parent();

    if(running == 0) {
		/* corosync CPGサービスへの接続を試みる */
        sbd_membership_connect();
    }
    return true;
}
/* pacemaker_remoteプロセス(PID)を検索する */
static long unsigned int
find_pacemaker_remote(void)
{
    DIR *dp;
    char entry_name[64];
    struct dirent *entry;

    dp = opendir("/proc");
    if (!dp) {
        /* no proc directory to search through */
        cl_log(LOG_NOTICE, "Can not read /proc directory to track existing components");
        return FALSE;
    }
	/* /procからの探索ループ */
    while ((entry = readdir(dp)) != NULL) {
        int pid;
		/* 対象/porcかどうかの判定 */
        if (sbd_procfs_process_info(entry, entry_name, &pid) < 0) {
            continue;
        }

        /* entry_name is truncated to 16 characters including the nul terminator */
        cl_log(LOG_DEBUG, "Found %s at %u", entry_name, pid);
        if (strcmp(entry_name, "pacemaker_remot") == 0) {
			/* entry_nameが"pacemaker_remot"であれば、pacemaker_remoteのPIDとして保存 */
            cl_log(LOG_NOTICE, "Found Pacemaker Remote at PID %u", pid);
            remoted_pid = pid;
            remote_node = true;
            break;
        }
    }

    closedir(dp);

    return remoted_pid;
}

static void
clean_up(int rc)
{
    return;
}

static void
cluster_shutdown(int nsig)
{
    clean_up(0);
}
/* servant:Clusterの本体 */
int
servant_cluster(const char *diskname, int mode, const void* argp)
{
    enum cluster_type_e cluster_stack = get_cluster_type();

    crm_system_name = strdup("sbd:cluster");
    cl_log(LOG_INFO, "Monitoring %s cluster health", name_for_cluster_type(cluster_stack));
    set_proc_title("sbd: watcher: Cluster");
	/* corosyncのCPGサービスへの接続 */
    sbd_membership_connect();

    /* stonith_our_uname = cluster.uname; */
    /* stonith_our_uuid = cluster.uuid; */

    mainloop = g_main_new(FALSE);
    notify_timer = g_timeout_add(timeout_loop * 1000, notify_timer_cb, NULL);

    mainloop_add_signal(SIGTERM, cluster_shutdown);
    mainloop_add_signal(SIGINT, cluster_shutdown);
    
    g_main_run(mainloop);
    g_main_destroy(mainloop);
    
    clean_up(0);
    return 0;                   /* never reached */
}
