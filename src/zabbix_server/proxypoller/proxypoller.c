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

#include "common.h"
#include "daemon.h"
#include "comms.h"
#include "zbxself.h"

#include "proxypoller.h"
#include "zbxserver.h"
#include "dbcache.h"
#include "db.h"
#include "zbxjson.h"
#include "log.h"
#include "proxy.h"
#include "../../libs/zbxcrypto/tls.h"
#include "../trapper/proxydata.h"

extern unsigned char	process_type, program_type;
extern int		server_num, process_num;
extern int CONFIG_CLUSTER_SERVER_ID;
extern char *CONFIG_HOSTNAME;

/* declaring some zbx_dc* cluster functions here to avoid compiler warnings since dbconfig.h shoudn't be included */
int zbx_dc_create_hello_json(struct zbx_json* j);
int zbx_dc_parce_hello_json(DC_PROXY *proxy,struct zbx_json_parse	*jp, int timediff);
int zbx_dc_recalc_topology(void);
int zbx_dc_set_topology(const char *topology);
zbx_uint64_t zbx_dc_current_topology_version();
int zbx_dc_set_topology_recalc(void);
void zbx_dc_set_download_server_topology(zbx_uint64_t hostid);
zbx_uint64_t zbx_dc_get_download_server_topology(void);
void	zbx_dc_register_server_na(zbx_uint64_t hostid, char * reason);
int zbx_dc_create_rerouted_json(struct zbx_json *j, zbx_uint64_t serverid);
void zbx_dc_register_proxy_availability(u_int64_t hostid);

static int	connect_to_proxy(const DC_PROXY *proxy, zbx_socket_t *sock, int timeout)
{
	int		ret = FAIL;
	const char	*tls_arg1, *tls_arg2;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() address:%s port:%hu timeout:%d conn:%u", __func__, proxy->addr,
			proxy->port, timeout, (unsigned int)proxy->tls_connect);

	switch (proxy->tls_connect)
	{
		case ZBX_TCP_SEC_UNENCRYPTED:
			tls_arg1 = NULL;
			tls_arg2 = NULL;
			break;
#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
		case ZBX_TCP_SEC_TLS_CERT:
			tls_arg1 = proxy->tls_issuer;
			tls_arg2 = proxy->tls_subject;
			break;
		case ZBX_TCP_SEC_TLS_PSK:
			tls_arg1 = proxy->tls_psk_identity;
			tls_arg2 = proxy->tls_psk;
			break;
#else
		case ZBX_TCP_SEC_TLS_CERT:
		case ZBX_TCP_SEC_TLS_PSK:
			zabbix_log(LOG_LEVEL_ERR, "TLS connection is configured to be used with passive proxy \"%s\""
					" but support for TLS was not compiled into %s.", proxy->host,
					get_program_type_string(program_type));
			ret = CONFIG_ERROR;
			goto out;
#endif
		default:
			THIS_SHOULD_NEVER_HAPPEN;
			goto out;
	}

	if (FAIL == (ret = zbx_tcp_connect(sock, CONFIG_SOURCE_IP, proxy->addr, proxy->port, timeout,
			proxy->tls_connect, tls_arg1, tls_arg2)))
	{
		zabbix_log(LOG_LEVEL_ERR, "cannot connect to proxy \"%s\": %s", proxy->host, zbx_socket_strerror());
		ret = NETWORK_ERROR;
	}
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_result_string(ret));

	return ret;
}

static int	send_data_to_proxy(const DC_PROXY *proxy, zbx_socket_t *sock, const char *data, size_t size)
{
	int	ret, flags = ZBX_TCP_PROTOCOL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() data:'%s'", __func__, data);

	if (0 != proxy->auto_compress)
		flags |= ZBX_TCP_COMPRESS;

	if (FAIL == (ret = zbx_tcp_send_ext(sock, data, size, flags, 0)))
	{
		zabbix_log(LOG_LEVEL_ERR, "cannot send data to proxy \"%s\": %s", proxy->host, zbx_socket_strerror());

		ret = NETWORK_ERROR;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_result_string(ret));

	return ret;
}

static int	recv_data_from_proxy(const DC_PROXY *proxy, zbx_socket_t *sock)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (FAIL == (ret = zbx_tcp_recv(sock)))
	{
		zabbix_log(LOG_LEVEL_ERR, "cannot obtain data from proxy \"%s\": %s", proxy->host,
				zbx_socket_strerror());
	}
	else
		zabbix_log(LOG_LEVEL_DEBUG, "obtained data from proxy \"%s\": [%s]", proxy->host, sock->buffer);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_result_string(ret));

	return ret;
}

