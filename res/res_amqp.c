/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright 2015-2020 Wazo, Inc.
 *
 * David M. Lee, II <dlee@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief AMQP client APIs for AMQP.
 * \author David M. Lee, II <dlee@digium.com>
 * \author Sylvain Afchain <safchain@wazo.io>
 *
 * This is mostly a thin wrapper around some of the <a href="http://alanxz.github.io/rabbitmq-c">rabbitmq-c APIs</a>,
 * with additional features for thread safety and connection management.
 */

/*** MODULEINFO
   <depend>rabbitmq</depend>
   <support_level>core</support_level>
 ***/

/*** DOCUMENTATION
   <configInfo name="res_amqp" language="en_US">
       <synopsis>AMQP client API</synopsis>
       <configFile name="amqp.conf">
           <configObject name="general">
               <synopsis>General configuration settings</synopsis>
               <configOption name="enabled">
                   <synopsis>Enable/disable the AMQP module</synopsis>
                   <description>
                       <para>This option enables or disables the AMQP module.</para>
                   </description>
               </configOption>
           </configObject>

           <configObject name="connection">
               <synopsis>Per-connection configuration settings</synopsis>
               <configOption name="type">
                   <synopsis>Define this configuration section as a connection.</synopsis>
                   <description>
                       <enumlist>
                           <enum name="connection"><para>Configure this section as a <replaceable>connection</replaceable></para></enum>
                       </enumlist>
                   </description>
               </configOption>
               <configOption name="url">
                   <synopsis>URL to connect to</synopsis>
                   <description>
                       <para>URL of the AMQP server to connect to. Is of the form <literal>amqp://[$USERNAME[:$PASSWORD]@]$HOST[:$PORT]/[$VHOST]</literal></para>
                   </description>
               </configOption>
               <configOption name="password">
                   <synopsis>Password for AMQP login</synopsis>
                   <description>
                       <para>When the AMQP server requires login, specified the login password</para>
                   </description>
               </configOption>
               <configOption name="max_frame_bytes">
                   <synopsis>The maximum size of an AMQP frame on the wire to request of the broker for this connection.</synopsis>
                   <description>
                       <para>4096 is the minimum size, 2^31-1 is the maximum</para>
                   </description>
               </configOption>
               <configOption name="heartbeat_seconds">
                   <synopsis>the number of seconds between heartbeat frames to request of the broker</synopsis>
                   <description>
                       <para>A value of 0 disables heartbeats.</para>
                   </description>
               </configOption>
           </configObject>
       </configFile>
   </configInfo>
 ***/


#include "asterisk.h"

#include "asterisk/module.h"
#include "asterisk/res_amqp.h"
#include "amqp/internal.h"

#include <amqp.h>
#include <amqp_framing.h>
#include <amqp_tcp_socket.h>


#define NUM_ACTIVE_CONNECTION_BUCKETS 31
#define CHANNEL_ID 1

static struct ao2_container *active_connections;

struct recv_thread_args {
	struct ast_amqp_connection *cxn;
	void (*on_error_cb)(struct ast_amqp_connection * cxn);
	void (*on_exit_cb)(struct ast_amqp_connection * cxn);
};

