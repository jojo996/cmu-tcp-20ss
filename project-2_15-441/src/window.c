#include "cmu_tcp.h"
#include "window.h"
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>

#define RESEND_TIME 3

/* 返回timeval数据结构对应的微秒值 */
long get_timeval(struct timeval *time){
    return 1l*(time->tv_sec*1000000)+time->tv_usec;
}

/* 设置timeval的时间 */
void set_timeval(struct timeval *time, long interval){
    long int sec = interval / 1000000;
    long int usec = interval % 1000000;
    time->tv_sec = sec;
    time->tv_usec = usec;
}

/* 设置计时 */
void set_timer(int sec, int usec, void (*handler)(int)){
    /* 设置时间到达后的处理函数,注意这里占用了SIGALRM信号 */
    // printf("timer set\n");
    signal(SIGALRM,handler);
    struct itimerval itv;
    itv.it_interval.tv_sec=0;//自动装载，之后每10秒响应一次
    itv.it_interval.tv_usec=0;
    itv.it_value.tv_sec=sec;//第一次定时的时间
    itv.it_value.tv_usec=usec;
    setitimer(ITIMER_REAL,&itv,NULL);
}

/* 关闭计时 */
void unset_timer(){
    /* 设置回默认信号处理方式，也可以屏蔽信号 */
    // printf("unset timer\n");
    signal(SIGALRM,SIG_DFL);
    struct itimerval itv;
    itv.it_interval.tv_sec=0;//设置为0
    itv.it_interval.tv_usec=0;
    itv.it_value.tv_sec=0;
    itv.it_value.tv_usec=0;
    setitimer(ITIMER_REAL,&itv,NULL);
}

static void last_time_wait(int sig, void *ptr){
    static cmu_socket_t *sock;
    /* 首次调用初始化win参数 */
    if(sock == NULL){
        sock = (cmu_socket_t *)ptr;
        return;
    }
    else{
        /* 结束线程 */
        sock->state = TCP_CLOSED;
    }
}

/* 处理SIGALRM信号，即超时信号 */
static void time_out(int sig, void *ptr){
    static window_t *win;
    /* 首次调用初始化win参数 */
    if(win == NULL){
        win = (window_t *)ptr;
        return;
    }
    /* 正常的信号处理 */
    else{
        win->timer_flag = 0;
        win->state = SS_TIME_OUT;
        fprintf(stdout,"-------time out------------\n");
        fflush(stdout);
        unset_timer();
    }
}

/* 初始化一个滑窗 */
int slide_window_init(window_t *win,  cmu_socket_t *sock)
{
    win->adv_window = WINDOW_INITIAL_WINDOW_SIZE;
    win->seq_expect = win->last_seq_received;
    win->dup_ack_num = 0;
    win->LAR = 0;
    win->LFS = 0;
    win->DAT = 0;
    win->TimeoutInterval = 1000000;
    win->EstimatedRTT = 1000000;
    win->DevRTT = 0;
    win->state = SS_DEFAULT;
    /* 初始化信号处理函数，以便超时能够访问window */
    time_out(0,win);
    /* 初始化信号处理函数，以便超时能够访问socket */
    last_time_wait(0,sock);
    return EXIT_SUCCESS;
}

static void copy_string_to_buffer(window_t *win, cmu_socket_t* sock){
    char *data = sock->sending_buf;
    int len = sock->sending_len;
    int start = win->DAT % MAX_BUFFER_SIZE;
    int buf_len = len;
    if(start + len > MAX_BUFFER_SIZE){
        buf_len = MAX_BUFFER_SIZE - start;
        memcpy(win->send_buffer+start,data,buf_len);
    }
    else{
        memcpy(win->send_buffer+start,data,buf_len);
    }
    sock->sending_len -= buf_len;
    if(sock->sending_len != 0){
        char *buf = malloc(sock->sending_len);
        memcpy(buf,data+buf_len,sock->sending_len);
        free(sock->sending_buf);
        sock->sending_buf = buf;
    }
    else{
        free(sock->sending_buf);
        sock->sending_buf = NULL;
    }
    win->DAT += buf_len;
}

static int copy_string_from_buffer(window_t *win, SWPSeq idx, char *data, int max_len){
    idx = idx % MAX_BUFFER_SIZE;
    int len = min(win->DAT-idx,max_len);
    int start = idx % MAX_BUFFER_SIZE;
    if(start + len > MAX_BUFFER_SIZE){
        int temp = MAX_BUFFER_SIZE-start;
        memcpy(data,win->send_buffer+start, temp);
        memcpy(data+temp,win->send_buffer,len-temp);
    }
    else{
        memcpy(data,win->send_buffer+start, len);
    }
    return len;
}

