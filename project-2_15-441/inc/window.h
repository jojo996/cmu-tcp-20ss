#ifndef PROJECT_2_15_441_INC_WINDOW_H_
#define PROJECT_2_15_441_INC_WINDOW_H_

#include "cmu_tcp.h"

int slide_window_init(window_t *win,  cmu_socket_t *sock);
void slide_window_activate(window_t *win, cmu_socket_t *sock);
void slide_window_check_for_data(window_t * win, cmu_socket_t *sock, int flags);
static void slide_window_handle_message(window_t * win, cmu_socket_t *sock, char* pkt);
void slide_window_send(window_t *win, cmu_socket_t *sock);
void slide_window_check_for_data(window_t * win, cmu_socket_t *sock, int flags);
static void slide_window_handle_message(window_t * win, cmu_socket_t *sock, char* pkt);
static int copy_string_from_buffer(window_t *win, SWPSeq idx, char *data, int max_len);
static void copy_string_to_buffer(window_t *win, cmu_socket_t* sock);

#endif