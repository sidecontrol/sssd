/*
    SSSD

    LDAP Backend Module

    Authors:
        Sumit Bose <sbose@redhat.com>

    Copyright (C) 2008 Red Hat

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

#ifdef WITH_MOZLDAP
#define LDAP_OPT_SUCCESS LDAP_SUCCESS
#define LDAP_TAG_EXOP_MODIFY_PASSWD_ID  ((ber_tag_t) 0x80U)
#define LDAP_TAG_EXOP_MODIFY_PASSWD_OLD ((ber_tag_t) 0x81U)
#define LDAP_TAG_EXOP_MODIFY_PASSWD_NEW ((ber_tag_t) 0x82U)
#endif

#include <errno.h>
#include <ldap.h>
#include <sys/time.h>

#include <security/pam_modules.h>

#include "util/util.h"
#include "providers/dp_backend.h"
#include "db/sysdb.h"
#include "../sss_client/sss_cli.h"

struct sdap_ctx {
    char *ldap_uri;
    char *default_bind_dn;
    char *user_search_base;
    char *user_name_attribute;
    char *user_object_class;
    char *default_authtok_type;
    uint32_t default_authtok_size;
    char *default_authtok;
    int network_timeout;
    int opt_timeout;
};

struct sdap_req;

enum sdap_auth_steps {
    SDAP_NOOP = 0x0000,
    SDAP_OP_INIT = 0x0001,
    SDAP_CHECK_INIT_RESULT,
    SDAP_CHECK_STD_BIND,
    SDAP_CHECK_SEARCH_DN_RESULT,
    SDAP_CHECK_USER_BIND
};

struct sdap_req {
    struct be_req *req;
    struct pam_data *pd;
    struct sdap_ctx *sdap_ctx;
    LDAP *ldap;
    char *user_dn;
    tevent_fd_handler_t next_task;
    enum sdap_auth_steps next_step;
    int msgid;
};

static int schedule_next_task(struct sdap_req *lr, struct timeval tv,
                              tevent_timer_handler_t task)
{
    int ret;
    struct tevent_timer *te;
    struct timeval timeout;

    ret = gettimeofday(&timeout, NULL);
    if (ret == -1) {
        DEBUG(1, ("gettimeofday failed [%d][%s].\n", errno, strerror(errno)));
        return ret;
    }
    timeout.tv_sec += tv.tv_sec;
    timeout.tv_usec += tv.tv_usec;


    te = tevent_add_timer(lr->req->be_ctx->ev, lr, timeout, task, lr);
    if (te == NULL) {
        return EIO;
    }

    return EOK;
}

static int wait_for_fd(struct sdap_req *lr)
{
    int ret;
    int fd;
    struct tevent_fd *fde;

    ret = ldap_get_option(lr->ldap, LDAP_OPT_DESC, &fd);
    if (ret != LDAP_OPT_SUCCESS) {
        DEBUG(1, ("ldap_get_option failed.\n"));
        return ret;
    }

    fde = tevent_add_fd(lr->req->be_ctx->ev, lr, fd, TEVENT_FD_READ, lr->next_task, lr);
    if (fde == NULL) {
        return EIO;
    }

    return EOK;
}

static int sdap_pam_chauthtok(struct sdap_req *lr)
{
    BerElement *ber=NULL;
    int ret;
    int pam_status=PAM_SUCCESS;
    struct berval *bv;
    int msgid;
    LDAPMessage *result=NULL;
    int ldap_ret;

    ber = ber_alloc_t( LBER_USE_DER );
    if (ber == NULL) {
        DEBUG(1, ("ber_alloc_t failed.\n"));
        return PAM_SYSTEM_ERR;
    }

    ret = ber_printf( ber, "{tststs}", LDAP_TAG_EXOP_MODIFY_PASSWD_ID,
                     lr->user_dn,
                     LDAP_TAG_EXOP_MODIFY_PASSWD_OLD, lr->pd->authtok,
                     LDAP_TAG_EXOP_MODIFY_PASSWD_NEW, lr->pd->newauthtok);
    if (ret == -1) {
        DEBUG(1, ("ber_printf failed.\n"));
        pam_status = PAM_SYSTEM_ERR;
        goto cleanup;
    }

    ret = ber_flatten(ber, &bv);
    if (ret == -1) {
        DEBUG(1, ("ber_flatten failed.\n"));
        pam_status = PAM_SYSTEM_ERR;
        goto cleanup;
    }

    ret = ldap_extended_operation(lr->ldap, LDAP_EXOP_MODIFY_PASSWD, bv,
                                  NULL, NULL, &msgid);
    if (ret != LDAP_SUCCESS) {
        DEBUG(1, ("ldap_extended_operation failed.\n"));
        pam_status = PAM_SYSTEM_ERR;
        goto cleanup;
    }

    ret = ldap_result(lr->ldap, msgid, FALSE, NULL, &result);
    if (ret == -1) {
        DEBUG(1, ("ldap_result failed.\n"));
        pam_status = PAM_SYSTEM_ERR;
        goto cleanup;
    }
    ret = ldap_parse_result(lr->ldap, result, &ldap_ret, NULL, NULL, NULL,
                            NULL, 0);
    if (ret != LDAP_SUCCESS) {
        DEBUG(1, ("ldap_parse_result failed.\n"));
        pam_status = PAM_SYSTEM_ERR;
        goto cleanup;
    }
    DEBUG(3, ("LDAP_EXOP_MODIFY_PASSWD result: [%d][%s]\n", ldap_ret,
              ldap_err2string(ldap_ret)));

    ldap_msgfree(result);

    if (ldap_ret != LDAP_SUCCESS) pam_status = PAM_SYSTEM_ERR;

cleanup:
    ber_bvfree(bv);
    ber_free(ber, 1);
    return pam_status;
}

static int sdap_init(struct sdap_req *lr)
{
    int ret;
    int status=EOK;
    int ldap_vers = LDAP_VERSION3;
    int msgid;
    struct timeval network_timeout;
    struct timeval opt_timeout;

    ret = ldap_initialize(&(lr->ldap), lr->sdap_ctx->ldap_uri);
    if (ret != LDAP_SUCCESS) {
        DEBUG(1, ("ldap_initialize failed: %s\n", strerror(errno)));
        return EIO;
    }

    /* LDAPv3 is needed for TLS */
    ret = ldap_set_option(lr->ldap, LDAP_OPT_PROTOCOL_VERSION, &ldap_vers);
    if (ret != LDAP_OPT_SUCCESS) {
        DEBUG(1, ("ldap_set_option failed: %s\n", ldap_err2string(ret)));
        status = EIO;
        goto cleanup;
    }

    network_timeout.tv_sec = lr->sdap_ctx->network_timeout;
    network_timeout.tv_usec = 0;
    opt_timeout.tv_sec = lr->sdap_ctx->opt_timeout;
    opt_timeout.tv_usec = 0;
    ret = ldap_set_option(lr->ldap, LDAP_OPT_NETWORK_TIMEOUT, &network_timeout);
    if (ret != LDAP_OPT_SUCCESS) {
        DEBUG(1, ("ldap_set_option failed: %s\n", ldap_err2string(ret)));
        status = EIO;
        goto cleanup;
    }
    ret = ldap_set_option(lr->ldap, LDAP_OPT_TIMEOUT, &opt_timeout);
    if (ret != LDAP_OPT_SUCCESS) {
        DEBUG(1, ("ldap_set_option failed: %s\n", ldap_err2string(ret)));
        status = EIO;
        goto cleanup;
    }

    /* For now TLS is forced. Maybe it would be necessary to make this
     * configurable to allow people to expose their passwords over the
     * network. */
    ret = ldap_start_tls(lr->ldap, NULL, NULL, &msgid);
    if (ret != LDAP_SUCCESS) {
        DEBUG(1, ("ldap_start_tls failed: [%d][%s]\n", ret,
                  ldap_err2string(ret)));
        if (ret == LDAP_SERVER_DOWN) {
            status = EAGAIN;
        } else {
            status = EIO;
        }
        goto cleanup;
    }

    lr->msgid = msgid;

    return EOK;

