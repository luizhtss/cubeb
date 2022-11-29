/*
 * Copyright © 2016 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#define NOMINMAX

#include "cubeb_log.h"
#include "cubeb_ringbuffer.h"
#include "cubeb_tracing.h"
#include <cstdarg>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

std::atomic<cubeb_log_level> g_cubeb_log_level;
std::atomic<cubeb_log_callback> g_cubeb_log_callback;

/** The maximum size of a log message, after having been formatted. */
const size_t CUBEB_LOG_MESSAGE_MAX_SIZE = 256;
/** The maximum number of log messages that can be queued before dropping
 * messages. */
const size_t CUBEB_LOG_MESSAGE_QUEUE_DEPTH = 40;
/** Number of milliseconds to wait before dequeuing log messages. */
#define CUBEB_LOG_BATCH_PRINT_INTERVAL_MS 10

/**
 * This wraps an inline buffer, that represents a log message, that must be
 * null-terminated.
 * This class should not use system calls or other potentially blocking code.
 */
class cubeb_log_message {
public:
  cubeb_log_message() { *storage = '\0'; }
  cubeb_log_message(char const str[CUBEB_LOG_MESSAGE_MAX_SIZE])
  {
    size_t length = strlen(str);
    /* paranoia against malformed message */
    assert(length < CUBEB_LOG_MESSAGE_MAX_SIZE);
    if (length > CUBEB_LOG_MESSAGE_MAX_SIZE - 1) {
      return;
    }
    PodCopy(storage, str, length);
    storage[length] = '\0';
  }
  char const * get() { return storage; }

private:
  char storage[CUBEB_LOG_MESSAGE_MAX_SIZE];
};

/** Lock-free asynchronous logger, made so that logging from a
 *  real-time audio callback does not block the audio thread. */
class cubeb_async_logger {
public:
  /* This is thread-safe since C++11 */
  static cubeb_async_logger & get()
  {
    static cubeb_async_logger instance;
    return instance;
  }
  void push(char const str[CUBEB_LOG_MESSAGE_MAX_SIZE])
  {
    cubeb_log_message msg(str);
    msg_queue->enqueue(msg);
  }
  void run()
  {
    assert(logging_thread.get_id() == std::thread::id());
    assert(msg_queue);
    logging_thread = std::thread([this]() {
      CUBEB_REGISTER_THREAD("cubeb_log");
      while (!shutdown_thread) {
        cubeb_log_message msg;
        while (msg_queue->dequeue(&msg, 1)) {
          cubeb_log_internal_no_format(msg.get());
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(CUBEB_LOG_BATCH_PRINT_INTERVAL_MS));
      }
      CUBEB_UNREGISTER_THREAD();
    });
  }
  // Tell the underlying queue the producer thread has changed, so it does not
  // assert in debug. This should be called with the thread stopped.
  void reset_producer_thread() { msg_queue->reset_thread_ids(); }
  void start()
  {
    msg_queue.reset(
        new lock_free_queue<cubeb_log_message>(CUBEB_LOG_MESSAGE_QUEUE_DEPTH));
    shutdown_thread = false;
    run();
  }
  void stop()
  {
    shutdown_thread = true;
    if (logging_thread.get_id() != std::thread::id()) {
      logging_thread.join();
      logging_thread = std::thread();
      // This is OK, because at this point, we know the consumer has stopped
      // consuming.
      msg_queue->reset_thread_ids();
      purge_queue();
      msg_queue.reset(nullptr);
    }
  }
  void purge_queue()
  {
    assert(logging_thread.get_id() == std::thread::id() &&
           "Only purge the async logger queue when the thread is stopped");
    if (!msg_queue) {
      return;
    }
    cubeb_log_message msg;
    while (msg_queue->dequeue(&msg, 1)) { /* nothing */
    }
  }

private:
  cubeb_async_logger() {}
  /** This is quite a big data structure, but is only instantiated if the
   * asynchronous logger is used.*/
  std::unique_ptr<lock_free_queue<cubeb_log_message>> msg_queue;
  std::atomic<bool> shutdown_thread = {false};
  std::thread logging_thread;
};

void
cubeb_log_internal(char const * file, uint32_t line, char const * fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  char msg[CUBEB_LOG_MESSAGE_MAX_SIZE];
  vsnprintf(msg, CUBEB_LOG_MESSAGE_MAX_SIZE, fmt, args);
  va_end(args);
  g_cubeb_log_callback.load()("%s:%d:%s", file, line, msg);
}

void
cubeb_log_internal_no_format(const char * msg)
{
  g_cubeb_log_callback.load()(msg);
}

void
cubeb_async_log(char const * fmt, ...)
{
  // This is going to copy a 256 bytes array around, which is fine.
  // We don't want to allocate memory here, because this is made to
  // be called from a real-time callback.
  va_list args;
  va_start(args, fmt);
  char msg[CUBEB_LOG_MESSAGE_MAX_SIZE];
  vsnprintf(msg, CUBEB_LOG_MESSAGE_MAX_SIZE, fmt, args);
  cubeb_async_logger::get().push(msg);
  va_end(args);
}

void
cubeb_async_log_reset_threads(void)
{
  if (!g_cubeb_log_callback) {
    return;
  }
  cubeb_async_logger::get().reset_producer_thread();
}

void
cubeb_noop_log_callback(char const * /* fmt */, ...)
{
}

void
cubeb_log_set(cubeb_log_level log_level, cubeb_log_callback log_callback)
{
  g_cubeb_log_level = log_level;
  // Once a callback has a been set, `g_cubeb_log_callback` is never set back to
  // nullptr, to prevent a TOCTOU race between checking the pointer
  if (log_callback && log_level != CUBEB_LOG_DISABLED) {
    g_cubeb_log_callback = log_callback;
    cubeb_async_logger::get().start();
  } else if (!log_callback || CUBEB_LOG_DISABLED) {
    // This returns once the thread has joined.
    cubeb_async_logger::get().stop();
    g_cubeb_log_callback = cubeb_noop_log_callback;
    cubeb_async_logger::get().purge_queue();
  } else {
    assert(false && "Incorrect parameters passed to cubeb_log_set");
  }
}

cubeb_log_level
cubeb_log_get_level()
{
  return g_cubeb_log_level;
}

cubeb_log_callback
cubeb_log_get_callback()
{
  if (g_cubeb_log_callback == cubeb_noop_log_callback) {
    return nullptr;
  }
  return g_cubeb_log_callback;
}