static void *recv_thread(void *arg)
{
	struct recv_thread_args *args = arg;
	struct ast_amqp_connection *cxn = args->cxn;

	amqp_connection_state_t state;
	{
		SCOPED_AO2LOCK(lock, cxn);
		state = cxn->state;

		ast_debug(3, "AMQP: Start receive loop %s\n", cxn->name);
	}

	for (;;) {
		amqp_envelope_t envelope;
		amqp_rpc_reply_t ret;
		struct timeval tv = {
			.tv_sec = 0,
			.tv_usec = 100000,
		};

		{
			SCOPED_AO2LOCK(lock, cxn);
			if (!cxn->running) {
				break;
			}
		}

		amqp_maybe_release_buffers(state);
		ret = amqp_consume_message(state, &envelope, &tv, 0);
		amqp_destroy_envelope(&envelope);

		if (ret.reply_type == AMQP_RESPONSE_NORMAL ||
			(ret.reply_type == AMQP_RESPONSE_LIBRARY_EXCEPTION &&
			 ret.library_error == AMQP_STATUS_TIMEOUT)) {
			continue;
		}

		if (args->on_error_cb) {
			ast_log(LOG_ERROR, "AMQP: message error code %d\n", ret.reply_type);
			args->on_error_cb(cxn);
		}
	}

	{
		SCOPED_AO2LOCK(lock, cxn);
		ast_debug(3, "AMQP: Receive loop stopped %s\n", cxn->name);
	}

	if (args->on_exit_cb) {
		args->on_exit_cb(cxn);
	}

	ast_free(args);

	return NULL;
}

static int amqp_connection_hash(const void *obj, int flags)
{
	const struct ast_amqp_connection *cxn = obj;
	const char *key;

	switch (flags & OBJ_SEARCH_MASK) {
	case OBJ_SEARCH_KEY:
		key = obj;
		break;
	case OBJ_SEARCH_OBJECT:
		cxn = obj;
		key = cxn->name;
		break;
	default:
		/* Hash can only work on something with a full key. */
		ast_assert(0);
		return 0;
	}

	return ast_str_hash(key);
}

static int amqp_connection_cmp(void *obj_left, void *arg, int flags)
{
	const struct ast_amqp_connection *cxn_left = obj_left;
	const struct ast_amqp_connection *cxn_right = arg;
	const char *right_key = arg;
	int cmp;

	switch (flags & OBJ_SEARCH_MASK) {
	case OBJ_SEARCH_OBJECT:
		right_key = cxn_right->name;
		/* Fall through */
	case OBJ_SEARCH_KEY:
		cmp = strcmp(cxn_left->name, right_key);
		break;
	case OBJ_SEARCH_PARTIAL_KEY:
		cmp = strncmp(cxn_left->name, right_key, strlen(right_key));
		break;
	default:
		cmp = 0;
		break;
	}

	if (cmp) {
		return 0;
	}

	return CMP_MATCH;
}

static void amqp_connection_dtor(void *obj)
{
	struct ast_amqp_connection *cxn = obj;

	SCOPED_AO2LOCK(lock, cxn);
	ast_debug(3, "AMQP: Destroying connection %s\n", cxn->name);

	amqp_destroy_connection(cxn->state);
	cxn->state = NULL;
}

void ast_amqp_connection_close(struct ast_amqp_connection *cxn)
{
	SCOPED_AO2LOCK(lock, cxn);
	cxn->running = 0;
}

static void amqp_connection_wait_close(struct ast_amqp_connection *cxn)
{
	SCOPED_AO2LOCK(connections_lock, active_connections);

	pthread_join(cxn->recv_thread, NULL);

	ao2_unlink(active_connections, cxn);
}

static struct ast_amqp_connection *amqp_connection_create(const char *name)
{
	RAII_VAR(struct ast_amqp_connection *, cxn, NULL, ao2_cleanup);
	RAII_VAR(struct amqp_conf_connection *, cxn_conf, NULL, ao2_cleanup);
	amqp_socket_t *socket = NULL;
	amqp_rpc_reply_t login_reply;
	const char *password;

	ast_debug(3, "AMQP: Creating connection %s\n", name);

	cxn_conf = amqp_config_get_connection(name);
	if (!cxn_conf) {
		ast_log(LOG_WARNING, "AMQP: No config for connection '%s'\n", name);
		return NULL;
	}

	cxn = ao2_alloc(sizeof(*cxn) + strlen(name) + 1, amqp_connection_dtor);
	if (!cxn) {
		ast_log(LOG_ERROR, "AMQP: Allocation failed\n");
		return NULL;
	}

	strcpy(cxn->name, name);