cleanup:
    ldap_unbind_ext(lr->ldap, NULL, NULL);
    lr->ldap = NULL;
    return status;
}

static int sdap_bind(struct sdap_req *lr)
{
    int ret;
    int msgid;
    char *dn=NULL;
    struct berval pw;

    pw.bv_len = 0;
    pw.bv_val = NULL;

    if (lr->user_dn != NULL) {
        dn = lr->user_dn;
        pw.bv_len = lr->pd->authtok_size;
        pw.bv_val = (char *) lr->pd->authtok;
    }
    if (lr->user_dn == NULL && lr->sdap_ctx->default_bind_dn != NULL) {
        dn = lr->sdap_ctx->default_bind_dn;
        pw.bv_len = lr->sdap_ctx->default_authtok_size;
        pw.bv_val = lr->sdap_ctx->default_authtok;
    }

    DEBUG(3, ("Trying to bind as [%s][%*s]\n", dn, pw.bv_len, pw.bv_val));
    ret = ldap_sasl_bind(lr->ldap, dn, LDAP_SASL_SIMPLE, &pw, NULL, NULL,
                         &msgid);
    if (ret == -1 || msgid == -1) {
        DEBUG(1, ("ldap_bind failed\n"));
        return LDAP_OTHER;
    }
    lr->msgid = msgid;
    return LDAP_SUCCESS;
}

