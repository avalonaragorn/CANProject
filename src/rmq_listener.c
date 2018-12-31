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
#include <arpa/inet.h>

#include "utils.h"
#include "can_utils.h"

#define MAX_LINE 1024

#define EXCHANGE "amq.direct"
#define BINDING_KEY "tbox"
#define ROUTING_KEY "controller"

#define LOCAL_IMAGE "local_img.hex"

#define WAITING_TIME_INTERVAL 500000

#define MAX_SECTOR_SIZE (1800 - 8)

char const *exchange = EXCHANGE;
char const *bindingkey = BINDING_KEY;
char const *routingkey = ROUTING_KEY;
static amqp_connection_state_t conn;

static unsigned int dsp_data_can_id = 0x000004B1;
static unsigned int dsp_default_can_id = 0x000003B1;

static pthread_t can_msg_forward_thread;
void *forward_received_can_msg(void *arg);

int burn_hex_file_to_dsp();

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
  char const *canItf;
  amqp_socket_t *socket = NULL;

  amqp_bytes_t queuename;

  if (argc < 4) {
    fprintf(stderr, "Usage: canproxy host port canItf\n");
    return 1;
  }

  hostname = argv[1];
  port = atoi(argv[2]);
  canItf = argv[3];
  // exchange = argv[3];
  // bindingkey = argv[4];
  // bindingkey = BINDING_KEY;

  // zhj: init can socket
  if (can_init(canItf) !=0)
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

      printf("\nRecevied HEX file size: %ld\n\n", envelope.message.body.len);

      FILE* image = fopen (LOCAL_IMAGE, "w");
      fwrite(envelope.message.body.bytes, 1, envelope.message.body.len, image);
      fclose(image);

      //zhj: download image by can interface
      if (burn_hex_file_to_dsp() < 0)
      {
        printf("Oops! Download DSP image failed\n");
      } else
      {
        printf("Congratulation! Download DSP image successfully\n");
      }
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

