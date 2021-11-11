#!/usr/bin/env bash

# Copyright Notice:
# © (C) Postgres Professional 2015-2016 http://www.postgrespro.ru/
# Distributed under Apache License 2.0
# Распространяется по лицензии Apache 2.0

set -xe
set -o pipefail

ulimit -n 1024

if [ ${PBK_EDITION} == 'ent' ]; then
    exit 0
fi

apt-get clean -y
apt-get update -y
apt-get install nginx su -y

adduser nginx

cat <<EOF > /etc/nginx/nginx.conf
user nginx;
worker_processes  1;
error_log  /var/log/nginx/error.log;
events {
    worker_connections  1024;
}
http {
    server {
        listen   80 default;
        root /app/www;
    }
}
EOF

/etc/init.d/nginx start

# install POSTGRESQL

export PGDATA=/var/lib/pgsql/${PG_VERSION}/data

# install old packages

# install new packages
echo "127.0.0.1 repo.postgrespro.ru" >> /etc/hosts
echo "rpm http://repo.postgrespro.ru/pg_probackup-forks/rpm/latest/altlinux-p${DISTRIB_VERSION} x86_64 forks" > /etc/apt/sources.list.d/pg_probackup.list
echo "rpm [p${DISTRIB_VERSION}] http://mirror.yandex.ru/altlinux p${DISTRIB_VERSION}/branch/x86_64 debuginfo" > /etc/apt/sources.list.d/debug.list

apt-get update -y
apt-get install ${PKG_NAME} ${PKG_NAME}-debuginfo -y
${PKG_NAME} --help
${PKG_NAME} --version

exit 0

su postgres -c "/usr/pgsql-${PG_VERSION}/bin/pgbench --no-vacuum -t 1000 -c 1"
su postgres -c "${PKG_NAME} backup --instance=node -b page -B /tmp/backup -D ${PGDATA}"
su postgres -c "${PKG_NAME} show --instance=node -B /tmp/backup -D ${PGDATA}"

su postgres -c "/usr/pgsql-${PG_VERSION}/bin/pg_ctl stop -D ${PGDATA}"
rm -rf ${PGDATA}

su postgres -c "${PKG_NAME} restore --instance=node -B /tmp/backup -D ${PGDATA}"
su postgres -c "/usr/pgsql-${PG_VERSION}/bin/pg_ctl start -w -D ${PGDATA}"

sleep 5

echo "select count(*) from pgbench_accounts;" | su postgres -c "/usr/pgsql-${PG_VERSION}/bin/psql" || exit 1

exit 0 # while PG12 is not working

# SRC PACKAGE
cd /mnt
