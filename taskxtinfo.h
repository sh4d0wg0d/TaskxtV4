#ifndef __TASKXTINFO_H_
#define __TASKXTINFO_H_

#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/highmem.h>
#include <linux/pfn.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/string.h>
#include <linux/ioctl.h>
#include <net/sock.h>
#include <net/tcp.h>

#define TASKXT_DEBUG 1
#define TASKXT_MAGIC 0x4C694D4F //TASKXT

#ifdef TASKXT_DEBUG
#define DBG(fmt, args...) do { printk("[TASKXT] "fmt"\n", ## args); } while (0)
#else
#define DBG(fmt, args...) do {} while(0)
#endif

//structures



typedef struct {
   char pid[5000000];
   int cnt;
//	unsigned int magic;
//	unsigned int version;
//	unsigned long long s_addr;
//	unsigned long long e_addr;
//	unsigned char reserved[8];
} __attribute__ ((__packed__)) task_features;



#endif
