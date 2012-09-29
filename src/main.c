#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <ao/ao.h>
#include <mpg123.h>
#include "connection.h"

#define OUT_BUF_SIZE 32768

typedef struct thread_args_s {
    char *ip;
    int port;
} thread_args_t;

static int g_go_on;

static connection_t *g_connection;

static int
stream_loop(mpg123_handle *mh)
{
    ao_device *dev;
    ao_sample_format format;
    size_t bytes_decoded;
    int status;
    unsigned char out_buffer[OUT_BUF_SIZE];
    int channels;
    int encoding;
    long rate;
    int i;

    status = MPG123_OK;

    /* set some standard format */
    format.rate = 44100;
    format.channels = 2;
    format.byte_format = AO_FMT_LITTLE;
    format.bits = 16;
    dev = ao_open_live(ao_default_driver_id(), &format, NULL);
    if (!dev) {
        fprintf(stderr, "ao_open_live failed()");
    }

    g_go_on = dev ? 1 : 0;
    while(g_go_on) {
        status = mpg123_read(mh, out_buffer, OUT_BUF_SIZE, &bytes_decoded);
        if (status != 0) {
            fprintf(stderr, "status [%d] - %s\n", status, mpg123_plain_strerror(status));
        }
        if (status == MPG123_ERR || status == MPG123_DONE) {
            g_go_on = 0;
        } else { 
            if (status == MPG123_NEW_FORMAT) {
                status = mpg123_getformat(mh, &rate, &channels, &encoding);
                fprintf(stderr, "new format:\n\trate: %ld\n\tchannels:%d\n\tencoding: %d\n",
                        rate, channels, encoding);
                format.rate = rate;
                format.channels = channels;
                format.byte_format = AO_FMT_LITTLE;
                format.bits = mpg123_encsize(encoding) * 8;
                ao_close(dev);
                dev = ao_open_live(ao_default_driver_id(), &format, NULL);
                if (!dev) {
                    fprintf(stderr, "ao_open_live failed\n");
                    g_go_on = 0;
                }
            }
            if (dev) {
                ao_play(dev, (char*)out_buffer, bytes_decoded);
            }
        }
    }

    if (dev) {
        ao_close(dev);
    }
    return status;
}

static void*
stream_thread_main(void *args)
{
    mpg123_handle *mh;
    thread_args_t *thread_args;
    int status;
    
    fprintf(stderr, "start streaming thread\n");

    mh = NULL;
    thread_args = (thread_args_t*)args;

    g_connection = connection_make(thread_args->ip, thread_args->port);
    if (!g_connection) {
        fprintf(stderr, "error allocating struct connection\n");
        goto cleanup;
    }
    if (connection_connect(g_connection) < 0) {
        perror("connection_connect");
        goto cleanup;
    }

    mpg123_init();
    mh = mpg123_new(NULL, NULL);
    if (!mh) {
        fprintf(stderr, "error mpg123_new()\n");
        goto cleanup;
    }

    mpg123_param(mh, MPG123_VERBOSE, 2, 0);

    if (mpg123_open_fd(mh, g_connection->sock_fd) == MPG123_ERR) {
        fprintf(stderr, "%s\n", mpg123_strerror(mh));
        goto cleanup;
    }

    ao_initialize();

    /* go into loop which reads from connection and parses it to dev */
    status = stream_loop(mh);
    
    fprintf(stderr, "close stream_thread\n");
    fprintf(stderr, "%s\n", mpg123_plain_strerror(status));
    
cleanup:
    fprintf(stderr, "cleanup thread\n");
    
    ao_shutdown();

    if (mh) {
        mpg123_close(mh);
        mpg123_delete(mh);
    }
    mpg123_exit();

    if (g_connection) {
        connection_free(g_connection);
    }
    return NULL;
}

static void
print_usage()
{
    fprintf(stderr, "usage <server> <port>\n");
}

int
cmd_loop(int argc, char **argv)
{
    return 0;
}

static void
signal_handler(int sig)
{
    fprintf(stderr, "signal %d received\n", sig);
    g_go_on = 0;
    connection_disconnect(g_connection);
}

int 
main(int argc, char **argv)
{
    thread_args_t *thread_args;
    pthread_t stream_thread;
    int err;

    if (argc < 3) {
        print_usage();
        return 1;
    }

    signal(SIGINT, signal_handler);

    thread_args = malloc(sizeof(thread_args_t));
    if (!thread_args) {
        return 1;
    }

    thread_args->ip = argv[1];
    thread_args->port = atoi(argv[2]);

    fprintf(stderr, "ip: %s\n", thread_args->ip);
    fprintf(stderr, "port: %d\n", thread_args->port);

    err = pthread_create(&stream_thread, NULL, stream_thread_main, thread_args);
    if (err != 0) {
        perror("thread create");
    } else {
        err = cmd_loop(argc, argv);
        /* wait for stream_thread */
        pthread_join(stream_thread, NULL);
    }

    free(thread_args);

    fprintf(stderr, "program exit with status: %d\n", err);
    return err;
}