void slide_window_activate(window_t *win, cmu_socket_t *sock)
{
    int buf_len = sock->sending_len;
    if(buf_len > 0 && (win->DAT == win->LAR)){
        copy_string_to_buffer(win,sock);
    }
    //仍有数据要发送
    if(win->DAT > win->LAR){
        slide_window_send(win,sock);
    }
    //关闭滑窗前发送最后一个数据包
    if(sock->state == TCP_CLOSE_WAIT){
        char *rsp = create_packet(sock->my_port, sock->their_port, 
                sock->window.last_ack_received,
                sock->window.last_seq_received, 
                DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK|FIN_FLAG_MASK,
                        win->adv_window, 0, NULL, NULL, 0);
        sendto(sock->socket, rsp, DEFAULT_HEADER_LEN, 0, 
                    (struct sockaddr*) &(sock->conn), sizeof(sock->conn));
        free(rsp);
        sock->state = TCP_LAST_ACK;
    }
}

void slide_window_send(window_t *win, cmu_socket_t *sock)
{
	char* msg;
    char *data;
    int plen;
	size_t conn_len = sizeof(sock->conn);
    /* 当前发送的序列号 */
	uint32_t ack;
    /* 当前打包的seq */
    uint32_t mkpkt_seq = win->last_ack_received;
    int buf_len,adv_len = MAX_LEN;

    ack = win->last_seq_received;

    /* 如果数据已经全部发送了，等待ACK或者超时 */
    if((win->DAT == win->LFS)&&(win->state == SS_DEFAULT)){
        win->state = SS_SEND_OVER;
        sleep(1);
    }
    switch(win->state)
    {
        case SS_DEFAULT:  /* 正常发包 */
            data = (char *)malloc(win->adv_window);
            msg = create_packet(sock->my_port, sock->their_port, 
                win->last_ack_received, win->last_seq_received, 
                DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN + win->adv_window, NO_FLAG, win->adv_window, 0, NULL,
                data, win->adv_window);
            sendto(sock->socket, msg, DEFAULT_HEADER_LEN + win->adv_window, 0, (struct sockaddr*) &(sock->conn), conn_len);
            free(msg);
            free(data);
            msg = NULL;
            data = NULL;
            // 如果没有设置计时器，设置时钟
            if(!win->timer_flag){
                set_timer(win->TimeoutInterval/1000000,win->TimeoutInterval%1000000,(void (*)(int))time_out);
                win->timer_flag = 1;
            }
            /* 发送指针后移 */
            win->LFS += win->adv_window;
            break;
         case SS_RESEND:  /* 马上重发 */
            /* 如果缓存中有数据 */
            if(win->LFS > win->LAR){
                buf_len = win->LFS - win->LAR;
                /* 如果包太长,分多次发送,每次只发送最大包长 */
                buf_len = (buf_len <= adv_len)?buf_len:adv_len;
                int plen = DEFAULT_HEADER_LEN + buf_len;
                data = (char *)malloc(buf_len);
                copy_string_from_buffer(win,win->LAR,data,buf_len);
                mkpkt_seq = win->last_ack_received;
                msg = create_packet_buf(sock->my_port, sock->their_port, 
                    mkpkt_seq, ack, 
                    DEFAULT_HEADER_LEN, plen, NO_FLAG, win->adv_window, 0, NULL,
                    data, buf_len);
                /* 发送UDP的包 */
                sendto(sock->socket, msg, plen, 0, (struct sockaddr*) &(sock->conn), conn_len);
                free(msg);
                free(data);
                msg = NULL;
                data = NULL;
                unset_timer();
                set_timer(win->TimeoutInterval/1000000,win->TimeoutInterval%1000000,(void (*)(int))time_out);
                win->timer_flag = 1;
                /* 设置发送时间 */
                if(win->send_seq == -1){
                    win->send_seq = mkpkt_seq + buf_len;
                    gettimeofday(&win->time_send,NULL);
                }
            }
            win->state = SS_DEFAULT;
            break;
        case SS_TIME_OUT:  // 超时
            if(win->LFS > win->LAR){
                buf_len = win->LFS - win->LAR;
            data = (char *)malloc(win->adv_window);
            msg = create_packet_buf(sock->my_port, sock->their_port, 
                    mkpkt_seq, ack, 
                    DEFAULT_HEADER_LEN, plen, NO_FLAG, win->adv_window, 0, NULL,
                    data, buf_len);
            sendto(sock->socket, msg, DEFAULT_HEADER_LEN + win->adv_window, 0, (struct sockaddr*) &(sock->conn), conn_len);
            free(msg);
            free(data);
            msg = NULL;
            data = NULL;
            unset_timer();
            set_timer(win->TimeoutInterval/1000000,win->TimeoutInterval%1000000,(void (*)(int))time_out);
            win->timer_flag = 1;
            }
            win->state = SS_DEFAULT;
            break;
        
        case SS_SEND_OVER:
            win->state = SS_DEFAULT;
            break;
        default:
            break;
    }
    printf("slide window send over\n");
}

