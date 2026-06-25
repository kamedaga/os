struct __pachaos_filed_fd_entry {
	unsigned char used;
	unsigned long long handle;
};

struct __pachaos_filed_fd_entry __pachaos_filed_fds[128];
unsigned long long __pachaos_filed_request_id = 100;
