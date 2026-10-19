# Built inside IRIX with the freeware gcc (MIPSpro cc needs a licence).
#   make  -> libGL.so (the pass-through library), libglpass_x.so (colormap
#            notes for colour-index programs, preloaded), and the tests gltri
#            glthreads/glthreads_pt (two GL threads, sproc and pthreads) and
#            glmove (the image follows a moved or unmapped window) and glcheck
#            (selection, feedback and 3D textures, checked value by value) and
#            glgone (a context left current on a destroyed window) and glcolors
#            (libglpass_x with freed colormaps, closed displays and threads) and
#            glshare (one context current in one thread at most) and glmasks
#            (colour index masks taking several host passes, pixel by pixel) and
#            glstubs (unsupported entry points logged once, NULL arrays ignored)
# glpass_gen.c and glpass_fn.h come from glpass/tools/glpass-gen.py on the host.
CC      = /usr/freeware/bin/gcc
CFLAGS  = -mabi=n32 -O2 -fPIC -Wall
HEADERS = glpass_protocol.h glpass_fn.h glpass_internal.h glpass_xstore.h
OBJS    = glpass.o glpass_gen.o

all: libGL.so libglpass_x.so gltri glthreads glthreads_pt glmove glcheck glgone glcolors glshare glmasks glstubs

glpass.o: glpass.c $(HEADERS)
	$(CC) $(CFLAGS) -c glpass.c

glpass_gen.o: glpass_gen.c $(HEADERS)
	$(CC) $(CFLAGS) -c glpass_gen.c

xstore.o: xstore.c glpass_xstore.h
	$(CC) $(CFLAGS) -c xstore.c

libGL.so: $(OBJS)
	$(CC) -mabi=n32 -shared -Wl,-soname,libGL.so -o libGL.so $(OBJS) -lX11

# Only libc: every process started with _RLDN32_LIST loads it.
libglpass_x.so: xstore.o
	$(CC) -mabi=n32 -shared -Wl,-soname,libglpass_x.so -o libglpass_x.so xstore.o

gltri: gltri.c libGL.so
	$(CC) -mabi=n32 -O2 -o gltri gltri.c -L. -lGL -lX11

glthreads: glthreads.c libGL.so
	$(CC) -mabi=n32 -O2 -o glthreads glthreads.c -L. -lGL -lX11

glthreads_pt: glthreads.c libGL.so
	$(CC) -mabi=n32 -O2 -DGLTHREADS_PTHREAD -o glthreads_pt glthreads.c -L. -lGL -lX11 -lpthread

glmove: glmove.c libGL.so
	$(CC) -mabi=n32 -O2 -o glmove glmove.c -L. -lGL -lX11

glcheck: glcheck.c libGL.so
	$(CC) -mabi=n32 -O2 -o glcheck glcheck.c -L. -lGL -lX11

glgone: glgone.c libGL.so
	$(CC) -mabi=n32 -O2 -o glgone glgone.c -L. -lGL -lX11

glcolors: glcolors.c
	$(CC) -mabi=n32 -O2 -o glcolors glcolors.c -lX11

glshare: glshare.c libGL.so
	$(CC) -mabi=n32 -O2 -o glshare glshare.c -L. -lGL -lX11

glmasks: glmasks.c libGL.so
	$(CC) -mabi=n32 -O2 -o glmasks glmasks.c -L. -lGL -lX11

glstubs: glstubs.c libGL.so
	$(CC) -mabi=n32 -O2 -o glstubs glstubs.c -L. -lGL -lX11

clean:
	rm -f *.o libGL.so libglpass_x.so gltri glthreads glthreads_pt glmove glcheck glgone glcolors glshare glmasks glstubs
