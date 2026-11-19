#include <asm-generic/errno-base.h>
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

#define PARSE_OK          0
#define PARSE_INCOMPLETE  1
#define PARSE_ERROR       2
#define MAX_LINE_LEN	  1024
typedef struct {
	int client_fd;
	struct sockaddr_in client_addr;
} ThreadArgs;

int create_server_socket() {
	int server_fd;
	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd == -1) {
		printf("Socket creation failed: %s...\n", strerror(errno));
		return -1;
	}

	// Since the tester restarts your program quite often, setting SO_REUSEADDR
	// ensures that we don't run into 'Address already in use' errors
	int reuse = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		printf("SO_REUSEADDR failed: %s \n", strerror(errno));
		return -1;
	}

	struct sockaddr_in serv_addr = { .sin_family = AF_INET ,
									 .sin_port = htons(6379),
									 .sin_addr = { htonl(INADDR_ANY) },
									};
	
	if (bind(server_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) != 0) {
		printf("Bind failed: %s \n", strerror(errno));
		return -1;
	}
	
	int connection_backlog = 5;
	if (listen(server_fd, connection_backlog) != 0) {
		printf("Listen failed: %s \n", strerror(errno));
		return -1;
	}
	printf("Server listening on port 6379\n");
	return server_fd;
}

/*
\r 在 buffer 最后一个字节：*pos + 1 >= len，说明 \n 还没收到。这是典型半包，必须返回 INCOMPLETE 而不是错误。很多实现栽在这。

\r 后面不是 \n：协议错。严格 CRLF。

out 溢出：行太长（比如 $ 后面跟了一万个数字）。out_max 挡住，返回 ERROR。控制行正常就几个字节，MAX_LINE_LEN = 64 绰绰有余。

找不到 \r：整行都没到齐，INCOMPLETE。

为什么单独抽出来：*N 和 $len 都要"读一行"，逻辑一模一样。抽出来避免重复，也让 parse_array/parse_bulk_string 只剩结构判断。
*/
int read_line(const char *buffer, size_t len, size_t *pos, char *out, size_t out_max, size_t *out_len ){
	size_t start = *pos;
	size_t line_len = 0;
	while( *pos < len ){
		//当前读到了 \r
		if( buffer[*pos] == '\r'){
			//如果\r是最后的字符
			if( *pos + 1 >= len ) { *pos = start; return PARSE_INCOMPLETE; }
			//pos + 1 < len 但是下一个是不是\n
			if ( buffer[*pos + 1] != '\n') { *pos = start; return PARSE_ERROR;}
			//解析完成
			*pos += 2;
			*out_len = line_len;
			return PARSE_OK;
		}
		//超出了长度
		if ( line_len >= out_max) { *pos = start; return PARSE_ERROR;}
		out[line_len++] = buffer[*pos];
		(*pos) ++;
	}
	*pos = start;
	//ead_line 找不到 \r 返回 ERROR 是二号杀手 这里我之前写了PARSE_ERROR
	return PARSE_INCOMPLETE;
}