static void sdap_cache_password(struct sdap_req *lr);

static void sdap_pam_loop(struct tevent_context *ev, struct tevent_fd *te,
                         uint16_t fd, void *pvt)
{
    int ret;
    int pam_status=PAM_SUCCESS;
    int ldap_ret;
    struct sdap_req *lr;
    struct be_req *req;
    LDAPMessage *result=NULL;
    LDAPMessage *msg=NULL;
    struct timeval no_timeout={0, 0};
    char *errmsgp = NULL;
/* FIXME: user timeout form config */
    char *filter=NULL;
    char *attrs[2] = { NULL, NULL };

    lr = talloc_get_type(pvt, struct sdap_req);

    switch (lr->next_step) {
        case SDAP_OP_INIT:
            ret = sdap_init(lr);
            if (ret != EOK) {
                DEBUG(1, ("sdap_init failed.\n"));
                lr->ldap = NULL;
                if (ret == EAGAIN) {
                    pam_status = PAM_AUTHINFO_UNAVAIL;
                } else {
                    pam_status = PAM_SYSTEM_ERR;
                }
                goto done;
            }
        case SDAP_CHECK_INIT_RESULT:
            ret = ldap_result(lr->ldap, lr->msgid, FALSE, &no_timeout, &result);
            if (ret == -1) {
                DEBUG(1, ("ldap_result failed.\n"));
                pam_status = PAM_SYSTEM_ERR;
                goto done;
            }
            if (ret == 0) {
                DEBUG(1, ("ldap_result not ready yet, waiting.\n"));
                lr->next_task = sdap_pam_loop;
                lr->next_step = SDAP_CHECK_INIT_RESULT;
                return;
            }
            lr->next_step = SDAP_NOOP;

            ret = ldap_parse_result(lr->ldap, result, &ldap_ret, NULL, NULL, NULL, NULL, 0);
            if (ret != LDAP_SUCCESS) {
                DEBUG(1, ("ldap_parse_result failed.\n"));
                pam_status = PAM_SYSTEM_ERR;
                goto done;
            }
            DEBUG(3, ("ldap_start_tls result: [%d][%s]\n", ldap_ret, ldap_err2string(ldap_ret)));

            if (ldap_ret != LDAP_SUCCESS) {
                DEBUG(1, ("setting up TLS failed.\n"));
                pam_status = PAM_SYSTEM_ERR;
                goto done;
            }

/* FIXME: take care that ldap_install_tls might block */
            ret = ldap_install_tls(lr->ldap);
            if (ret != LDAP_SUCCESS) {
                DEBUG(1, ("ldap_install_tls failed.\n"));
                pam_status = PAM_SYSTEM_ERR;
                goto done;
            }

            ret = sdap_bind(lr);
            if (ret != LDAP_SUCCESS) {
                DEBUG(1, ("sdap_bind failed.\n"));
                pam_status = PAM_SYSTEM_ERR;
                goto done;
            }
        case SDAP_CHECK_STD_BIND:
            ret = ldap_result(lr->ldap, lr->msgid, FALSE, &no_timeout, &result);
            if (ret == -1) {
                DEBUG(1, ("ldap_result failed.\n"));
                pam_status = PAM_SYSTEM_ERR;
                goto done;
            }
            if (ret == 0) {
                DEBUG(1, ("ldap_result not ready yet, waiting.\n"));
                lr->next_task = sdap_pam_loop;
                lr->next_step = SDAP_CHECK_STD_BIND;
                return;
            }
            lr->next_step = SDAP_NOOP;

            ret = ldap_parse_result(lr->ldap, result, &ldap_ret, NULL, &errmsgp,
                                    NULL, NULL, 0);
            if (ret != LDAP_SUCCESS) {
                DEBUG(1, ("ldap_parse_result failed.\n"));
                pam_status = PAM_SYSTEM_ERR;
                goto done;
            }
            DEBUG(3, ("Bind result: [%d][%s][%s]\n", ldap_ret,
                      ldap_err2string(ldap_ret), errmsgp));
            if (ldap_ret != LDAP_SUCCESS) {
                DEBUG(1, ("bind failed.\n"));
                pam_status = PAM_SYSTEM_ERR;
                goto done;
            }

            filter = talloc_asprintf(lr->sdap_ctx,
                                     "(&(%s=%s)(objectclass=%s))",
                                     lr->sdap_ctx->user_name_attribute,
                                     lr->pd->user,
                                     lr->sdap_ctx->user_object_class);
            attrs[0] = talloc_strdup(lr->sdap_ctx, LDAP_NO_ATTRS);

            DEBUG(4, ("calling ldap_search_ext with [%s].\n", filter));
            ret = ldap_search_ext(lr->ldap,
                                  lr->sdap_ctx->user_search_base,
                                  LDAP_SCOPE_SUBTREE,
                                  filter,
                                  attrs,
                                  TRUE,
                                  NULL,
                                  NULL,
                                  NULL,
                                  0,
                                  &(lr->msgid));
            if (ret != LDAP_SUCCESS) {
                DEBUG(1, ("ldap_search_ext failed [%d][%s].\n", ret, ldap_err2string(ret)));
                pam_status = PAM_SYSTEM_ERR;
                goto done;
            }
        case SDAP_CHECK_SEARCH_DN_RESULT:
            ret = ldap_result(lr->ldap, lr->msgid, TRUE, &no_timeout, &result);
            if (ret == -1) {
                DEBUG(1, ("ldap_result failed.\n"));
                pam_status = PAM_SYSTEM_ERR;
                goto done;
            }
            if (ret == 0) {
                DEBUG(1, ("ldap_result not ready yet, waiting.\n"));
                lr->next_task = sdap_pam_loop;
                lr->next_step = SDAP_CHECK_SEARCH_DN_RESULT;
                return;
            }
            lr->next_step = SDAP_NOOP;

            msg = ldap_first_message(lr->ldap, result);
            if (msg == NULL) {
                DEBUG(1, ("ldap_first_message failed.\n"));
                pam_status = PAM_SYSTEM_ERR;
                goto done;
            }

            do {
                switch ( ldap_msgtype(msg) ) {
                    case LDAP_RES_SEARCH_ENTRY:
                        if (lr->user_dn != NULL) {
                            DEBUG(1, ("Found more than one object with filter [%s].\n",
                                      filter));
                            pam_status = PAM_SYSTEM_ERR;
                            goto done;
                        }
                        lr->user_dn = ldap_get_dn(lr->ldap, msg);
                        if (lr->user_dn == NULL) {
                            DEBUG(1, ("ldap_get_dn failed.\n"));
                            pam_status = PAM_SYSTEM_ERR;
                            goto done;
                        }

                        if ( *(lr->user_dn) == '\0' ) {
                            DEBUG(1, ("No user found.\n"));
                            pam_status = PAM_USER_UNKNOWN;
                            goto done;
                        }
                        DEBUG(3, ("Found dn: %s\n",lr->user_dn));

                        ldap_msgfree(result);
                        result = NULL;
                        break;
                    default:
                        DEBUG(3, ("ignoring message with type %d.\n", ldap_msgtype(msg)));
                }
            } while( (msg=ldap_next_message(lr->ldap, msg)) != NULL );

            switch (lr->pd->cmd) {
                case SSS_PAM_AUTHENTICATE:
                case SSS_PAM_CHAUTHTOK:
                    break;
                case SSS_PAM_ACCT_MGMT:
                case SSS_PAM_SETCRED:
                case SSS_PAM_OPEN_SESSION:
                case SSS_PAM_CLOSE_SESSION:
                    pam_status = PAM_SUCCESS;
                    goto done;
                    break;
                default:
                    DEBUG(1, ("Unknown pam command %d.\n", lr->pd->cmd));
                    pam_status = PAM_SYSTEM_ERR;
                    goto done;
            }

            ret = sdap_bind(lr);
            if (ret != LDAP_SUCCESS) {
                DEBUG(1, ("sdap_bind failed.\n"));
                pam_status = PAM_SYSTEM_ERR;
                goto done;
            }
        case SDAP_CHECK_USER_BIND:
            ret = ldap_result(lr->ldap, lr->msgid, FALSE, &no_timeout, &result);
            if (ret == -1) {
                DEBUG(1, ("ldap_result failed.\n"));
                pam_status = PAM_SYSTEM_ERR;
                goto done;
            }
            if (ret == 0) {
                DEBUG(1, ("ldap_result not ready yet, waiting.\n"));
                lr->next_task = sdap_pam_loop;
                lr->next_step = SDAP_CHECK_USER_BIND;
                return;
            }
            lr->next_step = SDAP_NOOP;

            ret = ldap_parse_result(lr->ldap, result, &ldap_ret, NULL, &errmsgp,
                                    NULL, NULL, 0);
            if (ret != LDAP_SUCCESS) {
                DEBUG(1, ("ldap_parse_result failed.\n"));
                pam_status = PAM_SYSTEM_ERR;
                goto done;
            }
            DEBUG(3, ("Bind result: [%d][%s][%s]\n", ldap_ret,
                      ldap_err2string(ldap_ret), errmsgp));
            switch (ldap_ret) {
                case LDAP_SUCCESS:
                    pam_status = PAM_SUCCESS;
                    break;
                case LDAP_INVALID_CREDENTIALS:
                    pam_status = PAM_CRED_INSUFFICIENT;
                    goto done;
                    break;
                default:
                    pam_status = PAM_SYSTEM_ERR;
                    goto done;
            }

            switch (lr->pd->cmd) {
                case SSS_PAM_AUTHENTICATE:
                    pam_status = PAM_SUCCESS;
                    break;
                case SSS_PAM_CHAUTHTOK:
                    pam_status = sdap_pam_chauthtok(lr);
                    break;
                case SSS_PAM_ACCT_MGMT:
                case SSS_PAM_SETCRED:
                case SSS_PAM_OPEN_SESSION:
                case SSS_PAM_CLOSE_SESSION:
                    pam_status = PAM_SUCCESS;
                    break;
                default:
                    DEBUG(1, ("Unknown pam command %d.\n", lr->pd->cmd));
                    pam_status = PAM_SYSTEM_ERR;
            }
            break;
        case SDAP_NOOP:
            DEBUG(1, ("current task is SDAP_NOOP, please check your workflow.\n"));
            return;
        default:
            DEBUG(1, ("Unknown ldap backend operation %d.\n", lr->next_step));
            pam_status = PAM_SYSTEM_ERR;
    }

done:
    ldap_memfree(errmsgp);
    ldap_msgfree(result);
    talloc_free(filter);
    if (lr->ldap != NULL) ldap_unbind_ext(lr->ldap, NULL, NULL);
    req = lr->req;
    lr->pd->pam_status = pam_status;

    if (((lr->pd->cmd == SSS_PAM_AUTHENTICATE) ||
         (lr->pd->cmd == SSS_PAM_CHAUTHTOK)) &&
        (lr->pd->pam_status == PAM_SUCCESS) &&
        lr->req->be_ctx->domain->cache_credentials) {
        sdap_cache_password(lr);
        return;
    }

    talloc_free(lr);
    req->fn(req, pam_status, NULL);
}

