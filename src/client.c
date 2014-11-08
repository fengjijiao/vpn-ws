#include "vpn-ws.h"

#include <netdb.h>
#include <sys/poll.h>

static struct option vpn_ws_options[] = {
        {"exec", required_argument, 0, 1 },
        {NULL, 0, 0, 0}
};

void vpn_ws_client_destroy(vpn_ws_peer *peer) {
	if (vpn_ws_conf.ssl_ctx) {
		vpn_ws_ssl_close(vpn_ws_conf.ssl_ctx);
	}
	vpn_ws_peer_destroy(peer);
}

int vpn_ws_client_read(vpn_ws_peer *peer, uint64_t amount) {
        uint64_t available = peer->len - peer->pos;
        if (available < amount) {
                peer->len += amount;
                void *tmp = realloc(peer->buf, peer->len);
                if (!tmp) {
                        vpn_ws_error("vpn_ws_client_read()/realloc()");
                        return -1;
                }
                peer->buf = tmp;
        }
	ssize_t rlen = -1;

	if (vpn_ws_conf.ssl_ctx) {
	}
	else {
		rlen = read(peer->fd, peer->buf + peer->pos, amount);
	}
        if (rlen <= 0) return -1;
        peer->pos += rlen;

        return 0;
}


int vpn_ws_rnrn(char *buf, size_t len) {
	if (len < 17) return 0;
	uint8_t status = 0;
	size_t i;
	for(i=0;i<len;i++) {
		if (status == 0) {
			if (buf[i] == '\r') {
				status = 1;
				continue;
			}
		}
		else if (status == 1) {
			if (buf[i] == '\n') {
				status = 2;
				continue;
			}
		}
		else if (status == 2) {
			if (buf[i] == '\r') {
				status = 3;
				continue;
			}
		}
		else if (status == 3) {
			if (buf[i] == '\n') {
                                status = 4;
				break;
                        }
		}
                status = 0;
	}
	if (status != 4) return 0;
	int code = 100 * (buf[9] - 48);
        code += 10 * (buf[10] - 48);
        code += buf[11] - 48;
	return code;
}

int vpn_ws_wait_101(int fd, void *ssl) {
	char buf[8192];
	size_t remains = 8192;

	for(;;) {
		if (!ssl) {
			ssize_t rlen = read(fd, buf + (8192-remains), remains);
			if (rlen <= 0) return -1;
			remains -= rlen;
		}

		int code = vpn_ws_rnrn(buf, 8192-remains);
		if (code) return code;
	}
}

int vpn_ws_full_write(int fd, char *buf, size_t len) {
	size_t remains = len;
	char *ptr = buf;
	while(remains > 0) {
		ssize_t wlen = write(fd, ptr, remains);
		if (wlen <= 0) return -1;
		ptr += wlen;
		remains -= wlen;
	}
	return 0;
}

int vpn_ws_client_write(vpn_ws_peer *peer, uint8_t *buf, uint64_t len) {
	if (vpn_ws_conf.ssl_ctx) {
	}
	return vpn_ws_full_write(peer->fd, (char *)buf, len);
}


