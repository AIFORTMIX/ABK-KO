#!/system/bin/sh



for i in $(seq 1 20); do
    [ -e /dev/ntsync ] || { sleep 1; continue; }

    LABEL=$(ls -Z /dev/ntsync 2>/dev/null | awk '{print $1}')
    if [ "$LABEL" != "u:object_r:ntsync_device:s0" ]; then
        chmod 0666 /dev/ntsync
        chcon u:object_r:ntsync_device:s0 /dev/ntsync
        echo "[ntsync-perm] service.sh: relabeled /dev/ntsync" >> /dev/kmsg
    fi
    break
done
