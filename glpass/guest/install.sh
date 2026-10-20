#! /sbin/sh
#
# install.sh: installs glpass system-wide in IRIX, so that every n32 OpenGL
# program uses the host GPU without glrun. As root, from the unpacked glpass
# directory (see glpass/README.md):
#   cd /usr/local/irix-emu/glpass && ./install.sh
#
#   1. builds libGL.so, libglpass_x.so and glpass-probe (make);
#   2. copies them, with glrun, glpass.init and uninstall.sh, to /usr/lib32/glpass;
#   3. keeps IRIX's own libGL: /usr/lib32/libGL.so is a symbolic link (to
#      /var/arch/lib32/libGL.so); its target is recorded in
#      /usr/lib32/glpass/native-target, and no IRIX file is renamed;
#   4. installs /etc/init.d/glpass (S13 at boot, K10 at shutdown), which
#      points the link at glpass only when the emulator's GL bridge answers;
#   5. adds the libglpass_x preload to new sessions in /etc/profile,
#      /etc/cshrc and /var/X11/xdm/Xsession, between "glpass: begin" and
#      "glpass: end" lines, each file saved first as <file>.pre-glpass;
#   6. switches now.
#
# Run it again after updating the emulator: it rebuilds and reinstalls.
# /usr/lib32/glpass/uninstall.sh undoes it.

SRC=`dirname $0`
SRC=`cd $SRC && pwd`
DIR=/usr/lib32/glpass
LINK=/usr/lib32/libGL.so
MARK="glpass: begin (added by glpass's install.sh; /usr/lib32/glpass/uninstall.sh removes it)"

# Nothing preloaded into the commands below (rm is an n32 program).
unset _RLDN32_LIST LD_LIBRARYN32_PATH

die() {
    echo "install.sh: $*" >&2
    exit 1
}

case "`uname -s`" in
IRIX*) ;;
*) die "this installs glpass in IRIX" ;;
esac
case "`id`" in
"uid=0("*) ;;
*) die "run it as root" ;;
esac

# ---- IRIX's own libGL ----------------------------------------------------------
target=`ls -l $LINK 2>/dev/null | sed -n 's/.* -> //p'`
case "$target" in
"")
    die "$LINK is not a symbolic link: this layout is not the one install.sh knows, nothing changed" ;;
glpass/*)
    [ -r $DIR/native-target ] || die "$LINK points to glpass, but $DIR/native-target is missing"
    native=`cat $DIR/native-target` ;;
*)
    native=$target ;;
esac
( cd /usr/lib32 && [ -f "$native" ] ) || die "IRIX's libGL ($native, from /usr/lib32) is missing, nothing changed"
case "$native" in
../../var/arch/lib32/libGL.so) ;;
*) echo "install.sh: IRIX's libGL is $native, not /var/arch/lib32/libGL.so: set GLPASS_NATIVE_LIBGL to it for the fallback" ;;
esac

# ---- Build -----------------------------------------------------------------------
echo "install.sh: building in $SRC"
( cd $SRC && make libGL.so libglpass_x.so glpass-probe ) || die "the build failed, nothing installed"

# ---- Files -----------------------------------------------------------------------
mkdir -p $DIR || die "cannot create $DIR"
for f in libGL.so libglpass_x.so glpass-probe glrun glpass.init uninstall.sh; do
    rm -f $DIR/$f.new
    cp $SRC/$f $DIR/$f.new || die "cannot copy $f"
    mv -f $DIR/$f.new $DIR/$f   # a running program keeps its mapped copy
done
chmod 755 $DIR $DIR/libGL.so $DIR/libglpass_x.so $DIR/glpass-probe $DIR/glrun $DIR/uninstall.sh
chmod 644 $DIR/glpass.init
echo "$native" > $DIR/native-target
chmod 644 $DIR/native-target

# ---- Boot and shutdown --------------------------------------------------------
cp $DIR/glpass.init /etc/init.d/glpass && chmod 755 /etc/init.d/glpass || die "cannot install /etc/init.d/glpass"
rm -f /etc/rc2.d/S13glpass /etc/rc0.d/K10glpass
ln -s ../init.d/glpass /etc/rc2.d/S13glpass
ln -s ../init.d/glpass /etc/rc0.d/K10glpass

# ---- Sessions: preload libglpass_x while glpass is active ------------------
sh_block() {
    cat << 'EOF'
if [ -f /usr/lib32/glpass/active -a -r /usr/lib32/glpass/libglpass_x.so ]; then
    case "$_RLDN32_LIST" in
    *libglpass_x.so*) ;;
    "") _RLDN32_LIST=/usr/lib32/glpass/libglpass_x.so:DEFAULT; export _RLDN32_LIST ;;
    *) _RLDN32_LIST=/usr/lib32/glpass/libglpass_x.so:$_RLDN32_LIST; export _RLDN32_LIST ;;
    esac
fi
EOF
}

csh_block() {
    cat << 'EOF'
if ( -f /usr/lib32/glpass/active && -r /usr/lib32/glpass/libglpass_x.so ) then
    if ( ! $?_RLDN32_LIST ) setenv _RLDN32_LIST DEFAULT
    if ( "$_RLDN32_LIST" !~ *libglpass_x.so* ) setenv _RLDN32_LIST /usr/lib32/glpass/libglpass_x.so:$_RLDN32_LIST
endif
EOF
}

# add <file> <sh|csh> <top|end>: the block, replacing one an earlier install
# added; the file saved first (once, before the first install).
add() {
    file=$1
    [ -f $file ] || { echo "install.sh: no $file, skipped"; return 0; }
    [ -f $file.pre-glpass ] || cp -p $file $file.pre-glpass || die "cannot save $file"
    rm -f /tmp/glpass-block.$$ /tmp/glpass-new.$$ /tmp/glpass-old.$$
    if grep "glpass: begin" $file > /dev/null 2>&1; then
        awk '/glpass: begin/ { skip = 1 } !skip { print } /glpass: end/ { skip = 0 }' $file > /tmp/glpass-old.$$ &&
            cp /tmp/glpass-old.$$ $file || die "cannot update $file"
        rm -f /tmp/glpass-old.$$
    fi
    { echo "# $MARK"; ${2}_block; echo "# glpass: end"; } > /tmp/glpass-block.$$
    if [ "$3" = top ]; then  # after the first line (#!/bin/sh)
        { sed -n '1p' $file; cat /tmp/glpass-block.$$; sed '1d' $file; } > /tmp/glpass-new.$$
    else
        { awk '{ print }' $file; cat /tmp/glpass-block.$$; } > /tmp/glpass-new.$$  # awk: a last line gets its newline
    fi
    cp /tmp/glpass-new.$$ $file || die "cannot update $file (saved as $file.pre-glpass)"
    rm -f /tmp/glpass-block.$$ /tmp/glpass-new.$$
    echo "install.sh: $file: libglpass_x preload added"
}

add /etc/profile sh end
add /etc/cshrc csh end
add /var/X11/xdm/Xsession sh top  # before it hands over to Xsession.dt

# ---- Now ---------------------------------------------------------------------------
/etc/init.d/glpass start
echo "install.sh: done. IRIX's own libGL stays in place ($native)."
echo "install.sh: log out and in again (or restart) for desktop programs to preload libglpass_x."
