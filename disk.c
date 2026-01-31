
#include "taskxtinfo.h"
#include <linux/kernel.h>


ssize_t write_vaddr_disk(void *, size_t);
int setup_disk(void);
void cleanup_disk(void);
int log_to_file(const char *message);

static void disable_dio(void);

static struct file * f = NULL;
extern char * filepath;
extern int dio;
static int reopen = 0;

static void disable_dio() {
	DBG("Direct IO may not be supported on this file system. Retrying.");
	dio = 0;
	reopen = 1;
	cleanup_disk();
	setup_disk();
}

int setup_disk() {
	int err;
	
	pr_info("[TaskXT] setup_disk: filepath=%s, dio=%d\n", filepath, dio);
	log_to_file("[TaskXT] setup_disk starting");
	
	if (dio && reopen) {	
		f = filp_open(filepath, O_WRONLY | O_CREAT | O_LARGEFILE | O_SYNC | O_DIRECT, 0444);
	} else if (dio) {
		f = filp_open(filepath, O_WRONLY | O_CREAT | O_LARGEFILE | O_TRUNC | O_SYNC | O_DIRECT, 0444);
	}
	
	if(!dio || (f == ERR_PTR(-EINVAL))) {
		DBG("Direct IO Disabled");
		pr_info("[TaskXT] setup_disk: opening without DIO\n");
		f = filp_open(filepath, O_WRONLY | O_CREAT | O_LARGEFILE | O_TRUNC, 0644);
		dio = 0;
	}

	if (!f || IS_ERR(f)) {
		err = (f) ? PTR_ERR(f) : -EIO;
		pr_err("[TaskXT] setup_disk ERROR: filp_open failed, err=%ld, filepath=%s\n", PTR_ERR(f), filepath);
		log_to_file("[TaskXT] setup_disk filp_open FAILED");
		DBG("Error opening file %ld", PTR_ERR(f));
		f = NULL;
		return err;
	}

	pr_info("[TaskXT] setup_disk: file opened successfully\n");
	log_to_file("[TaskXT] setup_disk file opened");
	return 0;
}

void cleanup_disk() {
	if(f) {
		pr_info("[TaskXT] cleanup_disk: closing file\n");
		filp_close(f, NULL);
		f = NULL;
		log_to_file("[TaskXT] File closed");
	}
}

ssize_t write_vaddr_disk(void * v, size_t is) {
	ssize_t s;
	loff_t pos;

	if (!f) {
		pr_err("[TaskXT] write_vaddr_disk ERROR: file handle is NULL\n");
		log_to_file("[TaskXT] write_vaddr_disk file NULL");
		return -EIO;
	}

	pos = f->f_pos;

	s = kernel_write(f, v, is, &pos);

	if (s == is) {
		f->f_pos = pos;
	}

	if (s != is) {
		pr_warn("[TaskXT] write_vaddr_disk: partial write (wanted %lu, got %zd)\n", is, s);
		if (dio) {
			disable_dio();
			f->f_pos = pos;
			return write_vaddr_disk(v, is);
		}
	}

	return s;
}

// Efficient file logging for troubleshooting
int log_to_file(const char *message) {
	struct file *log_f;
	char log_msg[256];
	ssize_t s;
	loff_t pos = 0;
	
	// Format message with timestamp hint
	snprintf(log_msg, sizeof(log_msg), "[%lu] %s\n", jiffies, message);
	
	// Open log file in append mode
	log_f = filp_open("/tmp/taskxtv4.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	
	if (IS_ERR(log_f)) {
		pr_err("[TaskXT] Failed to open log file: %ld\n", PTR_ERR(log_f));
		return PTR_ERR(log_f);
	}
	
	// Write to log
	s = kernel_write(log_f, log_msg, strlen(log_msg), &pos);
	
	// Close log file
	filp_close(log_f, NULL);
	
	return 0;
}
