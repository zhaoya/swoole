/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include "php_swoole.h"
#include "php_streams.h"
#include "php_network.h"

#include "async.h"

#define PHP_SWOOLE_AIO_MAXEVENTS       128
#define PHP_SWOOLE_AIO_MAX_FILESIZE    4194304

typedef struct {
	zval *callback;
	zval *filename;
	int fd;
	off_t offset;
	uint16_t type;
	uint8_t once;
	char *file_content;
	uint32_t content_length;
} swoole_async_request;

static void php_swoole_check_aio();
static void php_swoole_aio_onComplete(swAio_event *event);
static char php_swoole_aio_init = 0;

static void php_swoole_check_aio()
{
	if (php_swoole_aio_init == 0)
	{
		php_swoole_check_reactor();
		swoole_aio_init(SwooleG.main_reactor, PHP_SWOOLE_AIO_MAXEVENTS);
		swoole_aio_set_callback(php_swoole_aio_onComplete);
		php_swoole_try_run_reactor();
		php_swoole_aio_init = 1;
	}
}

static void php_swoole_aio_onComplete(swAio_event *event)
{
	int argc;
	int64_t ret;

	zval *retval;
	zval *zcontent;
	zval **args[2];
	swoole_async_request *req;
	MAKE_STD_ZVAL(zcontent);

	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);

	if(zend_hash_find(&php_sw_aio_callback, (char *)&(event->fd), sizeof(event->fd), (void**)&req) != SUCCESS)
	{
		zend_error(E_WARNING, "swoole_async: onAsyncComplete callback not found[1]");
		return;
	}

	if (req->callback == NULL && req->type == SW_AIO_READ)
	{
		zend_error(E_WARNING, "swoole_async: onAsyncComplete callback not found[2]");
		return;
	}

	ret = event->ret;
	if (ret < 0)
	{
		zend_error(E_WARNING, "swoole_async: Aio Error: %s[%d]", strerror((-ret)), (int) ret);
		return;
	}

	if (ret < req->content_length)
	{
		zend_error(E_WARNING, "swoole_async: return length < req->length.");
	}

	args[0] = &req->filename;
	if (req->type == SW_AIO_READ)
	{
		ZVAL_STRINGL(zcontent, req->file_content, ret, 0);
		args[1] = &zcontent;
		argc = 2;
	}
	else
	{
		argc = 1;
	}

	if (call_user_function_ex(EG(function_table), NULL, req->callback, &retval, argc, args, 0, NULL TSRMLS_CC) == FAILURE)
	{
		zend_error(E_WARNING, "swoole_async: onAsyncComplete handler error");
		return;
	}

	//readfile/writefile 只操作一次,完成后释放缓存区并关闭文件
	if (req->once == 1)
	{
		free(req->file_content);
		close(event->fd);
	}
	zval_ptr_dtor(&zcontent);
}

PHP_FUNCTION(swoole_async_read)
{
	RETURN_TRUE;
}

PHP_FUNCTION(swoole_async_write)
{
	RETURN_TRUE;
}