	cxn->state = amqp_new_connection();
	if (!cxn->state) {
		ast_log(LOG_ERROR, "AMQP: Allocation failed\n");
		return NULL;
	}

	socket = amqp_tcp_socket_new(cxn->state);
	if (!socket) {
		ast_log(LOG_ERROR, "AMQP: Failed to create socket\n");
		return NULL;
	}

	ast_debug(3, "AMQP: Open socket %s:%d\n", cxn_conf->current_url->info.host,
			  cxn_conf->current_url->info.port);
	if (amqp_socket_open
		(socket, cxn_conf->current_url->info.host,
		 cxn_conf->current_url->info.port) != 0) {
		ast_log(LOG_ERROR, "AMQP: Could not connect to %s:%d\n",
				cxn_conf->current_url->info.host, cxn_conf->current_url->info.port);
		return NULL;
	}

	/* The password may be in the URL, but we also allow them to put
	 * it in the config file directly, so it doesn't show on the status
	 * screen */
	password = cxn_conf->password;
	if (!password) {
		password = cxn_conf->current_url->info.password;
	}

	login_reply = amqp_login(cxn->state, cxn_conf->current_url->info.vhost, 1,	/* max_channels; we only use one */
							 cxn_conf->max_frame_bytes,
							 cxn_conf->heartbeat_seconds,
							 AMQP_SASL_METHOD_PLAIN,
							 cxn_conf->current_url->info.user, password);
	if (login_reply.reply_type != AMQP_RESPONSE_NORMAL) {
		ast_log(LOG_ERROR, "AMQP: Login error\n");
		return NULL;
	}

	/*
	 * Open a channel for messaging. The AMQP supports a 'lightweight'
	 * channel concept which allows multiplexing requests over a
	 * 'heavyweight' TCP socket. Unfortunately, librabbitmq isn't
	 * thread safe, so this multiplexing isn't very useful. We will
	 * simplify things and just use a single channel.
	 */
	if (amqp_channel_open(cxn->state, CHANNEL_ID) == 0) {
		ast_log(LOG_ERROR, "AMQP: Error opening channel\n");
		return NULL;
	}

	return ao2_bump(cxn);
}

struct ast_amqp_connection *ast_amqp_get_connection(const char *name)
{
	SCOPED_AO2LOCK(connections_lock, active_connections);
	struct ast_amqp_connection *cxn =
		ao2_find(active_connections, name, OBJ_SEARCH_KEY | OBJ_NOLOCK);
	return cxn;
}

struct ast_amqp_connection *ast_amqp_get_or_create_connection(const char *name,
															  ast_amqp_cxn_create_cb
															  handler)
{
	SCOPED_AO2LOCK(connections_lock, active_connections);
	struct ast_amqp_connection *cxn =
		ao2_find(active_connections, name, OBJ_SEARCH_KEY | OBJ_NOLOCK);
	struct recv_thread_args *recv_args;

	if (!cxn) {
		cxn = amqp_connection_create(name);
		if (!cxn) {
			return NULL;
		}

		if (!ao2_link_flags(active_connections, cxn, OBJ_NOLOCK)) {
			ast_log(LOG_ERROR, "AMQP: Allocation failed\n");
			ao2_cleanup(cxn);
			return NULL;
		}

		if (handler) {
			if (handler(cxn) == -1) {
				ast_log(LOG_ERROR, "AMQP: Error from connection creation handler\n");
				ao2_cleanup(cxn);
				return NULL;
			}
		}
		recv_args = ast_calloc(1, sizeof(struct recv_thread_args));
		if (!recv_args) {
			ao2_cleanup(cxn);
			return NULL;
		}
		recv_args->cxn = cxn;
		recv_args->on_error_cb = ast_amqp_connection_close;
		recv_args->on_exit_cb = amqp_connection_wait_close;

		cxn->running = 1;
		ast_pthread_create_background(&cxn->recv_thread, NULL, recv_thread, recv_args);
	}

	return cxn;
}

