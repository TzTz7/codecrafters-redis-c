#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <string.h>
#include <errno.h>
#include <threads.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>

typedef struct {
	int client_fd;
	struct sockaddr_in client_addr;
} ThreadArgs;

int create_server_socket() {
	int server_fd;
	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd == -1) {
		printf("Socket creation failed: %s...\n", strerror(errno));
		return 1;
	}

	// Since the tester restarts your program quite often, setting SO_REUSEADDR
	// ensures that we don't run into 'Address already in use' errors
	int reuse = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		printf("SO_REUSEADDR failed: %s \n", strerror(errno));
		return 1;
	}

	struct sockaddr_in serv_addr = { .sin_family = AF_INET ,
									 .sin_port = htons(6379),
									 .sin_addr = { htonl(INADDR_ANY) },
									};
	
	if (bind(server_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) != 0) {
		printf("Bind failed: %s \n", strerror(errno));
		return 1;
	}
	
	int connection_backlog = 5;
	if (listen(server_fd, connection_backlog) != 0) {
		printf("Listen failed: %s \n", strerror(errno));
		return 1;
	}
	printf("Server listening on port 6379\n");
	return server_fd;
}

void* handle_client(void *arg){
	ThreadArgs *args = (ThreadArgs *)arg;
	char buffer[1024];
	const char *response = "+PONG\r\n";

    printf("New connection from %s:%d\n",
           inet_ntoa(args->client_addr.sin_addr),
           ntohs(args->client_addr.sin_port));

	while(1) {
		ssize_t bytes_read = recv(args->client_fd, buffer, sizeof(buffer), 0);
		if(bytes_read <= 0){
			perror("read failed");
			close(args->client_fd);
			free(arg);
			return NULL;
		}
		send(args->client_fd, response, strlen(response), 0);
	}
	close(args->client_fd);
	free(arg);
	return NULL;
}

int main() {
	// Disable output buffering
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);
	
	int server_fd = create_server_socket();
	struct sockaddr_in client_addr;
	int client_addr_len = sizeof(client_addr);
	
	printf("Waiting for a client to connect...\n");
	
	while(1){
		// 接受新客户端连接
		int client_fd = accept(server_fd, (struct sockaddr *) &client_addr, &client_addr_len);
		if(client_fd == -1){
			printf("Accept client failed:%s...\n", strerror(errno));
			continue; //可能是临时问题
		}
		// 为每个客户端创建一个新线程
		pthread_t thread_id;
		ThreadArgs* args = malloc(sizeof(ThreadArgs));
		args->client_fd = client_fd;
		args->client_addr = client_addr;
		if(pthread_create(&thread_id, NULL, handle_client, (void *)args) != 0){
			perror("pthread create failed");
			close(client_fd);
			free(args);
		}else{
			pthread_detach(thread_id);
		}
		// printf("Client connected\n");
	}
	close(server_fd);
	return 0;
}
