/* List of features extracted using LM for kernel version 3.13 */
// map_count -> number of memory regions of a process
// page_table_lock -> used to mange the page table entries
// hiwater_rss -> Max number of page frames ever owned by the process
// hiwater_vm -> Max number of pages appeared in memory region of process
// total_vm -> Size of process's address space in terms of number of pages
// shared_vm -> Number of pages in shared file memory mappings of process
// exec_vm -> number of pages in executable memory mappings of process
// nr_ptes -> number of pages tables of a process
// utime -> Tick count of a process that is executing in user mode
// stime -> Tick count of a process in the kernel mode
// nvcsw -> number of volunter context switches
// nivcsw -> number of in-volunter context switches
// min_flt -> Contains the minor page faults
// alloc_lock.raw_lock.slock -> used to locl memory manager, files and file system etc.
// fs.count - > number of file usage (was count, now field called users)
 
#include <linux/module.h>
#include <linux/kernel.h> /* printk() */
#include <linux/errno.h>  /* error codes */
#include <linux/sched.h>
#include <linux/fs_struct.h>

#include <linux/types.h>   // for dev_t typedef
#include <linux/kdev_t.h>  // for format_dev_t
#include <linux/fs.h>      // for alloc_chrdev_region()

#include <linux/kthread.h>

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include "taskxtinfo.h"
#include "taskxt.h"

static char buffer[64];         // optional, for debugging
static dev_t dev;               // (major,minor) value
static struct cdev c_dev;
static struct class *cl;
static int sampling = 0;
static int fileOpen = 0;

static struct task_struct *thread_st = 0;

// operational information for driver
static  taskxt_arg_t q;

// The prototype functions for the character driver -- must come before the struct definition
static int     taskxt_open(struct inode *i, struct file *f);
static int     taskxt_close(struct inode *, struct file *);
static ssize_t taskxt_read(struct file *, char *, size_t, loff_t *);
static ssize_t taskxt_write(struct file *, const char *, size_t, loff_t *);
static long    taskxt_ioctl(struct file *f, unsigned int cmd, unsigned long arg);
static int writeFormatData(task_features *tf);
//static int runExtractor(void*);

static int taskActive(char *pname);
static int extract_features(void*);
static int write_buffer(char* buff);
//static int write_task_feature(task_features *);
static ssize_t write_vaddr(void *, size_t);
static int setup(void);
static void cleanup(void);

//External
extern int write_vaddr_disk(void *, size_t);
extern int setup_disk(void);
extern void cleanup_disk(void);
extern int log_to_file(const char *message);

char *path = 0;
char *filepath = 0;
static char fullFileName[1024];
static char ProcessName[50];

char *pname = 0;
int srate = 0;
int dura = 0;
int dio = 0;
int Ptype = 0;

//static int changed_process = 0;

//char storageFeatureBuffer[5000000]; 
//int storageFeatureIndex =0;
task_features tf;

// Multi-process support
#define MAX_MONITORED_PROCESSES 10
typedef struct {
    char name[50];
    int active;
    task_features data;
} monitored_process_t;

static monitored_process_t monitored_processes[MAX_MONITORED_PROCESSES];
static int num_monitored_processes = 0;
static char current_process[50] = {0};
static spinlock_t process_lock;

//sysfs attribute functions for runtime control
static ssize_t sampling_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    return sprintf(buf, "%d\n", sampling);
}

static ssize_t sampling_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) {
    int val;
    if (sscanf(buf, "%d", &val) != 1) return -EINVAL;
    
    if (val == 1 && !sampling) {
        pr_info("[TaskXT] Starting sampling for process: %s\n", pname);
        sampling = 1;
    } else if (val == 0 && sampling) {
        pr_info("[TaskXT] Stopping sampling\n");
        sampling = 0;
    }
    return count;
}

static ssize_t thread_status_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    return sprintf(buf, "thread_active=%d sampling=%d samples_collected=%d\n", 
                   thread_st ? 1 : 0, sampling, tf.cnt);
}

