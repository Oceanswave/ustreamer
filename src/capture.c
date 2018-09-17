#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <assert.h>
#include <sys/select.h>
#include <linux/videodev2.h>

#include "tools.h"
#include "device.h"
#include "jpeg.h"
#include "capture.h"


static int _capture_init_loop(struct device_t *dev, struct workers_pool_t *pool, sig_atomic_t *volatile global_stop);
static int _capture_init(struct device_t *dev, struct workers_pool_t *pool, sig_atomic_t *volatile global_stop);
static void _capture_init_workers(struct device_t *dev, struct workers_pool_t *pool, sig_atomic_t *volatile global_stop);
static void *_capture_worker_thread(void *v_ctx_ptr);
static void _capture_destroy_workers(struct device_t *dev, struct workers_pool_t *pool);
static int _capture_control(struct device_t *dev, const bool enable);
static int _capture_grab_buffer(struct device_t *dev, struct v4l2_buffer *buf_info);
static int _capture_release_buffer(struct device_t *dev, struct v4l2_buffer *buf_info);
static int _capture_handle_event(struct device_t *dev);


void capture_loop(struct device_t *dev, sig_atomic_t *volatile global_stop) {
	struct workers_pool_t pool;
	volatile sig_atomic_t workers_stop;

	MEMSET_ZERO(pool);
	pool.workers_stop = (sig_atomic_t *volatile)&workers_stop;

	LOG_INFO("Using V4L2 device: %s", dev->path);
	LOG_INFO("Using JPEG quality: %d%%", dev->jpeg_quality);

	while (_capture_init_loop(dev, &pool, global_stop) == 0) {
		int frames_count = 0;

		while (!*global_stop) {
			SEP_DEBUG('-');

			fd_set read_fds;
			fd_set write_fds;
			fd_set error_fds;

			FD_ZERO(&read_fds);
			FD_SET(dev->run->fd, &read_fds);

			FD_ZERO(&write_fds);
			FD_SET(dev->run->fd, &write_fds);

			FD_ZERO(&error_fds);
			FD_SET(dev->run->fd, &error_fds);

			struct timeval timeout;
			timeout.tv_sec = dev->timeout;
			timeout.tv_usec = 0;

			LOG_DEBUG("Calling select() on video device ...");
			int retval = select(dev->run->fd + 1, &read_fds, &write_fds, &error_fds, &timeout);
			LOG_DEBUG("Device select() --> %d", retval);

			if (retval < 0) {
				if (errno != EINTR) {
					LOG_PERROR("Mainloop select() error");
					break;
				}

			} else if (retval == 0) {
				LOG_ERROR("Mainloop select() timeout");
				break;

			} else {
				if (FD_ISSET(dev->run->fd, &read_fds)) {
					LOG_DEBUG("Frame is ready, waiting for workers ...");

					A_PTHREAD_M_LOCK(&pool.has_free_workers_mutex);
					A_PTHREAD_C_WAIT_TRUE(pool.has_free_workers, &pool.has_free_workers_cond, &pool.has_free_workers_mutex);
					A_PTHREAD_M_UNLOCK(&pool.has_free_workers_mutex);

					struct v4l2_buffer buf_info;

					if (_capture_grab_buffer(dev, &buf_info) < 0) {
						break;
					}

					if (dev->every_frame) {
						if (frames_count < (int)dev->every_frame - 1) {
							LOG_DEBUG("Dropping frame %d for option --every-frame=%d", frames_count + 1, dev->every_frame);
							++frames_count;
							goto pass_frame;
						} else {
							frames_count = 0;
						}
					}

					// Workaround for broken, corrupted frames:
					// Under low light conditions corrupted frames may get captured.
					// The good thing is such frames are quite small compared to the regular pictures.
					// For example a VGA (640x480) webcam picture is normally >= 8kByte large,
					// corrupted frames are smaller.
					if (buf_info.bytesused < dev->min_frame_size) {
						LOG_DEBUG("Dropping too small frame sized %d bytes, assuming it as broken", buf_info.bytesused);
						goto pass_frame;
					}

					LOG_DEBUG("Grabbed a new frame to buffer %d", buf_info.index);
					pool.workers[buf_info.index].ctx.buf_info = buf_info;

					A_PTHREAD_M_LOCK(&pool.workers[buf_info.index].has_job_mutex);
					pool.workers[buf_info.index].has_job = true;
					A_PTHREAD_M_UNLOCK(&pool.workers[buf_info.index].has_job_mutex);
					A_PTHREAD_C_SIGNAL(&pool.workers[buf_info.index].has_job_cond);

					goto next_handlers;

					pass_frame:

					if (_capture_release_buffer(dev, &buf_info) < 0) {
						break;
					}
				}

				next_handlers:

				if (FD_ISSET(dev->run->fd, &write_fds)) {
					LOG_ERROR("Got unexpected writing event, seems device was disconnected");
					break;
				}

				if (FD_ISSET(dev->run->fd, &error_fds)) {
					LOG_INFO("Got V4L2 event");
					if (_capture_handle_event(dev) < 0) {
						break;
					}
				}
			}
		}
	}

	_capture_destroy_workers(dev, &pool);
	_capture_control(dev, false);
	device_close(dev);
}

