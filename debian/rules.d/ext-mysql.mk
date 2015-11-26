ext_PACKAGES      += mysql
mysql_DESCRIPTION := MySQL
mysql_EXTENSIONS  := mysqli pdo_mysql
mysqli_config     := --with-mysqli=shared,mysqlnd
pdo_mysql_config  := --with-pdo-mysql=shared,mysqlnd
export mysql_EXTENSIONS
export mysql_DESCRIPTION