static ssize_t pname_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    int i, pos = 0;
    spin_lock(&process_lock);
    for (i = 0; i < num_monitored_processes; i++) {
        pos += sprintf(buf + pos, "%s%s", monitored_processes[i].name, 
                      (i < num_monitored_processes - 1) ? "," : "");
    }
    if (pos == 0) {
        pos = sprintf(buf, "(none)");
    }
    spin_unlock(&process_lock);
    return pos + 1; // include newline terminator
}

static ssize_t pname_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) {
    char temp_pname[50];
    int i, found = 0;
    
    if (count > 49) return -EINVAL;
    sscanf(buf, "%49s", temp_pname);
    
    spin_lock(&process_lock);
    
    // Check if process already in list
    for (i = 0; i < num_monitored_processes; i++) {
        if (!strcmp(monitored_processes[i].name, temp_pname)) {
            found = 1;
            monitored_processes[i].active = 1;
            pr_info("[TaskXT] Process %s re-activated\n", temp_pname);
            break;
        }
    }
    
    // Add new process if not found and room available
    if (!found && num_monitored_processes < MAX_MONITORED_PROCESSES) {
        strcpy(monitored_processes[num_monitored_processes].name, temp_pname);
        monitored_processes[num_monitored_processes].active = 1;
        monitored_processes[num_monitored_processes].data.cnt = 0;
        monitored_processes[num_monitored_processes].data.pid[0] = 0;
        num_monitored_processes++;
        pr_info("[TaskXT] Process name added: %s (total: %d)\n", temp_pname, num_monitored_processes);
    } else if (!found) {
        spin_unlock(&process_lock);
        pr_warn("[TaskXT] Maximum processes reached (%d)\n", MAX_MONITORED_PROCESSES);
        return -ENOMEM;
    }
    
    spin_unlock(&process_lock);
    return count;
}

static ssize_t srate_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    return sprintf(buf, "%d\n", srate);
}

static ssize_t srate_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) {
    int val;
    if (sscanf(buf, "%d", &val) != 1 || val <= 0) return -EINVAL;
    srate = val;
    pr_info("[TaskXT] Sampling rate updated to: %d ms\n", srate);
    return count;
}

static ssize_t dura_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    return sprintf(buf, "%d\n", dura);
}

static ssize_t dura_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) {
    int val;
    if (sscanf(buf, "%d", &val) != 1 || val <= 0) return -EINVAL;
    dura = val;
    pr_info("[TaskXT] Duration updated to: %d ms\n", dura);
    return count;
}

static struct kobj_attribute sampling_attr = __ATTR(sampling, 0644, sampling_show, sampling_store);
static struct kobj_attribute status_attr = __ATTR(status, 0444, thread_status_show, NULL);
static struct kobj_attribute pname_attr = __ATTR(pname_control, 0644, pname_show, pname_store);
static struct kobj_attribute srate_attr = __ATTR(srate_control, 0644, srate_show, srate_store);
static struct kobj_attribute dura_attr = __ATTR(dura_control, 0644, dura_show, dura_store);

static struct attribute *taskxt_attrs[] = {
    &sampling_attr.attr,
    &status_attr.attr,
    &pname_attr.attr,
    &srate_attr.attr,
    &dura_attr.attr,
    NULL,
};

static struct attribute_group taskxt_attr_group = {
    .attrs = taskxt_attrs,
};

static struct kobject *taskxt_kobj;

static struct file_operations query_fops =
{
    .owner = THIS_MODULE,
    .open = taskxt_open,
    .release = taskxt_close,
    .write = taskxt_write,
    .read = taskxt_read,
    .unlocked_ioctl = taskxt_ioctl
};


module_param(path, charp, 0644);
MODULE_PARM_DESC(path, "Directory path to save output file");
module_param(pname, charp, 0644);
MODULE_PARM_DESC(pname, "Process name to monitor");
module_param(srate, int, 0644);
MODULE_PARM_DESC(srate, "Sampling rate in milliseconds");
module_param(dura, int, 0644);
MODULE_PARM_DESC(dura, "Duration to collect samples in milliseconds");

 
MODULE_AUTHOR("Alex Pons");
MODULE_DESCRIPTION("Taskxt V3 Char Driver");
MODULE_LICENSE("GPL");

/* Declaration of functions */
void device_exit(void);
int device_init(void);
   
/* Declaration of the init and exit routines */
module_init(device_init);
module_exit(device_exit);
 