void slide_window_check_for_data(window_t * win, cmu_socket_t *sock, int flags)
{
    char hdr[DEFAULT_HEADER_LEN];
	socklen_t conn_len = sizeof(sock->conn);
	ssize_t len = 0;
	uint32_t plen = 0, buf_size = 0, n = 0;
    char *pkt;
	/* fd_set for select and pselect */
	fd_set ackFD;
	/* 3秒的超时时间 */
	struct timeval time_out;
	time_out.tv_sec = 1;
	time_out.tv_usec = 0;
	while(pthread_mutex_lock(&(sock->recv_lock)) != 0);
	switch(flags){
		case NO_FLAG:  // 堵塞直到数据收到，待确认
			len = recvfrom(sock->socket, hdr, DEFAULT_HEADER_LEN, MSG_PEEK,
								(struct sockaddr *) &(sock->conn), &conn_len);
			break;
		case TIMEOUT:
			// 设置非堵塞等待 
			FD_ZERO(&ackFD);
			FD_SET(sock->socket, &ackFD);
			// 等待设定时间直到socket收到信息，如果时间内没有返回则break 
			if(select(sock->socket+1, &ackFD, NULL, NULL, &time_out) <= 0){
				break;
			}
		case NO_WAIT:
			len = recvfrom(sock->socket, hdr, DEFAULT_HEADER_LEN, MSG_DONTWAIT | MSG_PEEK,
							 (struct sockaddr *) &(sock->conn), &conn_len);
			break;
		default:
			perror("ERROR unknown flag");
	}
    if(len < DEFAULT_HEADER_LEN){
        // 暂时没有收到有效数据
        // printf("###recv data error %d...\n",(int)len);
    }
    if(len >= DEFAULT_HEADER_LEN){
        plen = get_plen(hdr);
        pkt = malloc(plen);
        // 直到包的信息全部收到
        while(buf_size < plen ){
            n = recvfrom(sock->socket, pkt + buf_size, plen - buf_size, 
                    NO_FLAG, (struct sockaddr *) &(sock->conn), &conn_len);
            buf_size = buf_size + n;
        }
        slide_window_handle_message(win,sock,pkt);
    }
    pthread_mutex_unlock(&(sock->recv_lock));
}

