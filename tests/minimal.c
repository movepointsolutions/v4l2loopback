#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>

/* Name of the /dev/video* device we will be testing. */

const char *device_name = "/dev/video0";

/* Per-opener state, shared between source and sink. */

struct buffer {
        struct v4l2_buffer b;
        uint8_t *data;
};

struct opener {
        int fd;

        uint32_t buf_type;
        int buf_count;

        /* Array of buffers allocated to this opener. */
        struct buffer *buf;

        /* Bitmask recording buffer ownership. If set, userland owns
         * the buffer; if clear, the kernel owns it. */
        uint32_t buf_owner;
};

/* Assertions. */

static inline void assert_errno(int v) {
        if(!v) {
                fprintf(stderr, "Unexpected error: %s (%d)\n", strerror(errno), errno);
                assert(0);
        }
}

static inline void assert_rv(int v) {
        assert_errno(v >= 0);
}

/* Set up a new opener. */

void opener_open(struct opener *t) {

        t->fd = open(device_name, O_RDWR);
        assert_rv(t->fd);

        if(t->buf_type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
                
                struct v4l2_format fmt;

                memset(&fmt, 0, sizeof fmt);
                fmt.type = t->buf_type;

                assert_rv(ioctl(t->fd, VIDIOC_G_FMT, &fmt));

                fmt.fmt.pix.width = 800;
                fmt.fmt.pix.height = 600;
                fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
                
                assert_rv(ioctl(t->fd, VIDIOC_S_FMT, &fmt));
        }
        
        struct v4l2_requestbuffers req;

        memset(&req, 0, sizeof req);
        req.count = 2;
        req.type = t->buf_type;
        req.memory = V4L2_MEMORY_MMAP;

        assert_rv(ioctl(t->fd, VIDIOC_REQBUFS, &req));

        t->buf_count = req.count;
        t->buf = calloc(t->buf_count, sizeof *t->buf);
        assert(t->buf);

        int i;
        for(i=0; i<t->buf_count; i++) {
                struct v4l2_buffer *b = &t->buf[i].b;

                b->type = t->buf_type;
                b->memory = V4L2_MEMORY_MMAP;
                b->index = i;

                assert_rv(ioctl(t->fd, VIDIOC_QUERYBUF, b));
                
                t->buf[i].data = mmap(NULL, b->length,
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED,
                                      t->fd,
                                      b->m.offset);
                assert_errno(t->buf[i].data != MAP_FAILED);

                /* All buffers are owned by the user to begin with. */
                t->buf_owner |= 1 << i;
        }
}

/* Helpers for testing the ownership mask, to make assertion failure
 * messages more readable. */

static int owned_by_kernel(struct opener *t, int i) {
        return ((t->buf_owner >> i) & 1) == 0;
}

static int owned_by_user(struct opener *t, int i) {
        return ((t->buf_owner >> i) & 1) != 0;
}

/* These functions issue VIDIOC_QBUF and VIDIOC_DQBUF operations
 * respectively, sanity-checking the buffer ownership using the
 * buf_owner field, keeping it up to date, and printing a log. */

/* The DQBUF functions return the index of the dequeued buffer. */

int opener_dqbuf(struct opener *t) {
        struct v4l2_buffer buf;
        buf.type = t->buf_type;

        assert_rv(ioctl(t->fd, VIDIOC_DQBUF, &buf));
        printf("index %d\n", buf.index);
        assert(owned_by_kernel(t, buf.index));
        t->buf_owner |= (1 << buf.index);

        return buf.index;
}

int sink_dqbuf(struct opener *t) {
        printf("Sink   DQBUF ");
        return opener_dqbuf(t);
}

int source_dqbuf(struct opener *t) {
        printf("Source DQBUF ");
        return opener_dqbuf(t);
}

/* Two of the QBUF functions take the index of the buffer to be
 * queued. */

void opener_qbuf(struct opener *t, int i) {
        assert(owned_by_user(t, i));
        t->buf_owner &= ~(1 << i);

        assert_rv(ioctl(t->fd, VIDIOC_QBUF, &t->buf[i].b));
}

void sink_qbuf(struct opener *t, int i) {
        printf("Sink    QBUF index %d\n", i);
        opener_qbuf(t, i);
}

/* The source QBUF function looks for a buffer that we own and
 * dequeues one from the kernel if we've run out. */

void source_qbuf(struct opener *t) {
        int i;

        /* Look for a buffer that we own. */
        for(i=0; i<t->buf_count; i++)
                if(owned_by_user(t, i))
                        break;

        if(i == t->buf_count) {
                /* We're out of buffers. Dequeue one from the kernel. */
                i = source_dqbuf(t);
        }
        
        struct buffer *b = &t->buf[i];
        assert(b->b.index == i);

        b->b.bytesused = b->b.length;
        b->b.field = 0;

        printf("Source  QBUF index %d\n", i);

        opener_qbuf(t, i);
}

/* Start streaming. */

void opener_stream_on(struct opener *t) {
        assert_rv(ioctl(t->fd, VIDIOC_STREAMON, &t->buf_type));
}

/* The test case itself. */

int main(void) {

        /* Initialise two openers with two buffers. */
        
        struct opener src  = { .buf_type = V4L2_BUF_TYPE_VIDEO_OUTPUT  };
        struct opener sink = { .buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE };

        opener_open(&src);
        opener_open(&sink);

        assert(src.buf_count == 2);
        assert(sink.buf_count == 2);

        /* Queue an initial frame for each opener. */
        
        source_qbuf(&src); 
        
        sink_qbuf(&sink, 0);

        /* This variable represents the buffer that the sink is
         * reading from somehow while the source is writing into the
         * other one. */
        int sink_in_hand_buffer = 1;
                
        opener_stream_on(&src);
        opener_stream_on(&sink);

        /* Now stream data through the loopback device. */
        
        int i, buf;
        for(i=0; i<50; i++) {

                /* Read the next frame from the loopback device and
                 * requeue the old one that we've just finished
                 * "processing". */

                int sink_dequeued_buffer = sink_dqbuf(&sink);
                sink_qbuf(&sink, sink_in_hand_buffer);
                sink_in_hand_buffer = sink_dequeued_buffer;

                /* Queue the next frame from the source. This picks a
                 * buffer automatically, dequeuing one from the
                 * loopback device if we've run out. */
                source_qbuf(&src);
        }
}
