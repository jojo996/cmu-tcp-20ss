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
 * This file defines the API for the CMU TCP implementation.
 */

#ifndef PROJECT_2_15_441_INC_CMU_TCP_H_
#define PROJECT_2_15_441_INC_CMU_TCP_H_

#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "cmu_packet.h"
#include "grading.h"
#include "global.h"
//理论上只有server能调用，但是为了让client用上check_for_data所以写到这里来
// #include "backend.h"

#define EXIT_SUCCESS 0
#define EXIT_ERROR -1
#define EXIT_FAILURE 1
#define MAXSEQ 100
#define MAX_BUFFER_SIZE 1000000

/* 滑窗的下标 */
typedef int SWPSeq;  /* slide window protocol序列号 */

// 发送方状态
typedef enum {
	SS_DEFAULT = 1,   // 默认
	SS_TIME_OUT = 2,   // 超时
  SS_RESEND = 3,   /* 重发事件 */
	SS_SEND_OVER = 4,  // 当前数据发送完成
} send_state;

//滑窗接收单位，组织成链表形式
typedef struct window_slot {
	uint8_t recv_or_not;
	char *msg;
	struct window_slot *next; 
} recvQ_slot;

typedef struct {
  uint32_t next_seq_expected;
  uint32_t last_ack_received;
  uint32_t last_seq_received;
  pthread_mutex_t ack_lock;
  uint32_t adv_window;  //窗口大小，固定为WINDOW_INITIAL_WINDOW_SIZE
  uint32_t seq_expect;  //接收下一个包的seq
  uint32_t next_send_seq; //发送下一个的seq
  uint32_t dup_ack_num; //当前收到ack的数量
  char send_buffer[MAX_BUFFER_SIZE+1];  // 发送方缓冲

  send_state state; //发送方状态

  //发送方窗口 |----------LAR+++++++++LFS--------| 
	uint32_t LAR; 
	uint32_t LFS; 
  uint32_t DAT; // 数据的最大下标

  FILE *log;

  //超时控制
  struct timeval time_send;  /* 发送包的时间 */
  SWPSeq send_seq;
  uint8_t timer_flag;  // 时钟启用状态
  long TimeoutInterval;  // 超时时间 
	long EstimatedRTT;  // （加权）平均RTT时间
	long DevRTT;  // RTT偏差时间
} window_t;

/**
 * CMU-TCP socket types. (DO NOT CHANGE.)
 */
typedef enum {
  TCP_INITIATOR = 0,
  TCP_LISTENER = 1,
} cmu_socket_type_t;

/**
 * This structure holds the state of a socket. You may modify this structure as
 * you see fit to include any additional state you need for your implementation.
 */
typedef struct {
  int socket;
  pthread_t thread_id;
  uint16_t my_port;
  uint16_t their_port;
  struct sockaddr_in conn;
  uint8_t* received_buf;
  int received_len;
  pthread_mutex_t recv_lock;
  pthread_cond_t wait_cond;
  uint8_t* sending_buf;
  int sending_len;
  cmu_socket_type_t type;
  pthread_mutex_t send_lock;
  int dying;
  pthread_mutex_t death_lock;
  window_t window;
  TCP_State state;
} cmu_socket_t;

/*
 * DO NOT CHANGE THE DECLARATIONS BELOW
 */

/**
 * Read mode flags supported by a CMU-TCP socket.
 */
typedef enum {
  NO_FLAG = 0,  // Default behavior: block indefinitely until data is available. //无限期停止直到有数据
  NO_WAIT,      // Return immediately if no data is available.
  TIMEOUT,      // Block until data is available or the timeout is reached.
} cmu_read_mode_t;

/**
 * Constructs a CMU-TCP socket.
 *
 * An Initiator socket is used to connect to a Listener socket.
 *
 * @param sock The structure with the socket state. It will be initialized by
 *             this function.
 * @param socket_type Indicates the type of socket: Listener or Initiator.
 * @param port Port to either connect to, or bind to. (Based on socket_type.)
 * @param server_ip IP address of the server to connect to. (Only used if the
 *                 socket is an initiator.)
 *
 * @return 0 on success, -1 on error.
 */
int cmu_socket(cmu_socket_t* sock, const cmu_socket_type_t socket_type,
               const int port, const char* server_ip);

/**
 * Closes a CMU-TCP socket.
 *
 * @param sock The socket to close.
 *
 * @return 0 on success, -1 on error.
 */
int cmu_close(cmu_socket_t* sock);

/**
 * Reads data from a CMU-TCP socket.
 *
 * If there is data available in the socket buffer, it is placed in the
 * destination buffer.
 *
 * @param sock The socket to read from.
 * @param buf The buffer to read into.
 * @param length The maximum number of bytes to read.
 * @param flags Flags that determine how the socket should wait for data. Check
 *             `cmu_read_mode_t` for more information. `TIMEOUT` is not
 *             implemented for CMU-TCP.
 *
 * @return The number of bytes read on success, -1 on error.
 */
int cmu_read(cmu_socket_t* sock, void* buf, const int length,
             cmu_read_mode_t flags);

/**
 * Writes data to a CMU-TCP socket.
 *
 * @param sock The socket to write to.
 * @param buf The data to write.
 * @param length The number of bytes to write.
 *
 * @return 0 on success, -1 on error.
 */
int cmu_write(cmu_socket_t* sock, const void* buf, int length);

/*
 * You can declare more functions after this point if you need to.
 */

#endif  // PROJECT_2_15_441_INC_CMU_TCP_H_
