do kompilacji potrzeba scr kernela

# pobrac biezacy release
fetch -o /tmp ftp://ftp.freebsd.org/pub/`uname -s`/releases/`uname -m`/`uname -r | cut -d'-' -f1,2`/src.txz

# wypakowac
doas tar -C / -xvf /tmp/src.txz
