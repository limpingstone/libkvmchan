/**
 * Copyright 2018-2019 Shawn Anastasio
 *
 * This file is part of libkvmchan.
 *
 * libkvmchan is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libkvmchan is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libkvmchan.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * This file contains an implementation of the API exported to
 * applications on the current domain that link against libkvmchan.
 *
 * Most libkvmchan functions will simply forward their arguments
 * to an endpoint in this API and the request will be handled
 * in the daemon.
 */

#include <string.h>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/epoll.h>

#include "util.h"
#include "ipc.h"
#include "config.h"
#include "libkvmchan-priv.h"

struct localhandler_data {
    int epoll_fd;
    struct vec_int clients;
};

bool g_is_dom0;

static void cleanup_socket_path(void *path_) {
    const char *path = path_;
    unlink(path);
}

/**
 * Handle IPC mesages from other kvmchand processes
 */
static void handle_ipc_message(struct ipc_message *msg) {

}

static ssize_t localmsg_send(int socfd, void *data, size_t len, int fds[KVMCHAND_FD_MAX], uint8_t fd_count) {
    union {
        char cmsgbuf[CMSG_SPACE(sizeof(int) * KVMCHAND_FD_MAX)];
        struct cmsghdr align;
    } u;

    struct cmsghdr *cmsg;
    struct iovec iov = { .iov_base = data, .iov_len = len };
    struct msghdr msg = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_flags = 0,
        .msg_control = fds ? u.cmsgbuf : NULL,
        .msg_controllen = fds ? CMSG_LEN(sizeof(int) * fd_count) : 0,
    };

    /* Initialize the control message to hand off the fds */
    if (fds) {
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);

        for (uint8_t i=0; i<fd_count; i++) {
            ((int *)CMSG_DATA(cmsg))[i] = fds[i];
        }
    }

    return sendmsg(socfd, &msg, 0);
}

static ssize_t localmsg_recv(int socfd, void *buf, size_t len, int fds_out[KVMCHAND_FD_MAX]) {
    ssize_t s;
    union {
        char cmsgbuf[CMSG_SPACE(sizeof(int) * KVMCHAND_FD_MAX)];
        struct cmsghdr align;
    } u;

    struct cmsghdr *cmsg;
    struct iovec iov = { .iov_base = buf, .iov_len = len };
    struct msghdr msg = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_flags = 0,
        .msg_control = u.cmsgbuf,
        .msg_controllen = CMSG_LEN(sizeof(int) * KVMCHAND_FD_MAX)
    };

    if ((s = recvmsg(socfd, &msg, 0)) < 0)
        return s;

    if (fds_out) {
        cmsg = CMSG_FIRSTHDR(&msg);
        if (msg.msg_controllen < CMSG_LEN(sizeof(int))) {
            fds_out[0] = -1;
            goto out;
        }

        // Copy all fds to fds_out
        uint8_t *in = CMSG_DATA(cmsg);
        uint8_t *max = in + msg.msg_controllen;
        uint8_t i=0;
        while (in + sizeof(int) <= max && i < KVMCHAND_FD_MAX) {
            fds_out[i++] = *(int *)in;
            in += sizeof(int);
        }

        // Set any remaining fds to -1
        for (; i<KVMCHAND_FD_MAX; i++) {
            fds_out[i] = -1;
        }
    }

out:
    return s;
}

/**
 * Forward messages to dom 0 over VFIO
 */
static bool defer_client_message(struct kvmchand_message *msg, struct kvmchand_ret *ret) {
    // Pack the request into an IPC message and send to VFIO
    struct ipc_message ipc_resp, ipc_msg = {
        .type = IPC_TYPE_CMD,
        .cmd = {
            .command = VFIO_IPC_CMD_FORWARD_KVMCHAND_MSG,
            .args = {
                msg->command,
                msg->args[0],
                msg->args[1],
                msg->args[2],
                msg->args[3],
            },
        },
        .dest = IPC_DEST_VFIO,
        .flags = IPC_FLAG_WANTRESP
    };

    if (!ipc_send_message(&ipc_msg, &ipc_resp)) {
        log(LOGL_ERROR, "Unable to send IPC message to VFIO: %m.");
        return false;
    }

    ret->ret = ipc_resp.resp.ret;
    ret->error = ipc_resp.resp.error;
    return true;
}

