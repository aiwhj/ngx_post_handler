
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

#include "sapi/embed/php_embed.h"
#include "ext/standard/php_standard.h"

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

// 保存 nginx 配置中的脚本
typedef struct {
    ngx_str_t php_script;
} ngx_http_post_handler_loc_conf_t;

// 保存 nginx 中的 请求体 和 output 链表
typedef struct {
    ngx_http_request_t *ngx_http_request;
    ngx_chain_t *first_output_buf;
    ngx_chain_t *last_output_buf;
    size_t buf_size;
} ngx_http_post_handler_php;

/**
 * 拆分 embed 的 php_embed_init 函数，为 MINIT 和 RINIT
 * 
 * */
#define PHP_EMBED_MINIT_BLOCK(x,y) { \
    php_embed_minit(x, y);

#define PHP_EMBED_RINIT_BLOCK(x,y) \
    php_embed_rinit(x, y);

#define PHP_EMBED_TRY_BLOCK() \
  zend_first_try {

#define PHP_EMBED_CATCH_BLOCK() \
  } zend_catch {

#define PHP_EMBED_ENDTRY_BLOCK() \
  } zend_end_try();

#define PHP_EMBED_MEND_BLOCK() \
  php_embed_shutdown(); \
}

const char HARDCODED_INI[] =
	"html_errors=0\n"
	"register_argc_argv=1\n"
	"implicit_flush=1\n"
	"output_buffering=0\n"
	"max_execution_time=0\n"
	"max_input_time=-1\n\0";

static void* ngx_http_post_handler_create_loc_conf(ngx_conf_t* cf);
static char* ngx_http_post_handler_merge_loc_conf(ngx_conf_t* cf, void* parent, void* child);
static ngx_int_t ngx_http_post_handler_script(ngx_http_request_t *r);
static char* ngx_http_post_handler_conf(ngx_conf_t* cf, ngx_command_t* cmd, void* conf);
static ngx_int_t ngx_http_post_handler_init(ngx_conf_t *cf);
static void ngx_php_load_env_var(char *var, unsigned int var_len, char *val, unsigned int val_len, void *arg);
static size_t ngx_http_post_handler_php_ub_write(const char *str, size_t str_length);
static void (*php_php_import_environment_variables)(zval *array_ptr);
void ngx_php_import_environment_variables(zval *array_ptr);

static ngx_command_t ngx_http_post_handler_commands[] = {
    {
        ngx_string("post_handler"),
        NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
        ngx_http_post_handler_conf,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_post_handler_loc_conf_t, php_script),
        NULL
    },
    ngx_null_command
};

static ngx_http_module_t  ngx_http_post_handler_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_post_handler_init,            /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_post_handler_create_loc_conf,       /* create location configuration */
    ngx_http_post_handler_merge_loc_conf         /* merge location configuration */
};

ngx_module_t  ngx_http_post_handler_module = {
    NGX_MODULE_V1,
    &ngx_http_post_handler_module_ctx,           /* module context */
    ngx_http_post_handler_commands,              /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

static void* ngx_http_post_handler_create_loc_conf(ngx_conf_t* cf) {
    ngx_http_post_handler_loc_conf_t* conf;
 
    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_post_handler_loc_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }
    conf->php_script.len = 0;
    conf->php_script.data = NULL;
 
    return conf;
}

static char* ngx_http_post_handler_merge_loc_conf(ngx_conf_t* cf, void* parent, void* child) {
    ngx_http_post_handler_loc_conf_t* prev = parent;
    ngx_http_post_handler_loc_conf_t* conf = child;
    ngx_conf_merge_str_value(conf->php_script, prev->php_script, "exit(0);");
    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_post_handler_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PRECONTENT_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_post_handler_script;

    return NGX_OK;
}

static char* ngx_http_post_handler_conf(ngx_conf_t* cf, ngx_command_t* cmd, void* conf) {
    ngx_conf_set_str_slot(cf, cmd, conf);
    return NGX_CONF_OK;
}

static size_t ngx_http_post_handler_php_ub_write(const char *str, size_t str_length)
{
    ngx_http_post_handler_php *ctx = (ngx_http_post_handler_php *)SG(server_context);
    ngx_http_request_t *r = ctx->ngx_http_request;

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, r->connection->log, 0,
                       "php_ub_write: %s", str);

    ngx_buf_t* b;
    ngx_chain_t *out;

    out = ngx_pcalloc(r->pool, sizeof(ngx_chain_t));

    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));

    b->pos = (u_char*)str;
    b->last = b->pos + str_length;
    b->memory = 1;
    b->last_buf = 0;

    out->buf = b;
    out->next = NULL;

    if (ctx->first_output_buf == NULL) {
        ctx->first_output_buf = out;
        ctx->last_output_buf = out;
    } else {
        ctx->last_output_buf->next = out;
        ctx->last_output_buf = out;
    }

    ctx->buf_size += str_length;
    return str_length;
}

