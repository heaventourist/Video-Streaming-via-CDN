#include<stdlib.h>
#include<string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <time.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "DNSHeader.h"
#include "DNSQuestion.h"
#include "DNSRecord.h"

#define MAXSIZE 40000

// data structure sent to dns
typedef struct {
	struct DNSHeader header;
	struct DNSQuestion question;
} DNS_QUERY;

// data structure received from dns
typedef struct {
	struct DNSHeader header;
	struct DNSRecord record;
} DNS_RESPONSE;

// structure for mapping between file descriptor and ip, used by dns
typedef struct {
	int fd;
	char *ip;
} fd_ip_map;

// array to store mapping between file descriptor and ip, used by dns
fd_ip_map map[MAXSIZE] = {0};
// number of mapping 
int map_cnt = 0;

// available servers to assign to the client
char* server_list[MAXSIZE] = {NULL};
// number of available servers
int server_list_cnt = 0;
// when round robin, used to track the next ip to be assigned
int round_robin_index = 0;

// topology of the network, used to find the closest server to the client
char* topology_info[MAXSIZE] = {NULL};
// topology info is store line by line, this is the line number
int topology_info_cnt = 0;

// prototype
void update_map(int fd, char *ip, fd_ip_map *map, int *map_cnt);
void remove_map(int fd, fd_ip_map *map, int *map_cnt);
char* map_lookup(int fd, fd_ip_map *map, int *map_cnt);
int read_file(char *filename, char *buf);
int print_log(char *filename, char *log_buffer);
int open_listenfd(char *port);
int open_clientfd(char *hostname, char *port);
int rio_readn(int fd, char *usrbuf, int n);
int rio_writen(int fd, char *usrbuf, int n);
int readline(char *srcbuf, char *dstbuf, int maxlen, int offset);
void reset_array(int *array);
int get_index_of_smallest(int *array, int first, int last);
void find_server_ip(char *client_ip, char *server_ip);
void *get_in_addr(struct sockaddr *sa);

