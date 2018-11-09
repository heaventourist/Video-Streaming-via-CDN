/*
 * Assignment 2: Video Streaming via CDN
 * Name: Wei Huo
 * JHID: WHUO1
 */
#include <stdlib.h>
#include <string.h>
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

// default buffer size
#define MAXSIZE 400000

typedef struct {
	struct DNSHeader header;
	struct DNSQuestion question;
} DNS_QUERY;

typedef struct {
	struct DNSHeader header;
	struct DNSRecord record;
} DNS_RESPONSE;

// function declaration
int print_log(char *filename, char *log_buffer);
void *get_in_addr(struct sockaddr *sa);
int open_listenfd(char *port);
int open_clientfd(char *hostname, char *port);
int rio_readn(int fd, char *usrbuf, int n);
int rio_writen(int fd, char *usrbuf, int n);
int readline(char *srcbuf, char *dstbuf, int maxlen, int offset);
void selection_sort(int *array, int cnt);
int get_index_of_smallest(int *array, int first, int last);
void swap(int *array, int i, int j);

// Available bitrates for the video.
int available_bitrates[MAXSIZE] = {0};
// Number of available bitrates.
int bitrates_cnt = 0;
int dns_ID = 0;

// Main begins.
int main(int argc, char *argv[])
{
	// If no or more argument is given.
	if(argc == 1 || argc > 7)
	{
		printf("Error: missing or additional arguments\n");
		exit(1);
	}

	// log file name.
	char *log = argv[1];
	// coefficient in the EWMA throughput estimate.
	double alpha = atof(argv[2]);
	// The listen port on proxy.
	char *listen_port = argv[3];
	// buffer for log message before writing to file.
	char *dns_ip = argv[4];
	char *dns_port = argv[5];
	char log_buffer[MAXSIZE] = {0};
	sprintf(log_buffer, "%s\t%s\t%s\t%s\t%s\t%s\n", "<duration/s>", "<tput/kbps>", "<avg-tput/kbps>", "<bitrate/kbps>", "<server-ip>", "<chunk-name>");

	// file descriptor of listen port.
	int listenfd;
	// file descriptor of newly accepted connection.
	int newfd;

	struct sockaddr_storage remoteaddr;
	socklen_t addrlen;

	int i;

	// keep the proxy ip
	char remoteIP[INET6_ADDRSTRLEN];

	// master file descriptor list
	fd_set master;
	// temp file descriptor list for select
	fd_set read_fds;
	// maximum file descriptor number
	int fdmax;

	// clear the master and temp sets
	FD_ZERO(&master);
	FD_ZERO(&read_fds);

	// current throughput.
	double T_cur = 0.0;
	// newly measured throughput.
	double T_new = 0.0;

	// listenning
	if((listenfd = open_listenfd(listen_port)) < 0)
	{
		exit(1);
	}

	// add the listenfd to the master set
	FD_SET(listenfd, &master);

	//keep track of the biggest file descriptor
	fdmax = listenfd;

	// loop start.
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
						printf("selectserver: new connection from %s on socket %d\n",
							tmp_ip, newfd);
					}					
				}
				else
				{
					// general buffer size.
					char buf[MAXSIZE];
					// the number of bytes read or written
					int nbytes;
					// buffer for reading line by line.
					char line_buf[MAXSIZE];
					// buffer for request and request after being extracted out the Get line.
					char remaining_request[MAXSIZE], request[MAXSIZE];
					char method[MAXSIZE], uri[MAXSIZE], version[MAXSIZE];
					char server_ip[MAXSIZE];
					char chunk_name[MAXSIZE];
					memset(buf, 0, MAXSIZE*sizeof(char));
					memset(remaining_request, 0, MAXSIZE*sizeof(char));
					memset(method, 0, MAXSIZE*sizeof(char));
					memset(uri, 0, MAXSIZE*sizeof(char));
					memset(version, 0, MAXSIZE*sizeof(char));
					memset(server_ip, 0, MAXSIZE*sizeof(char));
					memset(chunk_name, 0, MAXSIZE*sizeof(char));
					memset(request, 0, MAXSIZE*sizeof(char));
					// the offset in the buffer for reading next line.
					int offset = 0;
					// file descriptor from connecting the server.
					int server_clientfd;
					int content_length = 0;
					double duration = 0;
					int bitrate;

					clock_t start_time;
					clock_t end_time;
					start_time = clock();
					//handle data from a client
					if((nbytes = read(i, buf, sizeof buf)) <= 0)
					{
						if(nbytes == 0)
						{
							// connecttion closed
							printf("selectorserver: socket %d hung up\n", i);
						}
						else
						{
							perror("recv");
						}
						if(print_log(log, log_buffer) == -1)
						{
							exit(1);
						}
						close(i);
						FD_CLR(i, &master);
					}
					else
					{
						strcpy(request, buf);

						// start parsing the request.
						while(1)
						{
							// initialize the line buffer.
							memset(line_buf, 0, MAXSIZE*sizeof(char));
							nbytes = readline(buf, line_buf, sizeof buf, offset);
							// the first line.
							if(offset == 0)
							{
								sscanf(line_buf, "%s %s %s", method, uri, version);
							}
							else if(nbytes == 0) // done reading.
							{
								break;
							}
							else
							{
								// put the rest of the request in a buffer.
								strcat(remaining_request, line_buf);				
							}
							// plus one because the newline character in not counted in
							offset += nbytes+1;
						}

						if(argc == 7)
						{
							strcpy(server_ip, argv[6]);
							char tmp_port[MAXSIZE] = "80";
							if((server_clientfd = open_clientfd(server_ip, tmp_port)) < 0)
							{
								exit(1);
							}
						}
						else
						{
							DNS_QUERY new_query;

							new_query.header.ID = dns_ID++;
							new_query.header.QR = 0;
							new_query.header.OPCODE = 0;
							new_query.header.AA = 0;
							new_query.header.TC = 0;
							new_query.header.RD = 0;
							new_query.header.RA = 0;
							new_query.header.Z = 0;
							new_query.header.RCODE = 0;
							new_query.header.QDCOUNT = 1;
							new_query.header.ANCOUNT = 0;
							new_query.header.NSCOUNT = 0;
							new_query.header.ARCOUNT = 0;

							strcpy(new_query.question.QNAME, uri);
							new_query.question.QTYPE = 1;
							new_query.question.QCLASS = 1;
							char *sent_query = (char *)&new_query;
							int dns_clientfd;

							if((dns_clientfd = open_clientfd(dns_ip, dns_port)) < 0)
							{
								exit(1);
							}

							if(rio_writen(dns_clientfd, sent_query, sizeof sent_query) == -1)
							{
								perror("dns send");
								exit(1);
							}

							memset(buf, 0, MAXSIZE*sizeof(char)); // initialize the buffer
							if((nbytes = read(dns_clientfd, buf, sizeof buf)) <= 0)
							{
								if(nbytes == 0)
								{
									// connection closed
									break;
								}
								else
								{
									perror("recv");
								}
								close(dns_clientfd);
							}
							else
							{
								DNS_RESPONSE *resp = (DNS_RESPONSE *)buf;
								strcpy(server_ip, resp->record.RDATA);
								char tmp_port[MAXSIZE] = "80";
								if((server_clientfd = open_clientfd(server_ip, tmp_port)) < 0)
								{
									exit(1);
								}
							}
						}
						
						char *revised;
						// when requesting for video chunk, revise the request.
						if(strstr(uri, "Seg") && strstr(uri, "Frag"))
						{
							char tmp_buf[MAXSIZE];
							memset(tmp_buf, 0, MAXSIZE*sizeof(char));
							int index;
							// search for appropriate bit rate.
							for(index = 0; index < bitrates_cnt; index++)
							{
								if(T_cur < 1.5 * available_bitrates[index])
								{
									break;
								}
							}
							// path to the video chunk excluding chunk name.
							char path[MAXSIZE];
							memset(path, 0, MAXSIZE*sizeof(char));
							
							if(index == 0)
							{
								bitrate = available_bitrates[0];
							}
							else
							{
								bitrate = available_bitrates[index-1];
							}
							sscanf(uri, "%[^0-9]%*d%s", path, chunk_name);
							sprintf(line_buf, "%s%d%s", path, bitrate, chunk_name);
							sprintf(tmp_buf, "%s %s %s\r\n", method, line_buf, version);
							strcat(tmp_buf, remaining_request);
							revised = tmp_buf;
						}
						else
						{
							revised = request;
						}

						// send the revised request to the server.
						if((nbytes=write(server_clientfd, revised, strlen(revised))) == -1)
						{
							exit(1);
						}

						memset(buf, 0, MAXSIZE*sizeof(char)); // initialize the buffer
						// the length of the response from the server.
						int bytes_read;
						if((bytes_read = read(server_clientfd, buf, sizeof buf)) < 0)
						{
							perror("recv");
							exit(1);								
						}
						else
						{
							offset = 0;
							while(1)
							{
								memset(line_buf, 0, MAXSIZE*sizeof(char));
								nbytes = readline(buf, line_buf, sizeof line_buf, offset);
								// plus one because the new line character is not counted.
								offset += nbytes+1;
								// pull out the content length.
								if(strstr(line_buf, "Content-Length: "))
								{
									sscanf(line_buf, "Content-Length: %d", &content_length);
								}
								// if the end of the response header
								if(strcmp(line_buf, "\r\n") == 0)
								{
									break;
								}								
							}

							// the number of bytes haven't been read.
							int content_to_read = content_length-(bytes_read - offset);
							// the start pointer of the content waiting to be read.
							char *tmp_tmp_buf = buf+bytes_read; 
							while(content_to_read > 0)
							{	
								if((nbytes = read(server_clientfd, tmp_tmp_buf, content_to_read)) < 0)
								{
									perror("recv");
									exit(1);								
								}
								content_to_read -= nbytes;
								tmp_tmp_buf += nbytes;
							}

							if(strstr(uri, "Seg") && strstr(uri, "Frag"))
							{
								end_time = clock();

								duration = (double)(end_time - start_time)/CLOCKS_PER_SEC;
						
								if(duration != 0)
								{
									// calculate the current throughput.
									T_new = content_length/(duration*1000);
									T_cur = alpha * T_new + (1 - alpha) * T_cur;
								}
							}

							// if the f4m file, pull out the available bitrates for the video.
							if(strstr(uri, ".f4m"))
							{
								offset = 0;
								while(1)
								{
									memset(line_buf, 0, MAXSIZE*sizeof(char));
									nbytes = readline(buf, line_buf, sizeof line_buf, offset);
									offset += nbytes+1;
									if(strstr(line_buf, "bitrate"))
									{
										int tmp;
										sscanf(line_buf, " bitrate=\"%d\" ", &tmp);
										available_bitrates[bitrates_cnt++] = tmp;
									}

									if(strstr(line_buf, "</manifest>"))
									{
										break;
									}
								}
								int z;
								for(z = 0; z < bitrates_cnt; z++)
								{
									printf("%d ", available_bitrates[z]);
								}
								printf("\n");
								char tmp_buf[MAXSIZE];
								// sort the bitrates.
								selection_sort(available_bitrates, bitrates_cnt);
								// get the nolist.f4m file from the server.
								sscanf(uri, "%[^.].%*s", line_buf);
								strcat(line_buf, "_nolist.f4m");
								sprintf(tmp_buf, "%s %s %s\r\n", method, line_buf, version);
								strcat(tmp_buf, remaining_request);

								if(rio_writen(server_clientfd, tmp_buf, strlen(tmp_buf)) < 0)
								{
									perror("proxy send");
									exit(1);
								}

								memset(buf, 0, MAXSIZE*sizeof(char));
								if((nbytes = read(server_clientfd, buf, sizeof buf)) < 0)
								{
									perror("proxy recv");
									exit(1);								
								}
							}
							
							// sent the response to the browser.
							if((nbytes=rio_writen(i, buf, sizeof buf)) < 0)
							{
								perror("proxy send");
								//exit(1);
							}

							// print to the log file.
							if(strlen(chunk_name) != 0)
							{
								sprintf(log_buffer+strlen(log_buffer), "%f\t%10.2f\t%10.2f\t%4d\t%s\t%s\n", duration, T_new, T_cur, bitrate, server_ip, chunk_name);
							}	
						}
						close(server_clientfd);						
					}	
				}
			}
		}
	}
	// loop end.	
}
// Main end.

// print the content in the log buffer to the disk.
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

// selection sort.
void selection_sort(int *array, int cnt)
{
	int index;
	for(index = 0; index < cnt; index++)
	{
		int index_of_next_smallest = get_index_of_smallest(array, index, cnt-1);
		swap(array, index, index_of_next_smallest);
	}
}

// helper function for selection sort.
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

// helper function for selection sort.
void swap(int *array, int i, int j)
{
	int tmp = array[i];
	array[i] = array[j];
	array[j] = tmp;
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