# [![tests](https://github.com/MapleHe/tmon/actions/workflows/main.yml/badge.svg)](https://github.com/MapleHe/tmon/actions/workflows/main.yml) tmon

tMon - A distributed resource monitor

### **!!! This updated version of tMon hasn't been test in production env, but only my local env. !!!**

## Installation

To install on UBUNTU:

```bash
git clone https://github.com/MapleHe/tmon.git
cd tmon; make
make install
```

If you are using redhat with no init.d
please do

```bash
make install-redhat
```

This will first compile both the daemon and client, then copy the daemon, 
tmond, to /usr/sbin and the client, tmon, to /usr/bin. Note that only root 
should be allowed to install the binaries.

## Configure `tmonrc`

Edit and copy the tmonrc file to your home directory as ~/.tmonrc which is the
default filename. Alternative filenames may be specified on the command line. 


## Usage

Make sure the port 7777 (the default port) are available and accessible.

### Server service

It using both the /etc/init.d/tmond and /lib/systemd/system/tmond.service

For systemd do

```bash
systemctl enable tmond.service
```

### Client

```bash
tmon
```

## Description

This code was forked from an ubuntu package by thorfinn thorfinn@binf.ku.dk back in 2007-2009.
Which has been maintained [here](https://github.com/ANGSD/tmon) by the author of ANGSD.

I forked the [main branch](https://github.com/ANGSD/tmon/tree/master) and update the code to show the name of users with top CPU/MEM usage.
This doesn't mean to blame anyone. Sometimes it can be helpful to know who is running the heavy
tasks (including myself), so that we can rescue the dead server with more efficiency.


## Changelog

See changelog for details of changes