static void sdap_start(struct tevent_context *ev, struct tevent_timer *te,
                       struct timeval tv, void *pvt)
{
    int ret;
    int pam_status;
    struct sdap_req *lr;
    struct be_req *req;

    lr = talloc_get_type(pvt, struct sdap_req);

    ret = sdap_init(lr);
    if (ret != EOK) {
        DEBUG(1, ("sdap_init failed.\n"));
        lr->ldap = NULL;
        if (ret == EAGAIN) {
            pam_status = PAM_AUTHINFO_UNAVAIL;
        } else {
            pam_status = PAM_SYSTEM_ERR;
        }
        goto done;
    }

    lr->next_task = sdap_pam_loop;
    lr->next_step = SDAP_CHECK_INIT_RESULT;
    ret = wait_for_fd(lr);
    if (ret != EOK) {
        DEBUG(1, ("schedule_next_task failed.\n"));
        pam_status = PAM_SYSTEM_ERR;
        goto done;
    }
    return;

done:
    if (lr->ldap != NULL ) ldap_unbind_ext(lr->ldap, NULL, NULL);
    req = lr->req;
    lr->pd->pam_status = pam_status;

    talloc_free(lr);

    req->fn(req, pam_status, NULL);
}

static void sdap_pam_handler(struct be_req *req)
{
    int ret;
    int pam_status=PAM_SUCCESS;
    struct sdap_req *lr;
    struct sdap_ctx *sdap_ctx;
    struct pam_data *pd;
    struct timeval timeout;

    pd = talloc_get_type(req->req_data, struct pam_data);

    sdap_ctx = talloc_get_type(req->be_ctx->pvt_auth_data, struct sdap_ctx);

    lr = talloc(req, struct sdap_req);

    lr->ldap = NULL;
    lr->req = req;
    lr->pd = pd;
    lr->sdap_ctx = sdap_ctx;
    lr->user_dn = NULL;
    lr->next_task = NULL;
    lr->next_step = SDAP_NOOP;

    timeout.tv_sec=0;
    timeout.tv_usec=0;
    ret = schedule_next_task(lr, timeout, sdap_start);
    if (ret != EOK) {
        DEBUG(1, ("schedule_next_task failed.\n"));
        pam_status = PAM_SYSTEM_ERR;
        goto done;
    }

    return;

done:
    talloc_free(lr);

    pd->pam_status = pam_status;
    req->fn(req, pam_status, NULL);
}

