#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#define BUFFER_SIZE 64000

unsigned char buffer[BUFFER_SIZE];

void recv_and_send(const fd_set *rfds, int from, int to) {
    ssize_t length;
    if (FD_ISSET(from, rfds)) {
        if ((length = read(from, buffer, BUFFER_SIZE)) > 0) {
            // std::cout << length << std::endl;
            length = write(to, buffer, length);
        } else {
            close(from);
            close(to);
            exit(length);
        }
    }
}

void relay(int csock, int rsock) {
    fd_set rfds;
    const int nfds = getdtablesize();
    while (true) {
        FD_ZERO(&rfds);
        FD_SET(csock, &rfds);
        FD_SET(rsock, &rfds);
        if (select(nfds, &rfds, nullptr, nullptr, nullptr) < 0) continue;
        recv_and_send(&rfds, csock, rsock);
        recv_and_send(&rfds, rsock, csock);
    }
}

bool is_firewall_allow(int cd, std::string client_ip) {
    std::ifstream file;
    file.open("socks.conf");
    const char cmd = (cd == 1 ? 'c' : (cd == 2 ? 'b' : 'x'));
    char c, ip[16];
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            sscanf(line.c_str(), "%*s %c %s %*s", &c, ip);
            if (c != cmd) continue;
            bool pass = true;
            const size_t sz = client_ip.length();
            size_t i = 0, j = 0;
            while (i < sz) {
                while (ip[j] != '\0' && client_ip[i] == ip[j]) ++i, ++j;
                if (ip[j] == '\0') {
                    if (i != sz) pass = false;
                    break;
                }
                if (ip[j] == '*') {
                    ++j;
                    while (client_ip[i] != '.' && i < sz) ++i;
                } else {
                    pass = false;
                    break;
                }
            }
            if (pass) {
                file.close();
                return true;
            }
        }
    }
    file.close();
    return false;
}

void reaper(int sig) {
    while (waitpid(-1, nullptr, WNOHANG) > 0)
        ;
}

int main(int argc, char **argv) {
    // server socket
    int ssock = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(ssock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(ssock, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
    struct sockaddr_in saddr, caddr, raddr;
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (argc > 1) {
        uint16_t port = std::atoi(argv[1]);
        saddr.sin_port = htons(port);
    } else {
        saddr.sin_port = htons(5566);
    }
    bind(ssock, (struct sockaddr *)&saddr, sizeof(saddr));
    listen(ssock, 5);
    // reap client child
    struct sigaction sa_sigchld;
    sa_sigchld.sa_handler = &reaper;
    sigemptyset(&sa_sigchld.sa_mask);
    sa_sigchld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_sigchld, nullptr);
    // accept client
    int csock, pid;
    socklen_t clen = sizeof(caddr);
    while (true) {
        csock = accept(ssock, (struct sockaddr *)&caddr, &clen);
        // fork client
        if ((pid = fork()) == 0) {
            close(ssock);
            break;
        }
        close(csock);
    }
    // read request
    ssize_t sz = read(csock, buffer, BUFFER_SIZE);
    bool valid = true;
    unsigned char VN = 0, CD = 0;
    unsigned short DST_PORT;
    std::string DST_IP;
    char *USER_ID = nullptr, *DOMAIN_NAME = nullptr;
    if (sz >= 9) {
        VN = buffer[0];
        CD = buffer[1];
        DST_PORT = buffer[2] << 8 | buffer[3];
        char dst_ip[INET_ADDRSTRLEN];
        sprintf(dst_ip, "%u.%u.%u.%u", buffer[4], buffer[5], buffer[6],
                buffer[7]);
        DST_IP = std::string(dst_ip);
        USER_ID = (char *)buffer + 8;
        DOMAIN_NAME = USER_ID + strlen(USER_ID) + 1;
    } else
        valid = false;
    // check request
    if (VN != 4) valid = false;
    if (CD != 1 && CD != 2) valid = false;
    char cip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &caddr.sin_addr, cip, INET_ADDRSTRLEN);
    if (!is_firewall_allow(CD, std::string(cip))) valid = false;
    if (valid && CD == 1) {
        struct hostent *he;
        if (DST_IP.substr(0, 6) == "0.0.0.")
            he = gethostbyname(DOMAIN_NAME);
        else
            he = gethostbyname(DST_IP.c_str());
        if (he == nullptr) {
            valid = false;
        } else {
            raddr.sin_family = AF_INET;
            raddr.sin_addr = *((struct in_addr *)he->h_addr);
            raddr.sin_port = htons(DST_PORT);
        }
    }
    // print info
    std::string command_info = (CD == 1 ? "CONNECT" : (CD == 2 ? "BIND" : "")),
                accept_info = (valid ? "Accept" : "Reject");
    std::cout << "<S_IP>: " << std::string(cip) << std::endl
              << "<S_PORT>: " << std::to_string(htons(caddr.sin_port))
              << std::endl
              << "<D_IP>: " << DST_IP << std::endl
              << "<D_PORT>: " << std::to_string(DST_PORT) << std::endl
              << "<Command>: " << command_info << std::endl
              << "<Reply>: " << accept_info << std::endl
              << std::endl;
    // write reply
    buffer[0] = 0;
    if (!valid) {
        buffer[1] = 91;
        write(csock, buffer, 8);
        close(csock);
    } else if (CD == 1) {
        // connect
        int rsock = socket(AF_INET, SOCK_STREAM, 0);
        connect(rsock, (struct sockaddr *)&raddr, sizeof(raddr));
        // reply
        buffer[1] = 90;
        write(csock, buffer, 8);
        // relay
        relay(csock, rsock);
    } else if (CD == 2) {
        // bind
        ssock = socket(AF_INET, SOCK_STREAM, 0);
        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = htonl(INADDR_ANY);
        saddr.sin_port = htons(0);
        bind(ssock, (struct sockaddr *)&saddr, sizeof(saddr));
        listen(ssock, 5);
        socklen_t slen = sizeof(saddr);
        getsockname(ssock, (struct sockaddr *)&saddr, &slen);
        // reply
        buffer[1] = 90;
        unsigned short port = ntohs(saddr.sin_port);
        buffer[2] = port / 256;
        buffer[3] = port % 256;
        buffer[4] = buffer[5] = buffer[6] = buffer[7] = 0;
        write(csock, buffer, 8);
        // accept
        socklen_t rlen = sizeof(raddr);
        int rsock = accept(ssock, (struct sockaddr *)&raddr, &rlen);
        close(ssock);
        write(csock, buffer, 8);
        // relay
        relay(csock, rsock);
    }
    return 0;
}
