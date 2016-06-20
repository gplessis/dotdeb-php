ext_PACKAGES      += mysql
mysql_DESCRIPTION := MySQL
mysql_EXTENSIONS  := mysqlnd mysqli pdo_mysql
mysqlnd_config    := --enable-mysqlnd=shared --enable-mysqlnd-compression-support --with-zlib-dir=/usr
mysqli_config     := --with-mysqli=shared,mysqlnd
pdo_mysql_config  := --with-pdo-mysql=shared,mysqlnd
mysqlnd_PRIORITY  := 10
export mysqlnd_PRIORITY
export mysql_EXTENSIONS
export mysql_DESCRIPTION
