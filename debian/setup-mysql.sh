#!/bin/sh

set -eu

[ $# -ge 2 ] || {
    echo "Usage: debian/setup-mysql.sh port data-dir" >&2
    exit 1
}

# CLI arguments #
port=$1
datadir=$2
action=${3:-start}

# Some vars #

socket=$datadir/mysql.sock
# Commands:
mysqladmin="mysqladmin -u root -P $port -h localhost --socket=$socket"
mysqld="/usr/sbin/mysqld --no-defaults --bind-address=localhost --port=$port --socket=$socket --datadir=$datadir --user=$USER"

# Main code #

if [ "$action" = "stop" ]; then
    $mysqladmin shutdown
    exit
fi

rm -rf $datadir
mkdir -p $datadir
chmod go-rx $datadir

mysql_install_db --datadir=$datadir --rpm --force >> $datadir/bootstrap.log 2>&1

tmpf=$(mktemp)
cat > "$tmpf" <<EOF
USE mysql;
UPDATE user SET password=PASSWORD('') WHERE user='root';
FLUSH PRIVILEGES;
EOF

$mysqld --bootstrap --skip-grant-tables < "$tmpf" >> $datadir/bootstrap.log 2>&1

unlink "$tmpf"

# Start the daemon
$mysqld > $datadir/run.log 2>&1 &

pid=$!

# wait for the server to be actually available
c=0;
while ! nc -z localhost $port; do
    c=$(($c+1));
    sleep 3;
    if [ $c -gt 20 ]; then
	echo "Timed out waiting for mysql server to be available" >&2
	if [ "$pid" ]; then
	    kill $pid || :
	    sleep 2
	    kill -s KILL $pid || :
	fi
	exit 1
    fi
done

$mysqladmin create test