static int _capture_init_loop(struct device_t *dev, struct workers_pool_t *pool, sig_atomic_t *volatile global_stop) {
	int retval = -1;

	LOG_DEBUG("%s: global_stop = %d", __FUNCTION__, *global_stop);
	while (!*global_stop) {
		if ((retval = _capture_init(dev, pool, global_stop)) < 0) {
			LOG_INFO("Sleeping %d seconds before new capture init ...", dev->error_timeout);
			sleep(dev->error_timeout);
		} else {
			break;
		}
	}
	return retval;
}

static int _capture_init(struct device_t *dev, struct workers_pool_t *pool, sig_atomic_t *volatile global_stop) {
	SEP_INFO('=');

	_capture_destroy_workers(dev, pool);
	_capture_control(dev, false);
	device_close(dev);

	if (device_open(dev) < 0) {
		goto error;
	}
	if (_capture_control(dev, true) < 0) {
		goto error;
	}
	_capture_init_workers(dev, pool, global_stop);

	return 0;

	error:
		device_close(dev);
		return -1;
}

static void _capture_init_workers(struct device_t *dev, struct workers_pool_t *pool, sig_atomic_t *volatile global_stop) {
	LOG_DEBUG("Spawning %d workers ...", dev->run->n_buffers);

	*pool->workers_stop = false;
	A_CALLOC(pool->workers, dev->run->n_buffers, sizeof(*pool->workers));

	A_PTHREAD_M_INIT(&pool->has_free_workers_mutex);
	A_PTHREAD_C_INIT(&pool->has_free_workers_cond);

	for (unsigned index = 0; index < dev->run->n_buffers; ++index) {
		A_PTHREAD_M_INIT(&pool->workers[index].has_job_mutex);
		A_PTHREAD_C_INIT(&pool->workers[index].has_job_cond);

		pool->workers[index].ctx.index = index;
		pool->workers[index].ctx.dev = dev;
		pool->workers[index].ctx.global_stop = global_stop;
		pool->workers[index].ctx.workers_stop = pool->workers_stop;

		pool->workers[index].ctx.last_comp_time_mutex = &pool->workers[index].last_comp_time_mutex;
		pool->workers[index].ctx.last_comp_time = &pool->workers[index].last_comp_time;

		pool->workers[index].ctx.has_job_mutex = &pool->workers[index].has_job_mutex;
		pool->workers[index].ctx.has_job = &pool->workers[index].has_job;
		pool->workers[index].ctx.has_job_cond = &pool->workers[index].has_job_cond;

		pool->workers[index].ctx.has_free_workers_mutex = &pool->has_free_workers_mutex;
		pool->workers[index].ctx.has_free_workers = &pool->has_free_workers;
		pool->workers[index].ctx.has_free_workers_cond = &pool->has_free_workers_cond;

		A_PTHREAD_CREATE(&pool->workers[index].tid,	_capture_worker_thread,	(void *)&pool->workers[index].ctx);
	}
}

static void *_capture_worker_thread(void *v_ctx_ptr) {
	struct worker_context_t *ctx = (struct worker_context_t *)v_ctx_ptr;

	LOG_DEBUG("Hello! I am a worker #%d ^_^", ctx->index);

	while (!*ctx->global_stop && !*ctx->workers_stop) {
		A_PTHREAD_M_LOCK(ctx->has_free_workers_mutex);
		*ctx->has_free_workers = true;
		A_PTHREAD_M_UNLOCK(ctx->has_free_workers_mutex);
		A_PTHREAD_C_SIGNAL(ctx->has_free_workers_cond);

		LOG_DEBUG("Worker %d waiting for a new job ...", ctx->index);
		A_PTHREAD_M_LOCK(ctx->has_job_mutex);
		A_PTHREAD_C_WAIT_TRUE(*ctx->has_job, ctx->has_job_cond, ctx->has_job_mutex);
		A_PTHREAD_M_UNLOCK(ctx->has_job_mutex);

		if (!*ctx->workers_stop) {
			unsigned long compressed;
			time_t start_sec;
			time_t stop_sec;
			long start_msec;
			long stop_msec;
			long double last_comp_time;

			now_ms(&start_sec, &start_msec);

			LOG_DEBUG("Worker %d compressing JPEG ...", ctx->index);

			compressed = jpeg_compress_buffer(ctx->dev, ctx->index);

			assert(!_capture_release_buffer(ctx->dev, &ctx->buf_info)); // FIXME
			*ctx->has_job = false;

			now_ms(&stop_sec, &stop_msec);
			if (start_sec <= stop_sec) {
				last_comp_time = (stop_sec - start_sec) + ((long double)(stop_msec - start_msec)) / 1000;
			} else {
				last_comp_time = 0;
			}

			A_PTHREAD_M_LOCK(ctx->last_comp_time_mutex);
			*ctx->last_comp_time = last_comp_time;
			A_PTHREAD_M_UNLOCK(ctx->last_comp_time_mutex);

			LOG_PERF("Compressed JPEG size=%ld; time=%LG (worker %d)", compressed, last_comp_time, ctx->index); // FIXME
		}
	}

	LOG_DEBUG("Bye-bye (worker %d)", ctx->index);
	return NULL;
}

