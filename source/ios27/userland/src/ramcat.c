/* Bounded first-boot diagnostic: read text logs from the RAM data volume. */
#include "mini.h"
int main(int argc, char **argv, char **envp)
{
	unsigned delay = 0;
	(void)envp;
	if (argc < 3) return 2;
	for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) delay = delay * 10 + *p - '0';
	if (delay > 120) return 2;
	for (unsigned n = 0; n < delay * 5; n++) usleep(200000);
	for (int n = 2; n < argc; n++) {
		char buf[4096];
		unsigned total = 0;
		int fd = open(argv[n], O_RDONLY, 0);
		printf("\n=== RAM diagnostic %s ===\n", argv[n]);
		if (fd < 0) { printf("open failed: %s\n", strerror(errno)); continue; }
		while (total < 131072) {
			ssize_t count = read(fd, buf, sizeof(buf));
			if (count <= 0) break;
			write(1, buf, (size_t)count);
			total += (unsigned)count;
		}
		close(fd);
	}
	return 0;
}