struct sdap_pw_cache {
    struct sysdb_req *sysreq;
    struct sdap_req *lr;
};

static int password_destructor(void *memctx)
{
    char *password = (char *)memctx;
    int i;

    /* zero out password */
    for (i = 0; password[i]; i++) password[i] = '\0';

    return 0;
}

static void sdap_reply(struct be_req *req, int ret, char *errstr)
{
    req->fn(req, ret, errstr);
}

static void sdap_cache_pw_callback(void *pvt, int error,
                                   struct ldb_result *ignore)
{
    struct sdap_pw_cache *data = talloc_get_type(pvt, struct sdap_pw_cache);
    if (error != EOK) {
        DEBUG(2, ("Failed to cache password (%d)[%s]!?\n",
                  error, strerror(error)));
    }

    sysdb_transaction_done(data->sysreq, error);

    /* password caching failures are not fatal errors */
    sdap_reply(data->lr->req, data->lr->pd->pam_status, NULL);
}

static void sdap_cache_pw_op(struct sysdb_req *req, void *pvt)
{
    struct sdap_pw_cache *data = talloc_get_type(pvt, struct sdap_pw_cache);
    struct pam_data *pd;
    const char *username;
    char *password;
    int ret;

    data->sysreq = req;

    pd = data->lr->pd;
    username = pd->user;

    if (pd->cmd == SSS_PAM_AUTHENTICATE) {
        password = talloc_strndup(data, (char *) pd->authtok, pd->authtok_size);
    }
    else if (pd->cmd == SSS_PAM_CHAUTHTOK) {
        password = talloc_strndup(data, (char *) pd->newauthtok, pd->newauthtok_size);
    }
    else {
        DEBUG(1, ("Attempting password caching on invalid Op!\n"));
        /* password caching failures are not fatal errors */
        sdap_reply(data->lr->req, data->lr->pd->pam_status, NULL);
        return;
    }
    talloc_set_destructor((TALLOC_CTX *) password, password_destructor);


    if (!password) {
        DEBUG(2, ("Out of Memory!\n"));
        /* password caching failures are not fatal errors */
        sdap_reply(data->lr->req, data->lr->pd->pam_status, NULL);
        return;
    }

    ret = sysdb_set_cached_password(req,
                                    data->lr->req->be_ctx->domain,
                                    username,
                                    password,
                                    sdap_cache_pw_callback, data);
    if (ret != EOK) {
        /* password caching failures are not fatal errors */
        sdap_reply(data->lr->req, data->lr->pd->pam_status, NULL);
    }
}

