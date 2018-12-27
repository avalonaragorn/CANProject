#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <amqp.h>
#include <amqp_tcp_socket.h>

#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <net/if.h>

#include "utils.h"
#include "can_utils.h"


#define EXCHANGE "amq.direct";
#define BINDING_KEY "tbox";
#define ROUTING_KEY "controller";

#define LOCAL_IMAGE "local_img.hex"

char const *exchange = EXCHANGE;
char const *bindingkey = BINDING_KEY;
char const *routingkey = ROUTING_KEY;
static amqp_connection_state_t conn;

static unsigned int dsp_data_can_id = 0x000004B1;
static unsigned int dsp_default_can_id = 0x000003B1;

static pthread_t can_msg_forward_thread;
void *forward_received_can_msg(void *arg);

int rmq_send(unsigned char* payload, unsigned char len)
{
    amqp_basic_properties_t props;
    props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
    props.content_type = amqp_cstring_bytes("text/plain");
    props.delivery_mode = 2; /* persistent delivery mode */

    amqp_bytes_t msg = amqp_bytes_malloc(len);
    memcpy(msg.bytes, payload, msg.len);    

    int ret = 0;
    if (AMQP_STATUS_OK  == amqp_basic_publish(conn, 1, amqp_cstring_bytes(exchange), amqp_cstring_bytes(routingkey), 0, 0, &props, msg))
    {
      ret = 0;
    } else
    {
      ret = -1;
    }

    amqp_bytes_free(msg);

    return ret;
}

