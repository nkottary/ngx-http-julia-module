#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <julia.h>


static char *ngx_http_julia(ngx_conf_t *cf, void *post, void *data);

static ngx_conf_post_handler_pt ngx_http_julia_p = ngx_http_julia;

// The config is just some julia code as a string
typedef struct {
    ngx_str_t   code;
} ngx_http_julia_loc_conf_t;

static void *
ngx_http_julia_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_julia_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_julia_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    return conf;
}

static ngx_command_t ngx_http_julia_commands[] = {
    {
         ngx_string("content_by_julia"),
         NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
         ngx_conf_set_str_slot,
         NGX_HTTP_LOC_CONF_OFFSET,
         offsetof(ngx_http_julia_loc_conf_t, code),
         &ngx_http_julia_p
    },
    {
         ngx_string("access_by_julia"),
         NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
         ngx_conf_set_str_slot,
         NGX_HTTP_LOC_CONF_OFFSET,
         offsetof(ngx_http_julia_loc_conf_t, code),
         &ngx_http_julia_p
    },
    ngx_null_command
};


static ngx_http_module_t ngx_http_julia_module_ctx = {
    NULL,                          /* preconfiguration */
    NULL,                          /* postconfiguration */

    NULL,                          /* create main configuration */
    NULL,                          /* init main configuration */

    NULL,                          /* create server configuration */
    NULL,                          /* merge server configuration */

    ngx_http_julia_create_loc_conf, /* create location configuration */
    NULL                           /* merge location configuration */
};

ngx_module_t ngx_http_julia_module = {
    NGX_MODULE_V1,
    &ngx_http_julia_module_ctx,    /* module context */
    ngx_http_julia_commands,       /* module directives */
    NGX_HTTP_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    NULL,                          /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    NULL,                          /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t
ngx_http_julia_handler(ngx_http_request_t *r)
{
    ngx_buf_t   *b;
    ngx_chain_t  out;

    size_t sz;

    // The the julia code from config and convert it to a regular c string
    // to be passed to julia_eval_string
    ngx_http_julia_loc_conf_t *juliacf = ngx_http_get_module_loc_conf(r, ngx_http_julia_module);
    char *strtmp = ngx_pcalloc(r->pool, juliacf->code.len);
    strncpy(strtmp, (char*) juliacf->code.data, juliacf->code.len);
    strtmp[juliacf->code.len] = '\0';

    char *args = ngx_pcalloc(r->pool,r->args.len);
    strncpy(args, (char*) r->args.data,r->args.len);

    char *strout = ngx_pcalloc(r->pool,100);
    /* required: setup the Julia context */
    jl_init();

    /* run Julia commands */
    jl_value_t *ret = jl_eval_string(strtmp);

    if (jl_typeis(ret, jl_string_type)) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Received string type answer");
        sprintf(strout, "%s \n", jl_string_data(ret));
    } else {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Unexpected return type");
        sprintf(strout, "ERROR: unexpected return type\n");
    }

    /* strongly recommended: notify Julia that the
         program is about to terminate. this allows
         Julia time to cleanup pending write requests
         and run all finalizers
    */
    jl_atexit_hook(0);
    sz = strlen(strout);


    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = sz;
    ngx_http_send_header(r);

    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    out.buf = b;
    out.next = NULL;

    b->pos = (u_char*)strout;
    b->last = b->pos + sz;
    b->memory = 1;
    b->last_buf = 1;
    return ngx_http_output_filter(r, &out);
}

static char *
ngx_http_julia(ngx_conf_t *cf, void *post, void *data)
{
    ngx_http_core_loc_conf_t *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_julia_handler;

    ngx_str_t  *name = data;

    if (ngx_strcmp(name->data, "") == 0) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}
