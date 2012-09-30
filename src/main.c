#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sys/queue.h>
#include <ao/ao.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <mpg123.h>

/* send from streamer to identify sockets */
const char *IDENT_STREAM_SOCK = "0x23231412";
const char *IDENT_CMD_SOCK  = "0x41235322";

/* send to streamer to tell we are done */
const char *REMOTE_CMD_DONE = "0x32423423";

#define OUT_BUF_SIZE 32768

typedef struct thread_args_s {
    mpg123_handle *mh;
    int stream_socket;
} thread_args_t;

typedef int (*cmd_callback_t)(char *, int);

typedef struct cmd_table_s {
    char *cmd;
    cmd_callback_t cb;
} cmd_table_t;

static int g_go_on;
static int g_cmd_sock;
static int g_busy;
static int g_stop;

static fd_set g_read_master;

static void
close_cmd_sock()
{
    if (g_cmd_sock != -1) {
        fprintf(stderr, "close_cmd_sock()\n");
        close(g_cmd_sock);
        FD_CLR(g_cmd_sock, &g_read_master);
        g_cmd_sock = -1;
    }
}

static int
handle_stop_command(char *buffer, int buf_len)
{
    /* shut up compiler */
    (void)buffer;
    (void)buf_len;
    
    fprintf(stderr, "stop playing\n");

    /* guess i wont need to lock this */
    g_stop = 1;

    return 0;
}

static void
handle_command(char *buffer, int buf_len)
{
    int i;
    int len;
    static const cmd_table_t cmds[] = {
        { "STOP", &handle_stop_command }
    };
    for (i = 0; i < sizeof(cmds) / sizeof(cmd_table_t); ++i) {
        len = sizeof(cmds[i].cmd);
        if (buf_len >= len && strncmp(buffer, cmds[i].cmd, len) == 0) {
            cmds[i].cb(buffer + len, buf_len - len);
        }
    }
}

static int
make_socket_nonblock(int fd)
{
    int x;
    x = fcntl(fd, F_GETFL, 0);
    if (x < 0) {
        return x;
    }
    return fcntl(fd, F_SETFL, x | O_NONBLOCK);
}

static int
stream_loop(mpg123_handle *mh, int socket)
{
    ao_device *dev;
    ao_sample_format format;
    size_t bytes_decoded;
    int status;
    unsigned char out_buffer[OUT_BUF_SIZE];
    int channels;
    int encoding;
    long rate;

    /* listen to socket */
    if (mpg123_open_fd(mh, socket) == MPG123_ERR) {
        fprintf(stderr, "%s\n", mpg123_strerror(mh));
        return 1;
    }

    status = MPG123_OK;

    /* set some standard format */
    memset(&format, 0, sizeof(ao_sample_format));
    format.rate = 44100;
    format.channels = 2;
    format.byte_format = AO_FMT_LITTLE;
    format.bits = 16;
    dev = ao_open_live(ao_default_driver_id(), &format, NULL);
    if (!dev) {
        fprintf(stderr, "ao_open_live failed()");
        return 1;
    }

    do {
        status = mpg123_read(mh, out_buffer, OUT_BUF_SIZE, &bytes_decoded);
        if (status != 0) {
            fprintf(stderr, "status [%d] - %s\n", status, mpg123_plain_strerror(status));
        }
        if (status == MPG123_ERR || status == MPG123_DONE) {
            break;
        } else { 
            if (status == MPG123_NEW_FORMAT) {
                status = mpg123_getformat(mh, &rate, &channels, &encoding);
                fprintf(stderr, "new format:\n\trate: %ld\n\tchannels:%d\n\tencoding: %d\n",
                        rate, channels, encoding);
                format.rate = rate;
                format.channels = channels;
                format.byte_format = AO_FMT_LITTLE;
                format.bits = mpg123_encsize(encoding) * 8;
                /* close and reopen again for new format */
                ao_close(dev);
                dev = ao_open_live(ao_default_driver_id(), &format, NULL);
                if (!dev) {
                    fprintf(stderr, "ao_open_live failed\n");
                    break;
                }
            }
            if (dev) {
                ao_play(dev, (char*)out_buffer, bytes_decoded);
            }
        }
    } while (g_go_on && !g_stop);

    if (dev) {
        fprintf(stderr, "stderr close ao device\n");
        ao_close(dev);
    }
    return status;
}

static void*
stream_thread_main(void *args)
{
    int status;
    thread_args_t *thread_args;
    
    fprintf(stderr, "start streaming thread\n");
    g_busy = 1;
    g_stop = 0;

    thread_args = (thread_args_t*)args;
    /* go into loop which reads from connection and parses it to dev */
    status = stream_loop(thread_args->mh, thread_args->stream_socket);
    
    fprintf(stderr, "close stream_thread\n");
    fprintf(stderr, "%s\n", mpg123_plain_strerror(status));
    
    close(thread_args->stream_socket);
    write(g_cmd_sock, REMOTE_CMD_DONE, strlen(REMOTE_CMD_DONE));
    close_cmd_sock(); 
    free(thread_args);
    g_busy = 0;
    return NULL;
}

