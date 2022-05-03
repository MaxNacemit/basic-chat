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
#include <netdb.h>
#include <string>
#include <map>
#include <iostream>
#include <cstdlib>

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
    Client(const char *server_address, const char *username) {
        struct addrinfo hints = {0};
        struct addrinfo *addresses;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        connfd = socket(AF_INET, SOCK_STREAM, 0);
        if (connfd == -1) {
            perror("couldn't create socket\n");
            exit(1);
        }
        int result = getaddrinfo(server_address, std::to_string(APP_PORT).c_str(), &hints, &addresses);
        if (result != 0) {
            fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(result));
		    exit(1);
        }
        if (connect(connfd, addresses->ai_addr, addresses->ai_addrlen) == -1) {
		    perror("couldn't connect\n");
		    exit(1);
    	}
        name = username;
        if (name.size() > 255) {
            std::cout << "Name too long!\n";
            exit(1);
        }
        name += '\n';
        write(connfd, name.c_str(), name.size());
        epoll_fd = epoll_create(1);
        struct epoll_event msgevent = {EPOLLIN | EPOLLERR | EPOLLPRI, {.u32 = 0}};
        struct epoll_event commandevent = {EPOLLIN | EPOLLERR | EPOLLPRI, {.u32 = 1}};
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, connfd, &msgevent);
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, STDIN_FILENO, &commandevent);
    }

    void receive_command() {
        std::string cmd;
        std::cin >> cmd;
        if (cmd == "send") {
            std::string receiver;
            std::cin >> receiver;
            std::string message;
            getline(std::cin, message);
            message += '\n';
            receiver += message;
            write(connfd, receiver.c_str(), receiver.size());
        } else if (cmd == "quit") {
            write(connfd, " ", 1);
            exit(0);
        } else {
            std::cout << "Command not recognised!\n";
        }
    }

    void receive_message() {
        char sender[256];
        int bytes_read = 0;
        int res = read(connfd, sender, 1);
        while (sender[bytes_read] != '\n' && res > 0) {
            ++bytes_read;
            res = read(connfd, sender + bytes_read, 1);
        }
        sender[bytes_read] = '\0';
        std::cout << "New message from " << std::string(sender) << '\n';
        res = read(connfd, sender, 1);
        while (sender[0] != '\n' && res > 0) {
            write(STDOUT_FILENO, sender, 1);
            res = read(connfd, sender, 1);
        }
        write(STDOUT_FILENO, "\n", 1);
    }

    void handle_events() {
        int evs = epoll_wait(epoll_fd, events, MAX_EVENTS, 50);
        for (int i = 0; i < evs; ++i) {
            if (events[i].data.u32) {
                receive_command();
            } else {
                receive_message();
            }
        }
    }
};

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
                struct epoll_event event = {EPOLLIN | EPOLLERR | EPOLLPRI, {.u32 = conn}};
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn, &event);
                clients[conn].connfd = conn;
                clients[conn].name = "";
                clients[conn].bytes_read = 0;
            } else {
                int connfd = events[i].data.u32;
                struct epoll_event event = {EPOLLIN | EPOLLERR | EPOLLPRI, {.u32 = connfd}};
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, connfd, &event);
                if (clients[connfd].name == "") { // we haven't received the client's name yet
                    clients[connfd].bytes_read = read(connfd, clients[connfd].buf, 256);
                    clients[connfd].buf[clients[connfd].bytes_read - 1] = '\0';
                    clients[connfd].name = std::string(clients[connfd].buf);
                    clients[connfd].bytes_read = 0;
                    connnums[clients[connfd].name] = connfd;
                } else {
                    char namesym[1];
                    std::string receiver;
                    int res = read(connfd, namesym, 1);
                    while (namesym[0] != ' ' && namesym[0] != '\0') {
                        receiver += namesym[0];
                        read(connfd, namesym, 1);
                    }
                    if (receiver == "") {
                        remove_client(connfd);
                    }
                    int recv_connfd = connnums[receiver];
                    if (recv_connfd == 0) {
                        continue; // if an attempt is made to send a message to a non-existent instance of the app, it's ignored
                    }
                    std::string full_message = (clients[connfd].name + '\n');
                    res = read(connfd, clients[connfd].buf, 1);
                    while (clients[connfd].buf[0] != '\n') {
                        full_message += clients[connfd].buf[0];
                        res = read(connfd, clients[connfd].buf, 1);
                    }
                    full_message += '\n';
                    write(recv_connfd, full_message.c_str(), full_message.size());
                }
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, connfd, &event);
            }
        }
    }

    void remove_client(int connfd) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, connfd, NULL);
        clients.erase(connfd);
        close(connfd);
    }
};

int main(int argc, const char *argv[]) {
    if (std::string(argv[1]) == "server") {
        Server s = Server();
        while (1) {
            s.process_messages();
        }
    } else if (std::string(argv[1]) == "client") {
        std::string username;
        std::cout << "What's your name?\n";
        std::cin >> username;
        Client c(argv[2], username.c_str());
        while (1) {
            c.handle_events();
        }
    }
}