// main begin
int main(int argc, char *argv[])
{
	// If no argument is given at all.
	if(argc == 1)
	{
		printf("Error: missing or additional arguments\n");
		exit(1);
	}
	// the file path to store the log
	char *log = argv[1];
	// the port dns is to listen
	char *port = argv[2];
	// if 1, implement the distance based scheme. if 0, round-robin
	int geography_based = atoi(argv[3]);
	// filename containing a list of IP addresses if geography_based is 0.
	// filename containing the network topology if geography_based is 1.
	char *servers = argv[4];
	// temporarily store the log message before print to txt file.
	char log_buffer[MAXSIZE] = {0};
	int log_index = 0;

	// master file descriptor list
	fd_set master;
	// temp file descriptor list for select
	fd_set read_fds;
	// maximum file descriptor number
	int fdmax;

	// listening socket descriptor
	int listenfd;
	// newly accept()ed socket descriptor
	int newfd;
	// client address
	struct sockaddr_storage remoteaddr;
	socklen_t addrlen;

	// buffer for data input and output
	char buf[MAXSIZE] = {0}; 
	// buffer for data line by line
	char line_buf[MAXSIZE] = {0};
	// when reading line, this is used to indicate the position in the buffer
	int offset;
	// number of bytes read
	int nbytes;

	// keep the proxy ip
	char remoteIP[INET6_ADDRSTRLEN];

	int i;

	// clear the master and temp sets
	FD_ZERO(&master);
	FD_ZERO(&read_fds);

	// read data from the text file
	if(read_file(servers, buf) < 0)
	{
		exit(1);
	}

	offset = 0;
	if(geography_based)
	{
		while((nbytes=readline(buf, line_buf, sizeof line_buf, offset)) > 0)
		{
			offset+=nbytes;
			char *tmp = (char *)malloc(nbytes*sizeof(char));
			strcpy(tmp, line_buf);
			topology_info[topology_info_cnt++] = tmp;
		}
	}
	else
	{
		while((nbytes=readline(buf, line_buf, sizeof line_buf, offset)) > 0)
		{
			offset+=nbytes;
			char *tmp = (char *)malloc(nbytes*sizeof(char));
			strcpy(tmp, line_buf);
			server_list[server_list_cnt++] = tmp;
		}
	}

	// listen
	if((listenfd = open_listenfd(port)) < 0)
	{
		exit(1);
	}

	// add the listenfd to the master set
	FD_SET(listenfd, &master);

	//keep track of the biggest file descriptor
	fdmax = listenfd;

	memset(buf, 0, MAXSIZE*sizeof(char));
	memset(line_buf, 0, MAXSIZE*sizeof(char));
	while(1)
	{
		read_fds = master; //copy it
		if(select(fdmax+1, &read_fds, NULL, NULL, NULL) == -1)
		{
			perror("select");
			exit(1);
		}

		// run through the existing connections looking for data to read
		for(i = 0; i <= fdmax; i++)
		{
			if(FD_ISSET(i, &read_fds))
			{
				if(i == listenfd)
				{
					//handle new connections
					addrlen = sizeof remoteaddr;
					newfd = accept(listenfd, (struct sockaddr *)&remoteaddr, &addrlen);

					if(newfd == -1)
					{
						perror("accept");
					}
					else
					{
						FD_SET(newfd, &master); //add to master set
						if(newfd > fdmax)
						{
							fdmax = newfd;
						}

						const char *tmp_ip = inet_ntop(remoteaddr.ss_family,
								get_in_addr((struct sockaddr*)&remoteaddr),
								remoteIP, INET6_ADDRSTRLEN);
						char tmp_tmp_ip[sizeof tmp_ip];
						strcpy(tmp_tmp_ip, tmp_ip);
						update_map(newfd, tmp_tmp_ip, map, &map_cnt);
						printf("selectserver: new connection from %s on socket %d\n",
							tmp_ip, newfd);
					}
				}
				else
				{
					if((nbytes = read(i, buf, sizeof buf)) <= 0)
					{
						if(nbytes == 0)
						{
							printf("selectserver: socket %d hung up\n", i);
						}
						else
						{
							perror("recv");
						}
						if(print_log(log, log_buffer) == -1)
						{
							exit(1);
						}
						remove_map(i, map, &map_cnt);
						close(i);
						FD_CLR(i, &master);
					}
					else
					{
						DNS_QUERY *qry = (DNS_QUERY *)buf;
						char *client_ip = map_lookup(i, map, &map_cnt);
						char *query_name = qry->question.QNAME;
						char response_ip[MAXSIZE];

						if(!geography_based)
						{
							strcpy(response_ip, server_list[round_robin_index%server_list_cnt]);
							round_robin_index++;
						}
						else
						{							
							find_server_ip(client_ip, response_ip);
						}

						DNS_RESPONSE new_response;
						new_response.header.ID = qry->header.ID;
						new_response.header.QR = 1;
						new_response.header.OPCODE = qry->header.OPCODE;
						new_response.header.AA = 1;
						new_response.header.TC = 0;
						new_response.header.RD = 0;
						new_response.header.RA = 0;
						new_response.header.Z = 0;
						new_response.header.RCODE = 0;
						new_response.header.QDCOUNT = 0;
						new_response.header.ANCOUNT = 1;
						new_response.header.NSCOUNT = 0;
						new_response.header.ARCOUNT = 0;

						new_response.record = *(new DNSRecord());
						strcpy(new_response.record.NAME, qry->question.QNAME);
						new_response.record.TYPE = 1;
						new_response.record.CLASS = 1;
						new_response.record.TTL = 1;
						new_response.record.RDLENGTH = strlen(response_ip);
						strcpy(new_response.record.RDATA, response_ip);
						char *sent_response = (char *)&new_response;
						if(rio_writen(i, sent_response, strlen(sent_response)) < 0)
						{
							perror("dns send");
							exit(1);
						}

						sprintf(log_buffer, "%s %s %s\n", client_ip, query_name, response_ip);
					}
				}
			}
		}
	}
}

void update_map(int fd, char *ip, fd_ip_map *map, int *map_cnt)
{
	fd_ip_map tmp;
	tmp.fd = fd;
	tmp.ip = ip;
	map[(*map_cnt)++] = tmp;
}

void remove_map(int fd, fd_ip_map *map, int *map_cnt)
{
	int index;
	for(index=0; index<*map_cnt; index++)
	{
		if(map[index].fd == fd)
			break;
	}
	map[index] = map[--(*map_cnt)];
	memset(map+(*map_cnt), 0, sizeof(fd_ip_map));
}

char* map_lookup(int fd, fd_ip_map *map, int *map_cnt)
{
	int index;
	for(index=0; index<*map_cnt; index++)
	{
		if(map[index].fd == fd)
		{
			return map[index].ip;
		}
	}
	return NULL;
}

int read_file(char *filename, char *buf)
{
	FILE *pFile;
	pFile = fopen(filename, "r+");
	if(pFile == NULL)
	{
		fprintf(stderr, "Can not open %s.\n", filename);
		return -1;
	}
	fread(buf, 1, sizeof buf, pFile);
	fclose(pFile);
	return 0;
}

int print_log(char *filename, char *log_buffer)
{
	FILE *pFile;
	pFile = fopen(filename, "w+");
	if(pFile == NULL)
	{
		fprintf(stderr, "Can not open %s.\n", filename);
		return -1;
	}
	fprintf(pFile, "%s", log_buffer);
	fclose(pFile);
	return 0;
}

