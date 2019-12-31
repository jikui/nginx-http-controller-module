
/*
 * Copyright (C) hongzhidao
 */

#include <ngx_http_ctrl.h>


static ngx_int_t ngx_http_ctrl_set_variable(ngx_http_request_t *r,
    u_char *name, uint16_t name_length, u_char *value, uint16_t value_length);
static void ngx_http_ctrl_read_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_ctrl_notify(ngx_http_request_t *r, nxt_str_t *notify);
static void ngx_http_ctrl_conf_release(ngx_slab_pool_t *shpool,
    ngx_http_ctrl_conf_t *conf);
static void ngx_http_ctrl_conf_locked_release(ngx_slab_pool_t *shpool,
    ngx_http_ctrl_conf_t *conf);


ngx_int_t
ngx_http_ctrl_request_init(ngx_http_request_t *r)
{
    u_char                     c;
    uint32_t                   hash;
    ngx_uint_t                 i, j;
    ngx_list_part_t           *part;
    ngx_table_elt_t           *header;
    nxt_http_field_t          *field;
    nxt_http_action_t         *action;
    nxt_http_request_t        *req;
    ngx_http_ctrl_ctx_t       *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_ctrl_module);

    req = nxt_mp_zalloc(ctx->mem_pool, sizeof(nxt_http_request_t));
    if (req == NULL) {
        return NGX_ERROR;
    }

    ctx->req = req;

    req->mem_pool = ctx->mem_pool;

    req->host.length = r->headers_in.server.len;
    req->host.start = r->headers_in.server.data;

    req->path.length = r->uri.len;
    req->path.start = r->uri.data;

    req->method.length = r->method_name.len;
    req->method.start = r->method_name.data;

    req->args.length = r->args.len;
    req->args.start = r->args.data;

    req->fields = nxt_list_create(req->mem_pool, 8, sizeof(nxt_http_field_t));
    if (nxt_slow_path(req->fields == NULL)) {
        return NGX_ERROR;
    }

    part = &r->headers_in.headers.part;
    header = part->elts;

    for (i = 0; /* void */; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            header = part->elts;
            i = 0;
        }

        field = nxt_list_add(req->fields);
        if (nxt_slow_path(field == NULL)) {
            return NGX_ERROR;
        }

        field->skip = 0;
        field->hopbyhop = 0;

        field->name_length = header[i].key.len;
        field->value_length = header[i].value.len;
        field->name = header[i].key.data;
        field->value = header[i].value.data;

        hash = NXT_HTTP_FIELD_HASH_INIT;

        for (j = 0; j < field->name_length; j++) {
            c = field->name[j];
            c = nxt_lowcase(c);
            hash = nxt_http_field_hash_char(hash, c);
        }

        field->hash = nxt_http_field_hash_end(hash) & 0xFFFF;
    }

    req->remote = nxt_mp_alloc(req->mem_pool, sizeof(nxt_sockaddr_t));
    if (req->remote == NULL) {
        return NGX_ERROR;
    }

    nxt_memcpy(&req->remote->u.sockaddr, r->connection->sockaddr,
               sizeof(ngx_sockaddr_t));

    action = nxt_http_conf_action(req, &ctx->http_conf);
    if (action == NXT_HTTP_ACTION_ERROR) {
        return NGX_ERROR;
    }

    ctx->action = action;

    return NGX_OK;
}


