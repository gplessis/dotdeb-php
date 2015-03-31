# This is a generic Dockerfile to ease Debian packaging
#
# Requirements:
#   * Docker >= 1.5
# Usage:
#   # docker build -t build-wheezy-amd64-php56 -f debian/Dockerfile .
#   # docker run build-wheezy-amd64-php56
#   # ID=$(docker ps -l -q)
#   # docker cp $ID:/usr/src ~/Downloads/
#   # docker rm $ID

FROM debian:wheezy
MAINTAINER Guillaume Plessis "gui@dotdeb.org"

ENV DEBIAN_FRONTEND noninteractive

RUN echo "deb http://http.debian.net/debian/ wheezy main contrib non-free" > /etc/apt/sources.list
RUN echo "deb http://http.debian.net/debian/ wheezy-updates main contrib non-free" >> /etc/apt/sources.list
RUN echo "deb http://http.debian.net/debian/ wheezy-backports main contrib non-free" >> /etc/apt/sources.list
RUN echo "deb http://security.debian.org/ wheezy/updates main contrib non-free" >> /etc/apt/sources.list

RUN echo "APT::Install-Recommends "false";" > /etc/apt/apt.conf.d/10recommends
RUN echo "APT::Install-Suggests "false";" >> /etc/apt/apt.conf.d/10recommends

RUN apt-get update
RUN apt-get dist-upgrade -y

RUN apt-get install -y build-essential devscripts equivs libwww-perl

ADD debian/control /root/
RUN mk-build-deps --install --tool 'apt-get -y' --remove /root/control && rm -f /root/control

ADD . /usr/src/builddir
WORKDIR /usr/src/builddir

RUN rm -f debian/Dockerfile
RUN uscan --download-current-version
RUN dpkg-buildpackage

RUN apt-get clean && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/* /usr/src/builddir