int device_init(void)
{
   int ret = 0;
   struct device *dev_ret;
   int err = 0;
 
   pr_info("[TaskXT] Init module starting\n");
   log_to_file("[TaskXT] Module init");
   printk(KERN_NOTICE "taskxt: init module\n");

   if(!path) {
     pr_err("[TaskXT] No path parameter specified\n");
     log_to_file("[TaskXT] ERROR: No path parameter");
     DBG("No path parameter specified");
     return -EINVAL;
   }

   if(!pname) {
     pr_err("[TaskXT] No Process Name parameter specified\n");
     log_to_file("[TaskXT] ERROR: No process name parameter");
     DBG("No Process Name or PID parameter specified");
     return -EINVAL;
   }

   if(!srate) {
     pr_err("[TaskXT] No Sample Rate parameter specified\n");
     log_to_file("[TaskXT] ERROR: No sample rate parameter");
     DBG("No Sample Rate parameter specified");
     return -EINVAL;
   }

   if(!dura) {
     pr_err("[TaskXT] No Duration parameter specified\n");
     log_to_file("[TaskXT] ERROR: No duration parameter");
     DBG("No Duration parameter specified");
     return -EINVAL;
   }

   pr_info("[TaskXT] Parameters validated: path=%s, pname=%s, srate=%d, dura=%d\n", path, pname, srate, dura);
   log_to_file("[TaskXT] Parameters OK - watching for process");

   // Initialize multi-process tracking
   spin_lock_init(&process_lock);
   memset(monitored_processes, 0, sizeof(monitored_processes));
   num_monitored_processes = 0;
   
   // Add initial process from module parameter
   strcpy(monitored_processes[0].name, pname);
   monitored_processes[0].active = 1;
   monitored_processes[0].data.cnt = 0;
   monitored_processes[0].data.pid[0] = 0;
   num_monitored_processes = 1;
   pr_info("[TaskXT] Initial process added: %s\n", pname);

   if ((ret = alloc_chrdev_region(&dev, 0, 1, "taskxt")) < 0)
   {
      return ret;
   }
   printk(KERN_INFO "major & minor: %s\n", format_dev_t(buffer, dev));
 
   cdev_init(&c_dev, &query_fops);
 
   if ((ret = cdev_add(&c_dev, dev, 1)) < 0)
   {
      return ret;
   }

   if (IS_ERR(cl = class_create("char")))
   {
      cdev_del(&c_dev);
      unregister_chrdev_region(dev, 1);
      return PTR_ERR(cl);
   }
   if (IS_ERR(dev_ret = device_create(cl, NULL, dev, NULL, "taskxt")))
   {
      class_destroy(cl);
      cdev_del(&c_dev);
      unregister_chrdev_region(dev, 1);
      return PTR_ERR(dev_ret);
   }

   // Create sysfs attributes for runtime control
   taskxt_kobj = kobject_create_and_add("taskxt", kernel_kobj);
   if (!taskxt_kobj) {
      pr_err("[TaskXT] Failed to create sysfs object\n");
      device_destroy(cl, dev);
      class_destroy(cl);
      cdev_del(&c_dev);
      unregister_chrdev_region(dev, 1);
      return -ENOMEM;
   }
   
   ret = sysfs_create_group(taskxt_kobj, &taskxt_attr_group);
   if (ret) {
      pr_err("[TaskXT] Failed to create sysfs group: %d\n", ret);
      kobject_put(taskxt_kobj);
      device_destroy(cl, dev);
      class_destroy(cl);
      cdev_del(&c_dev);
      unregister_chrdev_region(dev, 1);
      return ret;
   }
   
   pr_info("[TaskXT] sysfs control attributes available at /sys/kernel/taskxt/\n");


   printk(KERN_INFO "Creating Thread\n");
   //Create the kernel thread with name 'samp'
   DBG("Creating Thread");
   //Create the kernel thread with name 'samp'
   thread_st = kthread_run(extract_features, NULL, "samp");  //kthread_create(thread_fn, NULL, "mythread");
   if (thread_st) {
      pr_info("[TaskXT] Thread created successfully\n");
      DBG("Thread Created successfully");
   } 
   else {
      pr_err("[TaskXT] Thread creation FAILED\n");
      log_to_file("[TaskXT] ERROR: Thread creation failed");
      DBG("Thread creation failed");
      return -ENOMEM;
   }

   sampling = 1; // enables the 

   return err;
}

