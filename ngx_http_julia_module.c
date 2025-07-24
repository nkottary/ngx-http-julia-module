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

// Convert nginx headers_in list to a json string that can be
// sent to julia
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

// Setup a julia global Vector{UInt8} of the given size
char *setup_global_var(char var_name[], size_t sz)
{
    jl_sym_t *var = jl_symbol(var_name);
    jl_binding_t *bp = jl_get_binding_wr(jl_main_module, var, 1);
    jl_value_t* headers_array_type = jl_apply_array_type((jl_value_t*)jl_uint8_type, 1);
    jl_array_t* headers_array = jl_alloc_array_1d(headers_array_type, sz);
    char *headers_c_array = jl_array_data(headers_array, char);
    jl_checked_assignment(bp, jl_main_module, var, (jl_value_t *)headers_array);

    return headers_c_array;
}

void setup_global_pointer(char name[], void *ptr)
{
    jl_sym_t *var = jl_symbol(name);
    jl_binding_t *bp = jl_get_binding_wr(jl_main_module, var, 1);
    jl_value_t *jl_ptr = jl_box_int64((unsigned long int)ptr);
    jl_checked_assignment(bp, jl_main_module, var, jl_ptr);
}

// Parses headers sent back by julia as json. Inserts them to the nginx headers_out list
int build_response_headers(const char *raw_headers, ngx_http_request_t *req)
{
    if (raw_headers[0] == '\0') {
        // Nothing to do, skip
        return 1;
    }
    json_object *jarray = json_tokener_parse(raw_headers);
    int arraylen = json_object_array_length(jarray);
    enum json_type type;
    type = json_object_get_type(jarray);
    if (type != json_type_array) {
        ngx_log_error(NGX_LOG_ERR, req->connection->log, 0, "Expected array but got %s", type);
        json_object_put(jarray);
        return 0;
    }
    for (int i = 0; i < arraylen; i++) {
        json_object *jvalue = json_object_array_get_idx(jarray, i);
        type = json_object_get_type(jvalue);
        if (type != json_type_object) {
            ngx_log_error(NGX_LOG_ERR, req->connection->log, 0, "Expected object but got %s", type);
            json_object_put(jarray);
            return 0;
        }
        json_object_object_foreach(jvalue, key, val) {
            type = json_object_get_type(val);
            if (type != json_type_string) {
                ngx_log_error(NGX_LOG_ERR, req->connection->log, 0, "Expected string but got %s", type);
                json_object_put(jarray);
                return 0;
            }
            const char *value = json_object_get_string(val);
            ngx_table_elt_t *new = (ngx_table_elt_t *)ngx_list_push(&req->headers_out.headers);
            char *keycopy = (char *)ngx_pcalloc(req->pool, sizeof(char) * strlen(key));
            strcpy(keycopy, key);
            char *valuecopy = (char *)ngx_pcalloc(req->pool, sizeof(char) * strlen(value));
            strcpy(valuecopy, value);
            new->key.data = (u_char *)keycopy;
            new->key.len = strlen(keycopy);
            new->value.data = (u_char *)valuecopy;
            new->value.len = strlen(valuecopy);
        }
    }
    json_object_put(jarray);
    return 1;
}

u_char *
get_body_data()
{
    ngx_http_request_t *r;
    off_t         len;
    ngx_chain_t  *in;

    jl_value_t *ret = jl_eval_string("req_ptr");
    r = (ngx_http_request_t *)jl_unbox_uint64(ret);

    if (r->request_body == NULL) {
        return NULL;
    }

    // Get total length of content
    len = 0;
    for (in = r->request_body->bufs; in; in = in->next) {
        len += ngx_buf_size(in->buf);
    }
    // Allocate buffer of that length
    u_char *buf = ngx_pcalloc(r->pool, len);
    if (buf == NULL) {
        return NULL;
    }

    u_char *p = buf;
    size_t size = 0, rest = len;
    for (ngx_chain_t *cl = r->request_body->bufs; cl != NULL && rest > 0; cl = cl->next) {
        size = ngx_buf_size(cl->buf);
        if (size > rest) { /* reach limit*/
            size = rest;
        }

        p = ngx_copy(p, cl->buf->pos, size);
        rest -= size;
    }
    buf[len] = '\0';

    return buf;
}

void
ngx_http_julia_read_request_body(ngx_http_request_t *r)
{
    if (r->request_body == NULL) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }
    ngx_http_finalize_request(r, NGX_OK);
}

int test_ccall(int x)
{
    return x + 1;
}

int read_body()
{
    jl_value_t *ret = jl_eval_string("req_ptr");
    ngx_http_request_t *r = (ngx_http_request_t *)jl_unbox_uint64(ret);
    ngx_int_t rc = ngx_http_read_client_request_body(r, ngx_http_julia_read_request_body);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return 0;
    } else {
        return 1;
    }
}

static ngx_int_t
ngx_http_julia_handler(ngx_http_request_t *r)
{
    ngx_buf_t *b;
    ngx_chain_t  out;
    size_t sz;

    // Get julia code from config and convert it to a regular c string
    // to be passed to julia_eval_string
    ngx_http_julia_loc_conf_t *juliacf = ngx_http_get_module_loc_conf(r, ngx_http_julia_module);
    char *strtmp = ngx_pcalloc(r->pool, juliacf->code.len);
    if (strtmp == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    strncpy(strtmp, (char*) juliacf->code.data, juliacf->code.len);
    strtmp[juliacf->code.len] = '\0';

    /*
    char *args = ngx_pcalloc(r->pool,r->args.len);
    if (args == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    */

    char *strout = ngx_pcalloc(r->pool, 1024);
    if (strout == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    /* required: setup the Julia context */
    jl_init();

    // Request args
    char *args = setup_global_var("ngx_req_args", r->args.len);
    strncpy(args, (char*) r->args.data, r->args.len);

    // Request uri
    char *uri = setup_global_var("ngx_req_uri", r->uri.len);
    strncpy(uri, (char*) r->uri.data, r->uri.len);

    // Pass a string as a global variable from c to julia
    const char *req_headers_json = build_headers_list(&r->headers_in.headers.part);
    char *req_headers_c_array = setup_global_var("ngx_req_headers", strlen(req_headers_json));
    strcpy(req_headers_c_array, req_headers_json);

    jl_value_t *ret;
    // Read body
    // Add test_ccall function pointer to global
    setup_global_pointer("test_ccall", &test_ccall);

    // Try storing request as global in julia
    setup_global_pointer("req_ptr", r);

    setup_global_pointer("read_body", &read_body);
    setup_global_pointer("get_body_data", &get_body_data);


    // setup ngx_resp_headers
    char *resp_headers_c_array = setup_global_var("ngx_resp_headers", 1024);

    /* run Julia commands */
    ret = jl_eval_string(strtmp);

    jl_value_t *exception = jl_exception_occurred();
    if (exception) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "%s \n", jl_typeof_str(exception));
        jl_value_t *showf = jl_get_function(jl_base_module, "show");
        jl_call1(showf, exception);
        jl_print_backtrace();
    } else if (jl_typeis(ret, jl_string_type)) {
        strcpy(strout, jl_string_data(ret));
    } else {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Unexpected return type");
        sprintf(strout, "ERROR: unexpected return type\n");
    }

    // Exit julia
    jl_atexit_hook(0);
    sz = strlen(strout);


    if (!build_response_headers(resp_headers_c_array, r)){
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Failed to process headers_out");
    }
    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = sz;
    ngx_http_send_header(r);

    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
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