ngx_int_t
ngx_http_ctrl_set_variables(ngx_http_request_t *r, nxt_http_action_variables_t *variables)
{
    ngx_int_t                  rc;
    nxt_http_name_value_t     *nv, **variable, **end;

    if (variables == NULL) {
        return NGX_OK;
    }

    variable = &variables->variable[0];
    end = variable + variables->items;

    while (variable < end) {
        nv = *variable;

        rc = ngx_http_ctrl_set_variable(r, nv->name, nv->name_length,
                                        nv->value, nv->value_length);
        if (rc == NGX_ERROR) {
            return rc;
        }

        variable++;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_ctrl_set_variable(ngx_http_request_t *r, u_char *name,
    uint16_t name_length, u_char *value, uint16_t value_length)
{
    ngx_uint_t                  key;
    ngx_http_variable_t        *v;
    ngx_http_variable_value_t  *vv;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_core_module);

    key = ngx_hash_strlow(name, name, name_length);

    v = ngx_hash_find(&cmcf->variables_hash, key, name, name_length);

    if (v == NULL) {
        return NGX_DECLINED;
    }

    if (v->set_handler != NULL) {
        vv = ngx_pcalloc(r->pool, sizeof(ngx_http_variable_value_t));
        if (vv == NULL) {
            return NGX_ERROR;
        }

        vv->valid = 1;
        vv->not_found = 0;
        vv->data = value;
        vv->len = value_length;

        v->set_handler(r, vv, v->data);

        return NGX_OK;
    }

    if (!(v->flags & NGX_HTTP_VAR_INDEXED)) {
        return NGX_DECLINED;
    }

    vv = &r->variables[v->index];

    vv->valid = 1;
    vv->not_found = 0;

    vv->data = ngx_pnalloc(r->pool, value_length);
    if (vv->data == NULL) {
        return NGX_ERROR;
    }

    vv->len = value_length;
    ngx_memcpy(vv->data, value, value_length);

    return NGX_OK;
}


ngx_int_t
ngx_http_ctrl_blacklist(ngx_http_request_t *r, nxt_http_action_addr_t *blacklist)
{
    nxt_http_request_t         *req;
    ngx_http_ctrl_ctx_t        *ctx;
    nxt_http_addr_pattern_t    *p, *header, *end;

    if (blacklist == NULL) {
        return NGX_DECLINED;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_ctrl_module);
    req = ctx->req;

    header = &blacklist->addr_pattern[0];
    end = header + blacklist->items;

    while (header < end) {
        p = header;

        if (nxt_http_addr_pattern_match(p, req->remote)) {
            return NGX_OK;
        }

        header++;
    }

    return NGX_DECLINED;
}


ngx_int_t
ngx_http_ctrl_whitelist(ngx_http_request_t *r, nxt_http_action_addr_t *whitelist)
{
    nxt_http_request_t         *req;
    ngx_http_ctrl_ctx_t        *ctx;
    nxt_http_addr_pattern_t    *p, *header, *end;

    if (whitelist == NULL) {
        return NGX_DECLINED;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_ctrl_module);
    req = ctx->req;

    header = &whitelist->addr_pattern[0];
    end = header + whitelist->items;

    while (header < end) {
        p = header;

        if (nxt_http_addr_pattern_match(p, req->remote)) {
            return NGX_OK;
        }

        header++;
    }

    return NGX_DECLINED;
}


ngx_int_t
ngx_http_ctrl_config_handler(ngx_http_request_t *r)
{
    ngx_int_t                     rc;
    ngx_slab_pool_t              *shpool;
    nxt_http_request_t            req;
    ngx_http_ctrl_ctx_t          *ctx;
    nxt_http_conf_init_t          init;
    ngx_http_ctrl_conf_t         *conf;
    ngx_http_ctrl_shctx_t        *shctx;
    ngx_http_ctrl_main_conf_t    *cmcf;

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_ctrl_module);

    shctx = cmcf->shm_zone->data;
    conf = shctx->conf;
    shpool = (ngx_slab_pool_t *) cmcf->shm_zone->shm.addr;

    ctx = ngx_http_get_module_ctx(r, ngx_http_ctrl_module);
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    switch (r->method) {

    case NGX_HTTP_GET:
        ngx_memzero(&req, sizeof(nxt_http_request_t));
        nxt_memzero(&init, sizeof(nxt_http_conf_init_t));

        req.mem_pool = ctx->mem_pool;
        nxt_str_set(&req.method, "GET");
        req.path.start = r->uri.data;
        req.path.length = r->uri.len;;

        rc = nxt_http_conf_handle(&req, &init);
        if (rc != NXT_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        return ngx_http_ctrl_response(r, init.status, &init.response);

    case NGX_HTTP_PUT:

        ngx_shmtx_lock(&shpool->mutex);

        if (conf->counter > 0) {
            ngx_shmtx_unlock(&shpool->mutex);
            return NGX_HTTP_NOT_ALLOWED;
        }

        ngx_shmtx_unlock(&shpool->mutex);

        rc = ngx_http_read_client_request_body(r, ngx_http_ctrl_read_handler);

        if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
            return rc;
        }

        return NGX_DONE;

    case NGX_HTTP_DELETE:

        ngx_shmtx_lock(&shpool->mutex);

        if (conf->counter > 0) {
            ngx_shmtx_unlock(&shpool->mutex);
            return NGX_HTTP_NOT_ALLOWED;
        }

        ngx_shmtx_unlock(&shpool->mutex);

        ngx_memzero(&req, sizeof(nxt_http_request_t));
        nxt_memzero(&init, sizeof(nxt_http_conf_init_t));

        req.mem_pool = ctx->mem_pool;
        nxt_str_set(&req.method, "DELETE");
        req.path.start = r->uri.data;
        req.path.length = r->uri.len;;

        init.file = &cmcf->file;

        rc = nxt_http_conf_handle(&req, &init);
        if (rc != NXT_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        if (init.status == 200) {
            rc = ngx_http_ctrl_notify(r, &init.json);
            if (rc != NGX_OK) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }
        }

        return ngx_http_ctrl_response(r, init.status, &init.response);

    default:
        return NGX_HTTP_NOT_ALLOWED;
    }
}