void device_exit(void) {
   pr_info("[TaskXT] Module exit starting\n");
   log_to_file("[TaskXT] Module exit");
   printk(KERN_NOTICE "taskxt: exiting module\n");

   // Stop sampling FIRST
   pr_info("[TaskXT] Stopping sampling\n");
   sampling = 0;

   // Give the sampling a moment to stop
   msleep(100);

   // Stop the extraction thread
   if (thread_st) {
       pr_info("[TaskXT] Stopping thread\n");
       kthread_stop(thread_st);
       pr_info("[TaskXT] Thread stopped\n");
       thread_st = NULL;
   }

   // Close any open files
   if (fileOpen) {
       pr_info("[TaskXT] Cleaning up disk I/O\n");
       cleanup_disk();
       fileOpen = 0;
   }

   // Remove sysfs attributes BEFORE destroying device
   pr_info("[TaskXT] Removing sysfs\n");
   if (taskxt_kobj) {
      sysfs_remove_group(taskxt_kobj, &taskxt_attr_group);
      kobject_put(taskxt_kobj);
      taskxt_kobj = NULL;
   }

   // Clean up character device last
   pr_info("[TaskXT] Removing character device\n");
   device_destroy(cl, dev);
   
   pr_info("[TaskXT] Destroying device class\n");
   if (cl) {
       class_destroy(cl);
       cl = NULL;
   }
   
   pr_info("[TaskXT] Removing cdev\n");
   cdev_del(&c_dev);
   
   pr_info("[TaskXT] Unregistering chrdev\n");
   unregister_chrdev_region(dev, 1);
   
   pr_info("[TaskXT] Module exit complete\n");
}

static int taskxt_open(struct inode *i, struct file *f)
{
    return 0;
}

static int taskxt_close(struct inode *i, struct file *f)
{
    return 0;
}
static ssize_t taskxt_read(struct file *filep, char *buffer, size_t len, loff_t *offset){
   printk(KERN_INFO "taskxt Reading values\n");
   //printk(KERN_INFO "%s\n", format_dev_t(buffer, dev));
   return 0;    
}
 
static ssize_t taskxt_write(struct file *filep, const char *buffer, size_t len, loff_t *offset){
   printk(KERN_INFO "taskxt Writing values\n");
   return 0;
}

/*
static int isPid(char *buff) {
   int i; 
   int f = 1; //is pid
   
   for (i = 0; buff[i] != 0; i++) {
      if (buff[i] < 30 || buff[i] > 39) { f = 0; break; }
   }
   return f;
}
*/

static long taskxt_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
   int err = 0;

   switch (cmd)
   {
      case TASKXT_GET_PROCESSNAME:
         // check if process started, if started set status = 1
         q.status = 0;
         if (taskActive(q.processName)) {
            sampling = 1;
            q.status = 1;
         }
         if (copy_to_user((taskxt_arg_t *)arg, &q, sizeof(taskxt_arg_t)))
         {
               return -EACCES;
         }
         break;
      case TASKXT_SMP_PROCESSNAME:
         if (sampling == 1) {
            err = extract_features(q.processName);
            if (err) return err;
         }
         break;
      case TASKXT_SET_PROCESSNAME:
         if (copy_from_user(&q, (taskxt_arg_t *)arg, sizeof(taskxt_arg_t)))
         {
               return -EACCES;
         }

         strcpy(ProcessName, q.processName);
         DBG("New Process: %s", ProcessName);
         sampling = 1;
         break;
      default:
         return -EINVAL;
    }
    return 0;
}

static ssize_t write_vaddr(void * v, size_t is) {
   return (write_vaddr_disk(v, is));
}

static int setup(void) {
   return (setup_disk());
}

static void cleanup(void) {
   return (cleanup_disk());
}

