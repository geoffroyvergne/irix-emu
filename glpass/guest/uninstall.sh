#! /sbin/sh
#
# uninstall.sh: removes the system-wide glpass that install.sh installed,
# as root:
#   /usr/lib32/glpass/uninstall.sh
#
# /usr/lib32/libGL.so points at IRIX's own libGL again, the boot script and
# the session lines go, and the installed files are removed except
# libglpass_x.so: sessions started while glpass was active preload it, and
# their programs would not start without it. Remove it after logging out:
#   rm -r /usr/lib32/glpass
# The session files keep their copies from before the install
# (<file>.pre-glpass); only the glpass lines are taken out of them.

DIR=/usr/lib32/glpass

# Nothing preloaded into the commands below (rm is an n32 program).
unset _RLDN32_LIST LD_LIBRARYN32_PATH

case "`id`" in
"uid=0("*) ;;
*) echo "uninstall.sh: run it as root" >&2; exit 1 ;;
esac

if [ -x /etc/init.d/glpass ]; then
    /etc/init.d/glpass stop
elif [ -r $DIR/glpass.init ]; then
    sh $DIR/glpass.init stop
fi
target=`ls -l /usr/lib32/libGL.so 2>/dev/null | sed -n 's/.* -> //p'`
case "$target" in
glpass/*) echo "uninstall.sh: /usr/lib32/libGL.so still points to glpass: nothing removed" >&2; exit 1 ;;
esac
echo "uninstall.sh: /usr/lib32/libGL.so -> $target (IRIX's own)"

rm -f /etc/rc2.d/S13glpass /etc/rc0.d/K10glpass /etc/init.d/glpass

# The lines between "glpass: begin" and "glpass: end".
for file in /etc/profile /etc/cshrc /var/X11/xdm/Xsession; do
    [ -f $file ] || continue
    grep "glpass: begin" $file > /dev/null 2>&1 || continue
    rm -f /tmp/glpass-new.$$
    awk '/glpass: begin/ { skip = 1 } !skip { print } /glpass: end/ { skip = 0 }' $file > /tmp/glpass-new.$$ &&
        cp /tmp/glpass-new.$$ $file && echo "uninstall.sh: $file: glpass lines removed"
    rm -f /tmp/glpass-new.$$
done

for f in libGL.so glpass-probe glrun glpass.init native-target active uninstall.sh; do
    rm -f $DIR/$f
done
echo "uninstall.sh: done. $DIR/libglpass_x.so stays for the sessions started with it: remove it after logging out (rm -r $DIR)."
