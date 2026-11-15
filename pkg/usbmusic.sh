#!/bin/sh
# usbmusic.sh start|stop|status
# Exposes ONLY /mnt/SDCARD/Music to a USB host over MTP, using the stock MtpDaemon.
# The daemon's storage root is hard-coded to /mnt/UDISK, so a second copy is started in
# a private mount namespace where /mnt/UDISK is a bind mount of the Music folder.
MUSIC=/mnt/SDCARD/Music
FLAG=/tmp/vibe.usb
BUSY=/tmp/vibe.usbbusy      # app creates it; we remove it when finished
MTPD=/usr/sbin/MtpDaemon

restore_mode() {
    usbmode=`/usr/trimui/bin/systemval usbmode`
    if [ "$usbmode" = "dock" ]; then /usr/trimui/bin/usb_dock.sh
    elif [ "$usbmode" = "host" ]; then /usr/trimui/bin/usb_host.sh
    else /usr/trimui/bin/usb_device.sh; fi
}

stop_usb() {
    killall MtpDaemon 2>/dev/null
    sleep 1
    setsid $MTPD -D </dev/null >/dev/null 2>&1 &    # stock daemon back (background: it may not daemonize)
    sleep 1
    /bin/setusbconfig adb
    rm -f /tmp/stay_awake "$FLAG"
    restore_mode
}

# Windows needs the MS OS descriptor ("MSFT100" + vendor code + os_desc linked to the config) to
# recognise the MTP interface; Linux and Android don't. The gadget has to be unbound to change it.
fix_windows() {
    G=/sys/kernel/config/usb_gadget/g1
    UDC=`cat $G/UDC 2>/dev/null`
    [ -n "$UDC" ] || UDC=`ls /sys/class/udc | head -n1`
    echo "" > $G/UDC 2>/dev/null
    sleep 1
    echo 1 > $G/os_desc/use
    echo 0xcd > $G/os_desc/b_vendor_code
    echo MSFT100 > $G/os_desc/qw_sign
    [ -e $G/os_desc/c.1 ] || ln -s $G/configs/c.1 $G/os_desc/c.1
    echo "$UDC" > $G/UDC
    sleep 1
}

start_usb() {
    [ -f "$FLAG" ] && { echo "already on"; return 0; }
    [ -d "$MUSIC" ] || mkdir -p "$MUSIC"
    echo 1 > /tmp/stay_awake
    /usr/trimui/bin/usb_device.sh          # force device mode, like the stock USB app
    killall MtpDaemon 2>/dev/null
    sleep 1
    echo "starting namespaced MtpDaemon"
    unshare -m sh -c "mount --make-rprivate / 2>/dev/null; mount --bind \"$MUSIC\" /mnt/UDISK && exec $MTPD -D" </dev/null >/dev/null 2>&1 &
    sleep 2
    if ! pidof MtpDaemon >/dev/null; then
        echo "MtpDaemon did not start"; stop_usb; return 1
    fi
    # safety: the OS must still see its real /mnt/UDISK (settings live there)
    if [ ! -e /mnt/UDISK/system.json ]; then
        echo "mount leaked into the main namespace, rolling back"
        umount /mnt/UDISK 2>/dev/null
        stop_usb; return 1
    fi
    /bin/setusbconfig mtp,adb
    fix_windows
    touch "$FLAG"
    echo "USB Music mode on"
}

case "$1" in
    start)  start_usb; rm -f "$BUSY" ;;
    stop)   stop_usb; echo "USB Music mode off"; rm -f "$BUSY" ;;
    status) [ -f "$FLAG" ] && echo on || echo off ;;
    *)      echo "usage: $0 start|stop|status"; exit 1 ;;
esac
