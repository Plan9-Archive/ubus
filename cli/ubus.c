#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include "ubus.h"

#define MODE_TERMINAL 1
#define MODE_TAP 2
#define MODE_POOL 3
#define MODE_CONNECT 4

char stdineof=0;
char ** handler=0;
char mode=0;


struct  ubus_client_list_el{
    ubus_chan_t * chan;
    int handler;
    int handler_in;
    int handler_out;
    struct ubus_client_list_el * next;
};
typedef struct ubus_client_list_el ubus_client;
ubus_client * clients;

ubus_client * ubus_client_add(ubus_chan_t * chan){
    ubus_client * c=(ubus_client *)malloc(sizeof(ubus_client));
    c->chan=chan;
    c->handler=0;
    c->handler_in=0;
    c->handler_out=0;
    c->next=NULL;
    if(clients==NULL){
        clients=c;
        return c;
    }
    ubus_client * cur=clients;
    while(cur){
        if(cur->next==NULL){
            cur->next=c;
            return c;
        }
        cur=cur->next;
    }
    fprintf(stderr,"corrupted linked list");
    abort();
}
void ubus_client_del(ubus_chan_t * chan){
    ubus_client * prev=NULL;
    ubus_client * cur=clients;
    while(cur){
        if(cur->chan==chan){
            if(prev){
                prev->next=cur->next;
            }else{
                clients=cur->next;
            }
            if(cur->handler != 0){
                close(cur->handler_in);
                close(cur->handler_out);
                kill(cur->handler,SIGTERM);
                ubus_disconnect(cur->chan);
            }
            free(cur);
            return;
        }
        prev=cur;
        cur=cur->next;
    }
    fprintf(stderr,"corrupted linked list");
    abort();
}








//---------------main---------------------------------

