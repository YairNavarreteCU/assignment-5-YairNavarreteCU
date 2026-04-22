#!/bin/sh

DAEMON=/usr/bin/aesdsocket
DAEMON_ARGS="-d"
NAME=aesdsocket
DESC="AESD Socket Server"

case "$1" in
    start)
        echo "Starting $DESC"
        start-stop-daemon -S -n $NAME -a $DAEMON -- $DAEMON_ARGS
        ;;
    stop)
        echo "Stopping $DESC"
        start-stop-daemon -K -n $NAME -s TERM
        ;;
    restart)
        $0 stop
        sleep 1
        $0 start
        ;;
    *)
        echo "Usage: $0 {start|stop|restart}"
        exit 1
        ;;
esac

exit 0