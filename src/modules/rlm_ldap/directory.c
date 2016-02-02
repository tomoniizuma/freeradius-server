/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file directory.c
 * @brief Determine remote server implementation and capabilities.
 *
 * As described by http://ldapwiki.willeke.com/wiki/Determine%20LDAP%20Server%20Vendor
 *
 * @copyright 2016 The FreeRADIUS Server Project.
 * @copyright 2016 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 */
#include "rlm_ldap.h"

FR_NAME_NUMBER const ldap_directory_type_table[] = {
	{ "Unknown",			LDAP_DIRECTORY_UNKNOWN	},
	{ "Active Directory",		LDAP_DIRECTORY_ACTIVE_DIRECTORY	},
	{ "eDirectory",			LDAP_DIRECTORY_EDIRECTORY },
	{ "OpenLDAP",			LDAP_DIRECTORY_OPENLDAP	},
	{ "Oracle Unified Directory",	LDAP_DIRECTORY_ORACLE_UNIFIED_DIRECTORY },
	{ "Unbound ID",			LDAP_DIRECTORY_UNBOUND_ID },
	{  NULL , -1 }
};

/** Extract useful information from the rootDSE of the LDAP server
 *
 * @param[in] ctx	to allocate ldap_directory_t in.
 * @param[out] out	where to write pointer to new ldap_directory_t struct.
 * @param[in] inst	rlm_ldap configuration.
 * @pconn[in,out] pconn	to use to query the directory.
 * @return
 *	- 0 on success.
 *	- 1 if we failed identifying the directory server.
 *	- -1 on error.
 */
int rlm_ldap_directory_alloc(TALLOC_CTX *ctx, ldap_directory_t **out, rlm_ldap_t *inst, ldap_handle_t **pconn)
{
	static char const	*attrs[] = { "vendorname",
					     "vendorversion",
					     "isGlobalCatalogReady",
					     "objectClass",
					     NULL };
	ldap_rcode_t		status;
	int			entry_cnt;
	int			ldap_errno;
	int			i, num;
	int			rcode = 0;
	struct			berval **values = NULL;
	ldap_directory_t	*directory;

	LDAPMessage *result = NULL, *entry;

	*out = NULL;

	directory = talloc_zero(ctx, ldap_directory_t);
	if (!directory) return -2;
	*out = directory;

	directory->type = LDAP_DIRECTORY_UNKNOWN;

	status = rlm_ldap_search(&result, inst, NULL, pconn, "", LDAP_SCOPE_BASE, "(objectclass=*)", attrs, NULL, NULL);
	switch (status) {
	case LDAP_PROC_SUCCESS:
		break;

	case LDAP_PROC_NO_RESULT:
		WARN("Capability check failed: Can't access rootDSE");
		rcode = 1;
		goto finish;

	default:
		rcode = 1;
		goto finish;
	}

	entry_cnt = ldap_count_entries((*pconn)->handle, result);
	if (entry_cnt != 0) {
		WARN("Capability check failed: Ambiguous result for rootDSE, expected 1 entry, got %i", entry_cnt);
		rcode = 1;
		goto finish;
	}

	entry = ldap_first_entry((*pconn)->handle, result);
	if (!entry) {
		ldap_get_option((*pconn)->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);

		WARN("Capability check failed: Failed retrieving entry: %s", ldap_err2string(ldap_errno));
		rcode = 1;
		goto finish;
	}

	values = ldap_get_values_len((*pconn)->handle, entry, "vendorname");
	if (values) {
		directory->vendor_str = rlm_ldap_berval_to_string(inst, values[0]);
		INFO("Directory vendor: %s", directory->vendor_str);
		ldap_value_free_len(values);
	}

	values = ldap_get_values_len((*pconn)->handle, entry, "vendorversion");
	if (values) {
		directory->version_str = rlm_ldap_berval_to_string(inst, values[0]);
		INFO("Directory version: %s", directory->version_str);

		/*
		 *	Novell eDirectory vendorversion contains eDirectory
		 */
		if (strcasestr(directory->version_str, "eDirectory") == 0) {
			directory->type = LDAP_DIRECTORY_EDIRECTORY;
		/*
		 *	Oracle unified directory vendorversion contains Oracle Unified Directory
		 */
		} else if (strcasestr(directory->version_str, "Oracle Unified Directory") == 0) {
			directory->type = LDAP_DIRECTORY_ORACLE_UNIFIED_DIRECTORY;
		/*
		 *	Unbound directory vendorversion contains UnboundID
		 */
		} else if (strcasestr(directory->version_str, "UnboundID") == 0) {
			directory->type = LDAP_DIRECTORY_UNBOUND_ID;
		}
		ldap_value_free_len(values);
	}

	/*
	 *	isGlobalCatalogReady is only present on ActiveDirectory
	 *	instances. AD doesn't provide vendorname or vendorversion
	 */
	values = ldap_get_values_len((*pconn)->handle, entry, "isGlobalCatalogReady");
	if (values) {
		directory->type = LDAP_DIRECTORY_ACTIVE_DIRECTORY;
		ldap_value_free_len(values);
	}

	/*
	 *	OpenLDAP has a special objectClass for its RootDSE
	 */
	values = ldap_get_values_len((*pconn)->handle, entry, "objectClass");
	if (values) {
		num = ldap_count_values_len(values);
		for (i = 0; i < num; i++) {
			if (strncmp("OpenLDAProotDSE", values[i]->bv_val, values[i]->bv_len) == 0) {
				directory->type = LDAP_DIRECTORY_OPENLDAP;
			}
		}
		ldap_value_free_len(values);
	}

	INFO("Directory type: %s", fr_int2str(ldap_directory_type_table, directory->type, "<INVALID>"));

	switch (directory->type) {
	case LDAP_DIRECTORY_ACTIVE_DIRECTORY:
	case LDAP_DIRECTORY_EDIRECTORY:
		directory->cleartext_password = false;
		break;

	default:
		directory->cleartext_password = true;
		break;
	}

finish:
	if (result) ldap_msgfree(result);

	return rcode;
}