int main(int argc, char ** argv){

    int arg=1;
    char * p_argv [argc];
    int    p_argc=0;

    const char * filename=0;

    while(argc > arg){
        if(mode==0){
            if(strcmp (argv[arg],"terminal")==0){
                mode=MODE_TERMINAL;
            }else if(strcmp (argv[arg],"tap")==0){
                mode=MODE_TAP;
            }else if(strcmp (argv[arg],"pool")==0){
                mode=MODE_POOL;
            }else if(strcmp (argv[arg],"connect")==0){
                mode=MODE_CONNECT;
            }else{
                goto usage;
            }
        } else if(argv[arg][0]=='-'  && handler==0){
            if(strcmp (argv[arg],"-h")==0){
                goto usage;
            }else{
                goto usage;
            }
        }else if (filename==0){
            filename=argv[arg];
        }else if (handler==0){
            handler=argv+arg;
        }else{
        }
        ++arg;
    }
    if(filename==0 ||  (mode==MODE_TERMINAL && handler==0)){
    usage:
        fprintf (stderr,"Usage: ubus MODE /var/ipc/user/name/app/methodname  [OPTIONS]  [handler] [handler arg1] [...]\n\n"
                "\n"
                "MODE is one of:\n"
                "\n"
                "terminal\n\n"
                "  Create a channel. \n"
                "  Spawns a new handler for each incomming channel and connects stdin/stdout.\n"
                "  The channel and the handlers are destroyed when ubus is terminated.\n"
                "  \n"
                "tap\n\n"
                "  Create a channel. \n"
                "  Write stdin to all connected channels. Read from all connected channels to stdout.\n"
                "  EOF on stdin terminates ubus. \n"
                "  The channel is destroyed when ubus is terminated.\n"
                "  \n"
                "pool\n\n"
                "  Create a channel. \n"
                "  Connect all stdings to all stdouts.\n"
                "  EOF on stdin terminates ubus. \n"
                "  The channel is destroyed when ubus is terminated.\n"
                "  \n"
                "connect\n\n"
                "  connect stdin/stdout to an existing channel.\n"
                "  EOF on stdin terminates ubus. \n"
                "  \n"
                );
        exit (EXIT_FAILURE);
    }


    ubus_init();


    if(mode==MODE_TERMINAL || mode==MODE_TAP  || mode==MODE_POOL){
        //---------------server mode---------------------------------
        ubus_t * s=ubus_create(filename);
        if(s==0){
            perror("ubus_create");
            exit(0);
        }

        //linked list with connected clients
        clients=NULL;

        //We'll handle that as return of read()
        signal (SIGPIPE,SIG_IGN);

        fd_set rfds;
        for(;;) {3
            FD_ZERO (&rfds);

            if(mode!=MODE_TERMINAL){
                fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
                FD_SET  (0, &rfds);
            }

            int server=ubus_fd(s);
            FD_SET  (server, &rfds);

            int maxfd=server;
            ubus_client* c=clients;
            while(c){
                int fd=ubus_chan_fd(c->chan);
                FD_SET  (fd, &rfds);
                if(fd>maxfd){
                    maxfd=fd;
                }
                if(mode==MODE_TERMINAL){
                    FD_SET  (c->handler_out, &rfds);
                    if(c->handler_out>maxfd){
                        maxfd=c->handler_out;
                    }
                }
                c=c->next;
            }
            if (select(maxfd+2, &rfds, NULL, NULL, NULL) < 0){
                perror("select");
                exit(1);
            }

            //accept new callers
            if(FD_ISSET(server, &rfds)){
                ubus_chan_t * client =ubus_accept (s);
                if (client==0) {
                    if (errno==EAGAIN || errno==EWOULDBLOCK){
                    }else{
                        perror("ubus_accept");
                        exit(1);
                    }
                }else{
                    ubus_client * cl = ubus_client_add(client);
                    if(mode==MODE_TERMINAL){
                        int pipe_in[2];
                        int pipe_out[2];
                        if (pipe(pipe_in) < 0){
                            perror ("pipe");
                            exit (errno);
                        }
                        if (pipe(pipe_out) < 0){
                            perror ("pipe");
                            exit (errno);
                        }

                        cl->handler=fork();
                        if(cl->handler<0){
                            perror("fork");
                            exit(1);
                        }
                        if(cl->handler==0){
                            //i'm the child. wee

                            close (pipe_in[1]);
                            dup2 (pipe_in[0], 0);
                            close (pipe_in[0]);

                            close (pipe_out[0]);
                            dup2 (pipe_out[1], 1);
                            close (pipe_out[1]);

                            execvp (handler[0],handler);
                            perror("execvp");
                            exit(1);
                        }
                        close (pipe_in[0]);
                        cl->handler_in=pipe_in[1];

                        close (pipe_out[1]);
                        cl->handler_out=pipe_out[0];
                    }
                }
            }

            //read clients
            c=clients;
            while(c){
                if(mode==MODE_TERMINAL){
                    if(FD_ISSET(c->handler_out, &rfds)){
                        char buff [100];
                        int n=read(c->handler_out,&buff,100);
                        if (n<1){
                            if(n<0){
                                perror("handler");
                            }
                            ubus_chan_t * ff=c->chan;
                            c=c->next;
                            ubus_client_del(ff);
                            continue;
                        }
                        ubus_write(c->chan,&buff,n);
                    }
                }
                int fd=ubus_chan_fd(c->chan);
                if(FD_ISSET(fd, &rfds)){
                    char buff [100];
                    int n=ubus_read(c->chan,&buff,100);
                    if(n==0 && mode==MODE_TERMINAL){
                        close(c->handler_in);
                        c=c->next;
                        continue;
                    }
                    else if (n<1){
                        ubus_chan_t * ff=c->chan;
                        c=c->next;
                        ubus_client_del(ff);
                        continue;
                    }

                    if(mode==MODE_TERMINAL){
                        //FIXME: this may block :(
                        write(c->handler_in,&buff,n);
                    }else if (mode==MODE_TAP){
                        write(1,&buff,n);
                    }else if (mode==MODE_POOL){
                        write(1,&buff,n);
                        ubus_client * c2=clients;
                        while(c2){
                            if (ubus_write(c2->chan,&buff,n)<1){
                                perror("send");
                                if(c!=c2){
                                    ubus_chan_t * ff=c2->chan;
                                    c2=c2->next;
                                    ubus_client_del(ff);
                                    continue;
                                }
                            }
                            c2=c2->next;
                        }
                    }
                }
                c=c->next;
            }

            if(mode!=MODE_TERMINAL){
                if(FD_ISSET(0, &rfds)){
                    char buff [1000];
                    int n=read(0,&buff,1000);
                    if(n<0){
                        perror("read");
                        continue;
                    }
                    if(n==0){
                        //FIXME: do i have to flush the other fds?
                        exit(0);
                    }
                    ubus_client* c = clients;
                    while(c){
                        if (ubus_write(c->chan,&buff,n) < 0) {
                            ubus_chan_t * ff=c->chan;
                            c=c->next;
                            ubus_client_del(ff);
                        }
                        c=c->next;
                    }
                    if(mode==MODE_POOL){
                        write(1,&buff,n);
                    }
                }
            }

        }
        ubus_destroy(s);
    }else{
        //---------------client mode---------------------------------
        ubus_chan_t * chan=ubus_connect(filename);
        if(chan==0){
            perror("ubus_connect");
            exit(errno);
        }

        for(;;) {
            fd_set rfds;
            FD_ZERO (&rfds);
            fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
            FD_SET  (0, &rfds);
            int s=ubus_chan_fd(chan);
            fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
            FD_SET  (s, &rfds);

            if (select(s+2, &rfds, NULL, NULL, NULL) < 0){
                perror("select");
                exit(1);
            }


            if(FD_ISSET(s, &rfds)){
                char buff [1000];
                int n=ubus_read(chan,&buff,1000);
                if(n<1){
                    exit (0);
                }
                write(1,&buff,n);
            }

            if(FD_ISSET(0, &rfds)){
                char buff [100];
                int n=read(0,&buff,100);
                if(n==0){
                    ubus_disconnect(chan);
                    continue;
                }
                else if(n<1){
                    perror("read");
                    exit(errno);
                }
                if (ubus_write(chan, &buff, n) < 1) {
                    perror("send");
                    exit(1);
                }
            }
        }
    }
    return 0;
}