int burn_hex_file_to_dsp()
{
  int ret = 0;

  unsigned char can_frame_data[8] = {0};

  unsigned int Total_Data_Bytes_Has_Been_Sent = 0;
  
  unsigned int curBaseAddr = 0;
  unsigned int curAddrOffset = 0;
  unsigned int Data_Bytes_For_Current_Sector_Has_Been_Sent = 0;
  unsigned int Data_Bytes_For_Current_Offset_Has_Been_Sent = 0;
  unsigned int Data_Bytes_For_Current_Base_Addr_Has_Been_Sent = 0;
  
  unsigned char Base_Addr_Just_Be_Set = true;
  unsigned char Need_To_Send_New_Addr_Offset = true;
  unsigned char Last_Sector_Has_Been_Sent = false;

  unsigned char PAUSE_FLAG = false;

  //zhj: Parse HEX file per line, and download image to DSP
  FILE *fp;
  char strLine[MAX_LINE];
  if ((fp = fopen(LOCAL_IMAGE,"r")) == NULL)
  {
    printf("Failed to open file %s!\n", LOCAL_IMAGE);
    return -1;
  }

  unsigned int nbytes=0, addrOffset=0, type=0, line_num=0, i, val, line_chksum;
  unsigned char data[MAX_LINE];
  unsigned char chksum;

  unsigned char sectorId = 0;

  printf("Downloading DSP image ......\n");

  while (!feof(fp))
  { 
    memset(strLine, 0, sizeof(strLine));
    fgets(strLine, sizeof(strLine)-1, fp);

    if (strlen(strLine) == 0)
    {
      break;
    }

    ++line_num;

    const char *s = strLine;

    if (*s != ':')
    {  
      printf("Line[%d]: %s: format violation (1)\n", line_num, strLine);      
      ret = -1;
      goto OUT;
    }

    ++s;
    
    if (sscanf(s, "%02x%04x%02x", &nbytes, &addrOffset, &type) != 3)
    {  
      printf("Line[%d]: %s: format violation (2)\n", line_num, strLine);      
      ret = -1;
      goto OUT;
    }

    if (!(nbytes >= 0 && nbytes < MAX_LINE))
    {
      printf("Unsupport %d bytes per line\n", nbytes);      
      ret = -1;
      goto OUT;
    }

    s += 8;

    chksum = nbytes + addrOffset + (addrOffset>>8) + type;

    for (i=0; i < nbytes; i++)
    {
      val = 0;

      if (sscanf(s, "%02x", &val) != 1)
      {
        printf("Line[%d]: %s: format violation (3)\n", line_num, strLine);        
        ret = -1;
        goto OUT;
      }

      s += 2;

      data[i] = val;
      chksum += val;
    }

    line_chksum = 0;
    if (sscanf(s, "%02x", &line_chksum) != 1)
    {
      printf("Line[%d]: %s: format violation (4)\n", line_num, strLine);      
      ret = -1;
      goto OUT;
    }

    if ((chksum + line_chksum) & 0xff)
    {  
      printf("Line[%d]: %s: checksum mismatch (%u/%u)/n", line_num, strLine, chksum, line_chksum);      
      ret = -1;
      goto OUT;
    }

    unsigned int *tp = NULL;

    switch (type)
    {
        case 0: // Data Record
          if (Base_Addr_Just_Be_Set == false)
          {
            if ((curAddrOffset + Data_Bytes_For_Current_Offset_Has_Been_Sent / 2) != addrOffset)
            {
              if (Data_Bytes_For_Current_Offset_Has_Been_Sent != 0)
              {
                sectorId += 1;
                printf("\nSector[%d]: %d bytes, Done!\n", sectorId, Data_Bytes_For_Current_Sector_Has_Been_Sent);
                printf("\nBase Addr: 0x%08X, Offset: 0x%08X, %d bytes, Done!\n\n", curBaseAddr, curAddrOffset, 
                      Data_Bytes_For_Current_Offset_Has_Been_Sent);
                usleep(WAITING_TIME_INTERVAL);
              }

              //zhj: send can frame, FAD0XXXXXXXX0002
              can_frame_data[0] = 0xFA;
              can_frame_data[1] = 0xD0;
              
              tp = (unsigned int *)&can_frame_data[2];
              *tp = htonl(Data_Bytes_For_Current_Offset_Has_Been_Sent);

              can_frame_data[6] = 0x00;
              can_frame_data[7] = 0x02;

              if (can_send(dsp_data_can_id, can_frame_data, sizeof(can_frame_data)) != 0)
              {
                printf("%s:%d  Failed to send can frame: %s\n", __FILE__, __LINE__, strerror(errno));
                ret = -1;
                goto OUT;
              }

              Need_To_Send_New_Addr_Offset = true;
            }
          }          

          if (Need_To_Send_New_Addr_Offset)
          {
            //zhj: send can frame, FADD0000XXXXXXXX
            can_frame_data[0] = 0xFA;
            can_frame_data[1] = 0xDD;
            can_frame_data[2] = 0x00;
            can_frame_data[3] = 0x00;

            tp = (unsigned int *)&can_frame_data[4];
            *tp = htonl(addrOffset);

            if (can_send(dsp_data_can_id, can_frame_data, sizeof(can_frame_data)) != 0)
            {
              printf("%s:%d  Failed to send can frame: %s\n", __FILE__, __LINE__, strerror(errno));
              ret = -1;
              goto OUT;
            }

            if (Base_Addr_Just_Be_Set)
            {
              Base_Addr_Just_Be_Set = false;
            }

            Need_To_Send_New_Addr_Offset = false;
            curAddrOffset = addrOffset;
            Data_Bytes_For_Current_Offset_Has_Been_Sent = 0;
          }

          for (i = 0; i < (nbytes/8); i++)
          {
            memcpy(can_frame_data, data + i*8, 8);

            if (can_send(dsp_data_can_id, can_frame_data, sizeof(can_frame_data)) !=0)
            {
              printf("%s:%d  Failed to send can frame: %s\n", __FILE__, __LINE__, strerror(errno));
              ret = -1;
              goto OUT;
            }

            Total_Data_Bytes_Has_Been_Sent += 8;
            Data_Bytes_For_Current_Offset_Has_Been_Sent += 8;
            Data_Bytes_For_Current_Base_Addr_Has_Been_Sent += 8;
            Data_Bytes_For_Current_Sector_Has_Been_Sent += 8;

            if (Data_Bytes_For_Current_Sector_Has_Been_Sent >= MAX_SECTOR_SIZE)
            {
              // PAUSE_FLAG = true;

              //zhj: send can frame, FAD0XXXXXXXX0000
              can_frame_data[0] = 0xFA;
              can_frame_data[1] = 0xD0;

              tp = (unsigned int *)&can_frame_data[2];
              *tp = htonl(Data_Bytes_For_Current_Offset_Has_Been_Sent);

              can_frame_data[6] = 0x00;
              can_frame_data[7] = 0x00;

              if (can_send(dsp_default_can_id, can_frame_data, sizeof(can_frame_data)) != 0)
              {
                printf("%s:%d  Failed to send can frame: %s\n", __FILE__, __LINE__, strerror(errno));
                ret = -1;
                goto OUT;
              }

              sectorId += 1;
              printf("\nSector[%d]: %d bytes, Done!\n\n", sectorId, Data_Bytes_For_Current_Sector_Has_Been_Sent);
              usleep(WAITING_TIME_INTERVAL);

              Data_Bytes_For_Current_Sector_Has_Been_Sent = 0;
            }
          }

          unsigned int left = nbytes % 8;
          switch (left)
          {
            case 6:
              //zhj: send can frame, FAD6XXXXXXXXXXXX
              can_frame_data[0] = 0xFA;
              can_frame_data[1] = 0xD6;
              
              memcpy(&can_frame_data[2], data + i*8, 6);

              if (can_send(dsp_data_can_id, can_frame_data, sizeof(can_frame_data)) != 0)
              {
                printf("%s:%d  Failed to send can frame: %s\n", __FILE__, __LINE__, strerror(errno));
                ret = -1;
                goto OUT;
              }

              Total_Data_Bytes_Has_Been_Sent += 6;
              Data_Bytes_For_Current_Offset_Has_Been_Sent += 6;
              Data_Bytes_For_Current_Base_Addr_Has_Been_Sent += 6;
              Data_Bytes_For_Current_Sector_Has_Been_Sent += 6;

              Last_Sector_Has_Been_Sent = true;

              if (Data_Bytes_For_Current_Sector_Has_Been_Sent >= MAX_SECTOR_SIZE)
              {
                PAUSE_FLAG = true;
              }

              break;
            case 4:
              //zhj: send can frame, FAD40000XXXXXXXX
              can_frame_data[0] = 0xFA;
              can_frame_data[1] = 0xD4;
              can_frame_data[2] = 0x00;
              can_frame_data[3] = 0x00;
              
              memcpy(&can_frame_data[4], data + i*8, 4);

              if (can_send(dsp_data_can_id, can_frame_data, sizeof(can_frame_data)) != 0)
              {
                printf("%s:%d  Failed to send can frame: %s\n", __FILE__, __LINE__, strerror(errno));
                ret = -1;
                goto OUT;
              }

              Total_Data_Bytes_Has_Been_Sent += 4;
              Data_Bytes_For_Current_Offset_Has_Been_Sent += 4;
              Data_Bytes_For_Current_Base_Addr_Has_Been_Sent += 4;
              Data_Bytes_For_Current_Sector_Has_Been_Sent += 4;

              Last_Sector_Has_Been_Sent = true;

              if (Data_Bytes_For_Current_Sector_Has_Been_Sent >= MAX_SECTOR_SIZE)
              {
                PAUSE_FLAG = true;
              }

              break;
            case 2:
              //zhj: send can frame, FAD200000000XXXX
              can_frame_data[0] = 0xFA;
              can_frame_data[1] = 0xD4;
              can_frame_data[2] = 0x00;
              can_frame_data[3] = 0x00;
              can_frame_data[4] = 0x00;
              can_frame_data[5] = 0x00;
              
              memcpy(&can_frame_data[6], data + i*8, 2);

              if (can_send(dsp_data_can_id, can_frame_data, sizeof(can_frame_data)) != 0)
              {
                printf("%s:%d  Failed to send can frame: %s\n", __FILE__, __LINE__, strerror(errno));
                ret = -1;
                goto OUT;
              }

              Total_Data_Bytes_Has_Been_Sent += 2;
              Data_Bytes_For_Current_Offset_Has_Been_Sent += 2;
              Data_Bytes_For_Current_Base_Addr_Has_Been_Sent += 2;
              Data_Bytes_For_Current_Sector_Has_Been_Sent += 2;

              Last_Sector_Has_Been_Sent = true;

              if (Data_Bytes_For_Current_Sector_Has_Been_Sent >= MAX_SECTOR_SIZE)
              {
                PAUSE_FLAG = true;
              }

              break;
            case 0:
              Last_Sector_Has_Been_Sent = true;
              break;
            default:
              printf("Line[%d]: %s: data length is odd, %d !!!\n", line_num, strLine, nbytes);          
              ret = -1;
              goto OUT;
          }

          if (PAUSE_FLAG)
          {
            PAUSE_FLAG = false;

            //zhj: send can frame, FAD0XXXXXXXX0000
            can_frame_data[0] = 0xFA;
            can_frame_data[1] = 0xD0;

            tp = (unsigned int *)&can_frame_data[2];
            *tp = htonl(Data_Bytes_For_Current_Offset_Has_Been_Sent);

            can_frame_data[6] = 0x00;
            can_frame_data[7] = 0x00;

            if (can_send(dsp_default_can_id, can_frame_data, sizeof(can_frame_data)) != 0)
            {
              printf("%s:%d  Failed to send can frame: %s\n", __FILE__, __LINE__, strerror(errno));
              ret = -1;
              goto OUT;
            }

            sectorId += 1;
            printf("\nSector[%d]: %d bytes, Done!\n\n", sectorId, Data_Bytes_For_Current_Sector_Has_Been_Sent);
            usleep(WAITING_TIME_INTERVAL);

            Data_Bytes_For_Current_Sector_Has_Been_Sent = 0;
          }

          break;
        case 1: // EOF
          //zhj: send can frame, FAD0XXXXXXXX0003
          can_frame_data[0] = 0xFA;
          can_frame_data[1] = 0xD0;

          tp = (unsigned int *)&can_frame_data[2];
          *tp = htonl(Total_Data_Bytes_Has_Been_Sent);

          can_frame_data[6] = 0x00;
          can_frame_data[7] = 0x03;

          if (can_send(dsp_default_can_id, can_frame_data, sizeof(can_frame_data)) != 0)
          {
            printf("%s:%d  Failed to send can frame: %s\n", __FILE__, __LINE__, strerror(errno));
            ret = -1;
            goto OUT;
          }

          printf("\nTotally %d bytes, Done!\n\n", Total_Data_Bytes_Has_Been_Sent);

          break;
        case 4: // Extended Linear Address Record
          if (Last_Sector_Has_Been_Sent)
          {
            //zhj: send can frame, FAD0XXXXXXXX0001
            can_frame_data[0] = 0xFA;
            can_frame_data[1] = 0xD0;

            tp = (unsigned int *)&can_frame_data[2];
            *tp = htonl(Data_Bytes_For_Current_Base_Addr_Has_Been_Sent);

            can_frame_data[6] = 0x00;
            can_frame_data[7] = 0x01;

            if (can_send(dsp_default_can_id, can_frame_data, sizeof(can_frame_data)) != 0)
            {
              printf("%s:%d  Failed to send can frame: %s\n", __FILE__, __LINE__, strerror(errno));
              ret = -1;
              goto OUT;
            }

            sectorId += 1;
            printf("\nSector[%d]: %d bytes, Done!\n", sectorId, Data_Bytes_For_Current_Sector_Has_Been_Sent);
            printf("\nBase Addr: 0x%08X, Offset: 0x%08X, %d bytes, Done!\n", curBaseAddr, curAddrOffset, Data_Bytes_For_Current_Offset_Has_Been_Sent);
            printf("\nBase Addr: 0x%08X, %d bytes, Done!\n\n", curBaseAddr, Data_Bytes_For_Current_Base_Addr_Has_Been_Sent);

            Last_Sector_Has_Been_Sent = false;
            Data_Bytes_For_Current_Base_Addr_Has_Been_Sent = 0;

            usleep(WAITING_TIME_INTERVAL);
          }

          //zhj: send new base address, FADB0000XXXX0000
          can_frame_data[0] = 0xFA;
          can_frame_data[1] = 0xDB;
          can_frame_data[2] = 0x00;
          can_frame_data[3] = 0x00;

          memcpy(&can_frame_data[4], data, 2);

          can_frame_data[6] = 0x00;
          can_frame_data[7] = 0x00;

          curBaseAddr = *((unsigned int *)&can_frame_data[4]);
          curBaseAddr = ntohl(curBaseAddr);

          if (can_send(dsp_default_can_id, can_frame_data, sizeof(can_frame_data)) != 0)
          {
            printf("%s:%d  Failed to send can frame: %s\n", __FILE__, __LINE__, strerror(errno));
            ret = -1;
            goto OUT;
          }

          Base_Addr_Just_Be_Set = true;
          Need_To_Send_New_Addr_Offset = true;
          sectorId = 0;

          break;
        default:
          printf("Line[%d]: %s: Unknown entry type %d\n", line_num, strLine, type);
          ret = -1;
          goto OUT;
    }
  }


OUT:
  fclose(fp);
  return ret;
}