static int extract_features(void*n)
{
   sample val;
   int err = 0;
   struct task_struct *task; 
   int iterations;
   int loop, i;
   ktime_t startTime; 
   s64 timeTaken_us;
   int delayAmtMin, delayAmtMax;  
   int found;
   int len = 0;
   char fullFileName[1024];
   char prev_process[50] = {0};

   fileOpen = 1;   

   iterations = dura / srate;
   delayAmtMin = 100 * srate - 5;
   delayAmtMax = 100 * srate + 5;  

   DBG("In extract_features");
   
   // Allow the SIGKILL signal
   allow_signal(SIGKILL);

   while (!kthread_should_stop()) {
      if (signal_pending(current)) break;
      usleep_range(delayAmtMin, delayAmtMax);
      
      if (sampling) {
         // Check for any active monitored processes
         spin_lock(&process_lock);
         
         for (i = 0; i < num_monitored_processes; i++) {
            if (!monitored_processes[i].active) continue;
            
            // Look for this process
            found = 0;
            for_each_process(task) {
               if (!strcmp(task->comm, monitored_processes[i].name)) {
                  found = 1;
                  break;
               }
            }

            if (found) {
               // Check if we switched processes
               if (strcmp(prev_process, monitored_processes[i].name) != 0) {
                  pr_info("[TaskXT] Process FOUND: %s\n", monitored_processes[i].name);
                  log_to_file("[TaskXT] Process FOUND");
                  strcpy(prev_process, monitored_processes[i].name);
                  monitored_processes[i].data.pid[0] = 0;
                  monitored_processes[i].data.cnt = 0;
               }

               DBG("In extract_features found process: %s", monitored_processes[i].name);
               DBG("Number of iteration %i", iterations);

               memset(&val, 0, sizeof(val));
               startTime = ktime_get();

               // sample at rate indicated for the number of iterations
               for (loop = 0; loop < iterations; loop++) {
                  if (kthread_should_stop()) {
                     pr_info("[TaskXT] Thread stop signal received during sampling\n");
                     break;
                  }
                  
                  // Find the process again
                  for_each_process(task) {
                     if (!strcmp(task->comm, monitored_processes[i].name)) {
                        // Extract features
                        if ((task->active_mm)) {
                           val.map_count = (*task->active_mm).map_count;
                           val.hiwater_rss = (*task->active_mm).hiwater_rss; 
                           val.hiwater_vm = (*task->active_mm).hiwater_vm;
                           val.total_vm = (*task->active_mm).total_vm;
                           val.exec_vm = (*task->active_mm).exec_vm;
                        }  
                        val.utime = task->utime;
                        val.stime = task->stime;
                        val.nvcsw = task->nvcsw;
                        val.nivcsw = task->nivcsw;
                        val.min_flt = task->min_flt;
                        if (task->fs)
                           val.fscount = (*task->fs).users;
                        break;
                     }
                  }  

                  memcpy(&monitored_processes[i].data.pid[monitored_processes[i].data.cnt], &val, sizeof(val));
                  monitored_processes[i].data.cnt += sizeof(val);
                  memset(&val, 0, sizeof(val));

                  timeTaken_us = ktime_us_delta(ktime_get(), startTime);
                  if (timeTaken_us < delayAmtMin) {
                     usleep_range(delayAmtMin - timeTaken_us, delayAmtMax - timeTaken_us);
                  } else {
                     DBG("exceeded by %llu on iteration: %i", (timeTaken_us - delayAmtMin), loop);
                  }
                  startTime = ktime_get();
               }

               // Write collected data to file
               len = strlen(path);	
               strcpy(fullFileName, path);
               if (fullFileName[len-1] != '/') strcat(fullFileName, "/");
               strcat(fullFileName, monitored_processes[i].name);
               strcat(fullFileName, ".dat");
               pr_info("[TaskXT] File to write: %s\n", fullFileName);
               log_to_file("[TaskXT] File to write");
               filepath = fullFileName;

               DBG("Initializing write...");
               if((err = setup())) {
                  pr_err("[TaskXT] Setup Error: %d\n", err);
                  log_to_file("[TaskXT] Setup Error");
                  DBG("Setup Error");
                  cleanup();
               } else {
                  pr_info("[TaskXT] Starting data write for %s (cnt=%d samples)\n", 
                          monitored_processes[i].name, monitored_processes[i].data.cnt);
                  err = writeFormatData(&monitored_processes[i].data);
                  if (err) {
                     pr_err("[TaskXT] Write Error: %d\n", err);
                     log_to_file("[TaskXT] Write Error");
                  } else {
                     pr_info("[TaskXT] Data write complete for %s\n", monitored_processes[i].name);
                     log_to_file("[TaskXT] Data write complete");
                  }
                  cleanup();
               }

               // Reset this process's data and mark as handled
               monitored_processes[i].data.cnt = 0;
               monitored_processes[i].data.pid[0] = 0;
               memset(prev_process, 0, sizeof(prev_process));
            }
         }
         
         spin_unlock(&process_lock);
      }
   }
 
   DBG("Leaving extract_features");
   return 0;
}

