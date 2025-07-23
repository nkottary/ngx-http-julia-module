#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <json-c/json.h>

#include <julia.h>
#include <stdio.h>


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

const char *build_headers_list(ngx_list_part_t *part)
{
    void *v = part->elts;
    json_object *jarray = json_object_new_array();
    for (ngx_uint_t i = 0; ; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            v = part->elts;
            i = 0;
        }

        ngx_table_elt_t elt = ((ngx_table_elt_t *)v)[i];

        json_object *jobj = json_object_new_object();
        json_object *value_string = json_object_new_string((char *)elt.value.data);
        json_object_object_add(jobj, (char *)elt.key.data, value_string);
        json_object_array_add(jarray, jobj);
    }
    const char *ret = strdup(json_object_to_json_string_ext(jarray, JSON_C_TO_STRING_NOSLASHESCAPE));
    json_object_put(jarray);
    return ret;
}

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

    char *strout = ngx_pcalloc(r->pool, 1024);
    /* required: setup the Julia context */
    jl_init();

    // Pass a string as a global variable from c to julia
    jl_sym_t *var = jl_symbol("ngx_req_headers");
    jl_binding_t *bp = jl_get_binding_wr(jl_main_module, var, 1);
    jl_value_t* headers_array_type = jl_apply_array_type((jl_value_t*)jl_uint8_type, 1);

    const char *headers_json = build_headers_list(&r->headers_in.headers.part);
    jl_array_t* headers_array = jl_alloc_array_1d(headers_array_type, strlen(headers_json));
    char *headers_c_array = jl_array_data(headers_array, char);
    strcpy(headers_c_array, headers_json);
    jl_checked_assignment(bp, jl_main_module, var, (jl_value_t *)headers_array);

    /* run Julia commands */
    jl_value_t *ret = jl_eval_string(strtmp);

    jl_value_t *exception = jl_exception_occurred();
    if (exception) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "%s \n", jl_typeof_str(exception));
        jl_value_t *showf = jl_get_function(jl_base_module, "show");
        jl_call1(showf, exception);
        jl_print_backtrace();
    } else if (jl_typeis(ret, jl_string_type)) {
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
