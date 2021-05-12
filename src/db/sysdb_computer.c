/*
    SSSD

    Authors:
        Samuel Cabrero <scabrero@suse.com>
        David Mulder <dmulder@suse.com>

    Copyright (C) 2019 SUSE LINUX GmbH, Nuernberg, Germany.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <arpa/inet.h>

#include "db/sysdb.h"
#include "db/sysdb_private.h"
#include "db/sysdb_computer.h"

static errno_t
sysdb_search_computer_by_name(TALLOC_CTX *mem_ctx,
                              struct sss_domain_info *domain,
                              const char *name,
                              const char **attrs,
                              size_t *_num_hosts,
                              struct ldb_message ***_hosts)
{
    errno_t ret;
    TALLOC_CTX *tmp_ctx;
    const char *filter;
    struct ldb_message **results;
    size_t num_results;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) {
        return ENOMEM;
    }

    filter = talloc_asprintf(tmp_ctx, SYSDB_COMP_FILTER, name);
    if (!filter) {
        ret = ENOMEM;
        goto done;
    }

    ret = sysdb_search_custom(tmp_ctx, domain, filter,
                              COMPUTERS_SUBDIR, attrs,
                              &num_results, &results);
    if (ret != EOK) {
        if (ret == ENOENT) {
            DEBUG(SSSDBG_TRACE_FUNC, "No such host\n");
            *_hosts = NULL;
            *_num_hosts = 0;
        } else {
            DEBUG(SSSDBG_CRIT_FAILURE,
                  "Error looking up host [%d]: %s\n",
                   ret, strerror(ret));
        }
        goto done;
    }

    *_hosts = talloc_steal(mem_ctx, results);
    *_num_hosts = num_results;
    ret = EOK;

done:
    talloc_free(tmp_ctx);

    return ret;
}

int
sysdb_get_computer(TALLOC_CTX *mem_ctx,
                   struct sss_domain_info *domain,
                   const char *computer_name,
                   const char **attrs,
                   struct ldb_message **_computer)
{
    TALLOC_CTX *tmp_ctx;
    errno_t ret;
    struct ldb_message **hosts;
    size_t num_hosts;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) {
        return ENOMEM;
    }

    ret = sysdb_search_computer_by_name(tmp_ctx, domain,
                                        computer_name, attrs,
                                        &num_hosts, &hosts);
    if (ret != EOK) {
        goto done;
    }

    if (num_hosts != 1) {
        ret = EINVAL;
        DEBUG(SSSDBG_CRIT_FAILURE,
              "Did not find a single host with name %s\n", computer_name);
        goto done;
    }

    *_computer = talloc_steal(mem_ctx, hosts[0]);
    ret = EOK;

done:
    talloc_free(tmp_ctx);

    return ret;
}

int
sysdb_set_computer(TALLOC_CTX *mem_ctx,
                   struct sss_domain_info *domain,
                   const char *computer_name,
                   const char *dn,
                   const char *sid,
                   const char **group_sids,
                   int num_groups,
                   int cache_timeout,
                   time_t now)
{
    TALLOC_CTX *tmp_ctx;
    int ret, sret;
    int i;
    bool in_transaction = false;
    struct ldb_message *msg = NULL;
    struct sysdb_attrs *attrs;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) {
        return ENOMEM;
    }

    attrs = sysdb_new_attrs(tmp_ctx);
    if (!attrs) {
        ret = ENOMEM;
        goto done;
    }

    ret = sysdb_attrs_add_string(attrs, SYSDB_ORIG_DN, dn);
    if (ret) goto done;

    ret = sysdb_attrs_add_string(attrs, SYSDB_SID_STR, sid);
    if (ret) goto done;

    /* sysdb_store_custom(() cannot correctly modify multi-value attributes */
    if (num_groups > 0) {
        if (group_sids[0] != NULL) {
            ret = sysdb_attrs_add_string(attrs, SYSDB_MEMBEROF_SID_STR, group_sids[0]);
            if (ret) goto done;
        }
    }

    ret = sysdb_attrs_add_string(attrs, SYSDB_OBJECTCLASS, SYSDB_COMPUTER_CLASS);
    if (ret) goto done;

    ret = sysdb_attrs_add_string(attrs, SYSDB_NAME, computer_name);
    if (ret) goto done;

    /* creation time */
    ret = sysdb_attrs_add_time_t(attrs, SYSDB_CREATE_TIME, now);
    if (ret) goto done;

    /* Set a cache expire time. There is a periodic task that cleans up
     * expired entries from the cache even when enumeration is disabled */
    ret = sysdb_attrs_add_time_t(attrs, SYSDB_CACHE_EXPIRE,
                                 cache_timeout ? (now + cache_timeout) : 0);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, "Could not set sysdb cache expire [%d]: %s\n",
              ret, strerror(ret));
        goto done;
    }

    ret = sysdb_store_custom(domain, computer_name, COMPUTERS_SUBDIR, attrs);
    if (ret) goto done;

    /* Add remaining computer groups */
    for (i=1; i<num_groups; i++) {
        if (group_sids[i] != NULL) {
            if (!msg) {
                ret = sysdb_transaction_start(domain->sysdb);
                if (ret) {
                    goto done;
                }
                in_transaction = true;
                msg = ldb_msg_new(tmp_ctx);
                if (!msg) {
                    ret = ENOMEM;
                    goto done;
                }
                msg->dn = sysdb_custom_dn(tmp_ctx, domain,
                                          computer_name, COMPUTERS_SUBDIR);
                if (!msg->dn) {
                    DEBUG(SSSDBG_CRIT_FAILURE, "sysdb_custom_dn failed.\n");
                    ret = ENOMEM;
                    goto done;
                }
                ret = ldb_msg_add_empty(msg,
                                        SYSDB_MEMBEROF_SID_STR,
                                        LDB_FLAG_MOD_ADD,
                                        NULL);
                if (ret != LDB_SUCCESS) {
                    ret = sysdb_error_to_errno(ret);
                    goto done;
                }
            }
            ret = ldb_msg_add_string(msg,
                                     SYSDB_MEMBEROF_SID_STR, group_sids[i]);
            if (ret != LDB_SUCCESS) {
                ret = sysdb_error_to_errno(ret);
                goto done;
            }
        }
    }
    if (msg) {
        ret = ldb_modify(domain->sysdb->ldb, msg);
        if (ret != LDB_SUCCESS) {
            DEBUG(SSSDBG_MINOR_FAILURE,
                  "ldb_modify failed: [%s](%d)[%s]\n",
                  ldb_strerror(ret), ret, ldb_errstring(domain->sysdb->ldb));
            ret = sysdb_error_to_errno(ret);
            goto done;
        }
        ret = sysdb_transaction_commit(domain->sysdb);
        if (ret != EOK) {
            DEBUG(SSSDBG_CRIT_FAILURE,
                  "Could not commit transaction: [%s]\n", strerror(ret));
            goto done;
        }
        in_transaction = false;
    }

    /* FIXME As a future improvement we have to extend domain enumeration.
     * When 'enumerate = true' for a domain, sssd starts a periodic task
     * that brings all users and groups to the cache, cleaning up
     * stale objects after each run. If enumeration is disabled, the cleanup
     * task for expired entries is started instead.
     *
     * We have to extend the enumeration task to fetch 'computer'
     * objects as well (see ad_id_enumeration_send, the entry point of the
     * enumeration task for the  id provider).
     */
done:
    if (in_transaction) {
        sret = sysdb_transaction_cancel(domain->sysdb);
        if (sret != EOK) {
            DEBUG(SSSDBG_CRIT_FAILURE, "Could not cancel transaction\n");
        }
    }
    if (ret) {
        DEBUG(SSSDBG_MINOR_FAILURE, "Error: %d (%s)\n", ret, strerror(ret));
    }
    talloc_zfree(tmp_ctx);

    return ret;
}