int vpn_ws_connect(char *name) {
	int ssl = 0;
	uint16_t port = 80;
	if (strlen(name) < 6) {
		vpn_ws_log("invalid websocket url: %s\n", name);
		return -1;
	}

	if (!strncmp(name, "wss://", 6)) {
		ssl = 1;
		port = 443;
	}
	else if (!strncmp(name, "ws://", 5)) {
		ssl = 0;
		port = 80;
	}
	else {
		vpn_ws_log("invalid websocket url: %s (requires ws:// or wss://)\n", name);
		return -1;
	}

	char *path = NULL;

	// now get the domain part
	char *domain = name + 5 + ssl;
	size_t domain_len = strlen(domain);
	char *slash = strchr(domain, '/');
	if (slash) {
		domain_len = slash - domain;
		domain[domain_len] = 0;
		path = slash + 1;
	}

	// check for basic auth
	char *at = strchr(domain, '@');
	if (at) {
		*at = 0;
		domain = at+1;
		domain_len = strlen(domain);
	}

	// check for port
	char *port_str = strchr(domain, ':');
	if (port_str) {		
		*port_str = 0;
		domain_len = strlen(domain);
		port = atoi(port_str+1);
	}

	vpn_ws_log("connecting to %s port %u (transport: %s)\n", domain, port, ssl ? "wss": "ws");

	// resolve the domain
	struct hostent *he = gethostbyname(domain);
	if (!he) {
		vpn_ws_error("vpn_ws_connect()/gethostbyname()");
		return -1;
	}

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		vpn_ws_error("vpn_ws_connect()/socket()");
		return -1;
	}

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(struct sockaddr_in));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr = *((struct in_addr *) he->h_addr);

	if (connect(fd, (struct sockaddr *) &sin, sizeof(struct sockaddr_in)) < 0) {
		vpn_ws_error("vpn_ws_connect()/connect()");
		close(fd);
		return -1;
	}

	char *auth = NULL;

	if (at) {
		auth = vpn_ws_calloc(23 + (strlen(at+1) * 2));
		if (!auth) {
			close(fd);
                	return -1;
		}
		memcpy(auth, "Authorization: Basic ", 21);
		uint16_t auth_len = vpn_ws_base64_encode((uint8_t *)at+1, strlen(at+2), (uint8_t *)auth + 21);
		memcpy(auth + 21 + auth_len, "\r\n", 2); 
	}

	uint8_t key[32];
	uint8_t secret[10];
	int i;
	for(i=0;i<10;i++) secret[i] = rand();
	uint16_t key_len = vpn_ws_base64_encode(secret, 10, key);
	// now build and send the request
	char buf[8192];
	int ret = snprintf(buf, 8192, "GET %s%s HTTP/1.1\r\nHost: %s%s%s\r\n%sUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %.*s\r\n\r\n",
		path ? "/" : "",
		path ? path : "",
		domain,
		port_str ? ":" : "",
		port_str ? port_str+1 : "",
		auth ? auth : "",
		key_len,
		key);

	if (auth) free(auth);

	if (ret == 0 || ret > 8192) {
		vpn_ws_log("vpn_ws_connect()/snprintf()");
		close(fd);
		return -1;
	}

	void *ctx = NULL;

	if (ssl) {
		ctx = vpn_ws_ssl_handshake(fd, domain, NULL, NULL);
		if (!ctx) {
			close(fd);
			return -1;
		}
		if (vpn_ws_ssl_write(ctx, (uint8_t *)buf, ret)) {
			vpn_ws_ssl_close(ctx);
			close(fd);
			return -1;
		}
	}
	else {
		if (vpn_ws_full_write(fd, buf, ret)) {
			close(fd);
			return -1;
		}
	}

	int http_code = vpn_ws_wait_101(fd, ctx);
	if (http_code != 101) {
		vpn_ws_log("error, websocket handshake returned code: %d\n", http_code);
		if (ctx) vpn_ws_ssl_close(ctx);
		close(fd);
		return -1;
	}

	vpn_ws_log("connected to %s port %u (transport: %s)\n", domain, port, ssl ? "wss": "ws");
	return fd;
}