static ngx_int_t
ngx_http_ctrl_request_body(ngx_http_request_t *r, ngx_str_t *body)
{
    u_char       *p;
    size_t       len;
    ssize_t      size;
    ngx_buf_t    *b;
    ngx_chain_t  *cl, *bufs;

    if (r->request_body == NULL || r->request_body->bufs == NULL) {
        return NGX_DECLINED;
    }

    len = 0;
    bufs = r->request_body->bufs;

    if (r->request_body->temp_file) {

        for (cl = bufs; cl; cl = cl->next) {
            b = cl->buf;
            len += ngx_buf_size(b);
        }

        body->len = len;

        p = ngx_pnalloc(r->pool, len);
        if (p == NULL) {
            return NGX_ERROR;
        }

        body->data = p;

        for (cl = bufs; cl; cl = cl->next) {
            b = cl->buf;

            if (b->in_file) {
                size = ngx_read_file(b->file, p, b->file_last - b->file_pos,
                                     b->file_pos);

                if (size == NGX_ERROR) {
                    return NGX_ERROR;
                }

                p += size;

            } else {
                p = ngx_cpymem(p, b->pos, b->last - b->pos);
            }
        }

    } else {
        cl = bufs;
        b = cl->buf;

        if (cl->next == NULL) {
            body->len = b->last - b->pos;
            body->data = b->pos;

        } else {

            for ( /* void */ ; cl; cl = cl->next) {
                b = cl->buf;
                len += b->last - b->pos;
            }

            body->len = len;

            p = ngx_pnalloc(r->pool, len);
            if (p == NULL) {
                return NGX_ERROR;
            }

            body->data = p;

            for (cl = bufs; cl; cl = cl->next) {
                b = cl->buf;
                p = ngx_cpymem(p, b->pos, b->last - b->pos);
            }
        }
    }

    return NGX_OK;
}