static void sdap_cache_password(struct sdap_req *lr)
{
    struct sdap_pw_cache *data;
    int ret;

    data = talloc_zero(lr, struct sdap_pw_cache);
    if (!data) {
        DEBUG(2, ("Out of Memory!\n"));
        /* password caching failures are not fatal errors */
        sdap_reply(data->lr->req, lr->pd->pam_status, NULL);
        return;
    }
    data->lr = lr;

    ret = sysdb_transaction(data, lr->req->be_ctx->sysdb,
                            sdap_cache_pw_op, data);

    if (ret != EOK) {
        DEBUG(1, ("Failed to start transaction (%d)[%s]!?\n",
                  ret, strerror(ret)));
        /* password caching failures are not fatal errors */
        sdap_reply(data->lr->req, lr->pd->pam_status, NULL);
    }
}

static void sdap_shutdown(struct be_req *req)
{
    /* TODO: Clean up any internal data */
    req->fn(req, EOK, NULL);
}

struct be_auth_ops sdap_auth_ops = {
    .pam_handler = sdap_pam_handler,
    .finalize = sdap_shutdown
};


int sssm_ldap_auth_init(struct be_ctx *bectx,
                        struct be_auth_ops **ops,
                        void **pvt_data)
{
    struct sdap_ctx *ctx;
    char *ldap_uri;
    char *default_bind_dn;
    char *default_authtok_type;
    char *default_authtok;
    char *user_search_base;
    char *user_name_attribute;
    char *user_object_class;
    char *tls_reqcert;
    int ldap_opt_x_tls_require_cert;
    int network_timeout;
    int opt_timeout;
    int ret;

    ctx = talloc(bectx, struct sdap_ctx);
    if (!ctx) {
        return ENOMEM;
    }

/* TODO: add validation checks for ldapUri, user_search_base,
 * user_name_attribute, etc */
    ret = confdb_get_string(bectx->cdb, ctx, bectx->conf_path,
                           "ldapUri", "ldap://localhost", &ldap_uri);
    if (ret != EOK) goto done;
    ctx->ldap_uri = ldap_uri;

    ret = confdb_get_string(bectx->cdb, ctx, bectx->conf_path,
                           "defaultBindDn", NULL, &default_bind_dn);
    if (ret != EOK) goto done;
    ctx->default_bind_dn = default_bind_dn;

    ret = confdb_get_string(bectx->cdb, ctx, bectx->conf_path,
                           "defaultAuthtokType", NULL, &default_authtok_type);
    if (ret != EOK) goto done;
    ctx->default_authtok_type = default_authtok_type;

    ret = confdb_get_string(bectx->cdb, ctx, bectx->conf_path,
                           "userSearchBase", NULL, &user_search_base);
    if (ret != EOK) goto done;
    if (user_search_base == NULL) {
        DEBUG(1, ("missing userSearchBase.\n"));
        ret = EINVAL;
        goto done;
    }
    ctx->user_search_base = user_search_base;

    ret = confdb_get_string(bectx->cdb, ctx, bectx->conf_path,
                           "userNameAttribute", "uid", &user_name_attribute);
    if (ret != EOK) goto done;
    ctx->user_name_attribute = user_name_attribute;

    ret = confdb_get_string(bectx->cdb, ctx, bectx->conf_path,
                           "userObjectClass", "posixAccount",
                           &user_object_class);
    if (ret != EOK) goto done;
    ctx->user_object_class = user_object_class;

/* TODO: better to have a blob object than a string here */
    ret = confdb_get_string(bectx->cdb, ctx, bectx->conf_path,
                           "defaultAuthtok", NULL, &default_authtok);
    if (ret != EOK) goto done;
    ctx->default_authtok = default_authtok;
    ctx->default_authtok_size = (default_authtok==NULL?0:strlen(default_authtok));

    ret = confdb_get_int(bectx->cdb, ctx, bectx->conf_path,
                         "network_timeout", 5, &network_timeout);
    if (ret != EOK) goto done;
    ctx->network_timeout = network_timeout;

    ret = confdb_get_int(bectx->cdb, ctx, bectx->conf_path,
                         "opt_timeout", 5, &opt_timeout);
    if (ret != EOK) goto done;
    ctx->network_timeout = opt_timeout;

    ret = confdb_get_string(bectx->cdb, ctx, bectx->conf_path,
                         "tls_reqcert", NULL, &tls_reqcert);
    if (ret != EOK) goto done;
    if (tls_reqcert != NULL ) {
        if (strcasecmp(tls_reqcert, "never") == 0) {
            ldap_opt_x_tls_require_cert = LDAP_OPT_X_TLS_NEVER;
        } else if (strcasecmp(tls_reqcert, "allow") == 0) {
            ldap_opt_x_tls_require_cert = LDAP_OPT_X_TLS_ALLOW;
        } else if (strcasecmp(tls_reqcert, "try") == 0) {
            ldap_opt_x_tls_require_cert = LDAP_OPT_X_TLS_TRY;
        } else if (strcasecmp(tls_reqcert, "demand") == 0) {
            ldap_opt_x_tls_require_cert = LDAP_OPT_X_TLS_DEMAND;
        } else if (strcasecmp(tls_reqcert, "hard") == 0) {
            ldap_opt_x_tls_require_cert = LDAP_OPT_X_TLS_HARD;
        } else {
            DEBUG(1, ("Unknown value for tls_reqcert.\n"));
            ret = EINVAL;
            goto done;
        }
        /* LDAP_OPT_X_TLS_REQUIRE_CERT has to be set as a global option, because
         * the SSL/TLS context is initialized from this value. */
        ret = ldap_set_option(NULL, LDAP_OPT_X_TLS_REQUIRE_CERT,
                              &ldap_opt_x_tls_require_cert);
        if (ret != LDAP_OPT_SUCCESS) {
            DEBUG(1, ("ldap_set_option failed: %s\n", ldap_err2string(ret)));
            ret = EIO;
            goto done;
        }
    }

    *ops = &sdap_auth_ops;
    *pvt_data = ctx;
    ret = EOK;

done:
    if (ret != EOK) {
        talloc_free(ctx);
    }
    return ret;
}
