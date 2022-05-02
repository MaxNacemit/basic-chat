#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <sys/epoll.h>
#include <string>
#include <map>
#include <iostream>

const int APP_PORT = 1998;
const int MAX_EVENTS = 256;

struct ClientInfo {
    int connfd;
    std::string name;
    char buf[256];
    int bytes_read;
};

class Client {
private:
    std::string name;
    int connfd;
    int epoll_fd;
    struct epoll_event events[MAX_EVENTS];
public:
    Client(const char *server_address, const char *name) {
        struct addrinfo hints = {0};
        struct addrinfo *addresses;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        connfd = socket(AF_INET, SOCK_STREAM, 0);
        if (connfd == -1) {
            perror("couldn't create socket\n");
            exit(1);
        }
        int result = getaddrinfo(server_address, itoa(APP_PORT), &hints, &addresses);
        if (result != 0) {
            fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(result));
		    exit(1);
        }
        if (connect(connfd, addresses->ai_addr, addresses->ai_addrlen) == -1) {
		    perror("couldn't connect\n");
		    exit(1);
    	}
    }
}

class Server {
private:
    std::map<int, ClientInfo> clients; // key is number of connection FD
    std::map<std::string, int> connnums;
    int socketfd;
    int epoll_fd;
    struct epoll_event events[MAX_EVENTS];
public:
    Server() {
        socketfd = socket(AF_INET, SOCK_STREAM, 0);
        if (socketfd == -1) {
            return;
        }
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_port = htons(APP_PORT);
        address.sin_addr.s_addr = INADDR_ANY;
        if (bind(socketfd, (struct sockaddr *)&address, sizeof(address)) == -1) {
            close(socketfd);
            socketfd = -1;
            return;
        }
        if (listen(socketfd, 5) == -1) {
            close(socketfd);
            socketfd = -1;
            return;
        }
        epoll_fd = epoll_create(1);
        struct epoll_event mainevent = {EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLPRI, {.u32 = 0}};
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socketfd, &mainevent);
    }
    void process_messages() {
        int evs = epoll_wait(epoll_fd, events, MAX_EVENTS, 50);
        for (int i = 0; i < evs; ++i) {
            if (events[i].data.u32 == 0) {
                int conn = accept(socketfd, NULL, NULL);
                if (conn == -1) {
                    std::cerr << "Cannot accept connection!\n";
                    continue;
                }
                write(conn, "Hello!\n", 7);
                struct epoll_event event = {EPOLLIN | EPOLLERR | EPOLLPRI, {.u32 = conn}};
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn, &event);
                clients[conn] = {.connfd = conn, .name = "", .bytes_read = 0};
            }
        } else {
            int connfd = events[i].data.u32;
            if (clients[connfd].name == "") { // we haven't received the client's name yet
                read(connfd, clients[connfd].buf, 1);
                while (clients[connfd].buf[clients[connfd].bytes_read] != '\0') {
                    ++clients[connfd].bytes_read;
                    read(connfd, clients[connfd].buf + clients[connfd].bytes_read, 1);
                }
                clients[connfd].name = std::string(buf);
                clients[connfd].bytes_read = 0;
                connums[clients[connfd].name] = connfd;
            } else {
                char namesym[1];
                std::string receiver;
                int res = read(connfd, namesym, 1);
                while (namesym[0] != ' ') {
                    receiver += namesym[0];
                }
                int recv_connfd = connums[receiver];
                if (recv_connfd == 0) {
                    continue; // if an attempt is made to send a message to a non-existent instance of the app, it's ignored
                }
                const char *sender_name = clients[connfd].name.c_str();
                write(recv_connfd, sender_name, clients[connfd].name.size());
                write(recv_connfd, "\n", 1);
                int res = read(connfd, clients[connfd].buf, 255);
                while (res > 0) {
                    write(recv_connfd, clients[connfd].buf, res);
                    res = read(connfd, clients[connfd].buf, 255);
                }
                write(recv_connfd, "\0", 1);
            }
        }
    }
    void remove_client(int connfd) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, connfd, NULL);
        close(connfd);
    }
}

int main() {
    Server s = Server();
    while (1) {
        s.process_messages();
    }
}