int ast_amqp_declare_exchange(struct ast_amqp_connection *cxn,
							  const char *exchange, const char *type)
{
	if (!cxn) {
		return -1;
	}

	{
		SCOPED_AO2LOCK(lock, cxn);
		if (!cxn->state) {
			return -1;
		}

		if (amqp_exchange_declare(cxn->state, CHANNEL_ID, amqp_cstring_bytes(exchange),
								  amqp_cstring_bytes(type), 0, 1, 0, 0,
								  amqp_empty_table) == 0) {
			ast_log(LOG_ERROR, "AMQP: Error declaring exchange\n");
			return -1;
		}
	}

	return 0;
}

int ast_amqp_basic_publish(struct ast_amqp_connection *cxn,
						   amqp_bytes_t exchange,
						   amqp_bytes_t routing_key,
						   amqp_boolean_t mandatory,
						   amqp_boolean_t immediate,
						   const amqp_basic_properties_t * properties, amqp_bytes_t body)
{
	if (!cxn) {
		return -1;
	}

	{
		SCOPED_AO2LOCK(lock, cxn);
		if (!cxn->state) {
			return -1;
		}

		int res = amqp_basic_publish(cxn->state, CHANNEL_ID, exchange, routing_key,
									 mandatory, immediate, properties, body);
		char *err;
		char unknown[80];

		switch (res) {
		case AMQP_STATUS_OK:
			return 0;
		case AMQP_STATUS_TIMER_FAILURE:
			err = "timer failure";
			break;
		case AMQP_STATUS_HEARTBEAT_TIMEOUT:
			err = "heartbeat timeout";
			break;
		case AMQP_STATUS_NO_MEMORY:
			err = "no memory";
			break;
		case AMQP_STATUS_TABLE_TOO_BIG:
			err = "table too big";
			break;
		case AMQP_STATUS_CONNECTION_CLOSED:
			err = "connection closed";
			break;
		case AMQP_STATUS_SSL_ERROR:
			err = "SSL error";
			break;
		case AMQP_STATUS_TCP_ERROR:
			err = "TCP error";
			break;
		case AMQP_STATUS_SOCKET_ERROR:
			err = "Socket error";
			break;
		default:
			snprintf(unknown, sizeof(unknown), "code %d", res);
			err = unknown;
			break;
		}
		ast_log(LOG_ERROR, "AMQP: Publishing error: %s\n", err);

		// request for closing
		ast_amqp_connection_close(cxn);

		return -1;
	}
}

static int load_module(void)
{
	ast_debug(3, "AMQP: Loading client v%s\n", amqp_version());

	if (amqp_config_init() != 0) {
		ast_log(LOG_ERROR, "AMQP: Failed to init config\n");
		return AST_MODULE_LOAD_DECLINE;
	}

	active_connections =
		ao2_container_alloc_hash(AO2_ALLOC_OPT_LOCK_MUTEX, 0,
								 NUM_ACTIVE_CONNECTION_BUCKETS, amqp_connection_hash,
								 NULL, amqp_connection_cmp);

	if (!active_connections) {
		ast_log(LOG_ERROR, "AMQP: Allocation failure\n");
		return AST_MODULE_LOAD_FAILURE;
	}

	if (amqp_cli_register() != 0) {
		ast_log(LOG_ERROR, "AMQP: Failed to register CLI\n");
		return AST_MODULE_LOAD_FAILURE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	amqp_cli_unregister();
	amqp_config_destroy();
	return 0;
}

static int reload_module(void)
{
	if (amqp_config_reload() != 0) {
		return AST_MODULE_LOAD_DECLINE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_GLOBAL_SYMBOLS | AST_MODFLAG_LOAD_ORDER, "AMQP Interface",
				.support_level = AST_MODULE_SUPPORT_CORE,
                                .load = load_module,
				.unload = unload_module,
                                .reload = reload_module,
                                .load_pri = AST_MODPRI_APP_DEPEND);
