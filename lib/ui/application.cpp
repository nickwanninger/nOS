#include <ck/io.h>
#include <errno.h>
#include <lumen.h>
#include <lumen/msg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <ck/time.h>

#include <ui/application.h>

static ui::application *the_app = NULL;

ui::application &ui::application::get(void) {
  if (the_app == NULL) panic("No application constructed\n");
  return *the_app;
}

ui::application::application(void) {
  if (the_app != NULL) {
    fprintf(stderr, "Cannot have multiple ui::applications\n");
    exit(EXIT_FAILURE);
  }

  the_app = this;
  sock.connect("/usr/servers/lumen");

  if (sock.connected()) {
    sock.on_read([this] {
      drain_messages();
      dispatch_messages();
    });


    struct lumen::greet_msg greet = {0};
    struct lumen::greetback_msg greetback = {0};

    greet.pid = getpid();
    if (send_msg_sync(LUMEN_MSG_GREET, greet, &greetback)) {
      // printf("my guest id is %d!\n", greetback.guest_id);
    }
  }
}

ui::application::~application(void) {
  the_app = NULL;
  // TODO: send the shutdown message
}


static unsigned long nextmsgid(void) {
  static unsigned long sid = 0;

  return sid++;
}

long ui::application::send_raw(int type, void *payload, size_t payloadsize) {
  // ck::time::logger l("async ipc send");
  size_t msgsize = payloadsize + sizeof(lumen::msg);
  auto msg = (lumen::msg *)malloc(msgsize);

  long id = nextmsgid();

  msg->magic = LUMEN_MAGIC;
  msg->type = type;
  msg->id = id;
  msg->len = payloadsize;

  if (payloadsize > 0) memcpy(msg + 1, payload, payloadsize);

  // printf("\033[31;1m[client send] id: %6d. type: %04x\033[31;0m\n", msg->id,
  // msg->type);
  auto w = sock.write((const void *)msg, msgsize);

  free(msg);

  if (w < 0) return -1;

  return id;
}



lumen::msg *ui::application::send_raw_sync(int type, void *payload, size_t payloadsize) {
  long req_id = send_raw(type, payload, payloadsize);
  if (req_id == -1) return NULL;

  lumen::msg *response = NULL;

  // wait for a response (this can be smarter)
  while (response == NULL) {
    auto msgs = sock.drain<lumen::msg>();

    for (auto *got : msgs) {
      if (got->id == req_id) {
        response = got;
      } else {
        m_pending_messages.push(got);
      }
    }
  }

  return response;
}



void ui::application::drain_messages(void) {
  auto msgs = sock.drain<lumen::msg>();

  for (auto *msg : msgs) {
    m_pending_messages.push(msg);
  }
}


void ui::application::dispatch_messages(void) {
  for (auto *msg : m_pending_messages) {
    if (msg->type == LUMEN_MSG_INPUT) {
      auto *inp = (struct lumen::input_msg *)(msg + 1);

      int wid = inp->window_id;
      if (m_windows.contains(wid)) {
        auto *win = m_windows.get(wid);
        assert(win != NULL);
        win->handle_input(*inp);
      } else {
        printf(
            "Got n input message from the window server for a window I don't "
            "control! (wid=%d)\n",
            wid);
      }
    } else {
      printf("unhandled message %d. Type %d. (%p)\n", msg->id, msg->type, msg);
    }

    delete msg;
  }
  m_pending_messages.clear();
}


void ui::application::add_window(int id, ui::window *win) {
  assert(!m_windows.contains(id));

  m_windows[id] = win;
}


void ui::application::remove_window(int id) {
  assert(m_windows.contains(id));

  m_windows.remove(id);
}

void ui::application::start(void) { m_eventloop.start(); }