static void ngx_php_load_env_var(char *var, unsigned int var_len, char *val, unsigned int val_len, void *arg) /* {{{ */
{
	zval *array_ptr = (zval*)arg;
	int filter_arg = (Z_ARR_P(array_ptr) == Z_ARR(PG(http_globals)[TRACK_VARS_ENV]))?PARSE_ENV:PARSE_SERVER;
	size_t new_val_len;

	if (sapi_module.input_filter(filter_arg, var, &val, strlen(val), &new_val_len)) {
		php_register_variable_safe(var, val, new_val_len, array_ptr);
	}
}

EMBED_SAPI_API int php_embed_minit(int argc, char **argv)
{
	zend_llist global_vars;

#ifdef HAVE_SIGNAL_H
#if defined(SIGPIPE) && defined(SIG_IGN)
	signal(SIGPIPE, SIG_IGN);
#endif
#endif

#ifdef ZTS
  tsrm_startup(1, 1, 0, NULL);
  (void)ts_resource(0);
  ZEND_TSRMLS_CACHE_UPDATE();
#endif

  sapi_startup(&php_embed_module);

#ifdef PHP_WIN32
  _fmode = _O_BINARY;			/*sets default for file streams to binary */
  setmode(_fileno(stdin), O_BINARY);		/* make the stdio mode be binary */
  setmode(_fileno(stdout), O_BINARY);		/* make the stdio mode be binary */
  setmode(_fileno(stderr), O_BINARY);		/* make the stdio mode be binary */
#endif

  php_embed_module.ini_entries = malloc(sizeof(HARDCODED_INI));
  memcpy(php_embed_module.ini_entries, HARDCODED_INI, sizeof(HARDCODED_INI));

  if (argv) {
	php_embed_module.executable_location = argv[0];
  }

  if (php_embed_module.startup(&php_embed_module)==FAILURE) {
	  return FAILURE;
  }

  zend_llist_init(&global_vars, sizeof(char *), NULL, 0);

  /* Set some Embedded PHP defaults */
  SG(options) |= SAPI_OPTION_NO_CHDIR;
  SG(request_info).argc=argc;
  SG(request_info).argv=argv;

  return SUCCESS;
}

EMBED_SAPI_API int php_embed_rinit(int argc, char **argv)
{
  if (php_request_startup()==FAILURE) {
	  php_module_shutdown();
	  return FAILURE;
  }

  SG(headers_sent) = 1;
  SG(request_info).no_headers = 1;
  php_register_variable("PHP_SELF", "-", NULL);

  return SUCCESS;
}

// 注册 header 到 $_SERVER
void ngx_php_import_environment_variables(zval *array_ptr) /* {{{ */
{
    ngx_http_post_handler_php *ctx = NULL;
    ctx = (ngx_http_post_handler_php *)SG(server_context);
    ngx_http_request_t *r = ctx->ngx_http_request;

	if (Z_TYPE(PG(http_globals)[TRACK_VARS_ENV]) == IS_ARRAY &&
		Z_ARR_P(array_ptr) != Z_ARR(PG(http_globals)[TRACK_VARS_ENV]) &&
		zend_hash_num_elements(Z_ARRVAL(PG(http_globals)[TRACK_VARS_ENV])) > 0
	) {
		zval_dtor(array_ptr);
		ZVAL_DUP(array_ptr, &PG(http_globals)[TRACK_VARS_ENV]);
		return;
	} else if (Z_TYPE(PG(http_globals)[TRACK_VARS_SERVER]) == IS_ARRAY &&
		Z_ARR_P(array_ptr) != Z_ARR(PG(http_globals)[TRACK_VARS_SERVER]) &&
		zend_hash_num_elements(Z_ARRVAL(PG(http_globals)[TRACK_VARS_SERVER])) > 0
	) {
		zval_dtor(array_ptr);
		ZVAL_DUP(array_ptr, &PG(http_globals)[TRACK_VARS_SERVER]);
		return;
	}

	/* call php's original import as a catch-all */
	php_php_import_environment_variables(array_ptr);

    // 设置 header
    ngx_list_part_t              *part;
    ngx_table_elt_t              *h;
    ngx_uint_t                   i, n;
    u_char                       *header_name;
    u_char                       *header_name_last;
    u_char                       ch;
    ngx_uint_t                   header_name_len;

    part = &r->headers_in.headers.part;
    h = part->elts;

    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            h = part->elts;
            i = 0;
        }

        // 加上 HTTP_ 前缀并且转化为 大写
        header_name_len = sizeof("HTTP_") - 1 + h[i].key.len;
        header_name = ngx_palloc(r->pool, header_name_len + 1);

        header_name_last = ngx_cpymem(header_name, "HTTP_", sizeof("HTTP_") - 1);
        for (n = 0; n < h[i].key.len; n++) {
            ch = h[i].key.data[n];

            if (ch >= 'a' && ch <= 'z') {
                ch &= ~0x20;

            } else if (ch == '-') {
                ch = '_';
            }

            *header_name_last++ = ch;
        }
        *header_name_last = '\0';

        ngx_log_debug6(NGX_LOG_DEBUG_EVENT, r->connection->log, 0, "PHP set header: %s, %d, %s, %d, %s, %d", header_name, header_name_len, h[i].key.data, h[i].key.len, h[i].value.data, h[i].value.len);

        ngx_php_load_env_var((char *)header_name, header_name_len, (char *)h[i].value.data, h[i].value.len, array_ptr);
    }
}