static void _capture_destroy_workers(struct device_t *dev, struct workers_pool_t *pool) {
	LOG_INFO("Destroying workers ...");
	if (pool->workers) {
		*pool->workers_stop = true;
		for (unsigned index = 0; index < dev->run->n_buffers; ++index) {
			A_PTHREAD_M_LOCK(&pool->workers[index].has_job_mutex);
			pool->workers[index].has_job = true; // Final job: die
			A_PTHREAD_M_UNLOCK(&pool->workers[index].has_job_mutex);
			A_PTHREAD_C_SIGNAL(&pool->workers[index].has_job_cond);

			A_PTHREAD_JOIN(pool->workers[index].tid);
			A_PTHREAD_M_DESTROY(&pool->workers[index].has_job_mutex);
			A_PTHREAD_C_DESTROY(&pool->workers[index].has_job_cond);
		}

		A_PTHREAD_M_DESTROY(&pool->has_free_workers_mutex);
		A_PTHREAD_C_DESTROY(&pool->has_free_workers_cond);

		free(pool->workers);
	}
	pool->workers = NULL;
}

static int _capture_control(struct device_t *dev, const bool enable) {
	if (enable != dev->run->capturing) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		LOG_DEBUG("Calling ioctl(%s) ...", (enable ? "VIDIOC_STREAMON" : "VIDIOC_STREAMOFF"));
		if (xioctl(dev->run->fd, (enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF), &type) < 0) {
			LOG_PERROR("Unable to %s capturing", (enable ? "start" : "stop"));
			if (enable) {
				return -1;
			}
		}

		dev->run->capturing = enable;
		LOG_INFO("Capturing %s", (enable ? "started" : "stopped"));
	}
    return 0;
}

static int _capture_grab_buffer(struct device_t *dev, struct v4l2_buffer *buf_info) {
	memset(buf_info, 0, sizeof(struct v4l2_buffer));
	buf_info->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf_info->memory = V4L2_MEMORY_MMAP;

	LOG_DEBUG("Calling ioctl(VIDIOC_DQBUF) ...");
	if (xioctl(dev->run->fd, VIDIOC_DQBUF, buf_info) < 0) {
		LOG_PERROR("Unable to dequeue buffer");
		return -1;
	}

	LOG_DEBUG("Got a new frame in buffer index=%d; bytesused=%d", buf_info->index, buf_info->bytesused);
	if (buf_info->index >= dev->run->n_buffers) {
		LOG_ERROR("Got invalid buffer index=%d; nbuffers=%d", buf_info->index, dev->run->n_buffers);
		return -1;
	}
	return 0;
}

static int _capture_release_buffer(struct device_t *dev, struct v4l2_buffer *buf_info) {
	LOG_DEBUG("Calling ioctl(VIDIOC_QBUF) ...");
	if (xioctl(dev->run->fd, VIDIOC_QBUF, buf_info) < 0) {
		LOG_PERROR("Unable to requeue buffer");
		return -1;
	}
	return 0;
}

static int _capture_handle_event(struct device_t *dev) {
	struct v4l2_event event;

	LOG_DEBUG("Calling ioctl(VIDIOC_DQEVENT) ...");
	if (!xioctl(dev->run->fd, VIDIOC_DQEVENT, &event)) {
		switch (event.type) {
			case V4L2_EVENT_SOURCE_CHANGE:
				LOG_INFO("Got V4L2_EVENT_SOURCE_CHANGE: source changed");
				return -1;
			case V4L2_EVENT_EOS:
				LOG_INFO("Got V4L2_EVENT_EOS: end of stream (ignored)");
				return 0;
		}
	} else {
		LOG_ERROR("Got some V4L2 device event, but where is it? ");
	}
	return 0;
}