static void
ngx_http_ctrl_read_handler(ngx_http_request_t *r)
{
    ngx_int_t                     rc;
    ngx_str_t                     body;
    nxt_http_request_t            req;
    ngx_http_ctrl_ctx_t          *ctx;
    nxt_http_conf_init_t          init;
    ngx_http_ctrl_main_conf_t    *cmcf;

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_ctrl_module);

    rc = ngx_http_ctrl_request_body(r, &body);

    if (rc == NGX_DECLINED) {
        ngx_http_finalize_request(r, NGX_HTTP_NO_CONTENT);
        return;
    }

    if (rc == NGX_ERROR) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_ctrl_module);

    ngx_memzero(&req, sizeof(nxt_http_request_t));
    nxt_memzero(&init, sizeof(nxt_http_conf_init_t));

    req.mem_pool = ctx->mem_pool;
    nxt_str_set(&req.method, "PUT");
    req.path.start = r->uri.data;
    req.path.length = r->uri.len;

    req.body.start = body.data;
    req.body.length = body.len;

    init.file = &cmcf->file;

    rc = nxt_http_conf_handle(&req, &init);
    if (rc != NXT_OK) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    if (init.status == 200) {
        rc = ngx_http_ctrl_notify(r, &init.json);
        if (rc != NGX_OK) {
            ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
            return;
        }
    }

    rc = ngx_http_ctrl_response(r, init.status, &init.response);

    ngx_http_finalize_request(r, rc);
}


static ngx_int_t
ngx_http_ctrl_notify(ngx_http_request_t *r, nxt_str_t *notify)
{
    ngx_buf_t                  *b;
    ngx_uint_t                  i;
    ngx_chain_t                *cl;
    nxt_port_msg_t             *msg;
    ngx_slab_pool_t            *shpool;
    ngx_connection_t           *c;
    ngx_http_ctrl_conf_t       *conf;
    ngx_http_ctrl_shctx_t      *shctx;
    ngx_http_ctrl_main_conf_t  *cmcf;

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_ctrl_module);

    shctx = cmcf->shm_zone->data;
    conf = shctx->conf;
    shpool = (ngx_slab_pool_t *) cmcf->shm_zone->shm.addr;

    b = ngx_alloc(sizeof(ngx_buf_t) + sizeof(nxt_port_msg_t),
                  r->connection->log);
    if (b == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(b, sizeof(ngx_buf_t));

    b->start = (u_char *) b + sizeof(ngx_buf_t);
    b->pos = b->start;
    b->last = b->pos + sizeof(nxt_port_msg_t);
    b->end = b->last;

    msg = (nxt_port_msg_t *) b->pos;
    msg->size = b->last - b->pos;
    msg->type = NXT_PORT_MSG_CONF;

    ngx_shmtx_lock(&shpool->mutex);

    if (conf->json.data != NULL) {
        ngx_slab_free_locked(shpool, conf->json.data);
    }

    conf->json.len = notify->length;
    conf->json.data = ngx_slab_alloc_locked(shpool, conf->json.len);

    if (conf->json.data == NULL) {
        ngx_shmtx_unlock(&shpool->mutex);

        return NGX_ERROR;
    }

    ngx_memcpy(conf->json.data, notify->start, notify->length);

    for (i = 0; i < cmcf->nfd; i++) {
        c = cmcf->conn[i];

        if (c == NULL) {
            continue;
        }

        cl = ngx_alloc_chain_link(ngx_cycle->pool);
        if (cl == NULL) {
            continue;
        }

        b->file_pos++;

        cl->buf = b;
        cl->next = NULL;

        c->data = cl;

        if (c->write->ready) {
            conf->counter++;
            ngx_post_event(c->write, &ngx_posted_events);
        }
    }

    ngx_shmtx_unlock(&shpool->mutex);

    return NGX_OK;
}


