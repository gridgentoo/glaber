/*
** Zabbix
** Copyright (C) 2001-2019 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#ifndef ZABBIX_HEART_H
#define ZABBIX_HEART_H

#include "threads.h"


typedef struct  {
    //todo: fix to number value
	char addr[128];
	zbx_uint64_t last_activity;
	zbx_uint64_t loss_count;
	zbx_uint64_t status;	

} T_ZBX_SERVER;

//how long not to try to send heartbeats to the server
#define  ZBX_HEART_RETRY_TIMEOUT    60

//for how long hearbeats must be successiful to consider server alive
#define  ZBX_HEART_HOLD_TIMEOUT     30
//timeout for trial heartbeats, should be small enough
#define ZBX_HEARTBEAT_TRIAL_TIMEOUT      5
//number of heartbeats to fail to consider the host dead
#define ZBX_HEARTBEAT_LOSS_COUNT    2

extern int	CONFIG_HEARTBEAT_FREQUENCY;
extern int	SERVERS;
extern char *CONFIG_CLUSTER_DOMAINS;
extern T_ZBX_SERVER SERVER_LIST[ZBX_CLUSTER_MAX_SERVERS];

ZBX_THREAD_ENTRY(heart_thread, args);

#endif
