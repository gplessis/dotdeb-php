/*
   +----------------------------------------------------------------------+
   | Zend OPcache                                                         |
   +----------------------------------------------------------------------+
   | Copyright (c) 1998-2017 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Andi Gutmans <andi@zend.com>                                |
   |          Zeev Suraski <zeev@zend.com>                                |
   |          Stanislav Malyshev <stas@zend.com>                          |
   |          Dmitry Stogov <dmitry@zend.com>                             |
   +----------------------------------------------------------------------+
*/

#include "main/php.h"
#include "main/php_globals.h"
#include "zend.h"
#include "zend_extensions.h"
#include "zend_compile.h"
#include "ZendAccelerator.h"
#include "zend_persist.h"
#include "zend_shared_alloc.h"
#include "zend_accelerator_module.h"
#include "zend_accelerator_blacklist.h"
#include "zend_list.h"
#include "zend_execute.h"
#include "main/SAPI.h"
#include "main/php_streams.h"
#include "main/php_open_temporary_file.h"
#include "zend_API.h"
#include "zend_ini.h"
#include "zend_virtual_cwd.h"
#include "zend_accelerator_util_funcs.h"
#include "zend_accelerator_hash.h"
#include "ext/pcre/php_pcre.h"
#include "ext/standard/md5.h"

#ifdef HAVE_OPCACHE_FILE_CACHE
# include "zend_file_cache.h"
#endif

#ifndef ZEND_WIN32
#include  <netdb.h>
#endif

#ifdef ZEND_WIN32
typedef int uid_t;
typedef int gid_t;
#include <io.h>
#endif

#ifndef ZEND_WIN32
# include <sys/time.h>
#else
# include <process.h>
#endif

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <fcntl.h>
#include <signal.h>
#include <time.h>

#ifndef ZEND_WIN32
# include <sys/types.h>
# include <sys/ipc.h>
#endif

#include <sys/stat.h>
#include <errno.h>

#define SHM_PROTECT() \
	do { \
		if (ZCG(accel_directives).protect_memory) { \
			zend_accel_shared_protect(1); \
		} \
	} while (0)
#define SHM_UNPROTECT() \
	do { \
		if (ZCG(accel_directives).protect_memory) { \
			zend_accel_shared_protect(0); \
		} \
	} while (0)

ZEND_EXTENSION();

#ifndef ZTS
zend_accel_globals accel_globals;
#else
int accel_globals_id;
#if defined(COMPILE_DL_OPCACHE)
ZEND_TSRMLS_CACHE_DEFINE()
#endif
#endif

/* Points to the structure shared across all PHP processes */
zend_accel_shared_globals *accel_shared_globals = NULL;

/* true globals, no need for thread safety */
zend_bool accel_startup_ok = 0;
static char *zps_failure_reason = NULL;
char *zps_api_failure_reason = NULL;
#if ENABLE_FILE_CACHE_FALLBACK
zend_bool fallback_process = 0; /* process uses file cache fallback */
#endif

static zend_op_array *(*accelerator_orig_compile_file)(zend_file_handle *file_handle, int type);
static int (*accelerator_orig_zend_stream_open_function)(const char *filename, zend_file_handle *handle );
static zend_string *(*accelerator_orig_zend_resolve_path)(const char *filename, int filename_len);
static void (*orig_chdir)(INTERNAL_FUNCTION_PARAMETERS) = NULL;
static ZEND_INI_MH((*orig_include_path_on_modify)) = NULL;

static void accel_gen_system_id(void);

#ifdef ZEND_WIN32
# define INCREMENT(v) InterlockedIncrement64(&ZCSG(v))
# define DECREMENT(v) InterlockedDecrement64(&ZCSG(v))
# define LOCKVAL(v)   (ZCSG(v))
#endif

#ifdef ZEND_WIN32
static time_t zend_accel_get_time(void)
{
	FILETIME now;
	GetSystemTimeAsFileTime(&now);

	return (time_t) ((((((__int64)now.dwHighDateTime) << 32)|now.dwLowDateTime) - 116444736000000000L)/10000000);
}
#else
# define zend_accel_get_time() time(NULL)
#endif

static inline int is_stream_path(const char *filename)
{
	const char *p;

	for (p = filename;
	     (*p >= 'a' && *p <= 'z') ||
	     (*p >= 'A' && *p <= 'Z') ||
	     (*p >= '0' && *p <= '9') ||
	     *p == '+' || *p == '-' || *p == '.';
	     p++);
	return ((p != filename) && (p[0] == ':') && (p[1] == '/') && (p[2] == '/'));
}

static inline int is_cacheable_stream_path(const char *filename)
{
	return memcmp(filename, "file://", sizeof("file://") - 1) == 0 ||
	       memcmp(filename, "phar://", sizeof("phar://") - 1) == 0;
}

/* O+ overrides PHP chdir() function and remembers the current working directory
 * in ZCG(cwd) and ZCG(cwd_len). Later accel_getcwd() can use stored value and
 * avoid getcwd() call.
 */
