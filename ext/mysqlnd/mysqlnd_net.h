/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2017 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Andrey Hristov <andrey@php.net>                             |
  |          Ulf Wendel <uw@php.net>                                     |
  |          Georg Richter <georg@php.net>                               |
  +----------------------------------------------------------------------+
*/

#ifndef MYSQLND_NET_H
#define MYSQLND_NET_H

PHPAPI MYSQLND_NET * mysqlnd_net_init(zend_bool persistent, MYSQLND_STATS * stats, MYSQLND_ERROR_INFO * error_info);
PHPAPI void mysqlnd_net_free(MYSQLND_NET * const net, MYSQLND_STATS * stats, MYSQLND_ERROR_INFO * error_info);

#endif /* MYSQLND_NET_H */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
