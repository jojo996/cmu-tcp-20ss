/**
 * Copyright (C) 2022 Carnegie Mellon University
 *
 * This file is part of the TCP in the Wild course project developed for the
 * Computer Networks course (15-441/641) taught at Carnegie Mellon University.
 *
 * No part of the project may be copied and/or distributed without the express
 * permission of the 15-441/641 course staff.
 *
 *
 * This file implements a simple CMU-TCP server. Its purpose is to provide
 * simple test cases and demonstrate how the sockets will be used.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "cmu_tcp.h"
#include "backend.h"

#define BUF_SIZE 10000

/*
 * Param: sock - used for reading and writing to a connection
 *
 * Purpose: To provide some simple test cases and demonstrate how
 *  the sockets will be used.
 *
 */
void functionality(cmu_socket_t *sock) {
  uint8_t buf[BUF_SIZE];
  FILE *fp;
  int n;

  n = cmu_read(sock, buf, BUF_SIZE, NO_FLAG);
  printf("R: %s\n", buf);
  printf("N: %d\n", n);
  cmu_write(sock, "hi there", 9);
  n = cmu_read(sock, buf, 200, NO_FLAG);
  printf("R: %s\n", buf);
  printf("N: %d\n", n);
  cmu_write(sock, "https://www.youtube.com/watch?v=dQw4w9WgXcQ", 44);

  sleep(1);
  n = cmu_read(sock, buf, BUF_SIZE, NO_FLAG);
  printf("N: %d\n", n);
  fp = fopen("/tmp/file.c", "w");
  fwrite(buf, 1, n, fp);
  fclose(fp);
}

void TCP_handshake_server(cmu_socket_t *sock) {
  while (sock->state != TCP_ESTABLISHED){
    unsigned char *packet;
    cmu_tcp_header_t *header;
    switch (sock->state) {
      case TCP_CLOSED:
        sock->state = TCP_LISTEN;
        break;
      case TCP_LISTEN: { /* first time */
        uint32_t seq;
        /* server堵塞直到有SYN到达 */
        printf("waiting for SYN...");
        header = check_for_data(sock, NO_FLAG);
        if ((get_flags(header) & SYN_FLAG_MASK) == SYN_FLAG_MASK) {
          printf("SYN-ACK received");
          seq = get_seq(header);
          uint32_t ack = seq + 1;
          seq = 0;  // rand() % MAXSEQ; todo, 这里其实应该选另一个seq值
          /* 这里是SYN|ACK */
          packet = create_packet(sock->my_port, sock->their_port, seq, ack,
                                     DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                                     (SYN_FLAG_MASK | ACK_FLAG_MASK),
                                     MAX_RECV_SIZE, 0, NULL, NULL, 0);
          sendto(sock->socket, packet, DEFAULT_HEADER_LEN, 0,
                 (struct sockaddr *)&(sock->conn), sizeof(sock->conn));
          free(packet);
          sock->state = TCP_SYN_RCVD;
          sock->window.last_ack_received = ack;
          sock->window.last_seq_received = seq;
        }
        free(header);
        //这一轮等来的不是syn会继续到下一轮等
        break;
      }
      case TCP_SYN_RCVD: { /* after recv */
        printf("waiting for second ACK...");
        header = check_for_data(sock, TIMEOUT);
        int flag = ((get_flags(header) & ACK_FLAG_MASK) == ACK_FLAG_MASK);
        uint32_t ack = get_seq(header);
        uint32_t seq = get_ack(header);
        // sock->window.adv_window = get_advertised_window(header); 
        if (flag && ack == sock->window.last_ack_received &&
            seq == sock->window.last_seq_received + 1) {
          sock->state = TCP_ESTABLISHED;
          sock->window.last_ack_received = ack;
          sock->window.last_seq_received = seq;
        } else {
          printf("尝试建立连接失败");
          sock->state = TCP_LISTEN;
        }
        free(header);
        break;
      }
      default:
        break;
    }
  }
}

int main() {
  int portno;
  char *serverip;
  char *serverport;
  cmu_socket_t socket;

  serverip = getenv("server15441");
  if (!serverip) {
    serverip = "10.0.1.1";
  }

  serverport = getenv("serverport15441");
  if (!serverport) {
    serverport = "15441";
  }
  portno = (uint16_t)atoi(serverport);

  if (cmu_socket(&socket, TCP_LISTENER, portno, serverip) < 0) {
    exit(EXIT_FAILURE);
  }

  // 不成功就会一直尝试
  TCP_handshake_server(&socket);

  functionality(&socket);

  if (cmu_close(&socket) < 0) {
    exit(EXIT_FAILURE);
  }

  return EXIT_SUCCESS;
}