static void slide_window_handle_message(window_t * win, cmu_socket_t *sock, char* pkt)
{
    char* rsp;
	uint8_t flags = get_flags(pkt);
	uint32_t data_len, seq, ack, adv_win;
	socklen_t conn_len = sizeof(sock->conn);
    ack = get_ack(pkt);
    seq = get_seq(pkt);
    adv_win = MAX_NETWORK_BUFFER;
	switch(flags){
		case ACK_FLAG_MASK: 
            // 处理四次挥手事件 
            if(sock->state == TCP_FIN_WAIT1){
                if(win->last_ack_received < ack){
                    win->last_ack_received = ack; 
                }
                if(win->last_seq_received < seq){
                    win->last_seq_received = seq;
                }
                sock->state = TCP_FIN_WAIT2;
            }
            // 处理四次挥手事件 
            if(sock->state == TCP_LAST_ACK){
                if(win->last_ack_received < ack){
                    win->last_ack_received = ack; 
                }
                if(win->last_seq_received < seq){
                    win->last_seq_received = seq;
                }
                sock->state = TCP_CLOSED;
            }
            // 如果ack的值为期待的 
			if(ack > win->last_ack_received)
            {  
				win->last_ack_received = ack; 
                if(win->last_seq_received < seq){
                    win->last_seq_received = seq;
                    win->seq_expect = seq;
                }
                // 提醒发送者可以发送了
                win->state = SS_DEFAULT;
                /* 重置计时器 */
                unset_timer();               
            }
            else{  /* 收到错序的ACK */
                win->dup_ack_num++;
                if(win->dup_ack_num == 3){
                    resend(win);
                }
            }
            break;
        case FIN_FLAG_MASK:{/* 包含FIN */
            if(win->last_ack_received < ack){
                win->last_ack_received = ack; /* 设置为新值 */
            }
            if(win->last_seq_received < seq){
                win->last_seq_received = seq;
            }
            sock->window.last_seq_received++;
            rsp = create_packet_buf(sock->my_port, sock->their_port, 
                sock->window.last_ack_received,
                sock->window.last_seq_received, 
                DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK,
                        win->adv_window, 0, NULL, NULL, 0);
            sendto(sock->socket, rsp, DEFAULT_HEADER_LEN, 0, 
                    (struct sockaddr*) &(sock->conn), sizeof(sock->conn));
            free(rsp);
            sock->state = TCP_CLOSE_WAIT;
            break;
        } 
        case FIN_FLAG_MASK|ACK_FLAG_MASK:{/* 包含FIN */
            if(win->last_ack_received < ack){
                win->last_ack_received = ack; /* 设置为新值 */
            }
            if(win->last_seq_received < seq){
                win->last_seq_received = seq;
            }
            sock->window.last_seq_received++;
            rsp = create_packet_buf(sock->my_port, sock->their_port, 
                sock->window.last_ack_received,
                        sock->window.last_seq_received, 
                DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK,
                        win->adv_window, 0, NULL, NULL, 0);
            sendto(sock->socket, rsp, DEFAULT_HEADER_LEN, 0, 
                    (struct sockaddr*) &(sock->conn), sizeof(sock->conn));
            free(rsp);
            /* 通知上层可以读取数据,打破上层读取的循环 */
            pthread_cond_signal(&(sock->wait_cond));  
            sock->state = TCP_TIME_WAIT;
            /* 启动TIME WAIT */
            set_timer(win->TimeoutInterval/1000000,win->TimeoutInterval%1000000,(void (*)(int))last_time_wait);
            break;
        } 
        //不含ACK？？？
		// default:
        //     printf("# recv data\n");
        //     /* 如果收到的是期待的包 */
		// 	if(seq == win->seq_expect){
        //         /* seq可能小于上一个包的值 */
        //         if(win->last_ack_received < ack){
        //             win->last_ack_received = ack; /* 设置为新值 */
        //         }
        //         if(sock->received_len == MAX_RECV_SIZE){
        //             break;
        //         }
        //         data_len = get_plen(pkt) - DEFAULT_HEADER_LEN;
        //         adv_win = deliver_data(sock,pkt,data_len);
        //         win->last_seq_received = seq;
        //         win->seq_expect = (win->seq_expect + data_len)%MAX_SEQ_NUM;
        //         slot = win->recv_buffer_header.next;
        //         prev = &win->recv_buffer_header;
        //         /* 由于缓冲区可能已经储存了错序的pkt，所以直接复用那些pkt */
        //         while((slot != NULL) && (win->seq_expect == get_seq(slot->msg))){
        //             /* 把正确顺序的包发给上层 */
        //             data_len = get_plen(slot->msg) - DEFAULT_HEADER_LEN;
        //             adv_win = deliver_data(sock,slot->msg,data_len);
        //             win->last_seq_received = get_seq(slot->msg);
        //             win->seq_expect = (win->seq_expect + data_len)%MAX_SEQ_NUM;
        //             prev->next = slot->next;
        //             free(slot->msg);
        //             free(slot);
        //         }
        //         win->last_seq_received = win->seq_expect;
        //         // fprintf(stdout,"handle data: [%d(lar),%d(seqexp)]\n",win->last_ack_received, win->seq_expect);
        //         win->adv_window = MAX_RECV_SIZE - sock->received_len;
        //         /* 发送只有头部的包（ACK） */
        //         rsp = create_packet_buf(sock->my_port, ntohs(sock->conn.sin_port),
        //             win->last_ack_received, win->seq_expect, 
        //             DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, win->adv_window/*adv_win*/, 0, NULL, NULL, 0);
        //         /* 发送ACK确认包 */
        //         sendto(sock->socket, rsp, DEFAULT_HEADER_LEN, 0, (struct sockaddr*) 
        //             &(sock->conn), conn_len);
        //         free(rsp);
                
        //         /* 通知上层可以读取数据 */
        //         pthread_cond_signal(&(sock->wait_cond));  
        //     }
        //     /* 如果是错序的包 */
        //     else{
        //         // printf("handle: not accept msg(%d(seq),%d(exp))\n",seq,win->seq_expect);
        //         // fflush(stdout);
        //         seq = get_seq(pkt);
        //         /* 将错序包插入队列中 */
        //         insert_pkt_into_linked_list(&win->recv_buffer_header,pkt);
        //         /* 发送ACK */
        //         rsp = create_packet_buf(sock->my_port, ntohs(sock->conn.sin_port),
        //             win->last_ack_received, win->seq_expect, 
        //             DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, win->my_adv_window, 0, NULL, NULL, 0);
        //         sendto(sock->socket, rsp, DEFAULT_HEADER_LEN, 0, 
        //             (struct sockaddr*)&(sock->conn), conn_len);
        //         free(rsp);
        //     }
		// 	break;
	}
}

static void resend(window_t * win){
    win->state = SS_RESEND;
    fprintf(win->log,"resending.......done\n");
}

/* 关闭滑窗 */
void slide_window_close(window_t *win){
    fclose(win->log);
    signal(SIGALRM,SIG_DFL);
}
