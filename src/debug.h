#ifndef __AFL_SNAPSHOT_DEBUG_H__
#define __AFL_SNAPSHOT_DEBUG_H__

#include <linux/printk.h>
#include <linux/kern_levels.h>

/* Output macros */

#define HEXDUMP(type, prefix, ptr, size)                 \
  do {                                                   \
                                                         \
    int i;                                               \
    printk(type prefix "  [0] ");                        \
    for (i = 0; i < (size); ++i) {                       \
                                                         \
      printk(KERN_CONT "%02hhX ", ((char *)(ptr))[i]);   \
      if ((i + 1) % 16 == 0 && i < (size)-1)             \
        printk(KERN_CONT "\n" prefix "  [%4d] ", i + 1); \
                                                         \
    }                                                    \
    printk(KERN_CONT "\n");                              \
                                                         \
  } while (0)

#define FATAL(x...)                                                   \
  do {                                                                \
                                                                      \
    pr_crit("[AFL++] FATAL in %s(), %s:%u\n", __FUNCTION__, __FILE__, \
            __LINE__);                                                \
    pr_crit("  Message: " x);                                         \
    pr_crit("\n");                                                    \
                                                                      \
  } while (0)

#define WARNF(x...) pr_warn("[AFL++] WARNING: " x)

#define SAYF(x...) pr_info("[AFL++] SAY: " x)

#ifdef DEBUG

#define DBG_PRINT(x...) pr_debug("[AFL++] DEBUG: " x)
#define DBG_HEXDUMP(ptr, size) HEXDUMP(KERN_DEBUG, "[AFL++] DEBUG: ", ptr, size)

#else

#define DBG_PRINT(x...) \
  do {                  \
                        \
  } while (0)
#define DBG_HEXDUMP(x...) \
  do {                    \
                          \
  } while (0)

#endif

#define PREEMPT_DEBUG(tag) SAYF("[%s():%s:%d] " tag " preempt_count() == %d\n", __FUNCTION__, __FILE__, __LINE__, preempt_count())

#endif