void
ngx_http_ctrl_notify_write_handler(ngx_event_t *wev)
{
    ssize_t                     n;
    ngx_buf_t                  *b;
    ngx_chain_t                *cl;
    ngx_connection_t           *c;
    ngx_slab_pool_t            *shpool;
    ngx_http_ctrl_conf_t       *conf;
    ngx_http_ctrl_shctx_t      *shctx;
    ngx_http_ctrl_main_conf_t  *cmcf;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "http ctrl notify write handler");

    c = wev->data;

    if (c->data == NULL) {
        return;
    }

    cmcf = ngx_http_cycle_get_module_main_conf(ngx_cycle, ngx_http_ctrl_module);

    shctx = cmcf->shm_zone->data;
    conf = shctx->conf;
    shpool = (ngx_slab_pool_t *) cmcf->shm_zone->shm.addr;

    cl = c->data;
    b = cl->buf;

    n = c->send(c, b->pos, b->last - b->pos);

    switch (n) {

    case NGX_AGAIN:
        (void) ngx_handle_write_event(wev, 0);
        return;

    case NGX_ERROR:
        ngx_log_error(NGX_LOG_CRIT, ngx_cycle->log, ngx_errno,
                      "http ctrl notify send failed");
        ngx_http_ctrl_conf_release(shpool, conf);
        break;

    default:
        ngx_log_debug3(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                       "http ctrl notify send b:%p, n:%z, r:%O",
                       b, n, b->file_pos);

        ngx_free_chain(ngx_cycle->pool, cl);

        if (--b->file_pos == 0) {
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                           "http ctrl notify free buffer b:%p", b);
            ngx_free(b);
        }
    }

    c->data = NULL;
}


void
ngx_http_ctrl_notify_read_handler(ngx_event_t *rev)
{
    ssize_t                     n;
    u_char                     *start, *end;
    nxt_mp_t                   *mp;
    nxt_int_t                  ret;
    ngx_buf_t                  *b;
    ngx_str_t                  *json;
    nxt_port_msg_t             *msg;
    ngx_slab_pool_t            *shpool;
    ngx_connection_t           *c;
    nxt_conf_value_t           *value;
    ngx_http_ctrl_conf_t       *conf;
    ngx_http_ctrl_shctx_t      *shctx;
    ngx_http_ctrl_main_conf_t  *cmcf;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "http ctrl notify read handler");

    c = rev->data;
    cmcf = c->data;
    b = cmcf->buf;

    shctx = cmcf->shm_zone->data;
    conf = shctx->conf;
    shpool = (ngx_slab_pool_t *) cmcf->shm_zone->shm.addr;

    n = c->recv(c, b->start, b->end - b->start);

    if (n <= 0) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "ctrl notify recv error");
        goto fail;
    }

    if ((size_t) n < sizeof(nxt_port_msg_t)) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "ctrl notify message format error");
        goto fail;
    }

    msg = (nxt_port_msg_t *) b->start;

    if ((size_t) n != msg->size) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "ctrl notify message size error");
        goto fail;
    }

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        goto fail;
    }

    ngx_shmtx_lock(&shpool->mutex);

    json = &conf->json;

    start = nxt_mp_nget(mp, json->len);
    if (nxt_slow_path(start == NULL)) {
        ngx_shmtx_unlock(&shpool->mutex);
        nxt_mp_destroy(mp);
        goto fail;
    }

    end = nxt_cpymem(start, json->data, json->len);

    ngx_http_ctrl_conf_locked_release(shpool, conf);

    ngx_shmtx_unlock(&shpool->mutex);

    value = nxt_conf_json_parse(mp, start, end, NULL);

    if (nxt_slow_path(value == NULL)) {
        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                      "router conf parse failed.");
        nxt_mp_destroy(mp);
        return;
    }

    ret = nxt_http_conf_apply(mp, value);
    if (nxt_slow_path(ret != NXT_OK)) {
        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                      "router conf apply failed.");
        nxt_mp_destroy(mp);
        return;
    }

    (void) ngx_handle_read_event(rev, 0);

    return;

fail:

    ngx_http_ctrl_conf_release(shpool, conf);

    (void) ngx_handle_read_event(rev, 0);
}


static void
ngx_http_ctrl_conf_release(ngx_slab_pool_t *shpool, ngx_http_ctrl_conf_t *conf)
{
    ngx_shmtx_lock(&shpool->mutex);

    ngx_http_ctrl_conf_locked_release(shpool, conf);

    ngx_shmtx_unlock(&shpool->mutex);
}


static void
ngx_http_ctrl_conf_locked_release(ngx_slab_pool_t *shpool,
    ngx_http_ctrl_conf_t *conf)
{
    conf->counter--;

    if (conf->counter == 0) {
        ngx_slab_free_locked(shpool, conf->json.data);
        conf->json.data = NULL;
    }
}