PHP_FUNCTION(swoole_async_readfile)
{
	zval *cb;
	zval *filename;

#ifdef HAVE_LINUX_NATIVE_AIO
	int open_flag =  O_RDONLY | O_DIRECT;
#else
	int open_flag = O_RDONLY;
#endif

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zz", &filename, &cb) == FAILURE)
	{
		return;
	}
	convert_to_string(filename);

	int fd = open(Z_STRVAL_P(filename), open_flag, 0644);
	if (fd < 0)
	{
		zend_error(E_WARNING, "swoole_async_readfile: open file failed. Error: %s[%d]", strerror(errno), errno);
		RETURN_FALSE;
	}
	struct stat file_stat;
	if (fstat(fd, &file_stat) < 0)
	{
		zend_error(E_WARNING, "swoole_async_readfile: fstat failed. Error: %s[%d]", strerror(errno), errno);
		RETURN_FALSE;
	}
	if (file_stat.st_size <= 0)
	{
		zend_error(E_WARNING, "swoole_async_readfile: file is empty.");
		RETURN_FALSE;
	}
	if (file_stat.st_size > PHP_SWOOLE_AIO_MAX_FILESIZE)
	{
		zend_error(E_WARNING,
				"swoole_async_readfile: file_size[size=%ld|max_size=%d] is too big. Please use swoole_async_read.",
				(long int) file_stat.st_size, PHP_SWOOLE_AIO_MAX_FILESIZE);
		RETURN_FALSE;
	}

	void *fcnt;
#ifdef HAVE_LINUX_NATIVE_AIO
	int buf_len = file_stat.st_size + (sysconf(_SC_PAGESIZE) - (file_stat.st_size % sysconf(_SC_PAGESIZE)));
	if (posix_memalign((void **)&fcnt, sysconf(_SC_PAGESIZE), buf_len))
	{
		zend_error(E_WARNING, "posix_memalign failed. Error: %s[%d]", strerror(errno), errno);
		RETURN_FALSE;
	}
#else
	int buf_len = file_stat.st_size;
	fcnt = sw_malloc(buf_len);
	if (fcnt == NULL)
	{
		zend_error(E_WARNING, "malloc failed. Error: %s[%d]", strerror(errno), errno);
		RETURN_FALSE;
	}
#endif

	//printf("buf_len=%d|addr=%p\n", buf_len, fcnt);
	//printf("pagesize=%d|st_size=%d\n", sysconf(_SC_PAGESIZE), buf_len);

	swoole_async_request req;
	req.fd = fd;
	req.filename = filename;
	req.callback = cb;
	req.file_content = fcnt;
	req.once = 1;
	req.type = SW_AIO_READ;
	req.content_length = file_stat.st_size;
	req.offset = 0;

	zval_add_ref(&cb);
	zval_add_ref(&filename);

	if(zend_hash_update(&php_sw_aio_callback, (char *)&fd, sizeof(fd), &req, sizeof(swoole_async_request), NULL) == FAILURE)
	{
		zend_error(E_WARNING, "swoole_async_readfile add to hashtable failed");
		RETURN_FALSE;
	}

	php_swoole_check_aio();
	SW_CHECK_RETURN(swoole_aio_read(fd, fcnt, buf_len, 0));
}

PHP_FUNCTION(swoole_async_writefile)
{
	zval *cb = NULL;
	zval *filename;
	char *fcnt;
	int fcnt_len;

#ifdef HAVE_LINUX_NATIVE_AIO
	int open_flag =  O_RDONLY | O_DIRECT;
#else
	int open_flag = O_RDONLY;
#endif

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zs|z", &filename, &fcnt, &fcnt_len, &cb) == FAILURE)
	{
		return;
	}
	if (fcnt_len <= 0)
	{
		zend_error(E_WARNING, "swoole_async_writefile: file is empty.");
		RETURN_FALSE;
	}
	if (fcnt_len > PHP_SWOOLE_AIO_MAX_FILESIZE)
	{
		zend_error(E_WARNING,
				"swoole_async_writefile: file_size[size=%d|max_size=%d] is too big. Please use swoole_async_read.",
				fcnt_len, PHP_SWOOLE_AIO_MAX_FILESIZE);
		RETURN_FALSE;
	}
	int fd = open(Z_STRVAL_P(filename), open_flag, 0644);
	if (fd < 0)
	{
		zend_error(E_WARNING, "swoole_async_writefile: open file failed. Error: %s[%d]", strerror(errno), errno);
		RETURN_FALSE;
	}
	char *wt_cnt;
	if (posix_memalign((void **)&wt_cnt, sysconf(_SC_PAGESIZE), fcnt_len))
	{
		zend_error(E_WARNING, "posix_memalign failed. Error: %s[%d]", strerror(errno), errno);
		RETURN_FALSE;
	}

	swoole_async_request req;
	req.fd = fd;
	req.filename = filename;
	req.callback = cb;
	req.type = SW_AIO_WRITE;
	req.file_content = wt_cnt;
	req.once = 1;
	req.content_length = fcnt_len;
	req.offset = 0;
	zval_add_ref(&filename);

	if(req.callback != NULL)
	{
		zval_add_ref(&req.callback);
	}

	if(zend_hash_update(&php_sw_aio_callback, (char *)&fd, sizeof(fd), &req, sizeof(swoole_async_request), NULL) == FAILURE)
	{
		zend_error(E_WARNING, "swoole_async_writefile add to hashtable failed");
		RETURN_FALSE;
	}

	memcpy(wt_cnt, fcnt, fcnt_len);
	php_swoole_check_aio();
	SW_CHECK_RETURN(swoole_aio_write(fd, wt_cnt, fcnt_len, 0));
}
