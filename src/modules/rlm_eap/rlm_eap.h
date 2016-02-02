/*
 * rlm_eap.h    Local Header file.
 *
 * Version:     $Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2001  hereUare Communications, Inc. <raghud@hereuare.com>
 * Copyright 2003  Alan DeKok <aland@freeradius.org>
 * Copyright 2006  The FreeRADIUS server project
 */
#ifndef _RLM_EAP_H
#define _RLM_EAP_H

RCSIDH(rlm_eap_h, "$Id$")

#include <freeradius-devel/modpriv.h>
#include <freeradius-devel/state.h>
#include "eap.h"
#include "eap_types.h"

/*
 * Keep track of which sub modules we've loaded.
 */
typedef struct eap_module {
	char const		*name;
	rlm_eap_module_t	*type;
	void			*handle;
	CONF_SECTION		*cs;
	void			*instance;
} eap_module_t;

/*
 * This structure contains eap's persistent data.
 * sessions = remembered sessions, in a tree for speed.
 * types = All supported EAP-Types
 * mutex = ensure only one thread is updating the sessions[] struct
 */
typedef struct rlm_eap {
	eap_module_t 	*methods[PW_EAP_MAX_TYPES];

	char const	*default_method_name;
	eap_type_t	default_method;

	bool		ignore_unknown_types;
	bool		mod_accounting_username_bug;

#ifdef HAVE_PTHREAD_H
	pthread_mutex_t	session_mutex;
#endif

	char const	*name;
	fr_randctx	rand_pool;
} rlm_eap_t;

/*
 *	For simplicity in the rest of the code.
 */
#ifndef HAVE_PTHREAD_H
/*
 *	This is easier than ifdef's throughout the code.
 */
#define pthread_mutex_init(_x, _y)
#define pthread_mutex_destroy(_x)
#define pthread_mutex_lock(_x)
#define pthread_mutex_unlock(_x)
#endif

/*
 *	EAP Method selection
 */
int      	eap_module_instantiate(rlm_eap_t *inst, eap_module_t **method, eap_type_t num, CONF_SECTION *cs);
eap_rcode_t	eap_method_select(rlm_eap_t *inst, eap_session_t *eap_session);

/*
 *	EAP Method composition
 */
int  		eap_start(rlm_eap_t *inst, REQUEST *request) CC_HINT(nonnull);
void 		eap_fail(eap_session_t *eap_session) CC_HINT(nonnull);
void 		eap_success(eap_session_t *eap_session) CC_HINT(nonnull);
rlm_rcode_t 	eap_compose(eap_session_t *eap_session) CC_HINT(nonnull);

/*
 *	Session management
 */
void		eap_session_destroy(eap_session_t **eap_session);
void		eap_session_freeze(eap_session_t **eap_session);
eap_session_t	*eap_session_thaw(REQUEST *request);
eap_session_t 	*eap_session_continue(eap_packet_raw_t **eap_packet, rlm_eap_t *inst, REQUEST *request) CC_HINT(nonnull);

/*
 *	Memory management
 */
eap_round_t	*eap_round_alloc(eap_session_t *eap_session) CC_HINT(nonnull);
eap_session_t	*eap_session_alloc(rlm_eap_t *inst, REQUEST *request) CC_HINT(nonnull);

#endif /*_RLM_EAP_H*/