static ZEND_FUNCTION(accel_chdir)
{
	char cwd[MAXPATHLEN];

	orig_chdir(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	if (VCWD_GETCWD(cwd, MAXPATHLEN)) {
		if (ZCG(cwd)) {
			zend_string_release(ZCG(cwd));
		}
		ZCG(cwd) = zend_string_init(cwd, strlen(cwd), 0);
	} else {
		if (ZCG(cwd)) {
			zend_string_release(ZCG(cwd));
			ZCG(cwd) = NULL;
		}
	}
	ZCG(cwd_key_len) = 0;
	ZCG(cwd_check) = 1;
}

static inline zend_string* accel_getcwd(void)
{
	if (ZCG(cwd)) {
		return ZCG(cwd);
	} else {
		char cwd[MAXPATHLEN + 1];

		if (!VCWD_GETCWD(cwd, MAXPATHLEN)) {
			return NULL;
		}
		ZCG(cwd) = zend_string_init(cwd, strlen(cwd), 0);
		ZCG(cwd_key_len) = 0;
		ZCG(cwd_check) = 1;
		return ZCG(cwd);
	}
}

void zend_accel_schedule_restart_if_necessary(zend_accel_restart_reason reason)
{
	if ((((double) ZSMMG(wasted_shared_memory)) / ZCG(accel_directives).memory_consumption) >= ZCG(accel_directives).max_wasted_percentage) {
 		zend_accel_schedule_restart(reason);
	}
}

/* O+ tracks changes of "include_path" directive. It stores all the requested
 * values in ZCG(include_paths) shared hash table, current value in
 * ZCG(include_path)/ZCG(include_path_len) and one letter "path key" in
 * ZCG(include_path_key).
 */
static ZEND_INI_MH(accel_include_path_on_modify)
{
	int ret = orig_include_path_on_modify(entry, new_value, mh_arg1, mh_arg2, mh_arg3, stage);

	if (ret == SUCCESS) {
		ZCG(include_path) = new_value;
		ZCG(include_path_key_len) = 0;
		ZCG(include_path_check) = 1;
	}
	return ret;
}

static inline void accel_restart_enter(void)
{
#ifdef ZEND_WIN32
	INCREMENT(restart_in);
#else
# ifdef _AIX
	static FLOCK_STRUCTURE(restart_in_progress, F_WRLCK, SEEK_SET, 2, 1);
# else
	static const FLOCK_STRUCTURE(restart_in_progress, F_WRLCK, SEEK_SET, 2, 1);
#endif

	if (fcntl(lock_file, F_SETLK, &restart_in_progress) == -1) {
		zend_accel_error(ACCEL_LOG_DEBUG, "RestartC(+1):  %s (%d)", strerror(errno), errno);
	}
#endif
	ZCSG(restart_in_progress) = 1;
}

static inline void accel_restart_leave(void)
{
#ifdef ZEND_WIN32
	ZCSG(restart_in_progress) = 0;
	DECREMENT(restart_in);
#else
# ifdef _AIX
	static FLOCK_STRUCTURE(restart_finished, F_UNLCK, SEEK_SET, 2, 1);
# else
	static const FLOCK_STRUCTURE(restart_finished, F_UNLCK, SEEK_SET, 2, 1);
# endif

	ZCSG(restart_in_progress) = 0;
	if (fcntl(lock_file, F_SETLK, &restart_finished) == -1) {
		zend_accel_error(ACCEL_LOG_DEBUG, "RestartC(-1):  %s (%d)", strerror(errno), errno);
	}
#endif
}

static inline int accel_restart_is_active(void)
{
	if (ZCSG(restart_in_progress)) {
#ifndef ZEND_WIN32
		FLOCK_STRUCTURE(restart_check, F_WRLCK, SEEK_SET, 2, 1);

		if (fcntl(lock_file, F_GETLK, &restart_check) == -1) {
			zend_accel_error(ACCEL_LOG_DEBUG, "RestartC:  %s (%d)", strerror(errno), errno);
			return FAILURE;
		}
		if (restart_check.l_type == F_UNLCK) {
			ZCSG(restart_in_progress) = 0;
			return 0;
		} else {
			return 1;
		}
#else
		return LOCKVAL(restart_in) != 0;
#endif
	}
	return 0;
}

/* Creates a read lock for SHM access */
static inline int accel_activate_add(void)
{
#ifdef ZEND_WIN32
	INCREMENT(mem_usage);
#else
# ifdef _AIX
	static FLOCK_STRUCTURE(mem_usage_lock, F_RDLCK, SEEK_SET, 1, 1);
# else
	static const FLOCK_STRUCTURE(mem_usage_lock, F_RDLCK, SEEK_SET, 1, 1);
# endif

	if (fcntl(lock_file, F_SETLK, &mem_usage_lock) == -1) {
		zend_accel_error(ACCEL_LOG_DEBUG, "UpdateC(+1):  %s (%d)", strerror(errno), errno);
		return FAILURE;
	}
#endif
	return SUCCESS;
}

/* Releases a lock for SHM access */
static inline void accel_deactivate_sub(void)
{
#ifdef ZEND_WIN32
	if (ZCG(counted)) {
		DECREMENT(mem_usage);
		ZCG(counted) = 0;
	}
#else
# ifdef _AIX
	static FLOCK_STRUCTURE(mem_usage_unlock, F_UNLCK, SEEK_SET, 1, 1);
# else
	static const FLOCK_STRUCTURE(mem_usage_unlock, F_UNLCK, SEEK_SET, 1, 1);
# endif

	if (fcntl(lock_file, F_SETLK, &mem_usage_unlock) == -1) {
		zend_accel_error(ACCEL_LOG_DEBUG, "UpdateC(-1):  %s (%d)", strerror(errno), errno);
	}
#endif
}

static inline void accel_unlock_all(void)
{
#ifdef ZEND_WIN32
	accel_deactivate_sub();
#else
# ifdef _AIX
	static FLOCK_STRUCTURE(mem_usage_unlock_all, F_UNLCK, SEEK_SET, 0, 0);
# else
	static const FLOCK_STRUCTURE(mem_usage_unlock_all, F_UNLCK, SEEK_SET, 0, 0);
# endif

	if (fcntl(lock_file, F_SETLK, &mem_usage_unlock_all) == -1) {
		zend_accel_error(ACCEL_LOG_DEBUG, "UnlockAll:  %s (%d)", strerror(errno), errno);
	}
#endif
}

/* Interned strings support */
static zend_string *(*orig_new_interned_string)(zend_string *str);
static void (*orig_interned_strings_snapshot)(void);
static void (*orig_interned_strings_restore)(void);

/* O+ disables creation of interned strings by regular PHP compiler, instead,
 * it creates interned strings in shared memory when saves a script.
 * Such interned strings are shared across all PHP processes
 */
static zend_string *accel_new_interned_string_for_php(zend_string *str)
{
	return str;
}

static void accel_interned_strings_snapshot_for_php(void)
{
}

static void accel_interned_strings_restore_for_php(void)
{
}

#ifndef ZTS
static void accel_interned_strings_restore_state(void)
{
    uint idx = ZCSG(interned_strings).nNumUsed;
    uint nIndex;
    Bucket *p;

	memset(ZCSG(interned_strings_saved_top),
			0, ZCSG(interned_strings_top) - ZCSG(interned_strings_saved_top));
	ZCSG(interned_strings_top) = ZCSG(interned_strings_saved_top);
    while (idx > 0) {
    	idx--;
		p = ZCSG(interned_strings).arData + idx;
		if ((char*)p->key < ZCSG(interned_strings_top)) break;
		ZCSG(interned_strings).nNumUsed--;
		ZCSG(interned_strings).nNumOfElements--;

		nIndex = p->h | ZCSG(interned_strings).nTableMask;
		if (HT_HASH(&ZCSG(interned_strings), nIndex) == HT_IDX_TO_HASH(idx)) {
			HT_HASH(&ZCSG(interned_strings), nIndex) = Z_NEXT(p->val);
		} else {
			uint32_t prev = HT_HASH(&ZCSG(interned_strings), nIndex);
			while (Z_NEXT(HT_HASH_TO_BUCKET(&ZCSG(interned_strings), prev)->val) != idx) {
				prev = Z_NEXT(HT_HASH_TO_BUCKET(&ZCSG(interned_strings), prev)->val);
 			}
			Z_NEXT(HT_HASH_TO_BUCKET(&ZCSG(interned_strings), prev)->val) = Z_NEXT(p->val);
 		}
	}
}

static void accel_interned_strings_save_state(void)
{
	ZCSG(interned_strings_saved_top) = ZCSG(interned_strings_top);
}
#endif

#ifndef ZTS
static zend_string *accel_find_interned_string(zend_string *str)
{
/* for now interned strings are supported only for non-ZTS build */
	zend_ulong h;
	uint nIndex;
	uint idx;
	Bucket *arData, *p;

	if (IS_ACCEL_INTERNED(str)) {
		/* this is already an interned string */
		return str;
	}
	if (!ZCG(counted)) {
		if (accel_activate_add() == FAILURE) {
			return str;
		}
		ZCG(counted) = 1;
	}

	h = zend_string_hash_val(str);
	nIndex = h | ZCSG(interned_strings).nTableMask;

	/* check for existing interned string */
	idx = HT_HASH(&ZCSG(interned_strings), nIndex);
	arData = ZCSG(interned_strings).arData;
	while (idx != HT_INVALID_IDX) {
		p = HT_HASH_TO_BUCKET_EX(arData, idx);
		if ((p->h == h) && (ZSTR_LEN(p->key) == ZSTR_LEN(str))) {
			if (!memcmp(ZSTR_VAL(p->key), ZSTR_VAL(str), ZSTR_LEN(str))) {
				return p->key;
			}
		}
		idx = Z_NEXT(p->val);
	}

	return NULL;
}
#endif

zend_string *accel_new_interned_string(zend_string *str)
{
/* for now interned strings are supported only for non-ZTS build */
#ifndef ZTS
	zend_ulong h;
	uint nIndex;
	uint idx;
	Bucket *p;

#ifdef HAVE_OPCACHE_FILE_CACHE
	if (ZCG(accel_directives).file_cache_only) {
		return str;
	}
#endif

	if (IS_ACCEL_INTERNED(str)) {
		/* this is already an interned string */
		return str;
	}

	h = zend_string_hash_val(str);
	nIndex = h | ZCSG(interned_strings).nTableMask;

	/* check for existing interned string */
	idx = HT_HASH(&ZCSG(interned_strings), nIndex);
	while (idx != HT_INVALID_IDX) {
		p = HT_HASH_TO_BUCKET(&ZCSG(interned_strings), idx);
		if ((p->h == h) && (ZSTR_LEN(p->key) == ZSTR_LEN(str))) {
			if (!memcmp(ZSTR_VAL(p->key), ZSTR_VAL(str), ZSTR_LEN(str))) {
				zend_string_release(str);
				return p->key;
			}
		}
		idx = Z_NEXT(p->val);
	}

	if (ZCSG(interned_strings_top) + ZEND_MM_ALIGNED_SIZE(_ZSTR_STRUCT_SIZE(ZSTR_LEN(str))) >=
	    ZCSG(interned_strings_end)) {
	    /* no memory, return the same non-interned string */
		zend_accel_error(ACCEL_LOG_WARNING, "Interned string buffer overflow");
		return str;
	}

	/* create new interning string in shared interned strings buffer */

	idx = ZCSG(interned_strings).nNumUsed++;
	ZCSG(interned_strings).nNumOfElements++;
	p = ZCSG(interned_strings).arData + idx;
	p->key = (zend_string*) ZCSG(interned_strings_top);
	ZCSG(interned_strings_top) += ZEND_MM_ALIGNED_SIZE(_ZSTR_STRUCT_SIZE(ZSTR_LEN(str)));
	p->h = h;
	GC_REFCOUNT(p->key) = 1;
#if 1
	/* optimized single assignment */
	GC_TYPE_INFO(p->key) = IS_STRING | ((IS_STR_INTERNED | IS_STR_PERMANENT) << 8);
#else
	GC_TYPE(p->key) = IS_STRING;
	GC_FLAGS(p->key) = IS_STR_INTERNED | IS_STR_PERMANENT;
#endif
	ZSTR_H(p->key) = ZSTR_H(str);
	ZSTR_LEN(p->key) = ZSTR_LEN(str);
	memcpy(ZSTR_VAL(p->key), ZSTR_VAL(str), ZSTR_LEN(str));
	ZVAL_INTERNED_STR(&p->val, p->key);
	Z_NEXT(p->val) = HT_HASH(&ZCSG(interned_strings), nIndex);
	HT_HASH(&ZCSG(interned_strings), nIndex) = HT_IDX_TO_HASH(idx);
	zend_string_release(str);
	return p->key;
#else
	return str;
#endif
}

#ifndef ZTS
/* Copy PHP interned strings from PHP process memory into the shared memory */
static void accel_use_shm_interned_strings(void)
{
	uint idx, j;
	Bucket *p, *q;

	/* empty string */
	CG(empty_string) = accel_new_interned_string(CG(empty_string));
	for (j = 0; j < 256; j++) {
		char s[2];
		s[0] = j;
		s[1] = 0;
		CG(one_char_string)[j] = accel_new_interned_string(zend_string_init(s, 1, 0));
	}

	/* function table hash keys */
	for (idx = 0; idx < CG(function_table)->nNumUsed; idx++) {
		p = CG(function_table)->arData + idx;
		if (Z_TYPE(p->val) == IS_UNDEF) continue;
		if (p->key) {
			p->key = accel_new_interned_string(p->key);
		}
		if (Z_FUNC(p->val)->common.function_name) {
			Z_FUNC(p->val)->common.function_name = accel_new_interned_string(Z_FUNC(p->val)->common.function_name);
		}
	}

	/* class table hash keys, class names, properties, methods, constants, etc */
	for (idx = 0; idx < CG(class_table)->nNumUsed; idx++) {
		zend_class_entry *ce;

		p = CG(class_table)->arData + idx;
		if (Z_TYPE(p->val) == IS_UNDEF) continue;
		ce = (zend_class_entry*)Z_PTR(p->val);

		if (p->key) {
			p->key = accel_new_interned_string(p->key);
		}

		if (ce->name) {
			ce->name = accel_new_interned_string(ce->name);
		}

		for (j = 0; j < ce->properties_info.nNumUsed; j++) {
			zend_property_info *info;

			q = ce->properties_info.arData + j;
			if (Z_TYPE(q->val) == IS_UNDEF) continue;

			info = (zend_property_info*)Z_PTR(q->val);

			if (q->key) {
				q->key = accel_new_interned_string(q->key);
			}

			if (info->name) {
				info->name = accel_new_interned_string(info->name);
			}
		}

		for (j = 0; j < ce->function_table.nNumUsed; j++) {
			q = ce->function_table.arData + j;
			if (Z_TYPE(q->val) == IS_UNDEF) continue;
			if (q->key) {
				q->key = accel_new_interned_string(q->key);
			}
			if (Z_FUNC(q->val)->common.function_name) {
				Z_FUNC(q->val)->common.function_name = accel_new_interned_string(Z_FUNC(q->val)->common.function_name);
			}
		}

		for (j = 0; j < ce->constants_table.nNumUsed; j++) {
			q = ce->constants_table.arData + j;
			if (Z_TYPE(q->val) == IS_UNDEF) continue;
			if (q->key) {
				q->key = accel_new_interned_string(q->key);
			}
		}
	}

	/* constant hash keys */
	for (idx = 0; idx < EG(zend_constants)->nNumUsed; idx++) {
		p = EG(zend_constants)->arData + idx;
		if (Z_TYPE(p->val) == IS_UNDEF) continue;
		if (p->key) {
			p->key = accel_new_interned_string(p->key);
		}
	}

	/* auto globals hash keys and names */
	for (idx = 0; idx < CG(auto_globals)->nNumUsed; idx++) {
		zend_auto_global *auto_global;

		p = CG(auto_globals)->arData + idx;
		if (Z_TYPE(p->val) == IS_UNDEF) continue;

		auto_global = (zend_auto_global*)Z_PTR(p->val);;

		zend_string_addref(auto_global->name);
		auto_global->name = accel_new_interned_string(auto_global->name);
		if (p->key) {
			p->key = accel_new_interned_string(p->key);
		}
	}
}
#endif

#ifndef ZEND_WIN32
static inline void kill_all_lockers(struct flock *mem_usage_check)
{
	int success, tries;
	/* so that other process won't try to force while we are busy cleaning up */
	ZCSG(force_restart_time) = 0;
	while (mem_usage_check->l_pid > 0) {
		/* Clear previous errno, reset success and tries */
		errno = 0;
		success = 0;
		tries = 10;

		while (tries--) {
			zend_accel_error(ACCEL_LOG_WARNING, "Attempting to kill locker %d", mem_usage_check->l_pid);
			if (kill(mem_usage_check->l_pid, SIGKILL)) {
				if (errno == ESRCH) {
					/* Process died before the signal was sent */
					success = 1;
					zend_accel_error(ACCEL_LOG_WARNING, "Process %d died before SIGKILL was sent", mem_usage_check->l_pid);
				}
				break;
			}
			/* give it a chance to die */
			usleep(20000);
			if (kill(mem_usage_check->l_pid, 0)) {
				if (errno == ESRCH) {
					/* successfully killed locker, process no longer exists  */
					success = 1;
					zend_accel_error(ACCEL_LOG_WARNING, "Killed locker %d", mem_usage_check->l_pid);
				}
				break;
			}
			usleep(10000);
		}
		if (!success) {
			/* errno is not ESRCH or we ran out of tries to kill the locker */
			ZCSG(force_restart_time) = time(NULL); /* restore forced restart request */
			/* cannot kill the locker, bail out with error */
			zend_accel_error(ACCEL_LOG_ERROR, "Cannot kill process %d: %s!", mem_usage_check->l_pid, strerror(errno));
		}

		mem_usage_check->l_type = F_WRLCK;
		mem_usage_check->l_whence = SEEK_SET;
		mem_usage_check->l_start = 1;
		mem_usage_check->l_len = 1;
		mem_usage_check->l_pid = -1;
		if (fcntl(lock_file, F_GETLK, mem_usage_check) == -1) {
			zend_accel_error(ACCEL_LOG_DEBUG, "KLockers:  %s (%d)", strerror(errno), errno);
			break;
		}

		if (mem_usage_check->l_type == F_UNLCK || mem_usage_check->l_pid <= 0) {
			break;
		}
	}
}
#endif

static inline int accel_is_inactive(void)
{
#ifdef ZEND_WIN32
	if (LOCKVAL(mem_usage) == 0) {
		return SUCCESS;
	}
#else
	FLOCK_STRUCTURE(mem_usage_check, F_WRLCK, SEEK_SET, 1, 1);

	mem_usage_check.l_pid = -1;
	if (fcntl(lock_file, F_GETLK, &mem_usage_check) == -1) {
		zend_accel_error(ACCEL_LOG_DEBUG, "UpdateC:  %s (%d)", strerror(errno), errno);
		return FAILURE;
	}
	if (mem_usage_check.l_type == F_UNLCK) {
		return SUCCESS;
	}

	if (ZCG(accel_directives).force_restart_timeout
		&& ZCSG(force_restart_time)
		&& time(NULL) >= ZCSG(force_restart_time)) {
		zend_accel_error(ACCEL_LOG_WARNING, "Forced restart at %d (after %d seconds), locked by %d", time(NULL), ZCG(accel_directives).force_restart_timeout, mem_usage_check.l_pid);
		kill_all_lockers(&mem_usage_check);

		return FAILURE; /* next request should be able to restart it */
	}
#endif

	return FAILURE;
}

static int zend_get_stream_timestamp(const char *filename, zend_stat_t *statbuf)
{
	php_stream_wrapper *wrapper;
	php_stream_statbuf stream_statbuf;
	int ret, er;

	if (!filename) {
		return FAILURE;
	}

	wrapper = php_stream_locate_url_wrapper(filename, NULL, STREAM_LOCATE_WRAPPERS_ONLY);
	if (!wrapper) {
		return FAILURE;
	}
	if (!wrapper->wops || !wrapper->wops->url_stat) {
		statbuf->st_mtime = 1;
		return SUCCESS; /* anything other than 0 is considered to be a valid timestamp */
	}

	er = EG(error_reporting);
	EG(error_reporting) = 0;
	zend_try {
		ret = wrapper->wops->url_stat(wrapper, (char*)filename, PHP_STREAM_URL_STAT_QUIET, &stream_statbuf, NULL);
	} zend_catch {
		ret = -1;
	} zend_end_try();
	EG(error_reporting) = er;

	if (ret != 0) {
		return FAILURE;
	}

	*statbuf = stream_statbuf.sb;
	return SUCCESS;
}

#if ZEND_WIN32
static accel_time_t zend_get_file_handle_timestamp_win(zend_file_handle *file_handle, size_t *size)
{
	static unsigned __int64 utc_base = 0;
	static FILETIME utc_base_ft;
	WIN32_FILE_ATTRIBUTE_DATA fdata;

	if (!file_handle->opened_path) {
		return 0;
	}

	if (!utc_base) {
		SYSTEMTIME st;

		st.wYear = 1970;
		st.wMonth = 1;
		st.wDay = 1;
		st.wHour = 0;
		st.wMinute = 0;
		st.wSecond = 0;
		st.wMilliseconds = 0;

		SystemTimeToFileTime (&st, &utc_base_ft);
		utc_base = (((unsigned __int64)utc_base_ft.dwHighDateTime) << 32) + utc_base_ft.dwLowDateTime;
    }

	if (file_handle->opened_path && GetFileAttributesEx(file_handle->opened_path->val, GetFileExInfoStandard, &fdata) != 0) {
		unsigned __int64 ftime;

		if (CompareFileTime (&fdata.ftLastWriteTime, &utc_base_ft) < 0) {
			return 0;
		}

		ftime = (((unsigned __int64)fdata.ftLastWriteTime.dwHighDateTime) << 32) + fdata.ftLastWriteTime.dwLowDateTime - utc_base;
		ftime /= 10000000L;

		if (size) {
			*size = (size_t)((((unsigned __int64)fdata.nFileSizeHigh) << 32) + (unsigned __int64)fdata.nFileSizeLow);
		}
		return (accel_time_t)ftime;
	}
	return 0;
}
#endif

accel_time_t zend_get_file_handle_timestamp(zend_file_handle *file_handle, size_t *size)
{
	zend_stat_t statbuf;
#ifdef ZEND_WIN32
	accel_time_t res;
#endif

	if (sapi_module.get_stat &&
	    !EG(current_execute_data) &&
	    file_handle->filename == SG(request_info).path_translated) {

		zend_stat_t *tmpbuf = sapi_module.get_stat();

		if (tmpbuf) {
			if (size) {
				*size = tmpbuf->st_size;
			}
			return tmpbuf->st_mtime;
		}
	}

#ifdef ZEND_WIN32
	res = zend_get_file_handle_timestamp_win(file_handle, size);
	if (res) {
		return res;
	}
#endif

	switch (file_handle->type) {
		case ZEND_HANDLE_FD:
			if (zend_fstat(file_handle->handle.fd, &statbuf) == -1) {
				return 0;
			}
			break;
		case ZEND_HANDLE_FP:
			if (zend_fstat(fileno(file_handle->handle.fp), &statbuf) == -1) {
				if (zend_get_stream_timestamp(file_handle->filename, &statbuf) != SUCCESS) {
					return 0;
				}
			}
			break;
		case ZEND_HANDLE_FILENAME:
		case ZEND_HANDLE_MAPPED:
			if (file_handle->opened_path) {
				char *file_path = ZSTR_VAL(file_handle->opened_path);

				if (is_stream_path(file_path)) {
					if (zend_get_stream_timestamp(file_path, &statbuf) == SUCCESS) {
						break;
					}
				}
				if (VCWD_STAT(file_path, &statbuf) != -1) {
					break;
				}
			}

			if (zend_get_stream_timestamp(file_handle->filename, &statbuf) != SUCCESS) {
				return 0;
			}
			break;
		case ZEND_HANDLE_STREAM:
			{
				php_stream *stream = (php_stream *)file_handle->handle.stream.handle;
				php_stream_statbuf sb;
				int ret, er;

				if (!stream ||
				    !stream->ops ||
				    !stream->ops->stat) {
					return 0;
				}

				er = EG(error_reporting);
				EG(error_reporting) = 0;
				zend_try {
					ret = stream->ops->stat(stream, &sb);
				} zend_catch {
					ret = -1;
				} zend_end_try();
				EG(error_reporting) = er;
				if (ret != 0) {
					return 0;
				}

				statbuf = sb.sb;
			}
			break;

		default:
			return 0;
	}

	if (size) {
		*size = statbuf.st_size;
	}
	return statbuf.st_mtime;
}

static inline int do_validate_timestamps(zend_persistent_script *persistent_script, zend_file_handle *file_handle)
{
	zend_file_handle ps_handle;
	zend_string *full_path_ptr = NULL;

	/** check that the persistent script is indeed the same file we cached
	 * (if part of the path is a symlink than it possible that the user will change it)
	 * See bug #15140
	 */
	if (file_handle->opened_path) {
		if (persistent_script->full_path != file_handle->opened_path &&
		    (ZSTR_LEN(persistent_script->full_path) != ZSTR_LEN(file_handle->opened_path) ||
		     memcmp(ZSTR_VAL(persistent_script->full_path), ZSTR_VAL(file_handle->opened_path), ZSTR_LEN(file_handle->opened_path)) != 0)) {
			return FAILURE;
		}
	} else {
		full_path_ptr = accelerator_orig_zend_resolve_path(file_handle->filename, strlen(file_handle->filename));
		if (full_path_ptr &&
		    persistent_script->full_path != full_path_ptr &&
		    (ZSTR_LEN(persistent_script->full_path) != ZSTR_LEN(full_path_ptr) ||
		     memcmp(ZSTR_VAL(persistent_script->full_path), ZSTR_VAL(full_path_ptr), ZSTR_LEN(full_path_ptr)) != 0)) {
			zend_string_release(full_path_ptr);
			return FAILURE;
		}
		file_handle->opened_path = full_path_ptr;
	}

	if (persistent_script->timestamp == 0) {
		if (full_path_ptr) {
			zend_string_release(full_path_ptr);
			file_handle->opened_path = NULL;
		}
		return FAILURE;
	}

	if (zend_get_file_handle_timestamp(file_handle, NULL) == persistent_script->timestamp) {
		if (full_path_ptr) {
			zend_string_release(full_path_ptr);
			file_handle->opened_path = NULL;
		}
		return SUCCESS;
	}
	if (full_path_ptr) {
		zend_string_release(full_path_ptr);
		file_handle->opened_path = NULL;
	}

	ps_handle.type = ZEND_HANDLE_FILENAME;
	ps_handle.filename = ZSTR_VAL(persistent_script->full_path);
	ps_handle.opened_path = persistent_script->full_path;

	if (zend_get_file_handle_timestamp(&ps_handle, NULL) == persistent_script->timestamp) {
		return SUCCESS;
	}

	return FAILURE;
}

int validate_timestamp_and_record(zend_persistent_script *persistent_script, zend_file_handle *file_handle)
{
	if (ZCG(accel_directives).revalidate_freq &&
	    persistent_script->dynamic_members.revalidate >= ZCG(request_time)) {
		return SUCCESS;
	} else if (do_validate_timestamps(persistent_script, file_handle) == FAILURE) {
		return FAILURE;
	} else {
		persistent_script->dynamic_members.revalidate = ZCG(request_time) + ZCG(accel_directives).revalidate_freq;
		return SUCCESS;
	}
}

/* Instead of resolving full real path name each time we need to identify file,
 * we create a key that consist from requested file name, current working
 * directory, current include_path, etc */
char *accel_make_persistent_key(const char *path, int path_length, int *key_len)
{
	int key_length;

	/* CWD and include_path don't matter for absolute file names and streams */
    if (IS_ABSOLUTE_PATH(path, path_length)) {
		/* pass */
		ZCG(key_len) = 0;
    } else if (UNEXPECTED(is_stream_path(path))) {
		if (!is_cacheable_stream_path(path)) {
			return NULL;
		}
		/* pass */
		ZCG(key_len) = 0;
    } else if (UNEXPECTED(!ZCG(accel_directives).use_cwd)) {
		/* pass */
		ZCG(key_len) = 0;
    } else {
		const char *include_path = NULL, *cwd = NULL;
		int include_path_len = 0, cwd_len = 0;
		zend_string *parent_script = NULL;
		size_t parent_script_len = 0;

		if (EXPECTED(ZCG(cwd_key_len))) {
			cwd = ZCG(cwd_key);
			cwd_len = ZCG(cwd_key_len);
		} else {
			zend_string *cwd_str = accel_getcwd();

			if (UNEXPECTED(!cwd_str)) {
				/* we don't handle this well for now. */
				zend_accel_error(ACCEL_LOG_INFO, "getcwd() failed for '%s' (%d), please try to set opcache.use_cwd to 0 in ini file", path, errno);
				return NULL;
			}
			cwd = ZSTR_VAL(cwd_str);
			cwd_len = ZSTR_LEN(cwd_str);
#ifndef ZTS
			if (ZCG(cwd_check)) {
				ZCG(cwd_check) = 0;
				if ((ZCG(counted) || ZCSG(accelerator_enabled))) {

					zend_string *str = accel_find_interned_string(cwd_str);
					if (!str) {
						SHM_UNPROTECT();
						zend_shared_alloc_lock();
						str = accel_new_interned_string(zend_string_copy(cwd_str));
						if (str == cwd_str) {
							zend_string_release(str);
							str = NULL;
						}
						zend_shared_alloc_unlock();
						SHM_PROTECT();
					}
					if (str) {
						char buf[32];
						char *res = zend_print_long_to_buf(buf + sizeof(buf) - 1, ZSTR_VAL(str) - ZCSG(interned_strings_start));

						cwd_len = ZCG(cwd_key_len) = buf + sizeof(buf) - 1 - res;
						cwd = ZCG(cwd_key);
						memcpy(ZCG(cwd_key), res, cwd_len + 1);
					}
				}
			}
#endif
		}

		if (EXPECTED(ZCG(include_path_key_len))) {
			include_path = ZCG(include_path_key);
			include_path_len = ZCG(include_path_key_len);
		} else if (!ZCG(include_path) || ZSTR_LEN(ZCG(include_path)) == 0) {
			include_path = "";
			include_path_len = 0;
		} else {
			include_path = ZSTR_VAL(ZCG(include_path));
			include_path_len = ZSTR_LEN(ZCG(include_path));

#ifndef ZTS
			if (ZCG(include_path_check)) {
				ZCG(include_path_check) = 0;
				if ((ZCG(counted) || ZCSG(accelerator_enabled))) {

					zend_string *str = accel_find_interned_string(ZCG(include_path));
					if (!str) {
						SHM_UNPROTECT();
						zend_shared_alloc_lock();
						str = accel_new_interned_string(zend_string_copy(ZCG(include_path)));
						if (str == ZCG(include_path)) {
							str = NULL;
						}
						zend_shared_alloc_unlock();
						SHM_PROTECT();
					}
					if (str) {
						char buf[32];
						char *res = zend_print_long_to_buf(buf + sizeof(buf) - 1, ZSTR_VAL(str) - ZCSG(interned_strings_start));

						include_path_len = ZCG(include_path_key_len) = buf + sizeof(buf) - 1 - res;
						include_path = ZCG(include_path_key);
						memcpy(ZCG(include_path_key), res, include_path_len + 1);
					}
				}
			}
#endif
		}

		/* Calculate key length */
		if (UNEXPECTED((size_t)(cwd_len + path_length + include_path_len + 2) >= sizeof(ZCG(key)))) {
			return NULL;
		}

		/* Generate key
		 * Note - the include_path must be the last element in the key,
		 * since in itself, it may include colons (which we use to separate
		 * different components of the key)
		 */
		memcpy(ZCG(key), path, path_length);
		ZCG(key)[path_length] = ':';
		key_length = path_length + 1;
		memcpy(ZCG(key) + key_length, cwd, cwd_len);
		key_length += cwd_len;

		if (include_path_len) {
			ZCG(key)[key_length] = ':';
			key_length += 1;
			memcpy(ZCG(key) + key_length, include_path, include_path_len);
			key_length += include_path_len;
		}

		/* Here we add to the key the parent script directory,
		 * since fopen_wrappers from version 4.0.7 use current script's path
		 * in include path too.
		 */
		if (EXPECTED(EG(current_execute_data)) &&
		    EXPECTED((parent_script = zend_get_executed_filename_ex()) != NULL)) {

			parent_script_len = ZSTR_LEN(parent_script);
			while ((--parent_script_len > 0) && !IS_SLASH(ZSTR_VAL(parent_script)[parent_script_len]));

			if (UNEXPECTED((size_t)(key_length + parent_script_len + 1) >= sizeof(ZCG(key)))) {
				return NULL;
			}
			ZCG(key)[key_length] = ':';
			key_length += 1;
			memcpy(ZCG(key) + key_length, ZSTR_VAL(parent_script), parent_script_len);
			key_length += parent_script_len;
		}
		ZCG(key)[key_length] = '\0';
		*key_len = ZCG(key_len) = key_length;
		return ZCG(key);
	}

	/* not use_cwd */
	*key_len = path_length;
	return (char*)path;
}

int zend_accel_invalidate(const char *filename, int filename_len, zend_bool force)
{
	zend_string *realpath;
	zend_persistent_script *persistent_script;

	if (!ZCG(enabled) || !accel_startup_ok || !ZCSG(accelerator_enabled) || accelerator_shm_read_lock() != SUCCESS) {
		return FAILURE;
	}

	realpath = accelerator_orig_zend_resolve_path(filename, filename_len);

	if (!realpath) {
		return FAILURE;
	}

#ifdef HAVE_OPCACHE_FILE_CACHE
	if (ZCG(accel_directives).file_cache) {
		zend_file_cache_invalidate(realpath);
	}
#endif

	persistent_script = zend_accel_hash_find(&ZCSG(hash), realpath);
	if (persistent_script && !persistent_script->corrupted) {
		zend_file_handle file_handle;

		file_handle.type = ZEND_HANDLE_FILENAME;
		file_handle.filename = ZSTR_VAL(realpath);
		file_handle.opened_path = realpath;

		if (force ||
			!ZCG(accel_directives).validate_timestamps ||
			do_validate_timestamps(persistent_script, &file_handle) == FAILURE) {
			SHM_UNPROTECT();
			zend_shared_alloc_lock();
			if (!persistent_script->corrupted) {
				persistent_script->corrupted = 1;
				persistent_script->timestamp = 0;
				ZSMMG(wasted_shared_memory) += persistent_script->dynamic_members.memory_consumption;
				if (ZSMMG(memory_exhausted)) {
					zend_accel_restart_reason reason =
						zend_accel_hash_is_full(&ZCSG(hash)) ? ACCEL_RESTART_HASH : ACCEL_RESTART_OOM;
					zend_accel_schedule_restart_if_necessary(reason);
				}
			}
			zend_shared_alloc_unlock();
			SHM_PROTECT();
		}
	}

	accelerator_shm_read_unlock();
	zend_string_release(realpath);

	return SUCCESS;
}

/* Adds another key for existing cached script */
static void zend_accel_add_key(char *key, unsigned int key_length, zend_accel_hash_entry *bucket)
{
	if (!zend_accel_hash_str_find(&ZCSG(hash), key, key_length)) {
		if (zend_accel_hash_is_full(&ZCSG(hash))) {
			zend_accel_error(ACCEL_LOG_DEBUG, "No more entries in hash table!");
			ZSMMG(memory_exhausted) = 1;
			zend_accel_schedule_restart_if_necessary(ACCEL_RESTART_HASH);
		} else {
			char *new_key = zend_shared_alloc(key_length + 1);
			if (new_key) {
				memcpy(new_key, key, key_length + 1);
				if (zend_accel_hash_update(&ZCSG(hash), new_key, key_length, 1, bucket)) {
					zend_accel_error(ACCEL_LOG_INFO, "Added key '%s'", new_key);
				}
			} else {
				zend_accel_schedule_restart_if_necessary(ACCEL_RESTART_OOM);
			}
		}
	}
}

#ifdef HAVE_OPCACHE_FILE_CACHE
static zend_persistent_script *cache_script_in_file_cache(zend_persistent_script *new_persistent_script, int *from_shared_memory)
{
	uint memory_used;

	/* Check if script may be stored in shared memory */
	if (!zend_accel_script_persistable(new_persistent_script)) {
		return new_persistent_script;
	}

	if (!zend_accel_script_optimize(new_persistent_script)) {
		return new_persistent_script;
	}

	zend_shared_alloc_init_xlat_table();

	/* Calculate the required memory size */
	memory_used = zend_accel_script_persist_calc(new_persistent_script, NULL, 0);

	/* Allocate memory block */
#ifdef __SSE2__
	/* Align to 64-byte boundary */
	ZCG(mem) = zend_arena_alloc(&CG(arena), memory_used + 64);
	ZCG(mem) = (void*)(((zend_uintptr_t)ZCG(mem) + 63L) & ~63L);
#else
	ZCG(mem) = zend_arena_alloc(&CG(arena), memory_used);
#endif

	/* Copy into shared memory */
	new_persistent_script = zend_accel_script_persist(new_persistent_script, NULL, 0);

	zend_shared_alloc_destroy_xlat_table();

	new_persistent_script->is_phar =
		new_persistent_script->full_path &&
		strstr(ZSTR_VAL(new_persistent_script->full_path), ".phar") &&
		!strstr(ZSTR_VAL(new_persistent_script->full_path), "://");

	/* Consistency check */
	if ((char*)new_persistent_script->mem + new_persistent_script->size != (char*)ZCG(mem)) {
		zend_accel_error(
			((char*)new_persistent_script->mem + new_persistent_script->size < (char*)ZCG(mem)) ? ACCEL_LOG_ERROR : ACCEL_LOG_WARNING,
			"Internal error: wrong size calculation: %s start=0x%08x, end=0x%08x, real=0x%08x\n",
			ZSTR_VAL(new_persistent_script->full_path),
			new_persistent_script->mem,
			(char *)new_persistent_script->mem + new_persistent_script->size,
			ZCG(mem));
	}

	new_persistent_script->dynamic_members.checksum = zend_accel_script_checksum(new_persistent_script);

	zend_file_cache_script_store(new_persistent_script, 0);

	*from_shared_memory = 1;
	return new_persistent_script;
}
#endif

static zend_persistent_script *cache_script_in_shared_memory(zend_persistent_script *new_persistent_script, char *key, unsigned int key_length, int *from_shared_memory)
{
	zend_accel_hash_entry *bucket;
	uint memory_used;

	/* Check if script may be stored in shared memory */
	if (!zend_accel_script_persistable(new_persistent_script)) {
		return new_persistent_script;
	}

	if (!zend_accel_script_optimize(new_persistent_script)) {
		return new_persistent_script;
	}

	/* exclusive lock */
	zend_shared_alloc_lock();

	if (zend_accel_hash_is_full(&ZCSG(hash))) {
		zend_accel_error(ACCEL_LOG_DEBUG, "No more entries in hash table!");
		ZSMMG(memory_exhausted) = 1;
		zend_accel_schedule_restart_if_necessary(ACCEL_RESTART_HASH);
		zend_shared_alloc_unlock();
		return new_persistent_script;
	}

	/* Check if we still need to put the file into the cache (may be it was
	 * already stored by another process. This final check is done under
	 * exclusive lock) */
	bucket = zend_accel_hash_find_entry(&ZCSG(hash), new_persistent_script->full_path);
	if (bucket) {
		zend_persistent_script *existing_persistent_script = (zend_persistent_script *)bucket->data;

		if (!existing_persistent_script->corrupted) {
			if (key &&
			    (!ZCG(accel_directives).validate_timestamps ||
			     (new_persistent_script->timestamp == existing_persistent_script->timestamp))) {
				zend_accel_add_key(key, key_length, bucket);
			}
			zend_shared_alloc_unlock();
			return new_persistent_script;
		}
	}


	zend_shared_alloc_init_xlat_table();

	/* Calculate the required memory size */
	memory_used = zend_accel_script_persist_calc(new_persistent_script, key, key_length);

	/* Allocate shared memory */
#ifdef __SSE2__
	/* Align to 64-byte boundary */
	ZCG(mem) = zend_shared_alloc(memory_used + 64);
	ZCG(mem) = (void*)(((zend_uintptr_t)ZCG(mem) + 63L) & ~63L);
#else
	ZCG(mem) = zend_shared_alloc(memory_used);
#endif
	if (!ZCG(mem)) {
		zend_shared_alloc_destroy_xlat_table();
		zend_accel_schedule_restart_if_necessary(ACCEL_RESTART_OOM);
		zend_shared_alloc_unlock();
		return new_persistent_script;
	}

	/* Copy into shared memory */
	new_persistent_script = zend_accel_script_persist(new_persistent_script, &key, key_length);

	zend_shared_alloc_destroy_xlat_table();

	new_persistent_script->is_phar =
		new_persistent_script->full_path &&
		strstr(ZSTR_VAL(new_persistent_script->full_path), ".phar") &&
		!strstr(ZSTR_VAL(new_persistent_script->full_path), "://");

	/* Consistency check */
	if ((char*)new_persistent_script->mem + new_persistent_script->size != (char*)ZCG(mem)) {
		zend_accel_error(
			((char*)new_persistent_script->mem + new_persistent_script->size < (char*)ZCG(mem)) ? ACCEL_LOG_ERROR : ACCEL_LOG_WARNING,
			"Internal error: wrong size calculation: %s start=0x%08x, end=0x%08x, real=0x%08x\n",
			ZSTR_VAL(new_persistent_script->full_path),
			new_persistent_script->mem,
			(char *)new_persistent_script->mem + new_persistent_script->size,
			ZCG(mem));
	}

	new_persistent_script->dynamic_members.checksum = zend_accel_script_checksum(new_persistent_script);

	/* store script structure in the hash table */
	bucket = zend_accel_hash_update(&ZCSG(hash), ZSTR_VAL(new_persistent_script->full_path), ZSTR_LEN(new_persistent_script->full_path), 0, new_persistent_script);
	if (bucket) {
		zend_accel_error(ACCEL_LOG_INFO, "Cached script '%s'", ZSTR_VAL(new_persistent_script->full_path));
		if (key &&
		    /* key may contain non-persistent PHAR aliases (see issues #115 and #149) */
		    memcmp(key, "phar://", sizeof("phar://") - 1) != 0 &&
		    (ZSTR_LEN(new_persistent_script->full_path) != key_length ||
		     memcmp(ZSTR_VAL(new_persistent_script->full_path), key, key_length) != 0)) {
			/* link key to the same persistent script in hash table */
			if (zend_accel_hash_update(&ZCSG(hash), key, key_length, 1, bucket)) {
				zend_accel_error(ACCEL_LOG_INFO, "Added key '%s'", key);
			} else {
				zend_accel_error(ACCEL_LOG_DEBUG, "No more entries in hash table!");
				ZSMMG(memory_exhausted) = 1;
				zend_accel_schedule_restart_if_necessary(ACCEL_RESTART_HASH);
			}
		}
	}

	new_persistent_script->dynamic_members.memory_consumption = ZEND_ALIGNED_SIZE(new_persistent_script->size);

	zend_shared_alloc_unlock();

#ifdef HAVE_OPCACHE_FILE_CACHE
	if (ZCG(accel_directives).file_cache) {
		SHM_PROTECT();
		zend_file_cache_script_store(new_persistent_script, 1);
		SHM_UNPROTECT();
	}
#endif

	*from_shared_memory = 1;
	return new_persistent_script;
}

static const struct jit_auto_global_info
{
    const char *name;
    size_t len;
} jit_auto_globals_info[] = {
    { "_SERVER",  sizeof("_SERVER")-1},
    { "_ENV",     sizeof("_ENV")-1},
    { "_REQUEST", sizeof("_REQUEST")-1},
    { "GLOBALS",  sizeof("GLOBALS")-1},
};

static zend_string *jit_auto_globals_str[4];

static int zend_accel_get_auto_globals(void)
{
	int i, ag_size = (sizeof(jit_auto_globals_info) / sizeof(jit_auto_globals_info[0]));
	int n = 1;
	int mask = 0;

	for (i = 0; i < ag_size ; i++) {
		if (zend_hash_exists(&EG(symbol_table), jit_auto_globals_str[i])) {
			mask |= n;
		}
		n += n;
	}
	return mask;
}

static int zend_accel_get_auto_globals_no_jit(void)
{
	if (zend_hash_exists(&EG(symbol_table), jit_auto_globals_str[3])) {
		return 8;
	}
	return 0;
}

static void zend_accel_set_auto_globals(int mask)
{
	int i, ag_size = (sizeof(jit_auto_globals_info) / sizeof(jit_auto_globals_info[0]));
	int n = 1;

	for (i = 0; i < ag_size ; i++) {
		if ((mask & n) && !(ZCG(auto_globals_mask) & n)) {
			ZCG(auto_globals_mask) |= n;
			zend_is_auto_global(jit_auto_globals_str[i]);
		}
		n += n;
	}
}

static void zend_accel_init_auto_globals(void)
{
	int i, ag_size = (sizeof(jit_auto_globals_info) / sizeof(jit_auto_globals_info[0]));

	for (i = 0; i < ag_size ; i++) {
		jit_auto_globals_str[i] = zend_string_init(jit_auto_globals_info[i].name, jit_auto_globals_info[i].len, 1);
		zend_string_hash_val(jit_auto_globals_str[i]);
		jit_auto_globals_str[i] = accel_new_interned_string(jit_auto_globals_str[i]);
	}
}

static zend_persistent_script *opcache_compile_file(zend_file_handle *file_handle, int type, char *key, unsigned int key_length, zend_op_array **op_array_p)
{
	zend_persistent_script *new_persistent_script;
	zend_op_array *orig_active_op_array;
	HashTable *orig_function_table, *orig_class_table;
	zval orig_user_error_handler;
	zend_op_array *op_array;
	int do_bailout = 0;
	accel_time_t timestamp = 0;
	uint32_t orig_compiler_options = 0;

    /* Try to open file */
    if (file_handle->type == ZEND_HANDLE_FILENAME) {
        if (accelerator_orig_zend_stream_open_function(file_handle->filename, file_handle) == SUCCESS) {
        	/* key may be changed by zend_stream_open_function() */
        	if (key == ZCG(key)) {
        		key_length = ZCG(key_len);
        	}
        } else {
			*op_array_p = NULL;
			if (type == ZEND_REQUIRE) {
				zend_message_dispatcher(ZMSG_FAILED_REQUIRE_FOPEN, file_handle->filename);
				zend_bailout();
			} else {
				zend_message_dispatcher(ZMSG_FAILED_INCLUDE_FOPEN, file_handle->filename);
			}
			return NULL;
    	}
    }

	/* check blacklist right after ensuring that file was opened */
	if (file_handle->opened_path && zend_accel_blacklist_is_blacklisted(&accel_blacklist, ZSTR_VAL(file_handle->opened_path))) {
		ZCSG(blacklist_misses)++;
		*op_array_p = accelerator_orig_compile_file(file_handle, type);
		return NULL;
	}

	if (ZCG(accel_directives).validate_timestamps ||
	    ZCG(accel_directives).file_update_protection ||
	    ZCG(accel_directives).max_file_size > 0) {
		size_t size = 0;

		/* Obtain the file timestamps, *before* actually compiling them,
		 * otherwise we have a race-condition.
		 */
		timestamp = zend_get_file_handle_timestamp(file_handle, ZCG(accel_directives).max_file_size > 0 ? &size : NULL);

		/* If we can't obtain a timestamp (that means file is possibly socket)
		 *  we won't cache it
		 */
		if (timestamp == 0) {
			*op_array_p = accelerator_orig_compile_file(file_handle, type);
			return NULL;
		}

		/* check if file is too new (may be it's not written completely yet) */
		if (ZCG(accel_directives).file_update_protection &&
		    (ZCG(request_time) - ZCG(accel_directives).file_update_protection < timestamp)) {
			*op_array_p = accelerator_orig_compile_file(file_handle, type);
			return NULL;
		}

		if (ZCG(accel_directives).max_file_size > 0 && size > (size_t)ZCG(accel_directives).max_file_size) {
			ZCSG(blacklist_misses)++;
			*op_array_p = accelerator_orig_compile_file(file_handle, type);
			return NULL;
		}
	}

	new_persistent_script = create_persistent_script();

	/* Save the original values for the op_array, function table and class table */
	orig_active_op_array = CG(active_op_array);
	orig_function_table = CG(function_table);
	orig_class_table = CG(class_table);
	ZVAL_COPY_VALUE(&orig_user_error_handler, &EG(user_error_handler));

	/* Override them with ours */
	CG(function_table) = &ZCG(function_table);
	EG(class_table) = CG(class_table) = &new_persistent_script->class_table;
	ZVAL_UNDEF(&EG(user_error_handler));

	zend_try {
		orig_compiler_options = CG(compiler_options);
		CG(compiler_options) |= ZEND_COMPILE_HANDLE_OP_ARRAY;
		CG(compiler_options) |= ZEND_COMPILE_IGNORE_INTERNAL_CLASSES;
		CG(compiler_options) |= ZEND_COMPILE_DELAYED_BINDING;
		CG(compiler_options) |= ZEND_COMPILE_NO_CONSTANT_SUBSTITUTION;
		op_array = *op_array_p = accelerator_orig_compile_file(file_handle, type);
		CG(compiler_options) = orig_compiler_options;
	} zend_catch {
		op_array = NULL;
		do_bailout = 1;
		CG(compiler_options) = orig_compiler_options;
	} zend_end_try();

	/* Restore originals */
	CG(active_op_array) = orig_active_op_array;
	CG(function_table) = orig_function_table;
	EG(class_table) = CG(class_table) = orig_class_table;
	EG(user_error_handler) = orig_user_error_handler;

	if (!op_array) {
		/* compilation failed */
		free_persistent_script(new_persistent_script, 1);
		zend_accel_free_user_functions(&ZCG(function_table));
		if (do_bailout) {
			zend_bailout();
		}
		return NULL;
	}

	/* Build the persistent_script structure.
	   Here we aren't sure we would store it, but we will need it
	   further anyway.
	*/
	zend_accel_move_user_functions(&ZCG(function_table), &new_persistent_script->function_table);
	new_persistent_script->main_op_array = *op_array;

	efree(op_array); /* we have valid persistent_script, so it's safe to free op_array */

    /* Fill in the ping_auto_globals_mask for the new script. If jit for auto globals is enabled we
       will have to ping the used auto global variables before execution */
	if (PG(auto_globals_jit)) {
		new_persistent_script->ping_auto_globals_mask = zend_accel_get_auto_globals();
	} else {
		new_persistent_script->ping_auto_globals_mask = zend_accel_get_auto_globals_no_jit();
	}

	if (ZCG(accel_directives).validate_timestamps) {
		/* Obtain the file timestamps, *before* actually compiling them,
		 * otherwise we have a race-condition.
		 */
		new_persistent_script->timestamp = timestamp;
		new_persistent_script->dynamic_members.revalidate = ZCG(request_time) + ZCG(accel_directives).revalidate_freq;
	}

	if (file_handle->opened_path) {
		new_persistent_script->full_path = zend_string_copy(file_handle->opened_path);
	} else {
		new_persistent_script->full_path = zend_string_init(file_handle->filename, strlen(file_handle->filename), 0);
	}
	zend_string_hash_val(new_persistent_script->full_path);

	/* Now persistent_script structure is ready in process memory */
	return new_persistent_script;
}

#ifdef HAVE_OPCACHE_FILE_CACHE
zend_op_array *file_cache_compile_file(zend_file_handle *file_handle, int type)
{
	zend_persistent_script *persistent_script;
	zend_op_array *op_array = NULL;
	int from_memory; /* if the script we've got is stored in SHM */

	if (is_stream_path(file_handle->filename) &&
	    !is_cacheable_stream_path(file_handle->filename)) {
		return accelerator_orig_compile_file(file_handle, type);
	}

	if (!file_handle->opened_path) {
		if (file_handle->type == ZEND_HANDLE_FILENAME &&
		    accelerator_orig_zend_stream_open_function(file_handle->filename, file_handle) == FAILURE) {
			if (type == ZEND_REQUIRE) {
				zend_message_dispatcher(ZMSG_FAILED_REQUIRE_FOPEN, file_handle->filename);
				zend_bailout();
			} else {
				zend_message_dispatcher(ZMSG_FAILED_INCLUDE_FOPEN, file_handle->filename);
			}
			return NULL;
	    }
	}

	SHM_UNPROTECT();
	persistent_script = zend_file_cache_script_load(file_handle);
	SHM_PROTECT();
	if (persistent_script) {
		/* see bug #15471 (old BTS) */
		if (persistent_script->full_path) {
			if (!EG(current_execute_data) || !EG(current_execute_data)->opline ||
			    !EG(current_execute_data)->func ||
			    !ZEND_USER_CODE(EG(current_execute_data)->func->common.type) ||
			    EG(current_execute_data)->opline->opcode != ZEND_INCLUDE_OR_EVAL ||
			    (EG(current_execute_data)->opline->extended_value != ZEND_INCLUDE_ONCE &&
			     EG(current_execute_data)->opline->extended_value != ZEND_REQUIRE_ONCE)) {
				if (zend_hash_add_empty_element(&EG(included_files), persistent_script->full_path) != NULL) {
					/* ext/phar has to load phar's metadata into memory */
					if (persistent_script->is_phar) {
						php_stream_statbuf ssb;
						char *fname = emalloc(sizeof("phar://") + ZSTR_LEN(persistent_script->full_path));

						memcpy(fname, "phar://", sizeof("phar://") - 1);
						memcpy(fname + sizeof("phar://") - 1, ZSTR_VAL(persistent_script->full_path), ZSTR_LEN(persistent_script->full_path) + 1);
						php_stream_stat_path(fname, &ssb);
						efree(fname);
					}
				}
			}
		}
		zend_file_handle_dtor(file_handle);

	    if (persistent_script->ping_auto_globals_mask) {
			zend_accel_set_auto_globals(persistent_script->ping_auto_globals_mask);
		}

		return zend_accel_load_script(persistent_script, 1);
	}

	persistent_script = opcache_compile_file(file_handle, type, NULL, 0, &op_array);

	if (persistent_script) {
		from_memory = 0;
		persistent_script = cache_script_in_file_cache(persistent_script, &from_memory);
		return zend_accel_load_script(persistent_script, from_memory);
	}

	return op_array;
}
#endif

/* zend_compile() replacement */
zend_op_array *persistent_compile_file(zend_file_handle *file_handle, int type)
{
	zend_persistent_script *persistent_script = NULL;
	char *key = NULL;
	int key_length;
	int from_shared_memory; /* if the script we've got is stored in SHM */

	if (!file_handle->filename || !ZCG(enabled) || !accel_startup_ok) {
		/* The Accelerator is disabled, act as if without the Accelerator */
		return accelerator_orig_compile_file(file_handle, type);
#ifdef HAVE_OPCACHE_FILE_CACHE
	} else if (ZCG(accel_directives).file_cache_only) {
		return file_cache_compile_file(file_handle, type);
#endif
	} else if ((!ZCG(counted) && !ZCSG(accelerator_enabled)) ||
	           (ZCSG(restart_in_progress) && accel_restart_is_active())) {
#ifdef HAVE_OPCACHE_FILE_CACHE
		if (ZCG(accel_directives).file_cache) {
			return file_cache_compile_file(file_handle, type);
		}
#endif
		return accelerator_orig_compile_file(file_handle, type);
	}

	/* In case this callback is called from include_once, require_once or it's
	 * a main FastCGI request, the key must be already calculated, and cached
	 * persistent script already found */
	if (ZCG(cache_persistent_script) &&
	    ((!EG(current_execute_data) &&
	      file_handle->filename == SG(request_info).path_translated &&
	      ZCG(cache_opline) == NULL) ||
	     (EG(current_execute_data) &&
	      EG(current_execute_data)->func &&
	      ZEND_USER_CODE(EG(current_execute_data)->func->common.type) &&
	      ZCG(cache_opline) == EG(current_execute_data)->opline))) {

		persistent_script = ZCG(cache_persistent_script);
		if (ZCG(key_len)) {
			key = ZCG(key);
			key_length = ZCG(key_len);
		}

	} else {
		if (!ZCG(accel_directives).revalidate_path) {
			/* try to find cached script by key */
			key = accel_make_persistent_key(file_handle->filename, strlen(file_handle->filename), &key_length);
			if (!key) {
				return accelerator_orig_compile_file(file_handle, type);
			}
			persistent_script = zend_accel_hash_str_find(&ZCSG(hash), key, key_length);
		}
		if (!persistent_script) {
			/* try to find cached script by full real path */
			zend_accel_hash_entry *bucket;

			/* open file to resolve the path */
		    if (file_handle->type == ZEND_HANDLE_FILENAME &&
        		accelerator_orig_zend_stream_open_function(file_handle->filename, file_handle) == FAILURE) {
				if (type == ZEND_REQUIRE) {
					zend_message_dispatcher(ZMSG_FAILED_REQUIRE_FOPEN, file_handle->filename);
					zend_bailout();
				} else {
					zend_message_dispatcher(ZMSG_FAILED_INCLUDE_FOPEN, file_handle->filename);
				}
				return NULL;
		    }

			if (file_handle->opened_path) {
				bucket = zend_accel_hash_find_entry(&ZCSG(hash), file_handle->opened_path);

				if (bucket) {
					persistent_script = (zend_persistent_script *)bucket->data;

					if (key && !persistent_script->corrupted) {
						SHM_UNPROTECT();
						zend_shared_alloc_lock();
						zend_accel_add_key(key, key_length, bucket);
						zend_shared_alloc_unlock();
						SHM_PROTECT();
					}
				}
			}
		}
	}

	/* clear cache */
	ZCG(cache_opline) = NULL;
	ZCG(cache_persistent_script) = NULL;

	if (persistent_script && persistent_script->corrupted) {
		persistent_script = NULL;
	}

	/* Make sure we only increase the currently running processes semaphore
     * once each execution (this function can be called more than once on
     * each execution)
     */
	if (!ZCG(counted)) {
		if (accel_activate_add() == FAILURE) {
#ifdef HAVE_OPCACHE_FILE_CACHE
			if (ZCG(accel_directives).file_cache) {
				return file_cache_compile_file(file_handle, type);
			}
#endif
			return accelerator_orig_compile_file(file_handle, type);
		}
		ZCG(counted) = 1;
	}

	/* Revalidate acessibility of cached file */
	if (EXPECTED(persistent_script != NULL) &&
	    UNEXPECTED(ZCG(accel_directives).validate_permission) &&
	    file_handle->type == ZEND_HANDLE_FILENAME &&
	    UNEXPECTED(access(ZSTR_VAL(persistent_script->full_path), R_OK) != 0)) {
		if (type == ZEND_REQUIRE) {
			zend_message_dispatcher(ZMSG_FAILED_REQUIRE_FOPEN, file_handle->filename);
			zend_bailout();
		} else {
			zend_message_dispatcher(ZMSG_FAILED_INCLUDE_FOPEN, file_handle->filename);
		}
		return NULL;
	}

	SHM_UNPROTECT();

	/* If script is found then validate_timestamps if option is enabled */
	if (persistent_script && ZCG(accel_directives).validate_timestamps) {
		if (validate_timestamp_and_record(persistent_script, file_handle) == FAILURE) {
			zend_shared_alloc_lock();
			if (!persistent_script->corrupted) {
				persistent_script->corrupted = 1;
				persistent_script->timestamp = 0;
				ZSMMG(wasted_shared_memory) += persistent_script->dynamic_members.memory_consumption;
				if (ZSMMG(memory_exhausted)) {
					zend_accel_restart_reason reason =
						zend_accel_hash_is_full(&ZCSG(hash)) ? ACCEL_RESTART_HASH : ACCEL_RESTART_OOM;
					zend_accel_schedule_restart_if_necessary(reason);
				}
			}
			zend_shared_alloc_unlock();
			persistent_script = NULL;
		}
	}

	/* if turned on - check the compiled script ADLER32 checksum */
	if (persistent_script && ZCG(accel_directives).consistency_checks
		&& persistent_script->dynamic_members.hits % ZCG(accel_directives).consistency_checks == 0) {

		unsigned int checksum = zend_accel_script_checksum(persistent_script);
		if (checksum != persistent_script->dynamic_members.checksum ) {
			/* The checksum is wrong */
			zend_accel_error(ACCEL_LOG_INFO, "Checksum failed for '%s':  expected=0x%0.8X, found=0x%0.8X",
							 ZSTR_VAL(persistent_script->full_path), persistent_script->dynamic_members.checksum, checksum);
			zend_shared_alloc_lock();
			if (!persistent_script->corrupted) {
				persistent_script->corrupted = 1;
				persistent_script->timestamp = 0;
				ZSMMG(wasted_shared_memory) += persistent_script->dynamic_members.memory_consumption;
				if (ZSMMG(memory_exhausted)) {
					zend_accel_restart_reason reason =
						zend_accel_hash_is_full(&ZCSG(hash)) ? ACCEL_RESTART_HASH : ACCEL_RESTART_OOM;
					zend_accel_schedule_restart_if_necessary(reason);
				}
			}
			zend_shared_alloc_unlock();
			persistent_script = NULL;
		}
	}

#ifdef HAVE_OPCACHE_FILE_CACHE
	/* Check the second level cache */
	if (!persistent_script && ZCG(accel_directives).file_cache) {
		persistent_script = zend_file_cache_script_load(file_handle);
	}
#endif

	/* If script was not found or invalidated by validate_timestamps */
	if (!persistent_script) {
		uint32_t old_const_num = zend_hash_next_free_element(EG(zend_constants));
		zend_op_array *op_array;

		/* Cache miss.. */
		ZCSG(misses)++;

		/* No memory left. Behave like without the Accelerator */
		if (ZSMMG(memory_exhausted) || ZCSG(restart_pending)) {
			SHM_PROTECT();
			return accelerator_orig_compile_file(file_handle, type);
		}

		/* Try and cache the script and assume that it is returned from_shared_memory.
         * If it isn't compile_and_cache_file() changes the flag to 0
         */
       	from_shared_memory = 0;
		persistent_script = opcache_compile_file(file_handle, type, key, key ? key_length : 0, &op_array);
		if (persistent_script) {
			persistent_script = cache_script_in_shared_memory(persistent_script, key, key ? key_length : 0, &from_shared_memory);
		}

		/* Caching is disabled, returning op_array;
		 * or something went wrong during compilation, returning NULL
		 */
		if (!persistent_script) {
			SHM_PROTECT();
			return op_array;
		}
		if (from_shared_memory) {
			/* Delete immutable arrays moved into SHM */
			uint32_t new_const_num = zend_hash_next_free_element(EG(zend_constants));
			while (new_const_num > old_const_num) {
				new_const_num--;
				zend_hash_index_del(EG(zend_constants), new_const_num);
			}
		}
	} else {

#if !ZEND_WIN32
		ZCSG(hits)++; /* TBFixed: may lose one hit */
		persistent_script->dynamic_members.hits++; /* see above */
#else
#ifdef _M_X64
		InterlockedIncrement64(&ZCSG(hits));
#else
		InterlockedIncrement(&ZCSG(hits));
#endif
		InterlockedIncrement64(&persistent_script->dynamic_members.hits);
#endif

		/* see bug #15471 (old BTS) */
		if (persistent_script->full_path) {
			if (!EG(current_execute_data) || !EG(current_execute_data)->opline ||
			    !EG(current_execute_data)->func ||
			    !ZEND_USER_CODE(EG(current_execute_data)->func->common.type) ||
			    EG(current_execute_data)->opline->opcode != ZEND_INCLUDE_OR_EVAL ||
			    (EG(current_execute_data)->opline->extended_value != ZEND_INCLUDE_ONCE &&
			     EG(current_execute_data)->opline->extended_value != ZEND_REQUIRE_ONCE)) {
				if (zend_hash_add_empty_element(&EG(included_files), persistent_script->full_path) != NULL) {
					/* ext/phar has to load phar's metadata into memory */
					if (persistent_script->is_phar) {
						php_stream_statbuf ssb;
						char *fname = emalloc(sizeof("phar://") + ZSTR_LEN(persistent_script->full_path));

						memcpy(fname, "phar://", sizeof("phar://") - 1);
						memcpy(fname + sizeof("phar://") - 1, ZSTR_VAL(persistent_script->full_path), ZSTR_LEN(persistent_script->full_path) + 1);
						php_stream_stat_path(fname, &ssb);
						efree(fname);
					}
				}
			}
		}
		zend_file_handle_dtor(file_handle);
		from_shared_memory = 1;
	}

	persistent_script->dynamic_members.last_used = ZCG(request_time);

	SHM_PROTECT();

    /* Fetch jit auto globals used in the script before execution */
    if (persistent_script->ping_auto_globals_mask) {
		zend_accel_set_auto_globals(persistent_script->ping_auto_globals_mask);
	}

	return zend_accel_load_script(persistent_script, from_shared_memory);
}

/* zend_stream_open_function() replacement for PHP 5.3 and above */
static int persistent_stream_open_function(const char *filename, zend_file_handle *handle)
{
	if (ZCG(cache_persistent_script)) {
		/* check if callback is called from include_once or it's a main request */
		if ((!EG(current_execute_data) &&
		     filename == SG(request_info).path_translated &&
		     ZCG(cache_opline) == NULL) ||
		    (EG(current_execute_data) &&
		     EG(current_execute_data)->func &&
		     ZEND_USER_CODE(EG(current_execute_data)->func->common.type) &&
		     ZCG(cache_opline) == EG(current_execute_data)->opline)) {

			/* we are in include_once or FastCGI request */
			handle->filename = (char*)filename;
			handle->free_filename = 0;
			handle->opened_path = zend_string_copy(ZCG(cache_persistent_script)->full_path);
			handle->type = ZEND_HANDLE_FILENAME;
			return SUCCESS;
		}
		ZCG(cache_opline) = NULL;
		ZCG(cache_persistent_script) = NULL;
	}
	return accelerator_orig_zend_stream_open_function(filename, handle);
}

/* zend_resolve_path() replacement for PHP 5.3 and above */
static zend_string* persistent_zend_resolve_path(const char *filename, int filename_len)
{
	if (ZCG(enabled) && accel_startup_ok &&
	    (ZCG(counted) || ZCSG(accelerator_enabled)) &&
	    !ZCSG(restart_in_progress)) {

		/* check if callback is called from include_once or it's a main request */
		if ((!EG(current_execute_data) &&
		     filename == SG(request_info).path_translated) ||
		    (EG(current_execute_data) &&
		     EG(current_execute_data)->func &&
		     ZEND_USER_CODE(EG(current_execute_data)->func->common.type) &&
		     EG(current_execute_data)->opline->opcode == ZEND_INCLUDE_OR_EVAL &&
		     (EG(current_execute_data)->opline->extended_value == ZEND_INCLUDE_ONCE ||
		      EG(current_execute_data)->opline->extended_value == ZEND_REQUIRE_ONCE))) {

			/* we are in include_once or FastCGI request */
			zend_string *resolved_path;
			int key_length;
			char *key = NULL;
			
			if (!ZCG(accel_directives).revalidate_path) {
				/* lookup by "not-real" path */
				key = accel_make_persistent_key(filename, filename_len, &key_length);
				if (key) {
					zend_accel_hash_entry *bucket = zend_accel_hash_str_find_entry(&ZCSG(hash), key, key_length);
					if (bucket != NULL) {
						zend_persistent_script *persistent_script = (zend_persistent_script *)bucket->data;
						if (!persistent_script->corrupted) {
							ZCG(cache_opline) = EG(current_execute_data) ? EG(current_execute_data)->opline : NULL;
							ZCG(cache_persistent_script) = persistent_script;
							return zend_string_copy(persistent_script->full_path);
						}
					}
				} else {
					ZCG(cache_opline) = NULL;
					ZCG(cache_persistent_script) = NULL;
					return accelerator_orig_zend_resolve_path(filename, filename_len);
				}
			}

			/* find the full real path */
			resolved_path = accelerator_orig_zend_resolve_path(filename, filename_len);

			if (resolved_path) {
				/* lookup by real path */
				zend_accel_hash_entry *bucket = zend_accel_hash_find_entry(&ZCSG(hash), resolved_path);
				if (bucket) {
					zend_persistent_script *persistent_script = (zend_persistent_script *)bucket->data;
					if (!persistent_script->corrupted) {
						if (key) {
							/* add another "key" for the same bucket */
							SHM_UNPROTECT();
							zend_shared_alloc_lock();
							zend_accel_add_key(key, key_length, bucket);
							zend_shared_alloc_unlock();
							SHM_PROTECT();
						} else {
							ZCG(key_len) = 0;
						}
						ZCG(cache_opline) = EG(current_execute_data) ? EG(current_execute_data)->opline : NULL;
						ZCG(cache_persistent_script) = persistent_script;
						return resolved_path;
					}
				}
			}

			ZCG(cache_opline) = NULL;
			ZCG(cache_persistent_script) = NULL;
			return resolved_path;
		}
	}
	ZCG(cache_opline) = NULL;
	ZCG(cache_persistent_script) = NULL;
	return accelerator_orig_zend_resolve_path(filename, filename_len);
}

static void zend_reset_cache_vars(void)
{
	ZSMMG(memory_exhausted) = 0;
	ZCSG(hits) = 0;
	ZCSG(misses) = 0;
	ZCSG(blacklist_misses) = 0;
	ZSMMG(wasted_shared_memory) = 0;
	ZCSG(restart_pending) = 0;
	ZCSG(force_restart_time) = 0;
}

static void accel_reset_pcre_cache(void)
{
	Bucket *p;

	ZEND_HASH_FOREACH_BUCKET(&PCRE_G(pcre_cache), p) {
		/* Remove PCRE cache entries with inconsistent keys */
		if (zend_accel_in_shm(p->key)) {
			p->key = NULL;
			zend_hash_del_bucket(&PCRE_G(pcre_cache), p);
		}
	} ZEND_HASH_FOREACH_END();
}

static void accel_activate(void)
{
	zend_bool reset_pcre = 0;

	if (!ZCG(enabled) || !accel_startup_ok) {
		return;
	}

	if (!ZCG(function_table).nTableSize) {
		zend_hash_init(&ZCG(function_table), zend_hash_num_elements(CG(function_table)), NULL, ZEND_FUNCTION_DTOR, 1);
		zend_accel_copy_internal_functions();
	}

	/* PHP-5.4 and above return "double", but we use 1 sec precision */
	ZCG(auto_globals_mask) = 0;
	ZCG(request_time) = (time_t)sapi_get_request_time();
	ZCG(cache_opline) = NULL;
	ZCG(cache_persistent_script) = NULL;
	ZCG(include_path_key_len) = 0;
	ZCG(include_path_check) = 1;

	/* check if ZCG(function_table) wasn't somehow polluted on the way */
	if (ZCG(internal_functions_count) != zend_hash_num_elements(&ZCG(function_table))) {
		zend_accel_error(ACCEL_LOG_WARNING, "Internal functions count changed - was %d, now %d", ZCG(internal_functions_count), zend_hash_num_elements(&ZCG(function_table)));
	}

	ZCG(cwd) = NULL;
	ZCG(cwd_key_len) = 0;
	ZCG(cwd_check) = 1;

#ifdef HAVE_OPCACHE_FILE_CACHE
	if (ZCG(accel_directives).file_cache_only) {
		return;
	}
#endif

#ifndef ZEND_WIN32
	if (ZCG(accel_directives).validate_root) {
		struct stat buf;

		if (stat("/", &buf) != 0) {
			ZCG(root_hash) = 0;
		} else {
			ZCG(root_hash) = buf.st_ino;
			if (sizeof(buf.st_ino) > sizeof(ZCG(root_hash))) {
				if (ZCG(root_hash) != buf.st_ino) {
					zend_string *key = zend_string_init("opcache.enable", sizeof("opcache.enable")-1, 0);
					zend_alter_ini_entry_chars(key, "0", 1, ZEND_INI_SYSTEM, ZEND_INI_STAGE_RUNTIME);
					zend_string_release(key);
					zend_accel_error(ACCEL_LOG_WARNING, "Can't cache files in chroot() directory with too big inode");
					return;
				}
			}
		}
	} else {
		ZCG(root_hash) = 0;
	}
#endif

	SHM_UNPROTECT();

	if (ZCG(counted)) {
#ifdef ZTS
		zend_accel_error(ACCEL_LOG_WARNING, "Stuck count for thread id %d", tsrm_thread_id());
#else
		zend_accel_error(ACCEL_LOG_WARNING, "Stuck count for pid %d", getpid());
#endif
		accel_unlock_all();
		ZCG(counted) = 0;
	}

	if (ZCSG(restart_pending)) {
		zend_shared_alloc_lock();
		if (ZCSG(restart_pending) != 0) { /* check again, to ensure that the cache wasn't already cleaned by another process */
			if (accel_is_inactive() == SUCCESS) {
				zend_accel_error(ACCEL_LOG_DEBUG, "Restarting!");
				ZCSG(restart_pending) = 0;
				switch ZCSG(restart_reason) {
					case ACCEL_RESTART_OOM:
						ZCSG(oom_restarts)++;
						break;
					case ACCEL_RESTART_HASH:
						ZCSG(hash_restarts)++;
						break;
					case ACCEL_RESTART_USER:
						ZCSG(manual_restarts)++;
						break;
				}
				accel_restart_enter();

				zend_reset_cache_vars();
				zend_accel_hash_clean(&ZCSG(hash));

#if !defined(ZTS)
				if (ZCG(accel_directives).interned_strings_buffer) {
					accel_interned_strings_restore_state();
				}
#endif

				zend_shared_alloc_restore_state();
				ZCSG(accelerator_enabled) = ZCSG(cache_status_before_restart);
				if (ZCSG(last_restart_time) < ZCG(request_time)) {
					ZCSG(last_restart_time) = ZCG(request_time);
				} else {
					ZCSG(last_restart_time)++;
				}
				accel_restart_leave();
			}
		} else {
			reset_pcre = 1;
		}
		zend_shared_alloc_unlock();
	}

	SHM_PROTECT();

	if (ZCSG(last_restart_time) != ZCG(last_restart_time)) {
		/* SHM was reinitialized. */
		ZCG(last_restart_time) = ZCSG(last_restart_time);

		/* Reset in-process realpath cache */
		realpath_cache_clean();

		accel_reset_pcre_cache();
	} else if (reset_pcre) {
		accel_reset_pcre_cache();
	}
}

#if !ZEND_DEBUG

/* Fast Request Shutdown
 * =====================
 * Zend Memory Manager frees memory by its own. We don't have to free each
 * allocated block separately, but we like to call all the destructors and
 * callbacks in exactly the same order.
 */
static void accel_fast_hash_destroy(HashTable *ht);

static void accel_fast_zval_dtor(zval *zvalue)
{
tail_call:
	switch (Z_TYPE_P(zvalue)) {
		case IS_ARRAY:
			GC_REMOVE_FROM_BUFFER(Z_ARR_P(zvalue));
			if (Z_ARR_P(zvalue) != &EG(symbol_table)) {
				/* break possible cycles */
				ZVAL_NULL(zvalue);
				accel_fast_hash_destroy(Z_ARRVAL_P(zvalue));
			}
			break;
		case IS_OBJECT:
			OBJ_RELEASE(Z_OBJ_P(zvalue));
			break;
		case IS_RESOURCE:
			zend_list_delete(Z_RES_P(zvalue));
			break;
		case IS_REFERENCE: {
				zend_reference *ref = Z_REF_P(zvalue);

				if (--GC_REFCOUNT(ref) == 0) {
					if (Z_REFCOUNTED(ref->val) && Z_DELREF(ref->val) == 0) {
						zvalue = &ref->val;
						goto tail_call;
					}
				}
			}
			break;
	}
}

static void accel_fast_hash_destroy(HashTable *ht)
{
	Bucket *p = ht->arData;
	Bucket *end = p + ht->nNumUsed;

	while (p != end) {
		if (Z_REFCOUNTED(p->val) && Z_DELREF(p->val) == 0) {
			accel_fast_zval_dtor(&p->val);
		}
		p++;
	}
}

static inline void zend_accel_fast_del_bucket(HashTable *ht, uint32_t idx, Bucket *p)
{
	uint32_t nIndex = p->h | ht->nTableMask;
	uint32_t i = HT_HASH(ht, nIndex);

	ht->nNumOfElements--;
	if (idx != i) {
		Bucket *prev = HT_HASH_TO_BUCKET(ht, i);
		while (Z_NEXT(prev->val) != idx) {
			i = Z_NEXT(prev->val);
			prev = HT_HASH_TO_BUCKET(ht, i);
		}
		Z_NEXT(prev->val) = Z_NEXT(p->val);
 	} else {
		HT_HASH(ht, p->h | ht->nTableMask) = Z_NEXT(p->val);
	}
}

static void zend_accel_fast_shutdown(void)
{
	if (EG(full_tables_cleanup)) {
		return;
	}

	if (EG(objects_store).top > 1 || zend_hash_num_elements(&EG(regular_list)) > 0) {
		/* We don't have to destroy all zvals if they cannot call any destructors */
		zend_try {
			ZEND_HASH_REVERSE_FOREACH(&EG(symbol_table), 0) {
				if (Z_REFCOUNTED(_p->val) && Z_DELREF(_p->val) == 0) {
					accel_fast_zval_dtor(&_p->val);
				}
				zend_accel_fast_del_bucket(&EG(symbol_table), HT_IDX_TO_HASH(_idx-1), _p);
			} ZEND_HASH_FOREACH_END();
		} zend_end_try();
		zend_hash_init(&EG(symbol_table), 8, NULL, NULL, 0);

		ZEND_HASH_REVERSE_FOREACH(EG(function_table), 0) {
			zend_function *func = Z_PTR(_p->val);

			if (func->type == ZEND_INTERNAL_FUNCTION) {
				break;
			} else {
				if (func->op_array.static_variables) {
					if (!(GC_FLAGS(func->op_array.static_variables) & IS_ARRAY_IMMUTABLE)) {
						if (--GC_REFCOUNT(func->op_array.static_variables) == 0) {
							accel_fast_hash_destroy(func->op_array.static_variables);
						}
					}
				}
				zend_accel_fast_del_bucket(EG(function_table), HT_IDX_TO_HASH(_idx-1), _p);
			}
		} ZEND_HASH_FOREACH_END();

		ZEND_HASH_REVERSE_FOREACH(EG(class_table), 0) {
			zend_class_entry *ce = Z_PTR(_p->val);

			if (ce->type == ZEND_INTERNAL_CLASS) {
				break;
			} else {
				if (ce->ce_flags & ZEND_HAS_STATIC_IN_METHODS) {
					zend_function *func;

					ZEND_HASH_FOREACH_PTR(&ce->function_table, func) {
						if (func->type == ZEND_USER_FUNCTION) {
							if (func->op_array.static_variables) {
								if (!(GC_FLAGS(func->op_array.static_variables) & IS_ARRAY_IMMUTABLE)) {
									if (--GC_REFCOUNT(func->op_array.static_variables) == 0) {
										accel_fast_hash_destroy(func->op_array.static_variables);
									}
								}
								func->op_array.static_variables = NULL;
							}
						}
					} ZEND_HASH_FOREACH_END();
				}
				if (ce->static_members_table) {
					int i;

					for (i = 0; i < ce->default_static_members_count; i++) {
						zval *zv = &ce->static_members_table[i];
						ZVAL_UNDEF(&ce->static_members_table[i]);
						if (Z_REFCOUNTED_P(zv) && Z_DELREF_P(zv) == 0) {
							accel_fast_zval_dtor(zv);
						}
					}
					ce->static_members_table = NULL;
				}
				zend_accel_fast_del_bucket(EG(class_table), HT_IDX_TO_HASH(_idx-1), _p);
			}
		} ZEND_HASH_FOREACH_END();

	} else {

		zend_hash_init(&EG(symbol_table), 8, NULL, NULL, 0);

		ZEND_HASH_REVERSE_FOREACH(EG(function_table), 0) {
			zend_function *func = Z_PTR(_p->val);

			if (func->type == ZEND_INTERNAL_FUNCTION) {
				break;
			} else {
				zend_accel_fast_del_bucket(EG(function_table), HT_IDX_TO_HASH(_idx-1), _p);
			}
		} ZEND_HASH_FOREACH_END();

		ZEND_HASH_REVERSE_FOREACH(EG(class_table), 0) {
			zend_class_entry *ce = Z_PTR(_p->val);

			if (ce->type == ZEND_INTERNAL_CLASS) {
				break;
			} else {
				zend_accel_fast_del_bucket(EG(class_table), HT_IDX_TO_HASH(_idx-1), _p);
			}
		} ZEND_HASH_FOREACH_END();
	}

	ZEND_HASH_REVERSE_FOREACH(EG(zend_constants), 0) {
		zend_constant *c = Z_PTR(_p->val);

		if (c->flags & CONST_PERSISTENT) {
			break;
		} else {
			zend_accel_fast_del_bucket(EG(zend_constants), HT_IDX_TO_HASH(_idx-1), _p);
		}
	} ZEND_HASH_FOREACH_END();
	EG(function_table)->nNumUsed = EG(function_table)->nNumOfElements;
	EG(class_table)->nNumUsed = EG(class_table)->nNumOfElements;
	EG(zend_constants)->nNumUsed = EG(zend_constants)->nNumOfElements;

	CG(unclean_shutdown) = 1;
}
#endif

int accel_post_deactivate(void)
{
	if (!ZCG(enabled) || !accel_startup_ok) {
		return SUCCESS;
	}

	zend_shared_alloc_safe_unlock(); /* be sure we didn't leave cache locked */
	accel_unlock_all();
	ZCG(counted) = 0;

	return SUCCESS;
}

static void accel_deactivate(void)
{
	/* ensure that we restore function_table and class_table
	 * In general, they're restored by persistent_compile_file(), but in case
	 * the script is aborted abnormally, they may become messed up.
	 */

	if (ZCG(cwd)) {
		zend_string_release(ZCG(cwd));
		ZCG(cwd) = NULL;
	}

	if (!ZCG(enabled) || !accel_startup_ok) {
		return;
	}

#if !ZEND_DEBUG
	if (ZCG(accel_directives).fast_shutdown && is_zend_mm()) {
		zend_accel_fast_shutdown();
	}
#endif
}

static int accelerator_remove_cb(zend_extension *element1, zend_extension *element2)
{
	(void)element2; /* keep the compiler happy */

	if (!strcmp(element1->name, ACCELERATOR_PRODUCT_NAME )) {
		element1->startup = NULL;
#if 0
		/* We have to call shutdown callback it to free TS resources */
		element1->shutdown = NULL;
#endif
		element1->activate = NULL;
		element1->deactivate = NULL;
		element1->op_array_handler = NULL;

#ifdef __DEBUG_MESSAGES__
        fprintf(stderr, ACCELERATOR_PRODUCT_NAME " is disabled: %s\n", (zps_failure_reason ? zps_failure_reason : "unknown error"));
        fflush(stderr);
#endif
	}

	return 0;
}

static void zps_startup_failure(char *reason, char *api_reason, int (*cb)(zend_extension *, zend_extension *))
{
	accel_startup_ok = 0;
	zps_failure_reason = reason;
	zps_api_failure_reason = api_reason?api_reason:reason;
	zend_llist_del_element(&zend_extensions, NULL, (int (*)(void *, void *))cb);
}

static inline int accel_find_sapi(void)
{
	static const char *supported_sapis[] = {
		"apache",
		"fastcgi",
		"cli-server",
		"cgi-fcgi",
		"fpm-fcgi",
		"isapi",
		"apache2filter",
		"apache2handler",
		"litespeed",
		"uwsgi",
		NULL
	};
	const char **sapi_name;

	if (sapi_module.name) {
		for (sapi_name = supported_sapis; *sapi_name; sapi_name++) {
			if (strcmp(sapi_module.name, *sapi_name) == 0) {
				return SUCCESS;
			}
		}
		if (ZCG(accel_directives).enable_cli && (
		    strcmp(sapi_module.name, "cli") == 0
		  || strcmp(sapi_module.name, "phpdbg") == 0)) {
			return SUCCESS;
		}
	}

	return FAILURE;
}

static int zend_accel_init_shm(void)
{
	zend_shared_alloc_lock();

	accel_shared_globals = zend_shared_alloc(sizeof(zend_accel_shared_globals));
	if (!accel_shared_globals) {
		zend_accel_error(ACCEL_LOG_FATAL, "Insufficient shared memory!");
		zend_shared_alloc_unlock();
		return FAILURE;
	}
	ZSMMG(app_shared_globals) = accel_shared_globals;

	zend_accel_hash_init(&ZCSG(hash), ZCG(accel_directives).max_accelerated_files);

	ZCSG(interned_strings_start) = ZCSG(interned_strings_end) = NULL;
# ifndef ZTS
	zend_hash_init(&ZCSG(interned_strings), (ZCG(accel_directives).interned_strings_buffer * 1024 * 1024) / (sizeof(Bucket) + sizeof(Bucket*) + 8 /* average string length */), NULL, NULL, 1);
	if (ZCG(accel_directives).interned_strings_buffer) {
		void *data;

		ZCSG(interned_strings).nTableMask = -ZCSG(interned_strings).nTableSize;
		data = zend_shared_alloc(HT_SIZE(&ZCSG(interned_strings)));
		ZCSG(interned_strings_start) = zend_shared_alloc((ZCG(accel_directives).interned_strings_buffer * 1024 * 1024));
		if (!data || !ZCSG(interned_strings_start)) {
			zend_accel_error(ACCEL_LOG_FATAL, ACCELERATOR_PRODUCT_NAME " cannot allocate buffer for interned strings");
			zend_shared_alloc_unlock();
			return FAILURE;
		}
		HT_SET_DATA_ADDR(&ZCSG(interned_strings), data);
		HT_HASH_RESET(&ZCSG(interned_strings));
		ZCSG(interned_strings_end)   = ZCSG(interned_strings_start) + (ZCG(accel_directives).interned_strings_buffer * 1024 * 1024);
		ZCSG(interned_strings_top)   = ZCSG(interned_strings_start);

//		orig_interned_strings_start = CG(interned_strings_start);
//		orig_interned_strings_end = CG(interned_strings_end);
//		CG(interned_strings_start) = ZCSG(interned_strings_start);
//		CG(interned_strings_end) = ZCSG(interned_strings_end);
	}
# endif

	orig_new_interned_string = zend_new_interned_string;
	orig_interned_strings_snapshot = zend_interned_strings_snapshot;
	orig_interned_strings_restore = zend_interned_strings_restore;
	zend_new_interned_string = accel_new_interned_string_for_php;
	zend_interned_strings_snapshot = accel_interned_strings_snapshot_for_php;
	zend_interned_strings_restore = accel_interned_strings_restore_for_php;

# ifndef ZTS
	if (ZCG(accel_directives).interned_strings_buffer) {
		accel_use_shm_interned_strings();
		accel_interned_strings_save_state();
	}
# endif

	zend_reset_cache_vars();

	ZCSG(oom_restarts) = 0;
	ZCSG(hash_restarts) = 0;
	ZCSG(manual_restarts) = 0;

	ZCSG(accelerator_enabled) = 1;
	ZCSG(start_time) = zend_accel_get_time();
	ZCSG(last_restart_time) = 0;
	ZCSG(restart_in_progress) = 0;

	zend_shared_alloc_unlock();

	return SUCCESS;
}

static void accel_globals_ctor(zend_accel_globals *accel_globals)
{
#if defined(COMPILE_DL_OPCACHE) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	memset(accel_globals, 0, sizeof(zend_accel_globals));

	/* TODO refactor to init this just once. */
	accel_gen_system_id();
}

static void accel_globals_internal_func_dtor(zval *zv)
{
	free(Z_PTR_P(zv));
}

static void accel_globals_dtor(zend_accel_globals *accel_globals)
{
	if (accel_globals->function_table.nTableSize) {
		accel_globals->function_table.pDestructor = accel_globals_internal_func_dtor;
		zend_hash_destroy(&accel_globals->function_table);
	}
}

#define ZEND_BIN_ID "BIN_" ZEND_TOSTR(SIZEOF_CHAR) ZEND_TOSTR(SIZEOF_INT) ZEND_TOSTR(SIZEOF_LONG) ZEND_TOSTR(SIZEOF_SIZE_T) ZEND_TOSTR(SIZEOF_ZEND_LONG) ZEND_TOSTR(ZEND_MM_ALIGNMENT)

static void accel_gen_system_id(void)
{
	PHP_MD5_CTX context;
	unsigned char digest[16], c;
	char *md5str = ZCG(system_id);
	int i;

	PHP_MD5Init(&context);
	PHP_MD5Update(&context, PHP_VERSION, sizeof(PHP_VERSION)-1);
	PHP_MD5Update(&context, ZEND_EXTENSION_BUILD_ID, sizeof(ZEND_EXTENSION_BUILD_ID)-1);
	PHP_MD5Update(&context, ZEND_BIN_ID, sizeof(ZEND_BIN_ID)-1);
	if (strstr(PHP_VERSION, "-dev") != 0) {
		/* Development versions may be changed from build to build */
		PHP_MD5Update(&context, __DATE__, sizeof(__DATE__)-1);
		PHP_MD5Update(&context, __TIME__, sizeof(__TIME__)-1);
	}
	PHP_MD5Final(digest, &context);
	for (i = 0; i < 16; i++) {
		c = digest[i] >> 4;
		c = (c <= 9) ? c + '0' : c - 10 + 'a';
		md5str[i * 2] = c;
		c = digest[i] &  0x0f;
		c = (c <= 9) ? c + '0' : c - 10 + 'a';
		md5str[(i * 2) + 1] = c;
	}
}

#ifdef HAVE_HUGE_CODE_PAGES
# ifndef _WIN32
#  include <sys/mman.h>
#  ifndef MAP_ANON
#   ifdef MAP_ANONYMOUS
#    define MAP_ANON MAP_ANONYMOUS
#   endif
#  endif
#  ifndef MAP_FAILED
#   define MAP_FAILED ((void*)-1)
#  endif
# endif

# if defined(MAP_HUGETLB) || defined(MADV_HUGEPAGE)
static int accel_remap_huge_pages(void *start, size_t size, const char *name, size_t offset)
{
	void *ret = MAP_FAILED;
	void *mem;

	mem = mmap(NULL, size,
		PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS,
		-1, 0);
	if (mem == MAP_FAILED) {
		zend_error(E_WARNING,
			ACCELERATOR_PRODUCT_NAME " huge_code_pages: mmap failed: %s (%d)",
			strerror(errno), errno);
		return -1;
	}
	memcpy(mem, start, size);

#  ifdef MAP_HUGETLB
	ret = mmap(start, size,
		PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_HUGETLB,
		-1, 0);
#  endif
	if (ret == MAP_FAILED) {
		ret = mmap(start, size,
			PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
			-1, 0);
		/* this should never happen? */
		ZEND_ASSERT(ret != MAP_FAILED);
#  ifdef MADV_HUGEPAGE
		if (-1 == madvise(start, size, MADV_HUGEPAGE)) {
			memcpy(start, mem, size);
			mprotect(start, size, PROT_READ | PROT_EXEC);
			munmap(mem, size);
			zend_error(E_WARNING,
				ACCELERATOR_PRODUCT_NAME " huge_code_pages: madvise(HUGEPAGE) failed: %s (%d)",
				strerror(errno), errno);
			return -1;
		}
#  else
		memcpy(start, mem, size);
		mprotect(start, size, PROT_READ | PROT_EXEC);
		munmap(mem, size);
		zend_error(E_WARNING,
			ACCELERATOR_PRODUCT_NAME " huge_code_pages: mmap(HUGETLB) failed: %s (%d)",
			strerror(errno), errno);
		return -1;
#  endif
	}

	if (ret == start) {
		memcpy(start, mem, size);
		mprotect(start, size, PROT_READ | PROT_EXEC);
	}
	munmap(mem, size);

	return (ret == start) ? 0 : -1;
}

static void accel_move_code_to_huge_pages(void)
{
	FILE *f;
	long unsigned int huge_page_size = 2 * 1024 * 1024;

	f = fopen("/proc/self/maps", "r");
	if (f) {
		long unsigned int  start, end, offset, inode;
		char perm[5], dev[6], name[MAXPATHLEN];
		int ret;

		ret = fscanf(f, "%lx-%lx %4s %lx %5s %ld %s\n", &start, &end, perm, &offset, dev, &inode, name);
		if (ret == 7 && perm[0] == 'r' && perm[1] == '-' && perm[2] == 'x' && name[0] == '/') {
			long unsigned int  seg_start = ZEND_MM_ALIGNED_SIZE_EX(start, huge_page_size);
			long unsigned int  seg_end = (end & ~(huge_page_size-1L));

			if (seg_end > seg_start) {
				zend_accel_error(ACCEL_LOG_DEBUG, "remap to huge page %lx-%lx %s \n", seg_start, seg_end, name);
				accel_remap_huge_pages((void*)seg_start, seg_end - seg_start, name, offset + seg_start - start);
			}
		}
		fclose(f);
	}
}
# else
static void accel_move_code_to_huge_pages(void)
{
	zend_error(E_WARNING, ACCELERATOR_PRODUCT_NAME ": opcache.huge_code_pages has no affect as huge page is not supported");
	return;
}
# endif /* defined(MAP_HUGETLB) || defined(MADV_HUGEPAGE) */
#endif /* HAVE_HUGE_CODE_PAGES */

static int accel_startup(zend_extension *extension)
{
	zend_function *func;
	zend_ini_entry *ini_entry;

#ifdef ZTS
	accel_globals_id = ts_allocate_id(&accel_globals_id, sizeof(zend_accel_globals), (ts_allocate_ctor) accel_globals_ctor, (ts_allocate_dtor) accel_globals_dtor);
#else
	accel_globals_ctor(&accel_globals);
#endif

#ifdef ZEND_WIN32
	_setmaxstdio(2048); /* The default configuration is limited to 512 stdio files */
#endif

	if (start_accel_module() == FAILURE) {
		accel_startup_ok = 0;
		zend_error(E_WARNING, ACCELERATOR_PRODUCT_NAME ": module registration failed!");
		return FAILURE;
	}

	accel_gen_system_id();

#ifdef HAVE_HUGE_CODE_PAGES
	if (ZCG(accel_directives).huge_code_pages &&
	    (strcmp(sapi_module.name, "cli") == 0 ||
	     strcmp(sapi_module.name, "cli-server") == 0 ||
		 strcmp(sapi_module.name, "cgi-fcgi") == 0 ||
		 strcmp(sapi_module.name, "fpm-fcgi") == 0)) {
		accel_move_code_to_huge_pages();
	}
#endif

	/* no supported SAPI found - disable acceleration and stop initialization */
	if (accel_find_sapi() == FAILURE) {
		accel_startup_ok = 0;
		if (!ZCG(accel_directives).enable_cli &&
		    strcmp(sapi_module.name, "cli") == 0) {
			zps_startup_failure("Opcode Caching is disabled for CLI", NULL, accelerator_remove_cb);
		} else {
			zps_startup_failure("Opcode Caching is only supported in Apache, ISAPI, FPM, FastCGI and LiteSpeed SAPIs", NULL, accelerator_remove_cb);
		}
		return SUCCESS;
	}

	if (ZCG(enabled) == 0) {
		return SUCCESS ;
	}

/********************************************/
/* End of non-SHM dependent initializations */
/********************************************/
#ifdef HAVE_OPCACHE_FILE_CACHE
	if (!ZCG(accel_directives).file_cache_only) {
#else
	if (1) {
#endif
		switch (zend_shared_alloc_startup(ZCG(accel_directives).memory_consumption)) {
			case ALLOC_SUCCESS:
				if (zend_accel_init_shm() == FAILURE) {
					accel_startup_ok = 0;
					return FAILURE;
				}
				break;
			case ALLOC_FAILURE:
				accel_startup_ok = 0;
				zend_accel_error(ACCEL_LOG_FATAL, "Failure to initialize shared memory structures - probably not enough shared memory.");
				return SUCCESS;
			case SUCCESSFULLY_REATTACHED:
				zend_shared_alloc_lock();
				accel_shared_globals = (zend_accel_shared_globals *) ZSMMG(app_shared_globals);
				orig_new_interned_string = zend_new_interned_string;
				orig_interned_strings_snapshot = zend_interned_strings_snapshot;
				orig_interned_strings_restore = zend_interned_strings_restore;

				zend_new_interned_string = accel_new_interned_string_for_php;
				zend_interned_strings_snapshot = accel_interned_strings_snapshot_for_php;
				zend_interned_strings_restore = accel_interned_strings_restore_for_php;
#ifndef ZTS
				accel_use_shm_interned_strings();
#endif
				zend_shared_alloc_unlock();
				break;
			case FAILED_REATTACHED:
				accel_startup_ok = 0;
				zend_accel_error(ACCEL_LOG_FATAL, "Failure to initialize shared memory structures - can not reattach to exiting shared memory.");
				return SUCCESS;
				break;
#if ENABLE_FILE_CACHE_FALLBACK
			case ALLOC_FALLBACK:
				zend_shared_alloc_lock();
				fallback_process = 1;
				zend_accel_init_auto_globals();
				zend_shared_alloc_unlock();
				goto file_cache_fallback;
				break;
#endif
		}

		/* from this point further, shared memory is supposed to be OK */

		/* remeber the last restart time in the process memory */
		ZCG(last_restart_time) = ZCSG(last_restart_time);

		/* Init auto-global strings */
		zend_accel_init_auto_globals();

		zend_shared_alloc_lock();
		zend_shared_alloc_save_state();
		zend_shared_alloc_unlock();

		SHM_PROTECT();
#ifdef HAVE_OPCACHE_FILE_CACHE
	} else if (!ZCG(accel_directives).file_cache) {
		accel_startup_ok = 0;
		zend_accel_error(ACCEL_LOG_FATAL, "opcache.file_cache_only is set without a proper setting of opcache.file_cache");
		return SUCCESS;
	} else {
		accel_shared_globals = calloc(1, sizeof(zend_accel_shared_globals));

		/* Init auto-global strings */
		zend_accel_init_auto_globals();
#endif
	}
#if ENABLE_FILE_CACHE_FALLBACK
file_cache_fallback:
#endif

	/* Override compiler */
	accelerator_orig_compile_file = zend_compile_file;
	zend_compile_file = persistent_compile_file;

	/* Override stream opener function (to eliminate open() call caused by
	 * include/require statements ) */
	accelerator_orig_zend_stream_open_function = zend_stream_open_function;
	zend_stream_open_function = persistent_stream_open_function;

	/* Override path resolver function (to eliminate stat() calls caused by
	 * include_once/require_once statements */
	accelerator_orig_zend_resolve_path = zend_resolve_path;
	zend_resolve_path = persistent_zend_resolve_path;

	/* Override chdir() function */
	if ((func = zend_hash_str_find_ptr(CG(function_table), "chdir", sizeof("chdir")-1)) != NULL &&
	    func->type == ZEND_INTERNAL_FUNCTION) {
		orig_chdir = func->internal_function.handler;
		func->internal_function.handler = ZEND_FN(accel_chdir);
	}
	ZCG(cwd) = NULL;
	ZCG(include_path) = NULL;

	/* Override "include_path" modifier callback */
	if ((ini_entry = zend_hash_str_find_ptr(EG(ini_directives), "include_path", sizeof("include_path")-1)) != NULL) {
		ZCG(include_path) = ini_entry->value;
		orig_include_path_on_modify = ini_entry->on_modify;
		ini_entry->on_modify = accel_include_path_on_modify;
	}

	accel_startup_ok = 1;

	/* Override file_exists(), is_file() and is_readable() */
	zend_accel_override_file_functions();

	/* Load black list */
	accel_blacklist.entries = NULL;
	if (ZCG(enabled) && accel_startup_ok &&
	    ZCG(accel_directives).user_blacklist_filename &&
	    *ZCG(accel_directives.user_blacklist_filename)) {
		zend_accel_blacklist_init(&accel_blacklist);
		zend_accel_blacklist_load(&accel_blacklist, ZCG(accel_directives.user_blacklist_filename));
	}

	return SUCCESS;
}

static void accel_free_ts_resources()
{
#ifndef ZTS
	accel_globals_dtor(&accel_globals);
#else
	ts_free_id(accel_globals_id);
#endif
}

void accel_shutdown(void)
{
	zend_ini_entry *ini_entry;
	zend_bool file_cache_only = 0;

	zend_accel_blacklist_shutdown(&accel_blacklist);

	if (!ZCG(enabled) || !accel_startup_ok) {
		accel_free_ts_resources();
		return;
	}

	if (ZCG(accel_directives).interned_strings_buffer) {
#ifndef ZTS
		zend_hash_clean(CG(auto_globals));
		zend_hash_clean(CG(function_table));
		zend_hash_clean(CG(class_table));
		zend_hash_clean(EG(zend_constants));
#endif
	}

	accel_reset_pcre_cache();

	zend_new_interned_string = orig_new_interned_string;
	zend_interned_strings_snapshot = orig_interned_strings_snapshot;
	zend_interned_strings_restore = orig_interned_strings_restore;

#ifdef HAVE_OPCACHE_FILE_CACHE
	file_cache_only = ZCG(accel_directives).file_cache_only;
#endif

	accel_free_ts_resources();

	if (!file_cache_only) {
		zend_shared_alloc_shutdown();
	}
	zend_compile_file = accelerator_orig_compile_file;

	if ((ini_entry = zend_hash_str_find_ptr(EG(ini_directives), "include_path", sizeof("include_path")-1)) != NULL) {
		ini_entry->on_modify = orig_include_path_on_modify;
	}
}

void zend_accel_schedule_restart(zend_accel_restart_reason reason)
{
	const char *zend_accel_restart_reason_text[ACCEL_RESTART_USER + 1] = {
		"out of memory",
		"hash overflow",
		"user",
	};

	if (ZCSG(restart_pending)) {
		/* don't schedule twice */
		return;
	}
	zend_accel_error(ACCEL_LOG_DEBUG, "Restart Scheduled! Reason: %s",
			zend_accel_restart_reason_text[reason]);

	SHM_UNPROTECT();
	ZCSG(restart_pending) = 1;
	ZCSG(restart_reason) = reason;
	ZCSG(cache_status_before_restart) = ZCSG(accelerator_enabled);
	ZCSG(accelerator_enabled) = 0;

	if (ZCG(accel_directives).force_restart_timeout) {
		ZCSG(force_restart_time) = zend_accel_get_time() + ZCG(accel_directives).force_restart_timeout;
	} else {
		ZCSG(force_restart_time) = 0;
	}
	SHM_PROTECT();
}

/* this is needed because on WIN32 lock is not decreased unless ZCG(counted) is set */
#ifdef ZEND_WIN32
#define accel_deactivate_now() ZCG(counted) = 1; accel_deactivate_sub()
#else
#define accel_deactivate_now() accel_deactivate_sub()
#endif

/* ensures it is OK to read SHM
	if it's not OK (restart in progress) returns FAILURE
	if OK returns SUCCESS
	MUST call accelerator_shm_read_unlock after done lock operations
*/
int accelerator_shm_read_lock(void)
{
	if (ZCG(counted)) {
		/* counted means we are holding read lock for SHM, so that nothing bad can happen */
		return SUCCESS;
	} else {
		/* here accelerator is active but we do not hold SHM lock. This means restart was scheduled
			or is in progress now */
		if (accel_activate_add() == FAILURE) { /* acquire usage lock */
			return FAILURE;
		}
		/* Now if we weren't inside restart, restart would not begin until we remove usage lock */
		if (ZCSG(restart_in_progress)) {
			/* we already were inside restart this means it's not safe to touch shm */
			accel_deactivate_now(); /* drop usage lock */
			return FAILURE;
		}
		ZCG(counted) = 1;
	}
	return SUCCESS;
}

/* must be called ONLY after SUCCESSFUL accelerator_shm_read_lock */
void accelerator_shm_read_unlock(void)
{
	if (!ZCG(counted)) {
		/* counted is 0 - meaning we had to readlock manually, release readlock now */
		accel_deactivate_now();
	}
}

ZEND_EXT_API zend_extension zend_extension_entry = {
	ACCELERATOR_PRODUCT_NAME,               /* name */
	PHP_VERSION,							/* version */
	"Zend Technologies",					/* author */
	"http://www.zend.com/",					/* URL */
	"Copyright (c) 1999-2017",				/* copyright */
	accel_startup,					   		/* startup */
	NULL,									/* shutdown */
	accel_activate,							/* per-script activation */
	accel_deactivate,						/* per-script deactivation */
	NULL,									/* message handler */
	NULL,									/* op_array handler */
	NULL,									/* extended statement handler */
	NULL,									/* extended fcall begin handler */
	NULL,									/* extended fcall end handler */
	NULL,									/* op_array ctor */
	NULL,									/* op_array dtor */
	STANDARD_ZEND_EXTENSION_PROPERTIES
};