int parse_integer(const char* s, size_t l, long *out) {
	if( l <= 0 || l >= 10) return -1;
	long v = 0;
	for(size_t i = 0; i < l; i ++){
		if(s[i] < '0' || s[i] > '9') return -1;
		v = 10 * v + (s[i] - '0');
	}
	*out = v;
	return PARSE_OK;
}
/*
len - *pos < length + 2：这是整个设计最核心的一行。payload 要 length 字节，尾部 CRLF 要 2 字节，一共 length + 2。buffer 剩余不足这么多，就是半包，INCOMPLETE。注意 len - *pos 是 size_t 无符号减法，前面必须保证 *pos <= len（代码里开头 *pos >= len 已经挡了，中间 read_line 成功也保证 *pos <= len，所以安全）。

$ 后面直接 read_line：长度行和 payload 是分开读的，read_line 负责切出 "4"，parse_integer 转成 4，然后才去数 payload。顺序不能反。

尾部 CRLF 校验：payload 后面必须是 \r\n。不校验的话 $3\r\nabcXX 会被误判成功，pos 走到错误位置，后续全错位。

零拷贝的代价：*data_out 指向 buf 内部。这意味着调用方必须在 buf 内容被改动（比如 memmove）之前用掉它。这是 dispatch 必须放在 memmove 之前的原因。

(*pos)++ 吃掉 $ 之后没有回退：如果后面 read_line 失败，会 *pos = start 把 $ 也退回去。这是对的——整个 bulk 是一个原子单元，要么全成要么全退。

data_out 为什么是 const char *：提醒调用方"你别改它，它指向别人的 buffer"。payload 本身是二进制安全的，但所有权不在你。
*/
int parse_bulk_string(const char* buffer, size_t len, size_t *pos, const char **out, size_t *out_len){
	size_t start = *pos;
	if( *pos >= len ) return PARSE_INCOMPLETE;
	if( buffer[*pos] != '$') return PARSE_ERROR;
	(*pos) ++; //越过$
	char line[MAX_LINE_LEN]; size_t line_len = 0;
	int r = read_line(buffer, len, pos, line, sizeof(line), &line_len);
	if( r != PARSE_OK ) { *pos = start; return r;}

	long payload_len;
	if(parse_integer(line, line_len, &payload_len) != 0){
		*pos = start;
		return PARSE_ERROR;
	}

	if( len - *pos < payload_len + 2){
		*pos = start;
		return PARSE_INCOMPLETE;
	}

	if( buffer[*pos + payload_len] != '\r' || buffer[*pos + payload_len + 1] != '\n'){
		*pos = start;
		return PARSE_ERROR;
	}

	*out = buffer + *pos;
	*out_len = payload_len;
	*pos += payload_len + 2;
	return PARSE_OK;
}

/*
* 必须出现在第一个位置：buf[*pos] != '*' 就是 inline 命令或裸 bulk，直接 ERROR。丐版不支持这些。

count > max_argc：*99999999 会让循环空转，提前用 max_argc 挡。max_argc 由调用方传，handle_client 里传 1024。

循环里任何一次 parse_bulk_string 失败，整条命令退回起点：*pos = start 把前面已经解析成功的 *2 和第一个 bulk 全退掉。下次从头重解析。这是"路 1"的代价，但正确性有保证。

argv 不复制：直接存 parse_bulk_string 给的指针。argv[i] 和 argvlen[i] 配对，payload 里有什么字节都不管。

argc 是 int * 不是 size_t：argv 下标是 int，保持一致。count 是 long，count > max_argc 已经挡住，(int)count 安全。

为什么这是唯一入口：上层只需要"给我一条完整命令"，不需要知道内部是 * 还是 $。read_line 和 parse_bulk_string 的 static 就是表达"这些是内部实现，别从外面调"。
*/
int parse_array(const char *buffer, size_t len, size_t *pos, const char **argv, size_t *argvlen, int *argc, int max_argc){
	size_t start = *pos;
	if( *pos >= len ) return PARSE_INCOMPLETE;
	if ( buffer[*pos] != '*') return PARSE_ERROR;
	(*pos) ++;

	//read count
	char line[MAX_LINE_LEN]; size_t line_len = 0;
	int r = read_line(buffer, len, pos, line, sizeof(line), &line_len);
	if(r != PARSE_OK) { *pos = start; return r;}

	//parse
	long count = 0;
	if (parse_integer(line, line_len, &count) != 0) {   // ✅
		*pos = start;
		return PARSE_ERROR;
	}
	if(count >= max_argc){ *pos = start; return PARSE_ERROR;}

	for(size_t i = 0;i < count; i ++){
		const char *s; size_t l;
		int r = parse_bulk_string(buffer, len, pos, &s, &l);
		if(r != PARSE_OK) { *pos = start; return r;}
		argv[i] = s;
		argvlen[i] = l;
	}
	*argc = count;
	return PARSE_OK;
}