static ngx_int_t
ngx_http_post_handler_script(ngx_http_request_t *r)
{
    ngx_http_post_handler_loc_conf_t *hlcf;
    ngx_http_post_handler_php *ctx;
    volatile int exit_status = 0;

    int argc = 0;
    char ** argv = NULL;

    hlcf = ngx_http_get_module_loc_conf(r, ngx_http_post_handler_module);

    if (hlcf->php_script.data == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    //自定义 ub_write hook，捕获 php 的输出，转给 nginx
    php_embed_module.ub_write = ngx_http_post_handler_php_ub_write;

    // override php_import_environment_variables 函数，收集 nginx 的 header 给 $_SERVER
    php_php_import_environment_variables = php_import_environment_variables;
	php_import_environment_variables = ngx_php_import_environment_variables;
    
    // 设置全局变量 SG(server_context)
    ctx = ngx_palloc(r->pool, sizeof(ngx_http_post_handler_php));

    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ctx->ngx_http_request = r;
    ctx->first_output_buf = NULL;
    ctx->last_output_buf = NULL;
    ctx->buf_size = 0;

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, r->connection->log, 0, "PHP_EMBED_MINIT_BLOCK: %s", hlcf->php_script.data);

    PHP_EMBED_MINIT_BLOCK(argc, argv);
    // nginx_php_import_environment_variables 收集 header 在 RINIT 的时候，需要 在其之前 注入 ctx，所以拆分 php_embed_init 为两部分
    SG(server_context) = ctx;
    PHP_EMBED_RINIT_BLOCK(argc, argv);

    PHP_EMBED_TRY_BLOCK()
    
    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, r->connection->log, 0, "PHP_EMBED_RINIT_BLOCK: %s", hlcf->php_script.data);

    if (zend_eval_stringl_ex((char *)hlcf->php_script.data, hlcf->php_script.len, NULL, "ngx_http_post_handler handler", 1) == FAILURE) {
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, r->connection->log, 0, "PHP_EMBED_RINIT_BLOCK: %s", hlcf->php_script.data);

    PHP_EMBED_CATCH_BLOCK();
    PHP_EMBED_ENDTRY_BLOCK();

    exit_status = EG(exit_status);
    
    // override php_import_environment_variables 函数，收集 nginx 的 header 给 $_SERVER
    php_import_environment_variables = php_php_import_environment_variables;

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, r->connection->log, 0, "PHP exit_status: %d", exit_status);
    if (exit_status > 0 && ctx->last_output_buf != NULL) {
        ngx_int_t rc;

        r->headers_out.content_type.len = sizeof("text/plain") - 1;
        r->headers_out.content_type.data = (u_char*)"text/plain";

        r->headers_out.status = NGX_HTTP_OK;
        r->headers_out.content_length_n = ctx->buf_size;

        rc = ngx_http_send_header(r);

        if (rc != NGX_ERROR && rc == NGX_OK && !r->header_only) {
            // output 的最后一个 buf 设置为 last
            ctx->last_output_buf->buf->last_buf = 1;
            ngx_http_output_filter(r, ctx->first_output_buf);
        }
    }

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, r->connection->log, 0, "PHP_EMBED_MEND_BLOCK");

    PHP_EMBED_MEND_BLOCK();

    return NGX_OK;
}