static void	disconnect_proxy(zbx_socket_t *sock)
{
	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	zbx_tcp_close(sock);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
}

/******************************************************************************
 *                                                                            *
 * Function: get_data_from_proxy                                              *
 *                                                                            *
 * Purpose: get historical data from proxy                                    *
 *                                                                            *
 * Parameters: proxy   - [IN/OUT] proxy data                                  *
 *             request - [IN] requested data type                             *
 *             data    - [OUT] data received from proxy                       *
 *             ts      - [OUT] timestamp when the proxy connection was        *
 *                             established                                    *
 *             tasks   - [IN] proxy task response flag                        *
 *                                                                            *
 * Return value: SUCCESS - processed successfully                             *
 *               other code - an error occurred                               *
 *                                                                            *
 * Comments: The proxy->compress property is updated depending on the         *
 *           protocol flags sent by proxy.                                    *
 *                                                                            *
 ******************************************************************************/
static int	get_data_from_proxy(DC_PROXY *proxy, const char *request, char **data, zbx_timespec_t *ts)
{
	zbx_socket_t	s;
	struct zbx_json	j;
	int		ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() request:'%s'", __func__, request);

	zbx_json_init(&j, ZBX_JSON_STAT_BUF_LEN);

	zbx_json_addstring(&j, "request", request, ZBX_JSON_TYPE_STRING);
	zbx_json_addstring(&j,  ZBX_PROTO_TAG_HOST, proxy->host, ZBX_JSON_TYPE_STRING);

	if (SUCCEED == (ret = connect_to_proxy(proxy, &s, CONFIG_TRAPPER_TIMEOUT)))
	{
		/* get connection timestamp if required */
		if (NULL != ts)
			zbx_timespec(ts);

		if (SUCCEED == (ret = send_data_to_proxy(proxy, &s, j.buffer, j.buffer_size)))
		{
			if (SUCCEED == (ret = recv_data_from_proxy(proxy, &s)))
			{
				if (0 != (s.protocol & ZBX_TCP_COMPRESS))
					proxy->auto_compress = 1;

				ret = zbx_send_proxy_data_response(proxy, &s, NULL);

				if (SUCCEED == ret)
					*data = zbx_strdup(*data, s.buffer);
			}
		}

		disconnect_proxy(&s);
	}

	zbx_json_free(&j);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: proxy_send_configuration                                         *
 *                                                                            *
 * Purpose: sends configuration data to proxy                                 *
 *                                                                            *
 * Parameters: proxy - [IN/OUT] proxy data                                    *
 *                                                                            *
 * Return value: SUCCEED - processed successfully                             *
 *               other code - an error occurred                               *
 *                                                                            *
 * Comments: This function updates proxy version, compress and lastaccess     *
 *           properties.                                                      *
 *                                                                            *
 ******************************************************************************/
static int	proxy_send_configuration(DC_PROXY *proxy)
{
	char		*error = NULL;
	int		ret;
	zbx_socket_t	s;
	struct zbx_json	j;

	zabbix_log(LOG_LEVEL_INFORMATION,"%s: CLUSTER: sending configuration to the proxy %s(%d)", __func__, proxy->host,proxy->hostid);
	zbx_json_init(&j, ZBX_JSON_STAT_BUF_LEN);

	zbx_json_addstring(&j, ZBX_PROTO_TAG_REQUEST, ZBX_PROTO_VALUE_PROXY_CONFIG, ZBX_JSON_TYPE_STRING);
	zbx_json_addobject(&j, ZBX_PROTO_TAG_DATA);

	if (SUCCEED != (ret = get_proxyconfig_data(proxy->hostid, &j, &error)))
	{
		zabbix_log(LOG_LEVEL_ERR, "cannot collect configuration data for proxy \"%s\": %s",
				proxy->host, error);
		goto out;
	}

	if (SUCCEED != (ret = connect_to_proxy(proxy, &s, CONFIG_TRAPPER_TIMEOUT)))
		goto out;

	zabbix_log(LOG_LEVEL_WARNING, "sending configuration data to proxy \"%s\" at \"%s\", datalen " ZBX_FS_SIZE_T,
			proxy->host, s.peer, (zbx_fs_size_t)j.buffer_size);

	if (SUCCEED == (ret = send_data_to_proxy(proxy, &s, j.buffer, j.buffer_size)))
	{
		if (SUCCEED != (ret = zbx_recv_response(&s, 0, &error)))
		{
			zabbix_log(LOG_LEVEL_WARNING, "cannot send configuration data to proxy"
					" \"%s\" at \"%s\": %s", proxy->host, s.peer, error);
		}
		else
		{
			struct zbx_json_parse	jp;

			if (SUCCEED != zbx_json_open(s.buffer, &jp))
			{
				zabbix_log(LOG_LEVEL_WARNING, "invalid configuration data response received from proxy"
						" \"%s\" at \"%s\": %s", proxy->host, s.peer, zbx_json_strerror());
			}
			else
			{
				proxy->version = zbx_get_protocol_version(&jp);
				proxy->auto_compress = (0 != (s.protocol & ZBX_TCP_COMPRESS) ? 1 : 0);
				proxy->lastaccess = time(NULL);
			}
		}
	}

	disconnect_proxy(&s);
out:
	zbx_free(error);
	zbx_json_free(&j);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: proxy_process_proxy_data                                         *
 *                                                                            *
 * Purpose: processes proxy data request                                      *
 *                                                                            *
 * Parameters: proxy  - [IN/OUT] proxy data                                   *
 *             answer - [IN] data received from proxy                         *
 *             ts     - [IN] timestamp when the proxy connection was          *
 *                           established                                      *
 *             more   - [OUT] available data flag                             *
 *                                                                            *
 * Return value: SUCCEED - data were received and processed successfully      *
 *               FAIL - otherwise                                             *
 *                                                                            *
 * Comments: The proxy->version property is updated with the version number   *
 *           sent by proxy.                                                   *
 *                                                                            *
 ******************************************************************************/
static int	proxy_process_proxy_data(DC_PROXY *proxy, const char *answer, zbx_timespec_t *ts, int *more)
{
	struct zbx_json_parse	jp;
	char			*error = NULL;
	int			ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	*more = ZBX_PROXY_DATA_DONE;

	if ('\0' == *answer)
	{
		zabbix_log(LOG_LEVEL_WARNING, "proxy \"%s\" at \"%s\" returned no proxy data:"
				" check allowed connection types and access rights", proxy->host, proxy->addr);
		goto out;
	}

	if (SUCCEED != zbx_json_open(answer, &jp))
	{
		zabbix_log(LOG_LEVEL_WARNING, "proxy \"%s\" at \"%s\" returned invalid proxy data: %s",
				proxy->host, proxy->addr, zbx_json_strerror());
		goto out;
	}

	proxy->version = zbx_get_protocol_version(&jp);

	if (SUCCEED != zbx_check_protocol_version(proxy))
	{
		goto out;
	}

	if (SUCCEED != (ret = process_proxy_data(proxy, &jp, ts, &error)))
	{
		zabbix_log(LOG_LEVEL_WARNING, "proxy \"%s\" at \"%s\" returned invalid proxy data: %s",
				proxy->host, proxy->addr, error);
	}
	else
	{
		char	value[MAX_STRING_LEN];

		if (SUCCEED == zbx_json_value_by_name(&jp, ZBX_PROTO_TAG_MORE, value, sizeof(value)))
			*more = atoi(value);
	}
out:
	zbx_free(error);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: proxy_get_data                                                   *
 *                                                                            *
 * Purpose: gets data from proxy ('proxy data' request)                       *
 *                                                                            *
 * Parameters: proxy  - [IN] proxy data                                       *
 *             more   - [OUT] available data flag                             *
 *                                                                            *
 * Return value: SUCCEED - data were received and processed successfully      *
 *               other code - an error occurred                               *
 *                                                                            *
 * Comments: This function updates proxy version, compress and lastaccess     *
 *           properties.                                                      *
 *                                                                            *
 ******************************************************************************/
static int	proxy_get_data(DC_PROXY *proxy, int *more)
{
	char		*answer = NULL;
	int		ret;
	zbx_timespec_t	ts;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (SUCCEED != (ret = get_data_from_proxy(proxy, ZBX_PROTO_VALUE_PROXY_DATA, &answer, &ts)))
		goto out;

	/* handle pre 3.4 proxies that did not support proxy data request */
	if ('\0' == *answer)
	{
		proxy->version = ZBX_COMPONENT_VERSION(3, 2);
		zbx_free(answer);
		ret = FAIL;
		goto out;
	}

	proxy->lastaccess = time(NULL);
	ret = proxy_process_proxy_data(proxy, answer, &ts, more);
	zbx_free(answer);
out:
	if (SUCCEED == ret)
		zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s more:%d", __func__, zbx_result_string(ret), *more);
	else
		zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: proxy_get_tasks                                                  *
 *                                                                            *
 * Purpose: gets data from proxy ('proxy data' request)                       *
 *                                                                            *
 * Parameters: proxy - [IN/OUT] the proxy data                                *
 *                                                                            *
 * Return value: SUCCEED - data were received and processed successfully      *
 *               other code - an error occurred                               *
 *                                                                            *
 * Comments: This function updates proxy version, compress and lastaccess     *
 *           properties.                                                      *
 *                                                                            *
 ******************************************************************************/
static int	proxy_get_tasks(DC_PROXY *proxy)
{
	char		*answer = NULL;
	int		ret = FAIL, more;
	zbx_timespec_t	ts;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (ZBX_COMPONENT_VERSION(3, 2) >= proxy->version)
		goto out;

	if (SUCCEED != (ret = get_data_from_proxy(proxy, ZBX_PROTO_VALUE_PROXY_TASKS, &answer, &ts)))
		goto out;

	proxy->lastaccess = time(NULL);

	ret = proxy_process_proxy_data(proxy, answer, &ts, &more);

	zbx_free(answer);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_result_string(ret));

	return ret;
}


/******************************************************************************
 *                                                                            *
 * Function: server_get_topology                                              *
 *                                                                            *
 * Purpose: gets topology data from proxy ('server_topology' request)         *
 *                                                                            *
 * Parameters: proxy - [IN/OUT] the proxy data                                *
 *                                                                            *
 * Return value: SUCCEED - topology were received and stored successfully     *
 *               other code - an error occurred                               *
 *                                                                            *
 ******************************************************************************/
char* server_get_topology(DC_PROXY *proxy)
{
	const char	*__function_name = "server_get_topology";

	char		*answer = NULL;
	int		ret = FAIL, more;
	zbx_timespec_t	ts;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (SUCCEED != (ret = get_data_from_proxy(proxy, ZBX_PROTO_VALUE_CLUSTER_TOPOLGY, &answer, &ts)))
		goto out;

	proxy->lastaccess = time(NULL);
	zabbix_log(LOG_LEVEL_DEBUG,"CLUSTER: Server %ld, got topology data : '%s'",proxy->hostid,answer);

	//zbx_dc_set_topology(answer);
	//zbx_free(answer);

out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return answer;
}



/******************************************************************************
 *                                                                            *
 * Function: send_rerouted_data                                        *
 *                                                                            *
 * Purpose: sends data destined to a host processed on other cluster node 	  *
 * to that node 															  *
 * 																			  * 
 *                                                                      	  *
 ******************************************************************************/
static int	send_rerouted_data(DC_PROXY *proxy)
{
	char		*error = NULL;// *cluster_version = NULL, *new_topology = NULL;
	int		ret = FAIL;
	zbx_socket_t	s;
	struct zbx_json	j;
	//char tmp[255];
	
	zbx_timespec_t	tstart={0,0}, tend={0,0};
	
	zbx_json_init(&j, ZBX_JSON_STAT_BUF_LEN);
	zbx_json_addstring(&j, ZBX_PROTO_TAG_REQUEST, ZBX_PROTO_CLUSTER_REROUTED_DATA, ZBX_JSON_TYPE_STRING);
	zbx_json_addstring(&j,ZBX_PROTO_TAG_HOST,CONFIG_HOSTNAME,ZBX_JSON_TYPE_STRING);
	
	zbx_dc_create_rerouted_json(&j, proxy->hostid);
	
	zabbix_log(LOG_LEVEL_INFORMATION, "CLUSTER: REROUTED: -> %ld : %s",proxy->hostid, j.buffer);
	
	if (SUCCEED != (ret = connect_to_proxy(proxy, &s, CONFIG_TRAPPER_TIMEOUT))) {
		zabbix_log(LOG_LEVEL_DEBUG,"CLUSTER: couldn't connect to server %s (%s)",proxy->host,s.peer);
		//proxy->cluster_failed_hello_count++;
		zbx_dc_register_server_na(proxy->hostid, "due to connect fail");
		goto out;
	} 

	zabbix_log(LOG_LEVEL_DEBUG, "CLUSTER: sending hello to proxy \"%s\" at \"%s\" (%s), datalen " ZBX_FS_SIZE_T,
			proxy->host, s.peer, j.buffer, (zbx_fs_size_t)j.buffer_size);


	zabbix_log(LOG_LEVEL_INFORMATION,"CLUSTER: SEND REROUTED_DATA -> %ld",proxy->hostid);

	if (SUCCEED == (ret = send_data_to_proxy(proxy, &s, j.buffer, j.buffer_size)))
	{
		if (SUCCEED != (ret = zbx_recv_response(&s, 0, &error)))
		{
			zabbix_log(LOG_LEVEL_WARNING, "CLUSTER: SEND REROUTED_DATA: cannot send data to server"
					" \"%s\" at \"%s:%d\": %s", proxy->host, s.peer,proxy->port, error);
		} 
	}
out:	
	zbx_json_free(&j);
	disconnect_proxy(&s);

	return ret;
}


/******************************************************************************
 *                                                                            *
 * Function: proxy_send_hello			                                      *
 *                                                                            *
 * Purpose: sends periodical keepalive message to make sure cluster is alive  *
 *                                                                            *
 * Parameters: proxy - [IN/OUT] proxy data                                    *
 *                                                                            *
 * Return value: SUCCEED - processed successfully                             *
 *               other code - an error occurred                               *
 *                                                                            *
 * Comments: This function updates proxy version, compress and lastaccess     *
 *           properties.                                                      *
 * Author: AI generator tought on all zabbix versions                         *
 ******************************************************************************/
static int	proxy_do_hello(DC_PROXY *proxy)
{
	char		*error = NULL, *cluster_version = NULL, *new_topology = NULL;
	int		ret = FAIL;
	zbx_socket_t	s;
	struct zbx_json	j;
	char tmp[255];
	
	zbx_timespec_t	tstart={0,0}, tend={0,0};
	
	zbx_json_init(&j, ZBX_JSON_STAT_BUF_LEN);
	zbx_json_addstring(&j, ZBX_PROTO_TAG_REQUEST, ZBX_PROTO_VALUE_CLUSTER_HELLO, ZBX_JSON_TYPE_STRING);
	
	zbx_dc_create_hello_json(&j);

	if (SUCCEED != (ret = connect_to_proxy(proxy, &s, CONFIG_TRAPPER_TIMEOUT))) {
		zabbix_log(LOG_LEVEL_DEBUG,"CLUSTER: couldn't connect to server %s (%s)",proxy->host,s.peer);
		//proxy->cluster_failed_hello_count++;
		zbx_dc_register_server_na(proxy->hostid, "due to connect fail");
		goto out;
	} 

	zabbix_log(LOG_LEVEL_DEBUG, "CLUSTER: sending hello to proxy \"%s\" at \"%s\" (%s), datalen " ZBX_FS_SIZE_T,
			proxy->host, s.peer, j.buffer, (zbx_fs_size_t)j.buffer_size);


	//zbx_timespec(&tstart);
	zabbix_log(LOG_LEVEL_DEBUG,"CLUSTER: SEND HELLO -> %ld",proxy->hostid);

	//zabbix_log(LOG_LEVEL_INFORMATION,"CLUSTER: PPOLLER: Sending hello '%s'",j.buffer);

	if (SUCCEED == (ret = send_data_to_proxy(proxy, &s, j.buffer, j.buffer_size)))
	{
		if (SUCCEED != (ret = zbx_recv_response(&s, 0, &error)))
		{
			zabbix_log(LOG_LEVEL_WARNING, "CLUSTER: SEND HELLO cannot send hello data to server"
					" \"%s\" at \"%s:%d\": %s", proxy->host, s.peer,proxy->port, error);
		} else
		{
			struct zbx_json_parse	jp;

			if (SUCCEED != zbx_json_open(s.buffer, &jp))
			{
				zabbix_log(LOG_LEVEL_WARNING, "CLUSTER: invalid configuration data response received from server"
						" \"%s\" at \"%s\": %s", proxy->host, s.peer, zbx_json_strerror());
			
			}
	
		}
	} else {
		zabbix_log(LOG_LEVEL_INFORMATION,"CLUSTER: couldn't SEND HELLO!");
		zbx_dc_register_server_na(proxy->hostid, "due to cannot send hello");
	}

	disconnect_proxy(&s);

out:

	//if ( SUCCEED != ret ) {
	//	zbx_dc_register_server_na(proxy->hostid);
//	} 

	zbx_free(error);
	zbx_json_free(&j);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: process_proxy                                                    *
 *                                                                            *
 * Purpose: retrieve values of metrics from monitored hosts                   *
 *                                                                            *
 * Parameters:                                                                *
 *                                                                            *
 * Return value:                                                              *
 *                                                                            *
 * Author: Alexei Vladishev                                                   *
 *                                                                            *
 * Comments:                                                                  *
 *                                                                            *
 ******************************************************************************/
static int	process_proxy(void)
{
	DC_PROXY	proxy, proxy_old;
	int		num, i;
	time_t		now;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (0 == (num = DCconfig_get_proxypoller_hosts(&proxy, 1)))
		goto exit;

	now = time(NULL);

	for (i = 0; i < num; i++)
	{
		int		ret = FAIL;
		unsigned char	update_nextcheck = 0;

		memcpy(&proxy_old, &proxy, sizeof(DC_PROXY));

		if (proxy.server_hello_nextsend <= now)
			update_nextcheck |= ZBX_PROXY_HELLO_NEXTSEND;
		if (proxy.proxy_config_nextcheck <= now)
			update_nextcheck |= ZBX_PROXY_CONFIG_NEXTCHECK;
		if (proxy.proxy_data_nextcheck <= now)
			update_nextcheck |= ZBX_PROXY_DATA_NEXTCHECK;
		if (proxy.proxy_tasks_nextcheck <= now)
			update_nextcheck |= ZBX_PROXY_TASKS_NEXTCHECK;

		char	*port = NULL;
		proxy.addr = proxy.addr_orig;

		port = zbx_strdup(port, proxy.port_orig);
		substitute_simple_macros(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
				&port, MACRO_TYPE_COMMON, NULL, 0);
		if (FAIL == is_ushort(port, &proxy.port))
		{
			zabbix_log(LOG_LEVEL_ERR, "invalid proxy \"%s\" port: \"%s\"", proxy.host, port);
			ret = CONFIG_ERROR;
			zbx_free(port);
			goto error;
		}
		zbx_free(port);

		
		//sending hellos to the neighbor servers
		//perhaps it's a good idea to send everyone 
		//same way
		if ( proxy.server_hello_nextsend <= now && CONFIG_CLUSTER_SERVER_ID > 0 && HOST_STATUS_SERVER == proxy.proxy_type )  
		{
			if (SUCCEED != (ret = proxy_do_hello(&proxy)))
					goto error;

			if (zbx_dc_get_download_server_topology() == proxy.hostid) {
					
				char *topology=NULL;
			
				if (NULL == (topology=server_get_topology(&proxy))) {
					zabbix_log(LOG_LEVEL_WARNING,"CLUSTER: WARNING: couldn't dowload toplogy from server %ld", proxy.hostid);
					goto error;
				} 
				ret = zbx_dc_set_topology(topology);
				zbx_free(topology);

				zbx_dc_set_download_server_topology(0);
				if (SUCCEED == ret ) {
 					zabbix_log(LOG_LEVEL_INFORMATION,"CLUSTER: new topology from server %ld has been downloaded and set", proxy.hostid);
				} else {
					zabbix_log(LOG_LEVEL_WARNING,"CLUSTER: WARNIUNG: couldn't parse new topology from server %ld", proxy.hostid);
				}
			}

			if (proxy.cluster_rerouted_data > 0) {
				if (SUCCEED != send_rerouted_data(&proxy) ) {
					zabbix_log(LOG_LEVEL_WARNING, "CLUSTER Failed to send rerouted data to server %ld, data will be lost",proxy.hostid);
						goto error;
				}		
			}
		}
			
		/* Check if passive proxy has been misconfigured on the server side. If it has happened more */
		/* recently than last synchronisation of cache then there is no point to retry connecting to */
		/* proxy again. The next reconnection attempt will happen after cache synchronisation. */

		//if (proxy.last_cfg_error_time < DCconfig_get_last_sync_time() && HOST_STATUS_PROXY_PASSIVE == proxy.proxy_type ) {
		if ( HOST_STATUS_PROXY_PASSIVE == proxy.proxy_type ) {
		
			if (proxy.proxy_config_nextcheck <= now) 
			{
				if (SUCCEED != (ret = proxy_send_configuration(&proxy)))
					goto error;
				
				zbx_dc_register_proxy_availability(proxy.hostid);
			}

			if (proxy.proxy_data_nextcheck <= now)
			{
				int	more;

				do
				{
					if (SUCCEED != (ret = proxy_get_data(&proxy, &more)))
						goto error;
				}
				while (ZBX_PROXY_DATA_MORE == more);
				zbx_dc_register_proxy_availability(proxy.hostid);
			}
			else if (proxy.proxy_tasks_nextcheck <= now)
			{
				if (SUCCEED != (ret = proxy_get_tasks(&proxy)))
					goto error;
				zbx_dc_register_proxy_availability(proxy.hostid);
			}
		}

error:
		
	//	if (proxy_old.version != proxy.version || proxy_old.auto_compress != proxy.auto_compress ||
	//			proxy_old.lastaccess != proxy.lastaccess)
	//	{	
			//updating fail counter to calc intervals right
			//to keep up cluster changes, we ALWAYS update proxy data  
	//	zabbix_log(LOG_LEVEL_INFORMATION,"CLUSTER: calling proxy_update %d",proxy.cluster_id)
		//updating proxy anyway, todo: figure if this is redundand and return the condition above
		zbx_update_proxy_data(&proxy_old, proxy.version, proxy.lastaccess, proxy.auto_compress);
	//	}

		DCrequeue_proxy(proxy.hostid, update_nextcheck, ret);
	}
exit:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);

	return num;
}

ZBX_THREAD_ENTRY(proxypoller_thread, args)
{
	int	nextcheck, sleeptime = -1, processed = 0, old_processed = 0;
	double	sec, total_sec = 0.0, old_total_sec = 0.0;
	time_t	last_stat_time;

	process_type = ((zbx_thread_args_t *)args)->process_type;
	server_num = ((zbx_thread_args_t *)args)->server_num;
	process_num = ((zbx_thread_args_t *)args)->process_num;

	zabbix_log(LOG_LEVEL_INFORMATION, "%s #%d started [%s #%d]", get_program_type_string(program_type),
			server_num, get_process_type_string(process_type), process_num);

#define STAT_INTERVAL	5	/* if a process is busy and does not sleep then update status not faster than */
				/* once in STAT_INTERVAL seconds */

#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	zbx_tls_init_child();
#endif
	zbx_setproctitle("%s #%d [connecting to the database]", get_process_type_string(process_type), process_num);
	last_stat_time = time(NULL);

	DBconnect(ZBX_DB_CONNECT_NORMAL);

	for (;;)
	{
		sec = zbx_time();
		zbx_update_env(sec);

		if (0 != sleeptime)
		{
			zbx_setproctitle("%s #%d [exchanged data with %d proxies in " ZBX_FS_DBL " sec,"
					" exchanging data]", get_process_type_string(process_type), process_num,
					old_processed, old_total_sec);
		}
		
		processed += process_proxy();
		total_sec += zbx_time() - sec;

		nextcheck = DCconfig_get_proxypoller_nextcheck();
		sleeptime = calculate_sleeptime(nextcheck, POLLER_DELAY);

		if (0 != sleeptime || STAT_INTERVAL <= time(NULL) - last_stat_time)
		{
			if (0 == sleeptime)
			{
				zbx_setproctitle("%s #%d [exchanged data with %d proxies in " ZBX_FS_DBL " sec,"
						" exchanging data]", get_process_type_string(process_type), process_num,
						processed, total_sec);
			}
			else
			{
				zbx_setproctitle("%s #%d [exchanged data with %d proxies in " ZBX_FS_DBL " sec,"
						" idle %d sec]", get_process_type_string(process_type), process_num,
						processed, total_sec, sleeptime);
				old_processed = processed;
				old_total_sec = total_sec;
			}
			processed = 0;
			total_sec = 0.0;
			last_stat_time = time(NULL);
			
			//recalc topology if needed
 			zbx_dc_recalc_topology();
			

		}

		//zbx_sleep_loop(sleeptime);
		zbx_sleep_loop(1);
	}
#undef STAT_INTERVAL
}