static void
handle_sigint(int sig)
{
    fprintf(stderr, "signal %d received\n", sig);
    g_go_on = 0;
}

int 
main(int argc, char **argv)
{
    int sockfd;
    int newfd;
    int portno;
    int ready;
    fd_set read_set;
    socklen_t socklen;
    struct sockaddr_in serv_addr;
    struct sockaddr_in cli_addr;
    thread_args_t *thread_args;
    pthread_t stream_thread;
    mpg123_handle *mh;
    char buffer[32];
    int bytes_read;

    sockfd = -1;
    newfd = -1;
    stream_thread = -1;
    mpg123_init();
    ao_initialize();

    mh = mpg123_new(NULL, NULL);
    if (!mh) {
        fprintf(stderr, "error mpg123_new()\n");
        goto cleanup;
    }

    mpg123_param(mh, MPG123_VERBOSE, 2, 0);

    signal(SIGCHLD, SIG_IGN);
    signal(SIGINT, handle_sigint);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        goto cleanup;
    }
    memset(&serv_addr, 0, sizeof(serv_addr));
    portno = atoi(argv[1]);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);
    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        goto cleanup;
    }

    if (listen(sockfd, 5) < 0) {
        perror("listen");
        goto cleanup;
    }
    socklen = sizeof(cli_addr);
    g_go_on = 1;
    g_cmd_sock = -1;
    g_busy = 0;
    g_stop = 0;
    FD_ZERO(&g_read_master);
    FD_SET(sockfd, &g_read_master);
    while (g_go_on) {
        read_set = g_read_master;
        ready = select(10, &read_set, NULL, NULL, NULL);
        if (ready < 0) {
            perror("select!!!!");
            g_go_on = 0;
        } else if (ready > 0) {
            if (FD_ISSET(sockfd, &read_set)) {
                newfd = accept(sockfd, (struct sockaddr *)&cli_addr, &socklen);
                if (newfd < 0) {
                    perror("accept");
                } else {
                    if (g_busy) {
                        fprintf(stderr, "accept: busy\n");
                        close(newfd);
                    } else {
                        bytes_read = read(newfd, buffer, sizeof(buffer));
                        if (bytes_read > 0) {
                            if (bytes_read >= strlen(IDENT_STREAM_SOCK) &&
                                strncmp(buffer, 
                                        IDENT_STREAM_SOCK,
                                        strlen(IDENT_STREAM_SOCK)) == 0) {
                                fprintf(stderr, "stream sock %d connected\n", newfd);
                                thread_args = malloc(sizeof(thread_args_t));
                                thread_args->mh = mh;
                                thread_args->stream_socket = newfd;
                                if (stream_thread != -1) {
                                    pthread_detach(stream_thread);
                                    stream_thread = -1;
                                }
                                if (pthread_create(&stream_thread, 
                                                   NULL, 
                                                   stream_thread_main,
                                                   thread_args) < 0) {
                                    free(thread_args);
                                    perror("pthread create");
                                    break;
                                }
                            } else if (bytes_read >= strlen(IDENT_CMD_SOCK) &&
                                       strncmp(buffer, 
                                               IDENT_CMD_SOCK,
                                               strlen(IDENT_CMD_SOCK)) == 0) {
                                make_socket_nonblock(newfd);
                                FD_SET(newfd, &g_read_master);
                                g_cmd_sock = newfd;
                                fprintf(stderr, "cmd sock %d connected\n", newfd);
                            }
                        }
                    }
                }
            }
            if (FD_ISSET(g_cmd_sock, &read_set)) {
                memset(buffer, 0, sizeof(buffer));
                bytes_read = read(g_cmd_sock, buffer, sizeof(buffer));
                if (bytes_read <= 0) {
                    fprintf(stderr, "read() - lost cmd_sock\n");
                    close_cmd_sock();
                    if (bytes_read != 0) {
                        perror("cmd_sock read");
                    }
                } else {
                    handle_command(buffer, bytes_read);
                }
            }
        }
    }
    fprintf(stderr, "shutdown... waiting for thread\n");

    if (stream_thread != -1 ) {
        pthread_join(stream_thread, NULL);
        pthread_detach(stream_thread);
    }
    
    fprintf(stderr, "shutdown\n");

cleanup:
    if (g_cmd_sock != -1) {
        close_cmd_sock();
    }
    if (sockfd != -1) {
        close(sockfd);
    }
    
    if (mh) {
        fprintf(stderr, "close mpg123\n");
        mpg123_close(mh);
        mpg123_delete(mh);
    }
    mpg123_exit();

    fprintf(stderr, "shutdown ao\n");
    ao_shutdown();

    return 0;
}