void dispatch(const char **argv, size_t *argvlen, int argc, int fd)
{
    if (argc == 0) return;   // 空数组，直接忽略

    // 命令名大小写不敏感，用 strncasecmp 带长度比较
    if (strncasecmp(argv[0], "PING", argvlen[0]) == 0 && argvlen[0] == 4) {
        send(fd, "+PONG\r\n", 7, 0);
    }
    else if (strncasecmp(argv[0], "ECHO", argvlen[0]) == 0 && argvlen[0] == 4) {
        if (argc < 2) {
            send(fd, "-ERR wrong number of arguments\r\n", 33, 0);
            return;
        }
        // 响应格式：$<len>\r\n<payload>\r\n
        char header[32];
        int hlen = snprintf(header, sizeof(header), "$%zu\r\n", argvlen[1]);
        send(fd, header, hlen, 0);
        send(fd, argv[1], argvlen[1], 0);       // payload 二进制安全，带长度发
        send(fd, "\r\n", 2, 0);
    }
    else {
        // 未知命令兜底（COMMAND DOCS / HELLO 都走这里）
        char err[128];
        int n = snprintf(err, sizeof(err), "-ERR unknown command '%.*s'\r\n",
                         (int)argvlen[0], argv[0]);
        send(fd, err, n, 0);
    }
}

void* handle_client(void *arg){
	ThreadArgs *args = (ThreadArgs *)arg;
	char buffer[1024];
	size_t len = 0;
	const char* argv[1024];
	size_t argvlen[1024];

	const char *response = "+PONG\r\n";
    char ip[INET_ADDRSTRLEN];

	inet_ntop(AF_INET, &args->client_addr.sin_addr, ip, sizeof(ip));
	printf("New connection from %s:%d\n", ip,
		(int)ntohs(args->client_addr.sin_port));

	while(1) {
		// 客户端读
		ssize_t bytes_read = recv(args->client_fd, buffer + len, sizeof(buffer) - len, 0);
		if(bytes_read < 0){
			if(errno == EINTR) continue;
			perror("recv failed");  // 真正的错误（如 EAGAIN、ECONNRESET
			break;
		}else if(bytes_read == 0){
			printf("Client disconnected gracefully\n");  // 客户端主动关闭
			break;
		}
		len += bytes_read;

		while (1) {
			size_t pos = 0;
			int argc = 0;
			int r = parse_array(buffer, len, &pos, argv, argvlen, &argc, 1024);
			if (r == PARSE_INCOMPLETE) break;     // 等下次 recv
            if (r == PARSE_ERROR) goto out;        // 协议错，关连接
			dispatch(argv, argvlen, argc, args->client_fd);  // ← 必须在 memmove 前

			memmove(buffer, buffer + pos, len - pos);    // 移走已消费的
			len -= pos;
		}
		// printf("buffer:%s",buffer);
		// // 客户端发 TODO:检查send返回值
		// ssize_t n = send(args->client_fd, response, strlen(response), 0);
		// if(n < 0){
		// 	if(errno == EINTR) continue;
		// 	perror("send failed");
		// 	break;
		// }
		if (len == sizeof(buffer)) break;
	}
	out:
		close(args->client_fd);
		free(arg);
		return NULL;
}

int main() {
	// Disable output buffering
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);
	
	// signal(SIGPIPE, SIG_IGN);              // ← 必须

	int server_fd = create_server_socket();
	if (server_fd < 0) return 1;
	struct sockaddr_in client_addr;

	printf("Waiting for a client to connect...\n");
	
	while(1){
		socklen_t client_addr_len = sizeof(client_addr);
		// 接受新客户端连接
		int client_fd = accept(server_fd, (struct sockaddr *) &client_addr, &client_addr_len);
		if (client_fd == -1) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
		// 为每个客户端创建一个新线程
		pthread_t thread_id;
		ThreadArgs* args = malloc(sizeof(ThreadArgs));
		args->client_fd = client_fd;
		args->client_addr = client_addr;

        int ret = pthread_create(&thread_id, NULL, handle_client, args);
        if (ret != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(ret));
            close(client_fd);
            free(args);
        } else {
            pthread_detach(thread_id);
        }
	}
	close(server_fd);
	return 0;
}