static bool handle_client_message_guest(int fd, struct kvmchand_message *msg) {
    struct kvmchand_ret ret = { .error = true };
    int fds[KVMCHAND_FD_MAX];

    /**
     * If we're not in dom 0, the only way we can meaningfully service
     * requests is to defer them to the kvmchand server on dom0.
     */
    /*
    if (!g_is_dom0) {
        defer_client_message(msg, &ret);
        goto out;
    } */

    switch(msg->command) {
        case KVMCHAND_CMD_HELLO:
            ret.ret = KVMCHAND_API_VERSION;
            ret.error = false;
            break;

        case KVMCHAND_CMD_SERVERINIT:
        case KVMCHAND_CMD_CLIENTINIT:
            // Defer the command to dom0 so that the connection can be made.
            // Since it takes some time for the ivshmem device to show up,
            // don't immediately obtain the file descriptors. The user can
            // submit another call to obtain them.
            defer_client_message(msg, &ret);
            break;

        case KVMCHAND_CMD_GET_CONN_FDS:
        {
            // Get file descriptors
            uint32_t server_ivpos = (uint32_t)msg->args[0];
            struct ipc_message ipc_resp, ipc_msg = {
                .type = IPC_TYPE_CMD,
                .cmd = {
                    .command = VFIO_IPC_CMD_GET_CONN_FDS,
                    .args = {
                        server_ivpos,
                    },
                },
                .dest = IPC_DEST_VFIO,
                .flags = IPC_FLAG_WANTRESP
            };

            if (!ipc_send_message(&ipc_msg, &ipc_resp))
                break;

            if (ipc_resp.resp.error)
                break;

            log(LOGL_INFO, "fds: {%d, %d, %d, %d, %d}", ipc_resp.fds[0], ipc_resp.fds[1], ipc_resp.fds[2], ipc_resp.fds[3], ipc_resp.fds[4]);
            ret.fd_count = 5;
            memcpy(fds, ipc_resp.fds, ret.fd_count * sizeof(int));

            // Forward response to client
            ret.error = ipc_resp.resp.error;
            ret.ret = ipc_resp.resp.ret;

            break;
        }

        default:
            log(LOGL_WARN, "Warning, unknown command from client fd %d. Ignoring.", fd);
    }

    bool res = false;
    if (localmsg_send(fd, &ret, sizeof(ret), ret.fd_count > 0 ? fds : NULL, ret.fd_count) < 0)
        log(LOGL_ERROR, "Failed to send response to client fd %d.", fd);
    else
        res = true;

    // Close any fds
    for (size_t i=0; i<ret.fd_count; i++) {
        close(fds[i]);
    }

    return res;
}

static bool handle_client_message_dom0(int fd, struct kvmchand_message *msg) {
    struct kvmchand_ret ret = { .error = true };
    int fds[KVMCHAND_FD_MAX];

    switch(msg->command) {
        case KVMCHAND_CMD_HELLO:
            ret.ret = KVMCHAND_API_VERSION;
            ret.error = false;
            break;

        case KVMCHAND_CMD_SERVERINIT:
        {
            // Forward message to MAIN over IPC
            struct ipc_message ipc_msg = {
                .type = IPC_TYPE_CMD,
                .cmd = {
                    .command = MAIN_IPC_CMD_VCHAN_INIT,
                    .args = {
                        0,
                        msg->args[0],
                        msg->args[1],
                        msg->args[2],
                        msg->args[3],
                    }
                },
                .dest = IPC_DEST_MAIN,
                .flags = IPC_FLAG_WANTRESP
            };

            struct ipc_message ipc_resp;
            if (!ipc_send_message(&ipc_msg, &ipc_resp))
                goto out;
            if (ipc_resp.resp.error)
                goto out;

            // Get all file descriptors for the connection
            ipc_msg.cmd.command = IVSHMEM_IPC_CMD_GET_CONN_FDS;
            ipc_msg.cmd.args[0] = (pid_t)ipc_resp.resp.ret2;
            ipc_msg.cmd.args[1] = (uint32_t)ipc_resp.resp.ret;
            ipc_msg.dest = IPC_DEST_IVSHMEM;
            if (!ipc_send_message(&ipc_msg, &ipc_resp))
                goto out;
            if (ipc_resp.resp.error) {
                log(LOGL_ERROR, "Failed to get fds for vchan!");
                goto out;
            }

            ret.fd_count = 5;
            memcpy(fds, ipc_resp.fds, ret.fd_count * sizeof(int));

            // Forward response to client
            ret.error = ipc_resp.resp.error;
            ret.ret = ipc_resp.resp.ret;

            break;
        }

        case KVMCHAND_CMD_CLIENTINIT:
        {
            // Forward message to MAIN over IPC
            struct ipc_message ipc_msg = {
                .type = IPC_TYPE_CMD,
                .cmd = {
                    .command = MAIN_IPC_CMD_VCHAN_CONN,
                    .args = {
                        msg->args[0],
                        0,
                        msg->args[1],
                    }
                },
                .dest = IPC_DEST_MAIN,
                .flags = IPC_FLAG_WANTRESP
            };

            struct ipc_message ipc_resp;
            if (!ipc_send_message(&ipc_msg, &ipc_resp))
                goto out;
            if (ipc_resp.resp.error)
                goto out;

            // Get all file descriptors for the connection
            ipc_msg.cmd.command = IVSHMEM_IPC_CMD_GET_CONN_FDS;
            ipc_msg.cmd.args[0] = (pid_t)ipc_resp.resp.ret2;
            ipc_msg.cmd.args[1] = (uint32_t)ipc_resp.resp.ret;
            ipc_msg.dest = IPC_DEST_IVSHMEM;
            if (!ipc_send_message(&ipc_msg, &ipc_resp))
                goto out;
            if (ipc_resp.resp.error) {
                log(LOGL_ERROR, "Failed to get fds for vchan!");
                goto out;
            }

            ret.fd_count = 5;
            memcpy(fds, ipc_resp.fds, ret.fd_count * sizeof(int));

            // Forward response to client
            ret.error = ipc_resp.resp.error;
            ret.ret = ipc_resp.resp.ret;

            break;
        }

        default:
            log(LOGL_WARN, "Warning, unknown command from client fd %d. Ignoring.", fd);
    }

out:
    ;
    bool res = false;
    if (localmsg_send(fd, &ret, sizeof(ret), ret.fd_count > 0 ? fds : NULL, ret.fd_count) < 0)
        log(LOGL_ERROR, "Failed to send response to client fd %d.", fd);
    else
        res = true;

    // Close any fds
    for (size_t i=0; i<ret.fd_count; i++) {
        close(fds[i]);
    }

    return res;
}

