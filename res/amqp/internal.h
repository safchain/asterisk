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

#ifndef _ASTERISK_AMQP_INTERNAL_H_
#define _ASTERISK_AMQP_INTERNAL_H_

#include "asterisk/stringfields.h"

#include <amqp.h>

/*! \file
 *
 * \brief Internal API's for res_amqp.
 * \author David M. Lee, II <dlee@digium.com>
 * \author Sylvain Afchain <safchain@wazo.io>
 */

/*! @{ */

/*!
 * \brief Register the amqp commands
 *
 * \return 0 on success.
 * \return -1 on failure.
 */
int amqp_cli_register(void);

/*!
 * \brief Unregister the amqp commands
 *
 * \return 0 on success.
 * \return -1 on failure.
 */
int amqp_cli_unregister(void);

/*! @} */

/*! @{ */

struct amqp_conf_general;

/*! \brief Rabbitmq configuration structure */
struct amqp_conf {
	/*! The general section configuration options */
	struct amqp_conf_general *general;
	/*! Configured connections */
	struct ao2_container *connections;
};

/*! \brief General configuration options for AMQP */
struct amqp_conf_general {
	/*! Enabled by default, disabled if false. */
	int enabled;
};

/*! \brief AMQP URL kept in different format */
struct amqp_url {
	/*! Original URL from the config file */
	char *raw;
	/*! Pointer to the parsed version of the URL */
	char *parsed;
	/*! Information available from URL */
	struct amqp_connection_info info;
};

/*! \brief AMQP per-connection configuration */
struct amqp_conf_connection {
	AST_DECLARE_STRING_FIELDS(
								 /*! The name of the connection */
								 AST_STRING_FIELD(name);
								 /*! The URL to connect to */
								 AST_STRING_FIELD(url);
								 /*! The password to use for authentication */
								 AST_STRING_FIELD(password);
		);

	/*! Max allowed frame size */
	int max_frame_bytes;
	/*! Number of seconds between heartbeats */
	int heartbeat_seconds;

	/*! \brief List of url */
	struct ao2_container *urls;

	/*! \brief Current url used */
	struct amqp_url *current_url;
};

/*! \brief AMQP per-connection state */
struct ast_amqp_connection {
	amqp_connection_state_t state;
	pthread_t recv_thread;
	int running;
	char name[];
};

/*!
 * \brief Initialize AMQP configuration.
 *
 * \return 0 on success.
 * \return -1 on failure.
 */
int amqp_config_init(void);

/*!
 * \brief Reload AMQP configuration.
 *
 * \return 0 on success.
 * \return -1 on failure.
 */
int amqp_config_reload(void);

/*!
 * \brief Destroy AMQP configuration.
 *
 * \return 0 on success.
 * \return -1 on failure.
 */
void amqp_config_destroy(void);

/*!
 * \brief Get the AMQP configuration object.
 *
 * This object is AO2 managed, and should be freed with \ref ao2_cleanup().
 *
 * \return AMQP configuration.
 * \return \c NULL on error.
 */
struct amqp_conf *amqp_config_get(void);

/*!
 * \brief Get the AMQP configuration object for a connection.
 *
 * This object is AO2 managed, and should be freed with \ref ao2_cleanup().
 *
 * \return AMQP configuration.
 * \return \c NULL on error, or if connection is not configured.
 */
struct amqp_conf_connection *amqp_config_get_connection(const char *name);

/*! @} */

#endif /* _ASTERISK_AMQP_INTERNAL_H_ */