// here is where features are extracted for the matching process and place in Buffer which will be printed 
static int writeFormatData(task_features *tf)
{
   int err = 0;
   int i, cnt, loop;
   sample val;
   char buffer[100];  
   char writeBuffer[500];

   loop = tf->cnt/sizeof(val);
   pr_info("[TaskXT] writeFormatData: cnt=%d, sampleSize=%lu, loops=%d\n", tf->cnt, sizeof(val), loop);
   DBG("In writeFormatData");
   
   if (loop == 0) {
      pr_warn("[TaskXT] No samples collected (loop=0)\n");
      log_to_file("[TaskXT] WARNING: No samples collected");
      return -EINVAL;
   }
   

   for (i=0; i< loop; i++) {
      cnt = i * sizeof(val);
      memcpy(&val, &tf->pid[cnt], sizeof(val));
   
      // fpu_counter - > uage counter floatin point units (not available since version linux 2.13)

      // Memory related features
      // map_count -> number of memory regions of a process
      sprintf(buffer, "%d", val.map_count);
      strcpy(writeBuffer, buffer); strcat(writeBuffer, ",");

      // page_table_lock -> used to mange the page table entries
      //sprintf(buffer, "%lu", val.page_table_lock);
      //strcat(writeBuffer, buffer); strcat(writeBuffer, ",");

      // hiwater_rss -> Max number of page frames ever owned by the process
      sprintf(buffer, "%lu", val.hiwater_rss);
      strcat(writeBuffer, buffer); strcat(writeBuffer, ",");

      // hiwater_vm -> Max number of pages appeared in memory region of process
      sprintf(buffer, "%lu", val.hiwater_vm);
      strcat(writeBuffer, buffer); strcat(writeBuffer, ",");

      // total_vm -> Size of process's address space in terms of number of pages
      sprintf(buffer, "%lu", val.total_vm);
      strcat(writeBuffer, buffer); strcat(writeBuffer, ",");

      // shared_vm -> Number of pages in shared file memory mappings of process
      //sprintf(buffer, "%lu", val.shared_vm);
      //strcat(writeBuffer, buffer); strcat(writeBuffer, ",");

      // exec_vm -> number of pages in executable memory mappings of process
      sprintf(buffer, "%lu", val.exec_vm);
      strcat(writeBuffer, buffer); strcat(writeBuffer, ",");

      // nr_ptes -> number of pages tables of a process
      //sprintf(buffer, "%lu", val.nr_ptes);
      //strcat(writeBuffer, buffer); strcat(writeBuffer, ",");

      // utime -> Tick count of a process that is executing in user mode
      sprintf(buffer, "%ld", val.utime);
      strcat(writeBuffer, buffer); strcat(writeBuffer, ",");

      // stime -> Tick count of a process in the kernel mode
      sprintf(buffer, "%ld", val.stime);
      strcat(writeBuffer, buffer); strcat(writeBuffer, ",");

      // nvcsw -> number of volunter context switches
      sprintf(buffer, "%lu", val.nvcsw);
      strcat(writeBuffer, buffer); strcat(writeBuffer, ",");

      // nivcsw -> number of in-volunter context switches
      sprintf(buffer, "%lu", val.nivcsw);
      strcat(writeBuffer, buffer); strcat(writeBuffer, ",");

      // min_flt -> Contains the minor page faults
      sprintf(buffer, "%lu", val.min_flt);
      strcat(writeBuffer, buffer); strcat(writeBuffer, ",");

      // alloc_lock.raw_lock.slock -> used to locl memory manager, files and file system etc.
      //sprintf(buffer, "%lu", val.slock);
      //strcat(writeBuffer, buffer); strcat(writeBuffer, ",");

      // fs.count - > number of file usage (was count, now field called users)
      sprintf(buffer, "%d", val.fscount);
      strcat(writeBuffer, buffer);
      strcat(writeBuffer, "\n");
       
      err = write_buffer(writeBuffer);
      if (err) {
         pr_err("[TaskXT] write_buffer failed at sample %d: %d\n", i, err);
         log_to_file("[TaskXT] write_buffer failed");
         return err;
      }
   }
   return err;
}