/**
 * Handle messages from remote clients
 */
static bool handle_client_message(int fd, struct kvmchand_message *msg) {
    if (g_is_dom0)
        return handle_client_message_dom0(fd, msg);
    else
        return handle_client_message_guest(fd, msg);
}

static bool remove_connection(struct localhandler_data *data, int fd) {
    for (size_t i=0; i<data->clients.count; i++) {
        if (data->clients.data[i] == fd) {
            bool res = true;
            if (del_epoll_fd(data->epoll_fd, fd) < 0)
                res = false;
            vec_int_remove(&data->clients, i);
            close(fd);
            return res;
        }
    }

    return false;
}

void run_localhandler_loop(int mainsoc, bool is_dom0) {
    struct localhandler_data data;
    g_is_dom0 = is_dom0;

    if (!ipc_start(mainsoc, IPC_DEST_LOCALHANDLER, handle_ipc_message))
        goto error;

    // Set up the socket and bind it
    int socfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socfd < 0)
        goto error;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, LOCALHANDLER_SOCK_PATH, sizeof(addr.sun_path)-1);
    if (bind(socfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_un)) < 0)
        goto error;

    // Set proper permissions on the socket
    if (chmod(LOCALHANDLER_SOCK_PATH, 0666) < 0)
        goto error;

    // Install exit handler to remove socket
    if (!install_exit_callback(cleanup_socket_path, (void *)LOCALHANDLER_SOCK_PATH))
        goto error;

    // Start listening
    if (listen(socfd, 5) < 0)
        goto error;

    // Initialize epoll
    data.epoll_fd = epoll_create1(0);
    if (data.epoll_fd < 0)
        goto error;
    if (add_epoll_fd(data.epoll_fd, socfd, EPOLLIN) < 0)
        goto error;

    if (!vec_int_init(&data.clients, 10, NULL))
        goto error;

    // Poll for events
    struct epoll_event events[5];
    int event_count;
    for(;;) {
        event_count = epoll_wait(data.epoll_fd, events, ARRAY_SIZE(events), -1);
        for(int i=0; i<event_count; i++) {
            if (events[i].data.fd == socfd) {
                // Connection request on the socket, accept it
                int fd = accept(socfd, NULL, NULL);

                // Add connected fd to epoll
                if (add_epoll_fd(data.epoll_fd, fd, EPOLLIN) < 0) {
                    log(LOGL_WARN, "Failed to add fd to epoll set: %m.");
                    goto error;
                }

                if (!vec_int_push_back(&data.clients, fd)) {
                    log(LOGL_ERROR, "Failed to add fd to vec: %m.");
                    goto error;
                }

                log(LOGL_INFO, "Client connected! fd: %d", fd);
            } else {
                // Event from a client fd, handle it
                int fd = events[i].data.fd;
                struct kvmchand_message msg;
                ssize_t n = localmsg_recv(fd, &msg, sizeof(msg), NULL);
                if (n == 0) {
                    // Client disconnected
                    log(LOGL_INFO, "Client disconnected! fd: %d", fd);

                    if (!remove_connection(&data, fd))
                        log(LOGL_WARN, "Failed to delete client.");

                    continue;
                } else if (n != sizeof(msg)) {
                    log(LOGL_WARN, "Unexpected message received from client! Dropping client.");

                    if (!remove_connection(&data, fd))
                        log(LOGL_WARN, "Failed to delete client.");

                    continue;
                }

                // Valid message received, handle it
                if (!handle_client_message(fd, &msg))
                    if (!remove_connection(&data, fd))
                        log(LOGL_WARN, "Failed to delete client.");
           }
        }
    }

error:
    log(LOGL_ERROR, "localhandler encountered fatal error: %m!");
    bail_out();
}