// start listening on port.
int open_listenfd(char *port)
{
	struct addrinfo hints, *listp, *p;
	int listenfd, rv, yes = 1;

	/* Get a list of potential server addresses */
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	if((rv=getaddrinfo(NULL, port, &hints, &listp)) != 0)
	{
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return -1;
	}

	for(p = listp; p != NULL; p = p->ai_next)
	{
		if((listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
		{
			perror("client: socket");
			continue;
		}

		if(setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
		{
			perror("sersockopt");
			return -1;
		}
		if((bind(listenfd, p->ai_addr, p->ai_addrlen)) == -1)
		{
			close(listenfd);
			perror("server: bind");
			continue;
		}
		break;
	}

	freeaddrinfo(listp);

	if(p == NULL)
	{
		fprintf(stderr, "server: failed to bind\n");
		return -1;
	}

	if(listen(listenfd, 20) == -1)
	{
		perror("listen");
		close(listenfd);
		return -1;
	}

	printf("server: waiting for connections...\n");
	return listenfd;
}

// connecting to remote host on port.
int open_clientfd(char *hostname, char *port)
{
	int clientfd;
	struct addrinfo hints, *servinfo, *p;
	int rv;

	/* Get a list of potential server addresses */
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if((rv=getaddrinfo(hostname, port, &hints, &servinfo)) != 0)
	{
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return -1;
	}

	for(p = servinfo; p != NULL; p = p->ai_next)
	{
		if((clientfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
		{
			perror("client: socket");
			continue;
		}

		if((connect(clientfd, p->ai_addr, p->ai_addrlen)) == -1)
		{
			close(clientfd);
			perror("client: connect");
			continue;
		}
		break;
	}

	freeaddrinfo(servinfo);

	if(p == NULL)
	{
		fprintf(stderr, "client: failed to connect\n");
		return -1;
	}
	else
		return clientfd;
}


// keep reading from the connection until the connection is closed.
int rio_readn(int fd, char *usrbuf, int n)
{
	int nleft = n;
	int nread;
	char *bufp = usrbuf;

	while(nleft > 0)
	{	
		if((nread = read(fd, bufp, nleft)) < 0)
		{		
			return -1;		/* error set by read() */	
		}
		else if(nread == 0)
			break;				/* EOF */
		printf("%s\n", bufp);
		nleft -= nread;
		bufp += nread;
	}
	return (n - nleft);			/* return >=0 */
}

// keep writing to the connection until the end of the content.
int rio_writen(int fd, char *usrbuf, int n) 
{
    int nleft = n;
    int nwritten = 0;
    char *bufp = usrbuf;

    while (nleft > 0) 
    {
		if ((nwritten = write(fd, bufp, nleft)) < 0) 
		{
			return -1;
		}

		nleft -= nwritten;
		bufp += nwritten;
    }
    return (n - nleft);
}

// read from the buffer line by line.
int readline(char *srcbuf, char *dstbuf, int maxlen, int offset) 
{
    int n;
    char *bufp = srcbuf+offset;

    for (n = 0; n < maxlen; n++) 
    { 
    	dstbuf[n] = bufp[n];
    	if (bufp[n] == '\n' || bufp[n] == '\0')
    	{
    		break;
    	}
    }
    return n;
}

void reset_array(int *array)
{
	int index;
	for(index=0; index<sizeof array; index++)
	{
		array[index] = 100;
	}
}

int get_index_of_smallest(int *array, int first, int last)
{
	int min = array[first];
	int index_of_min = first;
	int index;
	for(index = first + 1; index <= last; index++)
	{
		if(array[index] < min)
		{
			min = array[index];
			index_of_min = index;
		}
	}
	return index_of_min;
}

void find_server_ip(char *client_ip, char *server_ip)
{
	int index;
	int clientID = -1;
	int serverID = -1;
	int matrix_index = -1;
	int firstID, secondID, cost;
	int array[MAXSIZE];
	reset_array(array);

	for(index=0; index<topology_info_cnt; index++)
	{
		if(strstr(topology_info[index], client_ip))
		{
			sscanf(topology_info[index], "%d %*s %*s", &clientID);
		}
		if(strstr(topology_info[index], "NUM_LINKS:"))
		{
			matrix_index = index+1;
			break;
		}
	}
	if(clientID<0)
	{
		perror("client ID not found\n");
		exit(1);
	}

	if(matrix_index<0)
	{
		perror("matrix not found\n");
		exit(1);
	}

	while(1)
	{
		int flag = 0;
		for(index=matrix_index; index<topology_info_cnt; index++)
		{
			sscanf(topology_info[index], "%d %d %d\n", &firstID, &secondID, &cost);
			if(firstID == clientID)
			{
				array[secondID] = cost;
				flag = 1;
			}
		}
		if(flag)
		{
			serverID = get_index_of_smallest(array, 0, MAXSIZE - 1);
			reset_array(array);
		}
		else
			break;
	}

	if(serverID < 0)
	{
		perror("serverID not found");
		exit(1);
	}

	int tmpID;
	for(index=0; index<matrix_index; index++)
	{		
		if(strstr(topology_info[index], "SERVER"))
		{
			sscanf(topology_info[index], "%d %*s %s", &tmpID, server_ip);
			if(tmpID == serverID)
			{
				break;
			}
		}
	}
}

// get the ip address.
void *get_in_addr(struct sockaddr *sa)
{
	if(sa->sa_family == AF_INET)
	{
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}
	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}
