#ifndef TERMINATOR_IPC_H
#define TERMINATOR_IPC_H

#include "common.h"

typedef enum {
    IPC_MSG_EXEC = 1,     /* GUI -> Worker: execute command string */
    IPC_MSG_INT = 2,      /* GUI -> Worker: SIGINT interrupt */
    IPC_MSG_QUIT = 3,     /* GUI -> Worker: terminate worker */
    IPC_MSG_OUTPUT = 4,   /* Worker -> GUI: output bytes */
    IPC_MSG_PROMPT = 5,   /* Worker -> GUI: command finished, payload: int exit_status */
    IPC_MSG_CWD = 6,      /* Worker -> GUI: current directory changed, payload: path */
    IPC_MSG_CLEAR = 7,    /* Worker -> GUI: clear screen */
    IPC_MSG_EXIT = 8      /* Worker -> GUI: exit command requested */
} IPCMsgType;

typedef struct {
    uint32_t type;
    uint32_t length;
} IPCHeader;

bool ipc_send_msg(int fd, IPCMsgType type, const void *payload, uint32_t len);
bool ipc_recv_header(int fd, IPCHeader *hdr);
bool ipc_recv_payload(int fd, void *buf, uint32_t len);

#endif /* TERMINATOR_IPC_H */