int main(int argc, char const *const *argv)
{
  char const *hostname;
  int port, status;
  // char const *exchange;
  // char const *bindingkey;
  amqp_socket_t *socket = NULL;

  amqp_bytes_t queuename;

  if (argc < 3) {
    fprintf(stderr, "Usage: canproxy host port\n");
    return 1;
  }

  // zhj: init can socket
  // if (can_init("can0") !=0)
  if (can_init("vcan0") !=0)
  {
    printf("Failed to init can socket!\n");
  }

  int ret = pthread_create(&can_msg_forward_thread, 0, forward_received_can_msg, NULL);
  if (ret != 0)
  {
      printf("Failed to Create CAN Msg Forward Thread: %s\n", strerror(errno));
      return ret;
  } else
  {
      printf("Create CAN Msg Forward Thread!\n");
  }
  //zhj: end

  hostname = argv[1];
  port = atoi(argv[2]);
  // exchange = argv[3];
  // bindingkey = argv[4];
  // bindingkey = BINDING_KEY;

  conn = amqp_new_connection();

  socket = amqp_tcp_socket_new(conn);
  if (!socket) {
    die("creating TCP socket");
  }

  status = amqp_socket_open(socket, hostname, port);
  if (status) {
    die("opening TCP socket");
  }

  die_on_amqp_error(amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN,
  //                             "guest", "guest"),
                               "admin", "asb#1234"),
                    "Logging in");
  amqp_channel_open(conn, 1);
  die_on_amqp_error(amqp_get_rpc_reply(conn), "Opening channel");

  {
    amqp_queue_declare_ok_t *r = amqp_queue_declare(
        conn, 1, amqp_empty_bytes, 0, 0, 0, 1, amqp_empty_table);
    die_on_amqp_error(amqp_get_rpc_reply(conn), "Declaring queue");
    queuename = amqp_bytes_malloc_dup(r->queue);
    if (queuename.bytes == NULL) {
      fprintf(stderr, "Out of memory while copying queue name");
      return 1;
    }
  }

  amqp_queue_bind(conn, 1, queuename, amqp_cstring_bytes(exchange),
                  amqp_cstring_bytes(bindingkey), amqp_empty_table);
  die_on_amqp_error(amqp_get_rpc_reply(conn), "Binding queue");

  amqp_basic_consume(conn, 1, queuename, amqp_empty_bytes, 0, 1, 0,
                     amqp_empty_table);
  die_on_amqp_error(amqp_get_rpc_reply(conn), "Consuming");

  {
    for (;;) {
      amqp_rpc_reply_t res;
      amqp_envelope_t envelope;

      amqp_maybe_release_buffers(conn);

      res = amqp_consume_message(conn, &envelope, NULL, 0);

      if (AMQP_RESPONSE_NORMAL != res.reply_type) {
        break;
      }

      printf("Delivery %u, exchange %.*s routingkey %.*s\n",
             (unsigned)envelope.delivery_tag, (int)envelope.exchange.len,
             (char *)envelope.exchange.bytes, (int)envelope.routing_key.len,
             (char *)envelope.routing_key.bytes);

      if (envelope.message.properties._flags & AMQP_BASIC_CONTENT_TYPE_FLAG) {
        printf("Content-type: %.*s\n",
               (int)envelope.message.properties.content_type.len,
               (char *)envelope.message.properties.content_type.bytes);
      }
      printf("----\n");

      amqp_dump(envelope.message.body.bytes, envelope.message.body.len);

      printf("\nRecevied HEX file size: %ld\n", envelope.message.body.len);

      FILE* image = fopen (LOCAL_IMAGE, "w");
      fwrite(envelope.message.body.bytes, 1, envelope.message.body.len, image);
      fclose(image);

      //zhj: download image by can interface
      printf("Downloading DSP image ......\n");

      unsigned char payload[8] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};

      int i;
      for (i = 0; i < (envelope.message.body.len/sizeof(payload)); i++)
      {
        memcpy(payload, (unsigned char*)envelope.message.body.bytes + i * sizeof(payload), sizeof(payload));

        if (can_send(dsp_default_can_id, payload, sizeof(payload)) !=0)
        {
          printf("Failed to send can frame!\n");
        }

      }

      if (envelope.message.body.len > (i * sizeof(payload)))
      {
        memcpy(payload, (unsigned char*)envelope.message.body.bytes + i * sizeof(payload), envelope.message.body.len%sizeof(payload));
        if (can_send(dsp_default_can_id, payload, envelope.message.body.len%sizeof(payload)) !=0)
        {
          printf("Failed to send can frame!\n");
        }
      }

      printf("End\n");

      // zhj

      amqp_destroy_envelope(&envelope);
    }
  }

  die_on_amqp_error(amqp_channel_close(conn, 1, AMQP_REPLY_SUCCESS),
                    "Closing channel");
  die_on_amqp_error(amqp_connection_close(conn, AMQP_REPLY_SUCCESS),
                    "Closing connection");
  die_on_error(amqp_destroy_connection(conn), "Ending connection");

  return 0;
}

void *forward_received_can_msg(void *arg)
{
  struct canfd_frame frame;

  while (1)
  {
      memset(&frame, 0, sizeof(struct canfd_frame));
      int nbytes = read(canSocket, &frame, sizeof(struct canfd_frame));
      if (nbytes < 0)
      {
          printf("CAN Recv failed: %s\n", strerror(errno));
          continue;
      }

      // printf("the nbytes:%d\n", nbytes);
      // printf("length:%ld\n", sizeof(frame));
      printf("Recv CAN Msg:\t%03X    [%d]    %02X %02X %02X %02X %02X %02X %02X %02X",
          frame.can_id,
          frame.len,
          frame.data[0],
          frame.data[1],
          frame.data[2],
          frame.data[3],
          frame.data[4],
          frame.data[5],
          frame.data[6],
          frame.data[7]);

      int frame_size = sizeof(struct canfd_frame) - sizeof(frame.data) + frame.len;

      if (rmq_send((unsigned char *)&frame, frame_size) < 0)
      {
        printf("\t\tForward Fail\n");     
      } else
      {
        printf("\t\tForward Succ\n");
      }
  }

  return NULL;
}
