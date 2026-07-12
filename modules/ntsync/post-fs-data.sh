#!/system/bin/sh

MODDIR="${0%/*}"


for i in $(seq 1 10); do
    [ -e /dev/ntsync ] && break
    sleep 0.5
done

if [ ! -e /dev/ntsync ]; then
    echo "[ntsync-perm] /dev/ntsync not found, aborting" >> /dev/kmsg
    exit 1
fi


chmod 0666 /dev/ntsync


chcon u:object_r:ntsync_device:s0 /dev/ntsync

echo "[ntsync-perm] /dev/ntsync configured: $(ls -lZ /dev/ntsync)" >> /dev/kmsg
