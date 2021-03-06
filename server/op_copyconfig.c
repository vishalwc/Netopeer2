/**
 * @file op_copyconfig.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief NETCONF <copy-config> operation implementation
 *
 * Copyright (c) 2016 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <libyang/libyang.h>
#include <nc_server.h>
#include <sysrepo.h>

#include "common.h"
#include "operations.h"

struct nc_server_reply *
op_copyconfig(struct lyd_node *rpc, struct nc_session *ncs)
{
    struct np2_sessions *sessions;
    sr_datastore_t target = 0, source = 0;
    struct ly_set *nodeset;
    struct lyd_node *config = NULL, *iter, *next;
    struct lyd_node_anydata *any;
    const char *dsname;
    char *str, path[1024];
    sr_val_t value;
    struct nc_server_error *e = NULL;
    int rc = SR_ERR_OK, path_index = 0, missing_keys = 0, lastkey = 0;
    unsigned int i;

    /* get sysrepo connections for this session */
    sessions = (struct np2_sessions *)nc_session_get_data(ncs);

    /* get know which datastore is being affected */
    nodeset = lyd_find_xpath(rpc, "/ietf-netconf:copy-config/target/*");
    dsname = nodeset->set.d[0]->schema->name;
    ly_set_free(nodeset);

    if (!strcmp(dsname, "running")) {
        target = SR_DS_RUNNING;
    } else if (!strcmp(dsname, "startup")) {
        target = SR_DS_STARTUP;
    } else if (!strcmp(dsname, "candidate")) {
        target = SR_DS_CANDIDATE;
    }
    /* TODO URL capability */

    if (sessions->ds != target) {
        /* update sysrepo session */
        sr_session_switch_ds(sessions->srs, target);
        sessions->ds = target;
    }
    if (sessions->ds != SR_DS_CANDIDATE) {
        /* update data from sysrepo */
        if (sr_session_refresh(sessions->srs) != SR_ERR_OK) {
            goto srerror;
        }
    }

    /* get source */
    nodeset = lyd_find_xpath(rpc, "/ietf-netconf:copy-config/source/*");
    dsname = nodeset->set.d[0]->schema->name;

    if (!strcmp(dsname, "running")) {
        source = SR_DS_RUNNING;
    } else if (!strcmp(dsname, "startup")) {
        source = SR_DS_STARTUP;
    } else if (!strcmp(dsname, "candidate")) {
        source = SR_DS_CANDIDATE;
    } else if (!strcmp(dsname, "config")) {
        any = (struct lyd_node_anydata *)nodeset->set.d[0];
        switch (any->value_type) {
        case LYD_ANYDATA_CONSTSTRING:
        case LYD_ANYDATA_STRING:
        case LYD_ANYDATA_SXML:
            config = lyd_parse_mem(np2srv.ly_ctx, any->value.str, LYD_XML, LYD_OPT_CONFIG | LYD_OPT_DESTRUCT);
            break;
        case LYD_ANYDATA_DATATREE:
            config = any->value.tree;
            any->value.tree = NULL; /* "unlink" data tree from anydata to have full control */
            break;
        case LYD_ANYDATA_XML:
            config = lyd_parse_xml(np2srv.ly_ctx, &any->value.xml, LYD_OPT_CONFIG | LYD_OPT_DESTRUCT);
            break;
        case LYD_ANYDATA_JSON:
        case LYD_ANYDATA_JSOND:
        case LYD_ANYDATA_SXMLD:
            EINT;
            ly_set_free(nodeset);
            goto error;
        }
        if (!config) {
            if (ly_errno != LY_SUCCESS) {
                ly_set_free(nodeset);
                goto error;
            } else {
                /* TODO delete-config ??? */
            }
        }
    }
    /* TODO URL capability */
    ly_set_free(nodeset);

    /* perform operation */
    if (config) {
        /* remove all the data from the models mentioned in the <config> ... */
        nodeset = ly_set_new();
        LY_TREE_FOR(config, iter) {
            ly_set_add(nodeset, iter->schema->module, 0);
        }
        for (i = 0; i < nodeset->number; i++) {
            snprintf(path, 1024, "/%s:*", ((struct lys_module *)nodeset->set.g[i])->name);
            sr_delete_item(sessions->srs, path, 0);
        }
        ly_set_free(nodeset);

        /* and copy <config>'s content into sysrepo */
        LY_TREE_DFS_BEGIN(config, next, iter) {
            /* maintain path */
            if (!missing_keys) {
                if (!iter->parent || lyd_node_module(iter) != lyd_node_module(iter->parent)) {
                    /* with prefix */
                    path_index += sprintf(&path[path_index], "/%s:%s", lyd_node_module(iter)->name, iter->schema->name);
                } else {
                    /* without prefix */
                    path_index += sprintf(&path[path_index], "/%s", iter->schema->name);
                }

                /* erase value */
                memset(&value, 0, sizeof value);
            }

            /* specific handling for different types of nodes */
            lastkey = 0;
            switch(iter->schema->nodetype) {
            case LYS_CONTAINER:
                if (!((struct lys_node_container *)iter->schema)->presence) {
                    /* do nothing */
                    goto dfs_continue;
                }
                /* set value for sysrepo */
                op_set_srval(iter, NULL, 0, &value, &str);

                break;
            case LYS_LEAF:
                if (missing_keys) {
                    /* still processing list keys */
                    missing_keys--;
                    /* add key predicate into the list's path */
                    path_index += sprintf(&path[path_index], "[%s=\'%s\']", iter->schema->name,
                                          ((struct lyd_node_leaf_list *)iter)->value_str);
                    if (!missing_keys) {
                        /* the last key, create the list instance */
                        lastkey = 1;
                        break;
                    }
                    goto dfs_continue;
                }
                /* regular leaf */

                /* set value for sysrepo */
                op_set_srval(iter, NULL, 0, &value, &str);

                break;
            case LYS_LEAFLIST:
                /* set value for sysrepo */
                op_set_srval(iter, NULL, 0, &value, &str);

                break;
            case LYS_LIST:
                /* set value for sysrepo, it will be used as soon as all the keys are processed */
                op_set_srval(iter, NULL, 0, &value, &str);

                /* the creation must be finished later when we get know keys */
                missing_keys = ((struct lys_node_list *)iter->schema)->keys_size;
                goto dfs_continue;
            case LYS_ANYXML:
                /* set value for sysrepo */
                op_set_srval(iter, NULL, 0, &value, &str);

                break;
            default:
                ERR("%s: Invalid node to process", __func__);
                goto error;
            }

            /* create the iter in sysrepo */
            rc = sr_set_item(sessions->srs, path, &value, 0);
            if (str) {
                free(str);
                str = NULL;
            }
            switch (rc) {
            case SR_ERR_OK:
                break;
            case SR_ERR_UNAUTHORIZED:
                e = nc_err(NC_ERR_ACCESS_DENIED, NC_ERR_TYPE_PROT);
                nc_err_set_path(e, path);
                goto srerror;
            default:
                /* not covered error */
                goto srerror;
            }

dfs_continue:
            /* modified LY_TREE_DFS_END,
             * select iter for the next run - children first */
            if (iter->schema->nodetype & (LYS_LEAF | LYS_LEAFLIST | LYS_ANYXML)) {
                next = NULL;
            } else {
                next = iter->child;
            }

            if (!next) {
                /* no children, try siblings */
                next = iter->next;

                /* maintain "stack" variables */
                if (!missing_keys && !lastkey) {
                    str = strrchr(path, '/');
                    if (str) {
                        *str = '\0';
                        path_index = str - path;
                    } else {
                        path[0] = '\0';
                        path_index = 0;
                    }
                }
            }

            while (!next) {
                /* parent is already processed, go to its sibling */
                iter = iter->parent;
                if (iter == config->parent) {
                    /* we are done, no next element to process */
                    break;
                }
                next = iter->next;

                /* maintain "stack" variables */
                if (!missing_keys) {
                    str = strrchr(path, '/');
                    if (str) {
                        *str = '\0';
                        path_index = str - path;
                    } else {
                        path[0] = '\0';
                        path_index = 0;
                    }
                }
            }
        }

        /* commit the result */
        if (sessions->ds != SR_DS_CANDIDATE) {
            /* commit in candidate causes copy to running,
             * so do it here only on non-candidate datastores */
            rc = sr_commit(sessions->srs);
        }
    } else {
        rc = sr_copy_config(sessions->srs, NULL, source, target);
        /* commit is done implicitely by sr_copy_config() */
    }

    if (rc != SR_ERR_OK) {
srerror:
        /* cleanup */
        lyd_free_withsiblings(config);

        /* handle error */
        if (!e) {
            return op_build_err_sr(NULL, sessions->srs);
        } else {
            return nc_server_reply_err(e);
        }
    }

    if (sessions->ds == SR_DS_CANDIDATE) {
        if (sr_validate(sessions->srs) != SR_ERR_OK) {
            /* content is not valid, rollback */
            sr_discard_changes(sessions->srs);
            goto srerror;
        }
        /* mark candidate as modified */
        sessions->flags |= NP2S_CAND_CHANGED;
    }

    /* cleanup */
    lyd_free_withsiblings(config);

    return nc_server_reply_ok();

error:
    lyd_free_withsiblings(config);
    e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
    nc_err_set_msg(e, np2log_lasterr(), "en");
    return nc_server_reply_err(e);
}
