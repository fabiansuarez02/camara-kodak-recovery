/*
 * mjpgstream.c - Minimal MJPEG-over-HTTP streamer for the Kodak CFH-V10
 *
 * Reads MJPEG frames straight from the UVC sensor (AIT8431) via V4L2 and
 * serves them as multipart/x-mixed-replace over HTTP on :8080. It replaces
 * the stock cloud client entirely: no vendor server, no internet.
 *
 * Target: MediaTek MT7620A (MIPS 24Kc, little-endian), kernel 2.6.36.
 * Build:  static, musl, mipsel  (see build.sh)
 *
 * --------------------------------------------------------------------------
 * The gotcha that cost hours:
 *
 *   modern musl uses a 64-bit time_t, which changes sizeof(struct
 *   v4l2_buffer). The V4L2 ioctl numbers are encoded with _IOWR(...,
 *   sizeof(struct)), so a struct of the "wrong" size produces a DIFFERENT
 *   ioctl number and the 2.6.36 kernel rejects it with EINVAL.
 *
 *   Fix: define the buffer struct with the EXACT kernel ABI (32-bit timeval,
 *   sizeof == 68) and hand-roll the ioctl numbers with that struct. See
 *   `struct kbuf` and the K_* macros below.
 * --------------------------------------------------------------------------
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/videodev2.h>

#define DEV        "/dev/video0"
#define WIDTH      1280
#define HEIGHT     720
#define NBUF       4
#define PORT       8080
#define MAXCLIENTS 8
#define BOUNDARY   "mjpgboundary"

/* --- Kernel-ABI buffer struct (sizeof == 68), independent of musl time_t --- */
struct kbuf {
    unsigned int  index;
    int           type;
    unsigned int  bytesused;
    unsigned int  flags;
    int           field;
    int           tv_sec;          /* 32-bit timeval, as the 2.6.36 kernel expects */
    int           tv_usec;
    unsigned char timecode[16];
    unsigned int  sequence;
    int           memory;
    unsigned int  m;               /* union: offset (MMAP) */
    unsigned int  length;
    unsigned int  input;
    unsigned int  reserved;
};
#define K_QUERYBUF _IOWR('V',  9, struct kbuf)
#define K_QBUF     _IOWR('V', 15, struct kbuf)
#define K_DQBUF    _IOWR('V', 17, struct kbuf)

static int   vfd = -1;
static void *bufs[NBUF];
static unsigned int buflen[NBUF];

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static int cam_open(void) {
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    unsigned int i;
    int type;

    vfd = open(DEV, O_RDWR);
    if (vfd < 0) { perror("open " DEV); return -1; }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = WIDTH;
    fmt.fmt.pix.height      = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;
    if (xioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return -1; }

    memset(&req, 0, sizeof(req));
    req.count  = NBUF;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(vfd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return -1; }

    for (i = 0; i < NBUF; i++) {
        struct kbuf b;
        memset(&b, 0, sizeof(b));
        b.index  = i;
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(vfd, K_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return -1; }
        buflen[i] = b.length;
        bufs[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, b.m);
        if (bufs[i] == MAP_FAILED) { perror("mmap"); return -1; }
        if (xioctl(vfd, K_QBUF, &b) < 0) { perror("QBUF"); return -1; }
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(vfd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return -1; }
    return 0;
}

/* Grab one frame into *out/*len (pointer into the mmap'd buffer, valid until requeue). */
static int cam_grab(struct kbuf *b) {
    memset(b, 0, sizeof(*b));
    b->type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b->memory = V4L2_MEMORY_MMAP;
    if (xioctl(vfd, K_DQBUF, b) < 0) return -1;   /* caller keeps going on error */
    return 0;
}

static void cam_requeue(struct kbuf *b) {
    xioctl(vfd, K_QBUF, b);
}

int main(void) {
    int srv, on = 1;
    struct sockaddr_in addr;

    /* Never die on a broken client or a hangup; self-daemonize. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP,  SIG_IGN);
    if (fork() > 0) _exit(0);
    setsid();

    if (cam_open() < 0) return 1;

    srv = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    listen(srv, MAXCLIENTS);

    for (;;) {
        int cfd = accept(srv, NULL, NULL);
        char req[512], hdr[256];
        struct kbuf b;
        if (cfd < 0) continue;

        /* drain the request line(s); we serve one thing regardless */
        recv(cfd, req, sizeof(req), MSG_DONTWAIT);

        snprintf(hdr, sizeof(hdr),
                 "HTTP/1.0 200 OK\r\n"
                 "Connection: close\r\n"
                 "Cache-Control: no-cache, no-store\r\n"
                 "Pragma: no-cache\r\n"
                 "Content-Type: multipart/x-mixed-replace;boundary=" BOUNDARY "\r\n\r\n");
        if (send(cfd, hdr, strlen(hdr), MSG_NOSIGNAL) < 0) { close(cfd); continue; }

        for (;;) {
            char part[128];
            int n;
            if (cam_grab(&b) < 0) { usleep(20000); continue; }  /* robust: skip & retry */

            n = snprintf(part, sizeof(part),
                         "--" BOUNDARY "\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "Content-Length: %u\r\n\r\n", b.bytesused);
            if (send(cfd, part, n, MSG_NOSIGNAL) < 0)                        break;
            if (send(cfd, bufs[b.index], b.bytesused, MSG_NOSIGNAL) < 0)     break;
            if (send(cfd, "\r\n", 2, MSG_NOSIGNAL) < 0)                      break;

            cam_requeue(&b);
        }
        cam_requeue(&b);
        close(cfd);
    }
    return 0;
}