int main(int argc, char *argv[]) {


	sigset_t sset;
        sigemptyset(&sset);
        sigaddset(&sset, SIGPIPE);
        sigprocmask(SIG_BLOCK, &sset, NULL);

	int option_index = 0;
	for(;;) {
                int c = getopt_long(argc, argv, "", vpn_ws_options, &option_index);
                if (c < 0) break;
                switch(c) {
                        // tuntap
                        case 1:
                                vpn_ws_conf.exec = optarg;
                                break;
                        case '?':
                                break;
                        default:
                                vpn_ws_log("error parsing arguments\n");
                                vpn_ws_exit(1);
                }
        }

	if (optind + 1 >= argc) {
		vpn_ws_log("syntax: %s <tap> <ws>\n", argv[0]);
		vpn_ws_exit(1);
	}

	vpn_ws_conf.tuntap = argv[optind];
	vpn_ws_conf.server_addr = argv[optind+1];

	// initialize rnd engine
	struct timeval tv;
	gettimeofday(&tv, NULL);
	srand((unsigned int) (tv.tv_usec * tv.tv_sec));


	int tuntap_fd = vpn_ws_tuntap(vpn_ws_conf.tuntap);
	if (tuntap_fd < 0) {
		vpn_ws_exit(1);
	}

	if (vpn_ws_conf.exec) {
		if (vpn_ws_exec(vpn_ws_conf.exec)) {
			vpn_ws_exit(1);
		}
	}

	vpn_ws_peer *peer = NULL;

	int throttle = -1;
	// back here whenever the server disconnect
reconnect:
	if (throttle >= 30) throttle = 0;
	throttle++;
	if (throttle) sleep(throttle);

	peer = vpn_ws_calloc(sizeof(vpn_ws_peer));
        if (!peer) {
		goto reconnect;
        }

	peer->fd = vpn_ws_connect(vpn_ws_conf.server_addr);
	if (peer->fd < 0) {
		vpn_ws_client_destroy(peer);
		goto reconnect;
	}
	memcpy(peer->mac, vpn_ws_conf.tuntap_mac, 6);

	struct pollfd pfd[2];
	pfd[0].fd = peer->fd;
	pfd[0].events = POLLIN;
	pfd[1].fd = tuntap_fd;
	pfd[1].events = POLLIN;

	uint8_t mask[4];
	mask[0] = rand();
	mask[1] = rand();
	mask[2] = rand();
	mask[3] = rand();

	for(;;) {
		// we send a websocket ping every 17 seconds (if inactive, should be enough
		// for every proxy out there)
		int ret = poll(pfd, 2, 17 * 1000);
		if (ret < 0) {
			// the process manager will save us here
			vpn_ws_error("main()/poll()");
			vpn_ws_exit(1);
		}

		// too much inactivity, send a ping
		if (ret == 0) {
			if (vpn_ws_client_write(peer, (uint8_t *) "\x89\x00", 2)) {
				vpn_ws_client_destroy(peer);
                		goto reconnect;
			}			
		}

		if (pfd[0].revents) {
			printf("data from server\n");
		}

		
		if (pfd[1].revents) {
			// we use this buffer for the websocket packet too
			// 2 byte header + 2 byte size + 4 bytes masking + mtu
			uint8_t mtu[8+1500];
			ssize_t rlen = read(tuntap_fd, mtu+8, 1500);
			if (rlen <= 0) {
				vpn_ws_error("main()/read()");
                        	vpn_ws_exit(1);
			}

			// mask packet
			ssize_t i;
			for (i=0;i<rlen;i++) {
                        	mtu[8+i] = mtu[8+i] ^ mask[i % 4];
			}

			mtu[4] = mask[0];
                        mtu[5] = mask[1];
                        mtu[6] = mask[2];
                        mtu[7] = mask[3];

			if (rlen < 126) {
				mtu[2] = 0x82;
				mtu[3] = rlen | 0x80;
				if (vpn_ws_client_write(peer, mtu + 2, rlen + 6)) {
					vpn_ws_client_destroy(peer);
					goto reconnect;
				}
			}
			else {
				mtu[0] = 0x82;
				mtu[1] = 126 | 0x80;
				mtu[2] = (uint8_t) ((rlen >> 8) & 0xff);
				mtu[3] = (uint8_t) (rlen & 0xff);
				if (vpn_ws_client_write(peer, mtu, rlen + 8)) {
					vpn_ws_client_destroy(peer);
					goto reconnect;
				}
			}
		}

	}

	return 0;
}