static int write_buffer(char* buff) {
   ssize_t s;

   int ss = strlen(buff);
   s = write_vaddr(buff, ss);

   if (s != ss) {
      pr_err("[TaskXT] write_buffer failed: expected %d bytes, got %zd\n", ss, s);
      DBG("Error sending task features %zd", s);
      return (int) s;
   }
   return 0;
}

static int taskActive(char *pname)
{
   int flag = 0;
   struct task_struct *task;   

   //printk(KERN_NOTICE "taskxt: current process: %s, PID: %d", task->comm, task->pid);
   for_each_process(task) {
   //pr_info("%s [%d]\n", task->comm, task->pid);
      if (!strcmp(task->comm, pname)) {
         flag = 1;
         printk(KERN_NOTICE "taskxt: Match: %s and %s\n", task->comm, pname);
         break;
      }
   }

   return flag;
}

/*
static int runExtractor(void*none)
{
   int len =0; 
   int err = 0;

   fileOpen = 1;   
   sampling = 1;

    DBG("In runExtractor");
   err = extract_features(pname);
   if (err) return err;


   // Open file to save samples
   len = strlen(path);	
   strcpy(fullFileName, path);
   if (fullFileName[len-1] != '/') strcat(fullFileName, "/");
   strcat(fullFileName, pname);
   strcat(fullFileName, ".dat");
   printk(KERN_INFO "File to Open: %s\n", fullFileName);
   filepath = fullFileName; // set for disk write code
 
   // open file to save samples use path when driver started and set process name as file name .dat
   DBG("Initilizing Dump...");
   if((err = setup())) {
      DBG("Setup Error");
      cleanup();
      return err;
   }

   err = writeFormatData();


   // close file if open
      cleanup();  // close sample file
      fileOpen = 0;
      sampling = 0;


    do_exit(0);
    return 0;
   //return err;
}

static int extract_features(void)
{
task_features tf;
   int err = 0;
   struct task_struct *task = current; // getting global current pointer

 //    struct task_struct *task = current; // getting global current pointer
      printk(KERN_NOTICE "assignment: current process: %s, PID: %d", task->comm, task->pid);
        do
        {
           int ux = task->utime;
           int sx = task->stime; 
           strcpy(tf.pid, task->comm);
//         sprintf(myString, "%d", i);

           if ((err = write_task_feature(&tf))) {
              DBG("Error writing header" );
              break;
           }

           task = task->parent;
           printk(KERN_NOTICE "assignment: parent process: %s, PID: %d", task->comm, task->pid);
        } while (task->pid != 0);

 //    cleanup();

    return err;



}

static int write_task_feature(task_features * tsk) {
        ssize_t s;

        int ss = strlen(tsk->pid);
       s = write_vaddr(tsk->pid, ss);

//        s = write_vaddr(tsk, sizeof(task_features));

//        if (s != sizeof(task_features)) {
        if (s != ss) {
                DBG("Error sending task features %zd", s);
                return (int) s;
        }

        return 0;
}

static int write_task_feature(task_features * tsk) {
        ssize_t s;

        s = write_vaddr(tsk, sizeof(task_features));

        if (s != sizeof(task_features)) {
                DBG("Error sending task features %zd", s);
                return (int) s;
        }

        return 0;
}

int read_process(char * fname, char *pname) 
{

// Create variables
    struct file *f;
    mm_segment_t fs;
    int i;
    // Init the buffer with 0
    for(i=0;i<PNAME_FILE;i++) pname[i] = 0;

    // To see in /var/log/messages that the module is operating
    printk(KERN_INFO "My module is loaded\n");

    // I am using Fedora and for the test I have chosen following file
    // Obviously it is much smaller than the 128 bytes, but hell with it =)
    f = filp_open(fname, O_RDONLY, 0);

    if(f == NULL)
        printk(KERN_ALERT "filp_open error!!.\n");
    else{
        // Get current segment descriptor
        fs = get_fs();
        // Set segment descriptor associated to kernel space
        set_fs(get_ds());
        // Read the file
        f->f_op->read(f, pname, PNAME_FILE, &f->f_pos);
        // Restore segment descriptor
        set_fs(fs);
        // See what we read from file
        printk(KERN_INFO "buf:%s\n",pname);
    }
    filp_close(f,NULL);
    return 0;
}
       // fpu_counter - > uage counter floatin point units (not available since version linux 2.13)


       // Memory related features
       // map_count -> number of memory regions of a process
         sprintf(buffer, "%d", (int) (*task->active_mm).map_count);
         strcat(tf.pid, buffer);
         strcat(tf.pid, ",");

       // page_table_lock -> used to mange the page table entries
         sprintf(buffer, "%lu", (unsigned long) (*task->active_mm).page_table_lock.rlock.raw_lock.tickets.head);
         strcat(tf.pid, buffer);
         strcat(tf.pid, ",");



        // hiwater_rss -> Max number of page frames ever owned by the process
         sprintf(buffer, "%lu", (unsigned long) (*task->active_mm).hiwater_rss);
         strcat(tf.pid, buffer);
         strcat(tf.pid, ",");

        // hiwater_vm -> Max number of pages appeared in memory region of process
         sprintf(buffer, "%lu", (unsigned long) (*task->active_mm).hiwater_vm);
         strcat(tf.pid, buffer);
         strcat(tf.pid, ",");

        // total_vm -> Size of process's address space in terms of number of pages
         sprintf(buffer, "%lu", (unsigned long) (*task->active_mm).total_vm);
         strcat(tf.pid, buffer);
         strcat(tf.pid, ",");

        // shared_vm -> Number of pages in shared file memory mappings of process
         sprintf(buffer, "%lu", (unsigned long) (*task->active_mm).shared_vm);
         strcat(tf.pid, buffer);
         strcat(tf.pid, ",");

        // exec_vm -> number of pages in executable memory mappings of process
         sprintf(buffer, "%lu", (unsigned long) (*task->active_mm).exec_vm);
         strcat(tf.pid, buffer);
         strcat(tf.pid, ",");

        // nr_ptes -> number of pages tables of a process
         sprintf(buffer, "%lu", (unsigned long) (*task->active_mm).nr_ptes.counter);
         strcat(tf.pid, buffer);
         strcat(tf.pid, ",");


         // utime -> Tick count of a process that is executing in user mode
         sprintf(buffer, "%ld", (long int)task->utime);
         strcat(tf.pid, buffer);
         strcat(tf.pid, ",");

         // stime -> Tick count of a process in the kernel mode
         sprintf(buffer, "%ld", (long int)task->stime);
         strcat(tf.pid, buffer);
         strcat(tf.pid, ",");


        // nvcsw -> number of volunter context switches
         sprintf(buffer, "%lu", (unsigned long) task->nvcsw);
         strcat(tf.pid, buffer);
         strcat(tf.pid, ",");

	
	// nivcsw -> number of in-volunter context switches
         sprintf(buffer, "%lu", (unsigned long)task->nivcsw);
         strcat(tf.pid, buffer);
         strcat(tf.pid, ",");


	// min_flt -> Contains the minor page faults
         sprintf(buffer, "%lu", (unsigned long)task->min_flt);
         strcat(tf.pid, buffer);
         strcat(tf.pid, ",");


	// alloc_lock.raw_lock.slock -> used to locl memory manager, files and file system etc.
         sprintf(buffer, "%lu", (unsigned long)task->alloc_lock.rlock.raw_lock.tickets.head);
         strcat(tf.pid, buffer);
         strcat(tf.pid, ",");


        // fs.count - > number of file usage (was count, now field called users)
         sprintf(buffer, "%d", (int)(*task->fs).users);
         strcat(tf.pid, buffer);
         // strcat(tf.pid, ",");
         strcat(tf.pid, "\n");
 
      }



*/


